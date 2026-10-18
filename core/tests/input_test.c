/* The keyboard: characters and positions to Atari keys; typing and pasting
   into BASIC; the on-screen keyboard; the bindings. */
#include "test_util.h"
#include "keymap.h"

#define AKEY_a 0x3f
#define AKEY_SHFT 0x40
#define AKEY_CTRL 0x80
#define AKEY_EXCLAMATION 0x5f
#define AKEY_LESS 0x36
#define AKEY_UP 0x8e
#define AKEY_F1 0x03

static void type_key(a800session *s, uint32_t ks, uint32_t uc)
{
    a800session_key(s, ks, uc, 1);
    test_sleep_ms(60);
    a800session_key(s, ks, uc, 0);
    test_sleep_ms(60);
}

int main(void)
{
    a800session *s;
    int k, n, i;
    const a800_kbd_key *keys;
    a800_binding b;
    char stolen[128];

    /* characters: what the host keyboard produced, whatever its layout */
    CHECK(keymap_translate('a', 'a', 0, 0, 0, 0, &k) && k == AKEY_a, "a");
    CHECK(keymap_translate('a', 'A', 1, 0, 0, 0, &k) && k == (AKEY_a | AKEY_SHFT), "A");
    CHECK(keymap_translate('1', '!', 1, 0, 0, 0, &k) && k == AKEY_EXCLAMATION, "!");
    CHECK(keymap_translate('q', 'a', 0, 0, 0, 0, &k) && k == AKEY_a, "AZERTY a on the Q key");
    CHECK(keymap_translate('a', 1, 0, 1, 0, 0, &k) && k == (AKEY_a | AKEY_CTRL), "Ctrl+A");
    /* positions: the US key in the Atari's place */
    CHECK(keymap_translate('-', '-', 0, 0, 1, 0, &k) && k == AKEY_LESS, "- is < by position");
    CHECK(keymap_translate(A800_KEYSYM_UP, 0, 0, 0, 0, 0, &k) && k == AKEY_UP, "Up arrow");
    CHECK(keymap_translate(A800_KEYSYM_UP, 0, 0, 0, 0, 1, &k) && k == AKEY_F1, "1200XL F1");
    CHECK(!keymap_translate(A800_KEYSYM_F9, 0, 0, 0, 0, 0, &k), "F9 is the frontends'");

    /* the bindings */
    s = test_session("input");
    CHECK(a800session_target_for_key(s, A800_KEYSYM_KP_8) == A800_TARGET_PORT(0, A800_ACT_UP), "KP8 up");
    CHECK(a800session_target_for_key(s, A800_KEYSYM_F4) == A800_TARGET_CONSOLE(A800_CON_START), "F4 START");
    a800session_binding_set_key(s, A800_TARGET_PORT(1, A800_ACT_FIRE), A800_KEYSYM_F4, stolen, sizeof stolen);
    CHECK(strstr(stolen, "Start"), "rebinding steals: %s", stolen);
    b = a800session_binding_get(s, A800_TARGET_CONSOLE(A800_CON_START));
    CHECK(b.keysym == 0, "START lost F4");
    a800session_bindings_reset(s);
    CHECK(a800session_target_for_key(s, A800_KEYSYM_F4) == A800_TARGET_CONSOLE(A800_CON_START), "reset");

    /* typing and pasting into BASIC */
    test_set_pref(s, "basic", 1);
    test_set_pref(s, "machine", A800_M_800XL);
    test_start(s, 0);
    CHECK(test_wait_text(s, "Ready", 3000), "boot");
    type_key(s, 'p', 'p');
    n = a800session_paste(s, "RINT 1+1\nPRINT \"Hi\"\n");
    CHECK(n > 10, "paste queued %d", n);
    for (i = 0; i < 200 && a800session_paste_pending(s); i++)
        test_sleep_ms(50);
    CHECK(test_wait_text(s, " 2", 3000), "pasted PRINT 1+1");
    CHECK(test_wait_text(s, "HI", 3000), "paste upper-cases");

    /* the on-screen keyboard */
    n = a800session_keyboard_layout(s, &keys);
    CHECK(n > 55, "layout %d keys", n);
    for (i = 0; i < n && strcmp(keys[i].label, "HELP"); i++)
        ;
    CHECK(i < n, "XL has HELP");
    for (i = 0; i < n && strcmp(keys[i].label, "7"); i++)
        ;
    a800session_keyboard_press(s, keys[i].index, 1);
    test_sleep_ms(80);
    CHECK(a800session_keyboard_held(s, keys[i].index), "7 held");
    a800session_keyboard_press(s, keys[i].index, 0);
    CHECK(test_wait_text(s, "\n  7", 3000), "the on-screen 7 reached BASIC");

    a800session_stop(s);
    a800session_free(s);
    puts("input: ok");
    return 0;
}
