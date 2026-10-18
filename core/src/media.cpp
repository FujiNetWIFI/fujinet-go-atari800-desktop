/*
 * media.cpp -- disks, cartridges, tapes, programs and states.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "session_impl.h"

int a800session_reboot_to_config(a800session* s)
{
	return a800session_power_cycle(s);
}
