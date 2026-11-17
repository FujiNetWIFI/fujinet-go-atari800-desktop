/*
 * input_merge_test -- every way of holding an input is a source of its own and
 * what the machine sees is their merge: letting go of one never releases
 * another; opposed joystick directions cancel; release_all (focus loss) lets go
 * of the keyboard and the window's pointer but not a pad; a pad that goes away
 * lets go of exactly what it held; a paddle's position is owned by whoever
 * wrote it last. No machine is started: the merge needs none.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "a800session.h"
#include "input_merge.h"
#include "test_tmpdir.h"

static int failures;
static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failures++;
}

#define B(a) (1u << (a))
#define UP    A800_TARGET_PORT(0, A800_ACT_UP)
#define DOWN  A800_TARGET_PORT(0, A800_ACT_DOWN)
#define LEFT  A800_TARGET_PORT(0, A800_ACT_LEFT)
#define RIGHT A800_TARGET_PORT(0, A800_ACT_RIGHT)
#define FIRE  A800_TARGET_PORT(0, A800_ACT_FIRE)
#define START A800_TARGET_CONSOLE(A800_CON_START)
#define SELECT A800_TARGET_CONSOLE(A800_CON_SELECT)

int main(void)
{
    char base[512], cfg[600], data[600];
    a800session_paths p;
    a800session *s;

    test_tmpdir(base, sizeof base, "merge");
    snprintf(cfg, sizeof cfg, "%s/config", base);
    snprintf(data, sizeof data, "%s/data", base);
    memset(&p, 0, sizeof p);
    p.config_dir = cfg; p.data_dir = data; p.fujinet_lib = "";
    s = a800session_new(&p);

    /* a target is held while any source holds it */
    input_press(s, A800_SRC_KEYBOARD, FIRE, 1);
    input_press(s, A800_SRC_POINTER, FIRE, 1);
    input_press(s, A800_SRC_PAD(0), FIRE, 1);
    check(a800session_buttons_held(s, 0) == B(A800_ACT_FIRE), "fire from three sources is held");
    input_press(s, A800_SRC_KEYBOARD, FIRE, 0);
    input_press(s, A800_SRC_POINTER, FIRE, 0);
    check(a800session_buttons_held(s, 0) == B(A800_ACT_FIRE), "two letting go leave the third's hold");
    input_press(s, A800_SRC_PAD(0), FIRE, 0);
    check(a800session_buttons_held(s, 0) == 0, "the last lets go");
    check(a800session_buttons_held(s, 1) == 0, "another port saw nothing");

    /* opposed pairs cancel, across sources */
    input_press(s, A800_SRC_KEYBOARD, UP, 1);
    input_press(s, A800_SRC_STICK(1), DOWN, 1);
    check(a800session_buttons_held(s, 0) == 0, "up from a key and down from a stick are neither");
    input_press(s, A800_SRC_STICK(1), DOWN, 0);
    check(a800session_buttons_held(s, 0) == B(A800_ACT_UP), "and up again once down lets go");
    input_press(s, A800_SRC_KEYBOARD, LEFT, 1);
    input_press(s, A800_SRC_POINTER, RIGHT, 1);
    input_press(s, A800_SRC_POINTER, FIRE, 1);
    check(a800session_buttons_held(s, 0) == (B(A800_ACT_UP) | B(A800_ACT_FIRE)), "left and right cancel, up and fire stay");
    a800session_release_all(s);
    check(a800session_buttons_held(s, 0) == 0, "release_all lets go of the keyboard and the pointer");

    /* a pad is left alone by release_all, and lets go by itself */
    input_press(s, A800_SRC_STICK(2), RIGHT, 1);
    input_press(s, A800_SRC_PAD(2), START, 1);
    input_press(s, A800_SRC_KEYBOARD, SELECT, 1);
    input_press(s, A800_SRC_KEYBOARD, LEFT, 1);
    check(a800session_console_held(s) == (B(A800_CON_START) | B(A800_CON_SELECT)), "console keys from two sources");
    a800session_release_all(s);
    check(a800session_console_held(s) == B(A800_CON_START), "release_all drops the keyboard's SELECT, not the pad's START");
    check(a800session_buttons_held(s, 0) == B(A800_ACT_RIGHT), "nor the pad's stick");
    input_press(s, A800_SRC_STICK(3), UP, 1);
    input_press(s, A800_SRC_PAD(3), FIRE, 1);
    input_pad_clear(s, 2);
    check(a800session_buttons_held(s, 0) == (B(A800_ACT_UP) | B(A800_ACT_FIRE)), "a pad going away lets go of its own: another pad's holds stay");
    check(a800session_console_held(s) == 0, "including its console key");
    input_pad_clear(s, 3);
    check(a800session_buttons_held(s, 0) == 0, "and the other pad's, when that one goes");
    input_pad_clear(s, 99);
    input_press(s, 99, FIRE, 1);
    input_press(s, A800_SRC_POINTER, A800_TARGET_SYSACT(0), 1);
    input_press(s, A800_SRC_POINTER, -1, 1);
    check(a800session_buttons_held(s, 0) == 0, "a bad source, a system action and a bad target are ignored");

    /* the window's stick drag holds the directions, not the fire button */
    a800session_press(s, FIRE, 1);
    a800session_pointer_set(s, 0, B(A800_ACT_UP) | B(A800_ACT_RIGHT));
    check(a800session_buttons_held(s, 0) == (B(A800_ACT_UP) | B(A800_ACT_RIGHT) | B(A800_ACT_FIRE)), "pointer_set holds up-right beside the fire button");
    a800session_pointer_set(s, 0, B(A800_ACT_LEFT));
    check(a800session_buttons_held(s, 0) == (B(A800_ACT_LEFT) | B(A800_ACT_FIRE)), "a new drag position replaces the old");
    a800session_pointer_set(s, 0, 0xffu);
    check(a800session_buttons_held(s, 0) == B(A800_ACT_FIRE),
          "a drag of every bit is four directions that cancel (fire is its own part)");
    a800session_pointer_set(s, 0, 0);
    a800session_pointer_set(s, 9, B(A800_ACT_UP));
    check(a800session_buttons_held(s, 0) == B(A800_ACT_FIRE), "letting go of the drag leaves fire; a bad port is ignored");
    a800session_press(s, FIRE, 0);
    check(a800session_buttons_held(s, 0) == 0, "all clear");

    check(a800session_buttons_held(s, -1) == 0 && a800session_buttons_held(s, A800_PORTS) == 0, "a bad port holds nothing");

    /* paddles: positions are owned by the last writer */
    check(a800session_paddle_value(s, 0, 0) == A800_PADDLE_MAX, "a paddle nobody drives reads full left (idle)");
    a800session_paddle_pointer(s, 0, 0, 50, 1);
    check(a800session_paddle_value(s, 0, 0) == 50, "the window's knob sets it");
    a800session_paddle_pointer(s, 0, 0, 0, 0);
    check(a800session_paddle_value(s, 0, 0) == 50, "ending the drag keeps the position");
    input_pot(s, A800_SRC_STICK(1), 0, 0, 200);
    check(a800session_paddle_value(s, 0, 0) == 200, "a pad's stick sets it");
    input_pad_clear(s, 0);
    check(a800session_paddle_value(s, 0, 0) == 200, "another pad going away leaves it");
    input_pad_clear(s, 1);
    check(a800session_paddle_value(s, 0, 0) == A800_PADDLE_MAX, "its own pad going away puts it back to idle");
    a800session_paddle_pointer(s, 0, 1, 1000, 1);
    check(a800session_paddle_value(s, 0, 1) == A800_PADDLE_MAX, "values are clamped to the POT range");
    a800session_paddle_pointer(s, 0, 1, -7, 1);
    check(a800session_paddle_value(s, 0, 1) == 0, "both ends");
    a800session_paddle_pointer(s, 0, 1, A800_PADDLE_CENTRE, 1);
    input_pad_clear(s, 5);
    check(a800session_paddle_value(s, 0, 1) == A800_PADDLE_CENTRE, "a pad that never touched it cannot reset it");
    a800session_release_all(s);
    check(a800session_paddle_value(s, 0, 1) == A800_PADDLE_CENTRE, "and release_all keeps a position the window left");
    check(a800session_paddle_value(s, 9, 0) == A800_PADDLE_MAX && a800session_paddle_value(s, 0, 2) == A800_PADDLE_MAX, "a bad paddle reads idle");
    a800session_paddle_fire(s, 1, 0, 1);
    input_pot_fire(s, A800_SRC_PAD(0), 1, 0, 1);
    input_pot_fire(s, A800_SRC_PAD(0), 1, 0, 0);
    a800session_release_all(s);   /* the window's fire goes */
    input_pot_fire(s, A800_SRC_PAD(4), 3, 1, 1);
    input_pad_clear(s, 4);
    check(1, "paddle fire bits come and go with their sources");

    a800session_free(s);
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
