/*
 * The Win32 Controllers panel (F9): the on-screen Atari keyboard, the
 * console keys, the joystick ports and the gamepads with their assignments
 * and bindings, in a fixed-size tool window. Hidden at start.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <windows.h>

#include "a800session.h"

/* Show / hide (F9). Created on first use, hidden not destroyed after. */
void a800_controller_window_toggle(HWND parent, a800session *session);
int  a800_controller_window_visible(void);

/* The gamepad set, the machine or a port's device changed: refresh. */
void a800_controller_window_refresh(void);

/* Give the window first refusal on a message from the main loop. Returns 1
 * if consumed. */
int a800_controller_pretranslate(MSG *msg);
