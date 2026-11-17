/*
 * The Controllers window (F9): what plugs into the Atari's controller ports,
 * drawn as a player knows it -- the CX40 joystick, the CX30 paddle pair, the
 * mice, the Trak-Ball, the Koala Pad, the touch tablet, the light pen and gun or
 * the CX85 keypad on each port, following what is plugged in -- over the console
 * keys, with the machine's own keyboard under them, the gamepads and the port each
 * plays, and Map mode for rebinding the joystick and the console keys. Hidden at
 * start; a fixed-size floating window.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QPointF>
#include <QWidget>

#include "a800session.h"

namespace a800ctl {
/* The drawing's metrics, shared with the offscreen test: pixels per layout unit
 * (the layout is 30.6 units wide, and 11.4 (two ports) or 21.3 (four) tall), the
 * margin round it, and the keyboard's pixels per key unit and the margin of its
 * case. */
constexpr double kUnit = 24.0;
constexpr int kMargin = 12;
constexpr int kStageW = 759;               /* ceil(30.6 * kUnit) + 2 * kMargin */
constexpr int kKeyUnit = 45;               /* the keyboard: 16 x 6 key units */
constexpr int kCaseMargin = 14;

/* where the layout's origin sits in the stage */
inline QPointF origin() { return QPointF(kMargin, kMargin); }
} // namespace a800ctl

/* Shows the window, or hides it when it is showing. */
void a800ControllersToggle(QWidget *parent, a800session *session);
/* Shows (and raises) the window; leaves it showing if it already is. */
void a800ControllersShow(QWidget *parent, a800session *session);
bool a800ControllersVisible();
/* The machine or a port's device changed: rebuild the ports and the keyboard. */
void a800ControllersRefresh();
