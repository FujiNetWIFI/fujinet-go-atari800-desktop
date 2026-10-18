/*
 * The Atari's picture: a GtkWidget that pulls frames from the session on
 * the compositor's frame clock.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <adwaita.h>

#include "a800session.h"

#define A800_TYPE_DISPLAY (a800_display_get_type())
G_DECLARE_FINAL_TYPE(A800Display, a800_display, A800, DISPLAY, GtkWidget)

GtkWidget *a800_display_new(a800session *session);
/* The TV's shape (the frame's pixel aspect applied), or square pixels. */
void a800_display_set_tv_aspect(A800Display *self, gboolean tv);
void a800_display_set_smooth(A800Display *self, gboolean smooth);
/* Saves the last frame as a PNG; FALSE on failure. */
gboolean a800_display_save_png(A800Display *self, const char *path);
/* Frames shown since it was made (A800_AUTOQUIT_FRAMES). */
guint64 a800_display_frames(A800Display *self);
