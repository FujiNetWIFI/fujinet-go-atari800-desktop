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
#define ID_ROM_LIST 3001
#define ID_ROM_IMPORT 3002
#define ID_ROW 3100             /* + pref * 4 + part */

typedef struct {
    HWND label, ctrl, name;     /* name: a file row's file name */
    char labels[2048];          /* a choice row's labels, as shown */
    int page;
} Row;

static HWND g_win, g_tabs, g_pages[MAX_PAGES], g_rom_list;
static int g_page_count;
static Row g_rows[MAX_ROWS];
static a800session *g_s;
static int g_syncing;
static int g_pending;
static void (*g_on_live)(void);
static void (*g_on_closed)(int);
static HFONT g_font, g_bold;

static void refresh(void);

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
}

/* The pages are children of the window (not of the tab control), so their
 * controls' notifications come here. */
static LRESULT CALLBACK page_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_COMMAND || msg == WM_NOTIFY || msg == WM_CTLCOLORSTATIC || msg == WM_CTLCOLORBTN)
        return SendMessageA(g_win, msg, wp, lp);
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK prefs_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_NOTIFY: {
        const NMHDR *n = (const NMHDR *)lp;
        if (n->idFrom == ID_TABS && n->code == TCN_SELCHANGE)
            show_page((int)SendMessageA(g_tabs, TCM_GETCURSEL, 0, 0));
        return 0;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        /* the pages are plain windows on the dialog face */
        SetBkColor((HDC)wp, GetSysColor(COLOR_BTNFACE));
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
    case WM_COMMAND: {
        const int id = LOWORD(wp), code = HIWORD(wp);
        int i, part;
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
        g_win = NULL;
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
    const char *last_page = NULL, *last_group = NULL;
    TCITEMA tab;

    if (g_win) { SetForegroundWindow(g_win); return; }
    g_s = session;
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
    }
    for (i = 0; i <= page; i++)
        if (heights[i] > page_h) page_h = heights[i];
    page_h += 12;
    if (page_h < 420) page_h = 420;

    SetRect(&want, 0, 0, PAGE_W + 24, page_h + 48);
    AdjustWindowRectEx(&want, style, FALSE, 0);
    GetWindowRect(parent, &pr);
    g_win = CreateWindowA("A800PrefsWindow", "Settings", style, pr.left + 40, pr.top + 40,
                          want.right - want.left, want.bottom - want.top, NULL, NULL,
                          GetModuleHandleA(NULL), NULL);
    g_tabs = child(g_win, 0, WC_TABCONTROLA, "", WS_CLIPSIBLINGS, 8, 8, PAGE_W + 8, page_h + 32, ID_TABS, g_font);
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
        default:
            r->name = child(pg, 0, "STATIC", "", SS_PATHELLIPSIS, CTRL_X, y + 4, CTRL_W - 160, 20, 0, g_font);
            r->ctrl = child(pg, 0, "BUTTON", "Choose...", BS_PUSHBUTTON, CTRL_X + CTRL_W - 156, y,
                            80, 24, id + 1, g_font);
            child(pg, 0, "BUTTON", "Clear", BS_PUSHBUTTON, CTRL_X + CTRL_W - 72, y, 72, 24, id + 2, g_font);
            break;
        }
        y += ROW_H;
    }
    refresh();
    show_page(0);
    ShowWindow(g_win, SW_SHOW);
}
