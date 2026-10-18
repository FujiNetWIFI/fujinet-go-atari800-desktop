/*
 * The Controllers panel (F9): the on-screen Atari keyboard, the console
 * keys, the four joystick ports and the gamepads with their assignments and
 * bindings. Hidden at start; a fixed-size floating window.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <adwaita.h>

#include "a800session.h"

G_BEGIN_DECLS

/* Shows the panel, or hides it when it is showing. parent is the
 * A800Window (keys typed with the panel active go through
 * a800_window_key). */
void a800_controllers_toggle(GtkWindow *parent, a800session *session);
gboolean a800_controllers_visible(void);
/* The machine or a port's device changed: rebuild the keyboard and ports. */
void a800_controllers_refresh(void);

G_END_DECLS
