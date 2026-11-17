/*
 * The FujiNet console log window: a live view of the in-process runtime's
 * captured output, refreshed once a second. And the configuration, which is
 * the FujiNet web UI in the system browser.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fujilog.h"
#include "help/help_window.h"

typedef struct {
    GtkWindow *win;
    GtkTextView *view;
    GtkScrolledWindow *scroll;
    a800session *session;
    guint timer;
    a800_log_cursor cursor;   /* where in FujiNet's log the view has got to */
    gboolean placeholder;     /* the "no output yet" line is on show, not log text */
    gboolean follow;          /* pin the end as the view grows */
} LogWindow;

/* a long session keeps the last LOG_KEEP characters, whole lines */
#define LOG_KEEP (256 * 1024)

/* GtkTextView lays new text out later and moves the adjustment itself, so the
 * end is pinned whenever the height changes; whether to follow is decided as
 * text arrives (the same scheme as the debugger's logs). */
static void on_resized(GtkAdjustment *adj, gpointer user_data)
{
    LogWindow *lw = user_data;
    if (lw->follow)
        gtk_adjustment_set_value(adj, gtk_adjustment_get_upper(adj) -
                                          gtk_adjustment_get_page_size(adj));
}

static void on_destroy(GtkWidget *w, gpointer user_data)
{
    LogWindow *lw = user_data;
    (void)w;
    if (lw->timer) {
        g_source_remove(lw->timer);
        lw->timer = 0;
    }
    g_free(lw);
}

/* Appends what the log gained since the last tick. The view is never
 * rebuilt, so the reader's scroll position and selection stay where they
 * are; only a reader already at the bottom follows the new text. A restart
 * of FujiNet (a new log) clears the view. */
static gboolean refresh(gpointer user_data)
{
    LogWindow *lw = user_data;
    static char buf[64 * 1024];
    GtkTextBuffer *b = gtk_text_view_get_buffer(lw->view);
    GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(lw->scroll);
    GtkTextIter it;
    int n, reset, rounds = 0;

    do {
        char *clean;
        n = a800session_fujinet_read_log(lw->session, &lw->cursor, buf, sizeof buf, &reset);
        if (n <= 0 && !reset)
            break;

        if (reset || lw->placeholder) {
            gtk_text_buffer_set_text(b, "", -1);
            lw->placeholder = FALSE;
            lw->follow = TRUE;
        }
        else {
            /* Follow the tail only while the reader is at the bottom (or
             * has not been laid out yet). */
            lw->follow = !adj || gtk_adjustment_get_page_size(adj) <= 0.0 ||
                         gtk_adjustment_get_value(adj) + gtk_adjustment_get_page_size(adj) >=
                             gtk_adjustment_get_upper(adj) - 2.0;
        }

        clean = g_utf8_make_valid(buf, -1);
        gtk_text_buffer_get_end_iter(b, &it);
        gtk_text_buffer_insert(b, &it, clean, -1);
        g_free(clean);

        {
            int count = gtk_text_buffer_get_char_count(b);
            if (count > LOG_KEEP) {
                GtkTextIter s0, e0;
                gtk_text_buffer_get_start_iter(b, &s0);
                gtk_text_buffer_get_iter_at_offset(b, &e0, count - LOG_KEEP * 3 / 4);
                gtk_text_iter_forward_line(&e0);
                gtk_text_buffer_delete(b, &s0, &e0);
            }
        }
        if (lw->follow && adj)
            on_resized(adj, lw);
    } while (n >= (int)sizeof buf - 1 && ++rounds < 16);
    return G_SOURCE_CONTINUE;
}

static gboolean on_key(GtkEventControllerKey *c, guint keyval, guint code,
                       GdkModifierType st, gpointer ud)
{
    (void)c; (void)code; (void)st;
    if (keyval != GDK_KEY_F1)
        return FALSE;
    a800_help_for(ud);
    return TRUE;
}

void a800_fujilog_show(GtkWindow *parent, a800session *session)
{
    static GtkWindow *existing;
    LogWindow *lw;
    GtkWidget *scroll, *view;

    if (existing) {
        gtk_window_present(existing);
        return;
    }

    lw = g_new0(LogWindow, 1);
    lw->session = session;

    view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(view), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), GTK_WRAP_WORD_CHAR);

    scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), view);
    gtk_widget_set_vexpand(scroll, TRUE);

    lw->win = GTK_WINDOW(adw_window_new());
    lw->view = GTK_TEXT_VIEW(view);
    lw->scroll = GTK_SCROLLED_WINDOW(scroll);
    gtk_window_set_title(lw->win, "FujiNet Console Log");
    gtk_window_set_default_size(lw->win, 820, 560);
    gtk_window_set_transient_for(lw->win, parent);
    gtk_window_set_application(lw->win, gtk_window_get_application(parent));

    {
        GtkWidget *toolbar = adw_toolbar_view_new();
        GtkWidget *header = adw_header_bar_new();
        GtkEventController *keys = gtk_event_controller_key_new();
        adw_header_bar_pack_end(ADW_HEADER_BAR(header), a800_help_button("fujilog"));
        a800_help_set_context(lw->win, "fujilog");
        g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key), lw->win);
        gtk_widget_add_controller(GTK_WIDGET(lw->win), keys);
        adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
        adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), scroll);
        adw_window_set_content(ADW_WINDOW(lw->win), toolbar);
    }

    lw->follow = TRUE;
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(lw->view), "(no FujiNet output yet)", -1);
    lw->placeholder = TRUE;
    g_signal_connect(gtk_scrolled_window_get_vadjustment(lw->scroll), "changed",
                     G_CALLBACK(on_resized), lw);
    g_signal_connect(lw->win, "destroy", G_CALLBACK(on_destroy), lw);
    lw->timer = g_timeout_add(1000, refresh, lw);
    refresh(lw);

    existing = lw->win;
    g_object_add_weak_pointer(G_OBJECT(lw->win), (gpointer *)&existing);
    gtk_window_present(lw->win);
}

void a800_fujiconfig_show(GtkWindow *parent, a800session *session)
{
    GtkUriLauncher *l = gtk_uri_launcher_new(a800session_fujinet_webui_url(session));
    gtk_uri_launcher_launch(l, parent, NULL, NULL, NULL);
    g_object_unref(l);
}
