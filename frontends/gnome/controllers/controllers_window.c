/*
 * The Controllers window -- the Atari's controller ports drawn as a player sees
 * them: each port shows what is plugged into it (the CX40 joystick, a pair of
 * CX30 paddles, an ST or Amiga mouse, the Trak-Ball, the Koala Pad, the touch
 * tablet, the light pen or gun, the CX85 keypad) over a strip of the console
 * keys, and below them, when it is shown, the machine's own keyboard (the 400's
 * membrane, the 800's brown keys, the XL/XE's charcoal ones). The ports and the
 * console are drawn from the session's layout (a800session_controls_layout), in
 * the layout's units, and the keyboard from its table (a800session_keyboard_layout),
 * so every frontend shows the same controls in the same places. The CX40 joystick
 * and the CX30 paddles are the same drawing as the Atari 2600 window's
 * (DESIGN-atari.md).
 *
 * Mouse: a press on a button or key holds it; a press on the joystick's boot tilts
 * the stick toward the pointer (the cap follows, eight directions) and a drag
 * follows; a press on a paddle's knob turns it to the pointer's angle and a drag
 * follows (the scroll wheel nudges it; a paddle stays where it was turned); a mouse
 * or the Trak-Ball's ball moves as the pointer is dragged over its mat; the Koala
 * Pad and the touch tablet read the point touched; the CX85's keys are held while
 * pressed. A mouse button can be latched with the right button, to drag with it
 * down. Every press is the raw press and release (a GtkGestureDrag, not a click: a
 * click gives up when the pointer wobbles), because the Atari samples its ports
 * and its keyboard once per frame. SHIFT and CTRL latch for the next key (the
 * session does that: a mouse cannot hold two keys at once). Losing the focus,
 * hiding and the pointer leaving a held button let go of whatever the mouse holds.
 *
 * What the Atari sees held -- host keyboard, gamepads and this window together
 * (a800session_buttons_held, console_held, keyboard_held, paddle_value) -- is
 * polled every frame and painted in the held orange, so the window doubles as a
 * gamepad tester.
 *
 * Keys typed with the window active go through the main window (a800_window_key),
 * so the Atari, F9 and the shortcuts behave the same whichever window has the
 * focus.
 *
 * FIXED SIZE, FLOATING: the labels ellipsize rather than grow and the window is
 * not resizable, so neither Map mode nor a long gamepad name changes its size
 * and tiling compositors float it. Its size follows only what it shows: the
 * number of ports (two on an XL/XE, four on a 400/800), and the Keyboard check
 * box. It is hidden at startup (F9 or the menu shows it). SINGLETON, hidden
 * rather than destroyed, so a remap survives closing it. Its timers run only
 * while it is on screen.
 *
 * MAP MODE: the Map button arms a two-step rebind -- click a control to pick the
 * target (the four quarters of the joystick's boot are its four directions; the
 * console keys, on the strip or on the keyboard, are targets too), or choose one
 * from the list, then press a keyboard key OR a gamepad button. While armed the
 * same gestures pick the target instead of pressing, every bindable control shows
 * the key bound to it, and typed keys are not typed into the Atari; once a target
 * is picked the other controls are greyed. Rebinding steals the input from
 * whatever held it. Escape cancels. F9, F11 and F12 belong to the app and cannot
 * be bound. Clear takes the picked target's key and gamepad button away, Defaults
 * puts every binding back.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "controllers_window.h"

#include "../help/help_window.h"
#include "../window.h"

#include <math.h>
#include <pango/pangocairo.h>
#include <string.h>

#define UNIT 24.0               /* pixels per layout unit (ports and console) */
#define KBD_UNIT 46.0           /* pixels per key unit (the keyboard) */
#define KBD_GAP 14.0            /* between the ports and the keyboard, pixels */
#define MARGIN 14.0             /* the desk round everything, pixels */
#define MAX_PADS 8              /* pads listed (SDL opens few more) */
#define PAD_ROWS 2              /* pad rows the window reserves; more scroll */
#define PAD_ROW_H 34            /* one row's height, px */
#define PAD_GAP 4
#define KNOB_NUDGE 8            /* one scroll step turns a paddle knob by this much */
#define MAX_KEYS 128
#define TICKS 36                /* the joystick ring's tick marks */
#define U(v) ((v) * UNIT)

/* the colours (DESIGN-atari.md): plastic black, fire red, the held orange */
#define COL_DESK_TOP 0x34302B
#define COL_DESK_BOT 0x211E1B
#define COL_DESK_EDGE 0x0E0C0A
#define COL_BLACK_TOP 0x262626
#define COL_BLACK_BOT 0x171717
#define COL_BLACK_HI 0x2E2E2E
#define COL_BLACK_LO 0x101010
#define COL_RED 0xE23B1F
#define COL_RED_HI 0xF05A38
#define COL_RED_DOWN 0xB02A14
#define COL_HELD 0xFFB04A
#define COL_SILVER 0xC9C9C9
#define COL_PIN 0xE58A1C
#define COL_CAPTION 0xD9D2C4

/* ---- singleton state ------------------------------------------------------ */

static GtkWidget *g_window;
static GtkWindow *g_parent;     /* the A800Window: typed keys go through it */
static a800session *g_session;
static GtkWidget *g_area;       /* the controllers' drawing area */

/* what the layout and the keyboard were built for */
static a800_control g_lay[A800_CONTROLS_MAX];
static int g_n;
static float g_lw, g_lh;
static int g_dev[A800_PORTS] = { -1, -1, -1, -1 };
static int g_nports;
static int g_xl = -1;
static int g_machine = -1;
static a800_kbd_key g_keys[MAX_KEYS];
static int g_nkeys;
static float g_kw, g_kh;
static int g_family;
static gboolean g_show_kbd;

/* what the Atari sees held (polled every frame) */
static unsigned g_held[A800_PORTS];     /* bit per a800_action */
static unsigned g_con_held;             /* bit per a800_console */
static guchar g_key_held[MAX_KEYS];     /* by position in g_keys */
static int g_paddle[A800_PORTS][2];     /* paddle values, 228 = full left .. 0 = full right */

/* what the pointer holds on a mouse-type device, per port: bit 1 left / trigger,
 * bit 2 right; a latched button stays down until it is clicked again */
static unsigned g_ptr_held[A800_PORTS];
static unsigned g_ptr_latched[A800_PORTS];
static double g_pad_u, g_pad_v;         /* the last point touched on a pad */
static gboolean g_pad_touch;

static gboolean g_syncing;      /* a refresh, not the user, is moving a widget */

/* What the pointer is over / holds. */
typedef struct {
    int idx;                    /* layout index, or -1 */
    int key;                    /* keyboard table position, or -1 */
    int target;                 /* what Map binds, or -1 (the stick: its quarter) */
} Hit;
static Hit g_hover = { -1, -1, -1 };
static Hit g_press = { -1, -1, -1 };
static double g_ptr_x, g_ptr_y; /* the pointer, for the scroll wheel */

/* What the mouse is dragging: the stick, a paddle's knob, a mouse mat or ball,
 * or a pad's surface. */
static struct {
    int kind;                   /* 0 none, 1 stick, 2 knob, 3 mat or ball, 4 pad */
    int port, paddle, idx;
    double cenx, ceny, rad;     /* the dial, pixels */
    double lastx, lasty;        /* the last pointer position, for deltas */
    double carryx, carryy;      /* pixels not yet a whole count */
} g_drag;

/* Map mode: -2 idle, -1 armed and waiting for a target, >= 0 waiting for a
 * key or pad button to bind to that target. */
static int g_map_state = -2;
static GtkWidget *g_map_button, *g_clear_button, *g_map_hint, *g_show_bindings, *g_kbd_check;
static GtkWidget *g_target_drop;
static GtkWidget *g_pad_box;
static GtkWidget *g_pad_eff[MAX_PADS];  /* each pad's "plays ..." */
static int g_pad_row_count;
static unsigned g_pad_generation = (unsigned)-1;
static guint g_capture_timer;
static guint g_swallow_code;    /* the key Map took: its repeats and release too */
static guint g_info_timer;

static void refresh_all(void);
static void set_map_state(int state);

/* ---- settings ------------------------------------------------------------------ */

/* "controllers_show_bindings", the same key the family's other windows use. */
static gboolean show_bindings_saved(void)
{
    return a800session_get_int(g_session, "controllers_show_bindings", 0) > 0;
}

/* "controllers_keyboard": the keyboard under the ports. Unset: shown for a
 * two-port machine (an XL/XE), hidden for the four-port 400/800 whose ports
 * already fill most of a screen. */
static gboolean keyboard_saved(void)
{
    const int saved = a800session_get_int(g_session, "controllers_keyboard", -1);
    if (saved >= 0) return saved > 0;
    return g_nports <= 2;
}

/* ---- small helpers ---------------------------------------------------------- */

static void rgb(cairo_t *cctx, guint32 col)
{
    cairo_set_source_rgb(cctx, ((col >> 16) & 0xFF) / 255.0, ((col >> 8) & 0xFF) / 255.0,
                         (col & 0xFF) / 255.0);
}

static void rgba(cairo_t *cctx, guint32 col, double alpha)
{
    cairo_set_source_rgba(cctx, ((col >> 16) & 0xFF) / 255.0, ((col >> 8) & 0xFF) / 255.0,
                          (col & 0xFF) / 255.0, alpha);
}

static void rrect(cairo_t *cctx, double xpos, double ypos, double wid, double hgt, double rad)
{
    if (rad > wid / 2) rad = wid / 2;
    if (rad > hgt / 2) rad = hgt / 2;
    cairo_new_sub_path(cctx);
    cairo_arc(cctx, xpos + wid - rad, ypos + rad, rad, -G_PI / 2, 0);
    cairo_arc(cctx, xpos + wid - rad, ypos + hgt - rad, rad, 0, G_PI / 2);
    cairo_arc(cctx, xpos + rad, ypos + hgt - rad, rad, G_PI / 2, G_PI);
    cairo_arc(cctx, xpos + rad, ypos + rad, rad, G_PI, 3 * G_PI / 2);
    cairo_close_path(cctx);
}

static void circle(cairo_t *cctx, double cenx, double ceny, double rad)
{
    cairo_new_sub_path(cctx);
    cairo_arc(cctx, cenx, ceny, rad, 0, 2 * G_PI);
}

static void stop_rgb(cairo_pattern_t *pat, double offset, guint32 col)
{
    cairo_pattern_add_color_stop_rgb(pat, offset, ((col >> 16) & 0xFF) / 255.0,
                                     ((col >> 8) & 0xFF) / 255.0, (col & 0xFF) / 255.0);
}

/* A vertical two-colour gradient over a box, set as the source. */
static void vgrad(cairo_t *cctx, double ypos, double hgt, guint32 top, guint32 bot)
{
    cairo_pattern_t *grad = cairo_pattern_create_linear(0, ypos, 0, ypos + hgt);
    stop_rgb(grad, 0, top);
    stop_rgb(grad, 1, bot);
    cairo_set_source(cctx, grad);
    cairo_pattern_destroy(grad);
}

/* Text in a box, in the current units (the drawing is scaled, so a size is in
 * the current units). align -1 left, 0 centre, 1 right. */
static void txt(cairo_t *cctx, GtkWidget *widget, const char *text, double size, gboolean bold,
                gboolean italic, guint32 colour, double xpos, double ypos, double boxw,
                double boxh, int align)
{
    PangoLayout *lay = gtk_widget_create_pango_layout(widget, text);
    PangoContext *pctx = pango_layout_get_context(lay);
    cairo_font_options_t *fopt = cairo_font_options_create();
    PangoFontDescription *fdesc = pango_font_description_new();
    PangoRectangle logical;
    double offx;

    /* smooth at any scale: no pixel hinting on a scaled drawing */
    cairo_font_options_set_hint_metrics(fopt, CAIRO_HINT_METRICS_OFF);
    cairo_font_options_set_hint_style(fopt, CAIRO_HINT_STYLE_NONE);
    pango_cairo_context_set_font_options(pctx, fopt);
    cairo_font_options_destroy(fopt);
    pango_font_description_set_family(fdesc, "Sans");
    pango_font_description_set_weight(fdesc, bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
    pango_font_description_set_style(fdesc, italic ? PANGO_STYLE_ITALIC : PANGO_STYLE_NORMAL);
    pango_font_description_set_absolute_size(fdesc, size * PANGO_SCALE);
    pango_layout_set_font_description(lay, fdesc);
    pango_cairo_update_layout(cctx, lay);
    pango_layout_get_extents(lay, NULL, &logical);
    offx = align < 0 ? 0 : align > 0 ? boxw - logical.width / (double)PANGO_SCALE
                                     : (boxw - logical.width / (double)PANGO_SCALE) / 2;
    rgb(cctx, colour);
    cairo_move_to(cctx, xpos + offx, ypos + (boxh - logical.height / (double)PANGO_SCALE) / 2);
    pango_cairo_show_layout(cctx, lay);
    cairo_new_path(cctx);
    pango_font_description_free(fdesc);
    g_object_unref(lay);
}

/* The width of a text at a size, in the same units. */
static double txt_width(GtkWidget *widget, const char *text, double size, gboolean bold)
{
    PangoLayout *lay = gtk_widget_create_pango_layout(widget, text);
    PangoFontDescription *fdesc = pango_font_description_new();
    PangoRectangle logical;
    double wid;
    pango_font_description_set_family(fdesc, "Sans");
    pango_font_description_set_weight(fdesc, bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
    pango_font_description_set_absolute_size(fdesc, size * PANGO_SCALE);
    pango_layout_set_font_description(lay, fdesc);
    pango_layout_get_extents(lay, NULL, &logical);
    wid = logical.width / (double)PANGO_SCALE;
    pango_font_description_free(fdesc);
    g_object_unref(lay);
    return wid;
}

/* A host key's name as a small legend: the long ones shortened to fit. */
static void legend_name(guint32 keysym, char *dst, int dstsz)
{
    char name[32];
    a800session_keysym_name(keysym, name, sizeof name);
    if (!strcmp(name, "Keypad Enter"))
        g_snprintf(dst, dstsz, "KP Ent");
    else if (g_str_has_prefix(name, "Keypad "))
        g_snprintf(dst, dstsz, "KP %s", name + 7);
    else if (g_str_has_prefix(name, "Left "))
        g_snprintf(dst, dstsz, "L %s", name + 5);
    else if (g_str_has_prefix(name, "Right "))
        g_snprintf(dst, dstsz, "R %s", name + 6);
    else if (!strcmp(name, "Backspace"))
        g_snprintf(dst, dstsz, "BkSp");
    else if (!strcmp(name, "Escape"))
        g_snprintf(dst, dstsz, "Esc");
    else
        g_snprintf(dst, dstsz, "%s", name);
}

static gboolean showing_bindings(void)
{
    return g_map_state != -2 ||
           gtk_check_button_get_active(GTK_CHECK_BUTTON(g_show_bindings));
}

/* The binding legend for a target, or "" when none or not shown. */
static void bound_text(int target, char *dst, int dstsz)
{
    a800_binding bind;
    dst[0] = '\0';
    if (target < 0 || !showing_bindings()) return;
    bind = a800session_binding_get(g_session, target);
    if (bind.keysym)
        legend_name(bind.keysym, dst, dstsz);
    else
        g_snprintf(dst, dstsz, "\xe2\x80\x94");
}

/* ---- geometry ---------------------------------------------------------------- */

static int machine_now(void)
{
    const int mach = a800session_get_int(g_session, "machine", A800_M_130XE);
    return mach >= 0 && mach < A800_M_COUNT ? mach : A800_M_130XE;
}

static double kbd_height(void) { return g_show_kbd && g_nkeys ? KBD_GAP + g_kh * KBD_UNIT : 0; }
static int area_width(void)
{
    const double ports = 2 * MARGIN + (g_lw > 0 ? g_lw : 30.6) * UNIT;
    const double kbd = g_show_kbd && g_nkeys ? 2 * MARGIN + g_kw * KBD_UNIT + 20 : 0;
    return (int)((ports > kbd ? ports : kbd) + 0.5);
}
static int area_height(void)
{
    return (int)(2 * MARGIN + (g_lh > 0 ? g_lh : 11.4) * UNIT + kbd_height() + 0.5);
}

static double ports_x0(void) { return (area_width() - (g_lw > 0 ? g_lw : 30.6) * UNIT) / 2.0; }
static double part_x(const a800_control *ctl) { return ports_x0() + U(ctl->x); }
static double part_y(const a800_control *ctl) { return MARGIN + U(ctl->y); }
static double kbd_x0(void) { return (area_width() - g_kw * KBD_UNIT) / 2.0; }
static double kbd_y0(void) { return MARGIN + U(g_lh) + KBD_GAP; }

static void size_area(void)
{
    if (!g_area) return;
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(g_area), area_width());
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(g_area), area_height());
}

/* Loads the layout and the keyboard for the machine and what is plugged into its
 * ports; TRUE when anything changed. */
static gboolean reload_layout(void)
{
    const int mach = machine_now();
    const int nports = a800session_active_ports(g_session);
    const int xl = a800_machine_is_xl(mach) ? 1 : 0;
    int dev[A800_PORTS], port;
    gboolean same = g_n > 0 && nports == g_nports && xl == g_xl && mach == g_machine;

    for (port = 0; port < A800_PORTS; port++) {
        dev[port] = port < nports ? a800session_port_device(g_session, port) : -1;
        if (dev[port] != g_dev[port]) same = FALSE;
    }
    if (same)
        return FALSE;
    g_nports = nports;
    g_xl = xl;
    g_machine = mach;
    memcpy(g_dev, dev, sizeof g_dev);
    g_n = a800session_controls_layout(dev, nports, xl, g_lay, A800_CONTROLS_MAX, &g_lw, &g_lh);
    {
        const a800_kbd_key *keys = NULL;
        int idx;
        g_nkeys = a800session_keyboard_layout(g_session, &keys);
        if (g_nkeys > MAX_KEYS) g_nkeys = MAX_KEYS;
        g_kw = g_kh = 0;
        for (idx = 0; idx < g_nkeys; idx++) {
            g_keys[idx] = keys[idx];
            if (keys[idx].x + keys[idx].w > g_kw) g_kw = keys[idx].x + keys[idx].w;
            if (keys[idx].y + keys[idx].h > g_kh) g_kh = keys[idx].y + keys[idx].h;
        }
        g_family = a800session_keyboard_family(g_session);
    }
    return TRUE;
}

static gboolean is_console_target(int target)
{
    return target >= A800_TARGET_CONSOLE(0) && target < A800_TARGET_SYSACT(0);
}

/* The stick's target under a pointer: its four quarters are the four directions
 * (a diagonal belongs to the nearer axis). */
static int stick_target(const a800_control *ctl, double xpos, double ypos)
{
    const double dx = xpos - (part_x(ctl) + U(ctl->w) / 2);
    const double dy = ypos - (part_y(ctl) + U(ctl->h) / 2);
    int act;
    if (fabs(dx) > fabs(dy))
        act = dx < 0 ? A800_ACT_LEFT : A800_ACT_RIGHT;
    else
        act = dy < 0 ? A800_ACT_UP : A800_ACT_DOWN;
    return A800_TARGET_PORT(ctl->port, act);
}

static int key_target(const a800_kbd_key *key)
{
    return key->console >= 0 ? A800_TARGET_CONSOLE(key->console) : -1;
}

static Hit hit_at(double xpos, double ypos)
{
    Hit hit = { -1, -1, -1 };
    int idx;
    if (g_n > 0) {
        idx = a800session_controls_hit(g_lay, g_n, (float)((xpos - ports_x0()) / UNIT),
                                       (float)((ypos - MARGIN) / UNIT));
        if (idx >= 0 && idx < g_n) {
            hit.idx = idx;
            hit.target = g_lay[idx].target;
            if (g_lay[idx].kind == A800_CTL_STICK)
                hit.target = stick_target(&g_lay[idx], xpos, ypos);
            return hit;
        }
    }
    if (g_show_kbd) {
        const double kx = xpos - kbd_x0(), ky = ypos - kbd_y0();
        for (idx = g_nkeys - 1; idx >= 0; idx--) {
            const a800_kbd_key *key = &g_keys[idx];
            if (kx >= key->x * KBD_UNIT && kx < (key->x + key->w) * KBD_UNIT &&
                ky >= key->y * KBD_UNIT && ky < (key->y + key->h) * KBD_UNIT) {
                hit.key = idx;
                hit.target = key_target(key);
                return hit;
            }
        }
    }
    return hit;
}

static gboolean same_hit(const Hit *aa, const Hit *bb)
{
    return aa->idx == bb->idx && aa->key == bb->key && aa->target == bb->target;
}

/* Is this part lit: the Atari sees its target held (or it is the one pressed)? */
static gboolean part_lit(int idx)
{
    const a800_control *ctl = &g_lay[idx];
    if (g_press.idx == idx && g_press.key < 0 && g_drag.kind == 0)
        return TRUE;
    if (ctl->kind == A800_CTL_POINTER_BUTTON)
        return ctl->port >= 0 && ((g_ptr_held[ctl->port] | g_ptr_latched[ctl->port]) & (unsigned)ctl->arg);
    if (ctl->target < 0) return FALSE;
    if (ctl->target < A800_PORTS * A800_ACT_PER_PORT)
        return (g_held[ctl->target / A800_ACT_PER_PORT] >> (ctl->target % A800_ACT_PER_PORT)) & 1u;
    if (is_console_target(ctl->target))
        return (g_con_held >> (ctl->target - A800_TARGET_CONSOLE(0))) & 1u;
    return FALSE;
}

/* Greyed while Map waits for a key for some other target. */
static gboolean dimmed(int idx)
{
    const a800_control *ctl = &g_lay[idx];
    if (g_map_state < 0) return FALSE;
    if (ctl->kind == A800_CTL_STICK) {
        const int base = A800_TARGET_PORT(ctl->port, 0);
        return g_map_state < base + A800_ACT_UP || g_map_state > base + A800_ACT_RIGHT;
    }
    if (ctl->kind == A800_CTL_BODY || ctl->kind == A800_CTL_CAPTION ||
        ctl->kind == A800_CTL_NOTE)
        return FALSE;
    return ctl->target != g_map_state;
}

/* A keyboard cap greyed in Map mode: only the console keys can be remapped. */
static gboolean key_dimmed(int idx)
{
    const int target = key_target(&g_keys[idx]);
    if (g_map_state == -2) return FALSE;
    if (g_map_state == -1) return target < 0;
    return target != g_map_state;
}

/* ---- drawing: the joystick ---------------------------------------------------- */

/* The case of the CX40 (DESIGN-atari.md section 2): a textured black square, the
 * fire button's teardrop recess, and the ring of tick marks round the boot. The
 * held directions light the ticks nearest them. */
static void draw_joy_case(cairo_t *cctx, GtkWidget *widget, int port)
{
    const unsigned held = g_held[port];
    const int dirx = (int)((held >> A800_ACT_RIGHT) & 1u) - (int)((held >> A800_ACT_LEFT) & 1u);
    const int diry = (int)((held >> A800_ACT_DOWN) & 1u) - (int)((held >> A800_ACT_UP) & 1u);
    const gboolean any = dirx || diry;
    const double want = any ? atan2((double)dirx, (double)-diry) * 180.0 / G_PI : 0;
    int tick;

    vgrad(cctx, 0, 8, COL_BLACK_TOP, COL_BLACK_BOT);
    rrect(cctx, 0, 0, 8, 8, 0.9);
    cairo_fill_preserve(cctx);
    rgb(cctx, 0x0B0B0B);
    cairo_set_line_width(cctx, 0.06);
    cairo_stroke(cctx);
    rgb(cctx, COL_BLACK_HI);
    rrect(cctx, 0.25, 0.25, 7.5, 7.5, 0.7);
    cairo_set_line_width(cctx, 0.07);
    cairo_stroke(cctx);

    /* the teardrop: the fire button's recess and the neck down to the ring */
    rgb(cctx, COL_BLACK_LO);
    cairo_set_line_width(cctx, 0.9);
    cairo_set_line_cap(cctx, CAIRO_LINE_CAP_BUTT);
    cairo_move_to(cctx, 1.7, 1.7);
    cairo_line_to(cctx, 4.5, 4.5);
    cairo_stroke(cctx);
    circle(cctx, 1.7, 1.7, 1.1);
    cairo_fill(cctx);

    /* the ring and its ticks */
    circle(cctx, 4.5, 4.5, 3.0);
    rgb(cctx, COL_BLACK_LO);
    cairo_fill_preserve(cctx);
    rgb(cctx, 0x050505);
    cairo_set_line_width(cctx, 0.05);
    cairo_stroke(cctx);
    cairo_set_line_cap(cctx, CAIRO_LINE_CAP_ROUND);
    for (tick = 0; tick < TICKS; tick++) {
        const double ang = tick * 360.0 / TICKS;
        double diff = fabs(fmod(ang - want + 540.0, 360.0) - 180.0);
        const gboolean lit = any && diff <= 25.0;
        const double sinv = sin(ang * G_PI / 180.0), cosv = -cos(ang * G_PI / 180.0);
        if (lit) rgb(cctx, COL_HELD); else rgb(cctx, 0x3A3A3A);
        cairo_set_line_width(cctx, lit ? 0.14 : 0.07);
        cairo_move_to(cctx, 4.5 + 2.55 * sinv, 4.5 + 2.55 * cosv);
        cairo_line_to(cctx, 4.5 + 2.85 * sinv, 4.5 + 2.85 * cosv);
        cairo_stroke(cctx);
    }
    txt(cctx, widget, "TOP", 0.3, TRUE, FALSE, 0x2E2E2E, 3.5, 1.6, 2.0, 0.4, 0);
}

/* The boot and the shaft's cap, which slides toward the held direction (up to
 * one unit; at a diagonal the same). The part's box is 4.2 square, centred on
 * the ring. */
static void draw_stick(cairo_t *cctx, const a800_control *ctl)
{
    const unsigned held = g_held[ctl->port];
    const double dirx = (double)((held >> A800_ACT_RIGHT) & 1u) - (double)((held >> A800_ACT_LEFT) & 1u);
    const double diry = (double)((held >> A800_ACT_DOWN) & 1u) - (double)((held >> A800_ACT_UP) & 1u);
    const double len = sqrt(dirx * dirx + diry * diry);
    const double vecx = len > 0 ? dirx / len : 0, vecy = len > 0 ? diry / len : 0;
    const double cenx = ctl->w / 2, ceny = ctl->h / 2;
    int idx;

    /* the ribbed boot: concentric rings, each leaning further toward the tilt */
    static const double k_rad[3] = { 2.1, 1.7, 1.3 };
    static const guint32 k_col[3] = { 0x151515, 0x262626, 0x151515 };
    for (idx = 0; idx < 3; idx++) {
        circle(cctx, cenx + vecx * 0.35 * idx, ceny + vecy * 0.35 * idx, k_rad[idx]);
        rgb(cctx, k_col[idx]);
        cairo_fill_preserve(cctx);
        rgb(cctx, 0x0A0A0A);
        cairo_set_line_width(cctx, 0.04);
        cairo_stroke(cctx);
    }
    /* the octagonal cap */
    cairo_save(cctx);
    cairo_translate(cctx, cenx + vecx * 1.0, ceny + vecy * 1.0);
    cairo_new_path(cctx);
    for (idx = 0; idx < 8; idx++) {
        const double ang = (idx * 45.0 + 22.5) * G_PI / 180.0;
        if (idx == 0) cairo_move_to(cctx, 0.85 * sin(ang), -0.85 * cos(ang));
        else cairo_line_to(cctx, 0.85 * sin(ang), -0.85 * cos(ang));
    }
    cairo_close_path(cctx);
    rgb(cctx, 0x0C0C0C);
    cairo_fill_preserve(cctx);
    rgb(cctx, len > 0 ? COL_HELD : COL_BLACK_HI);
    cairo_set_line_width(cctx, len > 0 ? 0.12 : 0.1);
    cairo_stroke(cctx);
    cairo_restore(cctx);
}

/* The joystick's one red fire button, in its recess. */
static void draw_fire(cairo_t *cctx, int idx)
{
    const a800_control *ctl = &g_lay[idx];
    const double cenx = ctl->w / 2, ceny = ctl->h / 2;
    const gboolean lit = part_lit(idx);
    cairo_pattern_t *grad = cairo_pattern_create_radial(cenx - 0.25, ceny - 0.3, 0.05, cenx, ceny, 0.85);
    stop_rgb(grad, 0, lit ? COL_RED : COL_RED_HI);
    stop_rgb(grad, 1, lit ? COL_RED_DOWN : COL_RED);
    circle(cctx, cenx, ceny, 0.8);
    cairo_set_source(cctx, grad);
    cairo_fill(cctx);
    cairo_pattern_destroy(grad);
    if (lit) {
        circle(cctx, cenx, ceny, 0.9);
        rgb(cctx, COL_HELD);
        cairo_set_line_width(cctx, 0.12);
        cairo_stroke(cctx);
    }
}

/* ---- drawing: paddles ---------------------------------------------------------- */

/* The wedge of a CX30 (section 3): a black body, narrower at the cable end, with
 * the label plate in the corner. */
static void draw_paddle_case(cairo_t *cctx, GtkWidget *widget)
{
    static const double k_poly[6][2] = { { 1.0, 0.3 }, { 5.6, 0.3 }, { 7.0, 5.0 },
                                         { 6.2, 5.9 }, { 0.8, 5.9 }, { 0.0, 4.6 } };
    int idx;

    cairo_new_path(cctx);
    for (idx = 0; idx < 6; idx++) {
        if (idx == 0) cairo_move_to(cctx, k_poly[idx][0], k_poly[idx][1]);
        else cairo_line_to(cctx, k_poly[idx][0], k_poly[idx][1]);
    }
    cairo_close_path(cctx);
    vgrad(cctx, 0.3, 5.6, 0x252525, 0x181818);
    cairo_fill_preserve(cctx);
    /* rounded corners: the same outline stroked with a round join, then the bevel */
    cairo_set_line_join(cctx, CAIRO_LINE_JOIN_ROUND);
    cairo_set_line_width(cctx, 0.6);
    vgrad(cctx, 0.3, 5.6, 0x252525, 0x181818);
    cairo_stroke_preserve(cctx);
    rgb(cctx, COL_BLACK_HI);
    cairo_set_line_width(cctx, 0.06);
    cairo_stroke(cctx);

    /* the label plate, in the corner the knob leaves free */
    rrect(cctx, 4.5, 4.7, 2.1, 0.95, 0.15);
    rgb(cctx, COL_BLACK_LO);
    cairo_fill_preserve(cctx);
    rgb(cctx, 0x8A8A8A);
    cairo_set_line_width(cctx, 0.05);
    cairo_stroke(cctx);
    txt(cctx, widget, "paddle", 0.4, FALSE, TRUE, 0xE6E6E6, 4.55, 4.7, 1.2, 0.95, -1);
    /* two crossed rackets and a ball */
    rgb(cctx, 0xDCCB78);
    cairo_set_line_width(cctx, 0.06);
    cairo_move_to(cctx, 5.85, 5.5); cairo_line_to(cctx, 6.25, 4.85); cairo_stroke(cctx);
    cairo_move_to(cctx, 6.25, 5.5); cairo_line_to(cctx, 5.85, 4.85); cairo_stroke(cctx);
    circle(cctx, 5.95, 4.9, 0.16); cairo_stroke(cctx);
    circle(cctx, 6.15, 4.9, 0.16); cairo_stroke(cctx);
    circle(cctx, 6.05, 5.55, 0.06); cairo_fill(cctx);
}

/* The ribbed knob (60 knurl ticks on its rim, a flat cap and a position mark that
 * turns with it); the part's box is 4.4 square, the knob centred. */
static void draw_knob(cairo_t *cctx, int idx)
{
    const a800_control *ctl = &g_lay[idx];
    const int pad = ctl->arg == 1 ? 1 : 0;
    const double cenx = ctl->w / 2, ceny = ctl->h / 2;
    const double ang = a800session_knob_angle(g_paddle[ctl->port][pad]) * G_PI / 180.0;
    const gboolean active = g_drag.kind == 2 && g_drag.port == ctl->port && g_drag.paddle == pad;
    const gboolean moved = g_paddle[ctl->port][pad] != A800_PADDLE_MAX;
    int tick;

    circle(cctx, cenx, ceny, 2.2);
    rgb(cctx, 0x0E0E0E);
    cairo_fill_preserve(cctx);
    rgb(cctx, active || moved ? COL_HELD : 0x050505);
    cairo_set_line_width(cctx, active || moved ? 0.1 : 0.05);
    cairo_stroke(cctx);
    /* the knurl: the ticks turn with the knob */
    cairo_set_line_width(cctx, 0.05);
    rgb(cctx, 0x2A2A2A);
    for (tick = 0; tick < 60; tick++) {
        const double a2 = ang + tick * 2 * G_PI / 60;
        cairo_move_to(cctx, cenx + 2.0 * sin(a2), ceny - 2.0 * cos(a2));
        cairo_line_to(cctx, cenx + 2.2 * sin(a2), ceny - 2.2 * cos(a2));
    }
    cairo_stroke(cctx);
    circle(cctx, cenx, ceny, 1.75);
    rgb(cctx, 0x1A1A1A);
    cairo_fill(cctx);
    {
        cairo_pattern_t *grad = cairo_pattern_create_radial(cenx - 0.5, ceny - 0.6, 0.1, cenx, ceny, 1.65);
        stop_rgb(grad, 0, 0x2E2E2E);
        stop_rgb(grad, 1, 0x1E1E1E);
        circle(cctx, cenx, ceny, 1.65);
        cairo_set_source(cctx, grad);
        cairo_fill(cctx);
        cairo_pattern_destroy(grad);
    }
    /* the position mark, which the real knob does not have: a light line from the
     * middle toward where the knob points */
    rgb(cctx, 0xD8D8D8);
    cairo_set_line_cap(cctx, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_width(cctx, 0.13);
    cairo_move_to(cctx, cenx + 0.9 * sin(ang), ceny - 0.9 * cos(ang));
    cairo_line_to(cctx, cenx + 1.55 * sin(ang), ceny - 1.55 * cos(ang));
    cairo_stroke(cctx);
}

/* The fire button on a paddle's left edge. */
static void draw_edge_button(cairo_t *cctx, int idx)
{
    const a800_control *ctl = &g_lay[idx];
    const gboolean lit = part_lit(idx);
    vgrad(cctx, 0, ctl->h, lit ? 0xC03018 : 0xE8492C, lit ? COL_RED_DOWN : 0xD93A22);
    rrect(cctx, 0, 0, ctl->w, ctl->h, 0.3);
    cairo_fill_preserve(cctx);
    rgb(cctx, lit ? COL_HELD : 0x7A1D0E);
    cairo_set_line_width(cctx, lit ? 0.12 : 0.06);
    cairo_stroke(cctx);
}

/* A turn arrow under a knob: held, it keeps turning. */
static void draw_turn(cairo_t *cctx, int idx)
{
    const a800_control *ctl = &g_lay[idx];
    const gboolean lit = part_lit(idx);
    rgb(cctx, lit ? COL_HELD : 0x4A4A4A);
    cairo_new_path(cctx);
    if (ctl->dir > 0) {
        cairo_move_to(cctx, 0.1, 0.1);
        cairo_line_to(cctx, 0.75, ctl->h / 2);
        cairo_line_to(cctx, 0.1, ctl->h - 0.1);
    } else {
        cairo_move_to(cctx, ctl->w - 0.1, 0.1);
        cairo_line_to(cctx, ctl->w - 0.75, ctl->h / 2);
        cairo_line_to(cctx, ctl->w - 0.1, ctl->h - 0.1);
    }
    cairo_close_path(cctx);
    cairo_fill(cctx);
}

/* ---- drawing: mice, the Trak-Ball, the Koala Pad and the tablet ------------------ */

/* The three bars of the Atari fuji. */
static void draw_fuji(cairo_t *cctx, double xpos, double ypos, double size, guint32 colour)
{
    int bar;
    rgb(cctx, colour);
    for (bar = 0; bar < 3; bar++) {
        const double off = (bar - 1) * size * 0.34;
        cairo_move_to(cctx, xpos + off - size * 0.08, ypos + size);
        cairo_curve_to(cctx, xpos + off - size * 0.08, ypos + size * 0.5,
                       xpos + off * 0.6 - size * 0.05, ypos + size * 0.15, xpos + off * 0.2, ypos);
        cairo_curve_to(cctx, xpos + off * 0.6 + size * 0.05, ypos + size * 0.15,
                       xpos + off + size * 0.08, ypos + size * 0.5, xpos + off + size * 0.08, ypos + size);
        cairo_close_path(cctx);
    }
    cairo_fill(cctx);
}

/* An Atari ST or Amiga mouse seen from above: a rounded cream body, two buttons
 * (parts of their own) in the top 40%, the fuji embossed low on the back. */
static void draw_mouse_case(cairo_t *cctx, const a800_control *ctl, gboolean amiga)
{
    vgrad(cctx, 0, ctl->h, amiga ? 0xD8D2C0 : 0xCCC6B7, amiga ? 0xC4BDA9 : 0xB8B2A2);
    rrect(cctx, 0, 0, ctl->w, ctl->h, amiga ? 1.0 : 1.2);
    cairo_fill_preserve(cctx);
    rgb(cctx, 0x8E8878);
    cairo_set_line_width(cctx, 0.06);
    cairo_stroke(cctx);
    /* the seam between the buttons and the body */
    rgb(cctx, 0x8E8878);
    cairo_set_line_width(cctx, 0.05);
    cairo_move_to(cctx, 0.2, 0.4 * ctl->h);
    cairo_line_to(cctx, ctl->w - 0.2, 0.4 * ctl->h);
    cairo_stroke(cctx);
    draw_fuji(cctx, ctl->w / 2, 0.62 * ctl->h, 1.0, 0xB0AA9B);
}

/* A mouse button, the Trak-Ball's, or the Koala Pad's. */
static void draw_pointer_button(cairo_t *cctx, int idx)
{
    const a800_control *ctl = &g_lay[idx];
    const int dev = ctl->port >= 0 ? g_dev[ctl->port] : -1;
    const gboolean lit = part_lit(idx);
    const gboolean latched = ctl->port >= 0 && (g_ptr_latched[ctl->port] & (unsigned)ctl->arg);
    guint32 fill, edge;

    if (dev == A800_DEV_KOALA) { fill = lit ? 0x3A3A3A : 0x151515; edge = 0x000000; }
    else if (dev == A800_DEV_TRAKBALL) { fill = lit ? 0x555555 : 0x3A3A3A; edge = 0x0C0C0C; }
    else { fill = lit ? 0xA8A290 : 0xBDB7A6; edge = 0x7E7868; }
    rrect(cctx, 0, 0, ctl->w, ctl->h, 0.3);
    rgb(cctx, fill);
    cairo_fill_preserve(cctx);
    rgb(cctx, lit ? COL_HELD : edge);
    cairo_set_line_width(cctx, lit ? 0.12 : 0.06);
    cairo_stroke(cctx);
    if (latched) {
        rgb(cctx, COL_HELD);
        circle(cctx, ctl->w / 2, ctl->h / 2, 0.12);
        cairo_fill(cctx);
    }
}

/* The mat a mouse is dragged over (the window's stand-in for a desk). */
static void draw_mat(cairo_t *cctx, GtkWidget *widget, const a800_control *ctl)
{
    static const double k_dash[2] = { 0.25, 0.25 };
    rrect(cctx, 0, 0, ctl->w, ctl->h, 0.6);
    rgb(cctx, 0x2A2A2A);
    cairo_fill_preserve(cctx);
    cairo_set_dash(cctx, k_dash, 2, 0);
    rgb(cctx, 0x555555);
    cairo_set_line_width(cctx, 0.08);
    cairo_stroke(cctx);
    cairo_set_dash(cctx, NULL, 0, 0);
    txt(cctx, widget, ctl->legend ? ctl->legend : "drag to move", 0.5, FALSE, TRUE, 0x8C8C8C, 0, 0,
        ctl->w, ctl->h, 0);
}

/* The CX22 Trak-Ball (section 8): an angular black body with a silver strip, the
 * ball in its ring, a button at each upper corner (parts). */
static void draw_trakball_case(cairo_t *cctx, GtkWidget *widget)
{
    static const double k_poly[8][2] = { { 0.6, 0.0 }, { 8.4, 0.0 }, { 9.0, 1.2 }, { 9.0, 4.4 },
                                         { 8.4, 5.0 }, { 0.6, 5.0 }, { 0.0, 4.4 }, { 0.0, 1.2 } };
    int idx;
    cairo_new_path(cctx);
    for (idx = 0; idx < 8; idx++) {
        if (idx == 0) cairo_move_to(cctx, k_poly[idx][0], k_poly[idx][1]);
        else cairo_line_to(cctx, k_poly[idx][0], k_poly[idx][1]);
    }
    cairo_close_path(cctx);
    vgrad(cctx, 0, 5, 0x2E2E2E, 0x1A1A1A);
    cairo_fill_preserve(cctx);
    rgb(cctx, 0x0A0A0A);
    cairo_set_line_width(cctx, 0.07);
    cairo_stroke(cctx);
    /* the ball's ring */
    circle(cctx, 4.5, 2.6, 1.9);
    rgb(cctx, 0x101010);
    cairo_fill_preserve(cctx);
    rgb(cctx, 0x3A3A3A);
    cairo_set_line_width(cctx, 0.08);
    cairo_stroke(cctx);
    /* the silver strip */
    rrect(cctx, 0.5, 4.05, 8.0, 0.7, 0.12);
    rgb(cctx, 0xD8D8D8);
    cairo_fill(cctx);
    txt(cctx, widget, "ATARI", 0.42, TRUE, FALSE, 0x3A3A3A, 0.7, 4.05, 2.0, 0.7, -1);
    txt(cctx, widget, "TRAK-BALL", 0.42, TRUE, FALSE, 0x3A3A3A, 5.8, 4.05, 2.5, 0.7, 1);
}

/* The Trak-Ball's ball: glossy, with a highlight. */
static void draw_ball(cairo_t *cctx, int idx)
{
    const a800_control *ctl = &g_lay[idx];
    const double cenx = ctl->w / 2, ceny = ctl->h / 2;
    cairo_pattern_t *grad = cairo_pattern_create_radial(cenx - 0.5, ceny - 0.6, 0.1, cenx, ceny, 1.5);
    const gboolean active = g_drag.kind == 3 && g_drag.idx == idx;
    stop_rgb(grad, 0, 0x6A6A6A);
    stop_rgb(grad, 0.5, 0x2A2A2A);
    stop_rgb(grad, 1, 0x0E0E0E);
    circle(cctx, cenx, ceny, 1.5);
    cairo_set_source(cctx, grad);
    cairo_fill_preserve(cctx);
    cairo_pattern_destroy(grad);
    rgb(cctx, active ? COL_HELD : 0x050505);
    cairo_set_line_width(cctx, active ? 0.1 : 0.05);
    cairo_stroke(cctx);
}

/* The Koala Pad (cream, a dark pad, a name plate) and the touch tablet (black, a
 * silver strip, a magenta-edged pad). The pad itself is a part (draw_pad). */
static void draw_koala_case(cairo_t *cctx, GtkWidget *widget, const a800_control *ctl)
{
    vgrad(cctx, 0, ctl->h, 0xEAE3CF, 0xDCD5C0);
    rrect(cctx, 0, 0, ctl->w, ctl->h, 0.6);
    cairo_fill_preserve(cctx);
    rgb(cctx, 0xA8A18C);
    cairo_set_line_width(cctx, 0.06);
    cairo_stroke(cctx);
    txt(cctx, widget, "KOALA PAD", 0.5, TRUE, FALSE, 0x6A6452, 0, ctl->h - 0.95, ctl->w, 0.8, 0);
}

static void draw_tablet_case(cairo_t *cctx, GtkWidget *widget, const a800_control *ctl)
{
    vgrad(cctx, 0, ctl->h, 0x242424, 0x181818);
    rrect(cctx, 0, 0, ctl->w, ctl->h, 0.5);
    cairo_fill_preserve(cctx);
    rgb(cctx, 0x0A0A0A);
    cairo_set_line_width(cctx, 0.06);
    cairo_stroke(cctx);
    rrect(cctx, 0.2, 0.2, ctl->w - 0.4, 0.7, 0.12);
    rgb(cctx, 0xD8D8D4);
    cairo_fill(cctx);
    txt(cctx, widget, "ATARI", 0.42, TRUE, FALSE, 0x3A3A3A, 0.4, 0.2, 2.0, 0.7, -1);
    txt(cctx, widget, "TOUCH TABLET", 0.42, TRUE, FALSE, 0x3A3A3A, ctl->w - 4.6, 0.2, 4.2, 0.7, 1);
    /* the stylus, clipped to the right edge */
    rgb(cctx, 0x9A9A9A);
    cairo_set_line_width(cctx, 0.14);
    cairo_set_line_cap(cctx, CAIRO_LINE_CAP_ROUND);
    cairo_move_to(cctx, ctl->w - 0.45, 1.5);
    cairo_line_to(cctx, ctl->w - 0.45, 4.6);
    cairo_stroke(cctx);
}

/* A pad's surface, with a cross hair at the point being touched. */
static void draw_pad(cairo_t *cctx, GtkWidget *widget, int idx)
{
    const a800_control *ctl = &g_lay[idx];
    const gboolean tablet = ctl->arg == 1;
    const gboolean active = g_drag.kind == 4 && g_drag.idx == idx;
    rrect(cctx, 0, 0, ctl->w, ctl->h, tablet ? 0.2 : 0.3);
    rgb(cctx, tablet ? 0x2B2E30 : 0x3B3B3D);
    cairo_fill_preserve(cctx);
    rgb(cctx, active ? COL_HELD : tablet ? 0xC0408A : 0x232325);
    cairo_set_line_width(cctx, active ? 0.12 : tablet ? 0.14 : 0.08);
    cairo_stroke(cctx);
    if (active && g_pad_touch) {
        const double xpos = g_pad_u * ctl->w, ypos = g_pad_v * ctl->h;
        rgb(cctx, COL_HELD);
        cairo_set_line_width(cctx, 0.08);
        cairo_move_to(cctx, xpos, 0.2); cairo_line_to(cctx, xpos, ctl->h - 0.2);
        cairo_move_to(cctx, 0.2, ypos); cairo_line_to(cctx, ctl->w - 0.2, ypos);
        cairo_stroke(cctx);
    } else {
        txt(cctx, widget, ctl->legend ? ctl->legend : "touch the pad", 0.5, FALSE, TRUE, 0x8C8C8C, 0, 0,
            ctl->w, ctl->h, 0);
    }
}

/* The CX75 light pen: a thin black pen with a clip and a tip. */
static void draw_pen(cairo_t *cctx, const a800_control *ctl)
{
    rrect(cctx, 0, 0.5, ctl->w, ctl->h - 1.9, 0.4);
    vgrad(cctx, 0, ctl->h, 0x2E2E2E, 0x1A1A1A);
    cairo_fill_preserve(cctx);
    rgb(cctx, 0x0A0A0A);
    cairo_set_line_width(cctx, 0.05);
    cairo_stroke(cctx);
    /* the tip */
    cairo_move_to(cctx, 0.15, ctl->h - 1.4);
    cairo_line_to(cctx, ctl->w - 0.15, ctl->h - 1.4);
    cairo_line_to(cctx, ctl->w / 2, ctl->h);
    cairo_close_path(cctx);
    rgb(cctx, 0x8A8A8A);
    cairo_fill(cctx);
    /* the clip */
    rrect(cctx, ctl->w - 0.2, 0.8, 0.22, 2.4, 0.08);
    rgb(cctx, 0x9A9A9A);
    cairo_fill(cctx);
}

/* The XG-1 light gun: a grey pistol with a barrel, front sight, ribbed grip and a
 * dark trigger. */
static void draw_gun(cairo_t *cctx, const a800_control *ctl)
{
    int rib;
    (void)ctl;
    /* barrel and slide */
    rrect(cctx, 0.2, 0.7, 7.2, 1.6, 0.35);
    vgrad(cctx, 0.7, 1.6, 0xC6C1B4, 0xA9A498);
    cairo_fill_preserve(cctx);
    rgb(cctx, 0x77736A);
    cairo_set_line_width(cctx, 0.07);
    cairo_stroke(cctx);
    /* the front sight */
    rgb(cctx, 0x77736A);
    cairo_rectangle(cctx, 0.5, 0.35, 0.4, 0.4);
    cairo_fill(cctx);
    /* the frame under the slide and the grip */
    cairo_move_to(cctx, 3.0, 2.3);
    cairo_line_to(cctx, 7.4, 2.3);
    cairo_line_to(cctx, 7.0, 3.5);
    cairo_line_to(cctx, 7.6, 6.7);
    cairo_line_to(cctx, 5.5, 6.7);
    cairo_line_to(cctx, 4.9, 3.5);
    cairo_line_to(cctx, 3.0, 3.5);
    cairo_close_path(cctx);
    vgrad(cctx, 2.3, 4.4, 0xB7B2A6, 0x9D988C);
    cairo_fill_preserve(cctx);
    rgb(cctx, 0x77736A);
    cairo_set_line_width(cctx, 0.07);
    cairo_stroke(cctx);
    /* the grip's ribs */
    rgb(cctx, 0x8A857A);
    cairo_set_line_width(cctx, 0.07);
    for (rib = 0; rib < 6; rib++) {
        const double ypos = 3.9 + rib * 0.45;
        cairo_move_to(cctx, 5.15 + (ypos - 3.5) * 0.18, ypos);
        cairo_line_to(cctx, 6.9 + (ypos - 3.5) * 0.18, ypos);
        cairo_stroke(cctx);
    }
    /* the trigger and its guard */
    rgb(cctx, 0x6E6C66);
    rrect(cctx, 4.5, 3.4, 0.35, 1.1, 0.12);
    cairo_fill(cctx);
    cairo_set_line_width(cctx, 0.09);
    cairo_arc(cctx, 4.1, 3.5, 0.9, 0.2, 1.4);
    cairo_stroke(cctx);
}

/* The CX85 numeric keypad: a cream case, a dark face and keys (parts). */
static void draw_cx85_case(cairo_t *cctx, GtkWidget *widget, const a800_control *ctl)
{
    rrect(cctx, 0, 0, ctl->w, ctl->h, 0.4);
    vgrad(cctx, 0, ctl->h, 0xDDD5BE, 0xCEC6AE);
    cairo_fill_preserve(cctx);
    rgb(cctx, 0x8E8774);
    cairo_set_line_width(cctx, 0.06);
    cairo_stroke(cctx);
    rrect(cctx, 0.1, 0.05, ctl->w - 0.2, ctl->h - 0.15, 0.3);
    rgb(cctx, 0x55564F);
    cairo_fill(cctx);
    txt(cctx, widget, "ATARI", 0.32, TRUE, FALSE, 0xF0C21A, 0.3, 0.02, 2.0, 0.34, -1);
    txt(cctx, widget, "CX85", 0.32, TRUE, FALSE, 0xE8E4D6, ctl->w - 2.3, 0.02, 2.0, 0.34, 1);
}

static void draw_cx85_key(cairo_t *cctx, GtkWidget *widget, int idx)
{
    const a800_control *ctl = &g_lay[idx];
    const gboolean lit = part_lit(idx);
    const int len = (int)strlen(ctl->label);
    double size = 0.5;
    if (len > 1 && size > ctl->w / (len * 0.62)) size = ctl->w / (len * 0.62);
    vgrad(cctx, 0, ctl->h, lit ? 0x57503F : 0x4A4437, lit ? 0x3E382B : 0x2E2A22);
    rrect(cctx, 0, 0, ctl->w, ctl->h, 0.18);
    cairo_fill_preserve(cctx);
    rgb(cctx, lit ? COL_HELD : 0x1A1812);
    cairo_set_line_width(cctx, lit ? 0.1 : 0.05);
    cairo_stroke(cctx);
    txt(cctx, widget, ctl->label, size, TRUE, FALSE, 0xE8E4D6, 0, 0, ctl->w, ctl->h, 0);
}

/* ---- drawing: the console strip ----------------------------------------------------- */

static void draw_console_panel(cairo_t *cctx, const a800_control *ctl)
{
    vgrad(cctx, 0, ctl->h, 0x1A1A1A, 0x101010);
    rrect(cctx, 0, 0, ctl->w, ctl->h, 0.3);
    cairo_fill_preserve(cctx);
    rgb(cctx, 0x050505);
    cairo_set_line_width(cctx, 0.06);
    cairo_stroke(cctx);
    rgb(cctx, COL_PIN);
    rrect(cctx, 0.12, 0.12, ctl->w - 0.24, ctl->h - 0.24, 0.22);
    cairo_set_line_width(cctx, 0.05);
    cairo_stroke(cctx);
}

/* The colours of a console key, the orange-to-yellow set of the real console
 * (section 7); HELP is grey and BREAK red. */
static void console_colours(int con, guint32 *top, guint32 *bot, guint32 *ink)
{
    switch (con) {
    case A800_CON_RESET:  *top = 0xE5A23A; *bot = 0xC57F10; *ink = 0x3A2A10; break;
    case A800_CON_OPTION: *top = 0xF0B03A; *bot = 0xD38A12; *ink = 0x3A2A10; break;
    case A800_CON_SELECT: *top = 0xF6BC3A; *bot = 0xDA9A14; *ink = 0x3A2A10; break;
    case A800_CON_START:  *top = 0xFAD040; *bot = 0xE2A812; *ink = 0x3A2A10; break;
    case A800_CON_HELP:   *top = 0xBDBBB2; *bot = 0x95938A; *ink = 0x2A2A28; break;
    default:              *top = 0xCC4A30; *bot = 0x9C2A14; *ink = 0xFFF2E8; break;
    }
}

static void draw_console_key(cairo_t *cctx, GtkWidget *widget, int idx)
{
    const a800_control *ctl = &g_lay[idx];
    const int con = ctl->target - A800_TARGET_CONSOLE(0);
    const gboolean lit = part_lit(idx);
    guint32 top, bot, ink;

    console_colours(con, &top, &bot, &ink);
    vgrad(cctx, 0, ctl->h, lit ? bot : top, lit ? 0x7A4A08 : bot);
    rrect(cctx, 0, 0, ctl->w, ctl->h, con == A800_CON_HELP ? ctl->h / 2 : 0.2);
    cairo_fill_preserve(cctx);
    rgb(cctx, lit ? 0xFFFFFF : 0x3A2A10);
    cairo_set_line_width(cctx, lit ? 0.12 : 0.06);
    cairo_stroke(cctx);
    txt(cctx, widget, ctl->label, 0.42, TRUE, FALSE, ink, 0, 0, ctl->w, ctl->h, 0);
}

/* ---- drawing: the keyboard ---------------------------------------------------------- */

typedef struct {
    guint32 plate;              /* the case behind the keys */
    guint32 key_top, key_bot, key_edge;
    guint32 legend, shifted;
    gboolean flat;              /* the 400's membrane keys */
} KbdLook;

static KbdLook kbd_look(void)
{
    KbdLook look;
    switch (g_family) {
    case A800_KBD_400:
        look.plate = 0x151515; look.key_top = 0x2A2A2A; look.key_bot = 0x1E1E1E;
        look.key_edge = 0x0A0A0A; look.legend = 0xF2F2F2; look.shifted = 0xE08A1F; look.flat = TRUE;
        break;
    case A800_KBD_800:
        look.plate = 0xD4CDB8; look.key_top = 0x4A4437; look.key_bot = 0x3B352B;
        look.key_edge = 0x1E1B15; look.legend = 0xE9E4D4; look.shifted = 0xB7B09C; look.flat = FALSE;
        break;
    default:
        look.plate = 0xE6E3DA; look.key_top = 0x3A3A3E; look.key_bot = 0x2E2E31;
        look.key_edge = 0x141416; look.legend = 0xF2F2F2; look.shifted = 0xBDBDC4; look.flat = FALSE;
        break;
    }
    return look;
}

/* One cap of the keyboard (pixels, in the keyboard's own origin). */
static void draw_cap(cairo_t *cctx, GtkWidget *widget, int idx, const KbdLook *look)
{
    const a800_kbd_key *key = &g_keys[idx];
    const double cx0 = key->x * KBD_UNIT + 2.0, cy0 = key->y * KBD_UNIT + 2.0;
    const double cw = key->w * KBD_UNIT - 4.0, ch = key->h * KBD_UNIT - 4.0;
    const gboolean lit = g_key_held[idx] != 0;
    const gboolean console = key->kind == A800_KEY_CONSOLE;
    const gboolean help = console && key->console == A800_CON_HELP;
    const gboolean orange = console && !help && g_family != A800_KBD_XL && g_family != A800_KBD_1200XL;
    guint32 top = look->key_top, bot = look->key_bot, ink = look->legend;
    const int len = (int)g_utf8_strlen(key->label, -1);
    double size;

    if (orange) {
        console_colours(key->console, &top, &bot, &ink);
    } else if (help) {
        top = 0xBDBBB2; bot = 0x95938A; ink = 0x2A2A28;
    } else if (key->kind == A800_KEY_BREAK) {
        ink = 0xFF8A5A;
    }
    cairo_save(cctx);
    cairo_translate(cctx, cx0, cy0);
    vgrad(cctx, 0, ch, lit ? bot : top, lit ? 0x15130F : bot);
    rrect(cctx, 0, 0, cw, ch, help ? ch / 2 : look->flat ? 6 : 5);
    cairo_fill_preserve(cctx);
    rgb(cctx, lit ? COL_HELD : look->key_edge);
    cairo_set_line_width(cctx, lit ? 2.2 : 1.0);
    cairo_stroke(cctx);
    if (!look->flat && !lit) {
        /* a light bevel along the top edge of a full-travel cap */
        rgba(cctx, 0xFFFFFF, 0.12);
        cairo_set_line_width(cctx, 1.0);
        cairo_move_to(cctx, 4, 2.5);
        cairo_line_to(cctx, cw - 4, 2.5);
        cairo_stroke(cctx);
    }

    if (key->kind == A800_KEY_ATARI) {
        draw_fuji(cctx, cw / 2, ch * 0.22, ch * 0.56, ink);
    } else if (key->kind == A800_KEY_SPACE) {
        /* a blank bar */
    } else {
        size = len <= 1 ? 17.0 : fmin(12.0, (cw - 8.0) / (0.64 * len));
        if (key->shifted) {
            const int slen = (int)g_utf8_strlen(key->shifted, -1);
            const double ssz = slen <= 1 ? 11.0 : fmin(9.0, (cw - 6.0) / (0.62 * slen));
            txt(cctx, widget, key->shifted, ssz, TRUE, FALSE, orange ? ink : look->shifted, 0, 2,
                cw, ch * 0.42, 0);
            txt(cctx, widget, key->label, len <= 1 ? 15.0 : size, TRUE, FALSE, ink, 0, ch * 0.38, cw,
                ch * 0.58, 0);
        } else if (key->kind == A800_KEY_SHIFT || key->kind == A800_KEY_CTRL) {
            /* the boxed legend of the real SHIFT and CTRL */
            const double bw = fmin(cw - 6.0, txt_width(widget, key->label, size, TRUE) + 8.0);
            rrect(cctx, (cw - bw) / 2, ch * 0.28, bw, ch * 0.44, 3);
            rgb(cctx, ink);
            cairo_set_line_width(cctx, 1.0);
            cairo_stroke(cctx);
            txt(cctx, widget, key->label, size, TRUE, FALSE, ink, 0, 0, cw, ch, 0);
        } else {
            txt(cctx, widget, key->label, size, TRUE, FALSE, ink, 0, 0, cw, ch, 0);
        }
    }
    cairo_restore(cctx);
}

static void draw_keyboard(cairo_t *cctx, GtkWidget *widget)
{
    const KbdLook look = kbd_look();
    int idx;

    cairo_save(cctx);
    cairo_translate(cctx, kbd_x0(), kbd_y0());
    /* the case: beige on the 800 and the XL/XE, the 400's black membrane panel */
    rrect(cctx, -10, -10, g_kw * KBD_UNIT + 20, g_kh * KBD_UNIT + 20, 10);
    rgb(cctx, look.plate);
    cairo_fill_preserve(cctx);
    rgb(cctx, g_family == A800_KBD_400 ? 0x050505 : 0x8E8878);
    cairo_set_line_width(cctx, 1.0);
    cairo_stroke(cctx);
    for (idx = 0; idx < g_nkeys; idx++) {
        const gboolean dim = key_dimmed(idx);
        if (dim) cairo_push_group(cctx);
        draw_cap(cctx, widget, idx, &look);
        if (dim) {
            cairo_pop_group_to_source(cctx);
            cairo_paint_with_alpha(cctx, 0.4);
        }
    }
    cairo_restore(cctx);
}

/* ---- drawing: bindings and Map -------------------------------------------------- */

/* A small dark pill with a key's name, centred at (xcen, ytop) in the current
 * units; the picked target's has the held ring. */
static void draw_pill(cairo_t *cctx, GtkWidget *widget, const char *text, double xcen, double ytop,
                      double size, gboolean picked)
{
    const double wid = fmax(size * 2.4, txt_width(widget, text, size, TRUE) + size * 0.9);
    const double hgt = size * 1.5;
    rrect(cctx, xcen - wid / 2, ytop, wid, hgt, hgt * 0.26);
    rgba(cctx, 0x000000, 0.8);
    cairo_fill_preserve(cctx);
    rgb(cctx, picked ? COL_HELD : 0x555555);
    cairo_set_line_width(cctx, picked ? size * 0.22 : size * 0.08);
    cairo_stroke(cctx);
    txt(cctx, widget, text, size, TRUE, FALSE, 0xF2F2F2, xcen - wid / 2, ytop, wid, hgt, 0);
}

/* The four quarters of the stick: its four direction keys round the ring. */
static void draw_stick_bindings(cairo_t *cctx, GtkWidget *widget, const a800_control *ctl)
{
    static const struct { int act; double dx, dy; } k_dir[4] = {
        { A800_ACT_UP, 0, -2.45 }, { A800_ACT_DOWN, 0, 2.45 },
        { A800_ACT_LEFT, -2.45, 0 }, { A800_ACT_RIGHT, 2.45, 0 },
    };
    int dir;
    for (dir = 0; dir < 4; dir++) {
        const int target = A800_TARGET_PORT(ctl->port, k_dir[dir].act);
        char text[40];
        bound_text(target, text, sizeof text);
        if (!text[0]) continue;
        draw_pill(cctx, widget, text, ctl->w / 2 + k_dir[dir].dx, ctl->h / 2 + k_dir[dir].dy - 0.27, 0.36,
                  target == g_map_state);
    }
}

/* ---- drawing: the whole window ---------------------------------------------------- */

static void draw_part(cairo_t *cctx, GtkWidget *widget, int idx)
{
    const a800_control *ctl = &g_lay[idx];

    cairo_save(cctx);
    cairo_translate(cctx, part_x(ctl), part_y(ctl));
    cairo_scale(cctx, UNIT, UNIT);
    switch (ctl->kind) {
    case A800_CTL_CAPTION: {
        char *text = ctl->legend ? g_strdup_printf("%s  \xc2\xb7  %s", ctl->label, ctl->legend)
                                 : g_strdup(ctl->label);
        txt(cctx, widget, text, 0.62, TRUE, FALSE, COL_CAPTION, 0, 0, ctl->w, ctl->h, 0);
        g_free(text);
        break;
    }
    case A800_CTL_BODY:
        if (!strcmp(ctl->label, "Joystick")) draw_joy_case(cctx, widget, ctl->port);
        else if (!strcmp(ctl->label, "Paddle A") || !strcmp(ctl->label, "Paddle B"))
            draw_paddle_case(cctx, widget);
        else if (!strcmp(ctl->label, "Atari ST mouse")) draw_mouse_case(cctx, ctl, FALSE);
        else if (!strcmp(ctl->label, "Amiga mouse")) draw_mouse_case(cctx, ctl, TRUE);
        else if (!strcmp(ctl->label, "Trak-Ball")) draw_trakball_case(cctx, widget);
        else if (!strcmp(ctl->label, "Koala Pad")) draw_koala_case(cctx, widget, ctl);
        else if (!strcmp(ctl->label, "Touch tablet")) draw_tablet_case(cctx, widget, ctl);
        else if (!strcmp(ctl->label, "Light pen")) draw_pen(cctx, ctl);
        else if (!strcmp(ctl->label, "Light gun")) draw_gun(cctx, ctl);
        else if (!strcmp(ctl->label, "CX85")) draw_cx85_case(cctx, widget, ctl);
        else if (!strcmp(ctl->label, "Console")) draw_console_panel(cctx, ctl);
        else if (!strcmp(ctl->label, "Nothing"))
            txt(cctx, widget, "nothing plugged in", 0.5, FALSE, TRUE, 0x8C8578, 0, 0, ctl->w, ctl->h, 0);
        break;
    case A800_CTL_STICK:
        draw_stick(cctx, ctl);
        break;
    case A800_CTL_ROUND_BUTTON:
        draw_fire(cctx, idx);
        break;
    case A800_CTL_KNOB:
        draw_knob(cctx, idx);
        break;
    case A800_CTL_PADDLE_FIRE:
        draw_edge_button(cctx, idx);
        break;
    case A800_CTL_TURN:
        draw_turn(cctx, idx);
        break;
    case A800_CTL_CONSOLE_KEY:
        draw_console_key(cctx, widget, idx);
        break;
    case A800_CTL_POINTER_BUTTON:
        draw_pointer_button(cctx, idx);
        break;
    case A800_CTL_MAT:
        draw_mat(cctx, widget, ctl);
        break;
    case A800_CTL_BALL:
        draw_ball(cctx, idx);
        break;
    case A800_CTL_PAD:
        draw_pad(cctx, widget, idx);
        break;
    case A800_CTL_CX85_KEY:
        draw_cx85_key(cctx, widget, idx);
        break;
    case A800_CTL_NOTE:
        txt(cctx, widget, ctl->label, 0.55, FALSE, TRUE, COL_CAPTION, 0, 0, ctl->w, ctl->h, -1);
        break;
    default:
        break;
    }
    cairo_restore(cctx);
}

/* The keys bound to the controls (Show bindings, Map mode), drawn after every
 * control so a label is never hidden under the next one. */
static void draw_bindings_pass(cairo_t *cctx, GtkWidget *widget)
{
    int idx;
    if (!showing_bindings()) return;
    for (idx = 0; idx < g_n; idx++) {
        const a800_control *ctl = &g_lay[idx];
        const gboolean dim = dimmed(idx);
        char text[40];
        if (ctl->kind != A800_CTL_STICK && ctl->kind != A800_CTL_ROUND_BUTTON &&
            ctl->kind != A800_CTL_CONSOLE_KEY)
            continue;
        if (dim) cairo_push_group(cctx);
        cairo_save(cctx);
        cairo_translate(cctx, part_x(ctl), part_y(ctl));
        cairo_scale(cctx, UNIT, UNIT);
        if (ctl->kind == A800_CTL_STICK) {
            draw_stick_bindings(cctx, widget, ctl);
        } else {
            bound_text(ctl->target, text, sizeof text);
            if (text[0])
                draw_pill(cctx, widget, text, ctl->w / 2, ctl->h + 0.05, 0.36, ctl->target == g_map_state);
        }
        cairo_restore(cctx);
        if (dim) {
            cairo_pop_group_to_source(cctx);
            cairo_paint_with_alpha(cctx, 0.5);
        }
    }
    if (g_show_kbd) {
        for (idx = 0; idx < g_nkeys; idx++) {
            const a800_kbd_key *key = &g_keys[idx];
            const int target = key_target(key);
            char text[40];
            if (target < 0) continue;
            bound_text(target, text, sizeof text);
            if (!text[0]) continue;
            cairo_save(cctx);
            cairo_translate(cctx, kbd_x0(), kbd_y0());
            draw_pill(cctx, widget, text, (key->x + key->w / 2) * KBD_UNIT, (key->y + key->h) * KBD_UNIT - 13,
                      8.5, target == g_map_state);
            cairo_restore(cctx);
        }
    }
}

/* The hover and the Map pick: an outline round the control under the pointer
 * (Map mode only: otherwise the control already shows what it does). */
static void draw_outline(cairo_t *cctx, const Hit *hit, gboolean picked)
{
    if (hit->key >= 0) {
        const a800_kbd_key *key = &g_keys[hit->key];
        cairo_save(cctx);
        cairo_translate(cctx, kbd_x0(), kbd_y0());
        rrect(cctx, key->x * KBD_UNIT + 1, key->y * KBD_UNIT + 1, key->w * KBD_UNIT - 2,
              key->h * KBD_UNIT - 2, 6);
        rgb(cctx, picked ? COL_HELD : 0xFFFFFF);
        cairo_set_line_width(cctx, picked ? 3.0 : 2.0);
        cairo_stroke(cctx);
        cairo_restore(cctx);
    } else if (hit->idx >= 0) {
        const a800_control *ctl = &g_lay[hit->idx];
        cairo_save(cctx);
        cairo_translate(cctx, part_x(ctl), part_y(ctl));
        cairo_scale(cctx, UNIT, UNIT);
        if (ctl->kind == A800_CTL_STICK || ctl->kind == A800_CTL_ROUND_BUTTON)
            circle(cctx, ctl->w / 2, ctl->h / 2, ctl->w / 2);
        else
            rrect(cctx, -0.1, -0.1, ctl->w + 0.2, ctl->h + 0.2, 0.25);
        rgb(cctx, picked ? COL_HELD : 0xFFFFFF);
        cairo_set_line_width(cctx, picked ? 0.14 : 0.08);
        cairo_stroke(cctx);
        cairo_restore(cctx);
    }
}

static void draw_area(GtkDrawingArea *area, cairo_t *cctx, int wid, int hgt, gpointer udata)
{
    GtkWidget *widget = GTK_WIDGET(area);
    cairo_pattern_t *grad = cairo_pattern_create_linear(0, 0, 0, hgt);
    int idx;
    (void)udata;

    /* the desk the controllers lie on */
    stop_rgb(grad, 0, COL_DESK_TOP);
    stop_rgb(grad, 1, COL_DESK_BOT);
    rrect(cctx, 0.5, 0.5, wid - 1, hgt - 1, 14);
    cairo_set_source(cctx, grad);
    cairo_fill_preserve(cctx);
    cairo_pattern_destroy(grad);
    rgb(cctx, COL_DESK_EDGE);
    cairo_set_line_width(cctx, 1);
    cairo_stroke(cctx);

    for (idx = 0; idx < g_n; idx++) {
        const gboolean dim = dimmed(idx);
        if (dim) cairo_push_group(cctx);
        draw_part(cctx, widget, idx);
        if (dim) {
            cairo_pop_group_to_source(cctx);
            cairo_paint_with_alpha(cctx, 0.38);
        }
    }
    if (g_show_kbd && g_nkeys)
        draw_keyboard(cctx, widget);
    draw_bindings_pass(cctx, widget);
    if (g_map_state != -2) {
        /* armed: the pickable control under the pointer, and the picked one */
        if ((g_hover.idx >= 0 || g_hover.key >= 0) && g_hover.target >= 0 && g_hover.target != g_map_state)
            draw_outline(cctx, &g_hover, FALSE);
        if (g_map_state >= 0) {
            for (idx = 0; idx < g_n; idx++) {
                if (g_lay[idx].target == g_map_state && g_lay[idx].kind != A800_CTL_STICK) {
                    Hit hit = { idx, -1, g_map_state };
                    draw_outline(cctx, &hit, TRUE);
                }
            }
            if (g_show_kbd) {
                for (idx = 0; idx < g_nkeys; idx++) {
                    if (key_target(&g_keys[idx]) == g_map_state) {
                        Hit hit = { -1, idx, g_map_state };
                        draw_outline(cctx, &hit, TRUE);
                    }
                }
            }
        }
    }
}

/* ---- pointer ---------------------------------------------------------------------- */

/* What the pointer holds on a mouse-type device: the buttons held with the mouse
 * and the ones latched with the right button. */
static unsigned pointer_mask(int port)
{
    return (g_ptr_held[port] | g_ptr_latched[port]) & 3u;
}

static void pointer_send_buttons(int port)
{
    a800session_pointer_motion(g_session, 0, 0, pointer_mask(port));
}

/* Lets go of everything the mouse holds on the controllers. */
static void mouse_release(void)
{
    const gboolean any = g_drag.kind || g_press.idx >= 0 || g_press.key >= 0;
    if (g_drag.kind == 1) {
        a800session_pointer_set(g_session, g_drag.port, 0);
    } else if (g_drag.kind == 2) {
        a800session_paddle_pointer(g_session, g_drag.port, g_drag.paddle, 0, 0);
    } else if (g_drag.kind == 4) {
        a800session_pointer_norm(g_session, 0, 0, 0, 0);
        g_pad_touch = FALSE;
    }
    if (g_press.key >= 0 && g_press.key < g_nkeys) {
        a800session_keyboard_press(g_session, g_keys[g_press.key].index, 0);
    } else if (g_press.idx >= 0 && g_press.idx < g_n && !g_drag.kind) {
        const a800_control *ctl = &g_lay[g_press.idx];
        switch (ctl->kind) {
        case A800_CTL_ROUND_BUTTON:
        case A800_CTL_CONSOLE_KEY:
            if (ctl->target >= 0) a800session_press(g_session, ctl->target, 0);
            break;
        case A800_CTL_PADDLE_FIRE:
            a800session_paddle_fire(g_session, ctl->port, ctl->arg, 0);
            break;
        case A800_CTL_TURN:
            a800session_paddle_turn(g_session, ctl->port, ctl->arg, 0);
            break;
        case A800_CTL_POINTER_BUTTON:
            g_ptr_held[ctl->port] &= ~(unsigned)ctl->arg;
            pointer_send_buttons(ctl->port);
            break;
        case A800_CTL_CX85_KEY:
            a800session_cx85_key(g_session, ctl->arg, 0);
            break;
        default:
            break;
        }
    }
    g_drag.kind = 0;
    g_press.idx = g_press.key = g_press.target = -1;
    if (any && g_area)
        gtk_widget_queue_draw(g_area);
}

static void stop_capture(void)
{
    if (g_capture_timer) {
        g_source_remove(g_capture_timer);
        g_capture_timer = 0;
    }
    a800session_gamepad_capture_cancel(g_session);
}

static void say_bound(const char *what, const char *stolen)
{
    char msg[260];
    if (stolen && stolen[0])
        g_snprintf(msg, sizeof msg, "Bound %s (was %s)", what, stolen);
    else
        g_snprintf(msg, sizeof msg, "Bound %s", what);
    gtk_label_set_text(GTK_LABEL(g_map_hint), msg);
}

static gboolean capture_tick(gpointer udata)
{
    int button;
    (void)udata;
    if (g_map_state < 0) { g_capture_timer = 0; return G_SOURCE_REMOVE; }
    if (a800session_gamepad_capture_poll(g_session, &button)) {
        char stolen[128];
        stolen[0] = '\0';
        a800session_binding_set_button(g_session, g_map_state, button, stolen, sizeof stolen);
        g_capture_timer = 0;
        set_map_state(-1);     /* stay armed: remapping several in a row is normal */
        say_bound(a800_pad_button_name(button), stolen);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void set_map_state(int state)
{
    g_map_state = state;
    gtk_widget_set_sensitive(g_clear_button, state >= 0);
    if (state == -2) {
        stop_capture();
        gtk_button_set_label(GTK_BUTTON(g_map_button), "Map");
        gtk_widget_remove_css_class(g_map_button, "a800-accent");
        gtk_label_set_text(GTK_LABEL(g_map_hint),
                           "Hold a button, drag the stick or a knob. SHIFT and CTRL latch for the next key. Map rebinds one.");
    } else if (state == -1) {
        stop_capture();
        gtk_button_set_label(GTK_BUTTON(g_map_button), "Done");
        gtk_widget_add_css_class(g_map_button, "a800-accent");
        gtk_label_set_text(GTK_LABEL(g_map_hint),
                           "Click a control to remap it (a quarter of the stick is a direction), or pick one from the list");
    } else {
        char msg[200];
        mouse_release();
        g_snprintf(msg, sizeof msg, "Press a key or a gamepad button for %s (Esc cancels)",
                   a800_target_name(state));
        gtk_label_set_text(GTK_LABEL(g_map_hint), msg);
        a800session_gamepad_capture_begin(g_session);
        if (!g_capture_timer)
            g_capture_timer = g_timeout_add(50, capture_tick, NULL);
    }
    refresh_all();
}

/* The stick under a drag: the pointer's offset from the ring's centre picks the
 * direction (eight sectors); the cap follows what the Atari sees held. */
static void stick_follow(double xpos, double ypos)
{
    a800session_pointer_set(g_session, g_drag.port,
        a800session_stick_from_vector((float)(xpos - g_drag.cenx), (float)(ypos - g_drag.ceny),
                                      (float)g_drag.rad));
    gtk_widget_queue_draw(g_area);
}

/* The knob under a drag: the pointer's angle from the dial's centre. */
static void knob_follow(double xpos, double ypos)
{
    a800session_paddle_pointer(g_session, g_drag.port, g_drag.paddle,
        a800session_knob_from_vector((float)(xpos - g_drag.cenx), (float)(ypos - g_drag.ceny)), 1);
    gtk_widget_queue_draw(g_area);
}

/* A mouse or the Trak-Ball's ball dragged: the pointer's movement in whole
 * counts, the rest carried to the next move. */
static void mat_follow(double xpos, double ypos)
{
    int dx, dy;
    g_drag.carryx += xpos - g_drag.lastx;
    g_drag.carryy += ypos - g_drag.lasty;
    g_drag.lastx = xpos;
    g_drag.lasty = ypos;
    dx = (int)g_drag.carryx;
    dy = (int)g_drag.carryy;
    g_drag.carryx -= dx;
    g_drag.carryy -= dy;
    if (dx || dy)
        a800session_pointer_motion(g_session, dx, dy, pointer_mask(g_drag.port));
    gtk_widget_queue_draw(g_area);
}

/* A pad touched: the point in 0..1 of its surface. The tablet's touch is its
 * button; the Koala Pad's buttons are its own parts'. */
static void pad_follow(double xpos, double ypos)
{
    const a800_control *ctl = &g_lay[g_drag.idx];
    double fu = (xpos - part_x(ctl)) / U(ctl->w), fv = (ypos - part_y(ctl)) / U(ctl->h);
    if (fu < 0) fu = 0;
    if (fu > 1) fu = 1;
    if (fv < 0) fv = 0;
    if (fv > 1) fv = 1;
    g_pad_u = fu;
    g_pad_v = fv;
    g_pad_touch = TRUE;
    a800session_pointer_norm(g_session, (float)fu, (float)fv, 1,
                             ctl->arg == 1 ? 1u : pointer_mask(g_drag.port));
    gtk_widget_queue_draw(g_area);
}

static void drag_follow(double xpos, double ypos)
{
    if (g_drag.kind == 1) stick_follow(xpos, ypos);
    else if (g_drag.kind == 2) knob_follow(xpos, ypos);
    else if (g_drag.kind == 3) mat_follow(xpos, ypos);
    else if (g_drag.kind == 4) pad_follow(xpos, ypos);
}

static void on_area_begin(GtkGestureDrag *gest, double xpos, double ypos, gpointer udata)
{
    const guint button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gest));
    const Hit hit = hit_at(xpos, ypos);
    const a800_control *ctl;
    (void)udata;

    if ((button != GDK_BUTTON_PRIMARY && button != GDK_BUTTON_SECONDARY) || (hit.idx < 0 && hit.key < 0))
        return;
    if (g_map_state != -2) {
        /* armed: picks (or changes) the target; a knob, the mats and the bodies are no target */
        if (button == GDK_BUTTON_PRIMARY && hit.target >= 0)
            set_map_state(hit.target);
        return;
    }
    if (hit.key >= 0) {
        if (button != GDK_BUTTON_PRIMARY) return;
        mouse_release();
        g_press = hit;
        a800session_keyboard_press(g_session, g_keys[hit.key].index, 1);
        gtk_widget_queue_draw(g_area);
        return;
    }
    ctl = &g_lay[hit.idx];
    if (button == GDK_BUTTON_SECONDARY) {
        /* the right button latches a mouse-type device's button, to drag with it down */
        if (ctl->kind == A800_CTL_POINTER_BUTTON && ctl->port >= 0) {
            g_ptr_latched[ctl->port] ^= (unsigned)ctl->arg;
            pointer_send_buttons(ctl->port);
            gtk_widget_queue_draw(g_area);
        }
        return;
    }
    mouse_release();
    g_press = hit;
    switch (ctl->kind) {
    case A800_CTL_STICK:
    case A800_CTL_KNOB:
    case A800_CTL_BALL:
    case A800_CTL_MAT:
    case A800_CTL_PAD:
        g_drag.kind = ctl->kind == A800_CTL_STICK ? 1 : ctl->kind == A800_CTL_KNOB ? 2
                    : ctl->kind == A800_CTL_PAD ? 4 : 3;
        g_drag.port = ctl->port;
        g_drag.paddle = ctl->arg == 1 ? 1 : 0;
        g_drag.idx = hit.idx;
        g_drag.cenx = part_x(ctl) + U(ctl->w) / 2;
        g_drag.ceny = part_y(ctl) + U(ctl->h) / 2;
        g_drag.rad = U(ctl->w) / 2;
        g_drag.lastx = xpos;
        g_drag.lasty = ypos;
        g_drag.carryx = g_drag.carryy = 0;
        drag_follow(xpos, ypos);
        break;
    case A800_CTL_ROUND_BUTTON:
    case A800_CTL_CONSOLE_KEY:
        if (ctl->target >= 0)
            a800session_press(g_session, ctl->target, 1);
        break;
    case A800_CTL_PADDLE_FIRE:
        a800session_paddle_fire(g_session, ctl->port, ctl->arg, 1);
        break;
    case A800_CTL_TURN:
        a800session_paddle_turn(g_session, ctl->port, ctl->arg, ctl->dir);
        break;
    case A800_CTL_POINTER_BUTTON:
        g_ptr_held[ctl->port] |= (unsigned)ctl->arg;
        pointer_send_buttons(ctl->port);
        break;
    case A800_CTL_CX85_KEY:
        a800session_cx85_key(g_session, ctl->arg, 1);
        break;
    default:
        g_press.idx = g_press.key = g_press.target = -1;
        return;
    }
    gtk_widget_queue_draw(g_area);
}

static void on_area_update(GtkGestureDrag *gest, double offx, double offy, gpointer udata)
{
    double startx, starty;
    (void)udata;
    if (!g_drag.kind) return;
    gtk_gesture_drag_get_start_point(gest, &startx, &starty);
    drag_follow(startx + offx, starty + offy);
}

static void on_area_end(GtkGestureDrag *gest, double offx, double offy, gpointer udata)
{
    (void)gest; (void)offx; (void)offy; (void)udata;
    if (g_press.idx >= 0 || g_press.key >= 0 || g_drag.kind)
        mouse_release();
}

static void on_area_cancel(GtkGesture *gest, GdkEventSequence *seq, gpointer udata)
{
    (void)gest; (void)seq; (void)udata;
    mouse_release();
}

/* Dragging off a held button or key lets it go. */
static void on_area_motion(GtkEventControllerMotion *ctrl, double xpos, double ypos, gpointer udata)
{
    const Hit hit = hit_at(xpos, ypos);
    (void)ctrl; (void)udata;
    g_ptr_x = xpos;
    g_ptr_y = ypos;
    if ((g_press.idx >= 0 || g_press.key >= 0) && !g_drag.kind && !same_hit(&hit, &g_press))
        mouse_release();
    if (!same_hit(&hit, &g_hover)) {
        g_hover = hit;
        if (g_map_state != -2)
            gtk_widget_queue_draw(g_area);
    }
}

static void on_area_leave(GtkEventControllerMotion *ctrl, gpointer udata)
{
    (void)ctrl; (void)udata;
    if ((g_press.idx >= 0 || g_press.key >= 0) && !g_drag.kind)
        mouse_release();
    if (g_hover.idx >= 0 || g_hover.key >= 0) {
        g_hover.idx = g_hover.key = g_hover.target = -1;
        gtk_widget_queue_draw(g_area);
    }
}

/* The wheel over a knob nudges it (up turns it left, a higher value); the
 * position stays where it was nudged. */
static gboolean on_area_scroll(GtkEventControllerScroll *ctrl, double delx, double dely,
                               gpointer udata)
{
    Hit hit;
    const a800_control *ctl;
    (void)ctrl; (void)delx; (void)udata;
    if (g_map_state != -2 || g_drag.kind)
        return FALSE;
    hit = hit_at(g_ptr_x, g_ptr_y);
    if (hit.idx < 0) return FALSE;
    ctl = &g_lay[hit.idx];
    if (ctl->kind == A800_CTL_KNOB || ctl->kind == A800_CTL_TURN) {
        const int pad = ctl->arg == 1 ? 1 : 0;
        int val = a800session_paddle_value(g_session, ctl->port, pad) + (dely < 0 ? KNOB_NUDGE : -KNOB_NUDGE);
        if (val > A800_PADDLE_MAX) val = A800_PADDLE_MAX;
        if (val < 0) val = 0;
        a800session_paddle_pointer(g_session, ctl->port, pad, val, 1);
        a800session_paddle_pointer(g_session, ctl->port, pad, val, 0);   /* the position stays */
    } else {
        return FALSE;
    }
    gtk_widget_queue_draw(g_area);
    return TRUE;
}

static void binding_tip(int target, char *text, int textsz)
{
    char key[32];
    const a800_binding bind = a800session_binding_get(g_session, target);
    if (bind.keysym) a800session_keysym_name(bind.keysym, key, sizeof key);
    else g_snprintf(key, sizeof key, "none");
    g_snprintf(text, textsz, "%s\nKey: %s    Gamepad: %s", a800_target_name(target), key,
               bind.button >= 0 ? a800_pad_button_name(bind.button) : "none");
}

static gboolean on_area_tooltip(GtkWidget *widget, int xpos, int ypos, gboolean kbmode,
                                GtkTooltip *tip, gpointer udata)
{
    Hit hit;
    const a800_control *ctl;
    char text[300];
    (void)widget; (void)udata;
    if (kbmode) return FALSE;
    hit = hit_at(xpos, ypos);
    if (hit.key >= 0) {
        const a800_kbd_key *key = &g_keys[hit.key];
        if (key->kind == A800_KEY_SHIFT || key->kind == A800_KEY_CTRL) {
            gtk_tooltip_set_text(tip, "Latches for the next key");
            return TRUE;
        }
        if (hit.target >= 0) {
            binding_tip(hit.target, text, sizeof text);
            gtk_tooltip_set_text(tip, text);
            return TRUE;
        }
        return FALSE;
    }
    if (hit.idx < 0) return FALSE;
    ctl = &g_lay[hit.idx];
    switch (ctl->kind) {
    case A800_CTL_STICK:
        g_snprintf(text, sizeof text, "Press and drag to tilt the stick\nThis quarter: ");
        binding_tip(hit.target, text + strlen(text), (int)(sizeof text - strlen(text)));
        break;
    case A800_CTL_KNOB:
        g_snprintf(text, sizeof text, "Drag to turn the knob, scroll to nudge it\nIt stays where you leave it");
        break;
    case A800_CTL_TURN:
        g_snprintf(text, sizeof text, "Turns the knob while held");
        break;
    case A800_CTL_PADDLE_FIRE:
        g_snprintf(text, sizeof text, "The paddle's fire button");
        break;
    case A800_CTL_POINTER_BUTTON:
        g_snprintf(text, sizeof text, "Hold to press; right-click latches it so a drag moves with it down");
        break;
    case A800_CTL_MAT:
    case A800_CTL_BALL:
        g_snprintf(text, sizeof text, "Drag to move the pointer device");
        break;
    case A800_CTL_PAD:
        g_snprintf(text, sizeof text, "Touch and drag to place the point");
        break;
    case A800_CTL_ROUND_BUTTON:
    case A800_CTL_CONSOLE_KEY:
        if (hit.target < 0) return FALSE;
        binding_tip(hit.target, text, sizeof text);
        break;
    case A800_CTL_CX85_KEY:
        g_snprintf(text, sizeof text, "CX85 key %s", ctl->label);
        break;
    default:
        return FALSE;
    }
    gtk_tooltip_set_text(tip, text);
    return TRUE;
}

/* ---- refresh ---------------------------------------------------------------------- */

static void refresh_all(void)
{
    if (g_area) gtk_widget_queue_draw(g_area);
}

/* Every frame while the window is on screen: what the Atari sees held, painted in
 * the held orange, the knobs' positions and the keyboard's lit keys. Redraws only
 * when something moved. */
static gboolean frame_tick(GtkWidget *widget, GdkFrameClock *clock, gpointer udata)
{
    gboolean changed = FALSE;
    int port, pad, idx;
    unsigned con;
    (void)clock; (void)udata;

    if (reload_layout()) {
        mouse_release();
        size_area();
        changed = TRUE;
    }
    for (port = 0; port < A800_PORTS; port++) {
        const unsigned held = port < g_nports ? a800session_buttons_held(g_session, port) : 0;
        if (held != g_held[port]) {
            g_held[port] = held;
            changed = TRUE;
        }
        for (pad = 0; pad < 2; pad++) {
            const int val = port < g_nports ? a800session_paddle_value(g_session, port, pad)
                                            : A800_PADDLE_MAX;
            if (val != g_paddle[port][pad]) {
                g_paddle[port][pad] = val;
                changed = TRUE;
            }
        }
    }
    con = a800session_console_held(g_session);
    if (con != g_con_held) {
        g_con_held = con;
        changed = TRUE;
    }
    if (g_show_kbd) {
        for (idx = 0; idx < g_nkeys; idx++) {
            const guchar on = a800session_keyboard_held(g_session, g_keys[idx].index) != 0;
            if (on != g_key_held[idx]) {
                g_key_held[idx] = on;
                changed = TRUE;
            }
        }
    }
    if (changed)
        gtk_widget_queue_draw(widget);
    return G_SOURCE_CONTINUE;
}

/* ---- gamepads ------------------------------------------------------------- */

static void refresh_pad_ports(void)
{
    int idx;
    for (idx = 0; idx < g_pad_row_count; idx++) {
        const int eff = a800session_gamepad_effective_port(g_session, idx);
        char text[40];
        if (eff >= 0) g_snprintf(text, sizeof text, "plays port %d", eff + 1);
        else g_snprintf(text, sizeof text, "plays nothing");
        if (strcmp(gtk_label_get_text(GTK_LABEL(g_pad_eff[idx])), text) != 0)
            gtk_label_set_text(GTK_LABEL(g_pad_eff[idx]), text);
    }
}

static void on_assign_changed(GObject *obj, GParamSpec *pspec, gpointer udata)
{
    const int idx = GPOINTER_TO_INT(udata);
    const guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(obj));
    (void)pspec;
    if (g_syncing) return;
    /* 0 = automatic (connection order), 1-4 = that port */
    a800session_gamepad_assign(g_session, idx, sel == 0 ? -1 : (int)sel - 1);
    refresh_pad_ports();
}

/* Rebuilt whenever a pad comes or goes (the session's generation counter): one
 * row per pad, its name, what it plays and the port it is set to. The area is
 * PAD_ROWS rows tall whatever is plugged in (more pads scroll), so the window
 * never changes size. */
static void refresh_pads(void)
{
    static const char *const choices[] = { "Automatic", "Port 1", "Port 2", "Port 3", "Port 4", NULL };
    GtkWidget *child;
    int count, idx;

    g_syncing = TRUE;
    while ((child = gtk_widget_get_first_child(g_pad_box)) != NULL)
        gtk_box_remove(GTK_BOX(g_pad_box), child);
    g_pad_row_count = 0;

    count = a800session_gamepad_count(g_session);
    if (count == 0) {
        GtkWidget *none = gtk_label_new("No gamepads connected - plug one in");
        gtk_widget_add_css_class(none, "dim-label");
        gtk_label_set_xalign(GTK_LABEL(none), 0);
        gtk_widget_set_size_request(none, -1, PAD_ROW_H);
        gtk_widget_set_tooltip_text(none, "A gamepad is picked up as it appears");
        gtk_box_append(GTK_BOX(g_pad_box), none);
        g_syncing = FALSE;
        return;
    }
    for (idx = 0; idx < count && idx < MAX_PADS; idx++) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *name, *drop, *eff;
        char text[128];
        const int assign = a800session_gamepad_assignment(g_session, idx);

        a800session_gamepad_name(g_session, idx, text, sizeof text);
        gtk_widget_set_size_request(row, -1, PAD_ROW_H);
        name = gtk_label_new(text);
        gtk_widget_set_hexpand(name, TRUE);
        gtk_label_set_xalign(GTK_LABEL(name), 0);
        gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(name), 1);
        gtk_widget_set_tooltip_text(name, text);
        eff = gtk_label_new("");
        gtk_widget_add_css_class(eff, "dim-label");
        drop = gtk_drop_down_new_from_strings(choices);
        gtk_drop_down_set_selected(GTK_DROP_DOWN(drop),
                                   assign < 0 || assign >= A800_PORTS ? 0 : (guint)assign + 1);
        gtk_widget_set_focus_on_click(drop, FALSE);
        gtk_widget_set_valign(drop, GTK_ALIGN_CENTER);
        g_signal_connect(drop, "notify::selected", G_CALLBACK(on_assign_changed),
                         GINT_TO_POINTER(idx));
        gtk_box_append(GTK_BOX(row), name);
        gtk_box_append(GTK_BOX(row), eff);
        gtk_box_append(GTK_BOX(row), drop);
        gtk_box_append(GTK_BOX(g_pad_box), row);
        g_pad_eff[g_pad_row_count++] = eff;
    }
    g_syncing = FALSE;
    refresh_pad_ports();
}

/* The pad rows (hot plug, automatic assignment changes) and the keyboard box. */
static gboolean info_tick(gpointer udata)
{
    unsigned gen;
    (void)udata;
    if (!g_window || !gtk_widget_get_visible(g_window)) return G_SOURCE_CONTINUE;

    gen = a800session_gamepad_generation(g_session);
    if (gen != g_pad_generation) {
        g_pad_generation = gen;
        refresh_pads();
    } else {
        refresh_pad_ports();
    }
    return G_SOURCE_CONTINUE;
}

/* ---- keyboard ------------------------------------------------------------- */

static gboolean on_key(guint keyval, guint code, GdkModifierType mods, gboolean down)
{
    /* an open popover (a drop-down's list) has the arrows and Return */
    GtkWidget *focus = gtk_root_get_focus(GTK_ROOT(g_window));
    if (focus && gtk_widget_get_ancestor(focus, GTK_TYPE_POPOVER))
        return FALSE;

    /* F9, F11, F12 and Ctrl chords are never Map's: they go where they always do */
    if (g_map_state != -2 && !(keyval == GDK_KEY_F9 || keyval == GDK_KEY_F11 ||
                               keyval == GDK_KEY_F12 || (mods & GDK_CONTROL_MASK))) {
        if (g_swallow_code && code == g_swallow_code) {
            if (!down) g_swallow_code = 0;
            return TRUE;
        }
        if (!down)
            return TRUE;
        if (keyval == GDK_KEY_Escape) {
            g_swallow_code = code;
            set_map_state(g_map_state >= 0 ? -1 : -2);
            return TRUE;
        }
        if (g_map_state < 0)
            return TRUE;              /* armed, no target yet: typing is not typed */
        {
            guint32 keysym = code >= 8 ? a800session_keysym_from_evdev(code - 8) : 0;
            char stolen[128], name[32];
            if (!keysym) keysym = keyval;
            if (a800session_key_reserved(keysym)) {
                gtk_label_set_text(GTK_LABEL(g_map_hint),
                                   "F9, F11 and F12 belong to the app: they cannot be bound");
                return TRUE;
            }
            stolen[0] = '\0';
            a800session_binding_set_key(g_session, g_map_state, keysym, stolen, sizeof stolen);
            a800session_keysym_name(keysym, name, sizeof name);
            g_swallow_code = code;
            set_map_state(-1);        /* stay armed: remapping several in a row is normal */
            say_bound(name, stolen);
            return TRUE;
        }
    }
    /* Otherwise exactly what the main window does with it. */
    if (!g_parent) return FALSE;
    return a800_window_key(A800_WINDOW(g_parent), keyval, code, mods, down);
}

static gboolean on_key_pressed(GtkEventControllerKey *ctrl, guint keyval,
                               guint code, GdkModifierType mods, gpointer udata)
{
    (void)ctrl; (void)udata;
    return on_key(keyval, code, mods, TRUE);
}

static void on_key_released(GtkEventControllerKey *ctrl, guint keyval,
                            guint code, GdkModifierType mods, gpointer udata)
{
    (void)ctrl; (void)udata;
    on_key(keyval, code, mods, FALSE);
}

/* Keys typed here went through the main window's key path (the keyboard source),
 * so a key held into a focus change must be let go too, with the mouse's. */
static void on_focus_leave(GtkEventControllerFocus *ctrl, gpointer udata)
{
    (void)ctrl; (void)udata;
    mouse_release();
    a800session_release_all(g_session);
    g_swallow_code = 0;     /* its release went elsewhere */
}

static void on_active_changed(GObject *obj, GParamSpec *pspec, gpointer udata)
{
    (void)pspec; (void)udata;
    if (!gtk_window_is_active(GTK_WINDOW(obj))) {
        mouse_release();
        a800session_release_all(g_session);
    }
}

/* ---- the window ----------------------------------------------------------- */

static void on_map_clicked(GtkButton *button, gpointer udata)
{
    (void)button; (void)udata;
    set_map_state(g_map_state == -2 ? -1 : -2);
}

static void on_defaults_clicked(GtkButton *button, gpointer udata)
{
    (void)button; (void)udata;
    a800session_bindings_reset(g_session);
    gtk_label_set_text(GTK_LABEL(g_map_hint), "Bindings back to their defaults");
    refresh_all();
}

/* Takes the picked control's key and gamepad button away. */
static void on_clear_clicked(GtkButton *button, gpointer udata)
{
    (void)button; (void)udata;
    if (g_map_state < 0) return;
    a800session_binding_clear(g_session, g_map_state);
    gtk_label_set_text(GTK_LABEL(g_map_hint),
                       "Cleared. Press a key or a gamepad button to bind it again");
    refresh_all();
}

static void on_show_bindings(GtkCheckButton *button, gpointer udata)
{
    (void)udata;
    a800session_set_int(g_session, "controllers_show_bindings",
                        gtk_check_button_get_active(button) ? 1 : 0);
    refresh_all();
}

/* The keyboard under the ports: shown or hidden, remembered. The window follows
 * the drawing's size. */
static void on_keyboard_toggled(GtkCheckButton *button, gpointer udata)
{
    (void)udata;
    if (g_syncing) return;
    mouse_release();
    g_show_kbd = gtk_check_button_get_active(button);
    a800session_set_int(g_session, "controllers_keyboard", g_show_kbd ? 1 : 0);
    size_area();
    refresh_all();
}

/* "Remap a control from the list...": any of the targets, for the ones the picture
 * cannot show (the system actions: power cycle, reboot to CONFIG, stop, warp). */
static void on_target_picked(GObject *obj, GParamSpec *pspec, gpointer udata)
{
    const guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(obj));
    (void)pspec; (void)udata;
    if (g_syncing || sel == 0) return;
    g_syncing = TRUE;
    gtk_drop_down_set_selected(GTK_DROP_DOWN(obj), 0);
    g_syncing = FALSE;
    set_map_state((int)sel - 1);
}

static void hide_window(void)
{
    set_map_state(-2);
    mouse_release();
    a800session_release_all(g_session);
    g_swallow_code = 0;
    gtk_widget_set_visible(g_window, FALSE);
}

static gboolean on_close(GtkWindow *window, gpointer udata)
{
    (void)window; (void)udata;
    hide_window();
    return TRUE;
}

/* The pad line's timer runs only while the window is on screen: closing only
 * hides it, and a hidden window has nobody to update. (The per-frame tick stops by
 * itself while the drawing is unmapped.) */
static void on_window_map(GtkWidget *widget, gpointer udata)
{
    (void)widget; (void)udata;
    if (!g_info_timer) g_info_timer = g_timeout_add(250, info_tick, NULL);
    info_tick(NULL);
}

static void on_window_unmap(GtkWidget *widget, gpointer udata)
{
    (void)widget; (void)udata;
    if (g_info_timer) g_source_remove(g_info_timer);
    g_info_timer = 0;
}

static void on_destroy(GtkWidget *widget, gpointer udata)
{
    (void)widget; (void)udata;
    if (g_capture_timer) g_source_remove(g_capture_timer);
    if (g_info_timer) g_source_remove(g_info_timer);
    g_capture_timer = g_info_timer = 0;
    g_area = NULL;
    g_window = NULL;
    g_pad_box = NULL;
    g_pad_row_count = 0;
    g_map_state = -2;
    g_hover.idx = g_hover.key = g_press.idx = g_press.key = -1;
    g_drag.kind = 0;
    g_n = 0;
}

static void build_window(GtkWindow *parent)
{
    GtkWidget *root, *maprow, *toolbar, *header, *defaults, *padscroll;
    GtkEventController *keys, *focus, *motion, *scroll;
    GtkGesture *drag;
    GtkStringList *targets;
    int target;

    g_n = 0;
    g_machine = -1;
    memset(g_held, 0, sizeof g_held);
    memset(g_key_held, 0, sizeof g_key_held);
    memset(g_ptr_held, 0, sizeof g_ptr_held);
    memset(g_ptr_latched, 0, sizeof g_ptr_latched);
    for (target = 0; target < A800_PORTS; target++)
        g_paddle[target][0] = g_paddle[target][1] = A800_PADDLE_MAX;
    g_con_held = 0;
    g_hover.idx = g_hover.key = g_press.idx = g_press.key = -1;
    g_drag.kind = 0;
    reload_layout();
    g_show_kbd = keyboard_saved();

    g_window = adw_window_new();
    gtk_window_set_title(GTK_WINDOW(g_window), "Controllers");
    if (parent) {
        gtk_window_set_transient_for(GTK_WINDOW(g_window), parent);
        /* Same app-id as the main window, so the shell groups and icons it. */
        gtk_window_set_application(GTK_WINDOW(g_window), gtk_window_get_application(parent));
    }
    gtk_window_set_destroy_with_parent(GTK_WINDOW(g_window), TRUE);
    /* min == max: Wayland compositors float it */
    gtk_window_set_resizable(GTK_WINDOW(g_window), FALSE);
    g_signal_connect(g_window, "close-request", G_CALLBACK(on_close), NULL);
    g_signal_connect(g_window, "destroy", G_CALLBACK(on_destroy), NULL);
    g_signal_connect(g_window, "notify::is-active", G_CALLBACK(on_active_changed), NULL);

    /* the controllers */
    g_area = gtk_drawing_area_new();
    size_area();
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(g_area), draw_area, NULL, NULL);
    gtk_widget_set_halign(g_area, GTK_ALIGN_CENTER);
    /* Not focusable: clicking a control must not steal focus. */
    gtk_widget_set_focusable(g_area, FALSE);
    gtk_widget_set_has_tooltip(g_area, TRUE);
    g_signal_connect(g_area, "query-tooltip", G_CALLBACK(on_area_tooltip), NULL);
    drag = gtk_gesture_drag_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), 0);
    g_signal_connect(drag, "drag-begin", G_CALLBACK(on_area_begin), NULL);
    g_signal_connect(drag, "drag-update", G_CALLBACK(on_area_update), NULL);
    g_signal_connect(drag, "drag-end", G_CALLBACK(on_area_end), NULL);
    g_signal_connect(drag, "cancel", G_CALLBACK(on_area_cancel), NULL);
    gtk_widget_add_controller(g_area, GTK_EVENT_CONTROLLER(drag));
    motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion", G_CALLBACK(on_area_motion), NULL);
    g_signal_connect(motion, "leave", G_CALLBACK(on_area_leave), NULL);
    gtk_widget_add_controller(g_area, motion);
    scroll = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    g_signal_connect(scroll, "scroll", G_CALLBACK(on_area_scroll), NULL);
    gtk_widget_add_controller(g_area, scroll);
    gtk_widget_add_tick_callback(g_area, frame_tick, NULL, NULL);

    root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top(root, 12);
    gtk_widget_set_margin_bottom(root, 12);
    gtk_widget_set_margin_start(root, 12);
    gtk_widget_set_margin_end(root, 12);
    gtk_box_append(GTK_BOX(root), g_area);

    /* the pad rows: a fixed-height area, so the window never resizes */
    g_pad_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, PAD_GAP);
    padscroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(padscroll), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(padscroll), FALSE);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(padscroll),
        PAD_ROWS * PAD_ROW_H + (PAD_ROWS - 1) * PAD_GAP);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(padscroll),
        PAD_ROWS * PAD_ROW_H + (PAD_ROWS - 1) * PAD_GAP);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(padscroll), g_pad_box);
    gtk_box_append(GTK_BOX(root), padscroll);
    g_pad_generation = a800session_gamepad_generation(g_session);
    refresh_pads();

    maprow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    g_map_button = gtk_button_new_with_label("Map");
    gtk_widget_set_size_request(g_map_button, 72, -1);
    gtk_widget_set_focus_on_click(g_map_button, FALSE);
    g_signal_connect(g_map_button, "clicked", G_CALLBACK(on_map_clicked), NULL);
    g_clear_button = gtk_button_new_with_label("Clear");
    gtk_widget_set_focus_on_click(g_clear_button, FALSE);
    gtk_widget_set_tooltip_text(g_clear_button, "Take the picked control's key and gamepad button away");
    g_signal_connect(g_clear_button, "clicked", G_CALLBACK(on_clear_clicked), NULL);
    defaults = gtk_button_new_with_label("Defaults");
    gtk_widget_set_focus_on_click(defaults, FALSE);
    g_signal_connect(defaults, "clicked", G_CALLBACK(on_defaults_clicked), NULL);
    g_show_bindings = gtk_check_button_new_with_label("Show bindings");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(g_show_bindings), show_bindings_saved());
    gtk_widget_set_focus_on_click(g_show_bindings, FALSE);
    g_signal_connect(g_show_bindings, "toggled", G_CALLBACK(on_show_bindings), NULL);
    g_kbd_check = gtk_check_button_new_with_label("Keyboard");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(g_kbd_check), g_show_kbd);
    gtk_widget_set_focus_on_click(g_kbd_check, FALSE);
    gtk_widget_set_tooltip_text(g_kbd_check, "Show the machine's keyboard under the ports");
    g_signal_connect(g_kbd_check, "toggled", G_CALLBACK(on_keyboard_toggled), NULL);
    targets = gtk_string_list_new(NULL);
    gtk_string_list_append(targets, "Remap a control from the list...");
    for (target = 0; target < A800_TARGET_COUNT; target++)
        gtk_string_list_append(targets, a800_target_name(target));
    g_target_drop = gtk_drop_down_new(G_LIST_MODEL(targets), NULL);
    gtk_widget_set_focus_on_click(g_target_drop, FALSE);
    gtk_widget_set_hexpand(g_target_drop, TRUE);
    gtk_widget_set_tooltip_text(g_target_drop,
        "Any of the 30 controls (the power, reboot, stop and warp actions too): pick one, then press its new key or gamepad button");
    g_signal_connect(g_target_drop, "notify::selected", G_CALLBACK(on_target_picked), NULL);
    gtk_box_append(GTK_BOX(maprow), g_map_button);
    gtk_box_append(GTK_BOX(maprow), g_clear_button);
    gtk_box_append(GTK_BOX(maprow), defaults);
    gtk_box_append(GTK_BOX(maprow), g_show_bindings);
    gtk_box_append(GTK_BOX(maprow), g_kbd_check);
    gtk_box_append(GTK_BOX(maprow), g_target_drop);
    gtk_box_append(GTK_BOX(root), maprow);

    g_map_hint = gtk_label_new("");
    gtk_widget_add_css_class(g_map_hint, "dim-label");
    gtk_widget_set_hexpand(g_map_hint, TRUE);
    gtk_label_set_xalign(GTK_LABEL(g_map_hint), 0);
    /* a long hint wraps onto a second line rather than clip its "Esc cancels"
     * or widen the window; two lines are always reserved, so the window never
     * changes height either */
    gtk_label_set_wrap(GTK_LABEL(g_map_hint), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(g_map_hint), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_lines(GTK_LABEL(g_map_hint), 2);
    gtk_label_set_ellipsize(GTK_LABEL(g_map_hint), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(g_map_hint), 1);
    gtk_label_set_yalign(GTK_LABEL(g_map_hint), 0);
    {
        PangoLayout *twolines = gtk_widget_create_pango_layout(g_map_hint, "A\nA");
        int linesw, linesh;
        pango_layout_get_pixel_size(twolines, &linesw, &linesh);
        g_object_unref(twolines);
        gtk_widget_set_size_request(g_map_hint, -1, linesh);
    }
    gtk_box_append(GTK_BOX(root), g_map_hint);

    header = adw_header_bar_new();
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header),
                                    adw_window_title_new("Controllers", "Atari 800"));
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), a800_help_button("controllers"));
    a800_help_set_context(GTK_WINDOW(g_window), "controllers");
    toolbar = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), root);
    adw_window_set_content(ADW_WINDOW(g_window), toolbar);

    keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key_pressed), NULL);
    g_signal_connect(keys, "key-released", G_CALLBACK(on_key_released), NULL);
    gtk_widget_add_controller(g_window, keys);

    focus = gtk_event_controller_focus_new();
    g_signal_connect(focus, "leave", G_CALLBACK(on_focus_leave), NULL);
    gtk_widget_add_controller(g_window, focus);

    set_map_state(-2);
    g_signal_connect(g_window, "map", G_CALLBACK(on_window_map), NULL);
    g_signal_connect(g_window, "unmap", G_CALLBACK(on_window_unmap), NULL);
}

static void ensure_window(GtkWindow *parent, a800session *session)
{
    g_session = session;
    if (parent)
        g_parent = parent;
    if (!g_window)
        build_window(g_parent);
}

void a800_controllers_show(GtkWindow *parent, a800session *session)
{
    ensure_window(parent, session);
    gtk_window_present(GTK_WINDOW(g_window));
}

void a800_controllers_toggle(GtkWindow *parent, a800session *session)
{
    ensure_window(parent, session);
    if (gtk_widget_get_visible(g_window))
        hide_window();
    else
        gtk_window_present(GTK_WINDOW(g_window));
}

gboolean a800_controllers_visible(void)
{
    return g_window && gtk_widget_get_visible(g_window);
}

/* The machine or a port's device changed in Preferences: the next frame reloads
 * the layout and the keyboard (frame_tick); a hidden window reloads when shown. */
void a800_controllers_refresh(void)
{
    if (!g_window)
        return;
    g_machine = -1;
}
