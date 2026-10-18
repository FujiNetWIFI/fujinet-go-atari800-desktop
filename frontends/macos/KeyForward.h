/*
 * AppKit key events to the Atari: the hardware key code through the
 * session's table (so a press and its release pair up, and positional mode
 * has the physical key), with the character the key typed for symbolic
 * mode -- the route the other frontends take.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#import <Cocoa/Cocoa.h>
#include <stdint.h>

#include "a800session.h"

/* The keysym for a key event (the session's X11-style keysym), or 0. */
uint32_t A800KeysymFromEvent(NSEvent *event);

/* A keyDown/keyUp for the Atari: YES when it took it. Command shortcuts
 * are the menus'; F9, F11 and F12 and Shift+F5 are the app's (the caller
 * handles them first). Pause/F13 is BREAK. Auto-repeats are swallowed (the
 * Atari repeats on its own). */
BOOL A800ForwardKey(a800session *session, NSEvent *event, BOOL down);

/* flagsChanged: Shift and Control as the Atari's SHIFT and CONTROL keys. */
void A800ForwardFlags(a800session *session, NSEvent *event);

/* This app's accent (A800SESSION_ACCENT_RGB, the 800's rust): a fill, with
 * white text on it. As a text colour on a dark appearance it is too dark:
 * A800AccentTextColor() gives a lightened rust there. */
NSColor *A800AccentColor(void);
NSColor *A800AccentTextColor(NSAppearance *appearance);
