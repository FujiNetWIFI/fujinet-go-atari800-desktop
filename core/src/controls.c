/*
 * controls -- the layout of the on-screen controllers, for every toolkit.
 *
 * One table (a800session_controls_layout) says what the Controllers window
 * draws and where, in layout units (1.0 = a keypad key's width) from the
 * top-left; the toolkits draw the art from it and ask a800session_controls_hit
 * what a click landed on, so the shapes cannot drift apart between GTK, Qt,
 * AppKit and Win32. It has a view per controller port, following what is
 * plugged in (joystick, paddles, the mice, the Koala Pad, the touch tablet,
 * the light pen and gun, the CX85 keypad), in a grid (2 x 2 for four ports, a
 * row for two), over a strip of the console keys.
 *
 * The CX40 joystick and the CX30 paddles are the same hardware on the Atari
 * 2600 (they plug into its controller ports too): add_joystick and add_paddles
 * are the 2600 desktop's builders (core/src/controls.c there), with this
 * machine's targets. Geometry is DESIGN-atari.md's own, device-local, in its
 * units (a CX40's case is 8.0 x 8.0, a CX30 paddle 7.0 x 6.0), so a frontend
 * scales one factor for the whole window.
 *
 * Pure: no session, no state. The caller owns the output buffer.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include <string.h>

#include "a800session.h"

/* ---- geometry (layout units) ------------------------------------------------------ */

#define CAPTION_H 0.9f
#define PORT_W   14.8f       /* wide enough for a pair of paddles */
#define PORT_H    8.4f       /* tall enough for a Koala Pad */
#define PORT_GAP  1.0f
#define ROW_GAP   0.6f
#define LAYOUT_W (2.0f * PORT_W + PORT_GAP)
#define CON_KEY_W 2.2f
#define CON_KEY_H 1.1f
#define CON_KEY_GAP 0.5f
#define CON_GAP   0.6f

typedef struct {
    a800_control *out;
    int max;
    int n;
} lay;

static void add(lay *l, int kind, const char *label, const char *legend, int port, int target,
                int arg, int dir, float x, float y, float w, float h)
{
    a800_control *c;
    if (l->n >= l->max) return;
    c = &l->out[l->n++];
    c->kind = kind;
    c->label = label;
    c->legend = legend;
    c->port = port;
    c->target = target;
    c->arg = arg;
    c->dir = dir;
    c->x = x; c->y = y; c->w = w; c->h = h;
}

/* The CX40 joystick, seen from above in an 8.0 x 8.0 case: the round stick
 * region is the boot's ring (centre (4.5, 4.5), r 2.1: press and drag, a quarter
 * of the reach is nothing, then eight 45-degree sectors) and the one red fire
 * button is a round button in the case's top-left corner (centre (1.7, 1.7), r
 * 0.95). (ox, oy) is the case's top-left. */
static void add_joystick(lay *l, int port, float ox, float oy)
{
    add(l, A800_CTL_BODY, "Joystick", NULL, port, -1, 0, 0, ox, oy, 8.0f, 8.0f);
    add(l, A800_CTL_STICK, "STICK", NULL, port, -1, 0, 0, ox + 2.4f, oy + 2.4f, 4.2f, 4.2f);
    add(l, A800_CTL_ROUND_BUTTON, "FIRE", NULL, port,
        A800_TARGET_PORT(port, A800_ACT_FIRE), 0, 0, ox + 0.75f, oy + 0.75f, 1.9f, 1.9f);
}

/* One CX30 paddle in a 7.0 x 6.0 box: the ribbed knob (centre (3.2, 2.9), r 2.2:
 * drag to turn it, the wheel nudges it), the red button on the left edge, and
 * the two small turn arrows under the knob (held = keeps turning). A pair is two
 * of these side by side with a 0.8 gap. */
static void add_paddle(lay *l, int port, int paddle, const char *name, float ox, float oy)
{
    add(l, A800_CTL_BODY, name, NULL, port, -1, paddle, 0, ox, oy, 7.0f, 6.0f);
    add(l, A800_CTL_KNOB, "KNOB", NULL, port, -1, paddle, 0, ox + 1.0f, oy + 0.7f, 4.4f, 4.4f);
    add(l, A800_CTL_PADDLE_FIRE, "FIRE", NULL, port, -1, paddle, 0, ox, oy + 2.2f, 0.9f, 1.6f);
    add(l, A800_CTL_TURN, "<", "turn left", port, -1, paddle, -1, ox + 1.8f, oy + 4.9f, 0.8f, 0.9f);
    add(l, A800_CTL_TURN, ">", "turn right", port, -1, paddle, 1, ox + 2.6f, oy + 4.9f, 0.8f, 0.9f);
}

static void add_paddles(lay *l, int port, float ox, float oy)
{
    add_paddle(l, port, 0, "Paddle A", ox, oy);
    add_paddle(l, port, 1, "Paddle B", ox + 7.8f, oy);
}

/* A mouse in `w` x `h` with its two buttons side by side in the top 40%, and the
 * mat to drag on beside it. */
static void add_mouse(lay *l, int port, const char *name, float w, float h, float ox, float oy)
{
    const float bw = (w - 0.6f) / 2.0f;
    add(l, A800_CTL_BODY, name, NULL, port, -1, 0, 0, ox, oy, w, h);
    add(l, A800_CTL_POINTER_BUTTON, "LEFT", NULL, port, -1, 1, 0, ox + 0.2f, oy + 0.25f, bw, 0.4f * h - 0.35f);
    add(l, A800_CTL_POINTER_BUTTON, "RIGHT", NULL, port, -1, 2, 0, ox + 0.4f + bw, oy + 0.25f, bw, 0.4f * h - 0.35f);
    add(l, A800_CTL_MAT, "MAT", "drag to move the mouse", port, -1, 0, 0, ox + w + 0.8f, oy + 0.4f, 8.8f, 5.6f);
}

/* The CX22 Trak-Ball: an angular 9.0 x 5.0 body, the ball (r 1.5) in the middle,
 * a button at each upper corner. */
static void add_trakball(lay *l, int port, float ox, float oy)
{
    add(l, A800_CTL_BODY, "Trak-Ball", NULL, port, -1, 0, 0, ox, oy, 9.0f, 5.0f);
    add(l, A800_CTL_BALL, "BALL", "drag the ball", port, -1, 0, 0, ox + 3.0f, oy + 1.1f, 3.0f, 3.0f);
    add(l, A800_CTL_POINTER_BUTTON, "LEFT", NULL, port, -1, 1, 0, ox + 0.4f, oy + 0.3f, 1.5f, 1.0f);
    add(l, A800_CTL_POINTER_BUTTON, "RIGHT", NULL, port, -1, 2, 0, ox + 7.1f, oy + 0.3f, 1.5f, 1.0f);
}

/* The Koala Pad: 6.4 x 8.2, two buttons across the top, the 5.6 x 5.6 pad. */
static void add_koala(lay *l, int port, float ox, float oy)
{
    add(l, A800_CTL_BODY, "Koala Pad", NULL, port, -1, 0, 0, ox, oy, 6.4f, 8.2f);
    add(l, A800_CTL_POINTER_BUTTON, "LEFT", NULL, port, -1, 1, 0, ox + 0.5f, oy + 0.3f, 2.4f, 0.9f);
    add(l, A800_CTL_POINTER_BUTTON, "RIGHT", NULL, port, -1, 2, 0, ox + 3.5f, oy + 0.3f, 2.4f, 0.9f);
    add(l, A800_CTL_PAD, "PAD", "touch the pad", port, -1, 0, 0, ox + 0.4f, oy + 1.7f, 5.6f, 5.6f);
}

/* The CX77 touch tablet: 8.6 x 5.2, the 7.0 x 3.6 pad inset. A touch is the
 * pad's own button (bit 1). */
static void add_tablet(lay *l, int port, float ox, float oy)
{
    add(l, A800_CTL_BODY, "Touch tablet", NULL, port, -1, 0, 0, ox, oy, 8.6f, 5.2f);
    add(l, A800_CTL_PAD, "PAD", "touch the pad", port, -1, 1, 0, ox + 0.8f, oy + 1.2f, 7.0f, 3.6f);
}

/* The CX85 numeric keypad: 6.4 x 5.0. Left column ESCAPE NO DELETE YES, a 3 x 4
 * grid 7 8 9 / 4 5 6 / 1 2 3 / 0 (two wide) ., and on the right `-` and a tall
 * `+ ENTER`. */
static void add_cx85(lay *l, int port, float ox, float oy)
{
    static const struct { const char *label; int key; } left[4] = {
        { "ESCAPE", A800_CX85_ESCAPE }, { "NO", A800_CX85_NO },
        { "DELETE", A800_CX85_DELETE }, { "YES", A800_CX85_YES },
    };
    static const struct { const char *label; int key; } grid[3][3] = {
        { { "7", A800_CX85_7 }, { "8", A800_CX85_8 }, { "9", A800_CX85_9 } },
        { { "4", A800_CX85_4 }, { "5", A800_CX85_5 }, { "6", A800_CX85_6 } },
        { { "1", A800_CX85_1 }, { "2", A800_CX85_2 }, { "3", A800_CX85_3 } },
    };
    int r, c;
    add(l, A800_CTL_BODY, "CX85", NULL, port, -1, 0, 0, ox, oy, 6.4f, 5.0f);
    for (r = 0; r < 4; r++)
        add(l, A800_CTL_CX85_KEY, left[r].label, NULL, port, -1, left[r].key, 0,
            ox + 0.2f, oy + 0.4f + (float)r * 1.1f, 1.3f, 0.9f);
    for (r = 0; r < 3; r++)
        for (c = 0; c < 3; c++)
            add(l, A800_CTL_CX85_KEY, grid[r][c].label, NULL, port, -1, grid[r][c].key, 0,
                ox + 1.7f + (float)c * 1.15f, oy + 0.4f + (float)r * 1.1f, 0.95f, 0.95f);
    add(l, A800_CTL_CX85_KEY, "0", NULL, port, -1, A800_CX85_0, 0,
        ox + 1.7f, oy + 0.4f + 3.0f * 1.1f, 2.1f, 0.95f);
    add(l, A800_CTL_CX85_KEY, ".", NULL, port, -1, A800_CX85_PERIOD, 0,
        ox + 1.7f + 2.0f * 1.15f, oy + 0.4f + 3.0f * 1.1f, 0.95f, 0.95f);
    add(l, A800_CTL_CX85_KEY, "-", NULL, port, -1, A800_CX85_MINUS, 0,
        ox + 5.2f, oy + 0.4f, 0.95f, 0.95f);
    add(l, A800_CTL_CX85_KEY, "+ ENTER", NULL, port, -1, A800_CX85_PLUS_ENTER, 0,
        ox + 5.2f, oy + 1.5f, 0.95f, 2.0f);
}

static const char *const port_names[A800_PORTS] = { "Port 1", "Port 2", "Port 3", "Port 4" };

/* One port's cell: a caption over the device, centred in PORT_W x PORT_H. */
static void add_port(lay *l, int port, int dev, float x0, float y0)
{
    const float cy = y0 + CAPTION_H;
    add(l, A800_CTL_CAPTION, port_names[port], a800_device_name(dev), port, -1, 0, 0,
        x0, y0, PORT_W, CAPTION_H);
    switch (dev) {
    case A800_DEV_JOYSTICK:
        add_joystick(l, port, x0 + (PORT_W - 8.0f) / 2.0f, cy + 0.2f);
        break;
    case A800_DEV_PADDLES:
        add_paddles(l, port, x0, cy + 1.2f);
        break;
    case A800_DEV_ST_MOUSE:
        add_mouse(l, port, "Atari ST mouse", 4.0f, 6.0f, x0 + 0.6f, cy + 0.8f);
        break;
    case A800_DEV_AMIGA_MOUSE:
        add_mouse(l, port, "Amiga mouse", 4.0f, 6.4f, x0 + 0.6f, cy + 0.6f);
        break;
    case A800_DEV_TRAKBALL:
        add_trakball(l, port, x0 + (PORT_W - 9.0f) / 2.0f, cy + 1.7f);
        break;
    case A800_DEV_KOALA:
        add_koala(l, port, x0 + (PORT_W - 6.4f) / 2.0f, cy + 0.1f);
        break;
    case A800_DEV_TOUCH_TABLET:
        add_tablet(l, port, x0 + (PORT_W - 8.6f) / 2.0f, cy + 1.6f);
        break;
    case A800_DEV_LIGHTPEN:
        add(l, A800_CTL_BODY, "Light pen", NULL, port, -1, 0, 0, x0 + 4.0f, cy + 0.2f, 1.0f, 8.0f);
        add(l, A800_CTL_NOTE, "Aim on the picture", NULL, port, -1, 0, 0, x0 + 5.6f, cy + 3.4f, 8.0f, 1.2f);
        break;
    case A800_DEV_LIGHTGUN:
        add(l, A800_CTL_BODY, "Light gun", NULL, port, -1, 0, 0, x0 + 0.8f, cy + 0.6f, 8.5f, 7.0f);
        add(l, A800_CTL_NOTE, "Aim on the picture", NULL, port, -1, 0, 0, x0 + 9.5f, cy + 3.4f, 5.0f, 1.2f);
        break;
    case A800_DEV_CX85:
        add_cx85(l, port, x0 + (PORT_W - 6.4f) / 2.0f, cy + 1.7f);
        break;
    default:
        add(l, A800_CTL_BODY, "Nothing", NULL, port, -1, 0, 0, x0 + (PORT_W - 3.0f) / 2.0f, cy + 3.7f, 3.0f, 1.0f);
        break;
    }
}

int a800session_controls_layout(const int *devices, int nports, int xl,
                                a800_control *out, int max, float *width, float *height)
{
    static const int con_order[6] = { A800_CON_START, A800_CON_SELECT, A800_CON_OPTION,
                                      A800_CON_HELP, A800_CON_RESET, A800_CON_BREAK };
    static const char *const con_labels[A800_CON_COUNT] = {
        "START", "SELECT", "OPTION", "HELP", "RESET", "BREAK",
    };
    lay l;
    int p, k, rows, ncon = 0;
    float y, sw, sx;

    l.out = out;
    l.max = out ? max : 0;
    l.n = 0;
    if (nports < 1) nports = 1;
    if (nports > A800_PORTS) nports = A800_PORTS;
    rows = (nports + 1) / 2;

    for (p = 0; p < nports; p++) {
        const int dev = devices && devices[p] >= 0 && devices[p] < A800_DEV_COUNT
                            ? devices[p] : A800_DEV_NONE;
        add_port(&l, p, dev, (float)(p % 2) * (PORT_W + PORT_GAP),
                 (float)(p / 2) * (CAPTION_H + PORT_H + ROW_GAP));
    }

    y = (float)rows * (CAPTION_H + PORT_H) + (float)(rows - 1) * ROW_GAP + CON_GAP;
    for (k = 0; k < 6; k++)
        if (con_order[k] != A800_CON_HELP || xl) ncon++;
    sw = (float)ncon * CON_KEY_W + (float)(ncon - 1) * CON_KEY_GAP;
    sx = (LAYOUT_W - sw) / 2.0f;
    add(&l, A800_CTL_BODY, "Console", NULL, -1, -1, 0, 0, sx - 0.4f, y - 0.2f, sw + 0.8f, CON_KEY_H + 0.4f);
    for (k = 0; k < 6; k++) {
        const int con = con_order[k];
        if (con == A800_CON_HELP && !xl) continue;
        add(&l, A800_CTL_CONSOLE_KEY, con_labels[con], NULL, -1, A800_TARGET_CONSOLE(con), 0, 0,
            sx, y, CON_KEY_W, CON_KEY_H);
        sx += CON_KEY_W + CON_KEY_GAP;
    }

    if (width) *width = LAYOUT_W;
    if (height) *height = y + CON_KEY_H + 0.4f;
    return l.n;
}

/* The shapes round parts have: an ellipse inside the box. Everything else is its
 * rectangle. */
static int is_round(int kind)
{
    return kind == A800_CTL_STICK || kind == A800_CTL_KNOB || kind == A800_CTL_BALL
        || kind == A800_CTL_ROUND_BUTTON;
}

int a800session_controls_hit(const a800_control *c, int n, float x, float y)
{
    int i;
    if (!c) return -1;
    /* the topmost: later entries are drawn over earlier ones */
    for (i = n - 1; i >= 0; i--) {
        if (c[i].kind == A800_CTL_BODY || c[i].kind == A800_CTL_CAPTION || c[i].kind == A800_CTL_NOTE)
            continue;
        if (x < c[i].x || x >= c[i].x + c[i].w || y < c[i].y || y >= c[i].y + c[i].h)
            continue;
        if (is_round(c[i].kind)) {
            const float rx = c[i].w * 0.5f, ry = c[i].h * 0.5f;
            const float dx = (x - (c[i].x + rx)) / rx, dy = (y - (c[i].y + ry)) / ry;
            if (dx * dx + dy * dy > 1.0f) continue;
        }
        return i;
    }
    return -1;
}

/* ---- dragging ---------------------------------------------------------------------- */

unsigned a800session_stick_from_vector(float dx, float dy, float radius)
{
    const float ax = fabsf(dx), ay = fabsf(dy);
    unsigned mask = 0;
    /* tan(22.5 degrees): the edge of a 45-degree sector */
    const float edge = 0.41421356f;

    if (radius <= 0.f || ax * ax + ay * ay < (0.25f * radius) * (0.25f * radius))
        return 0;
    if (ay <= ax * edge)       mask = dx < 0.f ? 1u << A800_ACT_LEFT : 1u << A800_ACT_RIGHT;
    else if (ax <= ay * edge)  mask = dy < 0.f ? 1u << A800_ACT_UP : 1u << A800_ACT_DOWN;
    else {
        mask = dx < 0.f ? 1u << A800_ACT_LEFT : 1u << A800_ACT_RIGHT;
        mask |= dy < 0.f ? 1u << A800_ACT_UP : 1u << A800_ACT_DOWN;
    }
    return mask;
}

float a800session_vector_angle(float dx, float dy)
{
    return atan2f(dx, -dy) * (180.0f / 3.14159265358979f);
}

/* Clockwise from straight up is to the right: a lower POT value. */
int a800session_knob_from_vector(float dx, float dy)
{
    float deg, value;
    if (dx == 0.f && dy == 0.f) return A800_PADDLE_CENTRE;
    deg = a800session_vector_angle(dx, dy);
    value = deg / A800_KNOB_TRAVEL_DEG;
    if (value < -1.f) value = -1.f;
    if (value > 1.f) value = 1.f;
    return (int)lroundf((1.0f - value) * (float)A800_PADDLE_CENTRE);
}

float a800session_knob_angle(int value)
{
    float v = (1.0f - (float)value / (float)A800_PADDLE_CENTRE);
    if (v < -1.f) v = -1.f;
    if (v > 1.f) v = 1.f;
    return v * A800_KNOB_TRAVEL_DEG;
}
