/*
 * DisplayGlue.h -- what DisplayView.m needs to know about THIS machine: the
 * session's functions, the frame size and the shape of the picture.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <stdatomic.h>

#include "a800session.h"

#define FN_VIEW      A800DisplayView
#define FN_SESSION   a800session
#define FN_FB_W      A800SESSION_FB_MAX_WIDTH
#define FN_FB_H      A800SESSION_FB_MAX_HEIGHT
#define FN_INITIAL_W 864
#define FN_INITIAL_H 720

/* The frames vary in width (the 80-column cards, the NTSC filter) and carry
 * their own pixel aspect. */
#define FN_VARIABLE_WIDTH 1

/* The pointer (mice, Koala Pad, light pen and gun) and the screenshot hooks
 * (DisplayExtras.inc). */
#define FN_EXTRA_IVARS \
    NSPoint _last; \
    BOOL _haveLast; \
    NSTrackingArea *_tracking;
#define FN_EXTRA_METHODS "DisplayExtras.inc"

/* The session reports the rate x1000 (59923 or 49860). */
static inline int fn_machine_hz(FN_SESSION *s) { return (a800session_refresh_rate(s) + 500) / 1000; }

/* The host stamps its own clock; the argument is not used. */
static inline void fn_notify_vsync(FN_SESSION *s, int64_t ns)
{
    a800session_notify_vsync(s, ns);
}

/* The last frame's pixel aspect x1000 (1000 square, 500 half-width pixels).
 * Written by the link's thread, read by whoever draws or hit-tests. */
static _Atomic int s_fn_par = 1000;

static inline int fn_pull_frame_w(FN_SESSION *s, uint32_t *dst, int *width, int *lines, uint64_t *serial)
{
    int par = 1000;
    const int got = a800session_copy_frame(s, dst, width, lines, &par, serial);
    if (got && *width > 0 && *lines > 0) {
        atomic_store(&s_fn_par, par > 0 ? par : 1000);
        return 1;
    }
    return 0;
}

static inline double fn_picture_aspect_w(FN_SESSION *s, int width, int lines, BOOL tv)
{
    const int par = atomic_load(&s_fn_par);
    const double w = width * par / 1000.0, h = lines;
    if (!tv) return w / h;
    if (width >= 560 && par == 500) return 4.0 / 3.0;   /* an 80-column card's monitor */
    /* an Atari pixel on an NTSC TV is 6/7 as wide as tall; on PAL, 1.03 */
    return w * (a800session_refresh_rate(s) > 55000 ? 6.0 / 7.0 : 1.03) / h;
}
