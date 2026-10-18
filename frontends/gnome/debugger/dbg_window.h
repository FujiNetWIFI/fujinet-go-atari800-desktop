/*
 * The debugger window (F12).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <adwaita.h>

#include "a800session.h"

G_BEGIN_DECLS

/* Shows the debugger (attaching it stops the machine), or hides it
 * (detaching lets it run). */
void a800_debugger_toggle(GtkWindow *parent, a800session *session);
void a800_debugger_show(GtkWindow *parent, a800session *session);

G_END_DECLS
