/*
 * The Controllers panel (F9): the on-screen Atari keyboard, the console
 * keys, the four joystick ports and the gamepads with their assignments and
 * bindings. Hidden at start; a fixed-size floating window.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QWidget>

#include "a800session.h"

/* Shows the panel, or hides it when it is showing. */
void a800ControllersToggle(QWidget *parent, a800session *session);
bool a800ControllersVisible();
/* The machine or a port's device changed: rebuild the keyboard and ports. */
void a800ControllersRefresh();
