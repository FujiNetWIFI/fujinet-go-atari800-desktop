/* Media: with FujiNet off, an ATR boots from atari800's D1, an XEX runs, a
   5200 cartridge is refused, a raw ROM asks for its type, states save and
   load, tapes record. With FujiNet on, Open copies the ATR to the SD folder,
   mounts it in FujiNet's D1 and boots it; Reboot to CONFIG brings CONFIG
   back. */
#include "test_util.h"
#include "test_files.h"

static void path_in(a800session *s, char *dst, size_t n, const char *leaf)
{
    snprintf(dst, n, "%s/%s", a800session_data_path(s), leaf);
}

int main(void)
{
    a800session *s = test_session("media");
    char atr[1100], xex[1100], car[1100], raw[1100], st[1100], cas[1100], msg[512];
    a800_drive_info di;
    int types[64], n;

    path_in(s, atr, sizeof atr, "hello.atr");
    path_in(s, xex, sizeof xex, "hello.xex");
    path_in(s, car, sizeof car, "game5200.car");
    path_in(s, raw, sizeof raw, "raw8k.rom");
    path_in(s, st, sizeof st, "state.a8s");
    path_in(s, cas, sizeof cas, "new.cas");
    CHECK(write_hello_atr(atr) == 0 && write_hello_xex(xex) == 0 && write_5200_car(car) == 0
          && write_raw_8k(raw) == 0, "test files");

    CHECK(a800session_media_kind(s, atr) == A800_MEDIA_DISK, "atr kind");
    CHECK(a800session_media_kind(s, xex) == A800_MEDIA_EXECUTABLE, "xex kind");
    CHECK(a800session_media_kind(s, car) == A800_MEDIA_CARTRIDGE, "car kind");

    test_start(s, 0);
    CHECK(a800session_open(s, atr, msg, sizeof msg) == 0, "open atr: %s", msg);
    CHECK(test_wait_text(s, "HELLO", 4000), "ATR booted from D1");
    CHECK(a800session_drive_info(s, 0, &di) == 0 && strstr(di.path, "hello.atr"), "D1 holds it");

    a800session_drive_eject(s, 0);
    CHECK(a800session_open(s, xex, msg, sizeof msg) == 0, "open xex: %s", msg);
    CHECK(test_wait_text(s, "HELLO", 4000), "XEX ran");

    CHECK(a800session_open(s, car, msg, sizeof msg) < 0 && strstr(msg, "5200"), "5200 refused: %s", msg);
    CHECK(a800session_cart_needs_type(raw) == 8, "raw 8K needs a type");
    n = a800session_cart_types_for(8, types, 64);
    CHECK(n > 1, "8K types %d", n);
    CHECK(a800session_open(s, raw, msg, sizeof msg) == 1, "open raw asks for a type");
    CHECK(a800session_cart_insert(s, raw, 1, msg, sizeof msg) == 0, "standard 8K: %s", msg);
    a800session_cart_info(s, msg, sizeof msg);
    CHECK(strstr(msg, "raw8k"), "cart info: %s", msg);
    a800session_cart_remove(s);

    CHECK(a800session_states_available(s), "states with FujiNet off");
    CHECK(a800session_state_save(s, st) == 0, "state save");
    CHECK(a800session_state_load(s, st) == 0, "state load");
    CHECK(a800session_tape_record_new(s, cas) == 0, "new tape");
    a800session_tape_eject(s);
    CHECK(a800session_new_blank_disk(atr, 2) == 0, "blank DD disk");
    a800session_stop(s);
    a800session_free(s);

    /* FujiNet on */
    s = test_session_ex("media-fn", 1);
    path_in(s, atr, sizeof atr, "hello.atr");
    write_hello_atr(atr);
    if (!test_start_fujinet(s)) {
        fprintf(stderr, "SKIP: FujiNet half (no runtime)\n");
        a800session_free(s);
        puts("media: ok (FujiNet skipped)");
        return 0;
    }
    CHECK(test_wait_fujinet_log(s, "READ DEVICE SLOTS", 20000), "CONFIG first");
    CHECK(!a800session_states_available(s), "no states with FujiNet");
    CHECK(a800session_open(s, atr, msg, sizeof msg) == 0, "open atr via FujiNet: %s", msg);
    CHECK(test_wait_text(s, "HELLO", 10000), "booted from FujiNet's D1");
    CHECK(a800session_drive_info(s, 0, &di) == 0 && di.fujinet && strstr(di.path, "hello.atr"),
          "FujiNet D1: %s", di.path);
    a800session_reboot_to_config(s);
    CHECK(test_wait_fujinet_log(s, "READ DEVICE SLOTS", 20000), "CONFIG again");
    CHECK(a800session_drive_info(s, 0, &di) == 0 && !di.path[0], "D1 ejected: %s", di.path);
    a800session_stop(s);
    a800session_free(s);
    puts("media: ok");
    return 0;
}
