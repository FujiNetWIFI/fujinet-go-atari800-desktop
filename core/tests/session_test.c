/* The session without FujiNet: start, boot, every machine, re-init, video,
   audio, keyboard, settings that apply live. */
#include "test_util.h"

static void type_keys(a800session *s, const char *text)
{
    const char *p;
    for (p = text; *p; p++) {
        uint32_t ks = *p == '\n' ? A800_KEYSYM_RETURN : (uint32_t)(unsigned char)*p;
        uint32_t uc = *p == '\n' ? 0 : (uint32_t)(unsigned char)*p;
        if (ks >= 'A' && ks <= 'Z')
            ks += 32;
        a800session_key(s, ks, uc, 1);
        test_sleep_ms(60);
        a800session_key(s, ks, uc, 0);
        test_sleep_ms(60);
    }
}

int main(void)
{
    a800session *s = test_session("session");
    uint32_t *fb = malloc(A800SESSION_FB_MAX_WIDTH * A800SESSION_FB_MAX_HEIGHT * 4);
    uint64_t serial = 0;
    int w, h, par, m, i, colours;
    float audio[2 * 1024];

    CHECK(s, "new");
    test_set_pref(s, "basic", 1);
    test_set_pref(s, "machine", A800_M_800XL);
    test_start(s, 0);
    CHECK(a800session_is_running(s), "running");
    CHECK(test_wait_text(s, "Ready", 3000), "800XL boots BASIC");

    /* a picture with more than one colour */
    CHECK(a800session_copy_frame(s, fb, &w, &h, &par, &serial), "frame");
    CHECK(w == 336 && (h == 224 || h == 240), "TV area %dx%d", w, h);
    for (i = 0, colours = 0; i < w * h; i++)
        if (fb[i] != fb[0]) { colours = 1; break; }
    CHECK(colours, "picture has content");
    CHECK(a800session_refresh_rate(s) == 59923, "NTSC rate %d", a800session_refresh_rate(s));

    /* typing */
    type_keys(s, "PRINT 6*7\n");
    CHECK(test_wait_text(s, "42", 3000), "typed PRINT 6*7");

    /* audio flows (silent BASIC still renders frames) */
    a800session_render_audio(s, audio, 1024);

    /* every machine boots its BASIC (the 1400XL/1450XLD without their ROMs
       behave as an 800XL) */
    for (m = 0; m < A800_M_COUNT; m++) {
        test_set_pref(s, "machine", m);
        CHECK(a800session_apply_settings(s) == A800_APPLY_POWER, "machine %d power", m);
        CHECK(test_wait_text(s, "Ready", 4000), "%s boots", a800_machine_name(m));
    }

    /* PAL */
    test_set_pref(s, "machine", A800_M_130XE);
    test_set_pref(s, "tv", 1);
    a800session_apply_settings(s);
    CHECK(test_wait_text(s, "Ready", 4000), "PAL boots");
    CHECK(a800session_refresh_rate(s) == 49861, "PAL rate %d", a800session_refresh_rate(s));

    /* live settings: no power cycle */
    test_set_pref(s, "artifact_pal", 2);
    CHECK(a800session_apply_settings(s) == A800_APPLY_LIVE, "blend is live");
    test_set_pref(s, "tv", 0);
    test_set_pref(s, "artifact_ntsc", 3);
    a800session_apply_settings(s);
    CHECK(test_wait_text(s, "Ready", 4000), "NTSC again");
    test_sleep_ms(200);
    serial = 0;
    a800session_copy_frame(s, fb, &w, &h, &par, &serial);
    CHECK(w == 588 && par < 1000, "NTSC filter %dx%d par %d", w, h, par);

    /* stop and start again */
    for (i = 0; i < 5; i++) {
        a800session_stop(s);
        test_start(s, 0);
        CHECK(test_wait_text(s, "Ready", 4000), "restart %d", i);
    }
    a800session_free(s);
    free(fb);
    puts("session: ok");
    return 0;
}
