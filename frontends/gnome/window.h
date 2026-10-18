/*
 * The main application window.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <adwaita.h>

#include "a800session.h"

#define A800_TYPE_WINDOW (a800_window_get_type())
G_DECLARE_FINAL_TYPE(A800Window, a800_window, A800, WINDOW, AdwApplicationWindow)

GtkWidget *a800_window_new(AdwApplication *app, a800session *session);
/* main.c: the icon name to use (the installed id, or the in-tree art). */
const char *a800_icon_name(void);
void a800_window_toast(A800Window *self, const char *text);

/* Starts the machine (FujiNet first). */
void a800_window_start(A800Window *self);

/* A key pressed or released with this window, or one of its panels, active:
 * the window's own keys (F9, F11, F12, Shift+F5, the CTRL+SHIFT shortcuts),
 * else the Atari's. keycode is GDK's hardware keycode (evdev + 8), which
 * keeps press and release paired whatever the modifiers did in between.
 * TRUE when it was used. */
gboolean a800_window_key(A800Window *self, guint keyval, guint keycode,
                         GdkModifierType state, gboolean down);

/* A dropped or command-line file: Open's routing (a disk boots, a program
 * runs, a cartridge goes in, a ROM is imported ...). */
void a800_window_load_media(A800Window *self, const char *path);

/* The view settings (aspect, smooth, 80-column display) from the store. */
void a800_window_apply_view(A800Window *self);

/* After Preferences: the menus and the status follow what changed. */
void a800_window_settings_changed(A800Window *self);

/* The accent colour (A800SESSION_ACCENT_RGB) as CSS classes every window
 * can use: ".a800-accent" fills a widget's background with it (white text),
 * ".a800-accent-text" colours a label (light themes only: on dark themes it
 * falls back to the theme's accent), ".a800-dot" with "-on"/"-off" a status
 * light, ".a800-held" a pressed on-screen key. Installed by main.c. */
void a800_install_accent_css(void);
