/*
 * display.c -- see display.h.
 *
 * Frames are pulled by serial, not pushed: copy_frame does nothing when the
 * Atari has not drawn anything new, so a tick between changes costs a mutex
 * and no memcpy. The tick also tells the session when the compositor shows
 * a frame, which the emulator phase-locks to.
 *
 * The frame's size changes with the visible area, the NTSC filter and the
 * 80-column cards; its pixel aspect (x1000) says how wide each pixel is
 * against the Atari's own. TV aspect then gives the Atari's pixels their
 * shape on a TV (NTSC 6:7, PAL a little wide) and shows an 80-column
 * card's screen 4:3, like the monitor it drove.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "display.h"

#define FB_W A800SESSION_FB_MAX_WIDTH
#define FB_H A800SESSION_FB_MAX_HEIGHT

struct _A800Display {
    GtkWidget parent_instance;

    a800session *session;
    guint32 *fb;            /* XRGB8888 straight from the session */
    GdkTexture *texture;
    int fw, fh, par;        /* the frame's size and pixel aspect */
    guint64 serial;
    guint64 frames;
    guint tick_id;
    gboolean tv_aspect;
    gboolean smooth;
    graphene_rect_t shown;  /* where the picture is, for the pointer */
    double last_x, last_y;
    gboolean have_last;
    unsigned buttons;
};

G_DEFINE_FINAL_TYPE(A800Display, a800_display, GTK_TYPE_WIDGET)

static void rebuild_texture(A800Display *self)
{
    GBytes *bytes;
    int i, n = self->fw * self->fh;

    /* 0x00RRGGBB read as B,G,R,A on a little-endian host: only the alpha
     * needs setting; the texture takes its own copy */
    for (i = 0; i < n; i++)
        self->fb[i] |= 0xFF000000u;
    bytes = g_bytes_new(self->fb, (gsize)n * 4);
    g_clear_object(&self->texture);
    self->texture = gdk_memory_texture_new(self->fw, self->fh, GDK_MEMORY_B8G8R8A8, bytes,
                                           (gsize)self->fw * 4);
    g_bytes_unref(bytes);
}

static gboolean on_tick(GtkWidget *widget, GdkFrameClock *clock, gpointer user_data)
{
    A800Display *self = A800_DISPLAY(widget);
    int w, h, par;
    (void)user_data;

    a800session_notify_vsync(self->session, gdk_frame_clock_get_frame_time(clock) * 1000);
    if (a800session_copy_frame(self->session, self->fb, &w, &h, &par, &self->serial) &&
        w > 0 && h > 0) {
        self->fw = w;
        self->fh = h;
        self->par = par > 0 ? par : 1000;
        rebuild_texture(self);
        self->frames++;
        gtk_widget_queue_draw(widget);
    }
    return G_SOURCE_CONTINUE;
}

/* The picture's width over its height, as shown. */
static double picture_aspect(A800Display *self)
{
    double w = self->fw * self->par / 1000.0, h = self->fh;
    if (!self->tv_aspect)
        return w / h;
    if (self->fw >= 560 && self->par == 500)
        return 4.0 / 3.0;   /* an 80-column card's monitor */
    /* an Atari pixel on an NTSC TV is 6/7 as wide as tall; on PAL, 1.03 */
    return w * (a800session_refresh_rate(self->session) > 55000 ? 6.0 / 7.0 : 1.03) / h;
}

static void a800_display_snapshot(GtkWidget *widget, GtkSnapshot *snapshot)
{
    A800Display *self = A800_DISPLAY(widget);
    int w = gtk_widget_get_width(widget);
    int h = gtk_widget_get_height(widget);
    double want, sw, sh;

    gtk_snapshot_append_color(snapshot, &(GdkRGBA){ 0, 0, 0, 1 },
                              &GRAPHENE_RECT_INIT(0, 0, w, h));
    if (!self->texture || w <= 0 || h <= 0)
        return;

    want = picture_aspect(self);
    if ((double)w / (double)h > want) {
        sh = h;
        sw = sh * want;
    } else {
        sw = w;
        sh = sw / want;
    }
    graphene_rect_init(&self->shown, (float)((w - sw) / 2.0), (float)((h - sh) / 2.0),
                       (float)sw, (float)sh);
    gtk_snapshot_append_scaled_texture(
        snapshot, self->texture,
        self->smooth ? GSK_SCALING_FILTER_LINEAR : GSK_SCALING_FILTER_NEAREST, &self->shown);
}

/* ---- the pointer: mice, Koala Pad, light pen and gun ------------------------ */

static void pointer_at(A800Display *self, double x, double y)
{
    const graphene_rect_t *r = &self->shown;
    int fx, fy, inside;

    if (!a800session_pointer_device_active(self->session) || r->size.width <= 0)
        return;
    fx = (int)((x - r->origin.x) * self->fw / r->size.width);
    fy = (int)((y - r->origin.y) * self->fh / r->size.height);
    inside = fx >= 0 && fy >= 0 && fx < self->fw && fy < self->fh;
    a800session_pointer(self->session, CLAMP(fx, 0, self->fw - 1), CLAMP(fy, 0, self->fh - 1),
                        inside, self->buttons);
    if (self->have_last) {
        /* mice count movement: in frame pixels, as the picture is drawn */
        int dx = (int)((x - self->last_x) * self->fw / r->size.width);
        int dy = (int)((y - self->last_y) * self->fh / r->size.height);
        if (dx || dy)
            a800session_pointer_motion(self->session, dx, dy, self->buttons);
        if (!dx)
            x = self->last_x;
        if (!dy)
            y = self->last_y;
    }
    self->last_x = x;
    self->last_y = y;
    self->have_last = TRUE;
}

static void on_motion(GtkEventControllerMotion *c, double x, double y, gpointer user_data)
{
    (void)c;
    pointer_at(user_data, x, y);
}

static void on_leave(GtkEventControllerMotion *c, gpointer user_data)
{
    A800Display *self = user_data;
    (void)c;
    self->have_last = FALSE;
    if (a800session_pointer_device_active(self->session))
        a800session_pointer(self->session, 0, 0, 0, 0);
}

static unsigned button_bit(GtkGestureClick *g)
{
    guint b = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(g));
    return b == GDK_BUTTON_PRIMARY ? 1u : b == GDK_BUTTON_SECONDARY ? 2u : 0u;
}

static void on_pressed(GtkGestureClick *g, int n, double x, double y, gpointer user_data)
{
    A800Display *self = user_data;
    (void)n;
    gtk_widget_grab_focus(GTK_WIDGET(self));
    self->buttons |= button_bit(g);
    pointer_at(self, x, y);
}

static void on_released(GtkGestureClick *g, int n, double x, double y, gpointer user_data)
{
    A800Display *self = user_data;
    (void)n;
    self->buttons &= ~button_bit(g);
    pointer_at(self, x, y);
}

/* ---- the widget -------------------------------------------------------------- */

static void a800_display_dispose(GObject *object)
{
    A800Display *self = A800_DISPLAY(object);

    if (self->tick_id) {
        gtk_widget_remove_tick_callback(GTK_WIDGET(self), self->tick_id);
        self->tick_id = 0;
    }
    g_clear_object(&self->texture);
    g_clear_pointer(&self->fb, g_free);

    G_OBJECT_CLASS(a800_display_parent_class)->dispose(object);
}

static void a800_display_class_init(A800DisplayClass *klass)
{
    G_OBJECT_CLASS(klass)->dispose = a800_display_dispose;
    GTK_WIDGET_CLASS(klass)->snapshot = a800_display_snapshot;
}

static void a800_display_init(A800Display *self)
{
    GtkEventController *motion;
    GtkGesture *click;

    self->fb = g_new0(guint32, FB_W * FB_H);
    self->par = 1000;
    self->tv_aspect = TRUE;
    self->smooth = TRUE;
    gtk_widget_set_focusable(GTK_WIDGET(self), TRUE);
    gtk_widget_set_hexpand(GTK_WIDGET(self), TRUE);
    gtk_widget_set_vexpand(GTK_WIDGET(self), TRUE);

    motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion", G_CALLBACK(on_motion), self);
    g_signal_connect(motion, "leave", G_CALLBACK(on_leave), self);
    gtk_widget_add_controller(GTK_WIDGET(self), motion);
    click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0);
    g_signal_connect(click, "pressed", G_CALLBACK(on_pressed), self);
    g_signal_connect(click, "released", G_CALLBACK(on_released), self);
    gtk_widget_add_controller(GTK_WIDGET(self), GTK_EVENT_CONTROLLER(click));
}

GtkWidget *a800_display_new(a800session *session)
{
    A800Display *self = g_object_new(A800_TYPE_DISPLAY, NULL);
    self->session = session;
    self->tick_id = gtk_widget_add_tick_callback(GTK_WIDGET(self), on_tick, NULL, NULL);
    return GTK_WIDGET(self);
}

void a800_display_set_tv_aspect(A800Display *self, gboolean tv)
{
    self->tv_aspect = tv;
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

void a800_display_set_smooth(A800Display *self, gboolean smooth)
{
    self->smooth = smooth;
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

gboolean a800_display_save_png(A800Display *self, const char *path)
{
    return self->texture && gdk_texture_save_to_png(self->texture, path);
}

guint64 a800_display_frames(A800Display *self)
{
    return self->frames;
}
