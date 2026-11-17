/*
 * gamepad_sdl -- SDL3 gamepads (and unmapped joysticks) as Atari 7800
 * joysticks, on their own polling thread.
 *
 * A polling thread rather than SDL's event queue on the UI thread: this
 * app's video belongs to GTK/Qt/AppKit/Win32, not SDL, and only the gamepad
 * subsystem is initialised here -- never SDL video. Devices are added and
 * removed as they appear on the system: SDL posts the JOYSTICK_ and
 * GAMEPAD_ add/remove pairs and both are taken, with idempotent open/close,
 * so neither kind of device can be missed. A device SDL has no gamepad
 * mapping for (a plain HID adapter) is opened as a raw joystick instead:
 * its buttons and hats are bindable in the raw bands, just unnamed.
 *
 * What a pad drives is the port's joystick: the D-pad (and, with "analog
 * sticks drive the joystick" on, the left stick as an eight-way switch with
 * hysteresis) is the stick; its two buttons come from the bindings table. A
 * port set to "None" takes nothing.
 *
 * Buttons go through the bindings table (a port's buttons, the console's
 * switches, the system actions); a D-pad direction the user has bound to
 * something explicit stops also nudging the directional role.
 *
 * Pads are assigned to ports in connection order unless assigned explicitly.
 * An explicit assignment is remembered (settings key "pad_assign", see
 * pad_assign.h): a pad that is unplugged and plugged back in, or is there on
 * the next run (matched by its SDL GUID, else its name), gets it back, and
 * every connect/disconnect is reported to the session for the frontend's toast.
 *
 * Everything a pad holds is its own source (input_merge.h): its buttons and
 * its stick are separate, so a pad letting go never cancels the keyboard, the
 * window or another pad, and a pad that is unplugged lets go of exactly what
 * it held.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "gamepad_sdl.h"
#include "input_merge.h"
#include "pad_assign.h"
#include "session_internal.h"

#define MAX_PADS A800_MAX_PADS
#define MAX_HATS 4

/* A third of full deflection engages, a sixth releases: low enough that a
 * worn stick still registers, high enough that resting drift does not, and
 * a gap wide enough that jitter across one threshold cannot flutter. */
#define STICK_ENTER 0.35f
#define STICK_EXIT  0.15f
#define TRIGGER_ON  0.5f

typedef struct {
    SDL_JoystickID id;
    int owner;                /* the input_merge.h source number for this pad: stable
                               * while it is connected, whatever its slot */
    SDL_Gamepad *handle;      /* mapped gamepad, or NULL */
    SDL_Joystick *joy;        /* raw joystick when not a gamepad */
    int assign;               /* explicit port, -1 = automatic */
    int by_name;              /* assign was restored from a name-only match
                               * (the entry's GUID differs): an exact-GUID
                               * pad may take it over */
    int port;                 /* the port it drove on the last poll, or -1 */
    int dev;                  /* what that port had plugged in on the last poll */
    int stick_dir;            /* left stick's four-way state (hysteresis) */
    int dpad_dir;             /* D-pad / hat 0 state */
    uint8_t sent[A800_ACT_PER_PORT]; /* directional actions currently held */
    int sent_pot[2];          /* paddle positions last handed to input.cpp, -1 none */
    int sent_fire[2];         /* paddle fire buttons last handed over */
    /* What each button's press asserted (target + 1, 0 = nothing), so its
     * release lets go of exactly that even if the binding changed meanwhile,
     * and a pad that goes away drops what it held. */
    int16_t btn_target[256];
    SDL_GUID guid;
    char guidstr[A800_PAD_GUID_MAX];   /* guid as hex, for the pad_assign table */
    char name[A800_PAD_NAME_MAX];
    int trigger_down[2];
    Uint8 last_hat[MAX_HATS];
} pad_slot;

typedef struct {
    struct a800session *session;
    pthread_t thread;
    pthread_mutex_t lock;
    int running;
    volatile int stop_requested;
    pad_slot pads[MAX_PADS];
    int npads;
    unsigned generation;
    int quiet_scan;           /* the first scan of a restarted session re-finds
                               * pads without announcing them */
    /* Map-mode capture */
    int cap_active, cap_have, cap_result;
    /* the remembered explicit assignments (the "pad_assign" setting), for
     * pads that are away as well as those here; guarded by lock */
    a800_pad_table table;
} gamepad_state;

static gamepad_state *g_state;   /* one per process: SDL's subsystem is */

/* ---- pure helpers ----------------------------------------------------------- */

int a800_dir_from_stick(float x, float y, int prev, float enter, float exit)
{
    int dir = 0;
    /* each axis independently, so diagonals are two bits and a stick that
     * is exactly horizontal never picks up a vertical bit from noise */
    if (x <= -enter || (x <= -exit && (prev & A800_DIR_LEFT))) dir |= A800_DIR_LEFT;
    if (x >=  enter || (x >=  exit && (prev & A800_DIR_RIGHT))) dir |= A800_DIR_RIGHT;
    if (y <= -enter || (y <= -exit && (prev & A800_DIR_UP))) dir |= A800_DIR_UP;
    if (y >=  enter || (y >=  exit && (prev & A800_DIR_DOWN))) dir |= A800_DIR_DOWN;
    return dir;
}

int a800_pad_for_port(const int *assign, int npads, int port)
{
    int i, next = 0;
    for (i = 0; i < npads; i++)
        if (assign[i] == port) return i;
    for (i = 0; i < npads; i++) {
        if (assign[i] >= 0) continue;   /* explicitly elsewhere */
        if (next == port) return i;
        next++;
    }
    return -1;
}

int a800_pad_button_from_sdl(uint8_t button)
{
    switch (button) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return A800_PAD_BTN_SOUTH;
    case SDL_GAMEPAD_BUTTON_EAST: return A800_PAD_BTN_EAST;
    case SDL_GAMEPAD_BUTTON_WEST: return A800_PAD_BTN_WEST;
    case SDL_GAMEPAD_BUTTON_NORTH: return A800_PAD_BTN_NORTH;
    case SDL_GAMEPAD_BUTTON_BACK: return A800_PAD_BTN_BACK;
    case SDL_GAMEPAD_BUTTON_GUIDE: return A800_PAD_BTN_GUIDE;
    case SDL_GAMEPAD_BUTTON_START: return A800_PAD_BTN_START;
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: return A800_PAD_BTN_LEFT_STICK;
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return A800_PAD_BTN_RIGHT_STICK;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return A800_PAD_BTN_LEFT_SHOULDER;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return A800_PAD_BTN_RIGHT_SHOULDER;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return A800_PAD_BTN_DPAD_UP;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return A800_PAD_BTN_DPAD_DOWN;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return A800_PAD_BTN_DPAD_LEFT;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return A800_PAD_BTN_DPAD_RIGHT;
    default: return A800_PAD_BTN_NONE;
    }
}

int a800_sdl_button_from_pad(int button)
{
    switch (button) {
    case A800_PAD_BTN_SOUTH: return SDL_GAMEPAD_BUTTON_SOUTH;
    case A800_PAD_BTN_EAST: return SDL_GAMEPAD_BUTTON_EAST;
    case A800_PAD_BTN_WEST: return SDL_GAMEPAD_BUTTON_WEST;
    case A800_PAD_BTN_NORTH: return SDL_GAMEPAD_BUTTON_NORTH;
    case A800_PAD_BTN_BACK: return SDL_GAMEPAD_BUTTON_BACK;
    case A800_PAD_BTN_GUIDE: return SDL_GAMEPAD_BUTTON_GUIDE;
    case A800_PAD_BTN_START: return SDL_GAMEPAD_BUTTON_START;
    case A800_PAD_BTN_LEFT_STICK: return SDL_GAMEPAD_BUTTON_LEFT_STICK;
    case A800_PAD_BTN_RIGHT_STICK: return SDL_GAMEPAD_BUTTON_RIGHT_STICK;
    case A800_PAD_BTN_LEFT_SHOULDER: return SDL_GAMEPAD_BUTTON_LEFT_SHOULDER;
    case A800_PAD_BTN_RIGHT_SHOULDER: return SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER;
    case A800_PAD_BTN_DPAD_UP: return SDL_GAMEPAD_BUTTON_DPAD_UP;
    case A800_PAD_BTN_DPAD_DOWN: return SDL_GAMEPAD_BUTTON_DPAD_DOWN;
    case A800_PAD_BTN_DPAD_LEFT: return SDL_GAMEPAD_BUTTON_DPAD_LEFT;
    case A800_PAD_BTN_DPAD_RIGHT: return SDL_GAMEPAD_BUTTON_DPAD_RIGHT;
    default: return -1;
    }
}

/* ---- slots ------------------------------------------------------------------ */

static int slot_for_id(gamepad_state *g, SDL_JoystickID id)
{
    int i;
    for (i = 0; i < g->npads; i++)
        if (g->pads[i].id == id) return i;
    return -1;
}

static const char *slot_name(const pad_slot *slot)
{
    const char *n = slot->handle ? SDL_GetGamepadName(slot->handle)
                                 : SDL_GetJoystickName(slot->joy);
    return n ? n : "Gamepad";
}

static void slot_release_all(gamepad_state *g, pad_slot *slot)
{
    memset(slot->sent, 0, sizeof slot->sent);
    slot->sent_pot[0] = slot->sent_pot[1] = -1;
    slot->sent_fire[0] = slot->sent_fire[1] = 0;
    memset(slot->btn_target, 0, sizeof slot->btn_target);
    input_pad_clear(g->session, slot->owner);
}

static void slot_close(gamepad_state *g, pad_slot *slot)
{
    slot_release_all(g, slot);
    if (slot->handle) SDL_CloseGamepad(slot->handle);
    if (slot->joy) SDL_CloseJoystick(slot->joy);
    slot->handle = NULL;
    slot->joy = NULL;
}

static int effective_port_locked(gamepad_state *g, int idx);

/* Gives the pad in slot idx its remembered port. An entry with the pad's own
 * GUID is the pad's: it takes it from a pad that got it only through the name
 * fallback. A pad known only by name takes the entry if no other pad carries
 * it (two identical pads, or one pad known under two GUIDs): then this one
 * stays automatic. Locked by the caller; only reads g->table, never writes. */
static void restore_assignment(gamepad_state *g, int idx)
{
    pad_slot *slot = &g->pads[idx];
    int ent = a800_pad_table_find(&g->table, slot->guidstr, slot->name), k;
    int exact;

    if (ent < 0 || g->table.entry[ent].port >= A800_PORTS) return;
    exact = !strcmp(g->table.entry[ent].guid, slot->guidstr);
    for (k = 0; k < g->npads; k++) {
        pad_slot *other = &g->pads[k];
        if (k == idx || other->assign < 0 ||
            a800_pad_table_find(&g->table, other->guidstr, other->name) != ent)
            continue;
        if (!exact || !other->by_name) return;
        other->assign = -1;          /* the entry's owner is here: it wins */
        other->by_name = 0;
    }
    slot->assign = g->table.entry[ent].port;
    slot->by_name = !exact;
}

/* Idempotent, and the single place the gamepad/joystick decision is made:
 * both SDL_EVENT_JOYSTICK_ADDED and SDL_EVENT_GAMEPAD_ADDED land here (SDL
 * posts both for a mapped device) and the slot_for_id guard makes the second
 * one a no-op. */
static void open_pad(gamepad_state *g, SDL_JoystickID id)
{
    char msg[128];
    int announce = 0;

    pthread_mutex_lock(&g->lock);
    if (g->npads < MAX_PADS && slot_for_id(g, id) < 0) {
        /* Not an else-if on SDL_IsGamepad: a device SDL calls a gamepad can
         * still fail to open as one. Falling through to the raw joystick
         * keeps it usable -- every button is still bindable, just unnamed. */
        SDL_Gamepad *gp = SDL_IsGamepad(id) ? SDL_OpenGamepad(id) : NULL;
        SDL_Joystick *j = gp ? NULL : SDL_OpenJoystick(id);
        if (gp || j) {
            pad_slot *slot = &g->pads[g->npads++];
            int h, k, used = 0, port;
            for (k = 0; k < g->npads - 1; k++)
                used |= 1 << g->pads[k].owner;
            memset(slot, 0, sizeof *slot);
            for (k = 0; k < MAX_PADS && (used & (1 << k)); k++) {}
            slot->owner = k;
            slot->id = id;
            slot->handle = gp;
            slot->joy = j;
            slot->assign = -1;
            slot->port = -1;
            slot->dev = -1;
            slot->sent_pot[0] = slot->sent_pot[1] = -1;
            for (h = 0; h < MAX_HATS; h++) slot->last_hat[h] = SDL_HAT_CENTERED;
            slot->guid = SDL_GetJoystickGUIDForID(id);
            snprintf(slot->name, sizeof slot->name, "%s", slot_name(slot));
            SDL_GUIDToString(slot->guid, slot->guidstr, sizeof slot->guidstr);
            restore_assignment(g, g->npads - 1);
            fprintf(stderr, "a800 gamepad: %s connected (%s)\n", slot->name,
                    gp ? "gamepad" : "raw joystick");
            if (!g->quiet_scan) {
                port = effective_port_locked(g, g->npads - 1);
                if (port >= 0)
                    snprintf(msg, sizeof msg, "Connected: %s (joystick %d)", slot->name, port + 1);
                else
                    snprintf(msg, sizeof msg, "Connected: %s", slot->name);
                g->generation++;
                announce = 1;
            }
        }
    }
    pthread_mutex_unlock(&g->lock);
    if (announce) session_gamepad_event(g->session, msg);
}

static void close_pad(gamepad_state *g, SDL_JoystickID id)
{
    char msg[128];
    int idx, i, announce = 0;
    pthread_mutex_lock(&g->lock);
    idx = slot_for_id(g, id);
    if (idx >= 0) {
        fprintf(stderr, "a800 gamepad: %s disconnected\n", g->pads[idx].name);
        snprintf(msg, sizeof msg, "Disconnected: %s", g->pads[idx].name);
        /* its explicit assignment stays in the table: it comes back with it */
        slot_close(g, &g->pads[idx]);
        for (i = idx; i < g->npads - 1; i++) g->pads[i] = g->pads[i + 1];
        g->npads--;
        g->generation++;
        announce = 1;
    }
    pthread_mutex_unlock(&g->lock);
    if (announce) session_gamepad_event(g->session, msg);
}

/* ---- buttons ---------------------------------------------------------------- */

/* The port a slot drives right now. Locked by the caller. */
static int effective_port_locked(gamepad_state *g, int idx)
{
    int assign[MAX_PADS], i, port;
    for (i = 0; i < MAX_PADS; i++) assign[i] = -1;
    for (i = 0; i < g->npads; i++) assign[i] = g->pads[i].assign;
    for (port = 0; port < session_active_ports(g->session); port++)
        if (a800_pad_for_port(assign, g->npads, port) == idx) return port;
    return -1;
}

static int is_sysact(int target)
{
    return target >= A800_TARGET_SYSACT(0);
}

/* The slot's port (and what is plugged into it) as of now. When either has
 * changed -- the pad was assigned elsewhere, or the port's device was swapped --
 * what it held belongs to the old arrangement and is let go. A pad that had no
 * port holds nothing. Called by the poll and by the button dispatch alike, so a
 * button is always pressed on the port the pad has at that moment: a press that
 * lands just before the poll that would have noticed the move is not undone by
 * it. Locked by the caller. */
static void slot_sync_port(gamepad_state *g, pad_slot *slot, int port)
{
    const int dev = port >= 0 ? a800session_port_device(g->session, port) : -1;
    if (port == slot->port && dev == slot->dev) return;
    if (slot->port >= 0) slot_release_all(g, slot);
    slot->port = port;
    slot->dev = dev;
    slot->stick_dir = 0;
}

static void dispatch_button(gamepad_state *g, SDL_JoystickID which, int b, int down)
{
    int idx, port, owner = 0, target = -1;
    if (b == A800_PAD_BTN_NONE) return;

    pthread_mutex_lock(&g->lock);
    if (g->cap_active) {
        /* Map mode: every button event is swallowed while armed, not just
         * the captured one, so the press that maps a button does not also
         * fire whatever it used to do. */
        if (down && !g->cap_have) { g->cap_have = 1; g->cap_result = b; }
        pthread_mutex_unlock(&g->lock);
        return;
    }
    idx = slot_for_id(g, which);
    port = idx >= 0 ? effective_port_locked(g, idx) : -1;
    if (idx >= 0) slot_sync_port(g, &g->pads[idx], port);
    if (idx >= 0 && port >= 0 && b >= 0 && b < 256) {
        pad_slot *slot = &g->pads[idx];
        owner = slot->owner;
        if (down) {
            target = a800session_target_for_button(g->session, port, b);
            if (target >= 0 && !is_sysact(target))
                slot->btn_target[b] = (int16_t)(target + 1);
        } else if (slot->btn_target[b]) {
            /* what the press asserted, whatever the binding says now */
            target = slot->btn_target[b] - 1;
            slot->btn_target[b] = 0;
        }
    }
    pthread_mutex_unlock(&g->lock);
    if (target < 0) return;

    /* a system action is posted, on the press only, for the UI thread */
    if (is_sysact(target)) {
        if (down) a800session_sysaction_post(g->session, target - A800_TARGET_SYSACT(0));
        return;
    }
    input_press(g->session, A800_SRC_PAD(owner), target, down);
}

static int hat_dir_from_mask(Uint8 mask)
{
    switch (mask) {
    case SDL_HAT_UP: return 0;
    case SDL_HAT_RIGHTUP: return 1;
    case SDL_HAT_RIGHT: return 2;
    case SDL_HAT_RIGHTDOWN: return 3;
    case SDL_HAT_DOWN: return 4;
    case SDL_HAT_LEFTDOWN: return 5;
    case SDL_HAT_LEFT: return 6;
    case SDL_HAT_LEFTUP: return 7;
    default: return -1;
    }
}

static void handle_hat_motion(gamepad_state *g, SDL_JoystickID which, int hat, Uint8 mask)
{
    int idx, prev_dir, dir;
    if (hat < 0 || hat >= MAX_HATS) return;
    pthread_mutex_lock(&g->lock);
    idx = slot_for_id(g, which);
    if (idx < 0 || g->pads[idx].handle) { pthread_mutex_unlock(&g->lock); return; }
    prev_dir = hat_dir_from_mask(g->pads[idx].last_hat[hat]);
    g->pads[idx].last_hat[hat] = mask;
    pthread_mutex_unlock(&g->lock);
    dir = hat_dir_from_mask(mask);
    /* hat positions are bindable buttons in their own band; hat 0's four
     * cardinal points also feed the directional role in poll_sticks */
    if (prev_dir >= 0)
        dispatch_button(g, which, A800_PAD_HAT_BASE + hat * A800_PAD_HAT_DIRS + prev_dir, 0);
    if (dir >= 0)
        dispatch_button(g, which, A800_PAD_HAT_BASE + hat * A800_PAD_HAT_DIRS + dir, 1);
}

/* ---- the directional role and the analog controllers ------------------------ */

static float axis_norm(Sint16 v)
{
    return v < 0 ? (float)v / 32768.0f : (float)v / 32767.0f;
}

static void slot_stick(const pad_slot *slot, int right, float *x, float *y)
{
    if (slot->handle) {
        *x = axis_norm(SDL_GetGamepadAxis(slot->handle, right ? SDL_GAMEPAD_AXIS_RIGHTX : SDL_GAMEPAD_AXIS_LEFTX));
        *y = axis_norm(SDL_GetGamepadAxis(slot->handle, right ? SDL_GAMEPAD_AXIS_RIGHTY : SDL_GAMEPAD_AXIS_LEFTY));
    } else {
        int base = right ? 2 : 0;
        *x = SDL_GetNumJoystickAxes(slot->joy) > base ? axis_norm(SDL_GetJoystickAxis(slot->joy, base)) : 0.f;
        *y = SDL_GetNumJoystickAxes(slot->joy) > base + 1 ? axis_norm(SDL_GetJoystickAxis(slot->joy, base + 1)) : 0.f;
    }
}

static int slot_dpad(gamepad_state *g, const pad_slot *slot, int port)
{
    int dir = 0;
    if (slot->handle) {
        /* a D-pad direction the user bound explicitly no longer plays the
         * directional role -- otherwise a button remapped to a keypad key
         * would also nudge the joystick every poll */
        if (a800session_target_for_button(g->session, port, A800_PAD_BTN_DPAD_UP) < 0
            && SDL_GetGamepadButton(slot->handle, SDL_GAMEPAD_BUTTON_DPAD_UP)) dir |= A800_DIR_UP;
        if (a800session_target_for_button(g->session, port, A800_PAD_BTN_DPAD_DOWN) < 0
            && SDL_GetGamepadButton(slot->handle, SDL_GAMEPAD_BUTTON_DPAD_DOWN)) dir |= A800_DIR_DOWN;
        if (a800session_target_for_button(g->session, port, A800_PAD_BTN_DPAD_LEFT) < 0
            && SDL_GetGamepadButton(slot->handle, SDL_GAMEPAD_BUTTON_DPAD_LEFT)) dir |= A800_DIR_LEFT;
        if (a800session_target_for_button(g->session, port, A800_PAD_BTN_DPAD_RIGHT) < 0
            && SDL_GetGamepadButton(slot->handle, SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) dir |= A800_DIR_RIGHT;
    } else if (SDL_GetNumJoystickHats(slot->joy) > 0) {
        Uint8 m = SDL_GetJoystickHat(slot->joy, 0);
        if (m & SDL_HAT_UP) dir |= A800_DIR_UP;
        if (m & SDL_HAT_DOWN) dir |= A800_DIR_DOWN;
        if (m & SDL_HAT_LEFT) dir |= A800_DIR_LEFT;
        if (m & SDL_HAT_RIGHT) dir |= A800_DIR_RIGHT;
    }
    return dir;
}

static void want_action(uint8_t *want, int act, int on)
{
    if (on) want[act] = 1;
}

/* A stick axis as a paddle position: a small dead zone (resting drift must not
 * wander a paddle), rescaled so the paddle still reaches its stops. 0 is the
 * centre; the sign is the POT counters' (228 full left .. 0 full right). */
#define PADDLE_DEADZONE 0.12f
static int pot_from_axis(float v)
{
    float mag = fabsf(v);
    if (mag <= PADDLE_DEADZONE) v = 0.f;
    else v = (v < 0.f ? -1.f : 1.f) * (mag - PADDLE_DEADZONE) / (1.f - PADDLE_DEADZONE);
    if (v > 1.f) v = 1.f;
    if (v < -1.f) v = -1.f;
    return (int)(((1.0f - v) * (float)A800_PADDLE_CENTRE) + 0.5f);
}

/* A trigger crossing its threshold. Dispatched by poll_sticks once it has
 * let go of the lock: dispatch_button takes it too, and it is not recursive. */
typedef struct {
    SDL_JoystickID id;
    int button;
    int down;
} trigger_edge;

#define MAX_EDGES (MAX_PADS * 2)

static void poll_one(gamepad_state *g, pad_slot *slot, int port,
                     trigger_edge *edges, int *nedges)
{
    struct a800session *s = g->session;
    uint8_t want[A800_ACT_PER_PORT];
    int dev, dir, a;
    float lx, ly;

    memset(want, 0, sizeof want);
    slot_sync_port(g, slot, port);   /* moved, or what is plugged in changed: start again */
    dev = slot->dev;
    if (port < 0) return;   /* a pad with no port (more pads than ports) plays nothing */

    slot->dpad_dir = slot_dpad(g, slot, port);
    slot_stick(slot, 0, &lx, &ly);

    if (dev == A800_DEV_JOYSTICK) {
        dir = slot->dpad_dir;
        if (a800session_get_int(s, "analog_stick", 1)) {
            slot->stick_dir = a800_dir_from_stick(lx, ly, slot->stick_dir, STICK_ENTER, STICK_EXIT);
            dir |= slot->stick_dir;
        }
        want_action(want, A800_ACT_UP, dir & A800_DIR_UP);
        want_action(want, A800_ACT_DOWN, dir & A800_DIR_DOWN);
        want_action(want, A800_ACT_LEFT, dir & A800_DIR_LEFT);
        want_action(want, A800_ACT_RIGHT, dir & A800_DIR_RIGHT);
    }
    else if (dev == A800_DEV_PADDLES && slot->handle) {
        /* the sticks turn the pair's two paddles; South and East fire them. A
         * position is written only when it changes, so a still stick never
         * overrides the Controllers window's knob. */
        float rx, ry;
        int pot[2], fire[2], k;
        slot_stick(slot, 1, &rx, &ry);
        pot[0] = pot_from_axis(lx);
        pot[1] = pot_from_axis(rx);
        fire[0] = SDL_GetGamepadButton(slot->handle, SDL_GAMEPAD_BUTTON_SOUTH) ? 1 : 0;
        fire[1] = SDL_GetGamepadButton(slot->handle, SDL_GAMEPAD_BUTTON_EAST) ? 1 : 0;
        for (k = 0; k < 2; k++) {
            if (pot[k] != slot->sent_pot[k]) {
                slot->sent_pot[k] = pot[k];
                input_pot(s, A800_SRC_STICK(slot->owner), port, k, pot[k]);
            }
            if (fire[k] != slot->sent_fire[k]) {
                slot->sent_fire[k] = fire[k];
                input_pot_fire(s, A800_SRC_PAD(slot->owner), port, k, fire[k]);
            }
        }
    }

    for (a = 0; a < A800_ACT_PER_PORT; a++) {
        if (want[a] != slot->sent[a]) {
            slot->sent[a] = want[a];
            input_press(s, A800_SRC_STICK(slot->owner), A800_TARGET_PORT(port, a), want[a]);
        }
    }

    /* the triggers, as buttons */
    if (slot->handle) {
        int t;
        for (t = 0; t < 2; t++) {
            float v = axis_norm(SDL_GetGamepadAxis(slot->handle,
                t ? SDL_GAMEPAD_AXIS_RIGHT_TRIGGER : SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
            int down = v >= TRIGGER_ON;
            if (down != slot->trigger_down[t]) {
                slot->trigger_down[t] = down;
                if (*nedges < MAX_EDGES) {
                    edges[*nedges].id = slot->id;
                    edges[*nedges].button = t ? A800_PAD_BTN_RIGHT_TRIGGER : A800_PAD_BTN_LEFT_TRIGGER;
                    edges[*nedges].down = down;
                    (*nedges)++;
                }
            }
        }
    }
}

static void poll_sticks(gamepad_state *g)
{
    int i, nedges = 0;
    trigger_edge edges[MAX_EDGES];
    pthread_mutex_lock(&g->lock);
    for (i = 0; i < g->npads; i++)
        poll_one(g, &g->pads[i], effective_port_locked(g, i), edges, &nedges);
    pthread_mutex_unlock(&g->lock);
    for (i = 0; i < nedges; i++)
        dispatch_button(g, edges[i].id, edges[i].button, edges[i].down);
}

/* ---- the thread ------------------------------------------------------------- */

static void *thread_main(void *arg)
{
    gamepad_state *g = arg;
    int count = 0, i;
    /* Every joystick, not just the ones SDL has a gamepad mapping for:
     * SDL_GetGamepads would silently omit a plain HID adapter entirely. */
    SDL_JoystickID *ids = SDL_GetJoysticks(&count);
    if (ids) {
        for (i = 0; i < count; i++) open_pad(g, ids[i]);
        SDL_free(ids);
    }

    while (!g->stop_requested) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_EVENT_JOYSTICK_ADDED: open_pad(g, ev.jdevice.which); break;
            case SDL_EVENT_JOYSTICK_REMOVED: close_pad(g, ev.jdevice.which); break;
            case SDL_EVENT_GAMEPAD_ADDED: open_pad(g, ev.gdevice.which); break;
            case SDL_EVENT_GAMEPAD_REMOVED: close_pad(g, ev.gdevice.which); break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            case SDL_EVENT_GAMEPAD_BUTTON_UP:
                dispatch_button(g, ev.gbutton.which,
                                a800_pad_button_from_sdl(ev.gbutton.button),
                                ev.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
                break;
            case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
            case SDL_EVENT_JOYSTICK_BUTTON_UP: {
                /* a mapped gamepad also emits these for the same press it
                 * reported as a GAMEPAD_BUTTON event; the slot's kind decides */
                int gamepad, idx;
                pthread_mutex_lock(&g->lock);
                idx = slot_for_id(g, ev.jbutton.which);
                gamepad = idx >= 0 && g->pads[idx].handle != NULL;
                pthread_mutex_unlock(&g->lock);
                if (gamepad || ev.jbutton.button >= 64) break;
                dispatch_button(g, ev.jbutton.which,
                                A800_PAD_BTN_RAW_BASE + ev.jbutton.button,
                                ev.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN);
                break;
            }
            case SDL_EVENT_JOYSTICK_HAT_MOTION:
                handle_hat_motion(g, ev.jhat.which, ev.jhat.hat, ev.jhat.value);
                break;
            default:
                break;
            }
        }
        poll_sticks(g);
        /* ~60 Hz, the machine's own sampling rate, with a pad; with none only
         * hot-plug events are waited for. Not SDL_WaitEventTimeout: SDL polls
         * at 1 kHz inside it. */
        SDL_Delay(g->npads > 0 ? 16 : 100);
    }

    pthread_mutex_lock(&g->lock);
    for (i = 0; i < g->npads; i++) slot_close(g, &g->pads[i]);
    g->npads = 0;
    pthread_mutex_unlock(&g->lock);
    return NULL;
}

int gamepad_start(struct a800session *s)
{
    gamepad_state *g;
    if (s->gamepad) return 0;
    /* The app owns its signals; SDL must not intercept SIGINT/SIGTERM. */
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    /* the poll thread is not the focused window: keep reading pads when the
     * app is in the background (the debugger or another window has focus) */
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    /* Bluetooth pads over hidapi and the joystick subsystem together. */
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "1");
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        session_set_error(s, "SDL gamepad init failed: %s", SDL_GetError());
        return -1;
    }
    g = calloc(1, sizeof *g);
    if (!g) { SDL_QuitSubSystem(SDL_INIT_GAMEPAD); return -1; }
    g->session = s;
    pthread_mutex_init(&g->lock, NULL);
    /* the generation carries across a restart, so a frontend's baseline never
     * goes stale; a restarted thread's first scan re-finds the same pads, which
     * is no hot-plug: no toast and no change of generation */
    g->generation = s->pad_gen_base;
    g->quiet_scan = s->pad_restarted;
    /* the remembered assignments, once: garbage or no key is an empty table */
    a800_pad_table_decode(&g->table, a800session_get_str(s, "pad_assign", ""), A800_PORTS);
    /* An explicit stack: the platform default is 512 KB on macOS, and SDL's
     * HID enumeration runs on this thread. */
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 4u << 20);
    if (pthread_create(&g->thread, &attr, thread_main, g) != 0) {
        pthread_attr_destroy(&attr);
        session_set_error(s, "gamepad thread could not start");
        pthread_mutex_destroy(&g->lock);
        free(g);
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
        return -1;
    }
    pthread_attr_destroy(&attr);
    g->running = 1;
    g_state = g;
    s->gamepad = g;
    return 0;
}

void gamepad_stop(struct a800session *s)
{
    gamepad_state *g = s->gamepad;
    if (!g) return;
    g->stop_requested = 1;
    pthread_join(g->thread, NULL);
    s->pad_gen_base = g->generation;
    s->pad_restarted = 1;
    pthread_mutex_destroy(&g->lock);
    s->gamepad = NULL;
    if (g_state == g) g_state = NULL;
    free(g);
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
}

/* ---- public API ------------------------------------------------------------- */

int a800session_gamepad_count(a800session *s)
{
    gamepad_state *g = s->gamepad;
    int n;
    if (!g) return 0;
    pthread_mutex_lock(&g->lock);
    n = g->npads;
    pthread_mutex_unlock(&g->lock);
    return n;
}

int a800session_gamepad_name(a800session *s, int idx, char *dst, int dstsz)
{
    gamepad_state *g = s->gamepad;
    int n = 0;
    if (!dst || dstsz <= 0) return 0;
    dst[0] = '\0';
    if (!g) return 0;
    pthread_mutex_lock(&g->lock);
    if (idx >= 0 && idx < g->npads)
        n = snprintf(dst, (size_t)dstsz, "%s", slot_name(&g->pads[idx]));
    pthread_mutex_unlock(&g->lock);
    return n;
}

void a800session_gamepad_assign(a800session *s, int idx, int port)
{
    gamepad_state *g = s->gamepad;
    char text[A800_PAD_ASSIGN_TEXT_MAX];
    int changed = 0;
    if (!g) return;
    pthread_mutex_lock(&g->lock);
    if (idx >= 0 && idx < g->npads) {
        pad_slot *slot = &g->pads[idx];
        if (port >= 0 && port < A800_PORTS) {
            slot->assign = port;
            slot->by_name = 0;
            changed = a800_pad_table_set(&g->table, slot->guidstr, slot->name, port) > 0;
        } else {
            const char *kguid[MAX_PADS], *kname[MAX_PADS];
            int k, nkeep = 0;
            slot->assign = -1;
            slot->by_name = 0;
            for (k = 0; k < g->npads; k++)
                if (k != idx && g->pads[k].assign >= 0) {
                    kguid[nkeep] = g->pads[k].guidstr;
                    kname[nkeep++] = g->pads[k].name;
                }
            changed = a800_pad_table_remove(&g->table, slot->guidstr, slot->name,
                                            nkeep, kguid, kname);
        }
        if (changed && a800_pad_table_encode(&g->table, text, sizeof text) < 0)
            changed = 0;
    }
    pthread_mutex_unlock(&g->lock);
    /* the settings are written here, on the caller's thread, with the pad lock
     * let go: the pad thread must never wait on the settings store */
    if (changed)
        a800session_set_str(s, "pad_assign", text);
}

int a800session_gamepad_assignment(a800session *s, int idx)
{
    gamepad_state *g = s->gamepad;
    int r = -1;
    if (!g) return -1;
    pthread_mutex_lock(&g->lock);
    if (idx >= 0 && idx < g->npads) r = g->pads[idx].assign;
    pthread_mutex_unlock(&g->lock);
    return r;
}

int a800session_gamepad_effective_port(a800session *s, int idx)
{
    gamepad_state *g = s->gamepad;
    int r = -1;
    if (!g) return -1;
    pthread_mutex_lock(&g->lock);
    if (idx >= 0 && idx < g->npads) r = effective_port_locked(g, idx);
    pthread_mutex_unlock(&g->lock);
    return r;
}

unsigned a800session_gamepad_generation(a800session *s)
{
    gamepad_state *g = s->gamepad;
    unsigned r;
    if (!g) return s->pad_gen_base;
    pthread_mutex_lock(&g->lock);
    r = g->generation;
    pthread_mutex_unlock(&g->lock);
    return r;
}

void a800session_gamepad_capture_begin(a800session *s)
{
    gamepad_state *g = s->gamepad;
    if (!g) return;
    pthread_mutex_lock(&g->lock);
    g->cap_active = 1; g->cap_have = 0; g->cap_result = A800_PAD_BTN_NONE;
    pthread_mutex_unlock(&g->lock);
}

void a800session_gamepad_capture_cancel(a800session *s)
{
    gamepad_state *g = s->gamepad;
    if (!g) return;
    pthread_mutex_lock(&g->lock);
    g->cap_active = 0; g->cap_have = 0;
    pthread_mutex_unlock(&g->lock);
}

int a800session_gamepad_capture_poll(a800session *s, int *button)
{
    gamepad_state *g = s->gamepad;
    int got = 0;
    if (!g) return 0;
    pthread_mutex_lock(&g->lock);
    if (g->cap_active && g->cap_have) {
        if (button) *button = g->cap_result;
        g->cap_active = 0; g->cap_have = 0;
        got = 1;
    }
    pthread_mutex_unlock(&g->lock);
    return got;
}
