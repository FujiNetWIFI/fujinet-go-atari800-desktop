/*
 * bindings_test -- the key/pad bindings table: the defaults, rebinding and the
 * steal it reports, the reserved keys (F9, F11, F12), Clear, persistence of
 * only what differs (and a reserved key in an old settings file is dropped),
 * and that a held key lets go of exactly what its press asserted even when it
 * is rebound while held. No machine is started: the merge needs none.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "a800session.h"
#include "test_tmpdir.h"

static int failures;
static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failures++;
}

static a800session *new_session(const char *tag, char *base, size_t basesz)
{
    char cfg[600], data[600];
    a800session_paths p;
    test_tmpdir(base, basesz, tag);
    snprintf(cfg, sizeof cfg, "%s/config", base);
    snprintf(data, sizeof data, "%s/data", base);
    memset(&p, 0, sizeof p);
    p.config_dir = cfg;
    p.data_dir = data;
    p.fujinet_lib = "";
    return a800session_new(&p);
}

#define B(a) (1u << (a))

int main(void)
{
    char base[512], stolen[256], name[64];
    a800session *s = new_session("bind", base, sizeof base);
    a800_binding b;
    const int up1 = A800_TARGET_PORT(0, A800_ACT_UP), fire1 = A800_TARGET_PORT(0, A800_ACT_FIRE);
    const int fire2 = A800_TARGET_PORT(1, A800_ACT_FIRE), start = A800_TARGET_CONSOLE(A800_CON_START);

    /* defaults */
    check(a800session_target_for_key(s, A800_KEYSYM_KP_8) == up1, "KP8 is joystick 1 up");
    check(a800session_target_for_key(s, A800_KEYSYM_F4) == start, "F4 is START");
    check(a800session_target_for_key(s, 'Z') == -1, "an unbound key drives nothing");
    check(a800session_binding_get(s, fire1).button == A800_PAD_BTN_SOUTH, "South fires joystick 1");
    check(A800_TARGET_COUNT == 30, "30 targets in all");

    /* reserved keys */
    check(a800session_key_reserved(A800_KEYSYM_F9) && a800session_key_reserved(A800_KEYSYM_F11)
          && a800session_key_reserved(A800_KEYSYM_F12), "F9, F11 and F12 are reserved");
    check(!a800session_key_reserved(A800_KEYSYM_F10) && !a800session_key_reserved('a') && !a800session_key_reserved(A800_KEYSYM_F4),
          "F10, letters and F4 are not");
    strcpy(stolen, "junk");
    a800session_binding_set_key(s, fire2, A800_KEYSYM_F9, stolen, sizeof stolen);
    check(a800session_binding_get(s, fire2).keysym == 0 && stolen[0] == '\0', "a reserved key cannot be bound (and steals nothing)");
    a800session_binding_set_key(s, fire2, A800_KEYSYM_F12, NULL, 0);
    check(a800session_binding_get(s, fire2).keysym == 0, "not F12 either");

    /* rebinding steals, and says from whom */
    a800session_binding_set_key(s, fire2, A800_KEYSYM_F4, stolen, sizeof stolen);
    check(strstr(stolen, "Start") != NULL, "rebinding F4 reports it took it from Start");
    check(a800session_binding_get(s, start).keysym == 0 && a800session_target_for_key(s, A800_KEYSYM_F4) == fire2, "START lost F4");
    a800session_binding_set_key(s, fire2, 'Q', NULL, 0);
    check(a800session_target_for_key(s, 'q') == fire2, "keys are case-folded");

    /* Clear: key AND button */
    a800session_binding_clear(s, fire1);
    b = a800session_binding_get(s, fire1);
    check(b.keysym == 0 && b.button == A800_PAD_BTN_NONE, "Clear takes both the key and the pad button");
    check(a800session_target_for_key(s, A800_KEYSYM_KP_0) == -1, "KP 0 is free");
    a800session_binding_clear(s, -1);
    a800session_binding_clear(s, A800_TARGET_COUNT);
    check(1, "Clear ignores a target out of range");

    /* persistence: only the difference, and it comes back */
    a800session_settings_flush(s);
    a800session_free(s);
    {
        a800session_paths p;
        char cfg[600], data[600];
        snprintf(cfg, sizeof cfg, "%s/config", base);
        snprintf(data, sizeof data, "%s/data", base);
        memset(&p, 0, sizeof p);
        p.config_dir = cfg; p.data_dir = data; p.fujinet_lib = "";
        s = a800session_new(&p);
    }
    check(a800session_target_for_key(s, 'q') == fire2, "a rebinding survives a restart");
    b = a800session_binding_get(s, fire1);
    check(b.keysym == 0 && b.button == A800_PAD_BTN_NONE, "and so does a Clear");
    check(a800session_target_for_key(s, A800_KEYSYM_KP_8) == up1, "an untouched default is still there");

    /* a reserved key in an old settings file is dropped on load */
    /* F11 (65480) on joystick 1's fire (target 4), F10 (65479) on START (target 20) */
    a800session_set_str(s, "bindings", "4.k:65480 20.k:65479");
    a800session_settings_flush(s);
    a800session_free(s);
    {
        a800session_paths p;
        char cfg[600], data[600];
        snprintf(cfg, sizeof cfg, "%s/config", base);
        snprintf(data, sizeof data, "%s/data", base);
        memset(&p, 0, sizeof p);
        p.config_dir = cfg; p.data_dir = data; p.fujinet_lib = "";
        s = a800session_new(&p);
    }
    check(a800session_binding_get(s, A800_TARGET_PORT(0, A800_ACT_FIRE)).keysym == 0, "F11 in an old settings file is dropped");
    check(a800session_binding_get(s, start).keysym == A800_KEYSYM_F10, "while F10 is fine");
    a800session_bindings_reset(s);

    /* a held key lets go of what it pressed, even rebound meanwhile */
    check(a800session_key(s, A800_KEYSYM_KP_8, 0, 1) == 1, "KP8 down is taken");
    check(a800session_buttons_held(s, 0) == B(A800_ACT_UP), "and holds joystick 1 up");
    check(a800session_key(s, A800_KEYSYM_KP_8, 0, 1) == 1 && a800session_buttons_held(s, 0) == B(A800_ACT_UP), "auto-repeat changes nothing");
    a800session_binding_set_key(s, A800_TARGET_PORT(1, A800_ACT_UP), A800_KEYSYM_KP_8, NULL, 0);   /* KP8 is now joystick 2 up */
    check(a800session_key(s, A800_KEYSYM_KP_8, 0, 0) == 1, "KP8 up is taken");
    check(a800session_buttons_held(s, 0) == 0 && a800session_buttons_held(s, 1) == 0, "it let go of joystick 1 up, not stranded by the rebinding");
    a800session_bindings_reset(s);

    /* the keyboard's hold and the window's hold are separate */
    a800session_key(s, A800_KEYSYM_KP_0, 0, 1);               /* fire from the keyboard */
    a800session_press(s, fire1, 1);                           /* and from the window */
    check(a800session_buttons_held(s, 0) == B(A800_ACT_FIRE), "fire held twice is held");
    a800session_key(s, A800_KEYSYM_KP_0, 0, 0);
    check(a800session_buttons_held(s, 0) == B(A800_ACT_FIRE), "the key letting go leaves the window's hold");
    a800session_press(s, fire1, 0);
    check(a800session_buttons_held(s, 0) == 0, "and now nothing is");

    /* names */
    a800session_keysym_name(A800_KEYSYM_F9, name, sizeof name);
    check(!strcmp(name, "F9"), "F9 is named");
    check(!strcmp(a800_target_name(start), "Start") && !strcmp(a800_target_name(up1), "Joystick 1: Up"), "target names");

    a800session_free(s);
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
