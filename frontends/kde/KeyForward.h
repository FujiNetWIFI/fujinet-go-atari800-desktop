/*
 * Qt key events to the Atari.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QKeyEvent>

#include "a800session.h"

/* The physical key (from the native scan code: evdev + 8 on Linux) as the
 * session names it, or 0. */
uint32_t a800KeysymFromQt(const QKeyEvent *e);

/* Sends a press (down) or release to the Atari: true when it took it. The
 * app's own keys -- F9, F11, F12, Shift+F5, CTRL+SHIFT and ALT shortcuts --
 * are not sent (they are application-wide QActions). Pause/Break is the
 * Atari's BREAK. Auto-repeats are swallowed: the Atari repeats on its own.
 * Every window that takes typing (the main window, the Controllers panel)
 * forwards its key presses and releases through this. */
bool a800ForwardKey(a800session *session, const QKeyEvent *e, bool down);

/* A key bound to a system action (Power Cycle, Reboot to CONFIG, Pause,
 * Warp): its a800_sysaction, else -1. */
int a800KeySysaction(a800session *session, const QKeyEvent *e);
