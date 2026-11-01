/*
 * The FujiNet console log window: a live view of the in-process runtime's
 * captured output, refreshed once a second. And the configuration, which is
 * the FujiNet web UI in the system browser.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fujilog.h"

typedef struct {
    GtkWindow *win;
    GtkTextView *view;
    GtkScrolledWindow *scroll;
    a800session *session;
    guint timer;
    char *last;        /* the text on show: an unchanged log is left alone */
    gboolean follow;   /* pin the end as the view grows */
} LogWindow;

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
    g_free(lw->last);
    g_free(lw);
}

static gboolean refresh(gpointer user_data)
{
    LogWindow *lw = user_data;
    static char buf[128 * 1024];
    GtkTextBuffer *b = gtk_text_view_get_buffer(lw->view);
    GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(lw->scroll);
    const char *text;
    char *clean;
    GtkTextIter it;
    int n, top = 0;

    n = a800session_fujinet_copy_log(lw->session, buf, sizeof buf);
    text = n > 0 ? buf : "(no FujiNet output yet)";
    clean = g_utf8_make_valid(text, -1);
    if (lw->last && !strcmp(lw->last, clean)) {
        g_free(clean);
        return G_SOURCE_CONTINUE;
    }

    /* Follow the tail only while the reader is at the bottom (or has not
     * been laid out yet). */
    lw->follow = !adj || gtk_adjustment_get_page_size(adj) <= 0.0 ||
                 gtk_adjustment_get_value(adj) + gtk_adjustment_get_page_size(adj) >=
                     gtk_adjustment_get_upper(adj) - 2.0;
    if (!lw->follow && adj && gtk_adjustment_get_value(adj) > 0.0) {
        gtk_text_view_get_line_at_y(lw->view, &it, (int)gtk_adjustment_get_value(adj), NULL);
        top = gtk_text_iter_get_line(&it);
    }

    gtk_text_buffer_set_text(b, clean, -1);
    g_free(lw->last);
    lw->last = clean;

    if (lw->follow) {
        if (adj)
            on_resized(adj, lw);
    }
    else if (top > 0) {
        /* keep the reader's line: scroll_to_mark waits for the new layout */
        GtkTextMark *mark;
        gtk_text_buffer_get_iter_at_line(b, &it, top);
        mark = gtk_text_buffer_create_mark(b, NULL, &it, TRUE);
        gtk_text_view_scroll_to_mark(lw->view, mark, 0.0, TRUE, 0.0, 0.0);
        gtk_text_buffer_delete_mark(b, mark);
    }
    return G_SOURCE_CONTINUE;
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
        adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar),
                                     adw_header_bar_new());
        adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), scroll);
        adw_window_set_content(ADW_WINDOW(lw->win), toolbar);
    }

    lw->follow = TRUE;
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
