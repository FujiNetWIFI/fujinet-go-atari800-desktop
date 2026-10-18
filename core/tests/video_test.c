/* Every picture path: the three visible areas, the NTSC filter, PAL blending
   and each 80-column card (fed filler "ROMs": only the geometry is checked). */
#include "test_util.h"
#include <stdint.h>

static uint32_t *fb;

static void frame(a800session *s, int *w, int *h, int *par)
{
    uint64_t serial = 0;
    test_sleep_ms(250);
    a800session_copy_frame(s, fb, w, h, par, &serial);
}

static void filler_rom(a800session *s, const char *key, size_t size)
{
    char path[1100], msg[256];
    uint8_t *d = malloc(size);
    size_t i;
    int slot;
    FILE *fp;
    for (i = 0; i < size; i++)
        d[i] = (uint8_t)(i * 13 + 7);
    snprintf(path, sizeof path, "%s/%s.bin", a800session_data_path(s), key);
    fp = fopen(path, "wb");
    fwrite(d, 1, size, fp);
    fclose(fp);
    free(d);
    for (slot = 0; slot < a800session_rom_slot_count(); slot++)
        if (!strcmp(a800session_rom_slot(slot)->key, key))
            break;
    CHECK(a800session_rom_import(s, path, slot, msg, sizeof msg) == 1, "%s: %s", key, msg);
}

int main(void)
{
    a800session *s = test_session("video");
    int w, h, par;
    fb = malloc(A800SESSION_FB_MAX_WIDTH * A800SESSION_FB_MAX_HEIGHT * 4);
    CHECK(s, "new");
    test_set_pref(s, "basic", 1);
    test_start(s, 0);
    CHECK(test_wait_text(s, "Ready", 3000), "boot");

    test_set_pref(s, "area", 0);
    a800session_apply_settings(s);
    frame(s, &w, &h, &par);
    CHECK(w == 320 && h == 200 && par == 1000, "narrow %dx%d", w, h);
    test_set_pref(s, "area", 2);
    a800session_apply_settings(s);
    frame(s, &w, &h, &par);
    CHECK(w == 384 && h == 240, "full %dx%d", w, h);
    test_set_pref(s, "artifact_ntsc", 3);
    a800session_apply_settings(s);
    frame(s, &w, &h, &par);
    CHECK(w == 672 && h == 240 && par == 571, "NTSC filter overscan %dx%d %d", w, h, par);
    test_set_pref(s, "area", 1);
    test_set_pref(s, "tv", 1);
    test_set_pref(s, "artifact_pal", 2);
    a800session_apply_settings(s);
    CHECK(test_wait_text(s, "Ready", 3000), "PAL boot");
    frame(s, &w, &h, &par);
    CHECK(w == 336 && h == 240 && par == 1000, "PAL blend %dx%d", w, h);
    test_set_pref(s, "tv", 0);
    test_set_pref(s, "artifact_ntsc", 0);

    /* the 80-column cards */
    filler_rom(s, "xep80_charset", 8192);
    filler_rom(s, "af80", 4096);
    filler_rom(s, "af80_charset", 4096);
    filler_rom(s, "bit3", 4096);
    filler_rom(s, "bit3_charset", 4096);
    filler_rom(s, "proto80", 2048);
    {
        static const struct { int card, w, h; } cards[] = {
            {1, 560, 250}, {2, 640, 250}, {3, 640, 240}, {4, 640, 192}};
        int i;
        for (i = 0; i < 4; i++) {
            test_set_pref(s, "col80", cards[i].card);
            CHECK(a800session_apply_settings(s) == A800_APPLY_POWER, "card %d power", i);
            CHECK(a800session_80col_fitted(s), "card %d fitted", cards[i].card);
            a800session_set_80col_display(s, 1);
            frame(s, &w, &h, &par);
            CHECK(w == cards[i].w && h == cards[i].h && par == 500, "card %d: %dx%d par %d",
                  cards[i].card, w, h, par);
            a800session_set_80col_display(s, 0);
            frame(s, &w, &h, &par);
            CHECK(w == 336, "card %d off: %d", cards[i].card, w);
        }
    }
    a800session_free(s);
    free(fb);
    puts("video: ok");
    return 0;
}
