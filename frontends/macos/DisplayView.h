/*
 * The Atari's picture.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#import <Cocoa/Cocoa.h>

#include "a800session.h"

@interface A800DisplayView : NSView
- (instancetype)initWithSession:(a800session *)session;
/* YES: a TV's shape (the frame's pixel aspect applied); NO: square pixels. */
- (void)setTvAspect:(BOOL)tv;
- (void)setSmooth:(BOOL)smooth;
/* The last frame as a PNG. */
- (BOOL)savePng:(NSString *)path;
/* Frames shown since it was made (A800_AUTOQUIT_FRAMES). */
- (uint64_t)frames;
- (void)stop;
@end
