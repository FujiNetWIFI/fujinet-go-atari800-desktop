/*
 * The Preferences dialog: an AdwPreferencesDialog built from the session's
 * preference table (a800session_pref), so the four frontends show the same
 * pages, groups and rows. No .ui file.
 *
 * Rows that apply LIVE (the picture, the sound, the ports) take effect at
 * once. The machine -- model, memory, ROMs, peripherals -- is the computer
 * you would switch off to change: those rows wait, and closing the dialog
 * applies them together (one power cycle), as do the host options (FujiNet,
 * sound, gamepads: a session restart). Rows follow each other as they
 * change: memory sizes by model, ROM revisions by what has been imported,
 * peripherals by what the machine can take.
 *
 * A ROMs page lists every slot, imported or not, each with its own Import:
 * peripheral ROMs have no published checksums, so they are put in their
 * slot by hand.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "prefs.h"

#include "controllers/controllers_window.h"

#include <string.h>

#define MAX_ROWS 128
#define MAX_ROM_ROWS 64

typedef struct {
    A800Window *window;
    a800session *session;
    AdwDialog *dialog;
    GtkWidget *rows[MAX_ROWS];          /* by pref index */
    char *labels[MAX_ROWS];             /* a choice row's labels, as shown */
    GtkWidget *rom_rows[MAX_ROM_ROWS];
    gboolean syncing;
    int pending;                        /* the heaviest change not applied yet */
} PrefsState;

static void refresh_rows(PrefsState *st);

static const char *page_icon(const char *page)
{
    if (!strcmp(page, "Machine"))
        return "computer-symbolic";
    if (!strcmp(page, "Peripherals"))
        return "drive-harddisk-symbolic";
    if (!strcmp(page, "Video"))
        return "video-display-symbolic";
    if (!strcmp(page, "Audio"))
        return "audio-speakers-symbolic";
    if (!strcmp(page, "Input"))
        return "input-gaming-symbolic";
    if (!strcmp(page, "FujiNet"))
        return "network-wireless-symbolic";
    return "applications-system-symbolic";
}

/* A change to pref i: LIVE ones apply now unless something heavier waits
 * (applying then would power-cycle mid-edit). */
static void changed(PrefsState *st, int i)
{
    const a800_pref *p = a800session_pref(i);
    if (p->applies > st->pending)
        st->pending = p->applies;
    if (st->pending == A800_APPLY_LIVE)
        a800session_apply_settings(st->session);
    if (!strcmp(p->key, "aspect") || !strcmp(p->key, "smooth"))
        a800_window_apply_view(st->window);
    if (!strcmp(p->key, "machine") || g_str_has_suffix(p->key, "_device"))
        a800_controllers_refresh();
    refresh_rows(st);
}

/* ---- rows -------------------------------------------------------------------- */

static void on_switch(GObject *row, GParamSpec *ps, gpointer ud)
{
    PrefsState *st = ud;
    int i = GPOINTER_TO_INT(g_object_get_data(row, "a800-pref"));
    (void)ps;
    if (st->syncing)
        return;
    a800session_set_int(st->session, a800session_pref(i)->key,
                        adw_switch_row_get_active(ADW_SWITCH_ROW(row)));
    changed(st, i);
}

static void on_spin(GObject *row, GParamSpec *ps, gpointer ud)
{
    PrefsState *st = ud;
    int i = GPOINTER_TO_INT(g_object_get_data(row, "a800-pref"));
    (void)ps;
    if (st->syncing)
        return;
    a800session_set_int(st->session, a800session_pref(i)->key,
                        (int)adw_spin_row_get_value(ADW_SPIN_ROW(row)));
    changed(st, i);
}

static void on_combo(GObject *row, GParamSpec *ps, gpointer ud)
{
    PrefsState *st = ud;
    int i = GPOINTER_TO_INT(g_object_get_data(row, "a800-pref"));
    guint c = adw_combo_row_get_selected(ADW_COMBO_ROW(row));
    (void)ps;
    if (st->syncing || c == GTK_INVALID_LIST_POSITION)
        return;
    if (!a800session_pref_choice_enabled(st->session, i, (int)c)) {
        /* not imported, or not on this machine: put it back */
        a800_window_toast(st->window, "Not available \xe2\x80\x94 import its ROM first, or "
                                      "choose a machine that takes it");
        refresh_rows(st);
        return;
    }
    a800session_pref_set_choice(st->session, i, (int)c);
    changed(st, i);
}

typedef struct {
    PrefsState *st;
    int i;
} PathCtx;

static void on_path_chosen(GObject *src, GAsyncResult *res, gpointer ud)
{
    PathCtx *c = ud;
    const a800_pref *p = a800session_pref(c->i);
    g_autoptr(GFile) f = p->type == A800_PREF_DIR
        ? gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(src), res, NULL)
        : gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
    g_autofree char *path = f ? g_file_get_path(f) : NULL;
    if (path) {
        a800session_set_str(c->st->session, p->key, path);
        changed(c->st, c->i);
    }
    g_free(c);
}

static void on_path_choose(GtkButton *b, gpointer ud)
{
    PrefsState *st = ud;
    int i = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "a800-pref"));
    const a800_pref *p = a800session_pref(i);
    GtkFileDialog *dlg = gtk_file_dialog_new();
    PathCtx *c = g_new0(PathCtx, 1);
    const char *cur = a800session_get_str(st->session, p->key, "");
    c->st = st;
    c->i = i;
    gtk_file_dialog_set_title(dlg, p->label);
    if (cur && *cur) {
        g_autoptr(GFile) f = g_file_new_for_path(cur);
        if (p->type == A800_PREF_DIR)
            gtk_file_dialog_set_initial_folder(dlg, f);
        else
            gtk_file_dialog_set_initial_file(dlg, f);
    }
    if (p->type == A800_PREF_DIR)
        gtk_file_dialog_select_folder(dlg, GTK_WINDOW(st->window), NULL, on_path_chosen, c);
    else
        gtk_file_dialog_open(dlg, GTK_WINDOW(st->window), NULL, on_path_chosen, c);
    g_object_unref(dlg);
}

static void on_path_clear(GtkButton *b, gpointer ud)
{
    PrefsState *st = ud;
    int i = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "a800-pref"));
    a800session_set_str(st->session, a800session_pref(i)->key, "");
    changed(st, i);
}

static GtkWidget *icon_button(const char *icon, const char *tip, int i, GCallback cb,
                              PrefsState *st)
{
    GtkWidget *b = gtk_button_new_from_icon_name(icon);
    gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(b, "flat");
    gtk_widget_set_tooltip_text(b, tip);
    g_object_set_data(G_OBJECT(b), "a800-pref", GINT_TO_POINTER(i));
    g_signal_connect(b, "clicked", cb, st);
    return b;
}

static GtkWidget *make_row(PrefsState *st, int i)
{
    const a800_pref *p = a800session_pref(i);
    GtkWidget *row;

    switch (p->type) {
    case A800_PREF_BOOL:
        row = adw_switch_row_new();
        g_signal_connect(row, "notify::active", G_CALLBACK(on_switch), st);
        break;
    case A800_PREF_INT:
        row = adw_spin_row_new_with_range(p->min, p->max, p->step > 0 ? p->step : 1);
        g_signal_connect(row, "notify::value", G_CALLBACK(on_spin), st);
        break;
    case A800_PREF_CHOICE:
        row = adw_combo_row_new();
        g_signal_connect(row, "notify::selected", G_CALLBACK(on_combo), st);
        break;
    default:   /* a file or a folder */
        row = adw_action_row_new();
        adw_action_row_add_suffix(ADW_ACTION_ROW(row),
                                  icon_button("edit-clear-symbolic", "Clear", i,
                                              G_CALLBACK(on_path_clear), st));
        adw_action_row_add_suffix(ADW_ACTION_ROW(row),
                                  icon_button(p->type == A800_PREF_DIR ? "folder-open-symbolic"
                                                                       : "document-open-symbolic",
                                              "Choose\xe2\x80\xa6", i,
                                              G_CALLBACK(on_path_choose), st));
        break;
    }
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), p->label);
    if (p->help && p->type != A800_PREF_FILE && p->type != A800_PREF_DIR)
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), p->help);
    g_object_set_data(G_OBJECT(row), "a800-pref", GINT_TO_POINTER(i));
    return row;
}

/* A choice row's labels joined, to see when they change. */
static char *choice_labels(PrefsState *st, int i)
{
    GString *g = g_string_new(NULL);
    const char *l;
    for (int c = 0; (l = a800session_pref_choice(st->session, i, c)) != NULL; c++) {
        g_string_append(g, l);
        if (!a800session_pref_choice_enabled(st->session, i, c))
            g_string_append(g, " (not available)");
        g_string_append_c(g, '\n');
    }
    return g_string_free(g, FALSE);
}

/* Every row from the store: values, labels that depend on others, and
 * which rows apply to this machine. */
static void refresh_rows(PrefsState *st)
{
    st->syncing = TRUE;
    for (int i = 0; i < a800session_pref_count() && i < MAX_ROWS; i++) {
        const a800_pref *p = a800session_pref(i);
        GtkWidget *row = st->rows[i];
        if (!row)
            continue;
        gtk_widget_set_sensitive(row, a800session_pref_enabled(st->session, i));
        switch (p->type) {
        case A800_PREF_BOOL:
            adw_switch_row_set_active(ADW_SWITCH_ROW(row),
                                      a800session_get_int(st->session, p->key, p->def) != 0);
            break;
        case A800_PREF_INT:
            adw_spin_row_set_value(ADW_SPIN_ROW(row),
                                   a800session_get_int(st->session, p->key, p->def));
            break;
        case A800_PREF_CHOICE: {
            char *labels = choice_labels(st, i);
            int sel = a800session_pref_get_choice(st->session, i);
            if (g_strcmp0(labels, st->labels[i]) != 0) {
                char **parts = g_strsplit(labels, "\n", -1);
                guint n = g_strv_length(parts);
                if (n && !*parts[n - 1]) {
                    g_free(parts[n - 1]);
                    parts[n - 1] = NULL;
                }
                GtkStringList *model = gtk_string_list_new((const char *const *)parts);
                adw_combo_row_set_model(ADW_COMBO_ROW(row), G_LIST_MODEL(model));
                g_object_unref(model);
                g_strfreev(parts);
                g_free(st->labels[i]);
                st->labels[i] = labels;
            } else {
                g_free(labels);
            }
            adw_combo_row_set_selected(ADW_COMBO_ROW(row), sel >= 0 ? (guint)sel : 0);
            break;
        }
        default: {
            const char *v = a800session_get_str(st->session, p->key, "");
            g_autofree char *base = v && *v ? g_path_get_basename(v) : NULL;
            adw_action_row_set_subtitle(ADW_ACTION_ROW(row),
                                        base ? base : (p->help ? p->help : "None"));
            gtk_widget_set_tooltip_text(row, v && *v ? v : NULL);
            break;
        }
        }
    }
    for (int r = 0; r < a800session_rom_slot_count() && r < MAX_ROM_ROWS; r++)
        if (st->rom_rows[r])
            adw_action_row_set_subtitle(ADW_ACTION_ROW(st->rom_rows[r]),
                                        a800session_rom_available(st->session, r)
                                            ? "Imported" : "Not imported");
    st->syncing = FALSE;
}

/* ---- the ROMs page -------------------------------------------------------------- */

typedef struct {
    PrefsState *st;
    int slot;
} RomCtx;

static void on_rom_chosen(GObject *src, GAsyncResult *res, gpointer ud)
{
    RomCtx *c = ud;
    g_autoptr(GFile) f = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
    g_autofree char *path = f ? g_file_get_path(f) : NULL;
    char msg[512];
    if (path) {
        if (a800session_rom_import(c->st->session, path, c->slot, msg, sizeof msg) > 0) {
            char t[600];
            g_snprintf(t, sizeof t, "Imported: %s", msg);
            a800_window_toast(c->st->window, t);
            /* revisions that need it become choosable */
            c->st->pending = MAX(c->st->pending, A800_APPLY_POWER);
        } else {
            a800_window_toast(c->st->window, msg);
        }
        refresh_rows(c->st);
    }
    g_free(c);
}

static void on_rom_import(GtkButton *b, gpointer ud)
{
    PrefsState *st = ud;
    int slot = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "a800-rom"));
    GtkFileDialog *dlg = gtk_file_dialog_new();
    RomCtx *c = g_new0(RomCtx, 1);
    char title[160];
    c->st = st;
    c->slot = slot;
    g_snprintf(title, sizeof title, "Import %s", a800session_rom_slot(slot)->name);
    gtk_file_dialog_set_title(dlg, title);
    gtk_file_dialog_open(dlg, GTK_WINDOW(st->window), NULL, on_rom_chosen, c);
    g_object_unref(dlg);
}

static AdwPreferencesPage *build_roms_page(PrefsState *st)
{
    AdwPreferencesPage *page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    AdwPreferencesGroup *group = NULL;
    const char *last = NULL;
    char desc[1200];

    adw_preferences_page_set_title(page, "ROMs");
    adw_preferences_page_set_icon_name(page, "media-flash-symbolic");
    for (int r = 0; r < a800session_rom_slot_count() && r < MAX_ROM_ROWS; r++) {
        const a800_rom_slot *slot = a800session_rom_slot(r);
        GtkWidget *row, *b;
        char tip[64];
        if (!last || strcmp(last, slot->group) != 0) {
            group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
            adw_preferences_group_set_title(group, slot->group);
            if (!last) {
                g_snprintf(desc, sizeof desc,
                    "The built-in AltirraOS and Altirra BASIC run everything without these. "
                    "Machine \xe2\x96\xb8 Import System ROMs recognises Atari's own ROMs and "
                    "MAME's sets; a peripheral's ROM goes in its slot here. Kept in %s.",
                    a800session_roms_path(st->session));
                adw_preferences_group_set_description(group, desc);
            }
            adw_preferences_page_add(page, group);
            last = slot->group;
        }
        row = adw_action_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), slot->name);
        b = gtk_button_new_with_label("Import\xe2\x80\xa6");
        gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
        g_snprintf(tip, sizeof tip, "%u bytes", slot->size);
        gtk_widget_set_tooltip_text(b, tip);
        g_object_set_data(G_OBJECT(b), "a800-rom", GINT_TO_POINTER(r));
        g_signal_connect(b, "clicked", G_CALLBACK(on_rom_import), st);
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), b);
        adw_preferences_group_add(group, row);
        st->rom_rows[r] = row;
    }
    return page;
}

/* ---- the dialog --------------------------------------------------------------------- */

static void on_closed(AdwDialog *dlg, gpointer ud)
{
    PrefsState *st = ud;
    int did;
    (void)dlg;
    a800session_settings_flush(st->session);
    did = st->pending > A800_APPLY_LIVE ? a800session_apply_settings(st->session) : 0;
    a800_window_settings_changed(st->window);
    if (did == A800_APPLY_POWER)
        a800_window_toast(st->window, "Power cycled with the new configuration");
    else if (did == A800_APPLY_RESTART)
        a800_window_toast(st->window, "Restarted with the new options");
    for (int i = 0; i < MAX_ROWS; i++)
        g_free(st->labels[i]);
    g_free(st);
}

void a800_prefs_show(A800Window *parent, a800session *session)
{
    PrefsState *st = g_new0(PrefsState, 1);
    AdwPreferencesDialog *dlg = ADW_PREFERENCES_DIALOG(adw_preferences_dialog_new());
    AdwPreferencesPage *page = NULL;
    AdwPreferencesGroup *group = NULL;
    const char *last_page = NULL, *last_group = NULL;

    st->window = parent;
    st->session = session;
    st->dialog = ADW_DIALOG(dlg);
    adw_dialog_set_title(ADW_DIALOG(dlg), "Preferences");
    adw_preferences_dialog_set_search_enabled(dlg, TRUE);

    for (int i = 0; i < a800session_pref_count() && i < MAX_ROWS; i++) {
        const a800_pref *p = a800session_pref(i);
        if (!last_page || strcmp(last_page, p->page) != 0) {
            page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
            adw_preferences_page_set_title(page, p->page);
            adw_preferences_page_set_icon_name(page, page_icon(p->page));
            adw_preferences_dialog_add(dlg, page);
            last_page = p->page;
            last_group = NULL;
            if (!strcmp(p->page, "Peripherals"))
                adw_preferences_dialog_add(dlg, build_roms_page(st));
        }
        if (!last_group || strcmp(last_group, p->group) != 0) {
            group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
            adw_preferences_group_set_title(group, p->group);
            if (!strcmp(p->group, "Computer"))
                adw_preferences_group_set_description(group,
                    "Machine settings apply together when Preferences closes: the Atari "
                    "power-cycles once.");
            adw_preferences_page_add(page, group);
            last_group = p->group;
        }
        st->rows[i] = make_row(st, i);
        adw_preferences_group_add(group, st->rows[i]);
    }
    refresh_rows(st);

    g_signal_connect(dlg, "closed", G_CALLBACK(on_closed), st);
    adw_dialog_present(ADW_DIALOG(dlg), GTK_WIDGET(parent));
}
