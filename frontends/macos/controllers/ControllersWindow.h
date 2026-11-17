/*
 * The Controllers panel (F9 / Cmd+K / View menu): what is plugged into each
 * controller port (the CX40 joystick, the CX30 paddles, the mice, the Trak-Ball,
 * the Koala Pad, the touch tablet, the light pen and gun, the CX85 keypad), the
 * console keys, an optional Atari keyboard drawn for the machine, the gamepads
 * with the port each one plays, and Map mode for rebinding. Hidden at start; a
 * fixed-size floating NSPanel.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#import <Cocoa/Cocoa.h>

#include "a800session.h"

@interface A800ControllersWindow : NSObject
- (instancetype)initWithSession:(a800session *)session;
/* Show, or hide when showing. */
- (void)toggle;
/* Show (or bring to the front). */
- (void)show;
- (void)hide;
- (BOOL)isVisible;
/* The machine, a port's device, the bindings or the gamepads changed: read again. */
- (void)refresh;
@end
