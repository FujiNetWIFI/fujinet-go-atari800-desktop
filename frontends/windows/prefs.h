/*
 * The Win32 Settings window, built from the session's preference table
 * (a800session_pref) like the other frontends' Preferences.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <windows.h>

#include "a800session.h"

/* Shows the window (raising it when open). Live rows apply at once and
 * call on_live (the view follows); machine and host rows apply together
 * when it closes, which calls on_closed with the a800_apply it needed. */
void a800_prefs_show(HWND parent, a800session *session, void (*on_live)(void),
                     void (*on_closed)(int applied));
