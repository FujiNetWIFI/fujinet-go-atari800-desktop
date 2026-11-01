/*
 * display.c -- see display.h.
 *
 * Frames are pulled by serial, not pushed: copy_frame does nothing when the
 * Atari has not drawn anything new (an identical frame is not published), so
 * a tick between changes costs a mutex and no memcpy. The tick also tells the
 * session when the compositor shows a frame, which the emulator phase-locks
 * to. A frame is copied once, straight into a pooled buffer the texture
 * borrows (GSK reuses its GPU texture while the size holds).
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

#define FB_PIXELS ((gsize)A800SESSION_FB_MAX_WIDTH * A800SESSION_FB_MAX_HEIGHT)

/* ---- frame buffer pool ----------------------------------------------------
 * The session copies a frame straight into a pooled buffer (sized for the
 * largest frame) and the texture borrows it: no allocation in steady state.
 * The buffer comes back to the pool when GSK drops the texture's bytes,
 * possibly after the widget is gone, so the pool is reference counted and
 * each outstanding buffer holds a reference. */
typedef struct {
    gatomicrefcount refs;
    GMutex lock;
    GPtrArray *spare;      /* idle guint32 buffers, FB_PIXELS each */
} FramePool;

typedef struct {
    FramePool *pool;
    guint32 *px;
} FrameLoan;

#define POOL_KEEP 4

static FramePool *pool_new(void)
{
    FramePool *p = g_new0(FramePool, 1);
    g_atomic_ref_count_init(&p->refs);
    g_mutex_init(&p->lock);
    p->spare = g_ptr_array_new_with_free_func(g_free);
    return p;
}

static void pool_unref(FramePool *p)
{
    if (!g_atomic_ref_count_dec(&p->refs))
        return;
    g_ptr_array_free(p->spare, TRUE);
    g_mutex_clear(&p->lock);
    g_free(p);
}

static guint32 *pool_take(FramePool *p)
{
    guint32 *px = NULL;

    g_mutex_lock(&p->lock);
    if (p->spare->len > 0)
        px = g_ptr_array_steal_index(p->spare, p->spare->len - 1);
    g_mutex_unlock(&p->lock);
    return px ? px : g_malloc(FB_PIXELS * sizeof(guint32));
}

static void pool_give(FramePool *p, guint32 *px)
{
    g_mutex_lock(&p->lock);
    if (p->spare->len < POOL_KEEP) {
        g_ptr_array_add(p->spare, px);
        px = NULL;
    }
    g_mutex_unlock(&p->lock);
    g_free(px);
}

static void loan_release(gpointer data)
{
    FrameLoan *loan = data;

    pool_give(loan->pool, loan->px);
    pool_unref(loan->pool);
    g_free(loan);
}

struct _A800Display {
    GtkWidget parent_instance;

    a800session *session;
    FramePool *pool;
    guint32 *next;          /* a pooled buffer the next frame is copied into */
    GdkTexture *texture;
    int fw, fh, par;        /* the frame's size and pixel aspect */
    guint64 serial;
    guint tick_id;
    gulong suspended_id;    /* notify::suspended on the toplevel, while mapped */
    GtkWindow *toplevel;
    gboolean tv_aspect;
    gboolean smooth;
    graphene_rect_t shown;  /* where the picture is, for the pointer */
    double last_x, last_y;
    gboolean have_last;
    unsigned buttons;
};

G_DEFINE_FINAL_TYPE(A800Display, a800_display, GTK_TYPE_WIDGET)

/* Wrap px (w x h, XRGB8888 = GDK_MEMORY_B8G8R8X8 on a little-endian host; the
 * X byte is ignored, so no alpha pass) in the texture. When the size is
 * unchanged the previous texture goes in as the update source so GSK can
 * reuse its GPU texture; a new size starts afresh. Takes ownership of px. */
static void publish_texture(A800Display *self, guint32 *px, int w, int h)
{
    GdkMemoryTextureBuilder *b = gdk_memory_texture_builder_new();
    FrameLoan *loan = g_new(FrameLoan, 1);
    GBytes *bytes;

    g_atomic_ref_count_inc(&self->pool->refs);
    loan->pool = self->pool;
    loan->px = px;
    bytes = g_bytes_new_with_free_func(px, (gsize)w * h * sizeof(guint32), loan_release, loan);
    gdk_memory_texture_builder_set_bytes(b, bytes);
    gdk_memory_texture_builder_set_stride(b, (gsize)w * 4);
    gdk_memory_texture_builder_set_width(b, w);
    gdk_memory_texture_builder_set_height(b, h);
    gdk_memory_texture_builder_set_format(b, GDK_MEMORY_B8G8R8X8);
    if (self->texture && w == self->fw && h == self->fh) {
        cairo_rectangle_int_t all = { 0, 0, w, h };
        cairo_region_t *region = cairo_region_create_rectangle(&all);

        gdk_memory_texture_builder_set_update_texture(b, self->texture);
        gdk_memory_texture_builder_set_update_region(b, region);
        cairo_region_destroy(region);
    }
    g_clear_object(&self->texture);
    self->texture = gdk_memory_texture_builder_build(b);
    g_bytes_unref(bytes);
    g_object_unref(b);
}

static gboolean on_tick(GtkWidget *widget, GdkFrameClock *clock, gpointer user_data)
{
    A800Display *self = A800_DISPLAY(widget);
    int w, h, par;
    (void)user_data;

    a800session_notify_vsync(self->session, gdk_frame_clock_get_frame_time(clock) * 1000);
    if (!self->next)
        self->next = pool_take(self->pool);
    if (a800session_copy_frame(self->session, self->next, &w, &h, &par, &self->serial) &&
        w > 0 && h > 0) {
        guint32 *px = self->next;

        self->next = NULL;
        publish_texture(self, px, w, h);
        self->fw = w;
        self->fh = h;
        self->par = par > 0 ? par : 1000;
        gtk_widget_queue_draw(widget);
    }
    return G_SOURCE_CONTINUE;
}

/* The tick runs only while somebody can see the result. A hidden or
 * suspended window gets no frame callbacks from the compositor anyway, but a
 * minimised one on some stacks keeps ticking, and every tick is a wakeup.
 * The emulator does not care: with no ticks the vsync stream goes stale and
 * the session paces on the wall clock. The first tick after the window comes
 * back copies whatever frame is newest. */
static void update_tick(A800Display *self)
{
    gboolean want = gtk_widget_get_mapped(GTK_WIDGET(self)) &&
                    !(self->toplevel && gtk_window_is_suspended(self->toplevel));

    if (want && !self->tick_id)
        self->tick_id = gtk_widget_add_tick_callback(GTK_WIDGET(self), on_tick, NULL, NULL);
    else if (!want && self->tick_id) {
        gtk_widget_remove_tick_callback(GTK_WIDGET(self), self->tick_id);
        self->tick_id = 0;
    }
}

static void on_suspended(GObject *obj, GParamSpec *pspec, gpointer user_data)
{
    (void)obj;
    (void)pspec;
    update_tick(A800_DISPLAY(user_data));
}

static void a800_display_map(GtkWidget *widget)
{
    A800Display *self = A800_DISPLAY(widget);
    GtkRoot *root;

    GTK_WIDGET_CLASS(a800_display_parent_class)->map(widget);
    root = gtk_widget_get_root(widget);
    if (GTK_IS_WINDOW(root)) {
        self->toplevel = GTK_WINDOW(root);
        self->suspended_id = g_signal_connect(self->toplevel, "notify::suspended",
                                              G_CALLBACK(on_suspended), self);
    }
    update_tick(self);
}

static void a800_display_unmap(GtkWidget *widget)
{
    A800Display *self = A800_DISPLAY(widget);

    if (self->toplevel && self->suspended_id)
        g_signal_handler_disconnect(self->toplevel, self->suspended_id);
    self->suspended_id = 0;
    self->toplevel = NULL;
    GTK_WIDGET_CLASS(a800_display_parent_class)->unmap(widget);
    update_tick(self);
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
    g_clear_pointer(&self->next, g_free);
    if (self->pool) {
        pool_unref(self->pool);
        self->pool = NULL;
    }

    G_OBJECT_CLASS(a800_display_parent_class)->dispose(object);
}

static void a800_display_class_init(A800DisplayClass *klass)
{
    G_OBJECT_CLASS(klass)->dispose = a800_display_dispose;
    GTK_WIDGET_CLASS(klass)->snapshot = a800_display_snapshot;
    GTK_WIDGET_CLASS(klass)->map = a800_display_map;
    GTK_WIDGET_CLASS(klass)->unmap = a800_display_unmap;
}

static void a800_display_init(A800Display *self)
{
    GtkEventController *motion;
    GtkGesture *click;

    self->pool = pool_new();
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
    /* machine frames, once a picture has reached the widget: a still screen
     * publishes nothing new, but the count still runs */
    return self->texture ? a800session_frame_count(self->session) : 0;
}
