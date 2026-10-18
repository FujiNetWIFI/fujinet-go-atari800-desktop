/*
 * The Controllers panel -- the on-screen Atari keyboard (drawn from the
 * session's table, so every frontend shows the same keys for the machine:
 * HELP on the XL/XE, F1-F4 on the 1200XL), the controller ports with what
 * is plugged into each (a stick and fire for a joystick, two dials for
 * paddles), the gamepads and their ports, and Map mode.
 *
 * Every key and control is pressed with a raw GtkGestureClick (press AND
 * release), not GtkButton's "clicked": the Atari scans its keyboard and
 * samples the sticks once per frame, so a press present only for the
 * instant of a click falls between frames and a polling program never sees
 * it. A key on screen is HELD for as long as the mouse button is down,
 * exactly like the real one. SHIFT and CTRL latch for the next key instead:
 * a mouse cannot hold two keys at once.
 *
 * The gesture's "cancel" is wired to the same release path as "released":
 * dragging off a key must not leave the Atari believing it is still down.
 * Losing the focus, hiding and rebuilding let go of whatever the mouse holds
 * here (only that: the main window releases its own keys).
 *
 * What the Atari sees held -- host keyboard, gamepads and this panel
 * together (a800session_keyboard_held, a800session_buttons_held) -- is lit
 * (.a800-held), so the panel doubles as an input tester for a new gamepad.
 *
 * Keys typed with the panel active go through the main window
 * (a800_window_key), so the Atari, F9 and the shortcuts behave the same
 * whichever window has the focus.
 *
 * FIXED SIZE: keys sit at KBD_UNIT pixels per key unit and every control has
 * a fixed size with labels that wrap (then ellipsize) rather than grow, so
 * neither Map mode nor a long gamepad name resizes the window, and tiling
 * compositors float it. The keyboard and the ports are rebuilt when the machine or a
 * port's device changes.
 *
 * SINGLETON, hidden rather than destroyed, so a remap survives closing it.
 *
 * MAP MODE: the Map button arms a two-step rebind -- click a joystick
 * control or a console key (RESET, OPTION, SELECT, START, HELP, BREAK) to
 * pick the target, then press a keyboard key OR a gamepad button. While
 * armed, the same gestures pick the target instead of pressing, the keys
 * that cannot be remapped are greyed, the key handler intercepts the next
 * keystroke, and a timer polls the session's gamepad capture. Rebinding
 * steals the key from whatever held it.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "controllers_window.h"

#include "../window.h"

#include <string.h>

#define KBD_UNIT 46             /* pixels per key unit */
#define KBD_GAP 3               /* between key caps */
#define CELL_W 60               /* a joystick control: three lines of Map label fit */
#define CELL_H 58
#define CELL_GAP 2
#define PORT_W (3 * CELL_W + 2 * CELL_GAP)
#define PADDLE_MAX 228
#define MAX_CONTROLS 128
#define MAX_PADS 8

/* ---- singleton state ------------------------------------------------------ */

static GtkWidget *g_window;
static GtkWindow *g_parent;     /* the A800Window: typed keys go through it */
static a800session *g_session;

/* Map mode: -2 idle, -1 armed and waiting for a target, >= 0 waiting for a
 * key or pad button to bind to that target. */
static int g_map_state = -2;
static GtkWidget *g_map_button;
static GtkWidget *g_map_hint;
static guint g_capture_timer;
static guint g_swallow_code;    /* the key Map took: its repeats and release too */
static guint g_held_timer;
static guint g_pad_timer;

static GtkWidget *g_kbd;        /* the key caps, placed in key units */
static GtkWidget *g_ports;      /* one frame per port */
static GtkWidget *g_pad_box;
static GtkWidget *g_pad_port[MAX_PADS];   /* each pad's "drives port N" */
static int g_npad_rows;
static unsigned g_pad_generation = (unsigned)-1;

/* what the keyboard and the ports were built for */
static int g_built_machine = -1;
static int g_built_ports;
static int g_built_dev[A800_PORTS];

/* what the mouse holds here */
static int g_mouse_key = -1;    /* on-screen keyboard index */
static int g_mouse_target = -1; /* joystick target */
static GtkWidget *g_paddle_w[A800_PORTS][2][2];   /* [port][paddle]: dial, fire */
static int g_paddle_value[A800_PORTS][2];
static gboolean g_paddle_fire[A800_PORTS][2];
static gboolean g_paddles_init;

typedef struct {
    GtkWidget *button;
    GtkWidget *label;
    int key;                    /* on-screen keyboard index, or -1 */
    int target;                 /* what Map rebinds, or -1 */
    gboolean lit;
} control;

static control g_controls[MAX_CONTROLS];
static int g_ncontrols;

static void refresh_labels(void);

/* Panel-only layout: key caps and controls without the theme's padding and
 * minimum sizes, so they are exactly the size given. The colours are the
 * app's (.a800-held, .a800-accent from main.c). */
static void install_css(void)
{
    static gboolean done;
    GtkCssProvider *css;
    if (done) return;
    done = TRUE;
    css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css,
        "button.a800-key { padding: 0 1px; min-width: 0; min-height: 0;"
        " font-size: 8pt; font-weight: bold; }\n"
        "button.a800-ctl { padding: 2px 3px; min-width: 0; min-height: 0; }\n");
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
        GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);
}

/* ---- pressing ------------------------------------------------------------- */

static void set_lit(GtkWidget *w, gboolean on)
{
    if (on) gtk_widget_add_css_class(w, "a800-held");
    else gtk_widget_remove_css_class(w, "a800-held");
}

static void paddle_send(int port, int p)
{
    a800session_paddle(g_session, port, p, g_paddle_value[port][p], g_paddle_fire[port][p]);
}

/* Lets go of everything the mouse holds in this panel. */
static void mouse_release(void)
{
    int port, p;
    if (g_mouse_key >= 0) {
        a800session_keyboard_press(g_session, g_mouse_key, 0);
        g_mouse_key = -1;
    }
    if (g_mouse_target >= 0) {
        a800session_press(g_session, g_mouse_target, 0);
        g_mouse_target = -1;
    }
    for (port = 0; port < A800_PORTS; port++)
        for (p = 0; p < 2; p++)
            if (g_paddle_fire[port][p]) {
                g_paddle_fire[port][p] = FALSE;
                paddle_send(port, p);
                if (g_paddle_w[port][p][1]) set_lit(g_paddle_w[port][p][1], FALSE);
            }
}

static void control_press(control *c, int down)
{
    if (c->key >= 0) {
        if (down) {
            mouse_release();
            g_mouse_key = c->key;
            a800session_keyboard_press(g_session, c->key, 1);
        } else if (g_mouse_key == c->key) {
            g_mouse_key = -1;
            a800session_keyboard_press(g_session, c->key, 0);
        }
    } else if (c->target >= 0) {
        if (down) {
            mouse_release();
            g_mouse_target = c->target;
            a800session_press(g_session, c->target, 1);
        } else if (g_mouse_target == c->target) {
            g_mouse_target = -1;
            a800session_press(g_session, c->target, 0);
        }
    }
}

static void stop_capture(void)
{
    if (g_capture_timer) {
        g_source_remove(g_capture_timer);
        g_capture_timer = 0;
    }
    a800session_gamepad_capture_cancel(g_session);
}

static gboolean capture_tick(gpointer d)
{
    int button;
    (void)d;
    if (g_map_state < 0) { g_capture_timer = 0; return G_SOURCE_REMOVE; }
    if (a800session_gamepad_capture_poll(g_session, &button)) {
        char stolen[128], msg[200];
        a800session_binding_set_button(g_session, g_map_state, button, stolen, sizeof stolen);
        if (stolen[0])
            g_snprintf(msg, sizeof msg, "Bound %s (was %s)", a800_pad_button_name(button), stolen);
        else
            g_snprintf(msg, sizeof msg, "Bound %s", a800_pad_button_name(button));
        g_capture_timer = 0;
        g_map_state = -1;     /* stay armed: remapping several in a row is normal */
        gtk_label_set_text(GTK_LABEL(g_map_hint), msg);
        refresh_labels();
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void set_map_state(int state)
{
    g_map_state = state;
    if (state == -2) {
        stop_capture();
        gtk_button_set_label(GTK_BUTTON(g_map_button), "Map");
        gtk_widget_remove_css_class(g_map_button, "a800-accent");
        gtk_label_set_text(GTK_LABEL(g_map_hint),
                           "Click and hold to press. SHIFT and CTRL latch for the next key.");
    } else if (state == -1) {
        stop_capture();
        gtk_button_set_label(GTK_BUTTON(g_map_button), "Done");
        gtk_widget_add_css_class(g_map_button, "a800-accent");
        gtk_label_set_text(GTK_LABEL(g_map_hint), "Click a joystick control or console key to remap");
    } else {
        char msg[128];
        mouse_release();
        g_snprintf(msg, sizeof msg, "Press a key or gamepad button for %s",
                   a800_target_name(state));
        gtk_label_set_text(GTK_LABEL(g_map_hint), msg);
        a800session_gamepad_capture_begin(g_session);
        if (!g_capture_timer)
            g_capture_timer = g_timeout_add(50, capture_tick, NULL);
    }
    refresh_labels();
}

static void on_pressed(GtkGestureClick *g, int n, double x, double y, gpointer user_data)
{
    control *c = &g_controls[GPOINTER_TO_INT(user_data)];
    (void)g; (void)n; (void)x; (void)y;

    if (g_map_state != -2) {
        /* armed: picks (or changes) the target */
        if (c->target >= 0) set_map_state(c->target);
        return;
    }
    control_press(c, 1);
    c->lit = TRUE;
    set_lit(c->button, TRUE);
}

static void on_released(GtkGestureClick *g, int n, double x, double y, gpointer user_data)
{
    (void)g; (void)n; (void)x; (void)y;
    control_press(&g_controls[GPOINTER_TO_INT(user_data)], 0);
}

static void on_cancelled(GtkGesture *g, GdkEventSequence *seq, gpointer user_data)
{
    (void)g; (void)seq;
    control_press(&g_controls[GPOINTER_TO_INT(user_data)], 0);
}

/* A fixed-size button whose label ellipsizes instead of widening it. face
 * is Pango markup. */
static GtkWidget *control_new(const char *face, int key, int target, int w, int h,
                              const char *css_class)
{
    GtkWidget *b = gtk_button_new();
    GtkWidget *l = gtk_label_new(NULL);
    GtkGesture *g;
    int i;

    /* Up to three lines (a key and a two-word pad button wrap to fit);
     * only a name longer than that ellipsizes (its tooltip has it whole). */
    gtk_label_set_markup(GTK_LABEL(l), face);
    gtk_label_set_justify(GTK_LABEL(l), GTK_JUSTIFY_CENTER);
    gtk_label_set_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(l), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_lines(GTK_LABEL(l), 3);
    gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(l), 1);    /* the button's size rules */
    gtk_button_set_child(GTK_BUTTON(b), l);
    gtk_widget_set_size_request(b, w, h);
    gtk_widget_set_focusable(b, FALSE);
    gtk_widget_set_focus_on_click(b, FALSE);
    gtk_widget_add_css_class(b, css_class);
    g_object_set_data_full(G_OBJECT(b), "face", g_strdup(face), g_free);

    if (g_ncontrols >= MAX_CONTROLS)
        return b;
    i = g_ncontrols++;
    g_controls[i].button = b;
    g_controls[i].label = l;
    g_controls[i].key = key;
    g_controls[i].target = target;
    g_controls[i].lit = FALSE;

    g = gtk_gesture_click_new();
    g_signal_connect(g, "pressed", G_CALLBACK(on_pressed), GINT_TO_POINTER(i));
    g_signal_connect(g, "released", G_CALLBACK(on_released), GINT_TO_POINTER(i));
    g_signal_connect(g, "cancel", G_CALLBACK(on_cancelled), GINT_TO_POINTER(i));
    gtk_widget_add_controller(b, GTK_EVENT_CONTROLLER(g));
    return b;
}

/* In Map mode each remappable control shows what is bound to it, so choosing
 * what to change does not require remembering the whole table; the keys
 * that cannot be remapped are greyed. */
static void refresh_labels(void)
{
    int i;
    for (i = 0; i < g_ncontrols; i++) {
        control *c = &g_controls[i];
        gboolean key_cap = c->key >= 0;
        c->lit = FALSE;
        set_lit(c->button, FALSE);
        if (g_map_state != -2 && c->target >= 0) {
            a800_binding bind = a800session_binding_get(g_session, c->target);
            char key[32], text[96], tip[160];
            a800session_keysym_name(bind.keysym, key, sizeof key);
            if (bind.button != A800_PAD_BTN_NONE) {
                /* U+2028, not \n: one paragraph, so the label's three-line
                 * cap counts both names */
                g_snprintf(text, sizeof text, "%s\xe2\x80\xa8%s", *key ? key : "\xe2\x80\x94",
                           a800_pad_button_name(bind.button));
                g_snprintf(tip, sizeof tip, "%s: %s, %s", a800_target_name(c->target),
                           *key ? key : "no key", a800_pad_button_name(bind.button));
            } else {
                g_snprintf(text, sizeof text, "%s", *key ? key : "\xe2\x80\x94");
                g_snprintf(tip, sizeof tip, "%s: %s", a800_target_name(c->target),
                           *key ? key : "no key");
            }
            gtk_label_set_text(GTK_LABEL(c->label), text);
            if (!key_cap) gtk_widget_add_css_class(c->label, "caption");
            gtk_widget_set_tooltip_text(c->button, tip);
            if (c->target == g_map_state) gtk_widget_add_css_class(c->button, "a800-accent");
            else gtk_widget_remove_css_class(c->button, "a800-accent");
            gtk_widget_set_sensitive(c->button, TRUE);
        } else {
            gtk_label_set_markup(GTK_LABEL(c->label), g_object_get_data(G_OBJECT(c->button), "face"));
            gtk_widget_remove_css_class(c->label, "caption");
            gtk_widget_set_tooltip_text(c->button, g_object_get_data(G_OBJECT(c->button), "tip"));
            gtk_widget_remove_css_class(c->button, "a800-accent");
            gtk_widget_set_sensitive(c->button, g_map_state == -2);
        }
    }
    {
        int port, p, w;
        for (port = 0; port < A800_PORTS; port++)
            for (p = 0; p < 2; p++)
                for (w = 0; w < 2; w++)
                    if (g_paddle_w[port][p][w])
                        gtk_widget_set_sensitive(g_paddle_w[port][p][w], g_map_state == -2);
    }
}

/* What the Atari sees held, lit. */
static gboolean held_tick(gpointer d)
{
    unsigned held[A800_PORTS];
    int i, port;
    (void)d;
    if (!g_window || !gtk_widget_get_visible(g_window) || g_map_state != -2)
        return G_SOURCE_CONTINUE;
    for (port = 0; port < A800_PORTS; port++)
        held[port] = a800session_buttons_held(g_session, port);
    for (i = 0; i < g_ncontrols; i++) {
        control *c = &g_controls[i];
        gboolean on = FALSE;
        if (c->key >= 0)
            on = a800session_keyboard_held(g_session, c->key) != 0;
        else if (c->target >= 0 && c->target < A800_TARGET_CONSOLE(0))
            on = (held[c->target / A800_ACT_PER_PORT] >> (c->target % A800_ACT_PER_PORT)) & 1;
        if (on != c->lit) {
            c->lit = on;
            set_lit(c->button, on);
        }
    }
    return G_SOURCE_CONTINUE;
}

/* ---- the keyboard ---------------------------------------------------------- */

/* The keys that are console keys as far as Map is concerned. The layout
 * table carries labels only, so they are matched by label. */
static int console_target(const char *label)
{
    static const struct { const char *label; int con; } con[] = {
        { "RESET", A800_CON_RESET }, { "OPTION", A800_CON_OPTION },
        { "SELECT", A800_CON_SELECT }, { "START", A800_CON_START },
        { "HELP", A800_CON_HELP }, { "BREAK", A800_CON_BREAK },
    };
    size_t i;
    for (i = 0; i < G_N_ELEMENTS(con); i++)
        if (!strcmp(label, con[i].label))
            return A800_TARGET_CONSOLE(con[i].con);
    return -1;
}

static int px(float units)
{
    return (int)(units * KBD_UNIT + 0.5f);
}

static void build_keyboard(void)
{
    const a800_kbd_key *keys = NULL;
    int n = a800session_keyboard_layout(g_session, &keys), i;
    float ew = 0, eh = 0;

    for (i = 0; i < n; i++) {
        const a800_kbd_key *k = &keys[i];
        char *face;
        GtkWidget *b;

        if (k->shifted)
            face = g_markup_printf_escaped("<span size=\"smaller\" weight=\"normal\">%s</span>\n%s",
                                           k->shifted, k->label);
        else
            face = g_markup_escape_text(k->label, -1);
        b = control_new(face, k->index, console_target(k->label),
                        px(k->x + k->w) - px(k->x) - KBD_GAP,
                        px(k->y + k->h) - px(k->y) - KBD_GAP, "a800-key");
        g_free(face);
        if (!strcmp(k->label, "SHIFT") || !strcmp(k->label, "CTRL"))
            g_object_set_data(G_OBJECT(b), "tip", (gpointer)"Latches for the next key");
        gtk_widget_set_tooltip_text(b, g_object_get_data(G_OBJECT(b), "tip"));
        gtk_fixed_put(GTK_FIXED(g_kbd), b, px(k->x) + KBD_GAP / 2.0, px(k->y) + KBD_GAP / 2.0);
        if (k->x + k->w > ew) ew = k->x + k->w;
        if (k->y + k->h > eh) eh = k->y + k->h;
    }
    gtk_widget_set_size_request(g_kbd, px(ew), px(eh));
}

/* ---- the ports ----------------------------------------------------------------- */

static void on_paddle_changed(GtkRange *r, gpointer user_data)
{
    int line = GPOINTER_TO_INT(user_data);
    g_paddle_value[line / 2][line % 2] = (int)gtk_range_get_value(r);
    paddle_send(line / 2, line % 2);
}

static void on_paddle_fire(GtkGestureClick *g, int n, double x, double y, gpointer user_data)
{
    int line = GPOINTER_TO_INT(user_data);
    (void)n; (void)x; (void)y;
    if (g_map_state != -2) return;
    g_paddle_fire[line / 2][line % 2] = TRUE;
    paddle_send(line / 2, line % 2);
    set_lit(gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g)), TRUE);
}

static void paddle_fire_off(GtkWidget *w, int line)
{
    if (!g_paddle_fire[line / 2][line % 2]) return;
    g_paddle_fire[line / 2][line % 2] = FALSE;
    paddle_send(line / 2, line % 2);
    set_lit(w, FALSE);
}

static void on_paddle_fire_released(GtkGestureClick *g, int n, double x, double y, gpointer user_data)
{
    (void)n; (void)x; (void)y;
    paddle_fire_off(gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g)),
                    GPOINTER_TO_INT(user_data));
}

static void on_paddle_fire_cancelled(GtkGesture *g, GdkEventSequence *seq, gpointer user_data)
{
    (void)seq;
    paddle_fire_off(gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g)),
                    GPOINTER_TO_INT(user_data));
}

/* A pair of paddles: a dial (turning right lowers the value, as clockwise
 * does) and a fire button each, numbered as Atari BASIC's PADDLE() does. */
static GtkWidget *build_paddles(int port)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    int p;

    for (p = 0; p < 2; p++) {
        int line = port * 2 + p;
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        GtkWidget *name, *dial, *fire;
        GtkGesture *g;
        char text[32];

        g_snprintf(text, sizeof text, "Paddle %d", line);
        name = gtk_label_new(text);
        gtk_widget_add_css_class(name, "caption");
        gtk_widget_add_css_class(name, "dim-label");
        gtk_label_set_xalign(GTK_LABEL(name), 0);

        dial = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, PADDLE_MAX, 1);
        gtk_scale_set_draw_value(GTK_SCALE(dial), FALSE);
        gtk_range_set_inverted(GTK_RANGE(dial), TRUE);
        gtk_range_set_value(GTK_RANGE(dial), g_paddle_value[port][p]);
        gtk_widget_set_hexpand(dial, TRUE);
        gtk_widget_set_focusable(dial, FALSE);
        gtk_widget_set_focus_on_click(dial, FALSE);
        g_signal_connect(dial, "value-changed", G_CALLBACK(on_paddle_changed), GINT_TO_POINTER(line));

        fire = gtk_button_new_with_label("Fire");
        gtk_widget_add_css_class(fire, "a800-ctl");
        gtk_widget_set_size_request(fire, CELL_W, 36);
        gtk_widget_set_focusable(fire, FALSE);
        gtk_widget_set_focus_on_click(fire, FALSE);
        g = gtk_gesture_click_new();
        g_signal_connect(g, "pressed", G_CALLBACK(on_paddle_fire), GINT_TO_POINTER(line));
        g_signal_connect(g, "released", G_CALLBACK(on_paddle_fire_released), GINT_TO_POINTER(line));
        g_signal_connect(g, "cancel", G_CALLBACK(on_paddle_fire_cancelled), GINT_TO_POINTER(line));
        gtk_widget_add_controller(fire, GTK_EVENT_CONTROLLER(g));

        gtk_box_append(GTK_BOX(row), dial);
        gtk_box_append(GTK_BOX(row), fire);
        gtk_box_append(GTK_BOX(box), name);
        gtk_box_append(GTK_BOX(box), row);
        g_paddle_w[port][p][0] = dial;
        g_paddle_w[port][p][1] = fire;
    }
    return box;
}

/* The stick's four directions around its fire button. */
static GtkWidget *build_joystick(int port)
{
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), CELL_GAP);
    gtk_grid_set_column_spacing(GTK_GRID(grid), CELL_GAP);
    gtk_grid_attach(GTK_GRID(grid), control_new("\xe2\x96\xb2", -1,
        A800_TARGET_PORT(port, A800_ACT_UP), CELL_W, CELL_H, "a800-ctl"), 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), control_new("\xe2\x97\x80", -1,
        A800_TARGET_PORT(port, A800_ACT_LEFT), CELL_W, CELL_H, "a800-ctl"), 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), control_new("Fire", -1,
        A800_TARGET_PORT(port, A800_ACT_FIRE), CELL_W, CELL_H, "a800-ctl"), 1, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), control_new("\xe2\x96\xb6", -1,
        A800_TARGET_PORT(port, A800_ACT_RIGHT), CELL_W, CELL_H, "a800-ctl"), 2, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), control_new("\xe2\x96\xbc", -1,
        A800_TARGET_PORT(port, A800_ACT_DOWN), CELL_W, CELL_H, "a800-ctl"), 1, 2, 1, 1);
    gtk_widget_set_halign(grid, GTK_ALIGN_CENTER);
    return grid;
}

static GtkWidget *build_port(int port)
{
    GtkWidget *frame = gtk_frame_new(NULL);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *title, *device, *body;
    int dev = a800session_port_device(g_session, port);
    char text[32];

    g_snprintf(text, sizeof text, "Port %d", port + 1);
    title = gtk_label_new(text);
    gtk_widget_add_css_class(title, "heading");
    device = gtk_label_new(a800_device_name(dev));
    gtk_widget_add_css_class(device, "dim-label");
    gtk_label_set_ellipsize(GTK_LABEL(device), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(device), 1);
    gtk_box_append(GTK_BOX(box), title);
    gtk_box_append(GTK_BOX(box), device);

    if (dev == A800_DEV_JOYSTICK) {
        body = build_joystick(port);
    } else if (dev == A800_DEV_PADDLES) {
        body = build_paddles(port);
    } else {
        const char *hint = dev == A800_DEV_CX85 ? "Type on the host's numeric keypad."
                         : dev == A800_DEV_NONE ? ""
                         : "The pointer over the picture drives it.";
        body = gtk_label_new(hint);
        gtk_label_set_wrap(GTK_LABEL(body), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(body), 1);
        gtk_label_set_justify(GTK_LABEL(body), GTK_JUSTIFY_CENTER);
        gtk_widget_add_css_class(body, "dim-label");
        gtk_widget_set_vexpand(body, TRUE);
    }
    gtk_widget_set_margin_top(body, 4);
    gtk_box_append(GTK_BOX(box), body);

    gtk_widget_set_size_request(box, PORT_W, -1);
    gtk_widget_set_margin_top(box, 8);
    gtk_widget_set_margin_bottom(box, 10);
    gtk_widget_set_margin_start(box, 10);
    gtk_widget_set_margin_end(box, 10);
    gtk_frame_set_child(GTK_FRAME(frame), box);
    return frame;
}

/* The 400 and 800 have four ports, the XL/XE two (four with a MultiJoy). */
static int machine_now(void)
{
    int m = a800session_get_int(g_session, "machine", A800_M_130XE);
    return m >= 0 && m < A800_M_COUNT ? m : A800_M_130XE;
}

static int ports_now(int machine)
{
    return a800_machine_is_xl(machine) && !a800session_get_int(g_session, "multijoy", 0)
           ? 2 : A800_PORTS;
}

static gboolean layout_changed(void)
{
    int m = machine_now(), n = ports_now(m), port;
    if (m != g_built_machine || n != g_built_ports) return TRUE;
    for (port = 0; port < n; port++)
        if (a800session_port_device(g_session, port) != g_built_dev[port]) return TRUE;
    return FALSE;
}

static void rebuild(void)
{
    GtkWidget *child;
    int port, p;

    mouse_release();
    g_ncontrols = 0;
    memset(g_paddle_w, 0, sizeof g_paddle_w);
    if (!g_paddles_init) {
        for (port = 0; port < A800_PORTS; port++)
            for (p = 0; p < 2; p++)
                g_paddle_value[port][p] = PADDLE_MAX;   /* where the session starts them */
        g_paddles_init = TRUE;
    }
    while ((child = gtk_widget_get_first_child(g_kbd)) != NULL)
        gtk_fixed_remove(GTK_FIXED(g_kbd), child);
    while ((child = gtk_widget_get_first_child(g_ports)) != NULL)
        gtk_box_remove(GTK_BOX(g_ports), child);

    g_built_machine = machine_now();
    g_built_ports = ports_now(g_built_machine);
    build_keyboard();
    for (port = 0; port < g_built_ports; port++) {
        g_built_dev[port] = a800session_port_device(g_session, port);
        gtk_box_append(GTK_BOX(g_ports), build_port(port));
    }
    refresh_labels();
}

/* ---- gamepads ------------------------------------------------------------- */

static void refresh_pad_ports(void)
{
    int i;
    for (i = 0; i < g_npad_rows; i++) {
        int effective = a800session_gamepad_effective_port(g_session, i);
        char text[32];
        if (effective >= 0) g_snprintf(text, sizeof text, "drives port %d", effective + 1);
        else g_snprintf(text, sizeof text, "drives nothing");
        if (strcmp(gtk_label_get_text(GTK_LABEL(g_pad_port[i])), text) != 0)
            gtk_label_set_text(GTK_LABEL(g_pad_port[i]), text);
    }
}

static void on_assign_changed(GObject *o, GParamSpec *ps, gpointer user_data)
{
    int idx = GPOINTER_TO_INT(user_data);
    guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(o));
    (void)ps;
    /* 0 = automatic (connection order), 1-4 = that port */
    a800session_gamepad_assign(g_session, idx, sel == 0 ? -1 : (int)sel - 1);
    refresh_pad_ports();
}

/* Rebuilt whenever a pad comes or goes (the session's generation counter):
 * one row per pad, its name, the port it drives and the port it is set to. */
static void refresh_pads(void)
{
    static const char *const choices[] = {
        "Automatic", "Port 1", "Port 2", "Port 3", "Port 4", NULL
    };
    GtkWidget *child;
    int n, i;

    while ((child = gtk_widget_get_first_child(g_pad_box)) != NULL)
        gtk_box_remove(GTK_BOX(g_pad_box), child);
    g_npad_rows = 0;

    n = a800session_gamepad_count(g_session);
    if (n == 0) {
        GtkWidget *none = gtk_label_new("No gamepads connected");
        gtk_widget_add_css_class(none, "dim-label");
        gtk_box_append(GTK_BOX(g_pad_box), none);
        return;
    }
    for (i = 0; i < n; i++) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *name, *dd, *eff;
        char text[128];
        int assign = a800session_gamepad_assignment(g_session, i);

        a800session_gamepad_name(g_session, i, text, sizeof text);
        name = gtk_label_new(text);
        gtk_widget_set_hexpand(name, TRUE);
        gtk_label_set_xalign(GTK_LABEL(name), 0);
        gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(name), 1);
        gtk_widget_set_tooltip_text(name, text);
        dd = gtk_drop_down_new_from_strings(choices);
        gtk_drop_down_set_selected(GTK_DROP_DOWN(dd),
                                   assign < 0 || assign >= A800_PORTS ? 0 : (guint)assign + 1);
        gtk_widget_set_focus_on_click(dd, FALSE);
        g_signal_connect(dd, "notify::selected", G_CALLBACK(on_assign_changed), GINT_TO_POINTER(i));
        eff = gtk_label_new("");
        gtk_widget_add_css_class(eff, "dim-label");
        gtk_box_append(GTK_BOX(row), name);
        gtk_box_append(GTK_BOX(row), eff);
        gtk_box_append(GTK_BOX(row), dd);
        gtk_box_append(GTK_BOX(g_pad_box), row);
        if (g_npad_rows < MAX_PADS)
            g_pad_port[g_npad_rows++] = eff;
    }
    refresh_pad_ports();
}

/* Hot-plugged pads, auto-assignment changes, and the machine or a port's
 * device changed in Preferences. */
static gboolean pad_tick(gpointer d)
{
    unsigned gen;
    (void)d;
    if (!g_window || !gtk_widget_get_visible(g_window)) return G_SOURCE_CONTINUE;
    gen = a800session_gamepad_generation(g_session);
    if (gen != g_pad_generation) {
        g_pad_generation = gen;
        refresh_pads();
    } else {
        refresh_pad_ports();
    }
    if (layout_changed())
        rebuild();
    return G_SOURCE_CONTINUE;
}

/* ---- keyboard ------------------------------------------------------------- */

static gboolean on_key(guint keyval, guint code, GdkModifierType st, gboolean down)
{
    /* an open drop-down (a gamepad's port) has the arrows and Return */
    GtkWidget *focus = gtk_root_get_focus(GTK_ROOT(g_window));
    if (focus && gtk_widget_get_ancestor(focus, GTK_TYPE_POPOVER))
        return FALSE;

    if (g_swallow_code && code == g_swallow_code) {
        if (!down) g_swallow_code = 0;
        return TRUE;
    }
    if (down && g_map_state >= 0) {
        guint32 keysym = code >= 8 ? a800session_keysym_from_evdev(code - 8) : 0;
        char stolen[128], msg[200], name[32];
        if (!keysym) keysym = keyval;
        a800session_binding_set_key(g_session, g_map_state, keysym, stolen, sizeof stolen);
        a800session_keysym_name(keysym, name, sizeof name);
        g_swallow_code = code;
        set_map_state(-1);   /* stay armed: remapping several in a row is normal */
        if (stolen[0])
            g_snprintf(msg, sizeof msg, "Bound %s (was %s)", name, stolen);
        else
            g_snprintf(msg, sizeof msg, "Bound %s", name);
        gtk_label_set_text(GTK_LABEL(g_map_hint), msg);
        return TRUE;
    }
    /* Otherwise exactly what the main window does with it. */
    if (!g_parent) return FALSE;
    return a800_window_key(A800_WINDOW(g_parent), keyval, code, st, down);
}

static gboolean on_key_pressed(GtkEventControllerKey *c, guint keyval,
                               guint code, GdkModifierType st, gpointer d)
{
    (void)c; (void)d;
    return on_key(keyval, code, st, TRUE);
}

static void on_key_released(GtkEventControllerKey *c, guint keyval,
                            guint code, GdkModifierType st, gpointer d)
{
    (void)c; (void)d;
    on_key(keyval, code, st, FALSE);
}

/* Only what this panel's mouse holds: the main window releases its own. */
static void on_focus_leave(GtkEventControllerFocus *c, gpointer d)
{
    (void)c; (void)d;
    mouse_release();
    g_swallow_code = 0;     /* its release went elsewhere */
}

/* ---- the window ----------------------------------------------------------- */

static void on_map_clicked(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    set_map_state(g_map_state == -2 ? -1 : -2);
}

static void on_defaults_clicked(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    a800session_bindings_reset(g_session);
    refresh_labels();
}

static void hide_window(void)
{
    set_map_state(-2);
    mouse_release();
    g_swallow_code = 0;
    gtk_widget_set_visible(g_window, FALSE);
}

static gboolean on_close(GtkWindow *w, gpointer d)
{
    (void)w; (void)d;
    hide_window();
    return TRUE;
}

static void on_destroy(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    if (g_capture_timer) g_source_remove(g_capture_timer);
    if (g_held_timer) g_source_remove(g_held_timer);
    if (g_pad_timer) g_source_remove(g_pad_timer);
    g_capture_timer = g_held_timer = g_pad_timer = 0;
    g_window = NULL;
    g_ncontrols = 0;
    g_npad_rows = 0;
    memset(g_paddle_w, 0, sizeof g_paddle_w);
    g_built_machine = -1;
    g_map_state = -2;
}

static void build_window(GtkWindow *parent)
{
    GtkWidget *root, *maprow, *toolbar, *header, *defaults, *padtitle;
    GtkEventController *keys, *focus;

    install_css();
    g_window = adw_window_new();
    gtk_window_set_title(GTK_WINDOW(g_window), "Controllers");
    if (parent) {
        gtk_window_set_transient_for(GTK_WINDOW(g_window), parent);
        /* Same app-id as the main window, so the shell groups and icons it. */
        gtk_window_set_application(GTK_WINDOW(g_window), gtk_window_get_application(parent));
    }
    gtk_window_set_destroy_with_parent(GTK_WINDOW(g_window), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(g_window), FALSE);
    g_signal_connect(g_window, "close-request", G_CALLBACK(on_close), NULL);
    g_signal_connect(g_window, "destroy", G_CALLBACK(on_destroy), NULL);

    root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(root, 12);
    gtk_widget_set_margin_bottom(root, 12);
    gtk_widget_set_margin_start(root, 12);
    gtk_widget_set_margin_end(root, 12);

    g_kbd = gtk_fixed_new();
    gtk_widget_set_halign(g_kbd, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(root), g_kbd);

    g_ports = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_set_homogeneous(GTK_BOX(g_ports), TRUE);
    gtk_widget_set_halign(g_ports, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(root), g_ports);

    padtitle = gtk_label_new("Gamepads");
    gtk_widget_add_css_class(padtitle, "heading");
    gtk_box_append(GTK_BOX(root), padtitle);
    g_pad_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_append(GTK_BOX(root), g_pad_box);

    maprow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    g_map_button = gtk_button_new_with_label("Map");
    gtk_widget_set_size_request(g_map_button, 72, -1);
    gtk_widget_set_focus_on_click(g_map_button, FALSE);
    g_signal_connect(g_map_button, "clicked", G_CALLBACK(on_map_clicked), NULL);
    defaults = gtk_button_new_with_label("Defaults");
    gtk_widget_set_focus_on_click(defaults, FALSE);
    g_signal_connect(defaults, "clicked", G_CALLBACK(on_defaults_clicked), NULL);
    g_map_hint = gtk_label_new("");
    gtk_widget_add_css_class(g_map_hint, "dim-label");
    gtk_widget_set_hexpand(g_map_hint, TRUE);
    gtk_label_set_xalign(GTK_LABEL(g_map_hint), 0);
    gtk_label_set_ellipsize(GTK_LABEL(g_map_hint), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(g_map_hint), 1);
    gtk_box_append(GTK_BOX(maprow), g_map_button);
    gtk_box_append(GTK_BOX(maprow), defaults);
    gtk_box_append(GTK_BOX(maprow), g_map_hint);
    gtk_box_append(GTK_BOX(root), maprow);

    header = adw_header_bar_new();
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

    g_ncontrols = 0;
    g_built_machine = -1;
    set_map_state(-2);
    rebuild();
    g_held_timer = g_timeout_add(33, held_tick, NULL);
    g_pad_timer = g_timeout_add(250, pad_tick, NULL);
}

static void show_window(void)
{
    if (layout_changed())
        rebuild();
    g_pad_generation = a800session_gamepad_generation(g_session);
    refresh_pads();
    gtk_window_present(GTK_WINDOW(g_window));
}

void a800_controllers_toggle(GtkWindow *parent, a800session *session)
{
    g_session = session;
    if (parent)
        g_parent = parent;
    if (!g_window)
        build_window(g_parent);

    if (gtk_widget_get_visible(g_window))
        hide_window();
    else
        show_window();
}

gboolean a800_controllers_visible(void)
{
    return g_window && gtk_widget_get_visible(g_window);
}

void a800_controllers_refresh(void)
{
    if (!g_window)
        return;
    if (gtk_widget_get_visible(g_window))
        rebuild();
    else
        g_built_machine = -1;   /* rebuilt when next shown */
}
