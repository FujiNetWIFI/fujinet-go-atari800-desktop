/*
 * The Controllers window's held colour. Parts of the drawing that something
 * holds down (a joystick direction, a console key, a button) light in this
 * orange, which reads on black plastic; the app's own accent
 * (A800SESSION_ACCENT_RGB, a brown) would vanish there (DESIGN-atari.md
 * section 1). Buttons in the accent state -- Map while armed -- keep the app
 * accent with white text.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QColor>

/* a held part on black plastic */
#define A800_CTL_HELD_RGB 0xFFB04Au
/* the dark ink that reads on it */
#define A800_CTL_HELD_INK_RGB 0x2A1604u

inline QColor a800CtlHeld()
{
    return QColor((A800_CTL_HELD_RGB >> 16) & 0xff, (A800_CTL_HELD_RGB >> 8) & 0xff, A800_CTL_HELD_RGB & 0xff);
}

inline QColor a800CtlHeldInk()
{
    return QColor((A800_CTL_HELD_INK_RGB >> 16) & 0xff, (A800_CTL_HELD_INK_RGB >> 8) & 0xff,
                  A800_CTL_HELD_INK_RGB & 0xff);
}
