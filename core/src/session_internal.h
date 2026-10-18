/*
 * The session's private state, shared by the core's C and C++ files.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef A800_SESSION_INTERNAL_H
#define A800_SESSION_INTERNAL_H

#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>

#include "a800session.h"

#ifdef __cplusplus
extern "C" {
#endif

#define A800_PATH_MAX 1024

typedef struct setting_kv {
    char *key;
    char *val;
    struct setting_kv *next;
} setting_kv;

/* ROM slots (roms.c) */
enum {
    ROM_OS_A_NTSC = 0, ROM_OS_A_PAL, ROM_OS_B_NTSC, ROM_OS_AA00R10, ROM_OS_AA01R11,
    ROM_OS_BB00R1, ROM_OS_BB01R2, ROM_OS_BB02R3, ROM_OS_BB02R3V4, ROM_OS_CC01R4,
    ROM_OS_BB01R3, ROM_OS_BB01R4, ROM_OS_BB01R59, ROM_OS_BB01R59A,
    ROM_BASIC_A, ROM_BASIC_B, ROM_BASIC_C, ROM_XEGAME,
    ROM_OS800_CUSTOM, ROM_OSXL_CUSTOM, ROM_BASIC_CUSTOM, ROM_XEGAME_CUSTOM,
    ROM_XLD_VOICE, ROM_XLD_DISK, ROM_MIO, ROM_BLACK_BOX, ROM_PROTO80,
    ROM_AF80, ROM_AF80_CHARSET, ROM_BIT3, ROM_BIT3_CHARSET, ROM_XEP80_CHARSET,
    ROM_COUNT
};

struct a800session {
    char config_dir[A800_PATH_MAX];
    char data_dir[A800_PATH_MAX];
    char media_dir[A800_PATH_MAX];
    char roms_dir[A800_PATH_MAX];
    char states_dir[A800_PATH_MAX];
    char settings_file[A800_PATH_MAX];
    setting_kv *settings;
    pthread_mutex_t settings_mtx;
    int settings_dirty;
    char last_error[256];

    char fujinet_root[A800_PATH_MAX];       /* <data>/fujinet */
    char fujinet_config[A800_PATH_MAX];     /* .../fnconfig.ini */
    char fujinet_sd[A800_PATH_MAX];         /* .../SD */
    char fujinet_data[A800_PATH_MAX];       /* .../data */
    char fujinet_lib[A800_PATH_MAX];        /* resolved libfujinet, "" until then */
    char fujinet_runtime_src[A800_PATH_MAX];
    char webui_url[64];
    int  fujinet_running;
    int  fujinet_wanted;                    /* FujiNet enabled for this run */
    int  fujinet_disabled;                  /* the caller said no FujiNet ("" lib) */

    pthread_mutex_t sysact_mtx;
    unsigned sysact_pending;

    a800session_start_opts opts;
    int running;

    void *impl;             /* session.cpp's SessionImpl */
    void *audio;            /* audio_sdl.c, NULL until started */
    void *gamepad;          /* gamepad_sdl.c, NULL until started */
    void *debugger;         /* a800debug, lazily created */

    pthread_mutex_t pad_event_mtx;
    char pad_event[128];
};

void settings_init(struct a800session *s);
void settings_free_all(struct a800session *s);

int  paths_init(struct a800session *s, const char *config_dir, const char *data_dir);
int  paths_provision_fujinet(struct a800session *s);
int  paths_mkdir_p(const char *path);

void session_set_error(struct a800session *s, const char *fmt, ...);
void session_roms_changed(struct a800session *s);

/* prefs.c */
typedef void (*prefs_emit_fn)(void *u, const char *key, const char *val);
int  prefs_machine(struct a800session *s);
void prefs_emit_options(struct a800session *s, prefs_emit_fn emit, void *u);
void prefs_emit_live(struct a800session *s, prefs_emit_fn emit, void *u);
int  prefs_build_argv(struct a800session *s, char **argv, int max);

/* roms.c */
const char *roms_config_key(int slot);
const char *roms_path(struct a800session *s, int slot);
uint32_t roms_crc32(const uint8_t *p, size_t n);
int  roms_identify_file(const char *path);
void roms_provision_embedded(struct a800session *s);

/* fujinet_runtime.c, and the NetSIO link (session.cpp) */
int  session_fujinet_available(struct a800session *s);
int  session_fujinet_start(struct a800session *s);
void session_fujinet_stop(struct a800session *s);
void session_fujinet_restart_for_power(struct a800session *s);
void session_fujinet_power_cycled(struct a800session *s);
int  session_fujinet_wait_link(struct a800session *s, int timeout_ms);
void session_fujinet_mix(struct a800session *s, float *stereo, int frames, float gain);
void session_fujinet_link_lost(struct a800session *s);

/* audio_sdl.c, gamepad_sdl.c */
int  audio_start(struct a800session *s);
void audio_stop(struct a800session *s);
int  gamepad_start(struct a800session *s);
void gamepad_stop(struct a800session *s);
void session_gamepad_apply(struct a800session *s, int port, int act, int down);
void session_gamepad_event(struct a800session *s, const char *text);
int  session_active_ports(struct a800session *s);

#ifdef __cplusplus
}
#endif

#endif
