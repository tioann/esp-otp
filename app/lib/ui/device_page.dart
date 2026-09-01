// Connected-device view: Tokens tab (list + add/remove/rename/reorder) and a
// Device tab (info, sync time, pair, rename, reboot, live screen). Full parity
// with the host CLI's mutating commands.
import 'package:flutter/material.dart';

import '../proto/models.dart';
import '../proto/protocol.dart';
import 'add_secret_page.dart';
import 'app_state.dart';
import 'screen_view.dart';

class DevicePage extends StatelessWidget {
  final AppState state;
  const DevicePage({super.key, required this.state});

  @override
  Widget build(BuildContext context) {
    return DefaultTabController(
      length: 2,
      child: ListenableBuilder(
        listenable: state,
        builder: (context, _) {
          final name = state.info?.name.isNotEmpty == true
              ? state.info!.name
              : (state.active?.name ?? 'esp-otp');
          return Scaffold(
            appBar: AppBar(
              title: Text(name),
              actions: [
                IconButton(
                  tooltip: 'Refresh',
                  onPressed: state.refresh,
                  icon: const Icon(Icons.refresh),
                ),
                IconButton(
                  tooltip: 'Disconnect',
                  onPressed: state.disconnect,
                  icon: const Icon(Icons.bluetooth_disabled),
                ),
              ],
              bottom: const TabBar(tabs: [
                Tab(text: 'Tokens', icon: Icon(Icons.key)),
                Tab(text: 'Device', icon: Icon(Icons.memory)),
              ]),
            ),
            body: Column(
              children: [
                if (state.lastError != null) _errorBar(context, state),
                Expanded(
                  child: TabBarView(children: [
                    _TokensTab(state: state),
                    _DeviceTab(state: state),
                  ]),
                ),
              ],
            ),
          );
        },
      ),
    );
  }

  Widget _errorBar(BuildContext context, AppState state) {
    return MaterialBanner(
      backgroundColor: Colors.red.shade50,
      content: Text(state.lastError!, style: const TextStyle(color: Colors.red)),
      actions: [
        TextButton(
          onPressed: state.clearError,
          child: const Text('Dismiss'),
        ),
      ],
    );
  }
}

class _TokensTab extends StatelessWidget {
  final AppState state;
  const _TokensTab({required this.state});

  @override
  Widget build(BuildContext context) {
    final rows = state.secrets;
    return Scaffold(
      body: rows.isEmpty
          ? const Center(child: Text('No tokens stored'))
          : ListView.separated(
              itemCount: rows.length,
              separatorBuilder: (_, __) => const Divider(height: 1),
              itemBuilder: (context, i) => _tokenTile(context, rows, i),
            ),
      floatingActionButton: FloatingActionButton.extended(
        onPressed: () => _add(context),
        icon: const Icon(Icons.add),
        label: const Text('Add'),
      ),
    );
  }

  Widget _tokenTile(BuildContext context, List<SecretEntry> rows, int i) {
    final r = rows[i];
    return ListTile(
      title: Text(r.label),
      subtitle: Text(
          'id ${r.id} · ${r.digits}d/${r.period}s · ${algoNames[r.algo.clamp(0, 2)]}'),
      trailing: PopupMenuButton<String>(
        onSelected: (v) => _action(context, rows, i, v),
        itemBuilder: (_) => [
          if (i > 0) const PopupMenuItem(value: 'up', child: Text('Move up')),
          if (i < rows.length - 1)
            const PopupMenuItem(value: 'down', child: Text('Move down')),
          const PopupMenuItem(value: 'rename', child: Text('Rename')),
          const PopupMenuItem(value: 'remove', child: Text('Remove')),
        ],
      ),
    );
  }

  Future<void> _action(
      BuildContext context, List<SecretEntry> rows, int i, String v) async {
    final r = rows[i];
    switch (v) {
      case 'up':
        await state.moveSecret(r.id, i - 1);
        break;
      case 'down':
        await state.moveSecret(r.id, i + 1);
        break;
      case 'rename':
        final label = await _prompt(context, 'Rename token', r.label);
        if (label != null && label.trim().isNotEmpty) {
          await state.renameSecret(r.id, label.trim());
        }
        break;
      case 'remove':
        final ok = await _confirm(context, 'Remove "${r.label}"?');
        if (ok) await state.removeSecret(r.id);
        break;
    }
  }

  Future<void> _add(BuildContext context) async {
    final spec = await Navigator.push(
      context,
      MaterialPageRoute(builder: (_) => const AddSecretPage()),
    );
    if (spec != null) await state.addSecret(spec);
  }
}

class _DeviceTab extends StatelessWidget {
  final AppState state;
  const _DeviceTab({required this.state});

  @override
  Widget build(BuildContext context) {
    final info = state.info;
    return ListView(
      padding: const EdgeInsets.all(16),
      children: [
        if (state.otp != null)
          Center(
            child: SizedBox(
              width: 240,
              child: ScreenView(device: state.otp!),
            ),
          ),
        const SizedBox(height: 20),
        if (info != null) _infoCard(context, info),
        const SizedBox(height: 16),
        Wrap(spacing: 12, runSpacing: 12, children: [
          FilledButton.icon(
            onPressed: () async {
              final ok = await state.syncTime();
              _toast(context, ok ? 'Time synced' : 'Sync failed');
            },
            icon: const Icon(Icons.schedule),
            label: const Text('Sync time now'),
          ),
          OutlinedButton.icon(
            onPressed: () async {
              final name =
                  await _prompt(context, 'Device name (1..20)', info?.name ?? '');
              if (name != null && name.trim().isNotEmpty) {
                await state.setName(name.trim());
              }
            },
            icon: const Icon(Icons.badge),
            label: const Text('Set name'),
          ),
          OutlinedButton.icon(
            onPressed: () async {
              if (await _confirm(context, 'Reboot the device?')) {
                await state.reboot();
                _toast(context, 'Reboot scheduled');
              }
            },
            icon: const Icon(Icons.restart_alt),
            label: const Text('Reboot'),
          ),
        ]),
        const Divider(height: 40),
        Text('Inject button', style: Theme.of(context).textTheme.titleSmall),
        const SizedBox(height: 8),
        Wrap(spacing: 12, children: [
          OutlinedButton(
              onPressed: () => state.injectButton(Gesture.single),
              child: const Text('Single')),
          OutlinedButton(
              onPressed: () => state.injectButton(Gesture.double_),
              child: const Text('Double')),
          OutlinedButton(
              onPressed: () => state.injectButton(Gesture.long),
              child: const Text('Long')),
        ]),
      ],
    );
  }

  Widget _infoCard(BuildContext context, DeviceInfo info) {
    Widget row(String k, String v) => Padding(
          padding: const EdgeInsets.symmetric(vertical: 2),
          child: Row(children: [
            SizedBox(width: 120, child: Text(k, style: const TextStyle(color: Colors.grey))),
            Text(v),
          ]),
        );
    return Card(
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
          row('Firmware', info.fw),
          row('RTC present', info.rtcPresent ? 'yes' : 'no'),
          row('Time valid', info.timeValid ? 'yes' : 'no'),
          row('Tokens', '${info.count} / ${info.capacity}'),
        ]),
      ),
    );
  }
}

// ------------------------------------------------------------------- helpers
Future<String?> _prompt(BuildContext context, String title, String initial) {
  final c = TextEditingController(text: initial);
  return showDialog<String>(
    context: context,
    builder: (_) => AlertDialog(
      title: Text(title),
      content: TextField(controller: c, autofocus: true),
      actions: [
        TextButton(
            onPressed: () => Navigator.pop(context), child: const Text('Cancel')),
        FilledButton(
            onPressed: () => Navigator.pop(context, c.text),
            child: const Text('OK')),
      ],
    ),
  );
}

Future<bool> _confirm(BuildContext context, String msg) async {
  final r = await showDialog<bool>(
    context: context,
    builder: (_) => AlertDialog(
      content: Text(msg),
      actions: [
        TextButton(
            onPressed: () => Navigator.pop(context, false),
            child: const Text('Cancel')),
        FilledButton(
            onPressed: () => Navigator.pop(context, true),
            child: const Text('Confirm')),
      ],
    ),
  );
  return r ?? false;
}

void _toast(BuildContext context, String msg) {
  ScaffoldMessenger.of(context)
      .showSnackBar(SnackBar(content: Text(msg), duration: const Duration(seconds: 2)));
}
