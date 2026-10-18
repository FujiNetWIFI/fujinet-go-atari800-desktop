/*
 * The debugger window (F12) over the debugger engine (a800debug.h).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#import <Cocoa/Cocoa.h>

#include "a800session.h"

@interface A800DebuggerWindow : NSObject
- (instancetype)initWithSession:(a800session *)session;
/* Show (attaching stops the machine), or hide (detaching lets it run). */
- (void)show;
- (void)toggle;
- (BOOL)isVisible;
@end
