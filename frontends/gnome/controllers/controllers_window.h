/*
 * The Controllers window (F9): the Atari's controller ports drawn as a player
 * sees them (what is plugged into each port: the CX40 joystick, CX30 paddles,
 * a mouse, the Trak-Ball, the Koala Pad, the touch tablet, the light pen or
 * gun, the CX85 keypad), the console keys, and the machine's keyboard, with
 * the gamepads' port assignments and Map mode. Hidden at start; a fixed-size
 * floating window.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <adwaita.h>

#include "a800session.h"

G_BEGIN_DECLS

/* Shows the window, or hides it when it is showing (F9). parent is the
 * A800Window (keys typed with this window active go through
 * a800_window_key). */
void a800_controllers_toggle(GtkWindow *parent, a800session *session);
/* Shows (and raises) the window; never hides it. */
void a800_controllers_show(GtkWindow *parent, a800session *session);
gboolean a800_controllers_visible(void);
/* The machine or a port's device changed: rebuild the layout and keyboard. */
void a800_controllers_refresh(void);

G_END_DECLS
