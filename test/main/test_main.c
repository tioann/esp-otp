// Test runner entry point. Runs every registered Unity TEST_CASE and prints a
// summary, then signals completion so QEMU runs can be scored by grepping the
// output. The actual cases live alongside the components (e.g.
// components/totp/test/test_totp.c).
#include "unity.h"

void app_main(void)
{
    UNITY_BEGIN();
    unity_run_all_tests();
    UNITY_END();

    // Sentinel for the QEMU harness (scripts/test-qemu.sh greps for this).
    printf("\nESP_OTP_TESTS_DONE\n");
}
