/*
 * FujiNet Go Atari 800 -- the Windows (Win32 + GDI) frontend.
 *
 * No toolkit: a plain window, a menu bar, a status bar and StretchDIBits.
 * That is enough for a 672x300 framebuffer at most, and it keeps the
 * artifact a folder you copy rather than a runtime hunt.
 *
 * Two Windows-specific things are load bearing:
 *
 *   DwmFlush() on a present thread is this platform's frame clock. There is
 *   no GdkFrameClock here, and a plain timer would beat against the panel.
 *
 *   WM_ACTIVATE releases every held key. Alt-tabbing away mid-game and
 *   coming back to a joystick still pushed is the classic symptom of not
 *   doing this, and Windows is where it happens most, because the WM eats
 *   the key-up.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "a800session.h"
#include "controllers/controller_window.h"
#include "debugger/dbg_window.h"
#include "key_forward.h"
#include "prefs.h"
#include "resource.h"

#define WIN_CLASS "FujiNetGoAtari800"
#define APP_TITLE "FujiNet Go Atari 800"

/* How long a status notice (a gamepad came or went, FujiNet restarted)
 * stays in the status bar. */
#define NOTICE_MS 4000

/* How long a menu click holds a console key: the OS looks once a frame. */
#define TAP_MS 120

/* The status bar's first part: the FujiNet link dot. */
#define DOT_PART_W 22

static a800session *g_session;
static HINSTANCE g_inst;
static HWND g_hwnd;
static HWND g_statusbar;
static uint32_t *g_fb;
static int g_fw, g_fh, g_par = 1000;
static uint64_t g_serial;
static volatile LONG g_frames;
static BITMAPINFO g_bmi;
static CRITICAL_SECTION g_fb_lock;
static volatile LONG g_running = 1;
static HANDLE g_present_thread;
static int g_aspect = 1, g_smooth = 0, g_fullscreen = 0;   /* TV aspect on */
static RECT g_shown;                /* where the picture is, for the pointer */
static int g_mouse_x, g_mouse_y, g_have_mouse;
static WINDOWPLACEMENT g_placement;
static unsigned g_pad_generation, g_fuji_generation;
static char g_notice[256];
static DWORD g_notice_until;
static int g_tap_key = -1;          /* the console key a menu click holds */
static HBRUSH g_accent_brush;
static char g_shot_path[MAX_PATH];
static LONG g_shot_frames = 300, g_quit_frames;

/* ---- the present thread: this platform's frame clock ---------------------- */

static DWORD WINAPI present_thread(LPVOID arg)
{
    (void)arg;
    while (InterlockedCompareExchange(&g_running, 1, 1)) {
        LARGE_INTEGER t, f;
        int w = 0, h = 0, par = 1000;
        /* Blocks until the compositor's next vblank; falls through at once
         * if the DWM is off, and the core's wall-clock pacing takes over --
         * which is the whole reason notify_vsync is advisory. */
        if (FAILED(DwmFlush())) Sleep(8);
        QueryPerformanceCounter(&t);
        QueryPerformanceFrequency(&f);
        a800session_notify_vsync(g_session, (int64_t)(t.QuadPart * 1000000000LL / f.QuadPart));
        EnterCriticalSection(&g_fb_lock);
        if (a800session_copy_frame(g_session, g_fb, &w, &h, &par, &g_serial) && w > 0 && h > 0) {
            g_fw = w;
            g_fh = h;
            g_par = par > 0 ? par : 1000;
            LeaveCriticalSection(&g_fb_lock);
            InterlockedIncrement(&g_frames);
            InvalidateRect(g_hwnd, NULL, FALSE);
        } else {
            LeaveCriticalSection(&g_fb_lock);
        }
    }
    return 0;
}

/* ---- painting -------------------------------------------------------------- */

static int statusbar_height(void)
{
    RECT r;
    if (!g_statusbar || !IsWindowVisible(g_statusbar)) return 0;
    GetWindowRect(g_statusbar, &r);
    return r.bottom - r.top;
}

/* The picture's width over its height, as shown: the frame's pixel aspect
 * (the NTSC filter and the 80-column cards make narrow pixels), then on a
 * TV an Atari pixel's shape (NTSC 6:7, PAL a little wide), and an
 * 80-column card's monitor at 4:3. */
static double picture_aspect(void)
{
    const double w = g_fw * g_par / 1000.0, h = g_fh;
    if (!g_aspect) return w / h;
    if (g_fw >= 560 && g_par == 500) return 4.0 / 3.0;
    return w * (a800session_refresh_rate(g_session) > 55000 ? 6.0 / 7.0 : 1.03) / h;
}

static void paint(HDC dc)
{
    RECT rc;
    double want, w, h, sw, sh;

    GetClientRect(g_hwnd, &rc);
    rc.bottom -= statusbar_height();
    w = rc.right - rc.left;
    h = rc.bottom - rc.top;
    FillRect(dc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
    if (w <= 0 || h <= 0) return;

    EnterCriticalSection(&g_fb_lock);
    if (g_fw <= 0 || g_fh <= 0) { LeaveCriticalSection(&g_fb_lock); return; }
    want = picture_aspect();
    if (w / h > want) { sh = h; sw = sh * want; }
    else              { sw = w; sh = sw / want; }
    SetRect(&g_shown, (int)((w - sw) / 2), (int)((h - sh) / 2),
            (int)((w - sw) / 2 + sw), (int)((h - sh) / 2 + sh));

    /* The session's pixels are 0x00RRGGBB, packed at the frame's width:
     * exactly a 32-bit BI_RGB DIB. */
    g_bmi.bmiHeader.biWidth = g_fw;
    g_bmi.bmiHeader.biHeight = -g_fh;   /* top-down */
    SetStretchBltMode(dc, g_smooth ? HALFTONE : COLORONCOLOR);
    StretchDIBits(dc, g_shown.left, g_shown.top, g_shown.right - g_shown.left,
                  g_shown.bottom - g_shown.top, 0, 0, g_fw, g_fh, g_fb, &g_bmi,
                  DIB_RGB_COLORS, SRCCOPY);
    LeaveCriticalSection(&g_fb_lock);
}

/* ---- A800_SCREENSHOT: the last frame as a PNG (zlib is the core's) --------- */

static void png_chunk(FILE *f, const char *type, const unsigned char *data, unsigned len)
{
    unsigned char hdr[8];
    uLong crc = crc32(0, (const Bytef *)type, 4);
    hdr[0] = (unsigned char)(len >> 24); hdr[1] = (unsigned char)(len >> 16);
    hdr[2] = (unsigned char)(len >> 8); hdr[3] = (unsigned char)len;
    memcpy(hdr + 4, type, 4);
    fwrite(hdr, 1, 8, f);
    if (len) {
        fwrite(data, 1, len, f);
        crc = crc32(crc, data, len);
    }
    hdr[0] = (unsigned char)(crc >> 24); hdr[1] = (unsigned char)(crc >> 16);
    hdr[2] = (unsigned char)(crc >> 8); hdr[3] = (unsigned char)crc;
    fwrite(hdr, 1, 4, f);
}

static int save_png(const char *path)
{
    static const unsigned char sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
    unsigned char ihdr[13];
    unsigned char *raw, *z;
    uLongf zlen;
    int w, h, x, y, ok = 0;
    FILE *f;

    EnterCriticalSection(&g_fb_lock);
    w = g_fw;
    h = g_fh;
    raw = malloc((size_t)(w * 3 + 1) * (size_t)h);
    if (raw) {
        for (y = 0; y < h; y++) {
            unsigned char *row = raw + (size_t)y * (size_t)(w * 3 + 1);
            *row++ = 0;   /* no filter */
            for (x = 0; x < w; x++) {
                const uint32_t p = g_fb[(size_t)y * (size_t)w + (size_t)x];
                *row++ = (unsigned char)(p >> 16);
                *row++ = (unsigned char)(p >> 8);
                *row++ = (unsigned char)p;
            }
        }
    }
    LeaveCriticalSection(&g_fb_lock);
    if (!raw || w <= 0 || h <= 0) { free(raw); return 0; }
    zlen = compressBound((uLong)((w * 3 + 1) * h));
    z = malloc(zlen);
    if (z && compress(z, &zlen, raw, (uLong)((w * 3 + 1) * h)) == Z_OK && (f = fopen(path, "wb"))) {
        ihdr[0] = (unsigned char)(w >> 24); ihdr[1] = (unsigned char)(w >> 16);
        ihdr[2] = (unsigned char)(w >> 8); ihdr[3] = (unsigned char)w;
        ihdr[4] = (unsigned char)(h >> 24); ihdr[5] = (unsigned char)(h >> 16);
        ihdr[6] = (unsigned char)(h >> 8); ihdr[7] = (unsigned char)h;
        ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = ihdr[11] = ihdr[12] = 0;   /* 8-bit RGB */
        fwrite(sig, 1, 8, f);
        png_chunk(f, "IHDR", ihdr, 13);
        png_chunk(f, "IDAT", z, (unsigned)zlen);
        png_chunk(f, "IEND", NULL, 0);
        ok = fclose(f) == 0;
    }
    free(z);
    free(raw);
    return ok;
}

/* ---- helpers ------------------------------------------------------------------ */

static void update_status(void);
static void load_media(const char *path);

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '\\');
    const char *t = strrchr(p, '/');
    if (t && (!s || t > s)) s = t;
    return s ? s + 1 : p;
}

static void set_text_utf8(HWND h, const char *text, int statusbar)
{
    wchar_t w[512];
    MultiByteToWideChar(CP_UTF8, 0, text, -1, w, 512);
    if (statusbar) SendMessageW(h, SB_SETTEXTW, 1, (LPARAM)w);   /* part 1: the text */
    else SetWindowTextW(h, w);
}

static void message_utf8(HWND owner, const char *text, const char *title, UINT flags)
{
    wchar_t wt[2048], wc[256];
    MultiByteToWideChar(CP_UTF8, 0, text, -1, wt, 2048);
    MultiByteToWideChar(CP_UTF8, 0, title, -1, wc, 256);
    MessageBoxW(owner, wt, wc, flags);
}

static void notice(const char *text)
{
    snprintf(g_notice, sizeof g_notice, "%.255s", text);
    g_notice_until = GetTickCount() + NOTICE_MS;
    update_status();
}

static void update_status(void)
{
    char line[512];
    if (!g_statusbar) return;
    /* Part 0 is the link dot, drawn in WM_DRAWITEM from this item data. */
    SendMessageA(g_statusbar, SB_SETTEXTA, SBT_OWNERDRAW | 0,
                 (LPARAM)(a800session_is_running(g_session) && a800session_fujinet_link(g_session)));
    if (g_notice[0] && GetTickCount() < g_notice_until) {
        snprintf(line, sizeof line, "%s", g_notice);
    } else {
        g_notice[0] = '\0';
        if (a800session_is_running(g_session))
            a800session_status(g_session, line, sizeof line);
        else
            snprintf(line, sizeof line, "Off");
    }
    set_text_utf8(g_statusbar, line, 1);
}

/* The link dot: the accent while FujiNet's NetSIO link is up, grey
 * otherwise. */
static void draw_status_dot(const DRAWITEMSTRUCT *di)
{
    const int up = di->itemData != 0;
    const int size = 10;
    RECT r = di->rcItem;
    const int x = r.left + ((r.right - r.left) - size) / 2;
    const int y = r.top + ((r.bottom - r.top) - size) / 2;
    HGDIOBJ oldb = SelectObject(di->hDC, up ? (HGDIOBJ)g_accent_brush : GetStockObject(GRAY_BRUSH));
    HGDIOBJ oldp = SelectObject(di->hDC, GetStockObject(NULL_PEN));
    Ellipse(di->hDC, x, y, x + size + 1, y + size + 1);
    SelectObject(di->hDC, oldp);
    SelectObject(di->hDC, oldb);
}

/* Where to look for media: the folder the last file came from. */
static const char *media_folder(void)
{
    const char *last = a800session_get_str(g_session, "last_dir", "");
    DWORD a = last && *last ? GetFileAttributesA(last) : INVALID_FILE_ATTRIBUTES;
    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY))
        return last;
    return a800session_media_path(g_session);
}

static void remember_folder(const char *path)
{
    char dir[MAX_PATH];
    char *s, *t;
    snprintf(dir, sizeof dir, "%s", path);
    s = strrchr(dir, '\\');
    t = strrchr(dir, '/');
    if (t && (!s || t > s)) s = t;
    if (s) {
        *s = '\0';
        a800session_set_str(g_session, "last_dir", dir);
    }
}

static int pick_file(HWND owner, const char *title, const char *filter, const char *folder,
                     char *path, DWORD pathsz, int save, const char *def_name)
{
    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof ofn);
    snprintf(path, pathsz, "%s", def_name ? def_name : "");
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = path;
    ofn.nMaxFile = pathsz;
    ofn.lpstrTitle = title;
    ofn.lpstrInitialDir = folder;
    if (save) {
        ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
        return GetSaveFileNameA(&ofn) ? 1 : 0;
    }
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    return GetOpenFileNameA(&ofn) ? 1 : 0;
}

static const char kAnyFilter[] =
    "Atari disks, programs, cartridges, tapes\0*.atr;*.xfd;*.atx;*.pro;*.dcm;*.xex;*.com;*.exe;"
    "*.bas;*.lst;*.car;*.rom;*.bin;*.cas;*.a8s;*.zip;*.gz;*.atz\0All files\0*.*\0\0";
static const char kDiskFilter[] =
    "Atari disk images (*.atr;*.xfd;*.atx;*.pro;*.dcm)\0*.atr;*.xfd;*.atx;*.pro;*.dcm;*.zip;*.gz;*.atz\0"
    "All files\0*.*\0\0";
static const char kCartFilter[] = "Cartridge images (*.car;*.rom;*.bin)\0*.car;*.rom;*.bin;*.zip\0"
                                  "All files\0*.*\0\0";
static const char kTapeFilter[] = "Cassette images (*.cas)\0*.cas;*.zip\0All files\0*.*\0\0";
static const char kStateFilter[] = "Save states (*.a8s)\0*.a8s\0All files\0*.*\0\0";
static const char kRomFilter[] = "ROM images and MAME sets (*.rom;*.bin;*.zip)\0*.rom;*.bin;*.zip\0"
                                 "All files\0*.*\0\0";

static void open_folder(const char *path)
{
    CreateDirectoryA(path, NULL);
    ShellExecuteA(g_hwnd, "open", path, NULL, NULL, SW_SHOWNORMAL);
}

/* ---- picking from a list: a cartridge's type, a ROM's slot ------------------- */

static int g_pick_result;

static LRESULT CALLBACK pick_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK || (LOWORD(wp) == 100 && HIWORD(wp) == LBN_DBLCLK)) {
            g_pick_result = (int)SendMessageA(GetDlgItem(hwnd, 100), LB_GETCURSEL, 0, 0);
            DestroyWindow(hwnd);
        } else if (LOWORD(wp) == IDCANCEL) {
            DestroyWindow(hwnd);
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* A modal list: the index chosen, or -1. */
static int pick_from_list(HWND owner, const char *title, const char *prompt,
                          const char *const *items, int n)
{
    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HWND dlg, h;
    RECT want, ro;
    MSG msg;
    int i;
    static int registered;

    if (!registered) {
        WNDCLASSA wc;
        memset(&wc, 0, sizeof wc);
        wc.lpfnWndProc = pick_proc;
        wc.hInstance = g_inst;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = "A800PickWindow";
        RegisterClassA(&wc);
        registered = 1;
    }
    SetRect(&want, 0, 0, 420, 300);
    AdjustWindowRectEx(&want, style, FALSE, WS_EX_DLGMODALFRAME);
    GetWindowRect(owner, &ro);
    dlg = CreateWindowExA(WS_EX_DLGMODALFRAME, "A800PickWindow", title, style,
                          ro.left + 60, ro.top + 60, want.right - want.left, want.bottom - want.top,
                          owner, NULL, g_inst, NULL);
    h = CreateWindowExA(0, "STATIC", prompt, WS_CHILD | WS_VISIBLE, 12, 10, 396, 48, dlg, NULL, g_inst, NULL);
    SendMessageA(h, WM_SETFONT, (WPARAM)font, TRUE);
    h = CreateWindowExA(WS_EX_CLIENTEDGE, "LISTBOX", "", WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY,
                        12, 62, 396, 190, dlg, (HMENU)(INT_PTR)100, g_inst, NULL);
    SendMessageA(h, WM_SETFONT, (WPARAM)font, TRUE);
    for (i = 0; i < n; i++)
        SendMessageA(h, LB_ADDSTRING, 0, (LPARAM)items[i]);
    SendMessageA(h, LB_SETCURSEL, 0, 0);
    h = CreateWindowExA(0, "BUTTON", "OK", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 232, 262, 84, 28,
                        dlg, (HMENU)(INT_PTR)IDOK, g_inst, NULL);
    SendMessageA(h, WM_SETFONT, (WPARAM)font, TRUE);
    h = CreateWindowExA(0, "BUTTON", "Cancel", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 324, 262, 84, 28,
                        dlg, (HMENU)(INT_PTR)IDCANCEL, g_inst, NULL);
    SendMessageA(h, WM_SETFONT, (WPARAM)font, TRUE);

    g_pick_result = -1;
    EnableWindow(owner, FALSE);
    ShowWindow(dlg, SW_SHOW);
    while (IsWindow(dlg) && GetMessage(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageA(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    return g_pick_result;
}

/* A raw image: the user says which board it is. */
static void ask_cart_type(const char *path, int piggyback)
{
    int types[64], n, i, pick;
    const char *names[64];
    char prompt[600], msg[512];
    const int kb = a800session_cart_needs_type(path);

    n = a800session_cart_types_for(kb, types, 64);
    if (n <= 0) {
        message_utf8(g_hwnd, "No Atari computer cartridge is that size.", "Cannot Use the Cartridge",
                     MB_ICONWARNING | MB_OK);
        return;
    }
    for (i = 0; i < n; i++)
        names[i] = a800session_cart_type_name(types[i]);
    snprintf(prompt, sizeof prompt, "%s is a raw %d KB image: the file does not say which "
             "cartridge board it came from. A .car file would.", base_name(path), kb);
    pick = pick_from_list(g_hwnd, "Which Cartridge Is It?", prompt, names, n);
    if (pick < 0) return;
    if ((piggyback ? a800session_cart_insert_piggyback(g_session, path, types[pick], msg, sizeof msg)
                   : a800session_cart_insert(g_session, path, types[pick], msg, sizeof msg)) == 0)
        notice(msg[0] ? msg : "Cartridge inserted");
    else
        message_utf8(g_hwnd, msg, "Cannot Use the Cartridge", MB_ICONWARNING | MB_OK);
}

/* ---- opening things ------------------------------------------------------------ */

/* A dropped file, one named on the command line, or Open: a disk boots, a
 * program runs, a cartridge goes in, a tape loads, a state loads, a ROM is
 * imported. */
static void load_media(const char *path)
{
    char msg[1024];
    int rc;
    if (!a800session_is_running(g_session)) {
        notice("The Atari is off");
        return;
    }
    rc = a800session_open(g_session, path, msg, sizeof msg);
    if (rc == 1) {
        ask_cart_type(path, 0);
        return;
    }
    if (rc != 0) {
        message_utf8(g_hwnd, msg[0] ? msg : a800session_last_error(g_session), "Cannot Open It",
                     MB_ICONWARNING | MB_OK);
        return;
    }
    remember_folder(path);
    notice(msg[0] ? msg : base_name(path));
}

static void import_roms(void)
{
    char path[MAX_PATH], msg[1024], log[2048] = "";
    int n, slots[16], i;
    const char *names[16];

    if (!pick_file(g_hwnd, "Import System ROMs", kRomFilter, NULL, path, sizeof path, 0, NULL))
        return;
    n = a800session_rom_import(g_session, path, -1, msg, sizeof msg);
    if (n > 0) {
        snprintf(log, sizeof log, "%s", msg);
        message_utf8(g_hwnd, log, "ROMs Imported", MB_ICONINFORMATION | MB_OK);
        return;
    }
    /* not known by its checksum: a peripheral's ROM fits slots by size */
    n = a800session_rom_probe(g_session, path, slots, 16);
    if (n <= 0) {
        message_utf8(g_hwnd, "Not a ROM this app recognises, and no ROM slot is its size.",
                     "Nothing Imported", MB_ICONWARNING | MB_OK);
        return;
    }
    for (i = 0; i < n; i++)
        names[i] = a800session_rom_slot(slots[i])->name;
    snprintf(log, sizeof log, "%s is not one of the ROMs this app knows by its checksum, but it "
             "is the size of these. Which is it?", base_name(path));
    i = pick_from_list(g_hwnd, "Which ROM Is It?", log, names, n);
    if (i < 0) return;
    if (a800session_rom_import(g_session, path, slots[i], msg, sizeof msg) > 0) {
        char t[600];
        snprintf(t, sizeof t, "Imported: %.580s", msg);
        notice(t);
    } else {
        message_utf8(g_hwnd, msg, "Nothing Imported", MB_ICONWARNING | MB_OK);
    }
}

static void insert_into(int drive, const char *path)
{
    char msg[512], t[700];
    if (a800session_drive_insert(g_session, drive, path, 0, msg, sizeof msg) != 0) {
        message_utf8(g_hwnd, msg, "Cannot Insert the Disk", MB_ICONWARNING | MB_OK);
        return;
    }
    remember_folder(path);
    snprintf(t, sizeof t, "%s is in D%d:", base_name(path), drive + 1);
    notice(msg[0] ? msg : t);
}

/* ---- the menus -------------------------------------------------------------------- */

static HMENU g_drives_menu, g_cart_menu, g_tape_menu, g_machine_menu, g_view_menu;

static void fill_drives_menu(void)
{
    const int fuji = a800session_fujinet_running(g_session);
    int d;
    while (GetMenuItemCount(g_drives_menu) > 0) DeleteMenu(g_drives_menu, 0, MF_BYPOSITION);
    for (d = 0; d < A800_DRIVES; d++) {
        a800_drive_info di;
        HMENU sub = CreatePopupMenu(), blank = CreatePopupMenu();
        char label[400];
        if (a800session_drive_info(g_session, d, &di) != 0) memset(&di, 0, sizeof di);
        snprintf(label, sizeof label, "D%d: %s%s", d + 1, di.path[0] ? base_name(di.path) : "empty",
                 di.path[0] && di.read_only ? " (protected)" : "");
        AppendMenuA(sub, MF_STRING, IDM_DRIVE_INSERT + d, fuji ? "&Mount from the SD Card..." : "&Insert...");
        AppendMenuA(blank, MF_STRING, IDM_DRIVE_BLANK + d * 4 + 0, "&Single density (90K)...");
        AppendMenuA(blank, MF_STRING, IDM_DRIVE_BLANK + d * 4 + 1, "&Enhanced density (130K)...");
        AppendMenuA(blank, MF_STRING, IDM_DRIVE_BLANK + d * 4 + 2, "&Double density (180K)...");
        AppendMenuA(sub, MF_POPUP, (UINT_PTR)blank, "&New Blank Disk");
        if (di.path[0]) {
            AppendMenuA(sub, MF_STRING, IDM_DRIVE_EJECT + d, "&Eject");
            if (!fuji)
                AppendMenuA(sub, MF_STRING | (di.read_only ? MF_CHECKED : 0), IDM_DRIVE_PROTECT + d,
                            "&Write Protect");
        }
        AppendMenuA(g_drives_menu, MF_POPUP, (UINT_PTR)sub, label);
    }
    AppendMenuA(g_drives_menu, MF_SEPARATOR, 0, NULL);
    if (fuji) {
        AppendMenuA(g_drives_menu, MF_STRING, IDM_IMPORT_SD, "&Import to SD Card...");
        AppendMenuA(g_drives_menu, MF_STRING, IDM_SD_FOLDER, "Open &SD Folder");
    } else {
        AppendMenuA(g_drives_menu, MF_STRING, IDM_MEDIA_FOLDER, "Open &Media Folder");
    }
}

static void fill_cart_menu(void)
{
    char info[256], label[300];
    while (GetMenuItemCount(g_cart_menu) > 0) DeleteMenu(g_cart_menu, 0, MF_BYPOSITION);
    if (a800session_cart_info(g_session, info, sizeof info) > 0) {
        snprintf(label, sizeof label, "In it: %s", info);
        AppendMenuA(g_cart_menu, MF_STRING | MF_GRAYED, IDM_CART_INFO, label);
    }
    AppendMenuA(g_cart_menu, MF_STRING, IDM_CART_INSERT, "&Insert Cartridge...");
    AppendMenuA(g_cart_menu, MF_STRING, IDM_CART_PIGGYBACK, "Insert &Piggyback...");
    AppendMenuA(g_cart_menu, MF_STRING, IDM_CART_REMOVE, "&Remove Cartridge");
    AppendMenuA(g_cart_menu, MF_STRING, IDM_CART_REMOVE_PB, "Remove Pi&ggyback");
}

static void fill_tape_menu(void)
{
    int block = 0, blocks = 0;
    char label[64];
    while (GetMenuItemCount(g_tape_menu) > 0) DeleteMenu(g_tape_menu, 0, MF_BYPOSITION);
    if (a800session_tape_position(g_session, &block, &blocks) == 0 && blocks > 0) {
        snprintf(label, sizeof label, "Block %d of %d", block, blocks);
        AppendMenuA(g_tape_menu, MF_STRING | MF_GRAYED, IDM_TAPE_INFO, label);
    }
    AppendMenuA(g_tape_menu, MF_STRING, IDM_TAPE_INSERT, "&Load Tape...");
    AppendMenuA(g_tape_menu, MF_STRING, IDM_TAPE_RECORD, "&Record New Tape...");
    AppendMenuA(g_tape_menu, MF_STRING, IDM_TAPE_REWIND, "Re&wind");
    AppendMenuA(g_tape_menu, MF_STRING, IDM_TAPE_BOOT, "&Boot Tape (Start held at power-on)");
    AppendMenuA(g_tape_menu, MF_STRING, IDM_TAPE_EJECT, "&Eject");
}

static void sync_menus(void)
{
    const int fuji = a800session_fujinet_running(g_session);
    const int states = a800session_states_available(g_session);
    EnableMenuItem(g_machine_menu, IDM_REBOOT_CONFIG, MF_BYCOMMAND | (fuji ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(g_machine_menu, IDM_STATE_SAVE, MF_BYCOMMAND | (states ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(g_machine_menu, IDM_STATE_LOAD, MF_BYCOMMAND | (states ? MF_ENABLED : MF_GRAYED));
    CheckMenuItem(g_machine_menu, IDM_WARP, MF_BYCOMMAND | (a800session_warp(g_session) ? MF_CHECKED : MF_UNCHECKED));
    EnableMenuItem(g_view_menu, IDM_COL80, MF_BYCOMMAND | (a800session_80col_fitted(g_session) ? MF_ENABLED : MF_GRAYED));
    CheckMenuItem(g_view_menu, IDM_COL80, MF_BYCOMMAND | (a800session_80col_display(g_session) ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(g_view_menu, IDM_TV_ASPECT, MF_BYCOMMAND | (g_aspect ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(g_view_menu, IDM_SMOOTH, MF_BYCOMMAND | (g_smooth ? MF_CHECKED : MF_UNCHECKED));
}

static void build_menu(HWND hwnd)
{
    HMENU bar = CreateMenu();
    HMENU console = CreatePopupMenu(), edit = CreatePopupMenu(), fuji = CreatePopupMenu();
    HMENU help = CreatePopupMenu();

    g_machine_menu = CreatePopupMenu();
    AppendMenuA(g_machine_menu, MF_STRING, IDM_OPEN, "&Open...\tCtrl+Shift+O");
    AppendMenuA(g_machine_menu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(g_machine_menu, MF_STRING, IDM_POWER, "&Power Cycle\tShift+F5");
    AppendMenuA(g_machine_menu, MF_STRING, IDM_RESET, "&Reset\tF5");
    AppendMenuA(g_machine_menu, MF_STRING, IDM_REBOOT_CONFIG, "Reboot to &CONFIG");
    AppendMenuA(g_machine_menu, MF_STRING, IDM_WARP, "&Warp Speed\tCtrl+Shift+W");
    AppendMenuA(g_machine_menu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(g_machine_menu, MF_STRING, IDM_STATE_SAVE, "&Save State...");
    AppendMenuA(g_machine_menu, MF_STRING, IDM_STATE_LOAD, "&Load State...");
    AppendMenuA(g_machine_menu, MF_STRING, IDM_IMPORT_ROMS, "Import System &ROMs...");
    AppendMenuA(g_machine_menu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(g_machine_menu, MF_STRING, IDM_SETTINGS, "Se&ttings...\tCtrl+,");
    AppendMenuA(g_machine_menu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(g_machine_menu, MF_STRING, IDM_EXIT, "E&xit\tCtrl+Shift+Q");

    g_drives_menu = CreatePopupMenu();
    g_cart_menu = CreatePopupMenu();
    g_tape_menu = CreatePopupMenu();
    fill_drives_menu();
    fill_cart_menu();
    fill_tape_menu();

    AppendMenuA(console, MF_STRING, IDM_CONSOLE + A800_CON_OPTION, "&Option\tF2");
    AppendMenuA(console, MF_STRING, IDM_CONSOLE + A800_CON_SELECT, "&Select\tF3");
    AppendMenuA(console, MF_STRING, IDM_CONSOLE + A800_CON_START, "S&tart\tF4");
    AppendMenuA(console, MF_STRING, IDM_CONSOLE + A800_CON_HELP, "&Help\tF6");
    AppendMenuA(console, MF_STRING, IDM_CONSOLE + A800_CON_BREAK, "&Break\tF7, Pause");

    AppendMenuA(edit, MF_STRING, IDM_COPY_SCREEN, "&Copy Screen Text\tCtrl+Shift+C");
    AppendMenuA(edit, MF_STRING, IDM_PASTE, "&Paste\tCtrl+Shift+V");

    g_view_menu = CreatePopupMenu();
    AppendMenuA(g_view_menu, MF_STRING, IDM_CONTROLLERS, "C&ontrollers\tF9");
    AppendMenuA(g_view_menu, MF_STRING, IDM_DEBUGGER, "&Debugger\tF12");
    AppendMenuA(g_view_menu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(g_view_menu, MF_STRING, IDM_COL80, "&80-Column Display");
    AppendMenuA(g_view_menu, MF_STRING, IDM_TV_ASPECT, "&TV Aspect");
    AppendMenuA(g_view_menu, MF_STRING, IDM_SMOOTH, "&Smooth Scaling");
    AppendMenuA(g_view_menu, MF_STRING, IDM_FULLSCREEN, "&Fullscreen\tF11");

    AppendMenuA(fuji, MF_STRING, IDM_FUJINET_CONFIG, "&Web UI");
    AppendMenuA(fuji, MF_STRING, IDM_FUJINET_LOG, "Console &Log");
    AppendMenuA(fuji, MF_STRING, IDM_FUJINET_RESTART, "&Restart FujiNet");

    AppendMenuA(help, MF_STRING, IDM_ABOUT, "&About " APP_TITLE);

    AppendMenuA(bar, MF_POPUP, (UINT_PTR)g_machine_menu, "&Machine");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)g_drives_menu, "&Drives");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)g_cart_menu, "&Cartridge");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)g_tape_menu, "&Program Recorder");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)console, "Co&nsole");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)edit, "&Edit");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)g_view_menu, "&View");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)fuji, "&FujiNet");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)help, "&Help");
    SetMenu(hwnd, bar);
    sync_menus();
}

/* ---- FujiNet console log -------------------------------------------------------- */

static HWND g_log_window;
static HWND g_log_edit;

static void log_refresh(void)
{
    static char buf[128 * 1024];
    int n, at_end;
    SCROLLINFO si;
    if (!g_log_edit) return;
    memset(&si, 0, sizeof si);
    si.cbSize = sizeof si;
    si.fMask = SIF_ALL;
    GetScrollInfo(g_log_edit, SB_VERT, &si);
    at_end = si.nPos + (int)si.nPage >= si.nMax;
    n = a800session_fujinet_copy_log(g_session, buf, sizeof buf);
    SetWindowTextA(g_log_edit, n > 0 ? buf : "(no FujiNet output yet)");
    /* follow the tail only while the reader is at the bottom */
    if (at_end) {
        int len = GetWindowTextLengthA(g_log_edit);
        SendMessageA(g_log_edit, EM_SETSEL, (WPARAM)len, (LPARAM)len);
        SendMessageA(g_log_edit, EM_SCROLLCARET, 0, 0);
    }
}

static LRESULT CALLBACK log_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZE: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        if (g_log_edit) MoveWindow(g_log_edit, 0, 0, rc.right - rc.left, rc.bottom - rc.top, TRUE);
        return 0;
    }
    case WM_TIMER:
        if (wp == IDT_LOG_REFRESH) log_refresh();
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, IDT_LOG_REFRESH);
        g_log_window = NULL;
        g_log_edit = NULL;
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static void show_fujinet_log(void)
{
    RECT rc;
    static int registered;
    if (g_log_window) { SetForegroundWindow(g_log_window); return; }
    if (!registered) {
        WNDCLASSA wc;
        memset(&wc, 0, sizeof wc);
        wc.lpfnWndProc = log_proc;
        wc.hInstance = g_inst;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = "A800FujiNetLogWindow";
        RegisterClassA(&wc);
        registered = 1;
    }
    g_log_window = CreateWindowA("A800FujiNetLogWindow", "FujiNet Console Log", WS_OVERLAPPEDWINDOW,
                                 CW_USEDEFAULT, CW_USEDEFAULT, 860, 600, NULL, NULL, g_inst, NULL);
    GetClientRect(g_log_window, &rc);
    g_log_edit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
        0, 0, rc.right - rc.left, rc.bottom - rc.top, g_log_window, (HMENU)(INT_PTR)IDC_LOG_EDIT, g_inst, NULL);
    SendMessageA(g_log_edit, WM_SETFONT, (WPARAM)GetStockObject(ANSI_FIXED_FONT), TRUE);
    SetTimer(g_log_window, IDT_LOG_REFRESH, 1000, NULL);
    log_refresh();
    ShowWindow(g_log_window, SW_SHOW);
}

/* ---- the machine -------------------------------------------------------------------- */

static void start_machine(void)
{
    a800session_start_opts opts;
    if (a800session_is_running(g_session)) return;
    a800session_default_opts(g_session, &opts);
    if (a800session_start(g_session, &opts) != 0)
        message_utf8(g_hwnd, a800session_last_error(g_session), "Cannot Start the Atari",
                     MB_ICONWARNING | MB_OK);
    update_status();
}

static void power_cycle(void)
{
    if (!a800session_is_running(g_session)) {
        start_machine();
        return;
    }
    if (a800session_power_cycle(g_session) != 0)
        message_utf8(g_hwnd, a800session_last_error(g_session), "Cannot Power Cycle",
                     MB_ICONWARNING | MB_OK);
    update_status();
}

static void run_sysaction(int sa)
{
    switch (sa) {
    case A800_SYSACT_PAUSE:
        /* showing the debugger attaches it, and attaching stops the machine */
        a800_debugger_show(g_hwnd, g_session);
        break;
    case A800_SYSACT_POWER: power_cycle(); break;
    case A800_SYSACT_REBOOT_CONFIG: a800session_reboot_to_config(g_session); break;
    case A800_SYSACT_WARP:
        a800session_set_warp(g_session, !a800session_warp(g_session));
        update_status();
        break;
    default: break;
    }
}

/* Hold a console key for a few frames, as a finger would. */
static void tap_console(int key)
{
    if (g_tap_key >= 0) a800session_console(g_session, g_tap_key, 0);
    g_tap_key = key;
    a800session_console(g_session, key, 1);
    SetTimer(g_hwnd, IDT_TAP, TAP_MS, NULL);
}

static void tap_release(void)
{
    KillTimer(g_hwnd, IDT_TAP);
    if (g_tap_key >= 0) a800session_console(g_session, g_tap_key, 0);
    g_tap_key = -1;
}

static void apply_view(void)
{
    const int i = a800session_pref_find("aspect");
    g_aspect = i < 0 || a800session_pref_get_choice(g_session, i) == 0;
    g_smooth = a800session_get_int(g_session, "smooth", 0) != 0;
    InvalidateRect(g_hwnd, NULL, TRUE);
}

static void settings_live(void)
{
    apply_view();
}

static void settings_closed(int applied)
{
    apply_view();
    a800_controller_window_refresh();
    if (applied == A800_APPLY_POWER) notice("Power cycled with the new configuration");
    else if (applied == A800_APPLY_RESTART) notice("Restarted with the new options");
    update_status();
}

static void copy_screen(void)
{
    static char buf[8192];
    wchar_t *w;
    int n;
    HGLOBAL mem;
    if (a800session_copy_screen_text(g_session, buf, sizeof buf) <= 0) {
        notice("No text screen to copy");
        return;
    }
    n = MultiByteToWideChar(CP_UTF8, 0, buf, -1, NULL, 0);
    mem = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)n * sizeof(wchar_t));
    if (!mem || !OpenClipboard(g_hwnd)) {
        if (mem) GlobalFree(mem);
        return;
    }
    w = GlobalLock(mem);
    MultiByteToWideChar(CP_UTF8, 0, buf, -1, w, n);
    GlobalUnlock(mem);
    EmptyClipboard();
    SetClipboardData(CF_UNICODETEXT, mem);
    CloseClipboard();
    notice("The screen's text is on the clipboard");
}

static void paste(void)
{
    HANDLE h;
    if (!OpenClipboard(g_hwnd)) return;
    h = GetClipboardData(CF_UNICODETEXT);
    if (h) {
        const wchar_t *w = GlobalLock(h);
        if (w) {
            int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
            char *u = malloc((size_t)n);
            if (u) {
                WideCharToMultiByte(CP_UTF8, 0, w, -1, u, n, NULL, NULL);
                a800session_paste(g_session, u);
                free(u);
            }
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
}

/* The Atari crashed with no debugger open: power cycle, or look. */
static void show_crash(const char *text)
{
    char body[600];
    int r;
    snprintf(body, sizeof body, "%s\n\nPower cycle now? (No opens the debugger.)", text);
    r = MessageBoxA(g_hwnd, body, "The Atari Stopped", MB_ICONWARNING | MB_YESNOCANCEL);
    if (r == IDYES) power_cycle();
    else if (r == IDNO) a800_debugger_show(g_hwnd, g_session);
}

static void about(void)
{
    message_utf8(g_hwnd,
        APP_TITLE " " A800_VERSION_STRING "\n\n"
        "The Atari 400, 800, XL and XE computers with a FujiNet on the SIO port.\n"
        "The emulator is atari800 (GPL-2.0-or-later, the Atari800 Development Team),\n"
        "with the FujiNet firmware (GPL-3.0-or-later) connected over NetSIO.\n"
        "AltirraOS and Altirra BASIC (Avery Lee) are built in; Atari's own ROMs\n"
        "can be imported.\n\n"
        "Copyright (C) 2026 Thomas Cherryhomes -- GPL-3.0-or-later\n"
        "https://fujinet.online/",
        "About " APP_TITLE, MB_ICONINFORMATION | MB_OK);
}

/* ---- window ------------------------------------------------------------------------ */

static void toggle_fullscreen(HWND hwnd)
{
    DWORD style = GetWindowLong(hwnd, GWL_STYLE);
    if (!g_fullscreen) {
        MONITORINFO mi;
        memset(&mi, 0, sizeof mi);
        mi.cbSize = sizeof mi;
        g_placement.length = sizeof g_placement;
        GetWindowPlacement(hwnd, &g_placement);
        if (GetMonitorInfo(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi)) {
            SetWindowLong(hwnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
            SetMenu(hwnd, NULL);
            ShowWindow(g_statusbar, SW_HIDE);
            SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                         mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                         SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
            g_fullscreen = 1;
        }
    } else {
        SetWindowLong(hwnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        build_menu(hwnd);
        ShowWindow(g_statusbar, SW_SHOW);
        SetWindowPlacement(hwnd, &g_placement);
        SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        g_fullscreen = 0;
    }
    InvalidateRect(hwnd, NULL, TRUE);
}

/* The pointer over the picture, for mice, the Koala Pad and the light pen. */
static void pointer_at(int x, int y, WPARAM wp)
{
    const unsigned buttons = ((wp & MK_LBUTTON) ? 1u : 0u) | ((wp & MK_RBUTTON) ? 2u : 0u);
    const int sw = g_shown.right - g_shown.left, sh = g_shown.bottom - g_shown.top;
    int fx, fy, inside;
    if (!a800session_pointer_device_active(g_session) || sw <= 0 || sh <= 0 || g_fw <= 0) return;
    fx = (x - g_shown.left) * g_fw / sw;
    fy = (y - g_shown.top) * g_fh / sh;
    inside = fx >= 0 && fy >= 0 && fx < g_fw && fy < g_fh;
    a800session_pointer(g_session, fx < 0 ? 0 : fx >= g_fw ? g_fw - 1 : fx,
                        fy < 0 ? 0 : fy >= g_fh ? g_fh - 1 : fy, inside, buttons);
    if (g_have_mouse) {
        /* mice count movement: in frame pixels, as the picture is drawn */
        const int dx = (x - g_mouse_x) * g_fw / sw, dy = (y - g_mouse_y) * g_fh / sh;
        if (dx || dy) a800session_pointer_motion(g_session, dx, dy, buttons);
        if (!dx) x = g_mouse_x;
        if (!dy) y = g_mouse_y;
    }
    g_mouse_x = x;
    g_mouse_y = y;
    g_have_mouse = 1;
}

static void command(HWND hwnd, int id)
{
    char path[MAX_PATH], msg[512];

    if (id >= IDM_DRIVE_INSERT && id < IDM_DRIVE_INSERT + A800_DRIVES) {
        const int d = id - IDM_DRIVE_INSERT;
        const int fuji = a800session_fujinet_running(g_session);
        snprintf(msg, sizeof msg, "%s D%d:", fuji ? "Mount in" : "Insert in", d + 1);
        if (pick_file(hwnd, msg, kDiskFilter, fuji ? a800session_sd_path(g_session) : media_folder(),
                      path, sizeof path, 0, NULL))
            insert_into(d, path);
        return;
    }
    if (id >= IDM_DRIVE_EJECT && id < IDM_DRIVE_EJECT + A800_DRIVES) {
        a800session_drive_eject(g_session, id - IDM_DRIVE_EJECT);
        return;
    }
    if (id >= IDM_DRIVE_PROTECT && id < IDM_DRIVE_PROTECT + A800_DRIVES) {
        a800_drive_info di;
        const int d = id - IDM_DRIVE_PROTECT;
        if (a800session_drive_info(g_session, d, &di) == 0 && di.path[0])
            a800session_drive_set_read_only(g_session, d, !di.read_only);
        return;
    }
    if (id >= IDM_DRIVE_BLANK && id < IDM_DRIVE_BLANK + A800_DRIVES * 4) {
        const int d = (id - IDM_DRIVE_BLANK) / 4, density = (id - IDM_DRIVE_BLANK) % 4;
        if (!pick_file(hwnd, "Save the New Disk", "ATR disk images (*.atr)\0*.atr\0\0", media_folder(),
                       path, sizeof path, 1, "blank.atr"))
            return;
        if (a800session_new_blank_disk(path, density) != 0)
            message_utf8(hwnd, path, "Cannot Create the Disk", MB_ICONWARNING | MB_OK);
        else
            insert_into(d, path);
        return;
    }
    if (id >= IDM_CONSOLE && id < IDM_CONSOLE + A800_CON_COUNT) {
        tap_console(id - IDM_CONSOLE);
        return;
    }

    switch (id) {
    case IDM_OPEN:
        if (pick_file(hwnd, "Open", kAnyFilter, media_folder(), path, sizeof path, 0, NULL))
            load_media(path);
        break;
    case IDM_POWER: power_cycle(); break;
    case IDM_RESET: a800session_reset(g_session); break;
    case IDM_REBOOT_CONFIG:
        if (a800session_reboot_to_config(g_session) == 0) notice("Back to FujiNet's CONFIG");
        break;
    case IDM_WARP:
        a800session_set_warp(g_session, !a800session_warp(g_session));
        update_status();
        break;
    case IDM_STATE_SAVE:
        snprintf(msg, sizeof msg, "%s\\states", a800session_data_path(g_session));
        CreateDirectoryA(msg, NULL);
        if (!pick_file(hwnd, "Save State", kStateFilter, msg, path, sizeof path, 1, "atari.a8s")) break;
        if (a800session_state_save(g_session, path) != 0)
            message_utf8(hwnd, a800session_last_error(g_session), "Cannot Save the State", MB_ICONWARNING | MB_OK);
        else
            notice("State saved");
        break;
    case IDM_STATE_LOAD:
        snprintf(msg, sizeof msg, "%s\\states", a800session_data_path(g_session));
        if (!pick_file(hwnd, "Load State", kStateFilter, msg, path, sizeof path, 0, NULL)) break;
        if (a800session_state_load(g_session, path) != 0)
            message_utf8(hwnd, a800session_last_error(g_session), "Cannot Load the State", MB_ICONWARNING | MB_OK);
        else
            notice("State loaded");
        break;
    case IDM_IMPORT_ROMS: import_roms(); break;
    case IDM_SETTINGS: a800_prefs_show(hwnd, g_session, settings_live, settings_closed); break;
    case IDM_EXIT: PostMessage(hwnd, WM_CLOSE, 0, 0); break;

    case IDM_IMPORT_SD:
        if (pick_file(hwnd, "Import to FujiNet's SD Card", kAnyFilter, media_folder(), path, sizeof path, 0, NULL)) {
            char dest[1024];
            if (a800session_import_to_sd(g_session, path, dest, sizeof dest) == 0)
                notice("Copied to FujiNet's SD card: mount it from CONFIG or Drives");
            else
                message_utf8(hwnd, a800session_last_error(g_session), "Import Failed", MB_ICONWARNING | MB_OK);
        }
        break;
    case IDM_SD_FOLDER: open_folder(a800session_sd_path(g_session)); break;
    case IDM_MEDIA_FOLDER: open_folder(a800session_media_path(g_session)); break;

    case IDM_CART_INSERT:
    case IDM_CART_PIGGYBACK: {
        const int pb = id == IDM_CART_PIGGYBACK;
        int rc;
        if (!pick_file(hwnd, pb ? "Insert a Piggyback Cartridge" : "Insert a Cartridge", kCartFilter,
                       media_folder(), path, sizeof path, 0, NULL))
            break;
        rc = pb ? a800session_cart_insert_piggyback(g_session, path, 0, msg, sizeof msg)
                : a800session_cart_insert(g_session, path, 0, msg, sizeof msg);
        remember_folder(path);
        if (rc > 0) ask_cart_type(path, pb);
        else if (rc < 0) message_utf8(hwnd, msg, "Cannot Use the Cartridge", MB_ICONWARNING | MB_OK);
        else notice(msg[0] ? msg : "Cartridge inserted");
        break;
    }
    case IDM_CART_REMOVE: a800session_cart_remove(g_session); break;
    case IDM_CART_REMOVE_PB: a800session_cart_remove_piggyback(g_session); break;

    case IDM_TAPE_INSERT:
        if (!pick_file(hwnd, "Load a Tape", kTapeFilter, media_folder(), path, sizeof path, 0, NULL)) break;
        if (a800session_tape_insert(g_session, path, msg, sizeof msg) != 0) {
            message_utf8(hwnd, msg, "Cannot Load the Tape", MB_ICONWARNING | MB_OK);
            break;
        }
        remember_folder(path);
        notice(msg[0] ? msg : "Tape loaded: Boot Tape, or CLOAD / RUN \"C:\"");
        break;
    case IDM_TAPE_RECORD:
        if (!pick_file(hwnd, "Record a New Tape", kTapeFilter, media_folder(), path, sizeof path, 1, "tape.cas"))
            break;
        if (a800session_tape_record_new(g_session, path) != 0)
            message_utf8(hwnd, a800session_last_error(g_session), "Cannot Record", MB_ICONWARNING | MB_OK);
        else
            notice("Recording: CSAVE or LIST \"C:\" writes to the tape");
        break;
    case IDM_TAPE_REWIND: a800session_tape_rewind(g_session); break;
    case IDM_TAPE_BOOT: a800session_tape_boot(g_session); break;
    case IDM_TAPE_EJECT: a800session_tape_eject(g_session); break;

    case IDM_COPY_SCREEN: copy_screen(); break;
    case IDM_PASTE: paste(); break;

    case IDM_CONTROLLERS: a800_controller_window_toggle(hwnd, g_session); break;
    case IDM_DEBUGGER: a800_debugger_toggle(hwnd, g_session); break;
    case IDM_FULLSCREEN: toggle_fullscreen(hwnd); break;
    case IDM_COL80:
        a800session_set_80col_display(g_session, !a800session_80col_display(g_session));
        break;
    case IDM_TV_ASPECT: {
        const int i = a800session_pref_find("aspect");
        if (i >= 0) a800session_pref_set_choice(g_session, i, g_aspect ? 1 : 0);
        apply_view();
        break;
    }
    case IDM_SMOOTH:
        a800session_set_int(g_session, "smooth", !g_smooth);
        apply_view();
        break;

    case IDM_FUJINET_CONFIG:
        if (a800session_fujinet_external(g_session)) {
            message_utf8(hwnd, "FujiNet is external: use its own web UI.", "FujiNet", MB_ICONINFORMATION | MB_OK);
            break;
        }
        if (!a800session_fujinet_running(g_session)) {
            message_utf8(hwnd, "FujiNet is off (Settings, FujiNet page).", "FujiNet", MB_ICONINFORMATION | MB_OK);
            break;
        }
        ShellExecuteA(hwnd, "open", a800session_fujinet_webui_url(g_session), NULL, NULL, SW_SHOWNORMAL);
        break;
    case IDM_FUJINET_LOG:
        if (a800session_fujinet_external(g_session)) {
            message_utf8(hwnd, "FujiNet is external: see its own console.", "FujiNet", MB_ICONINFORMATION | MB_OK);
            break;
        }
        show_fujinet_log();
        break;
    case IDM_FUJINET_RESTART:
        if (a800session_fujinet_external(g_session)) {
            message_utf8(hwnd, "FujiNet is external: restart it where it runs.", "FujiNet", MB_ICONINFORMATION | MB_OK);
            break;
        }
        if (a800session_fujinet_restart(g_session) != 0)
            message_utf8(hwnd, a800session_last_error(g_session), "Cannot Restart FujiNet", MB_ICONWARNING | MB_OK);
        else
            notice("FujiNet restarted");
        break;
    case IDM_ABOUT: about(); break;
    default: break;
    }
}

/* The app's shortcuts: no accelerator table, so the Atari's keys are never
 * eaten by the menu. */
static int app_shortcut(HWND hwnd, WPARAM vk)
{
    const int ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const int shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    if (vk == VK_F9) { a800_controller_window_toggle(hwnd, g_session); return 1; }
    if (vk == VK_F12) { a800_debugger_toggle(hwnd, g_session); return 1; }
    if (vk == VK_F11) { toggle_fullscreen(hwnd); return 1; }
    if (vk == VK_F5 && shift) { power_cycle(); return 1; }
    if (ctrl && vk == VK_OEM_COMMA) { command(hwnd, IDM_SETTINGS); return 1; }
    if (ctrl && shift) {
        switch (vk) {
        case 'O': command(hwnd, IDM_OPEN); return 1;
        case 'Q': command(hwnd, IDM_EXIT); return 1;
        case 'C': command(hwnd, IDM_COPY_SCREEN); return 1;
        case 'V': command(hwnd, IDM_PASTE); return 1;
        case 'W': command(hwnd, IDM_WARP); return 1;
        default: break;
        }
    }
    return 0;
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        paint(dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        if (g_statusbar) SendMessageA(g_statusbar, WM_SIZE, 0, 0);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_DRAWITEM:
        if (wp == IDC_STATUSBAR) { draw_status_dot((const DRAWITEMSTRUCT *)lp); return TRUE; }
        break;
    case WM_INITMENUPOPUP: {
        HMENU m = (HMENU)wp;
        if (m == g_drives_menu) fill_drives_menu();
        else if (m == g_cart_menu) fill_cart_menu();
        else if (m == g_tape_menu) fill_tape_menu();
        else sync_menus();
        break;
    }

    case WM_KEYDOWN: case WM_SYSKEYDOWN:
        if (!(lp & (1 << 30)) && app_shortcut(hwnd, wp)) return 0;
        if (!a800_app_key(wp) && !(lp & (1 << 30))) {
            const uint32_t ks = a800_keysym_from_msg(wp, lp);
            const int sa = ks ? a800session_key_sysaction(g_session, ks) : -1;
            if (sa >= 0) { run_sysaction(sa); update_status(); return 0; }
        }
        if (a800_forward_key(g_session, msg, wp, lp)) return 0;
        break;
    case WM_KEYUP: case WM_SYSKEYUP:
        if (a800_forward_key(g_session, msg, wp, lp)) return 0;
        break;
    case WM_SYSCHAR:
        /* ALT+key with no menu mnemonic: no beep */
        break;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE && !a800_controller_window_visible())
            a800session_release_all(g_session);
        return 0;

    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN: case WM_LBUTTONUP:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP:
        pointer_at((short)LOWORD(lp), (short)HIWORD(lp), wp);
        return 0;

    case WM_TIMER:
        if (wp == IDT_STATUS) {
            update_status();
        } else if (wp == IDT_SYSACT) {
            char text[256];
            unsigned gen;
            int sa;
            while (a800session_sysaction_take(g_session, &sa)) {
                run_sysaction(sa);
                update_status();
            }
            gen = a800session_gamepad_generation(g_session);
            if (gen != g_pad_generation) {
                g_pad_generation = gen;
                if (a800session_gamepad_last_event(g_session, text, sizeof text) > 0) notice(text);
                a800_controller_window_refresh();
            }
            gen = a800session_fujinet_generation(g_session);
            if (gen != g_fuji_generation) {
                g_fuji_generation = gen;
                if (a800session_fujinet_last_event(g_session, text, sizeof text) > 0) notice(text);
            }
            if (a800session_take_notice(g_session, text, sizeof text) > 0) show_crash(text);
            if (g_shot_path[0] && InterlockedCompareExchange(&g_frames, 0, 0) >= g_shot_frames) {
                if (!save_png(g_shot_path)) OutputDebugStringA("A800_SCREENSHOT: could not write\n");
                g_shot_path[0] = '\0';
            }
            if (g_quit_frames && InterlockedCompareExchange(&g_frames, 0, 0) >= g_quit_frames) {
                g_quit_frames = 0;
                PostMessage(hwnd, WM_CLOSE, 0, 0);
            }
        } else if (wp == IDT_TAP) {
            tap_release();
        }
        return 0;

    case WM_COMMAND:
        command(hwnd, LOWORD(wp));
        return 0;

    case WM_DROPFILES: {
        char path[MAX_PATH];
        HDROP drop = (HDROP)wp;
        if (DragQueryFileA(drop, 0, path, sizeof path)) load_media(path);
        DragFinish(drop);
        return 0;
    }

    case WM_DESTROY:
        KillTimer(hwnd, IDT_STATUS);
        KillTimer(hwnd, IDT_SYSACT);
        tap_release();
        PostQuitMessage(0);
        return 0;
    default: break;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

static int env_on(const char *name)
{
    const char *env = getenv(name);
    return env && *env && *env != '0';
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdline, int show)
{
    WNDCLASSEX wc;
    MSG msg;
    RECT want;
    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_BAR_CLASSES | ICC_TAB_CLASSES | ICC_STANDARD_CLASSES |
                                             ICC_LISTVIEW_CLASSES | ICC_UPDOWN_CLASS };
    int parts[2] = { DOT_PART_W, -1 };
    char media[MAX_PATH] = "";
    (void)prev;

    g_inst = inst;
    InitCommonControlsEx(&icc);
    InitializeCriticalSection(&g_fb_lock);

    g_session = a800session_new(NULL);
    if (!g_session) {
        MessageBoxA(NULL, "Could not create the session (unusable config or data directories?)",
                    APP_TITLE, MB_ICONERROR | MB_OK);
        return 1;
    }
    g_fb = calloc((size_t)A800SESSION_FB_MAX_WIDTH * A800SESSION_FB_MAX_HEIGHT, sizeof *g_fb);
    if (!g_fb) return 1;
    g_accent_brush = CreateSolidBrush(RGB((A800SESSION_ACCENT_RGB >> 16) & 0xff,
                                          (A800SESSION_ACCENT_RGB >> 8) & 0xff,
                                          A800SESSION_ACCENT_RGB & 0xff));

    memset(&g_bmi, 0, sizeof g_bmi);
    g_bmi.bmiHeader.biSize = sizeof g_bmi.bmiHeader;
    g_bmi.bmiHeader.biPlanes = 1;
    g_bmi.bmiHeader.biBitCount = 32;
    g_bmi.bmiHeader.biCompression = BI_RGB;

    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = WIN_CLASS;
    wc.hIcon = LoadIcon(inst, MAKEINTRESOURCE(IDI_APPICON));
    wc.hIconSm = wc.hIcon;
    RegisterClassEx(&wc);

    /* 336 x 240 at three times, a TV's shape, plus chrome */
    SetRect(&want, 0, 0, 864, 720);
    AdjustWindowRectEx(&want, WS_OVERLAPPEDWINDOW, TRUE, WS_EX_ACCEPTFILES);
    g_hwnd = CreateWindowEx(WS_EX_ACCEPTFILES, WIN_CLASS, APP_TITLE, WS_OVERLAPPEDWINDOW,
                            CW_USEDEFAULT, CW_USEDEFAULT, want.right - want.left,
                            want.bottom - want.top + 24, NULL, NULL, inst, NULL);
    if (!g_hwnd) return 1;
    apply_view();
    build_menu(g_hwnd);
    g_statusbar = CreateWindowExA(0, STATUSCLASSNAMEA, "", WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
                                  0, 0, 0, 0, g_hwnd, (HMENU)(INT_PTR)IDC_STATUSBAR, inst, NULL);
    SendMessageA(g_statusbar, SB_SETPARTS, 2, (LPARAM)parts);
    ShowWindow(g_hwnd, show);

    if (cmdline && *cmdline) {
        /* a path on the command line, quotes and all, goes where Open would
         * put it once the machine is up */
        const char *p = cmdline;
        size_t n;
        if (*p == '"') p++;
        snprintf(media, sizeof media, "%s", p);
        n = strlen(media);
        while (n && (media[n - 1] == '"' || media[n - 1] == ' ')) media[--n] = '\0';
    }
    /* FujiNet first, then the machine (the built-in OS needs no ROMs) */
    start_machine();
    if (media[0]) load_media(media);

    /* the current pads and FujiNet state are the baseline: no notices */
    g_pad_generation = a800session_gamepad_generation(g_session);
    g_fuji_generation = a800session_fujinet_generation(g_session);
    SetTimer(g_hwnd, IDT_STATUS, 1000, NULL);
    SetTimer(g_hwnd, IDT_SYSACT, 100, NULL);
    update_status();

    /* A800_OPEN_CONTROLLERS / _DEBUGGER / _SETTINGS open those windows at
     * launch; A800_SCREENSHOT=file.png saves the picture after
     * A800_SCREENSHOT_FRAMES frames (300); A800_AUTOQUIT_FRAMES quits. */
    if (env_on("A800_OPEN_CONTROLLERS")) a800_controller_window_toggle(g_hwnd, g_session);
    if (env_on("A800_OPEN_DEBUGGER")) a800_debugger_show(g_hwnd, g_session);
    if (env_on("A800_OPEN_SETTINGS")) a800_prefs_show(g_hwnd, g_session, settings_live, settings_closed);
    if (getenv("A800_SCREENSHOT")) snprintf(g_shot_path, sizeof g_shot_path, "%s", getenv("A800_SCREENSHOT"));
    if (getenv("A800_SCREENSHOT_FRAMES")) g_shot_frames = atol(getenv("A800_SCREENSHOT_FRAMES"));
    if (getenv("A800_AUTOQUIT_FRAMES")) g_quit_frames = atol(getenv("A800_AUTOQUIT_FRAMES"));

    g_present_thread = CreateThread(NULL, 0, present_thread, NULL, 0, NULL);

    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        /* Before TranslateMessage, so the sub-windows' keys reach them
         * regardless of which child control has the focus. */
        if (a800_debugger_pretranslate(&msg)) continue;
        if (a800_controller_pretranslate(&msg)) continue;
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    InterlockedExchange(&g_running, 0);
    if (g_present_thread) {
        WaitForSingleObject(g_present_thread, 2000);
        CloseHandle(g_present_thread);
    }
    a800session_stop(g_session);
    a800session_free(g_session);
    free(g_fb);
    DeleteObject(g_accent_brush);
    DeleteCriticalSection(&g_fb_lock);
    return (int)msg.wParam;
}
