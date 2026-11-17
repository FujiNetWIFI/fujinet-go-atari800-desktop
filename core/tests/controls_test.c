/*
 * controls_test -- the on-screen controllers' layout, pure: for every device
 * on every port the layout has a sane extent (it depends only on the number of
 * ports), every part is inside it and drives a target of its own port or the
 * console, each device has the parts it should, the hit test finds the topmost
 * pressable part and treats round parts as ellipses, and the joystick and knob
 * drag maths.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "a800session.h"

static int failures;
static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failures++;
}

static a800_control g_c[A800_CONTROLS_MAX];

static int build(int d0, int d1, int d2, int d3, int nports, int xl, float *w, float *h)
{
    const int dev[A800_PORTS] = { d0, d1, d2, d3 };
    return a800session_controls_layout(dev, nports, xl, g_c, A800_CONTROLS_MAX, w, h);
}

static int count_kind(int n, int kind, int port)
{
    int i, num = 0;
    for (i = 0; i < n; i++)
        if (g_c[i].kind == kind && g_c[i].port == port) num++;
    return num;
}

static int has_target(int n, int target)
{
    int i;
    for (i = 0; i < n; i++)
        if (g_c[i].target == target) return 1;
    return 0;
}

static int find_arg(int n, int kind, int port, int arg)
{
    int i;
    for (i = 0; i < n; i++)
        if (g_c[i].kind == kind && g_c[i].port == port && g_c[i].arg == arg) return i;
    return -1;
}

static void every_device(void)
{
    int dev, nports, ok_inside = 1, ok_targets = 1, ok_unique = 1, ok_count = 1, ok_ext = 1;
    float w2 = 0, h2 = 0, w4 = 0, h4 = 0;

    for (nports = 2; nports <= 4; nports += 2) {
        float w0 = 0, h0 = 0;
        for (dev = 0; dev < A800_DEV_COUNT; dev++) {
            int other, n, i, j;
            float w, h;
            for (other = 0; other < A800_DEV_COUNT; other += 3) {
                n = build(dev, other, other, dev, nports, 1, &w, &h);
                if (n <= 0 || n >= A800_CONTROLS_MAX) ok_count = 0;
                if (w0 == 0) { w0 = w; h0 = h; }
                if (w != w0 || h != h0) ok_ext = 0;
                for (i = 0; i < n; i++) {
                    if (g_c[i].x < 0 || g_c[i].y < 0 || g_c[i].x + g_c[i].w > w + 0.001f
                        || g_c[i].y + g_c[i].h > h + 0.001f)
                        ok_inside = 0;
                    if (g_c[i].target >= 0) {
                        if (g_c[i].target >= A800_TARGET_COUNT) ok_targets = 0;
                        if (g_c[i].port >= 0 && g_c[i].target / A800_ACT_PER_PORT != g_c[i].port) ok_targets = 0;
                        if (g_c[i].port < 0 && (g_c[i].target < A800_TARGET_CONSOLE(0))) ok_targets = 0;
                        for (j = i + 1; j < n; j++)
                            if (g_c[j].target == g_c[i].target) ok_unique = 0;
                    }
                }
            }
        }
        if (nports == 2) { w2 = w0; h2 = h0; } else { w4 = w0; h4 = h0; }
    }
    check(ok_count, "every device on every port has a layout that fits the buffer");
    check(ok_ext, "the extent depends only on the number of ports");
    check(ok_inside, "every part is inside it");
    check(ok_targets, "every part drives a target of its own port (or the console's)");
    check(ok_unique, "no target has two parts in one view");
    check(w2 > 20.0f && w4 == w2 && h4 > h2, "four ports are two rows tall, the same width");
}

static void views(void)
{
    int n, i, key, seen[A800_CX85_COUNT];
    float w, h;

    /* joystick + paddles + nothing + ST mouse (four ports) */
    n = build(A800_DEV_JOYSTICK, A800_DEV_PADDLES, A800_DEV_NONE, A800_DEV_ST_MOUSE, 4, 1, &w, &h);
    check(count_kind(n, A800_CTL_STICK, 0) == 1, "a joystick port has the stick");
    check(has_target(n, A800_TARGET_PORT(0, A800_ACT_FIRE)), "and the fire button");
    check(!has_target(n, A800_TARGET_PORT(1, A800_ACT_FIRE)), "a paddle port has no joystick fire");
    check(count_kind(n, A800_CTL_KNOB, 1) == 2, "a paddle port has two knobs");
    check(find_arg(n, A800_CTL_KNOB, 1, 0) >= 0 && find_arg(n, A800_CTL_KNOB, 1, 1) >= 0, "for paddles 0 and 1");
    check(find_arg(n, A800_CTL_PADDLE_FIRE, 1, 0) >= 0 && find_arg(n, A800_CTL_PADDLE_FIRE, 1, 1) >= 0,
          "each with a fire button");
    check(count_kind(n, A800_CTL_TURN, 1) == 4, "and two turn arrows");
    {
        int left = 0, right = 0;
        for (i = 0; i < n; i++)
            if (g_c[i].kind == A800_CTL_TURN && g_c[i].port == 1) {
                if (g_c[i].dir < 0) left++;
                if (g_c[i].dir > 0) right++;
            }
        check(left == 2 && right == 2, "one of each way per paddle");
    }
    check(count_kind(n, A800_CTL_STICK, 2) == 0 && count_kind(n, A800_CTL_BODY, 2) == 1, "an empty port is a body and nothing to press");
    check(count_kind(n, A800_CTL_MAT, 3) == 1 && count_kind(n, A800_CTL_POINTER_BUTTON, 3) == 2, "a mouse port has the mat and two buttons");
    check(find_arg(n, A800_CTL_POINTER_BUTTON, 3, 1) >= 0 && find_arg(n, A800_CTL_POINTER_BUTTON, 3, 2) >= 0,
          "buttons are bits 1 and 2");

    /* the absolute devices, the Trak-Ball, the light pen and gun, the CX85 */
    n = build(A800_DEV_KOALA, A800_DEV_TOUCH_TABLET, A800_DEV_LIGHTPEN, A800_DEV_LIGHTGUN, 4, 1, &w, &h);
    check(count_kind(n, A800_CTL_PAD, 0) == 1 && count_kind(n, A800_CTL_POINTER_BUTTON, 0) == 2, "a Koala Pad has its pad and two buttons");
    check(count_kind(n, A800_CTL_PAD, 1) == 1 && find_arg(n, A800_CTL_PAD, 1, 1) >= 0, "the touch tablet's pad touches with bit 1");
    check(count_kind(n, A800_CTL_NOTE, 2) == 1 && count_kind(n, A800_CTL_NOTE, 3) == 1, "the light pen and gun say to aim on the picture");
    check(count_kind(n, A800_CTL_STICK, 2) == 0 && count_kind(n, A800_CTL_MAT, 3) == 0, "and have nothing to press in the window");
    n = build(A800_DEV_TRAKBALL, A800_DEV_CX85, A800_DEV_AMIGA_MOUSE, A800_DEV_NONE, 4, 1, &w, &h);
    check(count_kind(n, A800_CTL_BALL, 0) == 1 && count_kind(n, A800_CTL_POINTER_BUTTON, 0) == 2, "the Trak-Ball has its ball and buttons");
    memset(seen, 0, sizeof seen);
    for (i = 0; i < n; i++)
        if (g_c[i].kind == A800_CTL_CX85_KEY && g_c[i].port == 1) {
            if (g_c[i].arg >= 0 && g_c[i].arg < A800_CX85_COUNT) seen[g_c[i].arg]++;
        }
    for (key = 0; key < A800_CX85_COUNT; key++)
        if (seen[key] != 1) break;
    check(key == A800_CX85_COUNT, "the CX85 has all 17 keys once, YES and NO among them");
    check(count_kind(n, A800_CTL_MAT, 2) == 1, "an Amiga mouse has a mat");

    /* two ports (an XL): one row; HELP only with the XL key */
    {
        float w2, h2, w4, h4;
        int n2, n4, helps = 0, helps_xl = 0;
        n2 = build(A800_DEV_JOYSTICK, A800_DEV_JOYSTICK, 0, 0, 2, 0, &w2, &h2);
        for (i = 0; i < n2; i++)
            if (g_c[i].kind == A800_CTL_CONSOLE_KEY && g_c[i].target == A800_TARGET_CONSOLE(A800_CON_HELP)) helps++;
        n4 = build(A800_DEV_JOYSTICK, A800_DEV_JOYSTICK, 0, 0, 2, 1, &w4, &h4);
        for (i = 0; i < n4; i++)
            if (g_c[i].kind == A800_CTL_CONSOLE_KEY && g_c[i].target == A800_TARGET_CONSOLE(A800_CON_HELP)) helps_xl++;
        check(helps == 0 && helps_xl == 1 && n4 == n2 + 1, "HELP is a console key on an XL/XE only");
        check(w2 == w4 && h2 == h4, "and the extent does not change with it");
        check(count_kind(n4, A800_CTL_CAPTION, 2) == 0, "two ports: no captions for ports 3 and 4");
    }
    n = build(A800_DEV_JOYSTICK, A800_DEV_JOYSTICK, A800_DEV_JOYSTICK, A800_DEV_JOYSTICK, 4, 1, &w, &h);
    for (key = 0; key < A800_CON_COUNT; key++)
        if (!has_target(n, A800_TARGET_CONSOLE(key))) break;
    check(key == A800_CON_COUNT, "the console strip has START SELECT OPTION HELP RESET BREAK");
    check(build(-5, 99, 99, -1, 4, 1, &w, &h) > 0, "an unknown device is drawn as nothing");
    check(a800session_controls_layout(NULL, 4, 1, g_c, A800_CONTROLS_MAX, &w, &h) > 0, "no devices at all is four empty ports");
}

static void hit(void)
{
    int n, i, idx;
    float w, h, cx, cy;

    n = build(A800_DEV_JOYSTICK, A800_DEV_PADDLES, A800_DEV_NONE, A800_DEV_NONE, 4, 1, &w, &h);
    /* the stick's centre, and a corner of its box (outside the ellipse) */
    for (i = 0; i < n; i++)
        if (g_c[i].kind == A800_CTL_STICK) break;
    cx = g_c[i].x + g_c[i].w * 0.5f; cy = g_c[i].y + g_c[i].h * 0.5f;
    check(a800session_controls_hit(g_c, n, cx, cy) == i, "the hit test finds the stick at its centre");
    check(a800session_controls_hit(g_c, n, g_c[i].x + 0.05f, g_c[i].y + 0.05f) != i, "but not in the corner of its box (round)");
    /* the fire button sits over the case's body: topmost wins */
    for (i = 0; i < n; i++)
        if (g_c[i].kind == A800_CTL_ROUND_BUTTON) break;
    cx = g_c[i].x + g_c[i].w * 0.5f; cy = g_c[i].y + g_c[i].h * 0.5f;
    idx = a800session_controls_hit(g_c, n, cx, cy);
    check(idx == i && g_c[idx].target == A800_TARGET_PORT(0, A800_ACT_FIRE), "the fire button is hit over the case");
    for (i = 0; i < n; i++)
        if (g_c[i].kind == A800_CTL_BODY) break;
    check(a800session_controls_hit(g_c, n, g_c[i].x + 0.02f, g_c[i].y + 0.02f) == -1, "a body is never hit");
    check(a800session_controls_hit(g_c, n, -1.f, -1.f) == -1 && a800session_controls_hit(g_c, n, 999.f, 999.f) == -1, "nothing outside the layout");
    check(a800session_controls_hit(NULL, 0, 1.f, 1.f) == -1, "no layout, no hit");
    idx = find_arg(n, A800_CTL_PADDLE_FIRE, 1, 1);
    cx = g_c[idx].x + g_c[idx].w * 0.5f; cy = g_c[idx].y + g_c[idx].h * 0.5f;
    check(a800session_controls_hit(g_c, n, cx, cy) == idx, "paddle B's fire button is a rectangle part");
    /* every pressable part is hit at its own centre (the knobs sit over their bodies) */
    {
        int ok = 1;
        for (i = 0; i < n; i++) {
            if (g_c[i].kind == A800_CTL_BODY || g_c[i].kind == A800_CTL_CAPTION || g_c[i].kind == A800_CTL_NOTE) continue;
            cx = g_c[i].x + g_c[i].w * 0.5f; cy = g_c[i].y + g_c[i].h * 0.5f;
            if (a800session_controls_hit(g_c, n, cx, cy) != i) ok = 0;
        }
        check(ok, "every pressable part is hit at its own centre");
    }
}

static void drags(void)
{
    const unsigned U = 1u << A800_ACT_UP, D = 1u << A800_ACT_DOWN, L = 1u << A800_ACT_LEFT, R = 1u << A800_ACT_RIGHT;
    int v;
    float a;

    check(a800session_stick_from_vector(0, 0, 2.1f) == 0, "a stick at its centre holds nothing");
    check(a800session_stick_from_vector(0.4f, 0.2f, 2.1f) == 0, "and inside a quarter of the reach");
    check(a800session_stick_from_vector(1.5f, 0, 2.1f) == R && a800session_stick_from_vector(-1.5f, 0, 2.1f) == L, "left and right");
    check(a800session_stick_from_vector(0, -1.5f, 2.1f) == U && a800session_stick_from_vector(0, 1.5f, 2.1f) == D, "up and down");
    check(a800session_stick_from_vector(1.2f, -1.2f, 2.1f) == (R | U) && a800session_stick_from_vector(-1.2f, 1.2f, 2.1f) == (L | D), "the diagonals are two bits");
    check(a800session_stick_from_vector(1.5f, 0.2f, 2.1f) == R, "a slight slant is still pure right (22.5 degree sectors)");
    check(a800session_stick_from_vector(1, 1, 0) == 0, "no radius, no direction");

    check(a800session_knob_from_vector(0, 0) == A800_PADDLE_CENTRE, "a knob dragged on its centre is the centre");
    check(a800session_knob_from_vector(0, -1) == A800_PADDLE_CENTRE, "straight up is the centre");
    check(a800session_knob_from_vector(1, -1) < A800_PADDLE_CENTRE, "clockwise is right: a lower value");
    check(a800session_knob_from_vector(-1, -1) > A800_PADDLE_CENTRE, "counter-clockwise is left: a higher value");
    check(a800session_knob_from_vector(1, 0) > 0 && a800session_knob_from_vector(1, 0) < A800_PADDLE_CENTRE, "90 degrees is two thirds of the way right");
    check(a800session_knob_from_vector(1, 1) == 0 && a800session_knob_from_vector(0, 1) == 0 && a800session_knob_from_vector(5, 5) == 0, "at and beyond the travel it holds its end (full right)");
    check(a800session_knob_from_vector(-1, 1) == A800_PADDLE_MAX && a800session_knob_from_vector(-5, 5) == A800_PADDLE_MAX, "and full left");
    v = a800session_knob_from_vector(0.7071f, -0.7071f);   /* 45 degrees right */
    a = a800session_knob_angle(v);
    check(fabsf(a - 45.0f) < 1.5f, "an angle round-trips through the knob's value");
    check(fabsf(a800session_knob_angle(A800_PADDLE_CENTRE)) < 0.01f && fabsf(a800session_knob_angle(0) - A800_KNOB_TRAVEL_DEG) < 0.01f
          && fabsf(a800session_knob_angle(A800_PADDLE_MAX) + A800_KNOB_TRAVEL_DEG) < 0.01f, "the centre and both stops");
    check(fabsf(a800session_vector_angle(0, -1)) < 0.01f && fabsf(a800session_vector_angle(1, 0) - 90.f) < 0.01f
          && fabsf(fabsf(a800session_vector_angle(0, 1)) - 180.f) < 0.01f, "the pointer's angle is from straight up, clockwise");
}

int main(void)
{
    every_device();
    views();
    hit();
    drags();
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
