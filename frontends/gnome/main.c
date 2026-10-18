/*
 * FujiNet Go Atari 800 -- the GNOME (GTK4/libadwaita) frontend.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <adwaita.h>
#include <glib-unix.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>

#include "a800session.h"
#include "window.h"

#define APP_ID "online.fujinet.go.atari800.gnome"

static a800session *g_session;
static char *g_media_arg;

/* The installed icons are named after the desktop-entry id. Running straight
 * out of the build tree there is no such icon, so point GTK's icon theme at
 * the in-tree artwork (named for the project, not the id) and use that --
 * otherwise GTK's window-icon lookup fails with a NULL texture on every
 * frame it tries to paint the icon. */
const char *a800_icon_name(void)
{
    static const char *name;
    GtkIconTheme *theme;

    if (name)
        return name;

    name = APP_ID;
    theme = gtk_icon_theme_get_for_display(gdk_display_get_default());
    if (theme && !gtk_icon_theme_has_icon(theme, name)) {
        gtk_icon_theme_add_search_path(theme, A800_SOURCE_ICON_DIR);
        if (gtk_icon_theme_has_icon(theme, "fujinet-go-atari800"))
            name = "fujinet-go-atari800";
    }
    return name;
}

/* The Controllers, debugger and log windows are hidden rather than destroyed
 * and belong to the application (so the shell groups them with it), which
 * means the application would outlive its main window. Closing the main
 * window is quitting. */
static gboolean on_main_close(GtkWindow *win, gpointer user_data)
{
    (void)win;
    g_application_quit(G_APPLICATION(user_data));
    return FALSE;
}

/* SIGTERM (a logout, a service manager) and SIGINT (Ctrl+C in the terminal)
 * quit the way closing the window does, so the session stops the machine
 * and FujiNet and flushes the settings instead of dying mid-write. */
static gboolean on_quit_signal(gpointer user_data)
{
    g_application_quit(G_APPLICATION(user_data));
    return G_SOURCE_CONTINUE;
}

static void on_activate(AdwApplication *app, gpointer user_data)
{
    GtkWidget *win;
    (void)user_data;

    /* One window per process: the machine, and the FujiNet inside it, are
     * process singletons. Raising the existing window is the honest response
     * to a second activation. */
    win = GTK_WIDGET(gtk_application_get_active_window(GTK_APPLICATION(app)));
    if (win) {
        gtk_window_present(GTK_WINDOW(win));
        return;
    }

    a800_install_accent_css();
    win = a800_window_new(app, g_session);
    g_signal_connect(win, "close-request", G_CALLBACK(on_main_close), app);
    gtk_window_present(GTK_WINDOW(win));

    /* FujiNet first, then the machine (the built-in OS needs no ROMs) */
    a800_window_start(A800_WINDOW(win));

    /* A file named on the command line goes where Open would put it: a
     * disk boots, a program runs, a cartridge goes in, a ROM is imported. */
    if (g_media_arg) {
        a800_window_load_media(A800_WINDOW(win), g_media_arg);
        g_clear_pointer(&g_media_arg, g_free);
    }
}

static void on_open(GApplication *app, GFile **files, gint n_files,
                    const gchar *hint, gpointer user_data)
{
    GtkWindow *win;
    (void)hint; (void)user_data;
    if (n_files <= 0) {
        g_application_activate(app);
        return;
    }
    /* Already running: the file is for the machine that is up. */
    win = gtk_application_get_active_window(GTK_APPLICATION(app));
    if (win) {
        g_autofree char *path = g_file_get_path(files[0]);
        gtk_window_present(win);
        if (path)
            a800_window_load_media(A800_WINDOW(win), path);
        return;
    }
    g_free(g_media_arg);
    g_media_arg = g_file_get_path(files[0]);
    g_application_activate(app);
}

int main(int argc, char **argv)
{
    AdwApplication *app;
    int status;

    g_session = a800session_new(NULL);
    if (!g_session) {
        g_printerr("Could not create the session (unusable config/data "
                   "directories?)\n");
        return 1;
    }

    app = adw_application_new(APP_ID, G_APPLICATION_HANDLES_OPEN);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    g_signal_connect(app, "open", G_CALLBACK(on_open), NULL);
    g_unix_signal_add(SIGTERM, on_quit_signal, app);
    g_unix_signal_add(SIGINT, on_quit_signal, app);

    status = g_application_run(G_APPLICATION(app), argc, argv);

    a800session_stop(g_session);
    a800session_free(g_session);
    g_object_unref(app);
    g_free(g_media_arg);
    return status;
}
