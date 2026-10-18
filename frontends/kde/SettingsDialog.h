/*
 * The Preferences dialog, built from the session's preference table.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QDialog>
#include <functional>

#include "a800session.h"

namespace SettingsDialog {
/* Runs the dialog. Live rows apply at once (onLive runs after each, for the
 * view); machine and host rows apply together when it closes. Returns the
 * a800_apply that closing needed. */
int run(QWidget *parent, a800session *session, const std::function<void()> &onLive);
}
