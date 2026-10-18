/* The real in-process FujiNet over NetSIO: it connects to the hub, the Atari
   boots FujiNet's CONFIG from it, a power cycle restarts FujiNet and boots
   CONFIG again. SKIPs (77) when the runtime was not built. */
#include "test_util.h"

int main(void)
{
    a800session *s = test_session_ex("netboot", 1);
    CHECK(s, "new");
    if (!test_start_fujinet(s)) {
        fprintf(stderr, "SKIP: no FujiNet runtime (%s)\n", a800session_last_error(s));
        a800session_free(s);
        return 77;
    }
    /* CONFIG running: it asks FujiNet for the host and drive slots */
    CHECK(test_wait_fujinet_log(s, "READ DEVICE SLOTS", 20000), "CONFIG booted");
    test_sleep_ms(1500);
    if (getenv("A800_TEST_SHOT"))
        test_save_ppm(s, getenv("A800_TEST_SHOT"));

    /* power cycle: FujiNet restarts (a fresh log) and boots CONFIG again */
    a800session_power_cycle(s);
    CHECK(a800session_fujinet_running(s) && a800session_fujinet_link(s), "FujiNet back");
    CHECK(test_wait_fujinet_log(s, "READ DEVICE SLOTS", 20000), "CONFIG after power cycle");

    a800session_stop(s);
    a800session_free(s);
    puts("netboot: ok");
    return 0;
}
