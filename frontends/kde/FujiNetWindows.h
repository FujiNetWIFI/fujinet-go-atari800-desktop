/*
 * The FujiNet console log window, the FujiNet web UI, and the accent.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QColor>
#include <QWidget>

#include "a800session.h"

/* Shows (raising an existing one) the FujiNet console log window. */
void fujinet_log_show(QWidget *parent, a800session *session);
/* Opens the FujiNet web UI in the system browser. Not embedded: the web UI's
 * Google and OneDrive Authorize buttons open the provider's consent page in a
 * new tab, and both providers refuse OAuth from an embedded web view. */
void fujinet_config_show(QWidget *parent, a800session *session);

/* The accent (A800SESSION_ACCENT_RGB, the 800's rust): a fill colour, with
 * white text on it. As a text colour on a dark palette it is too dark:
 * a800AccentTextColor() gives a lightened rust there. */
QColor a800AccentColor();
QColor a800AccentTextColor(const QWidget *w);
