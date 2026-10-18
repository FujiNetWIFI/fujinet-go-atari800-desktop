/*
 * The Controllers panel (F9): the on-screen Atari keyboard, the console
 * keys, the joystick ports and the gamepads with their assignments and
 * bindings. Hidden at start; a fixed-size floating NSPanel.
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
- (BOOL)isVisible;
/* The machine, a port's device or the gamepads changed: rebuild. */
- (void)refresh;
@end
