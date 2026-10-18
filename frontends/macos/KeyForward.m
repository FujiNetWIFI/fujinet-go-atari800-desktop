/*
 * KeyForward -- see the header.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "KeyForward.h"

#import <Carbon/Carbon.h>   /* kVK_* virtual key codes */

uint32_t A800KeysymFromEvent(NSEvent *event)
{
    const uint32_t ks = a800session_keysym_from_macos_keycode([event keyCode]);
    if (ks) return ks;
    if ([event keyCode] == kVK_Space) return ' ';

    /* Anything else: the character the key makes without modifiers, a
     * Latin-1 keysym as it stands. */
    NSString *chars = [event charactersIgnoringModifiers];
    if ([chars length] == 1) {
        const unichar c = [chars characterAtIndex:0];
        if (c >= 'A' && c <= 'Z') return (uint32_t)(c - 'A' + 'a');
        if (c >= 0x20 && c <= 0x7e) return (uint32_t)c;
    }
    return 0;
}

/* The app's own keys: F9, F11, F12, Shift+F5. */
static BOOL appKey(NSEvent *event)
{
    const unsigned short k = [event keyCode];
    if (k == kVK_F9 || k == kVK_F11 || k == kVK_F12) return YES;
    return k == kVK_F5 && ([event modifierFlags] & NSEventModifierFlagShift);
}

BOOL A800ForwardKey(a800session *session, NSEvent *event, BOOL down)
{
    const NSEventModifierFlags f = [event modifierFlags];
    if ([event keyCode] == kVK_F13) {
        /* no Pause key on a Mac keyboard: F13 is BREAK */
        a800session_console(session, A800_CON_BREAK, down);
        return YES;
    }
    const uint32_t ks = A800KeysymFromEvent(event);
    if (!down) {
        /* always: a key let go while a shortcut was held must not stick */
        if (ks) a800session_key(session, ks, 0, 0);
        return ks != 0 && !appKey(event);
    }
    /* Command shortcuts are the menus' */
    if ((f & NSEventModifierFlagCommand) || appKey(event) || !ks) return NO;
    if ([event isARepeat]) return YES;
    uint32_t unicode = 0;
    NSString *chars = [event charactersIgnoringModifiers];
    if ([chars length] == 1) {
        const unichar c = [chars characterAtIndex:0];
        if (c >= 0x20 && c < 0xf700) unicode = c;   /* not AppKit's function-key codes */
    }
    return a800session_key(session, ks, unicode, 1) != 0;
}

void A800ForwardFlags(a800session *session, NSEvent *event)
{
    const NSEventModifierFlags f = [event modifierFlags];
    switch ([event keyCode]) {
    case kVK_Shift:
        a800session_key(session, A800_KEYSYM_LSHIFT, 0, (f & NSEventModifierFlagShift) != 0);
        break;
    case kVK_RightShift:
        a800session_key(session, A800_KEYSYM_RSHIFT, 0, (f & NSEventModifierFlagShift) != 0);
        break;
    case kVK_Control:
        a800session_key(session, A800_KEYSYM_LCTRL, 0, (f & NSEventModifierFlagControl) != 0);
        break;
    case kVK_RightControl:
        a800session_key(session, A800_KEYSYM_RCTRL, 0, (f & NSEventModifierFlagControl) != 0);
        break;
    default:
        break;
    }
}

NSColor *A800AccentColor(void)
{
    return [NSColor colorWithSRGBRed:((A800SESSION_ACCENT_RGB >> 16) & 0xff) / 255.0
                               green:((A800SESSION_ACCENT_RGB >> 8) & 0xff) / 255.0
                                blue:(A800SESSION_ACCENT_RGB & 0xff) / 255.0
                               alpha:1.0];
}

NSColor *A800AccentTextColor(NSAppearance *appearance)
{
    NSAppearanceName best = [(appearance ? appearance : [NSApp effectiveAppearance])
        bestMatchFromAppearancesWithNames:@[ NSAppearanceNameAqua, NSAppearanceNameDarkAqua ]];
    if ([best isEqualToString:NSAppearanceNameDarkAqua])
        /* the rust lightened to #C8A27A (7:1 on a dark window) */
        return [NSColor colorWithSRGBRed:0xc8 / 255.0 green:0xa2 / 255.0 blue:0x7a / 255.0 alpha:1.0];
    return A800AccentColor();
}
