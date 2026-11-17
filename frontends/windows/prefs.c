/*
 * prefs.c -- the Win32 Settings window, built from a800session_pref like
 * the other frontends' Preferences: a tab per page, a heading per group, a
 * row per preference, and a ROMs tab with every slot.
 *
 * Rows that apply LIVE (the picture, the sound, the ports) take effect at
 * once; the machine -- model, memory, ROMs, peripherals -- waits and is
 * applied together when the window closes (one power cycle), as are the
 * host options (FujiNet, sound, gamepads: a session restart). Rows follow
 * each other as they change: memory sizes by model, ROM revisions by what
 * has been imported, peripherals by what the machine can take.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "prefs.h"
#include "controllers/controller_window.h"
#include "help/help_window.h"

#include <commctrl.h>
#include <shlobj.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_ROWS 128
#define MAX_PAGES 8
#define ROW_H 28
#define HEAD_H 26
#define LABEL_W 220
#define CTRL_X 236
#define CTRL_W 300
#define PAGE_W 560
#define ID_TABS 3000
#define ID_PREFS_HELP 3001
#define ID_ROM_LIST 3001
#define ID_ROM_IMPORT 3002
#define ID_ROW 3100             /* + pref * 4 + part */
#define ID_PAD_COMBO 4000       /* + row: a gamepad's port */
#define ID_RESET_CONTROLS 4010
#define ID_OPEN_CONTROLLERS 4011
#define ID_PADS_TIMER 1
#define PAD_ROWS 4              /* gamepads the Input page has room for */
#define PAD_EXTRA_H (HEAD_H + 4 + ROW_H + PAD_ROWS * ROW_H + 8 + 30) /* the Gamepads group */
#define HELP_H 40               /* a text row's note under it */
#ifndef EM_SETCUEBANNER
#define EM_SETCUEBANNER 0x1501
#endif

typedef struct {
    HWND label, ctrl, name;     /* name: a file row's file name */
    char labels[2048];          /* a choice row's labels, as shown; a text row's "filled in" mark */
    int page;
    int bad;                    /* a text row whose text was refused */
} Row;

static HWND g_win, g_tabs, g_pages[MAX_PAGES], g_rom_list, g_parent;
static HWND g_pad_name[PAD_ROWS], g_pad_combo[PAD_ROWS], g_pad_note;
static int g_page_count;
static Row g_rows[MAX_ROWS];
static a800session *g_s;
static int g_syncing;
static int g_pending;
static void (*g_on_live)(void);
static void (*g_on_closed)(int);
static HFONT g_font, g_bold;

static void refresh(void);
static HWND child(HWND parent, DWORD ex, const char *cls, const char *text, DWORD style,
                  int x, int y, int w, int h, int id, HFONT font);

static void changed(int i)
{
    const a800_pref *p = a800session_pref(i);
    if (p->applies > g_pending)
        g_pending = p->applies;
    if (g_pending == A800_APPLY_LIVE) {
        a800session_apply_settings(g_s);
        if (g_on_live) g_on_live();
    }
    refresh();
}

static void choice_labels(int i, char *dst, size_t dstsz)
{
    const char *l;
    int c;
    dst[0] = '\0';
    for (c = 0; (l = a800session_pref_choice(g_s, i, c)) != NULL; c++) {
        strncat(dst, l, dstsz - strlen(dst) - 1);
        if (!a800session_pref_choice_enabled(g_s, i, c))
            strncat(dst, " (not available)", dstsz - strlen(dst) - 1);
        strncat(dst, "\n", dstsz - strlen(dst) - 1);
    }
}

/* UTF-8 into a combo box (the labels have x and dashes in them). */
static void combo_add_utf8(HWND combo, const char *text)
{
    wchar_t w[256];
    MultiByteToWideChar(CP_UTF8, 0, text, -1, w, 256);
    SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)w);
}

static void set_text_utf8(HWND h, const char *text)
{
    wchar_t w[1024];
    MultiByteToWideChar(CP_UTF8, 0, text, -1, w, 1024);
    SetWindowTextW(h, w);
}

static void refresh(void)
{
    int i, s;
    g_syncing = 1;
    for (i = 0; i < a800session_pref_count() && i < MAX_ROWS; i++) {
        const a800_pref *p = a800session_pref(i);
        Row *r = &g_rows[i];
        const int on = a800session_pref_enabled(g_s, i);
        if (!r->ctrl) continue;
        EnableWindow(r->ctrl, on);
        EnableWindow(r->label, on);
        switch (p->type) {
        case A800_PREF_BOOL:
            SendMessageA(r->ctrl, BM_SETCHECK,
                         a800session_get_int(g_s, p->key, p->def) ? BST_CHECKED : BST_UNCHECKED, 0);
            break;
        case A800_PREF_INT: {
            char v[16];
            snprintf(v, sizeof v, "%d", a800session_get_int(g_s, p->key, p->def));
            SetWindowTextA(r->ctrl, v);
            break;
        }
        case A800_PREF_CHOICE: {
            char labels[2048];
            int sel = a800session_pref_get_choice(g_s, i);
            choice_labels(i, labels, sizeof labels);
            if (strcmp(labels, r->labels) != 0) {
                char *line = labels, *nl;
                SendMessageA(r->ctrl, CB_RESETCONTENT, 0, 0);
                snprintf(r->labels, sizeof r->labels, "%s", labels);
                while ((nl = strchr(line, '\n')) != NULL) {
                    *nl = '\0';
                    combo_add_utf8(r->ctrl, line);
                    line = nl + 1;
                }
            }
            SendMessageA(r->ctrl, CB_SETCURSEL, (WPARAM)(sel >= 0 ? sel : 0), 0);
            break;
        }
        case A800_PREF_TEXT:
            /* the user's own text: filled in once, never rewritten mid-edit */
            if (!r->labels[0]) {
                set_text_utf8(r->ctrl, a800session_pref_get_text(g_s, i));
                r->labels[0] = 1;
            }
            break;
        default: {
            const char *v = a800session_get_str(g_s, p->key, "");
            const char *b = v && *v ? v + strlen(v) : NULL;
            if (b) while (b > v && b[-1] != '\\' && b[-1] != '/') b--;
            set_text_utf8(r->name, b ? b : "None");
            EnableWindow(r->name, on);
            break;
        }
        }
    }
    for (s = 0; s < a800session_rom_slot_count(); s++) {
        LVITEMA it;
        char state[32];
        snprintf(state, sizeof state, "%s", a800session_rom_available(g_s, s) ? "Imported" : "Not imported");
        memset(&it, 0, sizeof it);
        it.iSubItem = 2;
        it.pszText = state;
        SendMessageA(g_rom_list, LVM_SETITEMTEXTA, (WPARAM)s, (LPARAM)&it);
    }
    g_syncing = 0;
}

/* ---- picking a file or a folder ------------------------------------------------ */

static void choose_path(int i)
{
    const a800_pref *p = a800session_pref(i);
    char path[MAX_PATH] = "";
    if (p->type == A800_PREF_DIR) {
        BROWSEINFOA bi;
        PIDLIST_ABSOLUTE pidl;
        memset(&bi, 0, sizeof bi);
        bi.hwndOwner = g_win;
        bi.lpszTitle = p->label;
        bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        pidl = SHBrowseForFolderA(&bi);
        if (!pidl) return;
        if (!SHGetPathFromIDListA(pidl, path)) path[0] = '\0';
        CoTaskMemFree(pidl);
    } else {
        OPENFILENAMEA ofn;
        memset(&ofn, 0, sizeof ofn);
        ofn.lStructSize = sizeof ofn;
        ofn.hwndOwner = g_win;
        ofn.lpstrFilter = "All files\0*.*\0\0";
        ofn.lpstrFile = path;
        ofn.nMaxFile = sizeof path;
        ofn.lpstrTitle = p->label;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        if (!GetOpenFileNameA(&ofn)) return;
    }
    if (!path[0]) return;
    a800session_set_str(g_s, p->key, path);
    changed(i);
}

static void import_rom(void)
{
    const int slot = (int)SendMessageA(g_rom_list, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
    char path[MAX_PATH] = "", msg[512], title[160];
    OPENFILENAMEA ofn;
    if (slot < 0) {
        MessageBoxA(g_win, "Select a ROM slot first.", "Import ROM", MB_ICONINFORMATION | MB_OK);
        return;
    }
    snprintf(title, sizeof title, "Import %s", a800session_rom_slot(slot)->name);
    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = g_win;
    ofn.lpstrFilter = "ROM images (*.rom;*.bin;*.zip)\0*.rom;*.bin;*.zip\0All files\0*.*\0\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = sizeof path;
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameA(&ofn)) return;
    if (a800session_rom_import(g_s, path, slot, msg, sizeof msg) > 0) {
        /* revisions that need it become choosable */
        if (g_pending < A800_APPLY_POWER) g_pending = A800_APPLY_POWER;
    } else {
        MessageBoxA(g_win, msg, "Nothing Imported", MB_ICONWARNING | MB_OK);
    }
    refresh();
}

/* ---- the window ------------------------------------------------------------------ */

static void show_page(int n)
{
    int i;
    for (i = 0; i < g_page_count; i++)
        ShowWindow(g_pages[i], i == n ? SW_SHOW : SW_HIDE);
    /* Help, F1 and Ctrl+Shift+H open the page's own: the tab's text names it */
    if (g_tabs && g_win) {
        char name[16] = "", ctx[32];
        TCITEMA item;
        memset(&item, 0, sizeof item);
        item.mask = TCIF_TEXT;
        item.pszText = name;
        item.cchTextMax = (int)sizeof name;
        if (SendMessageA(g_tabs, TCM_GETITEMA, (WPARAM)n, (LPARAM)&item)) {
            snprintf(ctx, sizeof ctx, "prefs/%s", name);
            a800_help_set_context(g_win, ctx, 1);
        }
    }
}

/* The pages are children of the window (not of the tab control), so their
 * controls' notifications come here. */
static LRESULT CALLBACK page_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_COMMAND || msg == WM_NOTIFY || msg == WM_CTLCOLORSTATIC || msg == WM_CTLCOLORBTN ||
        msg == WM_CTLCOLOREDIT)
        return SendMessageA(g_win, msg, wp, lp);
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* The Gamepads group on the Input page: a row per connected pad with the port it
 * plays (Automatic, or one of the four). Rebuilt on a timer, so a pad plugged in
 * with Settings open shows up. */
static void refresh_pads_ui(void)
{
    int r;
    const int count = g_s ? a800session_gamepad_count(g_s) : 0;
    if (!g_pad_note) return;
    g_syncing = 1;
    ShowWindow(g_pad_note, count == 0 ? SW_SHOW : SW_HIDE);
    for (r = 0; r < PAD_ROWS; r++) {
        const int show = r < count;
        ShowWindow(g_pad_name[r], show ? SW_SHOW : SW_HIDE);
        ShowWindow(g_pad_combo[r], show ? SW_SHOW : SW_HIDE);
        if (show) {
            char name[128], line[200];
            const int assign = a800session_gamepad_assignment(g_s, r);
            const int effective = a800session_gamepad_effective_port(g_s, r);
            a800session_gamepad_name(g_s, r, name, sizeof name);
            if (effective >= 0) snprintf(line, sizeof line, "%s (port %d)", name, effective + 1);
            else snprintf(line, sizeof line, "%s (plays nothing)", name);
            set_text_utf8(g_pad_name[r], line);
            SendMessageA(g_pad_combo[r], CB_SETCURSEL, (WPARAM)(assign < 0 || assign > 3 ? 0 : assign + 1), 0);
        }
    }
    g_syncing = 0;
}

static void build_gamepads(HWND pg, int y)
{
    static const char *const ports[5] = { "Automatic", "Port 1", "Port 2", "Port 3", "Port 4" };
    int r, k;
    child(pg, 0, "STATIC", "Gamepads", 0, 12, y + 4, PAGE_W - 24, 20, 0, g_bold);
    y += HEAD_H + 4;
    g_pad_note = child(pg, 0, "STATIC", "No gamepads connected - plug one in. Which port each USB pad plays is "
                       "remembered.", 0, 24, y + 4, PAGE_W - 48, 20, 0, g_font);
    y += ROW_H;
    for (r = 0; r < PAD_ROWS; r++) {
        g_pad_name[r] = child(pg, 0, "STATIC", "", SS_ENDELLIPSIS, 24, y + 4, CTRL_X - 36 + 100, 20, 0, g_font);
        g_pad_combo[r] = child(pg, 0, "COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL, CTRL_X + 100, y,
                               CTRL_W - 100, 200, ID_PAD_COMBO + r, g_font);
        for (k = 0; k < 5; k++) SendMessageA(g_pad_combo[r], CB_ADDSTRING, 0, (LPARAM)ports[k]);
        ShowWindow(g_pad_name[r], SW_HIDE);
        ShowWindow(g_pad_combo[r], SW_HIDE);
        y += ROW_H;
    }
    y += 8;
    child(pg, 0, "BUTTON", "Reset Controls", BS_PUSHBUTTON, 24, y, 130, 26, ID_RESET_CONTROLS, g_font);
    child(pg, 0, "BUTTON", "Open Controllers", BS_PUSHBUTTON, 164, y, 140, 26, ID_OPEN_CONTROLLERS, g_font);
}

static LRESULT CALLBACK prefs_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_TIMER:
        if (wp == ID_PADS_TIMER) refresh_pads_ui();
        return 0;
    case WM_NOTIFY: {
        const NMHDR *n = (const NMHDR *)lp;
        if (n->idFrom == ID_TABS && n->code == TCN_SELCHANGE)
            show_page((int)SendMessageA(g_tabs, TCM_GETCURSEL, 0, 0));
        return 0;
    }
    case WM_CTLCOLOREDIT: {
        int i;
        for (i = 0; i < a800session_pref_count() && i < MAX_ROWS; i++)
            if (g_rows[i].bad && g_rows[i].ctrl == (HWND)lp) {
                SetTextColor((HDC)wp, RGB(192, 28, 40));
                break;
            }
        break;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        /* the pages are plain windows on the dialog face */
        SetBkColor((HDC)wp, GetSysColor(COLOR_BTNFACE));
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
    case WM_COMMAND: {
        const int id = LOWORD(wp), code = HIWORD(wp);
        int i, part;
        if (id == ID_PREFS_HELP) { a800_help_for(g_win); return 0; }
        if (id >= ID_PAD_COMBO && id < ID_PAD_COMBO + PAD_ROWS) {
            if (code == CBN_SELCHANGE && !g_syncing) {
                const int sel = (int)SendMessageA(g_pad_combo[id - ID_PAD_COMBO], CB_GETCURSEL, 0, 0);
                if (sel >= 0) a800session_gamepad_assign(g_s, id - ID_PAD_COMBO, sel - 1);   /* 0 = automatic */
                refresh_pads_ui();
            }
            return 0;
        }
        if (id == ID_RESET_CONTROLS) {
            if (MessageBoxA(hwnd, "Put every key and gamepad button back to its default, and every gamepad on "
                            "Automatic?", "Reset Controls", MB_ICONQUESTION | MB_YESNO) == IDYES) {
                int pad;
                a800session_bindings_reset(g_s);
                for (pad = 0; pad < a800session_gamepad_count(g_s); pad++) a800session_gamepad_assign(g_s, pad, -1);
                refresh_pads_ui();
            }
            return 0;
        }
        if (id == ID_OPEN_CONTROLLERS) { a800_controller_window_show(g_parent, g_s); return 0; }
        if (id == ID_ROM_IMPORT) { import_rom(); return 0; }
        if (id < ID_ROW || g_syncing) return 0;
        i = (id - ID_ROW) / 4;
        part = (id - ID_ROW) % 4;
        if (i >= a800session_pref_count()) return 0;
        {
            const a800_pref *p = a800session_pref(i);
            switch (p->type) {
            case A800_PREF_BOOL:
                if (code == BN_CLICKED) {
                    a800session_set_int(g_s, p->key,
                        SendMessageA(g_rows[i].ctrl, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    changed(i);
                }
                break;
            case A800_PREF_INT:
                if (code == EN_CHANGE) {
                    char v[16];
                    long n;
                    char *end;
                    GetWindowTextA(g_rows[i].ctrl, v, sizeof v);
                    n = strtol(v, &end, 10);
                    if (end != v && n >= p->min && n <= p->max &&
                        n != a800session_get_int(g_s, p->key, p->def)) {
                        a800session_set_int(g_s, p->key, (int)n);
                        changed(i);
                    }
                }
                break;
            case A800_PREF_CHOICE:
                if (code == CBN_SELCHANGE) {
                    const int c = (int)SendMessageA(g_rows[i].ctrl, CB_GETCURSEL, 0, 0);
                    if (c < 0) break;
                    if (!a800session_pref_choice_enabled(g_s, i, c)) {
                        /* not imported, or not on this machine: put it back */
                        MessageBoxA(hwnd, "Not available: import its ROM first (the ROMs tab), or "
                                    "choose a machine that takes it.", "Settings",
                                    MB_ICONINFORMATION | MB_OK);
                        refresh();
                        break;
                    }
                    a800session_pref_set_choice(g_s, i, c);
                    changed(i);
                }
                break;
            case A800_PREF_TEXT:
                if (code == EN_CHANGE) {
                    /* valid text is stored; anything else is shown in red and not stored,
                     * so the last good text stays in force */
                    char text[160], *start = text, *end;
                    GetWindowTextA(g_rows[i].ctrl, text, sizeof text);
                    while (*start == ' ' || *start == '\t') start++;
                    end = start + strlen(start);
                    while (end > start && (end[-1] == ' ' || end[-1] == '\t')) *--end = '\0';
                    g_rows[i].bad = a800session_pref_set_text(g_s, i, start, NULL, 0) != 0;
                    InvalidateRect(g_rows[i].ctrl, NULL, TRUE);
                    if (!g_rows[i].bad) changed(i);
                }
                break;
            default:
                if (code == BN_CLICKED) {
                    if (part == 1) choose_path(i);
                    else if (part == 2) {
                        a800session_set_str(g_s, p->key, "");
                        changed(i);
                    }
                }
                break;
            }
        }
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY: {
        int did;
        KillTimer(hwnd, ID_PADS_TIMER);
        g_win = NULL;
        g_pad_note = NULL;
        a800session_settings_flush(g_s);
        did = g_pending > A800_APPLY_LIVE ? a800session_apply_settings(g_s) : 0;
        if (g_on_closed) g_on_closed(did);
        return 0;
    }
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static HWND child(HWND parent, DWORD ex, const char *cls, const char *text, DWORD style,
                  int x, int y, int w, int h, int id, HFONT font)
{
    HWND c = CreateWindowExA(ex, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, parent,
                             (HMENU)(INT_PTR)id, GetModuleHandleA(NULL), NULL);
    SendMessageA(c, WM_SETFONT, (WPARAM)font, TRUE);
    return c;
}

static HWND make_page(HWND parent, const RECT *area)
{
    static int registered;
    if (!registered) {
        WNDCLASSA wc;
        memset(&wc, 0, sizeof wc);
        wc.lpfnWndProc = page_proc;
        wc.hInstance = GetModuleHandleA(NULL);
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = "A800PrefsPage";
        RegisterClassA(&wc);
        registered = 1;
    }
    return CreateWindowExA(WS_EX_CONTROLPARENT, "A800PrefsPage", "", WS_CHILD, area->left, area->top,
                           area->right - area->left, area->bottom - area->top, parent, NULL,
                           GetModuleHandleA(NULL), NULL);
}

static void build_roms_page(HWND page, int h)
{
    LVCOLUMNA col;
    int s;
    char note[1024];
    snprintf(note, sizeof note,
             "The built-in AltirraOS and Altirra BASIC run everything without these. Machine > "
             "Import System ROMs recognises Atari's own ROMs and MAME's sets; a peripheral's ROM "
             "goes in its slot here. Kept in %s.", a800session_roms_path(g_s));
    child(page, 0, "STATIC", note, 0, 12, 8, PAGE_W - 24, 52, 0, g_font);
    g_rom_list = child(page, WS_EX_CLIENTEDGE, WC_LISTVIEWA, "",
                       LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS, 12, 64, PAGE_W - 24, h - 112,
                       ID_ROM_LIST, g_font);
    SendMessageA(g_rom_list, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT);
    memset(&col, 0, sizeof col);
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.cx = 270; col.pszText = "ROM";
    SendMessageA(g_rom_list, LVM_INSERTCOLUMNA, 0, (LPARAM)&col);
    col.cx = 130; col.pszText = "Group";
    SendMessageA(g_rom_list, LVM_INSERTCOLUMNA, 1, (LPARAM)&col);
    col.cx = 110; col.pszText = "State";
    SendMessageA(g_rom_list, LVM_INSERTCOLUMNA, 2, (LPARAM)&col);
    for (s = 0; s < a800session_rom_slot_count(); s++) {
        const a800_rom_slot *slot = a800session_rom_slot(s);
        LVITEMA it;
        memset(&it, 0, sizeof it);
        it.mask = LVIF_TEXT;
        it.iItem = s;
        it.pszText = (char *)slot->name;
        SendMessageA(g_rom_list, LVM_INSERTITEMA, 0, (LPARAM)&it);
        it.iSubItem = 1;
        it.pszText = (char *)slot->group;
        SendMessageA(g_rom_list, LVM_SETITEMTEXTA, (WPARAM)s, (LPARAM)&it);
    }
    child(page, 0, "BUTTON", "Import...", BS_PUSHBUTTON, PAGE_W - 112, h - 40, 100, 28,
          ID_ROM_IMPORT, g_font);
}

void a800_prefs_show(HWND parent, a800session *session, void (*on_live)(void),
                     void (*on_closed)(int applied))
{
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    static int registered;
    RECT want, area, pr;
    int i, page = -1, y = 0, page_h = 0, heights[MAX_PAGES] = {0};
    int input_page = -1, cur_y = 0;
    HWND cur_pg = NULL;
    const char *last_page = NULL, *last_group = NULL;
    TCITEMA tab;

    if (g_win) { SetForegroundWindow(g_win); return; }
    g_s = session;
    g_parent = parent;
    g_on_live = on_live;
    g_on_closed = on_closed;
    g_pending = A800_APPLY_LIVE;
    g_page_count = 0;
    memset(g_rows, 0, sizeof g_rows);
    if (!g_font) {
        LOGFONTA lf;
        g_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        GetObjectA(g_font, sizeof lf, &lf);
        lf.lfWeight = FW_BOLD;
        g_bold = CreateFontIndirectA(&lf);
    }
    if (!registered) {
        WNDCLASSA wc;
        memset(&wc, 0, sizeof wc);
        wc.lpfnWndProc = prefs_proc;
        wc.hInstance = GetModuleHandleA(NULL);
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = "A800PrefsWindow";
        RegisterClassA(&wc);
        registered = 1;
    }

    /* the tallest page decides the window's height */
    for (i = 0; i < a800session_pref_count() && i < MAX_ROWS; i++) {
        const a800_pref *p = a800session_pref(i);
        if (!last_page || strcmp(last_page, p->page)) {
            page++;
            last_page = p->page;
            last_group = NULL;
            heights[page] = 12 + (!strcmp(p->page, "Machine") ? 40 : 0);
        }
        if (!last_group || strcmp(last_group, p->group)) {
            heights[page] += HEAD_H + 4;
            last_group = p->group;
        }
        heights[page] += ROW_H;
        if (p->type == A800_PREF_TEXT && p->help)
            heights[page] += HELP_H;
        if (!strcmp(p->page, "Input")) input_page = page;
    }
    if (input_page >= 0) heights[input_page] += PAD_EXTRA_H;
    for (i = 0; i <= page; i++)
        if (heights[i] > page_h) page_h = heights[i];
    page_h += 12;
    if (page_h < 420) page_h = 420;

    SetRect(&want, 0, 0, PAGE_W + 24, page_h + 48 + 36);
    AdjustWindowRectEx(&want, style, FALSE, 0);
    GetWindowRect(parent, &pr);
    g_win = CreateWindowA("A800PrefsWindow", "Settings", style, pr.left + 40, pr.top + 40,
                          want.right - want.left, want.bottom - want.top, NULL, NULL,
                          GetModuleHandleA(NULL), NULL);
    g_tabs = child(g_win, 0, WC_TABCONTROLA, "", WS_CLIPSIBLINGS, 8, 8, PAGE_W + 8, page_h + 32, ID_TABS, g_font);
    child(g_win, 0, "BUTTON", "Help", BS_PUSHBUTTON, 12, page_h + 48, 90, 26, ID_PREFS_HELP, g_font);
    SetRect(&area, 12, 36, 12 + PAGE_W, 36 + page_h);

    last_page = last_group = NULL;
    memset(&tab, 0, sizeof tab);
    tab.mask = TCIF_TEXT;
    for (i = 0; i < a800session_pref_count() && i < MAX_ROWS; i++) {
        const a800_pref *p = a800session_pref(i);
        HWND pg;
        Row *r = &g_rows[i];
        const int id = ID_ROW + i * 4;
        if (!last_page || strcmp(last_page, p->page)) {
            if (last_page && !strcmp(last_page, "Input") && cur_pg) build_gamepads(cur_pg, cur_y);
            if (!strcmp(p->page, "Peripherals") && g_page_count < MAX_PAGES - 1) {
                /* the ROMs tab follows Peripherals, as in the other frontends */
                tab.pszText = (char *)p->page;
                g_pages[g_page_count] = make_page(g_win, &area);
                SendMessageA(g_tabs, TCM_INSERTITEMA, (WPARAM)g_page_count, (LPARAM)&tab);
                g_page_count++;
                tab.pszText = "ROMs";
                g_pages[g_page_count] = make_page(g_win, &area);
                SendMessageA(g_tabs, TCM_INSERTITEMA, (WPARAM)g_page_count, (LPARAM)&tab);
                build_roms_page(g_pages[g_page_count], page_h);
                g_page_count++;
                pg = g_pages[g_page_count - 2];
            } else {
                tab.pszText = (char *)p->page;
                g_pages[g_page_count] = make_page(g_win, &area);
                SendMessageA(g_tabs, TCM_INSERTITEMA, (WPARAM)g_page_count, (LPARAM)&tab);
                pg = g_pages[g_page_count++];
            }
            last_page = p->page;
            last_group = NULL;
            y = 12;
            if (!strcmp(p->page, "Machine")) {
                child(pg, 0, "STATIC", "Machine settings apply together when Settings closes: "
                      "the Atari power-cycles once.", 0, 12, y, PAGE_W - 24, 34, 0, g_font);
                y += 40;
            }
        }
        pg = !strcmp(p->page, "Peripherals") ? g_pages[g_page_count - 2] : g_pages[g_page_count - 1];
        if (!last_group || strcmp(last_group, p->group)) {
            child(pg, 0, "STATIC", p->group, 0, 12, y + 4, PAGE_W - 24, 20, 0, g_bold);
            y += HEAD_H + 4;
            last_group = p->group;
        }
        r->label = child(pg, 0, "STATIC", p->label, SS_NOTIFY, 24, y + 4, LABEL_W - 12, 20, 0, g_font);
        switch (p->type) {
        case A800_PREF_BOOL:
            r->ctrl = child(pg, 0, "BUTTON", p->help ? p->help : "", BS_AUTOCHECKBOX,
                            CTRL_X, y + 2, PAGE_W - CTRL_X - 12, 22, id, g_font);
            break;
        case A800_PREF_INT: {
            HWND ud;
            r->ctrl = child(pg, WS_EX_CLIENTEDGE, "EDIT", "", ES_NUMBER | ES_AUTOHSCROLL,
                            CTRL_X, y + 1, 80, 24, id, g_font);
            ud = child(pg, 0, UPDOWN_CLASSA, "", UDS_SETBUDDYINT | UDS_ALIGNRIGHT | UDS_ARROWKEYS |
                       UDS_NOTHOUSANDS, 0, 0, 0, 0, 0, g_font);
            SendMessageA(ud, UDM_SETBUDDY, (WPARAM)r->ctrl, 0);
            SendMessageA(ud, UDM_SETRANGE32, (WPARAM)p->min, (LPARAM)p->max);
            /* ES_NUMBER refuses a minus sign: ranges below zero type freely */
            if (p->min < 0)
                SetWindowLongA(r->ctrl, GWL_STYLE, GetWindowLongA(r->ctrl, GWL_STYLE) & ~ES_NUMBER);
            if (p->help)
                child(pg, 0, "STATIC", p->help, 0, CTRL_X + 96, y + 4, PAGE_W - CTRL_X - 108, 20, 0, g_font);
            break;
        }
        case A800_PREF_CHOICE:
            r->ctrl = child(pg, 0, "COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL,
                            CTRL_X, y, CTRL_W, 300, id, g_font);
            SendMessageA(r->ctrl, CB_SETDROPPEDWIDTH, 360, 0);
            break;
        case A800_PREF_TEXT:
            r->ctrl = child(pg, WS_EX_CLIENTEDGE, "EDIT", "", ES_AUTOHSCROLL | WS_TABSTOP, CTRL_X, y + 1,
                            CTRL_W, 24, id, g_font);
            if (a800session_pref_placeholder(i)) {
                wchar_t hold[64];
                MultiByteToWideChar(CP_UTF8, 0, a800session_pref_placeholder(i), -1, hold, 64);
                SendMessageW(r->ctrl, EM_SETCUEBANNER, FALSE, (LPARAM)hold);
            }
            if (p->help) {
                child(pg, 0, "STATIC", p->help, 0, 24, y + ROW_H - 4, PAGE_W - 48, HELP_H - 6, 0, g_font);
                y += HELP_H;
            }
            break;
        default:
            r->name = child(pg, 0, "STATIC", "", SS_PATHELLIPSIS, CTRL_X, y + 4, CTRL_W - 160, 20, 0, g_font);
            r->ctrl = child(pg, 0, "BUTTON", "Choose...", BS_PUSHBUTTON, CTRL_X + CTRL_W - 156, y,
                            80, 24, id + 1, g_font);
            child(pg, 0, "BUTTON", "Clear", BS_PUSHBUTTON, CTRL_X + CTRL_W - 72, y, 72, 24, id + 2, g_font);
            break;
        }
        y += ROW_H;
        cur_pg = pg;
        cur_y = y;
    }
    if (last_page && !strcmp(last_page, "Input") && cur_pg) build_gamepads(cur_pg, cur_y);
    refresh();
    refresh_pads_ui();
    SetTimer(g_win, ID_PADS_TIMER, 300, NULL);
    show_page(0);
    ShowWindow(g_win, SW_SHOW);
}
