/*
 * The application delegate: the window, the menu bar, and the session.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#import <Cocoa/Cocoa.h>

#include "a800session.h"

@interface A800AppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate, NSMenuDelegate>
/* mediaPath: a file named on the command line (a disk, a program, a
 * cartridge, a ROM ...), or NULL. */
- (instancetype)initWithSession:(a800session *)session mediaPath:(const char *)mediaPath;
/* The Controllers panel (F9), the debugger (F12) and fullscreen (F11): the
 * keys any of the app's windows passes on. */
- (void)toggleControllers:(id)sender;
- (void)toggleDebugger:(id)sender;
- (void)toggleFullScreen:(id)sender;
- (void)powerCycle:(id)sender;
@end
