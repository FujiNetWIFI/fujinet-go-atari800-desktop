/*
 * window.c -- the main window: the Atari's picture, the header-bar menu,
 * the keyboard, the drives, cartridges and tape, the FujiNet status, and
 * the timer that brings the gamepad thread's news to the UI.
 *
 * Keys go to the Atari pressed and released, keyed by the physical key
 * (GDK's hardware keycode is evdev + 8), with the character GDK says it
 * typed: the session types characters (or positions, if the user prefers)
 * through its one tested table. The Atari's CONTROL+letter keys are its
 * graphics characters, so the app's own shortcuts are CTRL+SHIFT ones, and
 * F9, F11, F12 and Shift+F5, which atari800 itself keeps for the host.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "window.h"

#include "controllers/controllers_window.h"
#include "debugger/dbg_window.h"
#include "display.h"
#include "fujilog.h"
#include "prefs.h"

#include <string.h>

struct _A800Window {
    AdwApplicationWindow parent_instance;

    a800session *session;
    GtkWidget *display;
    GtkWidget *toast_overlay;
    GtkWidget *status;
    GtkWidget *status_dot;
    GtkWidget *menu_button;
    char menu_sig[4096];        /* what the media menus last showed */
    guint status_id;
    guint fast_id;
    unsigned pad_generation;
    unsigned fujinet_generation;
    gboolean fullscreen;
    gboolean notice_open;
    /* A800_SCREENSHOT / A800_AUTOQUIT_FRAMES */
    char *shot_path;
    guint64 shot_frames, quit_frames;
};

G_DEFINE_FINAL_TYPE(A800Window, a800_window, ADW_TYPE_APPLICATION_WINDOW)

static void rebuild_menu(A800Window *self, gboolean force);
static void update_status(A800Window *self);

void a800_window_toast(A800Window *self, const char *text)
{
    adw_toast_overlay_add_toast(ADW_TOAST_OVERLAY(self->toast_overlay), adw_toast_new(text));
}

/* ---- the accent ---------------------------------------------------------------- */

static GtkCssProvider *s_accent_css;

static void load_accent_css(gboolean dark)
{
    char buf[1024];
    /* rust fills with white text everywhere; as text colour the rust is too
     * dark on a dark theme, so there it is lightened (#C8A27A, 7:1) */
    g_snprintf(buf, sizeof buf,
        ".a800-accent, .a800-held { background: #%06x; color: #ffffff; }\n"
        ".a800-accent:hover { background: shade(#%06x, 1.15); }\n"
        ".a800-accent-text { color: %s; font-weight: bold; }\n"
        ".a800-dot { border-radius: 6px; min-width: 12px; min-height: 12px; }\n"
        ".a800-dot-on { background: #%06x; }\n"
        ".a800-dot-off { background: #808080; }\n",
        A800SESSION_ACCENT_RGB, A800SESSION_ACCENT_RGB, dark ? "#c8a27a" : "#765c3c",
        A800SESSION_ACCENT_RGB);
    gtk_css_provider_load_from_string(s_accent_css, buf);
}

static void on_dark_changed(AdwStyleManager *sm, GParamSpec *ps, gpointer ud)
{
    (void)ps; (void)ud;
    load_accent_css(adw_style_manager_get_dark(sm));
}

void a800_install_accent_css(void)
{
    AdwStyleManager *sm = adw_style_manager_get_default();
    if (s_accent_css)
        return;
    s_accent_css = gtk_css_provider_new();
    load_accent_css(adw_style_manager_get_dark(sm));
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
        GTK_STYLE_PROVIDER(s_accent_css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_signal_connect(sm, "notify::dark", G_CALLBACK(on_dark_changed), NULL);
}

/* ---- small helpers ---------------------------------------------------------------- */

static void show_message(GtkWidget *over, const char *heading, const char *body)
{
    AdwDialog *dlg = adw_alert_dialog_new(heading, body);
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dlg), "ok", "OK");
    adw_dialog_present(dlg, over);
}

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

/* GMenu labels take _ as a mnemonic: a file name keeps its own. */
static char *menu_escape(const char *s)
{
    GString *g = g_string_new(NULL);
    for (; *s; s++) {
        if (*s == '_')
            g_string_append_c(g, '_');
        g_string_append_c(g, *s);
    }
    return g_string_free(g, FALSE);
}

static GtkFileDialog *file_dialog(const char *title, const char *filter_name,
                                  const char *const *exts, const char *folder)
{
    GtkFileDialog *dlg = gtk_file_dialog_new();
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    GtkFileFilter *these = gtk_file_filter_new();
    GtkFileFilter *all = gtk_file_filter_new();

    gtk_file_filter_set_name(these, filter_name);
    for (int i = 0; exts && exts[i]; i++)
        gtk_file_filter_add_suffix(these, exts[i]);   /* any case */
    gtk_file_filter_set_name(all, "All files");
    gtk_file_filter_add_pattern(all, "*");
    g_list_store_append(filters, these);
    g_list_store_append(filters, all);
    gtk_file_dialog_set_title(dlg, title);
    gtk_file_dialog_set_filters(dlg, G_LIST_MODEL(filters));
    if (folder && *folder && g_file_test(folder, G_FILE_TEST_IS_DIR)) {
        g_autoptr(GFile) dir = g_file_new_for_path(folder);
        gtk_file_dialog_set_initial_folder(dlg, dir);
    }
    g_object_unref(these);
    g_object_unref(all);
    g_object_unref(filters);
    return dlg;
}

static const char *const ANY_EXTS[] = { "atr", "xfd", "atx", "pro", "dcm", "xex", "com", "exe",
                                        "bas", "lst", "car", "rom", "bin", "cas", "a8s", "zip",
                                        "gz", "atz", NULL };
static const char *const DISK_EXTS[] = { "atr", "xfd", "atx", "pro", "dcm", "zip", "gz", "atz",
                                         NULL };
static const char *const CART_EXTS[] = { "car", "rom", "bin", "zip", NULL };
static const char *const TAPE_EXTS[] = { "cas", "zip", NULL };
static const char *const STATE_EXTS[] = { "a8s", NULL };

/* Where to look for media: the folder the last file came from. */
static char *media_folder(A800Window *self)
{
    const char *last = a800session_get_str(self->session, "last_dir", "");
    if (last && *last && g_file_test(last, G_FILE_TEST_IS_DIR))
        return g_strdup(last);
    return g_strdup(a800session_media_path(self->session));
}

static void remember_folder(A800Window *self, const char *path)
{
    g_autofree char *dir = g_path_get_dirname(path);
    a800session_set_str(self->session, "last_dir", dir);
}

static void open_folder(A800Window *self, const char *path)
{
    g_autoptr(GFile) dir = g_file_new_for_path(path);
    GtkFileLauncher *l = gtk_file_launcher_new(dir);
    g_mkdir_with_parents(path, 0755);
    gtk_file_launcher_launch(l, GTK_WINDOW(self), NULL, NULL, NULL);
    g_object_unref(l);
}

/* ---- the keyboard ------------------------------------------------------------------ */

static void run_sysaction(A800Window *self, int sa);

/* The physical key as the session names it; GDK's keyval when the keycode
 * means nothing to it. */
static uint32_t keysym_of(guint keyval, guint keycode)
{
    uint32_t k = keycode >= 8 ? a800session_keysym_from_evdev(keycode - 8) : 0;
    if (k)
        return k;
    keyval = gdk_keyval_to_lower(keyval);
    return keyval < 0x100 || (keyval >= 0xff00 && keyval <= 0xffff) ? keyval : 0;
}

gboolean a800_window_key(A800Window *self, guint keyval, guint keycode,
                         GdkModifierType state, gboolean down)
{
    gboolean ctrl = (state & GDK_CONTROL_MASK) != 0;
    gboolean shift = (state & GDK_SHIFT_MASK) != 0;
    uint32_t keysym = keysym_of(keyval, keycode);

    /* the window's own keys */
    switch (keyval) {
    case GDK_KEY_F9:
        if (down)
            a800_controllers_toggle(GTK_WINDOW(self), self->session);
        return TRUE;
    case GDK_KEY_F11:
        if (down)
            gtk_widget_activate_action(GTK_WIDGET(self), "win.fullscreen", NULL);
        return TRUE;
    case GDK_KEY_F12:
        if (down)
            a800_debugger_toggle(GTK_WINDOW(self), self->session);
        return TRUE;
    case GDK_KEY_F5:
        if (shift) {
            if (down)
                gtk_widget_activate_action(GTK_WIDGET(self), "win.power-cycle", NULL);
            return TRUE;
        }
        break;
    case GDK_KEY_Pause:
    case GDK_KEY_Break:
        /* BREAK, as on atari800's own keyboard */
        a800session_console(self->session, A800_CON_BREAK, down);
        return TRUE;
    default:
        break;
    }
    if (!down) {
        /* always: a key let go while a shortcut was held must not stick */
        a800session_key(self->session, keysym, 0, 0);
        return keysym != 0;
    }
    /* CTRL+SHIFT, ALT and SUPER are the app's and the desktop's, and CTRL+,
     * is Preferences */
    if ((state & (GDK_ALT_MASK | GDK_SUPER_MASK | GDK_META_MASK)) || (ctrl && shift &&
        keysym != A800_KEYSYM_LSHIFT && keysym != A800_KEYSYM_RSHIFT &&
        keysym != A800_KEYSYM_LCTRL && keysym != A800_KEYSYM_RCTRL) ||
        (ctrl && keyval == GDK_KEY_comma))
        return FALSE;
    if (!keysym)
        return FALSE;
    {
        int sa = a800session_key_sysaction(self->session, keysym);
        if (sa >= 0) {
            run_sysaction(self, sa);
            return TRUE;
        }
    }
    return a800session_key(self->session, keysym, gdk_keyval_to_unicode(keyval), 1) != 0;
}

/* An open dialog (Preferences, About, an alert) or menu has the keyboard:
 * Escape closes it and the arrows and Return move through it, instead of
 * going to the Atari. The key capture sits on the window, ahead of them. */
static gboolean ui_has_keyboard(A800Window *self)
{
    GtkWidget *focus = gtk_root_get_focus(GTK_ROOT(self));
    if (adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(self)))
        return TRUE;
    return focus && gtk_widget_get_ancestor(focus, GTK_TYPE_POPOVER) != NULL;
}

static gboolean on_key_pressed(GtkEventControllerKey *ctrl, guint keyval, guint keycode,
                               GdkModifierType state, gpointer user_data)
{
    A800Window *self = user_data;
    (void)ctrl;
    if (ui_has_keyboard(self))
        return FALSE;
    return a800_window_key(self, keyval, keycode, state, TRUE);
}

static void on_key_released(GtkEventControllerKey *ctrl, guint keyval, guint keycode,
                            GdkModifierType state, gpointer user_data)
{
    (void)ctrl;
    a800_window_key(user_data, keyval, keycode, state, FALSE);
}

/* Focus gone: nothing stays held. */
static void on_active_changed(GObject *win, GParamSpec *ps, gpointer user_data)
{
    A800Window *self = user_data;
    (void)ps;
    if (!gtk_window_is_active(GTK_WINDOW(win)) && !a800_controllers_visible())
        a800session_release_all(self->session);
}

/* ---- starting ------------------------------------------------------------------------ */

static void started(A800Window *self)
{
    self->menu_sig[0] = '\0';
    rebuild_menu(self, TRUE);
    update_status(self);
}

void a800_window_start(A800Window *self)
{
    a800session_start_opts opts;

    if (a800session_is_running(self->session))
        return;
    a800session_default_opts(self->session, &opts);
    if (a800session_start(self->session, &opts) != 0) {
        show_message(GTK_WIDGET(self), "Cannot Start the Atari",
                     a800session_last_error(self->session));
        update_status(self);
        return;
    }
    started(self);
}

/* ---- status, and news from the other threads ------------------------------------------- */

static void set_dot(GtkWidget *dot, gboolean on)
{
    gtk_widget_remove_css_class(dot, on ? "a800-dot-off" : "a800-dot-on");
    gtk_widget_add_css_class(dot, on ? "a800-dot-on" : "a800-dot-off");
}

static void update_status(A800Window *self)
{
    char text[512];
    if (!a800session_is_running(self->session)) {
        gtk_label_set_text(GTK_LABEL(self->status), "Off");
        set_dot(self->status_dot, FALSE);
        return;
    }
    a800session_status(self->session, text, sizeof text);
    gtk_label_set_text(GTK_LABEL(self->status), text);
    set_dot(self->status_dot, a800session_fujinet_link(self->session));
}

static gboolean status_tick(gpointer user_data)
{
    A800Window *self = user_data;
    update_status(self);
    rebuild_menu(self, FALSE);
    return G_SOURCE_CONTINUE;
}

static void on_notice_response(AdwAlertDialog *dlg, const char *response, gpointer user_data)
{
    A800Window *self = user_data;
    (void)dlg;
    self->notice_open = FALSE;
    if (!g_strcmp0(response, "debugger"))
        a800_debugger_show(GTK_WINDOW(self), self->session);
    else if (!g_strcmp0(response, "power"))
        gtk_widget_activate_action(GTK_WIDGET(self), "win.power-cycle", NULL);
}

static void show_notice(A800Window *self, const char *text)
{
    AdwDialog *dlg = adw_alert_dialog_new("The Atari Stopped", text);
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dlg), "ok", "Leave It");
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dlg), "debugger", "Open the Debugger");
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dlg), "power", "Power Cycle");
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dlg), "power",
                                             ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dlg), "power");
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dlg), "ok");
    g_signal_connect(dlg, "response", G_CALLBACK(on_notice_response), self);
    self->notice_open = TRUE;
    adw_dialog_present(dlg, GTK_WIDGET(self));
}

/* The gamepad thread cannot call into GTK; it posts system actions and this
 * timer takes them. It also says when gamepads come and go (a pad that drops
 * off Bluetooth mid-game should not be a mystery), when FujiNet restarted
 * itself, and when the Atari crashed with no debugger open. */
static gboolean fast_tick(gpointer user_data)
{
    A800Window *self = user_data;
    char text[512];
    unsigned gen;
    int sa;

    while (a800session_sysaction_take(self->session, &sa))
        run_sysaction(self, sa);
    gen = a800session_gamepad_generation(self->session);
    if (gen != self->pad_generation) {
        self->pad_generation = gen;
        if (a800session_gamepad_last_event(self->session, text, sizeof text) > 0)
            a800_window_toast(self, text);
    }
    gen = a800session_fujinet_generation(self->session);
    if (gen != self->fujinet_generation) {
        self->fujinet_generation = gen;
        if (a800session_fujinet_last_event(self->session, text, sizeof text) > 0)
            a800_window_toast(self, text);
        update_status(self);
    }
    if (!self->notice_open && a800session_take_notice(self->session, text, sizeof text) > 0)
        show_notice(self, text);

    if (self->shot_path &&
        a800_display_frames(A800_DISPLAY(self->display)) >= self->shot_frames) {
        if (!a800_display_save_png(A800_DISPLAY(self->display), self->shot_path))
            g_printerr("A800_SCREENSHOT: could not write %s\n", self->shot_path);
        g_clear_pointer(&self->shot_path, g_free);
    }
    if (self->quit_frames && a800_display_frames(A800_DISPLAY(self->display)) >= self->quit_frames) {
        self->quit_frames = 0;
        gtk_window_close(GTK_WINDOW(self));
    }
    return G_SOURCE_CONTINUE;
}

/* ---- opening things ------------------------------------------------------------------ */

typedef struct {
    A800Window *self;
    char *path;
    GtkWidget *drop;
    int types[64];
    int piggyback;
} CartCtx;

static void cart_ctx_free(CartCtx *c)
{
    g_free(c->path);
    g_free(c);
}

static void on_cart_type(AdwAlertDialog *dlg, const char *response, gpointer user_data)
{
    CartCtx *c = user_data;
    char msg[512];
    (void)dlg;
    if (!g_strcmp0(response, "insert")) {
        int type = c->types[gtk_drop_down_get_selected(GTK_DROP_DOWN(c->drop))];
        int rc = c->piggyback
            ? a800session_cart_insert_piggyback(c->self->session, c->path, type, msg, sizeof msg)
            : a800session_cart_insert(c->self->session, c->path, type, msg, sizeof msg);
        if (rc == 0)
            a800_window_toast(c->self, msg[0] ? msg : "Cartridge inserted");
        else
            show_message(GTK_WIDGET(c->self), "Cannot Use the Cartridge", msg);
        rebuild_menu(c->self, FALSE);
    }
    cart_ctx_free(c);
}

/* A raw image: the user says which board it is. */
static void ask_cart_type(A800Window *self, const char *path, int piggyback)
{
    CartCtx *c = g_new0(CartCtx, 1);
    GtkStringList *names = gtk_string_list_new(NULL);
    int kb = a800session_cart_needs_type(path), n;
    char body[400];
    AdwDialog *dlg;

    n = a800session_cart_types_for(kb, c->types, G_N_ELEMENTS(c->types));
    if (n <= 0) {
        g_object_unref(names);
        g_free(c);
        show_message(GTK_WIDGET(self), "Cannot Use the Cartridge",
                     "No Atari computer cartridge is that size.");
        return;
    }
    for (int i = 0; i < n; i++)
        gtk_string_list_append(names, a800session_cart_type_name(c->types[i]));
    c->self = self;
    c->path = g_strdup(path);
    c->piggyback = piggyback;
    c->drop = gtk_drop_down_new(G_LIST_MODEL(names), NULL);
    g_snprintf(body, sizeof body,
               "%s is a raw %d KB image: the file does not say which cartridge board it "
               "came from. A .car file would.", base_name(path), kb);
    dlg = adw_alert_dialog_new("Which Cartridge Is It?", body);
    adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dlg), c->drop);
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dlg), "cancel", "Cancel");
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dlg), "insert", "Insert");
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dlg), "insert",
                                             ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dlg), "insert");
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dlg), "cancel");
    g_signal_connect(dlg, "response", G_CALLBACK(on_cart_type), c);
    adw_dialog_present(dlg, GTK_WIDGET(self));
}

void a800_window_load_media(A800Window *self, const char *path)
{
    char msg[1024];
    int rc;

    if (!a800session_is_running(self->session)) {
        a800_window_toast(self, "The Atari is off");
        return;
    }
    rc = a800session_open(self->session, path, msg, sizeof msg);
    if (rc == 1) {
        ask_cart_type(self, path, 0);
        return;
    }
    if (rc != 0) {
        show_message(GTK_WIDGET(self), "Cannot Open It",
                     msg[0] ? msg : a800session_last_error(self->session));
        return;
    }
    remember_folder(self, path);
    a800_window_toast(self, msg[0] ? msg : base_name(path));
    rebuild_menu(self, FALSE);
}

static void on_open_chosen(GObject *src, GAsyncResult *res, gpointer user_data)
{
    A800Window *self = user_data;
    g_autoptr(GFile) file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
    g_autofree char *path = file ? g_file_get_path(file) : NULL;
    if (path)
        a800_window_load_media(self, path);
}

static void action_open(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    A800Window *self = user_data;
    g_autofree char *folder = media_folder(self);
    GtkFileDialog *dlg = file_dialog("Open", "Atari disks, programs, cartridges, tapes",
                                     ANY_EXTS, folder);
    (void)a; (void)p;
    gtk_file_dialog_open(dlg, GTK_WINDOW(self), NULL, on_open_chosen, self);
    g_object_unref(dlg);
}

static gboolean on_drop(GtkDropTarget *t, const GValue *value, double x, double y,
                        gpointer user_data)
{
    A800Window *self = user_data;
    g_autofree char *path = NULL;
    (void)t; (void)x; (void)y;
    if (!G_VALUE_HOLDS(value, G_TYPE_FILE))
        return FALSE;
    path = g_file_get_path(G_FILE(g_value_get_object(value)));
    if (!path)
        return FALSE;
    a800_window_load_media(self, path);
    return TRUE;
}

/* ---- System ROMs ------------------------------------------------------------------------ */

typedef struct {
    A800Window *self;
    char *path;
    int slots[16];
} RomCtx;

static void on_rom_slot(AdwAlertDialog *dlg, const char *response, gpointer user_data)
{
    RomCtx *c = user_data;
    char msg[512];
    (void)dlg;
    if (response && response[0] == 's') {
        int slot = atoi(response + 1);
        if (a800session_rom_import(c->self->session, c->path, slot, msg, sizeof msg) > 0) {
            char t[600];
            g_snprintf(t, sizeof t, "Imported: %s", msg);
            a800_window_toast(c->self, t);
        } else {
            show_message(GTK_WIDGET(c->self), "Nothing Imported", msg);
        }
    }
    g_free(c->path);
    g_free(c);
}

/* Not recognised by its checksum: a peripheral's ROM fits some slots by
 * its size; the user says which. */
static void ask_rom_slot(A800Window *self, const char *path, const int *slots, int n)
{
    RomCtx *c = g_new0(RomCtx, 1);
    char body[600];
    AdwDialog *dlg;

    g_snprintf(body, sizeof body,
               "%s is not one of the ROMs this app knows by its checksum, but it is the "
               "size of these. Which is it?", base_name(path));
    dlg = adw_alert_dialog_new("Which ROM Is It?", body);
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dlg), "cancel", "Cancel");
    for (int i = 0; i < n && i < 8; i++) {
        char id[16];
        g_snprintf(id, sizeof id, "s%d", slots[i]);
        adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dlg), id,
                                      a800session_rom_slot(slots[i])->name);
    }
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dlg), "cancel");
    c->self = self;
    c->path = g_strdup(path);
    g_signal_connect(dlg, "response", G_CALLBACK(on_rom_slot), c);
    adw_dialog_present(dlg, GTK_WIDGET(self));
}

static void on_roms_chosen(GObject *src, GAsyncResult *res, gpointer user_data)
{
    A800Window *self = user_data;
    g_autoptr(GListModel) files =
        gtk_file_dialog_open_multiple_finish(GTK_FILE_DIALOG(src), res, NULL);
    GString *log = g_string_new(NULL);
    int imported = 0, unknown = 0;
    char *unknown_path = NULL;

    if (!files) {
        g_string_free(log, TRUE);
        return;
    }
    for (guint i = 0; i < g_list_model_get_n_items(files); i++) {
        g_autoptr(GFile) f = g_list_model_get_item(files, i);
        g_autofree char *path = g_file_get_path(f);
        char msg[1024];
        int n;
        if (!path)
            continue;
        n = a800session_rom_import(self->session, path, -1, msg, sizeof msg);
        if (n > 0) {
            imported += n;
            g_string_append_printf(log, "%s%s", log->len ? "\n" : "", msg);
        } else {
            unknown++;
            g_free(unknown_path);
            unknown_path = g_strdup(path);
        }
    }
    if (unknown == 1 && imported == 0) {
        int slots[16], n = a800session_rom_probe(self->session, unknown_path, slots, 16);
        if (n > 0) {
            ask_rom_slot(self, unknown_path, slots, n);
            g_free(unknown_path);
            g_string_free(log, TRUE);
            return;
        }
    }
    if (unknown)
        g_string_append_printf(log, "%s%d file%s not recognised: a peripheral's ROM goes in its "
                               "slot from Preferences \xe2\x96\xb8 ROMs.", log->len ? "\n\n" : "",
                               unknown, unknown == 1 ? " was" : "s were");
    show_message(GTK_WIDGET(self), imported ? "ROMs Imported" : "Nothing Imported", log->str);
    if (imported)
        a800_window_toast(self, "Choose them in Preferences \xe2\x96\xb8 Machine");
    g_free(unknown_path);
    g_string_free(log, TRUE);
}

static void action_import_roms(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    A800Window *self = user_data;
    static const char *const exts[] = { "rom", "bin", "zip", NULL };
    GtkFileDialog *dlg = file_dialog("Import System ROMs",
                                     "ROM images and MAME sets (*.rom, *.bin, *.zip)", exts, NULL);
    (void)a; (void)p;
    gtk_file_dialog_open_multiple(dlg, GTK_WINDOW(self), NULL, on_roms_chosen, self);
    g_object_unref(dlg);
}

/* ---- drives ------------------------------------------------------------------------------ */

typedef struct {
    A800Window *self;
    int drive;
    int density;
} DriveCtx;

static void insert_into(A800Window *self, int drive, const char *path)
{
    char msg[512], t[700];
    if (a800session_drive_insert(self->session, drive, path, 0, msg, sizeof msg) != 0) {
        show_message(GTK_WIDGET(self), "Cannot Insert the Disk", msg);
        return;
    }
    remember_folder(self, path);
    g_snprintf(t, sizeof t, "%s is in D%d:", base_name(path), drive + 1);
    a800_window_toast(self, msg[0] ? msg : t);
    rebuild_menu(self, FALSE);
}

static void on_drive_chosen(GObject *src, GAsyncResult *res, gpointer user_data)
{
    DriveCtx *c = user_data;
    g_autoptr(GFile) file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
    g_autofree char *path = file ? g_file_get_path(file) : NULL;
    if (path)
        insert_into(c->self, c->drive, path);
    g_free(c);
}

static void action_drive_insert(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    A800Window *self = user_data;
    int drive = g_variant_get_int32(p);
    gboolean fuji = a800session_fujinet_running(self->session);
    g_autofree char *folder = fuji ? g_strdup(a800session_sd_path(self->session))
                                   : media_folder(self);
    char title[64];
    DriveCtx *c = g_new0(DriveCtx, 1);
    GtkFileDialog *dlg;
    (void)a;
    g_snprintf(title, sizeof title, "%s D%d:", fuji ? "Mount in" : "Insert in", drive + 1);
    dlg = file_dialog(title, "Atari disk images (*.atr, *.xfd, *.atx, ...)", DISK_EXTS, folder);
    c->self = self;
    c->drive = drive;
    gtk_file_dialog_open(dlg, GTK_WINDOW(self), NULL, on_drive_chosen, c);
    g_object_unref(dlg);
}

static void action_drive_eject(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    A800Window *self = user_data;
    (void)a;
    if (a800session_drive_eject(self->session, g_variant_get_int32(p)) != 0)
        a800_window_toast(self, a800session_last_error(self->session));
    rebuild_menu(self, FALSE);
}

static void action_drive_protect(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    A800Window *self = user_data;
    int drive = g_variant_get_int32(p);
    a800_drive_info d;
    (void)a;
    if (a800session_drive_info(self->session, drive, &d) == 0 && d.path[0])
        a800session_drive_set_read_only(self->session, drive, !d.read_only);
    rebuild_menu(self, FALSE);
}

static void on_blank_saved(GObject *src, GAsyncResult *res, gpointer user_data)
{
    DriveCtx *c = user_data;
    g_autoptr(GFile) file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(src), res, NULL);
    g_autofree char *path = file ? g_file_get_path(file) : NULL;
    if (path) {
        if (a800session_new_blank_disk(path, c->density) != 0)
            show_message(GTK_WIDGET(c->self), "Cannot Create the Disk", path);
        else
            insert_into(c->self, c->drive, path);
    }
    g_free(c);
}

static void on_blank_response(AdwAlertDialog *dlg, const char *response, gpointer user_data)
{
    DriveCtx *c = user_data;
    GtkWidget *drop = g_object_get_data(G_OBJECT(dlg), "a800-density");
    GtkFileDialog *save;
    if (g_strcmp0(response, "create") != 0) {
        g_free(c);
        return;
    }
    c->density = (int)gtk_drop_down_get_selected(GTK_DROP_DOWN(drop));
    a800session_set_int(c->self->session, "blank_density", c->density);
    save = gtk_file_dialog_new();
    gtk_file_dialog_set_title(save, "Save the New Disk");
    gtk_file_dialog_set_initial_name(save, "blank.atr");
    {
        g_autofree char *folder = media_folder(c->self);
        g_autoptr(GFile) dir = g_file_new_for_path(folder);
        gtk_file_dialog_set_initial_folder(save, dir);
    }
    gtk_file_dialog_save(save, GTK_WINDOW(c->self), NULL, on_blank_saved, c);
    g_object_unref(save);
}

static void action_drive_blank(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    A800Window *self = user_data;
    static const char *const densities[] = { "Single density (90K)", "Enhanced density (130K)",
                                             "Double density (180K)", NULL };
    DriveCtx *c = g_new0(DriveCtx, 1);
    GtkWidget *drop = gtk_drop_down_new_from_strings(densities);
    char heading[64];
    AdwDialog *dlg;
    (void)a;

    c->self = self;
    c->drive = g_variant_get_int32(p);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(drop),
                               (guint)CLAMP(a800session_get_int(self->session, "blank_density", 0), 0, 2));
    g_snprintf(heading, sizeof heading, "New Blank Disk in D%d:", c->drive + 1);
    dlg = adw_alert_dialog_new(heading, "An empty ATR: format it from DOS on the Atari.");
    adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dlg), drop);
    g_object_set_data(G_OBJECT(dlg), "a800-density", drop);
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dlg), "cancel", "Cancel");
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dlg), "create", "Create\xe2\x80\xa6");
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dlg), "create",
                                             ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dlg), "create");
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dlg), "cancel");
    g_signal_connect(dlg, "response", G_CALLBACK(on_blank_response), c);
    adw_dialog_present(dlg, GTK_WIDGET(self));
}

static void on_import_sd_chosen(GObject *src, GAsyncResult *res, gpointer user_data)
{
    A800Window *self = user_data;
    g_autoptr(GListModel) files =
        gtk_file_dialog_open_multiple_finish(GTK_FILE_DIALOG(src), res, NULL);
    int ok = 0, failed = 0;
    char t[200];
    if (!files)
        return;
    for (guint i = 0; i < g_list_model_get_n_items(files); i++) {
        g_autoptr(GFile) f = g_list_model_get_item(files, i);
        g_autofree char *path = g_file_get_path(f);
        char dest[1024];
        if (path && a800session_import_to_sd(self->session, path, dest, sizeof dest) == 0)
            ok++;
        else
            failed++;
    }
    if (failed)
        g_snprintf(t, sizeof t, "%d copied to FujiNet's SD card, %d could not be", ok, failed);
    else
        g_snprintf(t, sizeof t, "%d copied to FujiNet's SD card: mount from CONFIG or Drives", ok);
    a800_window_toast(self, t);
}

static void action_import_sd(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    A800Window *self = user_data;
    g_autofree char *folder = media_folder(self);
    GtkFileDialog *dlg = file_dialog("Import to FujiNet's SD Card",
                                     "Atari disks, programs, cartridges, tapes", ANY_EXTS, folder);
    (void)a; (void)p;
    gtk_file_dialog_open_multiple(dlg, GTK_WINDOW(self), NULL, on_import_sd_chosen, self);
    g_object_unref(dlg);
}

static void action_sd_folder(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    A800Window *self = user_data;
    (void)a; (void)p;
    open_folder(self, a800session_sd_path(self->session));
}

static void action_media_folder(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    A800Window *self = user_data;
    (void)a; (void)p;
    open_folder(self, a800session_media_path(self->session));
}

/* ---- cartridges and tape ------------------------------------------------------------------- */

typedef struct {
    A800Window *self;
    int piggyback;
} CartPick;

static void on_cart_chosen(GObject *src, GAsyncResult *res, gpointer user_data)
{
    CartPick *c = user_data;
    g_autoptr(GFile) file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
    g_autofree char *path = file ? g_file_get_path(file) : NULL;
    char msg[512];
    if (path) {
        int rc = c->piggyback
            ? a800session_cart_insert_piggyback(c->self->session, path, 0, msg, sizeof msg)
            : a800session_cart_insert(c->self->session, path, 0, msg, sizeof msg);
        remember_folder(c->self, path);
        if (rc > 0)
            ask_cart_type(c->self, path, c->piggyback);
        else if (rc < 0)
            show_message(GTK_WIDGET(c->self), "Cannot Use the Cartridge", msg);
        else
            a800_window_toast(c->self, msg[0] ? msg : "Cartridge inserted");
        rebuild_menu(c->self, FALSE);
    }
    g_free(c);
}

static void pick_cart(A800Window *self, int piggyback)
{
    g_autofree char *folder = media_folder(self);
    GtkFileDialog *dlg = file_dialog(piggyback ? "Insert a Piggyback Cartridge" : "Insert a Cartridge",
                                     "Cartridge images (*.car, *.rom, *.bin)", CART_EXTS, folder);
    CartPick *c = g_new0(CartPick, 1);
    c->self = self;
    c->piggyback = piggyback;
    gtk_file_dialog_open(dlg, GTK_WINDOW(self), NULL, on_cart_chosen, c);
    g_object_unref(dlg);
}

static void action_cart_insert(GSimpleAction *a, GVariant *p, gpointer u) { (void)a; (void)p; pick_cart(u, 0); }
static void action_cart_piggyback(GSimpleAction *a, GVariant *p, gpointer u) { (void)a; (void)p; pick_cart(u, 1); }

static void action_cart_remove(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    A800Window *self = user_data;
    (void)a; (void)p;
    a800session_cart_remove(self->session);
    rebuild_menu(self, FALSE);
}

static void action_cart_remove_piggyback(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    A800Window *self = user_data;
    (void)a; (void)p;
    a800session_cart_remove_piggyback(self->session);
    rebuild_menu(self, FALSE);
}

static void on_tape_chosen(GObject *src, GAsyncResult *res, gpointer user_data)
{
    A800Window *self = user_data;
    g_autoptr(GFile) file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
    g_autofree char *path = file ? g_file_get_path(file) : NULL;
    char msg[512];
    if (!path)
        return;
    if (a800session_tape_insert(self->session, path, msg, sizeof msg) != 0) {
        show_message(GTK_WIDGET(self), "Cannot Load the Tape", msg);
        return;
    }
    remember_folder(self, path);
    a800_window_toast(self, msg[0] ? msg : "Tape loaded: Boot Tape, or CLOAD / RUN \"C:\"");
    rebuild_menu(self, FALSE);
}

static void action_tape_insert(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    A800Window *self = user_data;
    g_autofree char *folder = media_folder(self);
    GtkFileDialog *dlg = file_dialog("Load a Tape", "Cassette images (*.cas)", TAPE_EXTS, folder);
    (void)a; (void)p;
    gtk_file_dialog_open(dlg, GTK_WINDOW(self), NULL, on_tape_chosen, self);
    g_object_unref(dlg);
}

static void on_tape_record_chosen(GObject *src, GAsyncResult *res, gpointer user_data)
{
    A800Window *self = user_data;
    g_autoptr(GFile) file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(src), res, NULL);
    g_autofree char *path = file ? g_file_get_path(file) : NULL;
    if (!path)
        return;
    if (a800session_tape_record_new(self->session, path) != 0)
        show_message(GTK_WIDGET(self), "Cannot Record", a800session_last_error(self->session));
    else
        a800_window_toast(self, "Recording: CSAVE or LIST \"C:\" writes to the tape");
    rebuild_menu(self, FALSE);
}

static void action_tape_record(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    A800Window *self = user_data;
    GtkFileDialog *dlg = gtk_file_dialog_new();
    g_autofree char *folder = media_folder(self);
    g_autoptr(GFile) dir = g_file_new_for_path(folder);
    (void)a; (void)p;
    gtk_file_dialog_set_title(dlg, "Record a New Tape");
    gtk_file_dialog_set_initial_name(dlg, "tape.cas");
    gtk_file_dialog_set_initial_folder(dlg, dir);
    gtk_file_dialog_save(dlg, GTK_WINDOW(self), NULL, on_tape_record_chosen, self);
    g_object_unref(dlg);
}

static void action_tape_rewind(GSimpleAction *a, GVariant *p, gpointer u)
{
    (void)a; (void)p;
    a800session_tape_rewind(A800_WINDOW(u)->session);
}

static void action_tape_boot(GSimpleAction *a, GVariant *p, gpointer u)
{
    (void)a; (void)p;
    a800session_tape_boot(A800_WINDOW(u)->session);
}

static void action_tape_eject(GSimpleAction *a, GVariant *p, gpointer u)
{
    (void)a; (void)p;
    a800session_tape_eject(A800_WINDOW(u)->session);
    rebuild_menu(u, FALSE);
}

/* ---- states ---------------------------------------------------------------------------------- */

static char *states_folder(A800Window *self)
{
    char *dir = g_build_filename(a800session_data_path(self->session), "states", NULL);
    g_mkdir_with_parents(dir, 0755);
    return dir;
}

static void on_state_saved(GObject *src, GAsyncResult *res, gpointer user_data)
{
    A800Window *self = user_data;
    g_autoptr(GFile) file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(src), res, NULL);
    g_autofree char *path = file ? g_file_get_path(file) : NULL;
    if (!path)
        return;
    if (a800session_state_save(self->session, path) != 0)
        show_message(GTK_WIDGET(self), "Cannot Save the State", a800session_last_error(self->session));
    else
        a800_window_toast(self, "State saved");
}

static void action_state_save(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    A800Window *self = user_data;
    GtkFileDialog *dlg = gtk_file_dialog_new();
    g_autofree char *folder = states_folder(self);
    g_autoptr(GFile) dir = g_file_new_for_path(folder);
    (void)a; (void)p;
    gtk_file_dialog_set_title(dlg, "Save State");
    gtk_file_dialog_set_initial_name(dlg, "atari.a8s");
    gtk_file_dialog_set_initial_folder(dlg, dir);
    gtk_file_dialog_save(dlg, GTK_WINDOW(self), NULL, on_state_saved, self);
    g_object_unref(dlg);
}

static void on_state_chosen(GObject *src, GAsyncResult *res, gpointer user_data)
{
    A800Window *self = user_data;
    g_autoptr(GFile) file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
    g_autofree char *path = file ? g_file_get_path(file) : NULL;
    if (!path)
        return;
    if (a800session_state_load(self->session, path) != 0)
        show_message(GTK_WIDGET(self), "Cannot Load the State", a800session_last_error(self->session));
    else
        a800_window_toast(self, "State loaded");
}

static void action_state_load(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    A800Window *self = user_data;
    g_autofree char *folder = states_folder(self);
    GtkFileDialog *dlg = file_dialog("Load State", "Save states (*.a8s)", STATE_EXTS, folder);
    (void)a; (void)p;
    gtk_file_dialog_open(dlg, GTK_WINDOW(self), NULL, on_state_chosen, self);
    g_object_unref(dlg);
}

/* ---- the machine and the console keys ----------------------------------------------------------- */

static void run_sysaction(A800Window *self, int sa)
{
    switch (sa) {
    case A800_SYSACT_PAUSE:
        a800_debugger_show(GTK_WINDOW(self), self->session);
        break;
    case A800_SYSACT_POWER:
        gtk_widget_activate_action(GTK_WIDGET(self), "win.power-cycle", NULL);
        break;
    case A800_SYSACT_REBOOT_CONFIG:
        gtk_widget_activate_action(GTK_WIDGET(self), "win.reboot-config", NULL);
        break;
    case A800_SYSACT_WARP:
        gtk_widget_activate_action(GTK_WIDGET(self), "win.warp", NULL);
        break;
    default:
        break;
    }
}

static void action_power_cycle(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    A800Window *self = user_data;
    (void)a; (void)p;
    if (!a800session_is_running(self->session)) {
        a800_window_start(self);
        return;
    }
    if (a800session_power_cycle(self->session) != 0)
        show_message(GTK_WIDGET(self), "Cannot Power Cycle", a800session_last_error(self->session));
    update_status(self);
}

static void action_reset(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    (void)a; (void)p;
    a800session_reset(A800_WINDOW(user_data)->session);
}

static void action_reboot_config(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    A800Window *self = user_data;
    (void)a; (void)p;
    if (a800session_reboot_to_config(self->session) == 0)
        a800_window_toast(self, "Back to FujiNet's CONFIG");
    rebuild_menu(self, FALSE);
}

static void action_warp(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    A800Window *self = user_data;
    gboolean on = !g_variant_get_boolean(g_action_get_state(G_ACTION(a)));
    (void)p;
    g_simple_action_set_state(a, g_variant_new_boolean(on));
    a800session_set_warp(self->session, on);
    update_status(self);
}

typedef struct {
    a800session *session;
    int key;
} ConsoleTap;

static gboolean console_up(gpointer user_data)
{
    ConsoleTap *t = user_data;
    a800session_console(t->session, t->key, 0);
    g_free(t);
    return G_SOURCE_REMOVE;
}

/* A console key from the menu: held for a tenth of a second, long enough
 * for the OS's once-a-frame look. */
static void action_console(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    A800Window *self = user_data;
    ConsoleTap *t = g_new0(ConsoleTap, 1);
    (void)a;
    t->session = self->session;
    t->key = g_variant_get_int32(p);
    a800session_console(self->session, t->key, 1);
    g_timeout_add(120, console_up, t);
}

static void action_prefs(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    A800Window *self = user_data;
    (void)a; (void)p;
    a800_prefs_show(self, self->session);
}

static void action_quit(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    (void)a; (void)p;
    gtk_window_close(GTK_WINDOW(user_data));
}

/* ---- text in and out -------------------------------------------------------------------------- */

static void action_copy_screen(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    A800Window *self = user_data;
    static char buf[8192];
    (void)a; (void)p;
    if (a800session_copy_screen_text(self->session, buf, sizeof buf) <= 0) {
        a800_window_toast(self, "No text screen to copy");
        return;
    }
    gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(self)), buf);
    a800_window_toast(self, "The screen's text is on the clipboard");
}

static void on_paste_text(GObject *src, GAsyncResult *res, gpointer user_data)
{
    A800Window *self = user_data;
    g_autofree char *text = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(src), res, NULL);
    if (text && *text)
        a800session_paste(self->session, text);
}

static void action_paste(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    A800Window *self = user_data;
    (void)a; (void)p;
    gdk_clipboard_read_text_async(gtk_widget_get_clipboard(GTK_WIDGET(self)), NULL,
                                  on_paste_text, self);
}

/* ---- the view ---------------------------------------------------------------------------------- */

static void action_controllers(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    A800Window *self = user_data;
    (void)a; (void)p;
    a800_controllers_toggle(GTK_WINDOW(self), self->session);
}

static void action_debugger(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    A800Window *self = user_data;
    (void)a; (void)p;
    a800_debugger_toggle(GTK_WINDOW(self), self->session);
}

static void action_fullscreen(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    A800Window *self = user_data;
    (void)a; (void)p;
    self->fullscreen = !self->fullscreen;
    if (self->fullscreen)
        gtk_window_fullscreen(GTK_WINDOW(self));
    else
        gtk_window_unfullscreen(GTK_WINDOW(self));
}

static void set_pref_choice(a800session *s, const char *key, int c)
{
    int i = a800session_pref_find(key);
    if (i >= 0)
        a800session_pref_set_choice(s, i, c);
}

static void action_aspect(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    A800Window *self = user_data;
    gboolean tv = !g_variant_get_boolean(g_action_get_state(G_ACTION(a)));
    (void)p;
    set_pref_choice(self->session, "aspect", tv ? 0 : 1);
    a800_window_apply_view(self);
}

static void action_smooth(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    A800Window *self = user_data;
    gboolean sm = !g_variant_get_boolean(g_action_get_state(G_ACTION(a)));
    (void)p;
    a800session_set_int(self->session, "smooth", sm);
    a800_window_apply_view(self);
}

static void action_80col(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    A800Window *self = user_data;
    gboolean on = !g_variant_get_boolean(g_action_get_state(G_ACTION(a)));
    (void)p;
    a800session_set_80col_display(self->session, on);
    g_simple_action_set_state(a, g_variant_new_boolean(on));
}

static void set_toggle(A800Window *self, const char *name, gboolean on)
{
    GAction *a = g_action_map_lookup_action(G_ACTION_MAP(self), name);
    if (a)
        g_simple_action_set_state(G_SIMPLE_ACTION(a), g_variant_new_boolean(on));
}

static void set_enabled(A800Window *self, const char *name, gboolean on)
{
    GAction *a = g_action_map_lookup_action(G_ACTION_MAP(self), name);
    if (a)
        g_simple_action_set_enabled(G_SIMPLE_ACTION(a), on);
}

void a800_window_apply_view(A800Window *self)
{
    int i = a800session_pref_find("aspect");
    gboolean tv = i < 0 || a800session_pref_get_choice(self->session, i) == 0;
    gboolean smooth = a800session_get_int(self->session, "smooth", 0) != 0;
    a800_display_set_tv_aspect(A800_DISPLAY(self->display), tv);
    a800_display_set_smooth(A800_DISPLAY(self->display), smooth);
    set_toggle(self, "tv-aspect", tv);
    set_toggle(self, "smooth", smooth);
    set_toggle(self, "col80", a800session_80col_display(self->session) != 0);
}

void a800_window_settings_changed(A800Window *self)
{
    a800_window_apply_view(self);
    a800_controllers_refresh();
    rebuild_menu(self, TRUE);
    update_status(self);
}

/* ---- FujiNet ------------------------------------------------------------------------------------- */

static void action_fujinet_config(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    A800Window *self = user_data;
    (void)a; (void)p;
    if (!a800session_fujinet_running(self->session)) {
        a800_window_toast(self, "FujiNet is off (Preferences \xe2\x96\xb8 FujiNet)");
        return;
    }
    a800_fujiconfig_show(GTK_WINDOW(self), self->session);
}

static void action_fujinet_log(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    A800Window *self = user_data;
    (void)a; (void)p;
    a800_fujilog_show(GTK_WINDOW(self), self->session);
}

static void action_fujinet_restart(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    A800Window *self = user_data;
    (void)a; (void)p;
    if (a800session_fujinet_restart(self->session) != 0)
        show_message(GTK_WIDGET(self), "Cannot Restart FujiNet",
                     a800session_last_error(self->session));
    else
        a800_window_toast(self, "FujiNet restarted");
}

static void action_about(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    A800Window *self = user_data;
    AdwDialog *about;
    (void)a; (void)p;
    about = adw_about_dialog_new();
    adw_about_dialog_set_application_name(ADW_ABOUT_DIALOG(about), "FujiNet Go Atari 800");
    adw_about_dialog_set_application_icon(ADW_ABOUT_DIALOG(about), a800_icon_name());
    adw_about_dialog_set_version(ADW_ABOUT_DIALOG(about), A800_VERSION_STRING);
    adw_about_dialog_set_developer_name(ADW_ABOUT_DIALOG(about), "Thomas Cherryhomes");
    adw_about_dialog_set_website(ADW_ABOUT_DIALOG(about), "https://fujinet.online/");
    adw_about_dialog_set_issue_url(ADW_ABOUT_DIALOG(about),
        "https://github.com/FujiNetWIFI/fujinet-go-atari800-desktop/issues");
    adw_about_dialog_set_license_type(ADW_ABOUT_DIALOG(about), GTK_LICENSE_GPL_3_0);
    adw_about_dialog_set_comments(ADW_ABOUT_DIALOG(about),
        "The Atari 400, 800, XL and XE computers with a FujiNet on the SIO port. The "
        "emulator is atari800 (GPL-2.0-or-later, the Atari800 Development Team), "
        "with the FujiNet firmware (GPL-3.0-or-later) connected over NetSIO. "
        "AltirraOS and Altirra BASIC (Avery Lee) are built in; Atari's own ROMs "
        "can be imported.");
    adw_dialog_present(about, GTK_WIDGET(self));
}

static const GActionEntry win_actions[] = {
    { "open", action_open, NULL, NULL, NULL, { 0 } },
    { "power-cycle", action_power_cycle, NULL, NULL, NULL, { 0 } },
    { "reset", action_reset, NULL, NULL, NULL, { 0 } },
    { "reboot-config", action_reboot_config, NULL, NULL, NULL, { 0 } },
    { "warp", action_warp, NULL, "false", NULL, { 0 } },
    { "state-save", action_state_save, NULL, NULL, NULL, { 0 } },
    { "state-load", action_state_load, NULL, NULL, NULL, { 0 } },
    { "import-roms", action_import_roms, NULL, NULL, NULL, { 0 } },
    { "drive-insert", action_drive_insert, "i", NULL, NULL, { 0 } },
    { "drive-eject", action_drive_eject, "i", NULL, NULL, { 0 } },
    { "drive-protect", action_drive_protect, "i", NULL, NULL, { 0 } },
    { "drive-blank", action_drive_blank, "i", NULL, NULL, { 0 } },
    { "import-sd", action_import_sd, NULL, NULL, NULL, { 0 } },
    { "sd-folder", action_sd_folder, NULL, NULL, NULL, { 0 } },
    { "media-folder", action_media_folder, NULL, NULL, NULL, { 0 } },
    { "cart-insert", action_cart_insert, NULL, NULL, NULL, { 0 } },
    { "cart-piggyback", action_cart_piggyback, NULL, NULL, NULL, { 0 } },
    { "cart-remove", action_cart_remove, NULL, NULL, NULL, { 0 } },
    { "cart-remove-piggyback", action_cart_remove_piggyback, NULL, NULL, NULL, { 0 } },
    { "tape-insert", action_tape_insert, NULL, NULL, NULL, { 0 } },
    { "tape-record", action_tape_record, NULL, NULL, NULL, { 0 } },
    { "tape-rewind", action_tape_rewind, NULL, NULL, NULL, { 0 } },
    { "tape-boot", action_tape_boot, NULL, NULL, NULL, { 0 } },
    { "tape-eject", action_tape_eject, NULL, NULL, NULL, { 0 } },
    { "console", action_console, "i", NULL, NULL, { 0 } },
    { "copy-screen", action_copy_screen, NULL, NULL, NULL, { 0 } },
    { "paste", action_paste, NULL, NULL, NULL, { 0 } },
    { "controllers", action_controllers, NULL, NULL, NULL, { 0 } },
    { "debugger", action_debugger, NULL, NULL, NULL, { 0 } },
    { "col80", action_80col, NULL, "false", NULL, { 0 } },
    { "tv-aspect", action_aspect, NULL, "true", NULL, { 0 } },
    { "smooth", action_smooth, NULL, "false", NULL, { 0 } },
    { "fullscreen", action_fullscreen, NULL, NULL, NULL, { 0 } },
    { "fujinet-config", action_fujinet_config, NULL, NULL, NULL, { 0 } },
    { "fujinet-log", action_fujinet_log, NULL, NULL, NULL, { 0 } },
    { "fujinet-restart", action_fujinet_restart, NULL, NULL, NULL, { 0 } },
    { "prefs", action_prefs, NULL, NULL, NULL, { 0 } },
    { "about", action_about, NULL, NULL, NULL, { 0 } },
    { "quit", action_quit, NULL, NULL, NULL, { 0 } },
};

/* ---- the menu ------------------------------------------------------------------------------------- */

static void append_target(GMenu *m, const char *label, const char *action, int target)
{
    GMenuItem *item = g_menu_item_new(label, NULL);
    g_menu_item_set_action_and_target_value(item, action, g_variant_new_int32(target));
    g_menu_append_item(m, item);
    g_object_unref(item);
}

static GMenu *drives_menu(A800Window *self, gboolean fuji)
{
    GMenu *menu = g_menu_new(), *drives = g_menu_new(), *more = g_menu_new();

    for (int d = 0; d < A800_DRIVES; d++) {
        a800_drive_info di;
        GMenu *sub = g_menu_new();
        g_autofree char *file = NULL;
        g_autofree char *label = NULL;
        if (a800session_drive_info(self->session, d, &di) != 0)
            memset(&di, 0, sizeof di);
        file = menu_escape(di.path[0] ? base_name(di.path) : "empty");
        label = g_strdup_printf("D%d: %s%s", d + 1, file,
                                di.path[0] && di.read_only ? " (protected)" : "");
        append_target(sub, fuji ? "_Mount from the SD Card\xe2\x80\xa6" : "_Insert\xe2\x80\xa6",
                      "win.drive-insert", d);
        append_target(sub, "_New Blank Disk\xe2\x80\xa6", "win.drive-blank", d);
        if (di.path[0]) {
            append_target(sub, "_Eject", "win.drive-eject", d);
            if (!fuji)
                append_target(sub, di.read_only ? "Allow _Writing" : "_Write Protect",
                              "win.drive-protect", d);
        }
        g_menu_append_submenu(drives, label, G_MENU_MODEL(sub));
        g_object_unref(sub);
    }
    g_menu_append_section(menu, fuji ? "FujiNet's drives" : "atari800's drives",
                          G_MENU_MODEL(drives));
    if (fuji) {
        g_menu_append(more, "_Import to SD Card\xe2\x80\xa6", "win.import-sd");
        g_menu_append(more, "Open _SD Folder", "win.sd-folder");
    } else {
        g_menu_append(more, "Open _Media Folder", "win.media-folder");
    }
    g_menu_append_section(menu, NULL, G_MENU_MODEL(more));
    g_object_unref(drives);
    g_object_unref(more);
    return menu;
}

static GMenu *build_menu(A800Window *self)
{
    gboolean fuji = a800session_fujinet_running(self->session);
    GMenu *menu = g_menu_new();
    GMenu *machine = g_menu_new(), *media = g_menu_new(), *edit = g_menu_new();
    GMenu *view = g_menu_new(), *fujim = g_menu_new(), *app = g_menu_new();
    GMenu *cart = g_menu_new(), *tape = g_menu_new(), *console = g_menu_new();
    GMenu *drives = drives_menu(self, fuji);
    char info[256], label[300];

    g_menu_append(machine, "_Open\xe2\x80\xa6", "win.open");
    g_menu_append(machine, "_Power Cycle (Shift+F5)", "win.power-cycle");
    g_menu_append(machine, "_Reset (F5)", "win.reset");
    if (fuji)
        g_menu_append(machine, "Reboot to _CONFIG", "win.reboot-config");
    g_menu_append(machine, "_Warp Speed", "win.warp");
    g_menu_append(machine, "_Save State\xe2\x80\xa6", "win.state-save");
    g_menu_append(machine, "_Load State\xe2\x80\xa6", "win.state-load");
    g_menu_append(machine, "Import System _ROMs\xe2\x80\xa6", "win.import-roms");
    g_menu_append_section(menu, "Machine", G_MENU_MODEL(machine));

    g_menu_append_submenu(media, "_Drives", G_MENU_MODEL(drives));

    if (a800session_cart_info(self->session, info, sizeof info) > 0) {
        g_autofree char *e = menu_escape(info);
        g_snprintf(label, sizeof label, "In it: %s", e);
        g_menu_append(cart, label, "win.cart-noop");
    }
    g_menu_append(cart, "_Insert Cartridge\xe2\x80\xa6", "win.cart-insert");
    g_menu_append(cart, "Insert _Piggyback\xe2\x80\xa6", "win.cart-piggyback");
    g_menu_append(cart, "_Remove Cartridge", "win.cart-remove");
    g_menu_append(cart, "Remove Pi_ggyback", "win.cart-remove-piggyback");
    g_menu_append_submenu(media, "_Cartridge", G_MENU_MODEL(cart));

    {
        int block = 0, blocks = 0;
        if (a800session_tape_position(self->session, &block, &blocks) == 0 && blocks > 0) {
            g_snprintf(label, sizeof label, "Block %d of %d", block, blocks);
            g_menu_append(tape, label, "win.cart-noop");
        }
    }
    g_menu_append(tape, "_Load Tape\xe2\x80\xa6", "win.tape-insert");
    g_menu_append(tape, "_Record New Tape\xe2\x80\xa6", "win.tape-record");
    g_menu_append(tape, "Re_wind", "win.tape-rewind");
    g_menu_append(tape, "_Boot Tape (Start held at power-on)", "win.tape-boot");
    g_menu_append(tape, "_Eject", "win.tape-eject");
    g_menu_append_submenu(media, "_Program Recorder", G_MENU_MODEL(tape));

    append_target(console, "_Option (F2)", "win.console", A800_CON_OPTION);
    append_target(console, "_Select (F3)", "win.console", A800_CON_SELECT);
    append_target(console, "S_tart (F4)", "win.console", A800_CON_START);
    append_target(console, "_Help (F6)", "win.console", A800_CON_HELP);
    append_target(console, "_Break (F7, Pause)", "win.console", A800_CON_BREAK);
    g_menu_append_submenu(media, "Co_nsole Keys", G_MENU_MODEL(console));
    g_menu_append_section(menu, NULL, G_MENU_MODEL(media));

    g_menu_append(edit, "_Copy Screen Text", "win.copy-screen");
    g_menu_append(edit, "P_aste", "win.paste");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(edit));

    g_menu_append(view, "C_ontrollers (F9)", "win.controllers");
    g_menu_append(view, "De_bugger (F12)", "win.debugger");
    if (a800session_80col_fitted(self->session))
        g_menu_append(view, "_80-Column Display", "win.col80");
    g_menu_append(view, "_TV Aspect", "win.tv-aspect");
    g_menu_append(view, "S_mooth Scaling", "win.smooth");
    g_menu_append(view, "_Fullscreen (F11)", "win.fullscreen");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(view));

    g_menu_append(fujim, "FujiNet _Web UI", "win.fujinet-config");
    g_menu_append(fujim, "Console _Log", "win.fujinet-log");
    g_menu_append(fujim, "R_estart FujiNet", "win.fujinet-restart");
    g_menu_append_section(menu, "FujiNet", G_MENU_MODEL(fujim));

    g_menu_append(app, "_Preferences", "win.prefs");
    g_menu_append(app, "_About FujiNet Go Atari 800", "win.about");
    g_menu_append(app, "_Quit", "win.quit");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(app));

    g_object_unref(machine);
    g_object_unref(media);
    g_object_unref(drives);
    g_object_unref(cart);
    g_object_unref(tape);
    g_object_unref(console);
    g_object_unref(edit);
    g_object_unref(view);
    g_object_unref(fujim);
    g_object_unref(app);
    return menu;
}

/* What the media menus show, to rebuild only on a change: GtkPopoverMenu
 * names its submenu pages by label, and changing the model it shows in
 * place re-adds pages whose names are still taken. */
static void rebuild_menu(A800Window *self, gboolean force)
{
    GString *sig = g_string_new(NULL);
    gboolean fuji = a800session_fujinet_running(self->session);
    char info[256];
    int block = 0, blocks = 0;

    g_string_append_printf(sig, "%d|%d|", fuji, a800session_80col_fitted(self->session));
    for (int d = 0; d < A800_DRIVES; d++) {
        a800_drive_info di;
        if (a800session_drive_info(self->session, d, &di) == 0)
            g_string_append_printf(sig, "%s%c|", di.path, di.read_only ? '+' : '-');
    }
    a800session_cart_info(self->session, info, sizeof info);
    a800session_tape_position(self->session, &block, &blocks);
    g_string_append_printf(sig, "%s|%d", info, blocks);

    set_enabled(self, "state-save", a800session_states_available(self->session));
    set_enabled(self, "state-load", a800session_states_available(self->session));
    set_enabled(self, "fujinet-config", fuji);
    set_enabled(self, "fujinet-restart", a800session_get_int(self->session, "enable_fujinet", 1));

    if (force || strcmp(sig->str, self->menu_sig) != 0) {
        GMenu *menu = build_menu(self);
        g_strlcpy(self->menu_sig, sig->str, sizeof self->menu_sig);
        gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(self->menu_button), G_MENU_MODEL(menu));
        g_object_unref(menu);
    }
    g_string_free(sig, TRUE);
}

/* ---- construction ---------------------------------------------------------------------------------- */

static void a800_window_dispose(GObject *object)
{
    A800Window *self = A800_WINDOW(object);
    if (self->status_id) {
        g_source_remove(self->status_id);
        self->status_id = 0;
    }
    if (self->fast_id) {
        g_source_remove(self->fast_id);
        self->fast_id = 0;
    }
    g_clear_pointer(&self->shot_path, g_free);
    G_OBJECT_CLASS(a800_window_parent_class)->dispose(object);
}

static void a800_window_class_init(A800WindowClass *klass)
{
    G_OBJECT_CLASS(klass)->dispose = a800_window_dispose;
}

static void a800_window_init(A800Window *self)
{
    (void)self;
}

static GtkWidget *dot(const char *tip)
{
    GtkWidget *d = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(d, "a800-dot");
    gtk_widget_add_css_class(d, "a800-dot-off");
    gtk_widget_set_valign(d, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(d, tip);
    return d;
}

static gboolean env_on(const char *name)
{
    const char *v = g_getenv(name);
    return v && *v && *v != '0';
}

static void noop(GSimpleAction *a, GVariant *p, gpointer u)
{
    (void)a; (void)p; (void)u;
}

GtkWidget *a800_window_new(AdwApplication *app, a800session *session)
{
    A800Window *self = g_object_new(A800_TYPE_WINDOW, "application", app, NULL);
    GtkWidget *header, *menu_button, *toolbar, *status_box;
    GtkEventController *keys;

    self->session = session;

    gtk_window_set_title(GTK_WINDOW(self), "FujiNet Go Atari 800");
    gtk_window_set_icon_name(GTK_WINDOW(self), a800_icon_name());
    /* 336 x 240 at three times, a TV's shape: 864 x 720 */
    gtk_window_set_default_size(GTK_WINDOW(self), 864, 720 + 46);

    g_action_map_add_action_entries(G_ACTION_MAP(self), win_actions, G_N_ELEMENTS(win_actions),
                                    self);
    {
        /* information rows in the menu: shown, never active */
        GSimpleAction *info = g_simple_action_new("cart-noop", NULL);
        g_simple_action_set_enabled(info, FALSE);
        g_signal_connect(info, "activate", G_CALLBACK(noop), NULL);
        g_action_map_add_action(G_ACTION_MAP(self), G_ACTION(info));
        g_object_unref(info);
    }
    {
        /* CTRL+SHIFT: CTRL alone is the Atari's */
        static const char *const prefs[] = { "<Control>comma", NULL };
        static const char *const quit[] = { "<Control><Shift>q", NULL };
        static const char *const open[] = { "<Control><Shift>o", NULL };
        static const char *const copy[] = { "<Control><Shift>c", NULL };
        static const char *const paste[] = { "<Control><Shift>v", NULL };
        static const char *const warp[] = { "<Control><Shift>w", NULL };
        GtkApplication *a = GTK_APPLICATION(app);
        gtk_application_set_accels_for_action(a, "win.prefs", prefs);
        gtk_application_set_accels_for_action(a, "win.quit", quit);
        gtk_application_set_accels_for_action(a, "win.open", open);
        gtk_application_set_accels_for_action(a, "win.copy-screen", copy);
        gtk_application_set_accels_for_action(a, "win.paste", paste);
        gtk_application_set_accels_for_action(a, "win.warp", warp);
    }

    header = adw_header_bar_new();
    menu_button = gtk_menu_button_new();
    self->menu_button = menu_button;
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menu_button), "open-menu-symbolic");
    gtk_menu_button_set_primary(GTK_MENU_BUTTON(menu_button), TRUE);
    gtk_widget_set_tooltip_text(menu_button, "Main Menu");
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), menu_button);

    status_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    self->status_dot = dot("FujiNet link");
    self->status = gtk_label_new("Starting\xe2\x80\xa6");
    gtk_widget_add_css_class(self->status, "dim-label");
    gtk_label_set_ellipsize(GTK_LABEL(self->status), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(status_box), self->status_dot);
    gtk_box_append(GTK_BOX(status_box), self->status);
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), status_box);

    self->display = a800_display_new(session);
    a800_window_apply_view(self);
    {
        GtkDropTarget *drop = gtk_drop_target_new(G_TYPE_FILE, GDK_ACTION_COPY);
        g_signal_connect(drop, "drop", G_CALLBACK(on_drop), self);
        gtk_widget_add_controller(self->display, GTK_EVENT_CONTROLLER(drop));
    }

    self->toast_overlay = adw_toast_overlay_new();
    adw_toast_overlay_set_child(ADW_TOAST_OVERLAY(self->toast_overlay), self->display);
    toolbar = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), self->toast_overlay);
    adw_application_window_set_content(ADW_APPLICATION_WINDOW(self), toolbar);

    /* capture on the WINDOW so typing works whatever has focus */
    keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key_pressed), self);
    g_signal_connect(keys, "key-released", G_CALLBACK(on_key_released), self);
    gtk_widget_add_controller(GTK_WIDGET(self), keys);
    g_signal_connect(self, "notify::is-active", G_CALLBACK(on_active_changed), self);

    /* the current pads and FujiNet state are the baseline: no toasts for them */
    self->pad_generation = a800session_gamepad_generation(session);
    self->fujinet_generation = a800session_fujinet_generation(session);
    rebuild_menu(self, TRUE);
    self->status_id = g_timeout_add_seconds(1, status_tick, self);
    self->fast_id = g_timeout_add(100, fast_tick, self);
    update_status(self);

    /* A800_OPEN_CONTROLLERS / _DEBUGGER / _SETTINGS open those windows at
     * launch, the family's way in when the app misbehaves before the menu
     * is reachable. A800_SCREENSHOT=file.png saves the picture after
     * A800_SCREENSHOT_FRAMES frames (300); A800_AUTOQUIT_FRAMES quits after
     * that many. */
    if (env_on("A800_OPEN_CONTROLLERS"))
        a800_controllers_toggle(GTK_WINDOW(self), session);
    if (env_on("A800_OPEN_DEBUGGER"))
        a800_debugger_show(GTK_WINDOW(self), session);
    if (env_on("A800_OPEN_SETTINGS"))
        a800_prefs_show(self, session);
    if (g_getenv("A800_SCREENSHOT") && *g_getenv("A800_SCREENSHOT")) {
        const char *n = g_getenv("A800_SCREENSHOT_FRAMES");
        self->shot_path = g_strdup(g_getenv("A800_SCREENSHOT"));
        self->shot_frames = n ? g_ascii_strtoull(n, NULL, 10) : 300;
    }
    if (g_getenv("A800_AUTOQUIT_FRAMES"))
        self->quit_frames = g_ascii_strtoull(g_getenv("A800_AUTOQUIT_FRAMES"), NULL, 10);
    return GTK_WIDGET(self);
}
