/*
 * The debugger window (F12).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QWidget>

#include "a800session.h"

/* Shows the debugger (attaching it stops the machine), or hides it
 * (detaching lets it run). */
void a800DebuggerToggle(QWidget *parent, a800session *session);
void a800DebuggerShow(QWidget *parent, a800session *session);
