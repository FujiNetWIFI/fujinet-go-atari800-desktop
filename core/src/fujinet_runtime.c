/*
 * fujinet_runtime.c -- the in-process FujiNet (libfujinet, the ATARI target).
 *
 * atari800 is the NetSIO hub (bound by the session before FujiNet starts);
 * FujiNet connects to it on A800SESSION_NETSIO_PORT. FujiNet ignores the
 * Atari's COLD_RESET (patched): the host owns its power, restarting it on
 * Power Cycle. A watchdog restarts a runtime that rebooted itself (CONFIG's
 * reset, the web UI's restart) and leaves one stopped that was asked to
 * exit. SAM speech comes back through read_audio and is mixed by the audio
 * callback (or drained when there is no audio device).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "compat.h"
#include "dynlib.h"
#include "session_internal.h"

typedef int (*start_runtime_fn)(const char *root, const char *config, const char *sd,
                                const char *data, int listen_port);
typedef void (*stop_runtime_fn)(void);
typedef const char *(*last_error_fn)(void);
typedef int (*copy_log_fn)(char *out, int max_bytes);
typedef int (*int_fn)(void);
typedef int (*read_audio_fn)(int16_t *out, int max_samples, int rate);

/* One runtime per process: the library owns global state and threads. */
static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
static a800_dynlib g_handle;
static start_runtime_fn g_start;
static stop_runtime_fn g_stop;
static last_error_fn g_last_error;
static copy_log_fn g_copy_log;
static int_fn g_is_running, g_exit_code;
static read_audio_fn g_read_audio;

/* the watchdog */
static pthread_t g_watch;
static int g_watch_started;
static volatile int g_watch_quit;
static struct a800session *g_session;
static unsigned g_generation;
static char g_event[160];
static time_t g_restart_times[3];

static void set_event(const char *fmt, const char *arg)
{
    snprintf(g_event, sizeof g_event, fmt, arg ? arg : "");
    g_generation++;
}

static int load_library_locked(struct a800session *s)
{
    char errbuf[256];
    if (g_handle)
        return 0;
    g_handle = a800_dynlib_open(s->fujinet_lib);
    if (!g_handle) {
        session_set_error(s, "FujiNet library load failed: %s",
                          a800_dynlib_error(errbuf, sizeof errbuf));
        return -1;
    }
    g_start = (start_runtime_fn)a800_dynlib_sym(g_handle, "fujinet_desktop_start_runtime");
    g_stop = (stop_runtime_fn)a800_dynlib_sym(g_handle, "fujinet_desktop_stop_runtime");
    g_last_error = (last_error_fn)a800_dynlib_sym(g_handle, "fujinet_desktop_last_error_message");
    g_copy_log = (copy_log_fn)a800_dynlib_sym(g_handle, "fujinet_desktop_copy_recent_log");
    g_is_running = (int_fn)a800_dynlib_sym(g_handle, "fujinet_desktop_is_running");
    g_exit_code = (int_fn)a800_dynlib_sym(g_handle, "fujinet_desktop_exit_code");
    g_read_audio = (read_audio_fn)a800_dynlib_sym(g_handle, "fujinet_desktop_read_audio");
    if (!g_start || !g_stop || !g_last_error || !g_is_running || !g_exit_code) {
        session_set_error(s, "%s is missing the desktop runtime contract", s->fujinet_lib);
        g_start = NULL;
        return -1;
    }
    return 0;
}

int session_fujinet_available(struct a800session *s)
{
    if (s->fujinet_disabled)
        return 0;
    return paths_provision_fujinet(s) == 0;
}

/* The [BOIP] section, every start: the web UI could have changed it. */
static void assert_boip_config(struct a800session *s)
{
    FILE *fp = fopen(s->fujinet_config, "rb");
    char *text = NULL, *out;
    long n = 0;
    const char *section = "[BOIP]\nenabled=1\nhost=127.0.0.1\nport=11514\n";
    char *start, *end;
    if (fp) {
        fseek(fp, 0, SEEK_END);
        n = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        text = calloc(1, (size_t)n + 1);
        if (text && fread(text, 1, (size_t)n, fp) != (size_t)n)
            n = 0;
        fclose(fp);
    }
    if (!text)
        text = calloc(1, 1);
    out = malloc((size_t)n + strlen(section) + 4);
    if (!out) {
        free(text);
        return;
    }
    start = strstr(text, "[BOIP]");
    if (start) {
        end = strstr(start + 6, "\n[");
        end = end ? end + 1 : text + n;
        snprintf(out, (size_t)n + strlen(section) + 4, "%.*s%s%s", (int)(start - text), text,
                 section, end);
    }
    else {
        snprintf(out, (size_t)n + strlen(section) + 4, "%s%s%s", text,
                 n && text[n - 1] != '\n' ? "\n" : "", section);
    }
    fp = fopen(s->fujinet_config, "wb");
    if (fp) {
        fwrite(out, 1, strlen(out), fp);
        fclose(fp);
    }
    free(out);
    free(text);
}

static int start_locked(struct a800session *s)
{
    char bind[64];
    if (load_library_locked(s) != 0)
        return -1;
    assert_boip_config(s);
    snprintf(bind, sizeof bind, "127.0.0.1:%d", A800SESSION_WEBUI_PORT);
    a800_setenv("FUJINET_WEBUI_BIND", bind);
    /* the flash filesystem and the CA bundle are rooted here, not the CWD */
    a800_setenv("FUJINET_RUNTIME_ROOT", s->fujinet_root);
    if (!g_start(s->fujinet_root, s->fujinet_config, s->fujinet_sd, s->fujinet_data,
                 A800SESSION_NETSIO_PORT)) {
        const char *err = g_last_error ? g_last_error() : NULL;
        session_set_error(s, "FujiNet failed to start: %s", err && *err ? err : "(unknown)");
        return -1;
    }
    s->fujinet_running = 1;
    return 0;
}

static void *watch_main(void *arg)
{
    struct a800session *s = arg;
    int16_t drain[2048];
    while (!g_watch_quit) {
        a800_sleep_ms(50);
        pthread_mutex_lock(&g_mtx);
        if (s->fujinet_running && g_is_running && !g_is_running()) {
            int code = g_exit_code();
            s->fujinet_running = 0;
            session_fujinet_link_lost(s);
            if (code == 75) {
                time_t now = time(NULL);
                if (now - g_restart_times[0] < 30) {
                    set_event("FujiNet keeps restarting; it is stopped (FujiNet > Restart)", NULL);
                }
                else {
                    memmove(g_restart_times, g_restart_times + 1, sizeof g_restart_times[0] * 2);
                    g_restart_times[2] = now;
                    if (start_locked(s) == 0)
                        set_event("FujiNet restarted", NULL);
                    else
                        set_event("FujiNet could not restart: %.120s", s->last_error);
                }
            }
            else {
                set_event("FujiNet stopped", NULL);
            }
        }
        /* no audio device to mix SAM into: keep the queue moving */
        if (s->fujinet_running && !s->audio && g_read_audio)
            while (g_read_audio(drain, (int)(sizeof drain / sizeof drain[0]), A800SESSION_AUDIO_RATE) > 0)
                ;
        pthread_mutex_unlock(&g_mtx);
    }
    return NULL;
}

int session_fujinet_start(struct a800session *s)
{
    int rc;
    pthread_mutex_lock(&g_mtx);
    if (s->fujinet_running) {
        pthread_mutex_unlock(&g_mtx);
        return 0;
    }
    rc = start_locked(s);
    if (rc == 0 && !g_watch_started) {
        g_session = s;
        g_watch_quit = 0;
        g_watch_started = pthread_create(&g_watch, NULL, watch_main, s) == 0;
    }
    pthread_mutex_unlock(&g_mtx);
    if (rc != 0)
        fprintf(stderr, "a800: %s (continuing without FujiNet)\n", s->last_error);
    return rc;
}

void session_fujinet_stop(struct a800session *s)
{
    stop_runtime_fn stop = NULL;
    if (g_watch_started) {
        g_watch_quit = 1;
        pthread_join(g_watch, NULL);
        g_watch_started = 0;
    }
    pthread_mutex_lock(&g_mtx);
    if (s->fujinet_running) {
        stop = g_stop;
        s->fujinet_running = 0;
    }
    pthread_mutex_unlock(&g_mtx);
    if (stop)
        stop();
    session_fujinet_link_lost(s);
}

/* As unplugging both: FujiNet starts afresh with the Atari. */
void session_fujinet_restart_for_power(struct a800session *s)
{
    int t;
    pthread_mutex_lock(&g_mtx);
    if (s->fujinet_running) {
        g_stop();
        s->fujinet_running = 0;
    }
    session_fujinet_link_lost(s);
    start_locked(s);
    pthread_mutex_unlock(&g_mtx);
    for (t = 0; t < 4000 && !a800session_fujinet_link(s); t += 20)
        a800_sleep_ms(20);
}

void session_fujinet_power_cycled(struct a800session *s)
{
    (void)s;
}

int a800session_fujinet_restart(a800session *s)
{
    if (!s->fujinet_wanted)
        return -1;
    session_fujinet_restart_for_power(s);
    pthread_mutex_lock(&g_mtx);
    set_event("FujiNet restarted", NULL);
    pthread_mutex_unlock(&g_mtx);
    return s->fujinet_running ? 0 : -1;
}

/* Waits for FujiNet to connect to the hub. */
int session_fujinet_wait_link(struct a800session *s, int timeout_ms)
{
    int t;
    for (t = 0; t < timeout_ms; t += 20) {
        if (a800session_fujinet_link(s))
            return 1;
        a800_sleep_ms(20);
    }
    return 0;
}

void session_fujinet_mix(struct a800session *s, float *stereo, int frames, float gain)
{
    int16_t buf[1024];
    int got, i;
    if (!s->fujinet_running || !g_read_audio || frames <= 0)
        return;
    if (frames > 1024)
        frames = 1024;
    got = g_read_audio(buf, frames, A800SESSION_AUDIO_RATE);
    for (i = 0; i < got; i++) {
        float v = buf[i] * gain / 32768.0f;
        stereo[i * 2] += v;
        stereo[i * 2 + 1] += v;
    }
}

int a800session_fujinet_running(const a800session *s)
{
    return s->fujinet_running;
}

const char *a800session_fujinet_webui_url(const a800session *s)
{
    return s->webui_url;
}

int a800session_fujinet_copy_log(a800session *s, char *dst, int max)
{
    copy_log_fn fn;
    (void)s;
    if (!dst || max <= 0)
        return 0;
    dst[0] = '\0';
    pthread_mutex_lock(&g_mtx);
    fn = g_copy_log;
    pthread_mutex_unlock(&g_mtx);
    return fn ? fn(dst, max) : 0;
}

unsigned a800session_fujinet_generation(a800session *s)
{
    unsigned g;
    (void)s;
    pthread_mutex_lock(&g_mtx);
    g = g_generation;
    pthread_mutex_unlock(&g_mtx);
    return g;
}

int a800session_fujinet_last_event(a800session *s, char *dst, int dstsz)
{
    int n;
    (void)s;
    pthread_mutex_lock(&g_mtx);
    n = snprintf(dst, (size_t)dstsz, "%s", g_event);
    pthread_mutex_unlock(&g_mtx);
    return n;
}
