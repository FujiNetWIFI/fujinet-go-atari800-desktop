/*
 * fujinet_runtime.c -- the in-process FujiNet (libfujinet).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "session_internal.h"

int session_fujinet_available(struct a800session *s)
{
    (void)s;
    return 0;
}

int session_fujinet_start(struct a800session *s)
{
    (void)s;
    return -1;
}

void session_fujinet_stop(struct a800session *s) { (void)s; }
void session_fujinet_restart_for_power(struct a800session *s) { (void)s; }
void session_fujinet_power_cycled(struct a800session *s) { (void)s; }

int a800session_fujinet_running(const a800session *s)
{
    return s->fujinet_running;
}

int a800session_fujinet_link(a800session *s)
{
    (void)s;
    return 0;
}

const char *a800session_fujinet_webui_url(const a800session *s)
{
    return s->webui_url;
}
