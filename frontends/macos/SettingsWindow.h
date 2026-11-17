/*
 * The Settings window, built from the session's preference table
 * (a800session_pref) like the other frontends' Preferences.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#import <Cocoa/Cocoa.h>

#include "a800session.h"

@interface A800SettingsWindow : NSObject <NSWindowDelegate, NSTextFieldDelegate, NSTabViewDelegate>
/* onLive runs after each live change (the view follows); onClosed when the
 * window closes, with the a800_apply that applying the rest needed. */
- (instancetype)initWithSession:(a800session *)session
                         onLive:(void (^)(void))onLive
                       onClosed:(void (^)(int applied))onClosed;
- (void)show;
/* Runs from the Input page's Open Controllers button (the app shows the
 * Controllers window). */
@property (nonatomic, copy) void (^openControllers)(void);
/* Runs after the Input page changed a gamepad's port or put the default
 * controls back (the Controllers window reads them again). */
@property (nonatomic, copy) void (^controlsChanged)(void);
@end
