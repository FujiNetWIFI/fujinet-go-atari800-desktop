/*
 * bindings -- the remappable key/gamepad table and the names the bindings
 * editor needs.
 *
 * One flat target table (A800_TARGET_*): per port the joystick's four
 * directions and fire; the console keys; the session's own actions. Each
 * target holds at most one keysym and one gamepad button; rebinding steals.
 * Persisted as one "bindings" key holding only what differs from the
 * defaults. Keys not bound go to the Atari's keyboard (keymap.c).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hid_keys.h"
#include "session_internal.h"
#include "bindings.h"

typedef struct {
    uint32_t keysym;
    int button;
} slot;

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static slot s_table[A800_TARGET_COUNT];
static slot s_defaults[A800_TARGET_COUNT];
static int s_seeded;
static struct a800session *s_store;   /* whose settings hold "bindings" */

/* ---- defaults ------------------------------------------------------------- */

static void seed_key(int target, uint32_t keysym)
{
    s_defaults[target].keysym = keysym;
}

static void seed_button(int target, int button)
{
    s_defaults[target].button = button;
}

/* Joystick 1 on the numeric keypad (the Atari has none, so nothing is
 * lost), fire on keypad 0; the console keys on F2-F7 as in atari800 itself
 * (Option, Select, Start, Reset, Help, Break). F9, F11 and F12 are the
 * frontends'. Gamepads: South fires; Start, Back and North are the console's
 * Start, Select and Option. */
static void compute_defaults_locked(void)
{
    int t, port;
    for (t = 0; t < A800_TARGET_COUNT; t++) {
        s_defaults[t].keysym = 0;
        s_defaults[t].button = A800_PAD_BTN_NONE;
    }
    seed_key(A800_TARGET_PORT(0, A800_ACT_UP), A800_KEYSYM_KP_8);
    seed_key(A800_TARGET_PORT(0, A800_ACT_DOWN), A800_KEYSYM_KP_2);
    seed_key(A800_TARGET_PORT(0, A800_ACT_LEFT), A800_KEYSYM_KP_4);
    seed_key(A800_TARGET_PORT(0, A800_ACT_RIGHT), A800_KEYSYM_KP_6);
    seed_key(A800_TARGET_PORT(0, A800_ACT_FIRE), A800_KEYSYM_KP_0);

    seed_key(A800_TARGET_CONSOLE(A800_CON_OPTION), A800_KEYSYM_F2);
    seed_key(A800_TARGET_CONSOLE(A800_CON_SELECT), A800_KEYSYM_F3);
    seed_key(A800_TARGET_CONSOLE(A800_CON_START), A800_KEYSYM_F4);
    seed_key(A800_TARGET_CONSOLE(A800_CON_RESET), A800_KEYSYM_F5);
    seed_key(A800_TARGET_CONSOLE(A800_CON_HELP), A800_KEYSYM_F6);
    seed_key(A800_TARGET_CONSOLE(A800_CON_BREAK), A800_KEYSYM_F7);

    for (port = 0; port < A800_PORTS; port++)
        seed_button(A800_TARGET_PORT(port, A800_ACT_FIRE), A800_PAD_BTN_SOUTH);
    seed_button(A800_TARGET_CONSOLE(A800_CON_START), A800_PAD_BTN_START);
    seed_button(A800_TARGET_CONSOLE(A800_CON_SELECT), A800_PAD_BTN_BACK);
    seed_button(A800_TARGET_CONSOLE(A800_CON_OPTION), A800_PAD_BTN_NORTH);
}

static uint32_t fold(uint32_t keysym)
{
    if (keysym >= 'A' && keysym <= 'Z') return keysym + 32;
    return keysym;
}

/* ---- persistence ---------------------------------------------------------- */

static void apply_persisted_locked(const char *packed)
{
    const char *p = packed;
    while (p && *p) {
        int target = -1; char kind = 0; long value = 0;
        const char *end = strchr(p, ' ');
        if (sscanf(p, "%d.%c:%ld", &target, &kind, &value) == 3
            && target >= 0 && target < A800_TARGET_COUNT) {
            if (kind == 'k') s_table[target].keysym = (uint32_t)value;
            else if (kind == 'b') s_table[target].button = (int)value;
        }
        if (!end) break;
        p = end + 1;
    }
}

static void pack_locked(char *out, size_t outsz)
{
    size_t len = 0;
    int t;
    out[0] = '\0';
    for (t = 0; t < A800_TARGET_COUNT; t++) {
        if (s_table[t].keysym != s_defaults[t].keysym)
            len += (size_t)snprintf(out + len, outsz - len, "%s%d.k:%u",
                                    len ? " " : "", t, s_table[t].keysym);
        if (len >= outsz) break;
        if (s_table[t].button != s_defaults[t].button)
            len += (size_t)snprintf(out + len, outsz - len, "%s%d.b:%d",
                                    len ? " " : "", t, s_table[t].button);
        if (len >= outsz) break;
    }
}

static void persist_locked(void)
{
    char packed[8192];
    if (!s_store) return;
    pack_locked(packed, sizeof packed);
    a800session_set_str(s_store, "bindings", packed);
}

static void ensure_seeded_locked(void)
{
    if (s_seeded) return;
    compute_defaults_locked();
    memcpy(s_table, s_defaults, sizeof s_table);
    s_seeded = 1;
}

void bindings_init(struct a800session *s)
{
    pthread_mutex_lock(&s_lock);
    compute_defaults_locked();
    memcpy(s_table, s_defaults, sizeof s_table);
    s_seeded = 1;
    s_store = s;
    if (s)
        apply_persisted_locked(a800session_get_str(s, "bindings", ""));
    pthread_mutex_unlock(&s_lock);
}

/* ---- lookups -------------------------------------------------------------- */

static int target_for_key_locked(uint32_t keysym)
{
    int t;
    if (!keysym) return -1;
    for (t = 0; t < A800_TARGET_COUNT; t++)
        if (s_table[t].keysym == keysym) return t;
    return -1;
}

int a800session_target_for_key(a800session *s, uint32_t keysym)
{
    int t;
    (void)s;
    pthread_mutex_lock(&s_lock);
    ensure_seeded_locked();
    t = target_for_key_locked(fold(keysym));
    pthread_mutex_unlock(&s_lock);
    return t;
}

/* Gamepad buttons are scoped to the pad's port: the same button index is
 * free to mean different things on different ports, so only that port's
 * targets and the machine-wide switches/actions are searched. */
int a800session_target_for_button(a800session *s, int port, int button)
{
    int t, found = -1;
    (void)s;
    if (button == A800_PAD_BTN_NONE) return -1;
    pthread_mutex_lock(&s_lock);
    ensure_seeded_locked();
    for (t = 0; t < A800_ACT_PER_PORT; t++)
        if (s_table[A800_TARGET_PORT(port, t)].button == button) {
            found = A800_TARGET_PORT(port, t);
            break;
        }
    if (found < 0)
        for (t = A800_PORTS * A800_ACT_PER_PORT; t < A800_TARGET_COUNT; t++)
            if (s_table[t].button == button) { found = t; break; }
    pthread_mutex_unlock(&s_lock);
    return found;
}

a800_binding a800session_binding_get(a800session *s, int target)
{
    a800_binding b = { 0, A800_PAD_BTN_NONE };
    (void)s;
    if (target < 0 || target >= A800_TARGET_COUNT) return b;
    pthread_mutex_lock(&s_lock);
    ensure_seeded_locked();
    b.keysym = s_table[target].keysym;
    b.button = s_table[target].button;
    pthread_mutex_unlock(&s_lock);
    return b;
}

/* A key may legitimately drive several targets (two players' controls can
 * be put on one key by hand). An explicit rebinding is the user saying
 * "this key does exactly this", so it displaces every holder -- and names
 * them all, comma-separated, so nothing is lost silently. */
static void describe_stolen(const int *victims, int nvictims, char *stolen,
                            int stolensz)
{
    int i, len = 0;
    if (!stolen || stolensz <= 0) return;
    stolen[0] = '\0';
    for (i = 0; i < nvictims && len < stolensz; i++)
        len += snprintf(stolen + len, (size_t)(stolensz - len), "%s%s",
                        i ? ", " : "", a800_target_name(victims[i]));
}

void a800session_binding_set_key(a800session *s, int target, uint32_t keysym,
                                  char *stolen, int stolensz)
{
    int victims[A800_TARGET_COUNT], nvictims = 0, t;
    (void)s;
    if (target < 0 || target >= A800_TARGET_COUNT) return;
    keysym = fold(keysym);
    pthread_mutex_lock(&s_lock);
    ensure_seeded_locked();
    if (keysym) {
        for (t = 0; t < A800_TARGET_COUNT; t++)
            if (t != target && s_table[t].keysym == keysym) {
                s_table[t].keysym = 0;
                victims[nvictims++] = t;
            }
    }
    s_table[target].keysym = keysym;
    persist_locked();
    pthread_mutex_unlock(&s_lock);
    describe_stolen(victims, nvictims, stolen, stolensz);
}

void a800session_binding_set_button(a800session *s, int target, int button,
                                     char *stolen, int stolensz)
{
    int victims[A800_TARGET_COUNT], nvictims = 0, t;
    int port = target / A800_ACT_PER_PORT;   /* >= A800_PORTS: console, sysactions */
    (void)s;
    if (target < 0 || target >= A800_TARGET_COUNT) return;
    pthread_mutex_lock(&s_lock);
    ensure_seeded_locked();
    if (button != A800_PAD_BTN_NONE) {
        for (t = 0; t < A800_TARGET_COUNT; t++) {
            int tport = t / A800_ACT_PER_PORT;
            /* steal only within the same scope: this port, or the
             * machine-wide targets, which every pad reaches */
            if (t != target && s_table[t].button == button
                && (tport == port || tport >= A800_PORTS || port >= A800_PORTS)) {
                s_table[t].button = A800_PAD_BTN_NONE;
                victims[nvictims++] = t;
            }
        }
    }
    s_table[target].button = button;
    persist_locked();
    pthread_mutex_unlock(&s_lock);
    describe_stolen(victims, nvictims, stolen, stolensz);
}

void a800session_bindings_reset(a800session *s)
{
    (void)s;
    pthread_mutex_lock(&s_lock);
    compute_defaults_locked();
    memcpy(s_table, s_defaults, sizeof s_table);
    s_seeded = 1;
    if (s_store) a800session_set_str(s_store, "bindings", "");
    pthread_mutex_unlock(&s_lock);
}

/* ---- names ---------------------------------------------------------------- */

static const char *const act_names[A800_ACT_PER_PORT] = {
    "Up", "Down", "Left", "Right", "Fire",
};
static const char *const console_names[A800_CON_COUNT] = {
    "Start", "Select", "Option", "Help", "Reset", "Break",
};
static const char *const sysact_names[A800_SYSACT_COUNT] = {
    "Power cycle", "Reboot to CONFIG", "Pause (debugger)", "Warp speed",
};

const char *a800_target_name(int target)
{
    static char buf[64];
    if (target < 0 || target >= A800_TARGET_COUNT) return "";
    if (target < A800_TARGET_CONSOLE(0)) {
        snprintf(buf, sizeof buf, "Joystick %d: %s", target / A800_ACT_PER_PORT + 1,
                 act_names[target % A800_ACT_PER_PORT]);
        return buf;
    }
    if (target < A800_TARGET_SYSACT(0)) return console_names[target - A800_TARGET_CONSOLE(0)];
    return sysact_names[target - A800_TARGET_SYSACT(0)];
}

const char *a800_target_short_name(int target)
{
    if (target < 0 || target >= A800_TARGET_COUNT) return "";
    if (target < A800_TARGET_CONSOLE(0)) return act_names[target % A800_ACT_PER_PORT];
    if (target < A800_TARGET_SYSACT(0)) return console_names[target - A800_TARGET_CONSOLE(0)];
    return sysact_names[target - A800_TARGET_SYSACT(0)];
}

static const char *const pad_button_names[A800_PAD_BTN_COUNT] = {
    "A", "B", "X", "Y", "Back", "Guide", "Start", "Left Stick", "Right Stick",
    "Left Shoulder", "Right Shoulder", "D-pad Up", "D-pad Down", "D-pad Left",
    "D-pad Right", "Left Trigger", "Right Trigger",
};

const char *a800_pad_button_name(int button)
{
    static char buf[32];
    if (A800_PAD_BTN_IS_NAMED(button)) return pad_button_names[button];
    if (A800_PAD_BTN_IS_RAW(button)) {
        snprintf(buf, sizeof buf, "Button %d", button - A800_PAD_BTN_RAW_BASE);
        return buf;
    }
    if (A800_PAD_BTN_IS_HAT(button)) {
        static const char *const dirs[A800_PAD_HAT_DIRS] =
            { "Up", "Up-Right", "Right", "Down-Right", "Down", "Down-Left",
              "Left", "Up-Left" };
        int h = (button - A800_PAD_HAT_BASE) / A800_PAD_HAT_DIRS;
        int d = (button - A800_PAD_HAT_BASE) % A800_PAD_HAT_DIRS;
        snprintf(buf, sizeof buf, "Hat %d %s", h, dirs[d]);
        return buf;
    }
    return "";
}

int a800session_keysym_name(uint32_t keysym, char *dst, int dstsz)
{
    const char *name = NULL;
    if (!dst || dstsz <= 0) return 0;
    switch (keysym) {
    case 0: name = ""; break;
    case A800_KEYSYM_UP: name = "Up"; break;
    case A800_KEYSYM_DOWN: name = "Down"; break;
    case A800_KEYSYM_LEFT: name = "Left"; break;
    case A800_KEYSYM_RIGHT: name = "Right"; break;
    case A800_KEYSYM_ESCAPE: name = "Escape"; break;
    case A800_KEYSYM_RETURN: name = "Return"; break;
    case A800_KEYSYM_BACKSPACE: name = "Backspace"; break;
    case A800_KEYSYM_TAB: name = "Tab"; break;
    case A800_KEYSYM_SPACE: name = "Space"; break;
    case A800_KEYSYM_LSHIFT: name = "Left Shift"; break;
    case A800_KEYSYM_RSHIFT: name = "Right Shift"; break;
    case A800_KEYSYM_LCTRL: name = "Left Ctrl"; break;
    case A800_KEYSYM_RCTRL: name = "Right Ctrl"; break;
    case A800_KEYSYM_LALT: name = "Left Alt"; break;
    case A800_KEYSYM_RALT: name = "Right Alt"; break;
    case A800_KEYSYM_KP_ENTER: name = "Keypad Enter"; break;
    case A800_KEYSYM_KP_MULTIPLY: name = "Keypad *"; break;
    case A800_KEYSYM_KP_DIVIDE: name = "Keypad /"; break;
    case A800_KEYSYM_KP_PERIOD: name = "Keypad ."; break;
    case 0xffff: name = "Delete"; break;
    case 0xff63: name = "Insert"; break;
    case 0xff50: name = "Home"; break;
    case 0xff57: name = "End"; break;
    case 0xff55: name = "Page Up"; break;
    case 0xff56: name = "Page Down"; break;
    case 0xffe5: name = "Caps Lock"; break;
    default: break;
    }
    if (name) return snprintf(dst, (size_t)dstsz, "%s", name);
    if (keysym >= A800_KEYSYM_F1 && keysym <= A800_KEYSYM_F12)
        return snprintf(dst, (size_t)dstsz, "F%u", keysym - A800_KEYSYM_F1 + 1);
    if (keysym >= A800_KEYSYM_KP_0 && keysym <= A800_KEYSYM_KP_9)
        return snprintf(dst, (size_t)dstsz, "Keypad %u", keysym - A800_KEYSYM_KP_0);
    if (keysym >= 0x21 && keysym <= 0x7e) {
        char c = (char)keysym;
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        return snprintf(dst, (size_t)dstsz, "%c", c);
    }
    if (keysym >= A800SESSION_KEYSYM_HID_BASE
        && keysym <= A800SESSION_KEYSYM_HID_BASE + A800SESSION_HID_USAGE_MAX) {
        const char *hid = a800_hid_usage_name(keysym - A800SESSION_KEYSYM_HID_BASE);
        if (hid) return snprintf(dst, (size_t)dstsz, "%s", hid);
        return snprintf(dst, (size_t)dstsz, "HID 0x%02X",
                        (unsigned)(keysym - A800SESSION_KEYSYM_HID_BASE));
    }
    return snprintf(dst, (size_t)dstsz, "Key 0x%X", keysym);
}

/* ---- for a800session_key ------------------------------------------------ */

int a800session_key_sysaction(a800session *s, uint32_t keysym)
{
    int t = a800session_target_for_key(s, keysym);
    if (t < A800_TARGET_SYSACT(0)) return -1;
    return t - A800_TARGET_SYSACT(0);
}

/* The joystick or console target a key drives, or -1 (system actions are
 * the frontends' to fire). */
int session_binding_target_for_key(struct a800session *s, uint32_t keysym)
{
    int t = a800session_target_for_key(s, keysym);
    return t >= 0 && t < A800_TARGET_SYSACT(0) ? t : -1;
}
