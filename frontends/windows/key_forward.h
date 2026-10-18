/*
 * Win32 key messages to X11/xkb keysyms, by hardware scan code through the
 * session's HID table -- the same physical-key route the other frontends
 * take, so a press and its release pair up whatever Shift did in between,
 * and a binding names the key whatever the layout is doing.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <stdint.h>
#include <windows.h>

#include "a800session.h"

static inline uint32_t a800_keysym_from_msg(WPARAM vk, LPARAM lp)
{
    const unsigned scancode = (unsigned)((lp >> 16) & 0xff);
    const int extended = (lp >> 24) & 1;
    uint32_t ks = a800session_keysym_from_win_scancode(scancode, extended);
    if (ks) return ks;
    /* Fallbacks for keys the scan-code table has no usage for. */
    switch (vk) {
    case VK_UP: return A800_KEYSYM_UP;
    case VK_DOWN: return A800_KEYSYM_DOWN;
    case VK_LEFT: return A800_KEYSYM_LEFT;
    case VK_RIGHT: return A800_KEYSYM_RIGHT;
    case VK_ESCAPE: return A800_KEYSYM_ESCAPE;
    case VK_RETURN: return A800_KEYSYM_RETURN;
    case VK_SPACE: return A800_KEYSYM_SPACE;
    case VK_TAB: return A800_KEYSYM_TAB;
    case VK_BACK: return A800_KEYSYM_BACKSPACE;
    case VK_SHIFT: return A800_KEYSYM_LSHIFT;
    case VK_CONTROL: return A800_KEYSYM_LCTRL;
    default: break;
    }
    if (vk >= VK_F1 && vk <= VK_F12) return A800_KEYSYM_F1 + (uint32_t)(vk - VK_F1);
    if (vk >= '0' && vk <= '9') return (uint32_t)vk;
    if (vk >= 'A' && vk <= 'Z') return (uint32_t)(vk - 'A' + 'a');
    return 0;
}

/* The character a key types with the keyboard as it is, for the session's
 * symbolic mode; 0 if none. */
static inline uint32_t a800_unicode_from_msg(WPARAM vk, LPARAM lp)
{
    BYTE state[256];
    WCHAR out[4];
    int n;
    if (!GetKeyboardState(state))
        return 0;
    /* CONTROL changes what ToUnicode makes; the session wants the
     * character the key types without it */
    state[VK_CONTROL] = state[VK_LCONTROL] = state[VK_RCONTROL] = 0;
    n = ToUnicode((UINT)vk, (UINT)((lp >> 16) & 0xff), state, out, 4, 4 /* no state change */);
    return n == 1 && out[0] >= 0x20 ? (uint32_t)out[0] : 0;
}

/* The app's own keys, never the Atari's: F9, F11, F12, Shift+F5, CTRL+SHIFT
 * and ALT shortcuts, CTRL+, (Settings). */
static inline int a800_app_key(WPARAM vk)
{
    const int ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const int shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    if (vk == VK_F9 || vk == VK_F11 || vk == VK_F12) return 1;
    if (vk == VK_F5 && shift) return 1;
    if (GetKeyState(VK_MENU) & 0x8000) return 1;
    if (ctrl && shift && vk != VK_SHIFT && vk != VK_CONTROL) return 1;
    return ctrl && vk == VK_OEM_COMMA;
}

/* A key press or release for the Atari from any of the app's windows (the
 * main window, the Controllers panel): 1 when it took it. Pause is BREAK;
 * auto-repeats are swallowed (the Atari repeats on its own). */
static inline int a800_forward_key(a800session *s, UINT msg, WPARAM vk, LPARAM lp)
{
    const int down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
    uint32_t ks;
    if (vk == VK_PAUSE || vk == VK_CANCEL) {
        a800session_console(s, A800_CON_BREAK, down);
        return 1;
    }
    ks = a800_keysym_from_msg(vk, lp);
    if (!down) {
        /* always: a key let go while a shortcut was held must not stick */
        if (ks) a800session_key(s, ks, 0, 0);
        return ks != 0 && !a800_app_key(vk);
    }
    if (a800_app_key(vk) || !ks) return 0;
    if (lp & (1 << 30)) return 1;   /* auto-repeat */
    return a800session_key(s, ks, a800_unicode_from_msg(vk, lp), 1) != 0;
}
