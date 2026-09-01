// NimBLE peripheral: brings up the controller + host, advertises, manages the
// single connection's security state, and runs the HID typing task. The GATT
// services themselves live in ble_gatt.c. See blecon.h for the public API.
#include "blecon.h"

#include <string.h>

#include "sdkconfig.h"

#include "ble_internal.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/ble_sm.h"
#include "host/util/util.h"
#include "nvs.h"
#include "nimble/hci_common.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "os/os_mbuf.h"
#include "protocol.h"
#include "services/gap/ble_svc_gap.h"

// Declared in NimBLE's NVS bond-persistence helper (store/config).
extern void ble_store_config_init(void);

static const char *TAG = "blecon";

#define DEVICE_NAME       "esp-otp"
#define APPEARANCE_KEYBOARD 0x03C1

// The advertised GAP device name, changeable at runtime via OP_SET_NAME so
// several people sharing one device can tell theirs apart. Loaded from NVS at
// init (defaulting to DEVICE_NAME) and re-persisted on change. Only the host
// task touches it. PROTOCOL_DEVICE_NAME_MAX bounds the length so it still fits
// the advertising scan response.
static char s_device_name[PROTOCOL_DEVICE_NAME_MAX + 1] = DEVICE_NAME;
#define NAME_KEY "name"

// Per-keystroke timing: hold the key, release, then a gap before the next.
// Generous enough that fast hosts register every key.
#define KEY_HOLD_MS  14
#define KEY_GAP_MS   14

// HID typing job: a code is short, so a fixed buffer avoids dynamic alloc.
#define TYPE_MAX 24
typedef struct {
    char str[TYPE_MAX];
} type_job_t;

static QueueHandle_t s_type_queue;

// Connection table. The device is a single GATT server that several centrals
// may use at once (e.g. a sync app + a keyboard host), up to the controller's
// limit. NimBLE callbacks run on the host task; the typing task only reads
// s_hid_target (an atomic word store on RISC-V), so no extra locking.
#define MAX_LINKS CONFIG_BT_NIMBLE_MAX_CONNECTIONS
// Longest peer name we keep for the UI label; longer names are truncated.
#define LINK_NAME_MAX 20
typedef struct {
    uint16_t handle;  // BLE_HS_CONN_HANDLE_NONE when the slot is free
    bool hid;         // subscribed to the HID input report (i.e. a keyboard host)
    bool encrypted;   // link reached an encrypted state (ENC_CHANGE succeeded)
    // Human-friendly peer name, read from the central's GAP Device Name
    // characteristic once the link encrypts; empty until then (falls back to a
    // hex address label). Only the host task touches it.
    char name[LINK_NAME_MAX];
} link_t;
static link_t s_links[MAX_LINKS];

// Keystrokes go to the most-recently-subscribed HID host: a sync-only app never
// enables HID notifications, so this naturally targets the keyboard host.
static uint16_t s_hid_target = BLE_HS_CONN_HANDLE_NONE;

// Persisted keystroke-target preference. When the user picks a specific host
// with a double-click (blecon_cycle_hid_target) we remember that host's bonded
// *identity address* in NVS, so after a restart the same host is re-pinned as
// the target as soon as it reconnects and subscribes — instead of defaulting to
// whichever host subscribed most recently. Only the host task touches these.
static ble_addr_t s_pref_addr;
static bool s_pref_valid;
#define PREF_NS  "blecon"
#define PREF_KEY "hidtgt"

// The 6-digit pairing code currently on the OLED (0..999999), or PASSKEY_NONE
// while no pairing is in progress. Set on BLE_GAP_EVENT_PASSKEY_ACTION, cleared
// once the link encrypts or drops. Under LE Secure Connections this is a
// *numeric comparison* value the user checks against the phone and confirms
// with the button (see s_numcmp below); the legacy fallback is a display-only
// passkey the host types. A plain word store; the UI task only reads it.
#define PASSKEY_NONE UINT32_MAX
static uint32_t s_passkey = PASSKEY_NONE;
static uint16_t s_passkey_conn = BLE_HS_CONN_HANDLE_NONE;
// True while s_passkey is a numeric-comparison value awaiting the user's button
// confirmation (vs. a display-only passkey the host types). While true the UI
// shows a "match?" prompt and a press calls blecon_confirm_pairing().
static bool s_numcmp = false;

// Button-gated pairing window. New bonds are accepted only while this is open;
// the user opens it with a long-press (blecon_open_pairing). It auto-closes
// after PAIRING_WINDOW_MS or as soon as a host finishes pairing. Outside the
// window, unsolicited pairing attempts are refused and re-pairing is ignored so
// a rogue can't overwrite the configured bond.
#define PAIRING_WINDOW_MS 60000
static bool s_pairing_open;
static esp_timer_handle_t s_pairing_timer;

static void start_advertising(void);
static void restart_advertising(void);

static void pairing_close(void)
{
    if (s_pairing_open) {
        ESP_LOGI(TAG, "pairing window closed");
    }
    bool was_open = s_pairing_open;
    s_pairing_open = false;
    if (s_pairing_timer) {
        esp_timer_stop(s_pairing_timer);
    }
    if (was_open) {
        restart_advertising();  // go non-discoverable now the window is shut
    }
}

static void pairing_timeout_cb(void *arg)
{
    (void)arg;
    bool was_open = s_pairing_open;
    if (s_pairing_open) {
        ESP_LOGI(TAG, "pairing window timed out");
    }
    s_pairing_open = false;
    if (was_open) {
        restart_advertising();  // go non-discoverable now the window is shut
    }
}

static uint8_t s_addr_type;

static link_t *link_find(uint16_t handle)
{
    for (int i = 0; i < MAX_LINKS; i++) {
        if (s_links[i].handle == handle) {
            return &s_links[i];
        }
    }
    return NULL;
}

static int link_count(void)
{
    int n = 0;
    for (int i = 0; i < MAX_LINKS; i++) {
        if (s_links[i].handle != BLE_HS_CONN_HANDLE_NONE) {
            n++;
        }
    }
    return n;
}

// Pick any still-subscribed HID host as the fallback target (or NONE).
static uint16_t any_hid_host(void)
{
    for (int i = 0; i < MAX_LINKS; i++) {
        if (s_links[i].handle != BLE_HS_CONN_HANDLE_NONE && s_links[i].hid) {
            return s_links[i].handle;
        }
    }
    return BLE_HS_CONN_HANDLE_NONE;
}

static void pref_load(void)
{
    nvs_handle_t h;
    if (nvs_open(PREF_NS, NVS_READONLY, &h) != ESP_OK) {
        return;  // namespace absent on first boot
    }
    size_t len = sizeof(s_pref_addr);
    if (nvs_get_blob(h, PREF_KEY, &s_pref_addr, &len) == ESP_OK &&
        len == sizeof(s_pref_addr)) {
        s_pref_valid = true;
    }
    nvs_close(h);
}

static void pref_save(const ble_addr_t *addr)
{
    s_pref_addr = *addr;
    s_pref_valid = true;
    nvs_handle_t h;
    if (nvs_open(PREF_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (nvs_set_blob(h, PREF_KEY, addr, sizeof(*addr)) == ESP_OK) {
        nvs_commit(h);
    }
    nvs_close(h);
}

// Load the persisted device name into s_device_name; keep the DEVICE_NAME
// default if none is stored (first boot) or the stored value is malformed.
static void name_load(void)
{
    nvs_handle_t h;
    if (nvs_open(PREF_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    char buf[sizeof(s_device_name)];
    size_t len = sizeof(buf);
    if (nvs_get_str(h, NAME_KEY, buf, &len) == ESP_OK && buf[0] != '\0') {
        memcpy(s_device_name, buf, sizeof(s_device_name));
        s_device_name[sizeof(s_device_name) - 1] = '\0';
    }
    nvs_close(h);
}

// Apply + persist a new advertised name. Registered as the OP_SET_NAME hook, so
// this runs on the NimBLE host task (BLE reads s_device_name there too). Returns
// false on an invalid length; the dispatch layer already bounds it, this is
// defence in depth. Re-emits advertising so scanners see the new name at once.
static bool blecon_set_name(const char *name)
{
    size_t n = name ? strlen(name) : 0;
    if (n == 0 || n > PROTOCOL_DEVICE_NAME_MAX) {
        return false;
    }
    memcpy(s_device_name, name, n);
    s_device_name[n] = '\0';

    nvs_handle_t h;
    if (nvs_open(PREF_NS, NVS_READWRITE, &h) == ESP_OK) {
        if (nvs_set_str(h, NAME_KEY, s_device_name) == ESP_OK) {
            nvs_commit(h);
        }
        nvs_close(h);
    }

    int rc = ble_svc_gap_device_name_set(s_device_name);
    if (rc != 0) {
        ESP_LOGW(TAG, "device_name_set failed (%d)", rc);
    }
    restart_advertising();
    ESP_LOGI(TAG, "device name set to \"%s\"", s_device_name);
    return true;
}

// GET_INFO reads the current name through this (registered getter).
static const char *blecon_get_name(void) { return s_device_name; }

// Handle of a currently-subscribed HID host whose bonded identity address
// matches the persisted preference, or NONE if that host isn't connected.
static uint16_t pref_host(void)
{
    if (!s_pref_valid) {
        return BLE_HS_CONN_HANDLE_NONE;
    }
    for (int i = 0; i < MAX_LINKS; i++) {
        if (s_links[i].handle == BLE_HS_CONN_HANDLE_NONE || !s_links[i].hid) {
            continue;
        }
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(s_links[i].handle, &desc) == 0 &&
            ble_addr_cmp(&desc.peer_id_addr, &s_pref_addr) == 0) {
            return s_links[i].handle;
        }
    }
    return BLE_HS_CONN_HANDLE_NONE;
}

// The active target after any host-set change: the persisted-preferred host if
// it's subscribed, otherwise `fallback` (e.g. the most-recent or any host).
static uint16_t resolve_target(uint16_t fallback)
{
    uint16_t p = pref_host();
    return (p != BLE_HS_CONN_HANDLE_NONE) ? p : fallback;
}

// GATT-client read of the peer's GAP Device Name (0x2A00): a central runs its
// own GATT server, so once the link is encrypted we can fetch a friendly name
// (e.g. "Tasos's iPhone") for the UI instead of a bare address. Best-effort —
// some centrals don't expose it; the label falls back to hex until/unless this
// lands.
static int name_read_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                        struct ble_gatt_attr *attr, void *arg)
{
    (void)arg;
    if (error->status != 0 || attr == NULL || attr->om == NULL) {
        return 0;  // peer refused or has no name; keep the hex fallback
    }
    link_t *slot = link_find(conn_handle);
    if (slot == NULL) {
        return 0;
    }
    uint16_t len = OS_MBUF_PKTLEN(attr->om);
    if (len >= sizeof(slot->name)) {
        len = sizeof(slot->name) - 1;
    }
    if (ble_hs_mbuf_to_flat(attr->om, slot->name, len, NULL) == 0) {
        slot->name[len] = '\0';
        ESP_LOGI(TAG, "peer name (handle %u): \"%s\"", conn_handle, slot->name);
    }
    return 0;
}

static void start_name_read(uint16_t conn_handle)
{
    const ble_uuid16_t dev_name = BLE_UUID16_INIT(0x2A00);
    int rc = ble_gattc_read_by_uuid(conn_handle, 0x0001, 0xffff, &dev_name.u,
                                    name_read_cb, NULL);
    if (rc != 0) {
        ESP_LOGD(TAG, "name read start failed (handle %u, rc %d)", conn_handle, rc);
    }
}

bool ble_conn_authorized(uint16_t conn_handle)
{
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(conn_handle, &desc) != 0) {
        return false;
    }
    // Require an *authenticated* (passkey-verified) link, not just an encrypted
    // one: with sm_mitm set there is no Just Works path, so this rejects any
    // peer that bonded without entering the OLED passkey.
    return desc.sec_state.encrypted && desc.sec_state.authenticated;
}

// --- HID keyboard typing ----------------------------------------------------

// Maps a printable character to a HID usage code and whether Shift is needed.
// Returns false for characters we can't type. TOTP codes are digits, but
// labels/letters are supported too for completeness.
static bool ascii_to_hid(char c, uint8_t *code, bool *shift)
{
    *shift = false;
    if (c >= 'a' && c <= 'z') { *code = 0x04 + (c - 'a'); return true; }
    if (c >= 'A' && c <= 'Z') { *shift = true; *code = 0x04 + (c - 'A'); return true; }
    if (c >= '1' && c <= '9') { *code = 0x1E + (c - '1'); return true; }
    switch (c) {
        case '0': *code = 0x27; return true;
        case ' ': *code = 0x2C; return true;
        case '\n': *code = 0x28; return true;  // Enter
        case '\t': *code = 0x2B; return true;
        case '-': *code = 0x2D; return true;
        case '.': *code = 0x37; return true;
        default:  return false;
    }
}

// Send one 8-byte HID input report to a specific host connection.
static int hid_notify(uint16_t conn, const uint8_t report[BLE_HID_REPORT_LEN])
{
    struct os_mbuf *om = ble_hs_mbuf_from_flat(report, BLE_HID_REPORT_LEN);
    if (!om) {
        return BLE_HS_ENOMEM;
    }
    return ble_gatts_notify_custom(conn, ble_gatt_hid_input_handle(), om);
}

static void type_task(void *arg)
{
    (void)arg;
    type_job_t job;
    for (;;) {
        if (xQueueReceive(s_type_queue, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        // Latch the target once per job so a mid-type subscribe change can't
        // split the code across two hosts.
        uint16_t target = s_hid_target;
        if (target == BLE_HS_CONN_HANDLE_NONE) {
            ESP_LOGW(TAG, "type request dropped: no subscribed HID host");
            continue;
        }
        for (const char *p = job.str; *p; p++) {
            uint8_t code;
            bool shift;
            if (!ascii_to_hid(*p, &code, &shift)) {
                continue;
            }
            uint8_t report[BLE_HID_REPORT_LEN] = {0};
            report[0] = shift ? 0x02 : 0x00;  // left-shift modifier
            report[2] = code;
            if (hid_notify(target, report) != 0) {
                break;  // host vanished mid-type
            }
            vTaskDelay(pdMS_TO_TICKS(KEY_HOLD_MS));

            uint8_t release[BLE_HID_REPORT_LEN] = {0};
            hid_notify(target, release);
            vTaskDelay(pdMS_TO_TICKS(KEY_GAP_MS));
        }
        ESP_LOGI(TAG, "typed code over HID");
    }
}

// --- GAP --------------------------------------------------------------------

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            uint16_t h = event->connect.conn_handle;
            link_t *slot = link_find(BLE_HS_CONN_HANDLE_NONE);
            if (slot) {
                slot->handle = h;
                slot->hid = false;
                slot->name[0] = '\0';  // filled in once the link encrypts
            }
            ESP_LOGI(TAG, "connected (handle %u, %d/%d links)", h,
                     link_count(), MAX_LINKS);
            // Don't initiate security here: the central drives it. A bonded host
            // re-encrypts with its stored keys; a new host starts pairing itself
            // (which surfaces our passkey, and is refused unless the window is
            // open). An unbonded, sync-only host (the clock daemon) is left on an
            // open link so SET_TIME still works without pairing.
        } else {
            ESP_LOGI(TAG, "connect failed (%d)", event->connect.status);
        }
        // Keep advertising while a connection slot remains free so a second
        // host (e.g. keyboard target) can still pair.
        start_advertising();
        return 0;

    case BLE_GAP_EVENT_DISCONNECT: {
        uint16_t h = event->disconnect.conn.conn_handle;
        ESP_LOGI(TAG, "disconnected (handle %u, reason %d)", h,
                 event->disconnect.reason);
        link_t *slot = link_find(h);
        if (slot) {
            slot->handle = BLE_HS_CONN_HANDLE_NONE;
            slot->hid = false;
        }
        // If the gone host was the keystroke target, fall back to the preferred
        // host if it's still around, else another subscribed HID host (or none).
        if (s_hid_target == h) {
            s_hid_target = resolve_target(any_hid_host());
        }
        // Drop any passkey still on screen for this (now-gone) pairing.
        if (s_passkey_conn == h) {
            s_passkey = PASSKEY_NONE;
            s_passkey_conn = BLE_HS_CONN_HANDLE_NONE;
            s_numcmp = false;
        }
        start_advertising();
        return 0;
    }

    case BLE_GAP_EVENT_ENC_CHANGE: {
        bool was_pairing = (event->enc_change.conn_handle == s_passkey_conn);
        if (event->enc_change.status == 0) {
            ESP_LOGI(TAG, "encryption enabled");
            // Now that the link is encrypted, fetch the peer's friendly name for
            // the UI (new pairing or bonded reconnect both land here).
            start_name_read(event->enc_change.conn_handle);
        } else {
            ESP_LOGW(TAG, "encryption failed: status=%d (0x%04x)",
                     event->enc_change.status, event->enc_change.status);
            // The peer couldn't encrypt with the keys we hold for it — our bond
            // is stale (it forgot us / lost its keys). Drop it and the link so it
            // doesn't reconnect and fail again on a loop.
            struct ble_gap_conn_desc desc;
            if (ble_gap_conn_find(event->enc_change.conn_handle, &desc) == 0) {
                ble_store_util_delete_peer(&desc.peer_id_addr);
            }
            ble_gap_terminate(event->enc_change.conn_handle,
                              BLE_ERR_REM_USER_CONN_TERM);
            restart_advertising();  // rebuild accept-list without the dropped peer
        }
        // If this is the pairing we were displaying a passkey for, drop the
        // passkey screen and close the window once it succeeds (job done).
        if (was_pairing) {
            s_passkey = PASSKEY_NONE;
            s_passkey_conn = BLE_HS_CONN_HANDLE_NONE;
            s_numcmp = false;
            if (event->enc_change.status == 0) {
                pairing_close();
            }
        }
        return 0;
    }

    case BLE_GAP_EVENT_PASSKEY_ACTION:
        // A passkey action is raised only for a *new* pairing (bonded reconnects
        // skip straight to ENC_CHANGE). Refuse any of them unless the user has
        // opened the pairing window, so a configured device can't be hijacked.
        if (!s_pairing_open) {
            ESP_LOGW(TAG, "pairing refused: window closed");
            ble_gap_terminate(event->passkey.conn_handle,
                              BLE_ERR_REM_USER_CONN_TERM);
            return 0;
        }
        if (event->passkey.params.action == BLE_SM_IOACT_NUMCMP) {
            // LE Secure Connections numeric comparison: the stack hands us a
            // 6-digit value that the phone shows too. Display it and wait for the
            // user to confirm the two match with the button — the reply is
            // injected later from blecon_confirm_pairing(). This is what a phone
            // uses when it treats us as a HID keyboard with a display.
            s_passkey = event->passkey.params.numcmp;
            s_passkey_conn = event->passkey.conn_handle;
            s_numcmp = true;
            ESP_LOGI(TAG, "numeric comparison: %06u (awaiting confirm)",
                     (unsigned)s_passkey);
        } else if (event->passkey.params.action == BLE_SM_IOACT_DISP) {
            // Legacy fallback: pick a random 6-digit passkey, show it on the
            // OLED (the UI polls blecon_pairing_passkey), and the host types it.
            struct ble_sm_io io = {
                .action = BLE_SM_IOACT_DISP,
                .passkey = esp_random() % 1000000,
            };
            s_passkey = io.passkey;
            s_passkey_conn = event->passkey.conn_handle;
            s_numcmp = false;
            ESP_LOGI(TAG, "pairing passkey: %06u", (unsigned)io.passkey);
            int rc = ble_sm_inject_io(event->passkey.conn_handle, &io);
            if (rc != 0) {
                ESP_LOGW(TAG, "inject_io failed (%d)", rc);
                s_passkey = PASSKEY_NONE;
                s_passkey_conn = BLE_HS_CONN_HANDLE_NONE;
            }
        } else {
            // INPUT / OOB: we have no keypad, so we can't satisfy these. Log and
            // let the pairing fail rather than stall silently.
            ESP_LOGW(TAG, "unsupported passkey action %u",
                     (unsigned)event->passkey.params.action);
        }
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == ble_gatt_hid_input_handle()) {
            uint16_t h = event->subscribe.conn_handle;
            link_t *slot = link_find(h);
            if (slot) {
                slot->hid = event->subscribe.cur_notify;
            }
            if (event->subscribe.cur_notify) {
                // Preferred host (persisted selection) wins if it's the one
                // reconnecting; otherwise most-recent HID host wins.
                s_hid_target = resolve_target(h);
            } else if (s_hid_target == h) {
                s_hid_target = resolve_target(any_hid_host());
            }
            ESP_LOGI(TAG, "HID input %ssubscribed (handle %u)",
                     event->subscribe.cur_notify ? "" : "un", h);
        }
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        // A peer re-pairs while a bond already exists. Only honour it while the
        // pairing window is open (drop the stale bond and let it proceed);
        // otherwise keep the configured bond and ignore the request so a rogue
        // can't overwrite it.
        if (!s_pairing_open) {
            // A configured host is re-pairing with the window shut — it has lost
            // its keys (e.g. the user forgot us on that host). Keeping our stale
            // bond makes it retry forever, popping a pairing prompt each time.
            // Drop our side of the bond and the link so it stops flapping; it can
            // pair cleanly once the user reopens the window.
            struct ble_gap_conn_desc desc;
            if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
                ble_store_util_delete_peer(&desc.peer_id_addr);
            }
            ESP_LOGW(TAG, "re-pair refused: window closed; dropped stale bond");
            ble_gap_terminate(event->repeat_pairing.conn_handle,
                              BLE_ERR_REM_USER_CONN_TERM);
            restart_advertising();  // rebuild accept-list without the dropped peer
            return BLE_GAP_REPEAT_PAIRING_IGNORE;
        }
        {
            struct ble_gap_conn_desc desc;
            if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
                ble_store_util_delete_peer(&desc.peer_id_addr);
            }
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        start_advertising();
        return 0;

    default:
        return 0;
    }
}

// Load the controller accept-list with our bonded peers' identity addresses.
// While the pairing window is shut we advertise with a connection filter (see
// start_advertising) so only these peers may open a link; a host that forgot us
// (its bond deleted) drops off the list and can no longer connect at all, which
// is what actually stops the connect/disconnect churn — a refuse-and-terminate
// at the host layer just makes it reconnect faster.
static void sync_whitelist(void)
{
    ble_addr_t peers[CONFIG_BT_NIMBLE_WHITELIST_SIZE];
    int count = 0;
    int rc = ble_store_util_bonded_peers(peers, &count,
                                         CONFIG_BT_NIMBLE_WHITELIST_SIZE);
    if (rc != 0) {
        ESP_LOGW(TAG, "bonded_peers failed (%d)", rc);
        return;
    }
    rc = ble_gap_wl_set(peers, count);
    if (rc != 0) {
        ESP_LOGW(TAG, "wl_set failed (%d)", rc);
    } else {
        ESP_LOGI(TAG, "accept-list: %d bonded peer(s)", count);
    }
}

static void start_advertising(void)
{
    // No point advertising with every connection slot in use.
    if (link_count() >= MAX_LINKS) {
        ESP_LOGI(TAG, "all %d links in use; not advertising", MAX_LINKS);
        return;
    }
    if (ble_gap_adv_active()) {
        return;  // already advertising
    }

    // Only invite fresh pairings while the button-opened window is up. Closed,
    // we drop the "general discoverable" flag AND advertise with a connection
    // filter keyed to the accept-list (bonded peers only), so a host that forgot
    // us can't open a link at all — no connect/disconnect churn. Bonded hosts and
    // the service-scanning clock daemon still reconnect: the filter allows scans
    // (service/name discovery works) and only gates connection requests.
    bool disc = s_pairing_open;
    if (!disc) {
        sync_whitelist();
    }

    // Advertising data: flags + appearance + the HID (16-bit) and management
    // (128-bit) service UUIDs. Advertising the custom management UUID lets a
    // scanner identify an esp-otp by type even after its GAP name changes. Budget
    // (31 bytes): flags 3 + appearance 4 + uuid16 4 + uuid128 18 = 29, fits. The
    // name is too long to also fit here, so it goes in the scan response.
    struct ble_hs_adv_fields fields = {0};
    fields.flags = (disc ? BLE_HS_ADV_F_DISC_GEN : 0) | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.appearance = APPEARANCE_KEYBOARD;
    fields.appearance_is_present = 1;
    ble_uuid16_t hid_uuid = BLE_UUID16_INIT(0x1812);
    fields.uuids16 = &hid_uuid;
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;
    fields.uuids128 = (ble_uuid128_t *)&mgmt_svc_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_set_fields failed (%d)", rc);
        return;
    }

    struct ble_hs_adv_fields scan_rsp = {0};
    scan_rsp.name = (uint8_t *)s_device_name;
    scan_rsp.name_len = strlen(s_device_name);
    scan_rsp.name_is_complete = 1;
    ble_gap_adv_rsp_set_fields(&scan_rsp);

    struct ble_gap_adv_params adv = {0};
    adv.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv.disc_mode = disc ? BLE_GAP_DISC_MODE_GEN : BLE_GAP_DISC_MODE_NON;
    // Window open: accept any connection (a new host must be able to reach us to
    // pair). Window closed: BLE_HCI_ADV_FILT_CONN filters connection requests to
    // the accept-list while still answering scans from anyone (so the clock
    // daemon's service discovery keeps working).
    adv.filter_policy = disc ? BLE_HCI_ADV_FILT_NONE : BLE_HCI_ADV_FILT_CONN;
    rc = ble_gap_adv_start(s_addr_type, NULL, BLE_HS_FOREVER, &adv, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_start failed (%d)", rc);
    } else {
        ESP_LOGI(TAG, "advertising as \"%s\"", s_device_name);
    }
}

// Re-emit advertising with fresh parameters. Called when the pairing window
// opens or closes so the discoverable flag flips immediately instead of at the
// next (never, since we advertise BLE_HS_FOREVER) adv cycle.
static void restart_advertising(void)
{
    if (ble_gap_adv_active()) {
        ble_gap_adv_stop();
    }
    start_advertising();
}

// --- Host bring-up ----------------------------------------------------------

// Advertise with a FIXED static-random identity address. It is derived
// deterministically from the chip's factory BT MAC (so it is unique per device)
// with the two most-significant bits forced to 0b11 — the bit pattern that marks
// an address as "static random" per the BLE spec. Because it comes straight from
// efuse it is identical on every boot with no NVS involved, so it NEVER changes:
// not across reboots, not when the pairing window (re)starts advertising, and it
// never rotates. A stable identity means one MAC per device in scan lists and a
// clean bond/reconnect for background auto-sync.
//
// We deliberately do NOT use privacy / resolvable-private addresses (RPA): those
// rotate (BLE default ~15 min, and again on every advertising restart), so the
// same physical device shows up under several MACs at once in the app's scan
// list and the address flips on each pairing attempt — confusing and pointless
// for a device we WANT to be re-findable.
static int static_addr_ensure(void)
{
    uint8_t mac[6];
    esp_err_t err = esp_read_mac(mac, ESP_MAC_BT);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "read_mac failed (%s)", esp_err_to_name(err));
        return -1;
    }
    // ble_addr_t.val is little-endian (val[5] = MSB); esp_read_mac is big-endian
    // (mac[0] = MSB), so reverse the byte order.
    uint8_t rnd[6];
    for (int i = 0; i < 6; i++) {
        rnd[i] = mac[5 - i];
    }
    rnd[5] |= 0xC0;  // force the two MSBs to 0b11 => valid static-random address
    ESP_LOGI(TAG, "static-random identity %02X:%02X:%02X:%02X:%02X:%02X",
             rnd[5], rnd[4], rnd[3], rnd[2], rnd[1], rnd[0]);
    return ble_hs_id_set_rnd(rnd);
}

static void on_sync(void)
{
    if (static_addr_ensure() != 0) {
        ESP_LOGE(TAG, "no static identity address");
        return;
    }
    // Use the static-random address we just set (no address-type inference,
    // which with privacy=1 would pick a rotating RPA instead).
    s_addr_type = BLE_OWN_ADDR_RANDOM;
    start_advertising();
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "host reset (reason %d)", reason);
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();  // returns only on nimble_port_stop()
    nimble_port_freertos_deinit();
}

esp_err_t blecon_init(void)
{
    for (int i = 0; i < MAX_LINKS; i++) {
        s_links[i].handle = BLE_HS_CONN_HANDLE_NONE;
        s_links[i].hid = false;
    }
    pref_load();  // restore the saved keystroke-target host, if any
    name_load();  // restore the saved advertised device name, if any
    protocol_register_set_name(blecon_set_name);  // wire up OP_SET_NAME
    protocol_register_get_name(blecon_get_name);  // name reported by GET_INFO

    s_type_queue = xQueueCreate(2, sizeof(type_job_t));
    if (!s_type_queue) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed (%s)", esp_err_to_name(err));
        return err;
    }

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    // Numeric-comparison bonding: with a display *and* a confirm button we
    // advertise DISPLAY_YESNO, so under LE Secure Connections the phone and the
    // OLED show the same 6-digit code and the user confirms the match with the
    // button (handled in gap_event's PASSKEY_ACTION / blecon_confirm_pairing).
    // A plain DISP_ONLY capability collides with the phone's HID-keyboard
    // pairing flow (both sides try to display, so the codes never match).
    // sm_mitm still demands an authenticated association — no Just Works — and
    // bonds persist in NVS so a paired host reconnects without re-pairing.
    // NOTE: sm_sc MUST stay 1. Legacy pairing (sm_sc=0) was tried and is WORSE —
    // the Android pairing dialog appears then instantly vanishes (pairing aborts
    // before the user can enter the passkey). See memory ble-legacy-pairing-dead-end.
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_DISP_YES_NO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    // Key distribution. We do NOT distribute our identity key (IRK): the device
    // uses a FIXED address (see on_sync), so a peer never needs an IRK to resolve
    // us. Sending ID_INFO also triggered a fatal race on Android — the peripheral
    // transmitted ID_INFO while the phone was still SMP_STATE_ENCRYPTION_PENDING,
    // the phone logged `Ignore ID_INFO in ENCRYPTION_PENDING` and dropped it, and
    // the handshake then stalled to the ~30s SMP timeout (pairing only survived
    // if the user happened to confirm on the phone before the ESP button). With
    // ID dropped there is no ID_INFO, so pairing completes regardless of confirm
    // order. We still REQUEST the peer's ID key so we can resolve a phone that
    // advertises a resolvable-private address on reconnect.
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    int rc = ble_svc_gap_device_name_set(s_device_name);
    if (rc != 0) {
        ESP_LOGW(TAG, "device_name_set failed (%d)", rc);
    }
    rc = ble_gatt_init();
    if (rc != 0) {
        ESP_LOGE(TAG, "GATT init failed (%d)", rc);
        return ESP_FAIL;
    }

    ble_store_config_init();  // wire up NVS bond persistence

    const esp_timer_create_args_t pt_args = {
        .callback = pairing_timeout_cb,
        .name = "pair_window",
    };
    if (esp_timer_create(&pt_args, &s_pairing_timer) != ESP_OK) {
        ESP_LOGW(TAG, "pairing timer create failed; window won't auto-close");
    }

    if (xTaskCreate(type_task, "ble_type", 3072, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    nimble_port_freertos_init(host_task);

    ESP_LOGI(TAG, "BLE up");
    return ESP_OK;
}

esp_err_t blecon_type_code(const char *str)
{
    if (!str) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_hid_target == BLE_HS_CONN_HANDLE_NONE) {
        return ESP_ERR_INVALID_STATE;
    }
    type_job_t job;
    strlcpy(job.str, str, sizeof(job.str));
    return xQueueSend(s_type_queue, &job, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}

// Collect the handles of currently-subscribed HID hosts in slot order. Returns
// the count; fills up to `cap` handles.
static size_t hid_hosts(uint16_t *out, size_t cap)
{
    size_t n = 0;
    for (int i = 0; i < MAX_LINKS; i++) {
        if (s_links[i].handle != BLE_HS_CONN_HANDLE_NONE && s_links[i].hid) {
            if (n < cap) {
                out[n] = s_links[i].handle;
            }
            n++;
        }
    }
    return n;
}

size_t blecon_hid_host_count(void)
{
    uint16_t tmp[MAX_LINKS];
    return hid_hosts(tmp, MAX_LINKS);
}

void blecon_cycle_hid_target(void)
{
    uint16_t hosts[MAX_LINKS];
    size_t n = hid_hosts(hosts, MAX_LINKS);
    if (n < 2) {
        return;  // nothing to cycle between
    }
    size_t cur = 0;
    for (size_t i = 0; i < n; i++) {
        if (hosts[i] == s_hid_target) {
            cur = i;
            break;
        }
    }
    s_hid_target = hosts[(cur + 1) % n];
    // Remember this explicit choice so it survives a restart.
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(s_hid_target, &desc) == 0) {
        pref_save(&desc.peer_id_addr);
    }
    ESP_LOGI(TAG, "HID target -> handle %u", s_hid_target);
}

size_t blecon_hid_target_label(char *buf, size_t cap)
{
    if (!cap) {
        return blecon_hid_host_count();
    }
    if (s_hid_target == BLE_HS_CONN_HANDLE_NONE) {
        strlcpy(buf, "none", cap);
        return 0;
    }
    // Prefer the peer's friendly name (read from its GAP Device Name once the
    // link encrypted). Fall back to the two low bytes of the address — stable
    // per host and enough to tell a laptop from a phone — when no name is known.
    link_t *slot = link_find(s_hid_target);
    if (slot != NULL && slot->name[0] != '\0') {
        strlcpy(buf, slot->name, cap);
    } else {
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(s_hid_target, &desc) == 0) {
            snprintf(buf, cap, "%02X:%02X", desc.peer_id_addr.val[1],
                     desc.peer_id_addr.val[0]);
        } else {
            strlcpy(buf, "?", cap);
        }
    }
    return blecon_hid_host_count();
}

bool blecon_pairing_passkey(uint32_t *passkey)
{
    uint32_t pk = s_passkey;
    if (pk == PASSKEY_NONE) {
        return false;
    }
    if (passkey) {
        *passkey = pk;
    }
    return true;
}

bool blecon_pairing_confirm_pending(uint32_t *code)
{
    if (!s_numcmp || s_passkey == PASSKEY_NONE) {
        return false;
    }
    if (code) {
        *code = s_passkey;
    }
    return true;
}

void blecon_confirm_pairing(bool accept)
{
    // Only meaningful while a numeric-comparison confirmation is pending; a
    // stray press otherwise is a no-op.
    if (!s_numcmp || s_passkey_conn == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }
    struct ble_sm_io io = {
        .action = BLE_SM_IOACT_NUMCMP,
        .numcmp_accept = accept,
    };
    ESP_LOGI(TAG, "numeric comparison %s", accept ? "accepted" : "rejected");
    int rc = ble_sm_inject_io(s_passkey_conn, &io);
    if (rc != 0) {
        ESP_LOGW(TAG, "inject_io (numcmp) failed (%d)", rc);
    }
    // The confirmation has been delivered; stop prompting. The code stays on
    // screen until ENC_CHANGE (success) or disconnect (failure) clears it.
    s_numcmp = false;
}

void blecon_open_pairing(void)
{
    bool was_open = s_pairing_open;
    s_pairing_open = true;
    ESP_LOGI(TAG, "pairing window open (%ds)", PAIRING_WINDOW_MS / 1000);
    if (s_pairing_timer) {
        esp_timer_stop(s_pairing_timer);  // restart the countdown if re-opened
        esp_timer_start_once(s_pairing_timer, (uint64_t)PAIRING_WINDOW_MS * 1000);
    }
    if (!was_open) {
        restart_advertising();  // become discoverable so a new host can find us
    }
}

bool blecon_pairing_open(void)
{
    return s_pairing_open;
}
