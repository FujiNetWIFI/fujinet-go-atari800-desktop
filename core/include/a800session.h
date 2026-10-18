/*
 * a800session -- the toolkit-agnostic desktop session for FujiNet Go Atari 800.
 *
 * Owns the emulator (atari800 on a thread of its own, core/atari800), the SDL
 * audio and gamepad backends, the in-process FujiNet runtime, the settings
 * store, the key/pad bindings, imported ROMs and the media layout. Frontends
 * (GTK4, Qt6, AppKit, Win32) do windowing, painting and event translation
 * only.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef A800SESSION_H
#define A800SESSION_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The widest picture: the NTSC filter over the full 384-pixel overscan
 * (672) and the PAL XEP80 (300 lines). */
#define A800SESSION_FB_MAX_WIDTH  672
#define A800SESSION_FB_MAX_HEIGHT 300

/* NetSIO (UDP; atari800 is the hub and listens, FujiNet connects) and the
 * FujiNet web UI. Siblings use 11500-11513. */
#define A800SESSION_NETSIO_PORT 11514
#define A800SESSION_WEBUI_PORT  11515

#define A800SESSION_AUDIO_RATE 48000

/* Highlights (Map target, debugger PC line, status dot): the rust of the
 * 800's "800" numerals. A fill colour; too dark for text on dark themes. */
#define A800SESSION_ACCENT_RGB 0x765C3C

typedef struct a800session a800session;
typedef struct a800debug a800debug;

/* All optional (NULL = default).
 *  config_dir   default $XDG_CONFIG_HOME/fujinet-go-atari800 (%APPDATA%)
 *  data_dir     default $XDG_DATA_HOME/fujinet-go-atari800 (%LOCALAPPDATA%)
 *  fujinet_lib  libfujinet path; "" disables FujiNet
 *  fujinet_runtime_src  pristine fnconfig.ini + data/ + SD/ (macOS bundle) */
typedef struct {
    const char *config_dir;
    const char *data_dir;
    const char *fujinet_lib;
    const char *fujinet_runtime_src;
} a800session_paths;

a800session *a800session_new(const a800session_paths *paths);
void a800session_free(a800session *s);

/* ---- settings ------------------------------------------------------------ */
int         a800session_get_int(a800session *s, const char *key, int def);
void        a800session_set_int(a800session *s, const char *key, int value);
const char *a800session_get_str(a800session *s, const char *key, const char *def);
void        a800session_set_str(a800session *s, const char *key, const char *value);
void        a800session_settings_flush(a800session *s);

/* ---- the machines ---------------------------------------------------------- */
typedef enum {
    A800_M_400 = 0, A800_M_800, A800_M_1200XL, A800_M_600XL, A800_M_800XL,
    A800_M_65XE, A800_M_130XE, A800_M_320XE_COMPY, A800_M_320XE_RAMBO,
    A800_M_576XE, A800_M_1088XE, A800_M_XEGS, A800_M_1400XL, A800_M_1450XLD,
    A800_M_COUNT
} a800_machine;
const char *a800_machine_name(int m);           /* "Atari 130XE" */
int a800_machine_is_xl(int m);                  /* XL/XE OS, two ports */

/* ---- Preferences, as a table every frontend builds its dialog from -------
 * Values live in the settings store under .key; after changing any, call
 * a800session_apply_settings. */
typedef enum {
    A800_PREF_BOOL = 0, A800_PREF_CHOICE, A800_PREF_INT, A800_PREF_FILE,
    A800_PREF_DIR
} a800_pref_type;

/* When a change takes effect. */
typedef enum {
    A800_APPLY_LIVE = 0,    /* at once */
    A800_APPLY_POWER,       /* power-cycles the Atari (apply_settings does it) */
    A800_APPLY_RESTART      /* restarts the session (apply_settings does it) */
} a800_apply;

typedef struct {
    const char *key;        /* settings key */
    const char *page;       /* "Machine", "Peripherals", "Video", "Audio",
                               "Input", "FujiNet" */
    const char *group;      /* group title on the page */
    const char *label;
    const char *help;       /* subtitle; may be NULL */
    int type;               /* a800_pref_type */
    int def;                /* BOOL/CHOICE/INT default */
    int min, max, step;     /* INT */
    int applies;            /* a800_apply */
} a800_pref;

int a800session_pref_count(void);
const a800_pref *a800session_pref(int i);
int a800session_pref_find(const char *key);     /* index or -1 */
/* CHOICE labels can depend on other settings (RAM per machine, imported
 * ROMs): label c of pref i, or NULL past the end. */
const char *a800session_pref_choice(a800session *s, int i, int c);
/* CHOICE values: always through these (some are stored by name). */
int  a800session_pref_get_choice(a800session *s, int i);
void a800session_pref_set_choice(a800session *s, int i, int c);
/* 0 = shown greyed (not imported, not available on this machine, ...). */
int a800session_pref_enabled(a800session *s, int i);
int a800session_pref_choice_enabled(a800session *s, int i, int c);

/* Applies every changed setting; returns the highest a800_apply it needed. */
int a800session_apply_settings(a800session *s);

/* ---- lifecycle ----------------------------------------------------------- */
typedef struct {
    int enable_fujinet;     /* start the in-process FujiNet runtime */
    int enable_audio;       /* open the SDL audio device */
    int enable_gamepad;     /* start the SDL gamepad thread */
} a800session_start_opts;

void a800session_default_opts(a800session *s, a800session_start_opts *opts);
int  a800session_start(a800session *s, const a800session_start_opts *opts);
void a800session_stop(a800session *s);
int  a800session_is_running(const a800session *s);
const char *a800session_last_error(const a800session *s);

/* ---- power ---------------------------------------------------------------- */
int  a800session_power_cycle(a800session *s);   /* cold start (and FujiNet) */
int  a800session_reset(a800session *s);         /* the RESET key: warm start */
int  a800session_reboot_to_config(a800session *s);
void a800session_set_warp(a800session *s, int on);
int  a800session_warp(a800session *s);

/* ---- video --------------------------------------------------------------- */
/* XRGB8888 into dst (A800SESSION_FB_MAX_WIDTH * _MAX_HEIGHT), its size and
 * the pixel aspect x1000 (1000 square; 500 for the NTSC filter's and the
 * 80-column cards' half-width pixels). Returns 1 if a new frame was copied;
 * *serial 0 forces a copy. */
int  a800session_copy_frame(a800session *s, uint32_t *dst, int *width, int *height,
                            int *par_milli, uint64_t *serial_inout);
int  a800session_refresh_rate(a800session *s);  /* x1000: 59923 or 49860 */
void a800session_notify_vsync(a800session *s, int64_t frame_time_ns);
/* The 80-column card's picture instead of the Atari's (View menu). */
void a800session_set_80col_display(a800session *s, int on);
int  a800session_80col_display(a800session *s);
int  a800session_80col_fitted(a800session *s);

/* ---- audio --------------------------------------------------------------- */
int  a800session_render_audio(a800session *s, float *stereo, int nframes);
void a800session_set_volume(a800session *s, int percent);

/* ---- input targets: joysticks, console keys, system actions -------------- */
typedef enum {
    A800_ACT_UP = 0, A800_ACT_DOWN, A800_ACT_LEFT, A800_ACT_RIGHT, A800_ACT_FIRE,
    A800_ACT_PER_PORT
} a800_action;
#define A800_PORTS 4

typedef enum {
    A800_CON_START = 0, A800_CON_SELECT, A800_CON_OPTION, A800_CON_HELP,
    A800_CON_RESET, A800_CON_BREAK,
    A800_CON_COUNT
} a800_console;

typedef enum {
    A800_SYSACT_POWER = 0,      /* power cycle */
    A800_SYSACT_REBOOT_CONFIG,
    A800_SYSACT_PAUSE,          /* stop in the debugger */
    A800_SYSACT_WARP,           /* toggle warp */
    A800_SYSACT_COUNT
} a800_sysaction;

#define A800_TARGET_PORT(port, act) ((port) * A800_ACT_PER_PORT + (act))
#define A800_TARGET_CONSOLE(c)      (A800_PORTS * A800_ACT_PER_PORT + (c))
#define A800_TARGET_SYSACT(sa)      (A800_PORTS * A800_ACT_PER_PORT + A800_CON_COUNT + (sa))
#define A800_TARGET_COUNT           (A800_PORTS * A800_ACT_PER_PORT + A800_CON_COUNT + A800_SYSACT_COUNT)

const char *a800_target_name(int target);       /* "Joystick 1: Up" */
const char *a800_target_short_name(int target); /* "Up" */

void a800session_press(a800session *s, int target, int down);
void a800session_console(a800session *s, int console, int down);
void a800session_sysaction(a800session *s, int sysact);
void a800session_release_all(a800session *s);
unsigned a800session_buttons_held(a800session *s, int port);  /* 1 << a800_action */
unsigned a800session_console_held(a800session *s);            /* 1 << a800_console */

/* What is plugged into each port (Preferences "port1_device" ...). */
typedef enum {
    A800_DEV_JOYSTICK = 0, A800_DEV_PADDLES, A800_DEV_KOALA, A800_DEV_ST_MOUSE,
    A800_DEV_AMIGA_MOUSE, A800_DEV_TRAKBALL, A800_DEV_LIGHTPEN, A800_DEV_LIGHTGUN,
    A800_DEV_TOUCH_TABLET, A800_DEV_CX85, A800_DEV_NONE,
    A800_DEV_COUNT
} a800_device;
const char *a800_device_name(int d);
int  a800session_port_device(a800session *s, int port);
/* A paddle from the host (228 = full left .. 0 = full right, as the POT
 * counters read), port 0-3, paddle 0-1 of the pair. */
void a800session_paddle(a800session *s, int port, int paddle, int value, int fire);
/* The pointer over the picture, in frame pixels, for mice, Koala Pad, light
 * pen and gun (buttons: bit0 left/trigger, bit1 right). */
void a800session_pointer(a800session *s, int x, int y, int inside, unsigned buttons);
void a800session_pointer_motion(a800session *s, int dx, int dy, unsigned buttons);
int  a800session_pointer_device_active(a800session *s);  /* a mouse-type device fitted */

/* ---- the keyboard ------------------------------------------------------------
 * keysym: X11-style (below); unicode: the character the host keyboard
 * produced for it (0 if none) -- the symbolic mode types that character.
 * Returns 1 if the Atari took the key. */
int  a800session_key(a800session *s, uint32_t keysym, uint32_t unicode, int down);
int  a800session_key_sysaction(a800session *s, uint32_t keysym);
/* While true, frontends send every key except their own accelerators. */
int  a800session_keyboard_captures(a800session *s);

enum {
    A800_KEYSYM_NONE = 0, A800_KEYSYM_SPACE = 0x20,
    A800_KEYSYM_BACKSPACE = 0xff08, A800_KEYSYM_TAB = 0xff09,
    A800_KEYSYM_RETURN = 0xff0d, A800_KEYSYM_PAUSE = 0xff13,
    A800_KEYSYM_SCROLL_LOCK = 0xff14, A800_KEYSYM_ESCAPE = 0xff1b,
    A800_KEYSYM_HOME = 0xff50, A800_KEYSYM_LEFT = 0xff51, A800_KEYSYM_UP = 0xff52,
    A800_KEYSYM_RIGHT = 0xff53, A800_KEYSYM_DOWN = 0xff54,
    A800_KEYSYM_PAGE_UP = 0xff55, A800_KEYSYM_PAGE_DOWN = 0xff56,
    A800_KEYSYM_END = 0xff57, A800_KEYSYM_INSERT = 0xff63,
    A800_KEYSYM_MENU = 0xff67, A800_KEYSYM_BREAK = 0xff6b,
    A800_KEYSYM_KP_ENTER = 0xff8d, A800_KEYSYM_KP_MULTIPLY = 0xffaa,
    A800_KEYSYM_KP_ADD = 0xffab, A800_KEYSYM_KP_SUBTRACT = 0xffad,
    A800_KEYSYM_KP_PERIOD = 0xffae, A800_KEYSYM_KP_DIVIDE = 0xffaf,
    A800_KEYSYM_KP_0 = 0xffb0, A800_KEYSYM_KP_1, A800_KEYSYM_KP_2,
    A800_KEYSYM_KP_3, A800_KEYSYM_KP_4, A800_KEYSYM_KP_5, A800_KEYSYM_KP_6,
    A800_KEYSYM_KP_7, A800_KEYSYM_KP_8, A800_KEYSYM_KP_9,
    A800_KEYSYM_F1 = 0xffbe, A800_KEYSYM_F2, A800_KEYSYM_F3, A800_KEYSYM_F4,
    A800_KEYSYM_F5, A800_KEYSYM_F6, A800_KEYSYM_F7, A800_KEYSYM_F8,
    A800_KEYSYM_F9, A800_KEYSYM_F10, A800_KEYSYM_F11, A800_KEYSYM_F12,
    A800_KEYSYM_LSHIFT = 0xffe1, A800_KEYSYM_RSHIFT = 0xffe2,
    A800_KEYSYM_LCTRL = 0xffe3, A800_KEYSYM_RCTRL = 0xffe4,
    A800_KEYSYM_CAPS_LOCK = 0xffe5,
    A800_KEYSYM_LALT = 0xffe9, A800_KEYSYM_RALT = 0xffea,
    A800_KEYSYM_LSUPER = 0xffeb, A800_KEYSYM_RSUPER = 0xffec,
    A800_KEYSYM_DELETE = 0xffff
};
/* Printable keys are their lower-case ASCII ('a', '1', '-', ...). */
uint32_t a800session_keysym_from_win_scancode(unsigned scancode, int extended);
uint32_t a800session_keysym_from_evdev(unsigned code);
uint32_t a800session_keysym_from_macos_keycode(unsigned keycode);
int a800session_keysym_name(uint32_t keysym, char *dst, int dstsz);

/* The on-screen keyboard, drawn by every frontend from this table. */
typedef struct {
    const char *label;      /* "Q", "RETURN", "\xe2\x86\x90" ... (UTF-8) */
    const char *shifted;    /* label of the shifted character, or NULL */
    int index;              /* for _keyboard_press / _held */
    float x, y, w, h;       /* key units from the top left */
} a800_kbd_key;
int  a800session_keyboard_layout(a800session *s, const a800_kbd_key **keys);
void a800session_keyboard_press(a800session *s, int index, int down);
int  a800session_keyboard_held(a800session *s, int index);

/* Type text as keystrokes, paced against the OS's own key handling. */
int  a800session_paste(a800session *s, const char *utf8);
void a800session_paste_cancel(a800session *s);
int  a800session_paste_pending(a800session *s);
/* The GR.0 text screen as UTF-8 lines; returns its length. */
int  a800session_copy_screen_text(a800session *s, char *dst, int dstsz);

/* ---- bindings ------------------------------------------------------------ */
typedef enum {
    A800_PAD_BTN_NONE = -1,
    A800_PAD_BTN_SOUTH = 0, A800_PAD_BTN_EAST, A800_PAD_BTN_WEST,
    A800_PAD_BTN_NORTH, A800_PAD_BTN_BACK, A800_PAD_BTN_GUIDE,
    A800_PAD_BTN_START, A800_PAD_BTN_LEFT_STICK, A800_PAD_BTN_RIGHT_STICK,
    A800_PAD_BTN_LEFT_SHOULDER, A800_PAD_BTN_RIGHT_SHOULDER,
    A800_PAD_BTN_DPAD_UP, A800_PAD_BTN_DPAD_DOWN, A800_PAD_BTN_DPAD_LEFT,
    A800_PAD_BTN_DPAD_RIGHT,
    A800_PAD_BTN_LEFT_TRIGGER, A800_PAD_BTN_RIGHT_TRIGGER,
    A800_PAD_BTN_COUNT,
    A800_PAD_BTN_RAW_BASE = 64, A800_PAD_BTN_RAW_LAST = 127,
    A800_PAD_HAT_BASE = 192, A800_PAD_HAT_LAST = 255
} a800_pad_button;
#define A800_PAD_HAT_DIRS 8
#define A800_PAD_BTN_IS_NAMED(b) ((b) >= 0 && (b) < A800_PAD_BTN_COUNT)
#define A800_PAD_BTN_IS_RAW(b)   ((b) >= A800_PAD_BTN_RAW_BASE && (b) <= A800_PAD_BTN_RAW_LAST)
#define A800_PAD_BTN_IS_HAT(b)   ((b) >= A800_PAD_HAT_BASE && (b) <= A800_PAD_HAT_LAST)

typedef struct {
    uint32_t keysym;        /* 0 = no key */
    int      button;        /* a800_pad_button */
} a800_binding;

a800_binding a800session_binding_get(a800session *s, int target);
void a800session_binding_set_key(a800session *s, int target, uint32_t keysym,
                                 char *stolen, int stolensz);
void a800session_binding_set_button(a800session *s, int target, int button,
                                    char *stolen, int stolensz);
void a800session_bindings_reset(a800session *s);
const char *a800_pad_button_name(int button);
int  a800session_target_for_key(a800session *s, uint32_t keysym);
int  a800session_target_for_button(a800session *s, int port, int button);
void a800session_gamepad_capture_begin(a800session *s);
void a800session_gamepad_capture_cancel(a800session *s);
int  a800session_gamepad_capture_poll(a800session *s, int *button);

/* ---- gamepads (hot-plugged) ----------------------------------------------- */
int  a800session_gamepad_count(a800session *s);
int  a800session_gamepad_name(a800session *s, int idx, char *dst, int dstsz);
void a800session_gamepad_assign(a800session *s, int idx, int port); /* -1 auto */
int  a800session_gamepad_assignment(a800session *s, int idx);
int  a800session_gamepad_effective_port(a800session *s, int idx);
unsigned a800session_gamepad_generation(a800session *s);
int  a800session_gamepad_last_event(a800session *s, char *dst, int dstsz);

/* Cross-thread system actions (the gamepad thread posts; a UI timer takes). */
void a800session_sysaction_post(a800session *s, int sysact);
int  a800session_sysaction_take(a800session *s, int *out);

/* ---- media ------------------------------------------------------------------ */
typedef enum {
    A800_MEDIA_UNKNOWN = 0, A800_MEDIA_DISK, A800_MEDIA_EXECUTABLE,
    A800_MEDIA_BASIC, A800_MEDIA_CARTRIDGE, A800_MEDIA_TAPE, A800_MEDIA_STATE,
    A800_MEDIA_ROM          /* a system or peripheral ROM to import */
} a800_media_kind;
int a800session_media_kind(a800session *s, const char *path);

/* Open anything: boots a disk, runs a program, inserts a cartridge, loads a
 * tape or a state, imports a ROM. msg says what happened. Returns 0, -1, or
 * 1 for a raw cartridge image whose type the user must pick
 * (cart_needs_type, cart_types_for, then cart_insert with it). */
int a800session_open(a800session *s, const char *path, char *msg, int msgsz);

/* Disk drives D1-D8. With FujiNet on they are FujiNet's (mounted from the
 * SD folder); off, they are atari800's own. */
#define A800_DRIVES 8
typedef struct {
    char path[1024];        /* "" = empty */
    int  read_only;
    int  fujinet;           /* a FujiNet slot rather than an internal drive */
} a800_drive_info;
int a800session_drive_info(a800session *s, int drive, a800_drive_info *out);
int a800session_drive_insert(a800session *s, int drive, const char *path, int read_only,
                             char *msg, int msgsz);
int a800session_drive_eject(a800session *s, int drive);
int a800session_drive_set_read_only(a800session *s, int drive, int ro);
/* A blank single (90K), enhanced (130K) or double (180K) density ATR. */
int a800session_new_blank_disk(const char *path, int density);

/* Cartridges. type 0 = from the file (or ask: cart_types_for). */
int a800session_cart_insert(a800session *s, const char *path, int type,
                            char *msg, int msgsz);
int a800session_cart_insert_piggyback(a800session *s, const char *path, int type,
                                      char *msg, int msgsz);
int a800session_cart_remove(a800session *s);
int a800session_cart_remove_piggyback(a800session *s);
/* Types a raw image of kb kilobytes may be (computer types only). */
int a800session_cart_types_for(int kb, int *types, int max);
const char *a800session_cart_type_name(int type);
int a800session_cart_info(a800session *s, char *dst, int dstsz);   /* "" = none */
/* The size of a raw cartridge image that needs a type, else 0. */
int a800session_cart_needs_type(const char *path);

/* The program recorder. */
int a800session_tape_insert(a800session *s, const char *path, char *msg, int msgsz);
int a800session_tape_record_new(a800session *s, const char *path);
int a800session_tape_rewind(a800session *s);
int a800session_tape_eject(a800session *s);
int a800session_tape_boot(a800session *s);              /* power on holding Start */
int a800session_tape_position(a800session *s, int *block, int *blocks);

/* Save states: only while FujiNet is off (it would replay SIO traffic). */
int a800session_states_available(a800session *s);
int a800session_state_save(a800session *s, const char *path);
int a800session_state_load(a800session *s, const char *path);

/* Copies into the FujiNet SD folder (archives unpacked). */
int a800session_import_to_sd(a800session *s, const char *src_path,
                             char *dest_out, int dest_sz);

/* ---- ROMs --------------------------------------------------------------------- */
typedef struct {
    const char *key;        /* settings key holding the imported path */
    const char *name;       /* "XL/XE OS rev. 2 (800XL)" */
    const char *group;      /* "400/800 OS", "XL/XE OS", "BASIC", "Peripherals" */
    uint32_t size;
} a800_rom_slot;
int a800session_rom_slot_count(void);
const a800_rom_slot *a800session_rom_slot(int i);
int a800session_rom_available(a800session *s, int slot);
/* Identify a file: the slots it can fill (best first); 0 if none. */
int a800session_rom_probe(a800session *s, const char *path, int *slots, int max);
/* Import into a slot (recognised files: rom_probe's first). */
int a800session_rom_import(a800session *s, const char *path, int slot,
                           char *msg, int msgsz);

/* ---- FujiNet ------------------------------------------------------------------- */
int         a800session_fujinet_running(const a800session *s);
int         a800session_fujinet_link(a800session *s);   /* NetSIO connected */
const char *a800session_fujinet_webui_url(const a800session *s);
int         a800session_fujinet_copy_log(a800session *s, char *dst, int max);
int         a800session_fujinet_restart(a800session *s);
unsigned    a800session_fujinet_generation(a800session *s);
int         a800session_fujinet_last_event(a800session *s, char *dst, int dstsz);

/* ---- paths and the debugger ------------------------------------------------- */
const char *a800session_config_path(const a800session *s);
const char *a800session_data_path(const a800session *s);
const char *a800session_roms_path(const a800session *s);
const char *a800session_media_path(const a800session *s);
const char *a800session_sd_path(const a800session *s);

/* A short status line: machine, TV system, FujiNet link, disk activity. */
int a800session_status(a800session *s, char *dst, int dstsz);
/* A CIM or other stop the user should hear about (one-shot); 0 if none. */
int a800session_take_notice(a800session *s, char *dst, int dstsz);

a800debug *a800session_debugger(a800session *s);

#ifdef __cplusplus
}
#endif

#endif /* A800SESSION_H */
