/*
 * The Preferences dialog.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <adwaita.h>

#include "a800session.h"
#include "window.h"

G_BEGIN_DECLS

/* Shows the Preferences (built from a800session_pref). Live rows apply at
 * once; machine and host rows apply together when it closes. */
void a800_prefs_show(A800Window *parent, a800session *session);

G_END_DECLS
