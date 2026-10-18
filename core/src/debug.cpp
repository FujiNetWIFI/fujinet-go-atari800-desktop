/*
 * debug.cpp -- the debugger engine (a800debug.h) over atari800's monitor.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "session_impl.h"

void session_debug_stopped(struct a800session* s)
{
	(void)s;
	Impl(s)->host.Resume();
}

void session_debug_pause(struct a800session* s) { (void)s; }
void session_debug_free(struct a800session* s) { (void)s; }
