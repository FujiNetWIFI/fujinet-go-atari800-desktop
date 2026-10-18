/*
 * The Win32 Controllers panel: the on-screen Atari keyboard (drawn from the
 * session's table, so every frontend shows the same keys for the machine:
 * HELP on the XL/XE, F1-F4 on the 1200XL), the controller ports with what
 * is plugged into each (a stick and fire for a joystick, two sliders for
 * paddles), the gamepads and their ports, and Map mode. The GNOME panel's
 * behaviour, in GDI.
 *
 * Keys and controls are driven by WM_LBUTTONDOWN/WM_LBUTTONUP on the window
 * rather than by BN_CLICKED: the Atari scans its keyboard and samples the
 * sticks once per frame, so a press present only for the instant of a click
 * falls between frames and a polling program never sees it. A key is HELD
 * while the mouse button is down, exactly like the real one; SHIFT and CTRL
 * latch for the next key instead (a mouse cannot hold two keys at once). The
 * mouse is captured on press; letting go anywhere, dragging off the key or
 * losing the capture releases it, so the Atari is never left holding one.
 *
 * What the Atari sees held -- host keyboard, gamepads and this panel
 * together -- is filled with the accent, so the panel doubles as an input
 * tester for a new gamepad.
 *
 * Keys typed here go to the Atari as in the main window (a800_forward_key);
 * the app's own keys (F11, F12, CTRL+SHIFT and ALT shortcuts) go to the main
 * window, and F9 hides the panel.
 *
 * FIXED SIZE: laid out for four ports and four gamepad rows whatever is
 * fitted, so neither the machine, Map mode nor a gamepad resizes it. The
 * keyboard and the ports are rebuilt when the machine or a port's device
 * changes.
 *
 * MAP MODE: Map arms a two-step rebind -- click a joystick control or a
 * console key (RESET, OPTION, SELECT, START, HELP, BREAK) to pick the
 * target, then press a key OR a gamepad button. While armed the other keys
 * grey out and each target shows what drives it. Rebinding steals the key
 * from whatever held it.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "controller_window.h"

/* GET_X_LPARAM / GET_Y_LPARAM live here, not in windows.h. */
#include <windowsx.h>
#include <commctrl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../key_forward.h"

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

#define PANEL_CLASS   "FujiNetGoAtari800Controllers"
#define PANEL_STYLE   (WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN)
#define PANEL_EXSTYLE WS_EX_TOOLWINDOW

/* Sizes at 96 dpi; S() scales them to the window's. */
#define MARGIN     12
#define KBD_UNIT   44       /* pixels per key unit */
#define KBD_GAP     3       /* between key caps */
#define KBD_COLS   16       /* key units: the widest row */
#define KBD_ROWS    6
#define CELL_W     52       /* a stick control: three lines of Map label fit */
#define CELL_H     44
#define CELL_GAP    3
#define PORT_PAD    8
#define PORT_GAP    8
#define TITLE_H    18
#define DEVICE_H   16
#define LABEL_H    16       /* "Paddle 0" */
#define SLIDER_H   28
#define FIRE_W     52
#define HEAD_H     22
#define ROW_H      26       /* one gamepad's row */
#define PAD_ROWS    4       /* gamepad rows the window has room for */
#define SEG_W      62       /* Automatic / Port 1-4 */
#define BTN_H      28
#define MAP_W      72
#define DEFAULTS_W 84

#define PADDLE_MAX 228
#define MAX_CONTROLS 128

#define IDT_CAPTURE 1
#define IDT_HELD    2
#define IDT_PADS    3
#define IDC_SLIDER  100     /* + port * 2 + paddle */
#define TIP_PAD_ROW 1000    /* tooltip ids: + row; below that, a control */

enum { CT_KEY, CT_STICK, CT_FIRE };

typedef struct {
    RECT rc;
    int kind;
    int key;                /* CT_KEY: on-screen keyboard index */
    int target;             /* what Map rebinds, or -1 */
    int act;                /* CT_STICK: an a800_action */
    int line;               /* CT_FIRE: port * 2 + paddle */
    const char *label;      /* the session's table (static strings) */
    const char *shifted;
    int lit;
} control;

typedef struct {
    unsigned generation;
    int count, assign[PAD_ROWS], effective[PAD_ROWS];
} pads_state;

static HWND g_panel, g_parent, g_tip;
static a800session *g_session;
static int g_dpi = 96;

/* Map mode: -2 idle, -1 armed and waiting for a target, >= 0 waiting for a
 * key or pad button to bind to that target. */
static int g_map_state = -2;
static WPARAM g_swallow_vk;     /* the key Map took: its repeats and release too */
static char g_hint[200];

static control g_ctl[MAX_CONTROLS];
static int g_nctl, g_ntips;
static int g_mouse_ctl = -1;    /* the control the captured mouse holds */

/* what the keyboard and the ports were built for */
static int g_built_machine = -1;
static int g_built_ports;
static int g_built_dev[A800_PORTS];

static HWND g_slider[A800_PORTS][2];
static int g_paddle_value[A800_PORTS][2];
static int g_paddle_fire[A800_PORTS][2];
static int g_paddles_init;

static pads_state g_pads_shown;

static int g_content_w, g_total_w, g_total_h, g_ports_y;
static RECT g_kbd_rc, g_port_rc[A800_PORTS], g_body_rc[A800_PORTS];
static RECT g_paddle_label_rc[A800_PORTS][2];
static RECT g_gp_rc, g_gp_head_rc, g_gp_row_rc[PAD_ROWS];
static RECT g_map_rc, g_defaults_rc, g_hint_rc;

static HBRUSH g_accent_brush;
static HFONT g_font, g_bold, g_small, g_key_font, g_key_small, g_sym_font;
static int g_key_h, g_key_small_h;
static char g_tip_text[256];

static void refresh_held(void);

/* ---- scale and fonts ------------------------------------------------------ */

static int S(int v)
{
    return MulDiv(v, g_dpi, 96);
}

static int system_dpi(void)
{
    HDC dc = GetDC(NULL);
    int d = dc ? GetDeviceCaps(dc, LOGPIXELSY) : 96;
    if (dc) ReleaseDC(NULL, dc);
    return d > 0 ? d : 96;
}

/* GetDpiForWindow is Windows 10 1607 and later. */
static int window_dpi(HWND hwnd)
{
    typedef UINT (WINAPI *dpi_fn)(HWND);
    static dpi_fn fn;
    static int looked;
    if (!looked) {
        HMODULE user = GetModuleHandleA("user32.dll");
        fn = user ? (dpi_fn)(void (*)(void))GetProcAddress(user, "GetDpiForWindow") : NULL;
        looked = 1;
    }
    if (fn && hwnd) {
        UINT d = fn(hwnd);
        if (d) return (int)d;
    }
    return system_dpi();
}

static void free_fonts(void)
{
    HFONT *f[] = { &g_font, &g_bold, &g_small, &g_key_font, &g_key_small, &g_sym_font };
    size_t i;
    for (i = 0; i < sizeof f / sizeof f[0]; i++)
        if (*f[i]) { DeleteObject(*f[i]); *f[i] = NULL; }
}

static HFONT font_like(const LOGFONTA *base, int px, int weight, const char *face)
{
    LOGFONTA lf = *base;
    lf.lfHeight = -px;
    lf.lfWidth = 0;
    lf.lfWeight = weight;
    if (face) snprintf(lf.lfFaceName, sizeof lf.lfFaceName, "%s", face);
    return CreateFontIndirectA(&lf);
}

static int font_has(HDC dc, HFONT f, const wchar_t *w, int n)
{
    WORD idx[32];
    HGDIOBJ old;
    int i, ok = 1;
    if (!f || n <= 0 || n > 32) return 0;
    old = SelectObject(dc, f);
    if (GetGlyphIndicesW(dc, w, n, idx, GGI_MARK_NONEXISTING_GLYPHS) == GDI_ERROR) ok = 0;
    for (i = 0; ok && i < n; i++)
        if (idx[i] == 0xffff) ok = 0;
    SelectObject(dc, old);
    return ok;
}

static int font_height(HDC dc, HFONT f)
{
    TEXTMETRICA tm;
    HGDIOBJ old = SelectObject(dc, f);
    int h = GetTextMetricsA(dc, &tm) ? tm.tmHeight : 12;
    SelectObject(dc, old);
    return h;
}

/* The message font (system metrics are at the system DPI), bold, and the
 * key caps' sizes. The Atari key's legend is a symbol the UI font may lack:
 * the first symbol face that has it draws it. */
static void make_fonts(void)
{
    static const char *const symbol_faces[] = { "Segoe UI Symbol", "DejaVu Sans" };
    NONCLIENTMETRICSA ncm;
    LOGFONTA lf;
    HDC dc;
    int px;
    size_t i;

    free_fonts();
    memset(&ncm, 0, sizeof ncm);
    ncm.cbSize = sizeof ncm;
    if (SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0)) lf = ncm.lfMessageFont;
    else GetObjectA(GetStockObject(DEFAULT_GUI_FONT), sizeof lf, &lf);
    px = MulDiv(abs(lf.lfHeight ? lf.lfHeight : 12), g_dpi, system_dpi());

    g_font = font_like(&lf, px, FW_NORMAL, NULL);
    g_bold = font_like(&lf, px, FW_BOLD, NULL);
    g_small = font_like(&lf, S(10), FW_NORMAL, NULL);
    g_key_font = font_like(&lf, S(11), FW_BOLD, NULL);
    g_key_small = font_like(&lf, S(9), FW_NORMAL, NULL);

    dc = GetDC(NULL);
    for (i = 0; i < sizeof symbol_faces / sizeof symbol_faces[0] && !g_sym_font; i++) {
        HFONT f = font_like(&lf, S(16), FW_NORMAL, symbol_faces[i]);
        if (font_has(dc, f, L"\x25d0", 1)) g_sym_font = f;
        else if (f) DeleteObject(f);
    }
    g_key_h = font_height(dc, g_key_font);
    g_key_small_h = font_height(dc, g_key_small);
    ReleaseDC(NULL, dc);
}

/* ---- text ------------------------------------------------------------------ */

/* UTF-8 (key legends, gamepad names) through DrawTextW. */
static int text_out(HDC dc, const char *utf8, RECT *rc, UINT fmt)
{
    wchar_t w[256];
    if (!utf8 || !*utf8) return 0;
    if (MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, (int)(sizeof w / sizeof w[0])) <= 0) return 0;
    return DrawTextW(dc, w, -1, rc, fmt | DT_NOPREFIX);
}

static void text_line(HDC dc, const char *utf8, const RECT *rc, UINT fmt)
{
    RECT r = *rc;
    text_out(dc, utf8, &r, fmt | DT_SINGLELINE | DT_VCENTER);
}

/* Centred both ways, wrapped (a Map label is a key and a pad button). */
static void text_block(HDC dc, const char *utf8, const RECT *rc)
{
    RECT m = *rc;
    int h;
    InflateRect(&m, -S(2), 0);
    h = text_out(dc, utf8, &m, DT_CENTER | DT_WORDBREAK | DT_CALCRECT);
    m.left = rc->left + S(2);
    m.right = rc->right - S(2);
    m.top = rc->top + ((rc->bottom - rc->top) - h) / 2;
    if (m.top < rc->top) m.top = rc->top;
    m.bottom = rc->bottom;
    text_out(dc, utf8, &m, DT_CENTER | DT_WORDBREAK);
}

/* UTF-8 to the ANSI code page, for the ANSI tooltips. */
static void to_acp(const char *utf8, char *out, int outsz)
{
    wchar_t w[256];
    out[0] = '\0';
    if (MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, (int)(sizeof w / sizeof w[0])) > 0)
        WideCharToMultiByte(CP_ACP, 0, w, -1, out, outsz, NULL, NULL);
    out[outsz - 1] = '\0';
}

/* ---- pressing ------------------------------------------------------------ */

static void paddle_send(int port, int p)
{
    a800session_paddle(g_session, port, p, g_paddle_value[port][p], g_paddle_fire[port][p]);
}

/* Lets go of what the mouse holds in this panel (only that: the main window
 * releases its own keys). */
static void mouse_release(void)
{
    const int i = g_mouse_ctl;
    control *c;
    if (i < 0) return;
    g_mouse_ctl = -1;
    if (i >= g_nctl) return;
    c = &g_ctl[i];
    if (c->kind == CT_KEY) {
        a800session_keyboard_press(g_session, c->key, 0);
    } else if (c->kind == CT_STICK) {
        a800session_press(g_session, c->target, 0);
    } else {
        g_paddle_fire[c->line / 2][c->line % 2] = 0;
        paddle_send(c->line / 2, c->line % 2);
    }
    if (g_panel) InvalidateRect(g_panel, &c->rc, FALSE);
}

static void control_press(int i)
{
    control *c = &g_ctl[i];
    mouse_release();
    g_mouse_ctl = i;
    if (c->kind == CT_KEY) {
        a800session_keyboard_press(g_session, c->key, 1);
    } else if (c->kind == CT_STICK) {
        a800session_press(g_session, c->target, 1);
    } else {
        g_paddle_fire[c->line / 2][c->line % 2] = 1;
        paddle_send(c->line / 2, c->line % 2);
    }
}

static void set_hint(const char *text)
{
    snprintf(g_hint, sizeof g_hint, "%s", text);
    if (g_panel) InvalidateRect(g_panel, &g_hint_rc, FALSE);
}

static void set_map_state(int state)
{
    int i, port, p;
    g_map_state = state;
    if (state < 0) {
        if (g_panel) KillTimer(g_panel, IDT_CAPTURE);
        a800session_gamepad_capture_cancel(g_session);
    }
    if (state == -2) {
        set_hint("Click and hold to press. SHIFT and CTRL latch for the next key.");
    } else if (state == -1) {
        set_hint("Click a joystick control or console key to remap");
    } else {
        char msg[160];
        mouse_release();
        snprintf(msg, sizeof msg, "Press a key or gamepad button for %s", a800_target_name(state));
        set_hint(msg);
        a800session_gamepad_capture_begin(g_session);
        if (g_panel) SetTimer(g_panel, IDT_CAPTURE, 50, NULL);
    }
    for (i = 0; i < g_nctl; i++)
        g_ctl[i].lit = 0;           /* refresh_held relights them when idle */
    for (port = 0; port < A800_PORTS; port++)
        for (p = 0; p < 2; p++)
            if (g_slider[port][p]) EnableWindow(g_slider[port][p], state == -2);
    if (g_panel) {
        InvalidateRect(g_panel, NULL, FALSE);
        refresh_held();
    }
}

/* After a rebind: what it is now, and whom it was taken from. */
static void bound_hint(const char *what, const char *stolen)
{
    char msg[200];
    if (stolen[0]) snprintf(msg, sizeof msg, "Bound %s (was %s)", what, stolen);
    else snprintf(msg, sizeof msg, "Bound %s", what);
    set_hint(msg);
}

/* ---- layout ------------------------------------------------------------------ */

static int port_w(void) { return 2 * S(PORT_PAD) + 3 * S(CELL_W) + 2 * S(CELL_GAP); }
static int body_h(void) { return 3 * S(CELL_H) + 2 * S(CELL_GAP); }
static int port_h(void) { return 2 * S(PORT_PAD) + S(TITLE_H) + S(DEVICE_H) + S(4) + body_h(); }

static int px(float units)
{
    return (int)(units * (float)S(KBD_UNIT) + 0.5f);
}

/* Everything but the keys and the ports: the window is this size for every
 * machine. */
static void layout_static(void)
{
    const int kbd_w = S(KBD_UNIT) * KBD_COLS;
    const int ports_w = A800_PORTS * port_w() + (A800_PORTS - 1) * S(PORT_GAP);
    const int left = S(MARGIN);
    int y = S(MARGIN), i;

    g_content_w = kbd_w > ports_w ? kbd_w : ports_w;
    g_total_w = 2 * left + g_content_w;
    SetRect(&g_kbd_rc, left + (g_content_w - kbd_w) / 2, y,
            left + (g_content_w - kbd_w) / 2 + kbd_w, y + S(KBD_UNIT) * KBD_ROWS);
    y = g_kbd_rc.bottom + S(MARGIN);
    g_ports_y = y;
    y += port_h() + S(MARGIN);

    SetRect(&g_gp_head_rc, left, y, left + g_content_w, y + S(HEAD_H));
    y += S(HEAD_H);
    for (i = 0; i < PAD_ROWS; i++) {
        SetRect(&g_gp_row_rc[i], left, y, left + g_content_w, y + S(ROW_H));
        y += S(ROW_H) + S(2);
    }
    SetRect(&g_gp_rc, left, g_gp_head_rc.top, left + g_content_w, y);
    y += S(MARGIN) - S(2);

    SetRect(&g_map_rc, left, y, left + S(MAP_W), y + S(BTN_H));
    SetRect(&g_defaults_rc, g_map_rc.right + S(6), y, g_map_rc.right + S(6) + S(DEFAULTS_W), y + S(BTN_H));
    SetRect(&g_hint_rc, g_defaults_rc.right + S(10), y, left + g_content_w, y + S(BTN_H));
    g_total_h = y + S(BTN_H) + S(MARGIN);
}

/* The Automatic / Port 1-4 choice at the right of a gamepad row. */
static void segment_rect(int row, int seg, RECT *rc)
{
    const RECT *r = &g_gp_row_rc[row];
    const int x = r->right - (5 - seg) * (S(SEG_W) + S(4)) + S(4);
    SetRect(rc, x, r->top + 1, x + S(SEG_W), r->bottom - 1);
}

static void gp_name_rect(int row, RECT *rc)
{
    RECT seg0;
    segment_rect(row, 0, &seg0);
    *rc = g_gp_row_rc[row];
    rc->right = seg0.left - S(120);
}

static control *add_control(int kind)
{
    control *c;
    if (g_nctl >= MAX_CONTROLS) return NULL;
    c = &g_ctl[g_nctl++];
    memset(c, 0, sizeof *c);
    c->kind = kind;
    c->key = -1;
    c->target = -1;
    c->line = -1;
    return c;
}

/* The keys that are console keys as far as Map is concerned. The layout
 * table carries labels only, so they are matched by label. */
static int console_target(const char *label)
{
    static const struct { const char *label; int con; } con[] = {
        { "RESET", A800_CON_RESET }, { "OPTION", A800_CON_OPTION },
        { "SELECT", A800_CON_SELECT }, { "START", A800_CON_START },
        { "HELP", A800_CON_HELP }, { "BREAK", A800_CON_BREAK },
    };
    size_t i;
    for (i = 0; label && i < sizeof con / sizeof con[0]; i++)
        if (!strcmp(label, con[i].label))
            return A800_TARGET_CONSOLE(con[i].con);
    return -1;
}

static void build_keyboard(void)
{
    const a800_kbd_key *keys = NULL;
    const int n = a800session_keyboard_layout(g_session, &keys);
    const int gap = S(KBD_GAP);
    float ew = 0, eh = 0;
    int i, x0, y0;

    for (i = 0; i < n; i++) {
        if (keys[i].x + keys[i].w > ew) ew = keys[i].x + keys[i].w;
        if (keys[i].y + keys[i].h > eh) eh = keys[i].y + keys[i].h;
    }
    x0 = g_kbd_rc.left + ((g_kbd_rc.right - g_kbd_rc.left) - px(ew)) / 2;
    y0 = g_kbd_rc.top + ((g_kbd_rc.bottom - g_kbd_rc.top) - px(eh)) / 2;
    for (i = 0; i < n; i++) {
        const a800_kbd_key *k = &keys[i];
        control *c = add_control(CT_KEY);
        if (!c) break;
        c->rc.left = x0 + px(k->x) + gap / 2;
        c->rc.top = y0 + px(k->y) + gap / 2;
        c->rc.right = c->rc.left + px(k->x + k->w) - px(k->x) - gap;
        c->rc.bottom = c->rc.top + px(k->y + k->h) - px(k->y) - gap;
        c->key = k->index;
        c->label = k->label ? k->label : "";
        c->shifted = k->shifted;
        c->target = console_target(c->label);
    }
}

/* The stick's four directions around its fire button. */
static void build_joystick(int port)
{
    static const int cells[5][3] = {        /* action, column, row */
        { A800_ACT_UP, 1, 0 }, { A800_ACT_LEFT, 0, 1 }, { A800_ACT_FIRE, 1, 1 },
        { A800_ACT_RIGHT, 2, 1 }, { A800_ACT_DOWN, 1, 2 },
    };
    const RECT *b = &g_body_rc[port];
    int i;
    for (i = 0; i < 5; i++) {
        control *c = add_control(CT_STICK);
        const int x = b->left + cells[i][1] * (S(CELL_W) + S(CELL_GAP));
        const int y = b->top + cells[i][2] * (S(CELL_H) + S(CELL_GAP));
        if (!c) return;
        SetRect(&c->rc, x, y, x + S(CELL_W), y + S(CELL_H));
        c->act = cells[i][0];
        c->target = A800_TARGET_PORT(port, cells[i][0]);
        c->label = cells[i][0] == A800_ACT_FIRE ? "Fire" : "";
    }
}

/* A pair of paddles, numbered as Atari BASIC's PADDLE() does: a slider each
 * (228 at the left, 0 at the right, as turning clockwise lowers it) and a
 * fire button. */
static void build_paddles(int port)
{
    const RECT *b = &g_body_rc[port];
    const int block = S(LABEL_H) + S(SLIDER_H), between = S(10);
    int p, y = b->top + ((b->bottom - b->top) - (2 * block + between)) / 2;

    for (p = 0; p < 2; p++, y += block + between) {
        const int line = port * 2 + p;
        const int top = y + S(LABEL_H), fire_x = b->right - S(FIRE_W);
        HWND s;
        control *c;

        SetRect(&g_paddle_label_rc[port][p], b->left, y, b->right, top);
        s = CreateWindowExA(0, TRACKBAR_CLASSA, "", WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_BOTH | TBS_NOTICKS,
                            b->left, top, fire_x - S(6) - b->left, S(SLIDER_H), g_panel,
                            (HMENU)(INT_PTR)(IDC_SLIDER + line), GetModuleHandleA(NULL), NULL);
        if (s) {
            SendMessageA(s, TBM_SETRANGE, FALSE, MAKELPARAM(0, PADDLE_MAX));
            SendMessageA(s, TBM_SETPAGESIZE, 0, 16);
            SendMessageA(s, TBM_SETPOS, TRUE, PADDLE_MAX - g_paddle_value[port][p]);
            EnableWindow(s, g_map_state == -2);
        }
        g_slider[port][p] = s;
        paddle_send(port, p);       /* the Atari reads what the slider shows */

        c = add_control(CT_FIRE);
        if (!c) continue;
        SetRect(&c->rc, fire_x, top, b->right, top + S(SLIDER_H));
        c->line = line;
        c->label = "Fire";
    }
}

static void build_port(int port, int x, int y)
{
    const int dev = a800session_port_device(g_session, port);
    const int bx = x + S(PORT_PAD), by = y + S(PORT_PAD) + S(TITLE_H) + S(DEVICE_H) + S(4);

    g_built_dev[port] = dev;
    SetRect(&g_port_rc[port], x, y, x + port_w(), y + port_h());
    SetRect(&g_body_rc[port], bx, by, bx + 3 * S(CELL_W) + 2 * S(CELL_GAP), by + body_h());
    if (dev == A800_DEV_JOYSTICK) build_joystick(port);
    else if (dev == A800_DEV_PADDLES) build_paddles(port);
}

/* The 400 and 800 have four ports, the XL/XE two (four with a MultiJoy). */
static int machine_now(void)
{
    int m = a800session_get_int(g_session, "machine", A800_M_130XE);
    return m >= 0 && m < A800_M_COUNT ? m : A800_M_130XE;
}

static int ports_now(int machine)
{
    return a800_machine_is_xl(machine) && !a800session_get_int(g_session, "multijoy", 0) ? 2 : A800_PORTS;
}

static int layout_changed(void)
{
    const int m = machine_now(), n = ports_now(m);
    int port;
    if (m != g_built_machine || n != g_built_ports) return 1;
    for (port = 0; port < n; port++)
        if (a800session_port_device(g_session, port) != g_built_dev[port]) return 1;
    return 0;
}

/* Hover text: SHIFT and CTRL latch; in Map mode, a target's whole binding;
 * a gamepad's whole name. */
static void build_tips(void)
{
    TTTOOLINFOA ti;
    int i;
    if (!g_tip) return;
    memset(&ti, 0, sizeof ti);
    ti.cbSize = TTTOOLINFOA_V2_SIZE;
    ti.hwnd = g_panel;
    for (i = 0; i < g_ntips; i++) {
        ti.uId = (UINT_PTR)i;
        SendMessageA(g_tip, TTM_DELTOOLA, 0, (LPARAM)&ti);
    }
    for (i = 0; i < PAD_ROWS; i++) {
        ti.uId = (UINT_PTR)(TIP_PAD_ROW + i);
        SendMessageA(g_tip, TTM_DELTOOLA, 0, (LPARAM)&ti);
    }
    ti.uFlags = TTF_SUBCLASS;
    ti.lpszText = LPSTR_TEXTCALLBACKA;
    for (i = 0; i < g_nctl; i++) {
        ti.uId = (UINT_PTR)i;
        ti.rect = g_ctl[i].rc;
        SendMessageA(g_tip, TTM_ADDTOOLA, 0, (LPARAM)&ti);
    }
    for (i = 0; i < PAD_ROWS; i++) {
        ti.uId = (UINT_PTR)(TIP_PAD_ROW + i);
        gp_name_rect(i, &ti.rect);
        SendMessageA(g_tip, TTM_ADDTOOLA, 0, (LPARAM)&ti);
    }
    g_ntips = g_nctl;
}

static void rebuild(void)
{
    int port, p, x0, total;

    mouse_release();
    if (g_panel && GetCapture() == g_panel) ReleaseCapture();
    g_nctl = 0;
    for (port = 0; port < A800_PORTS; port++)
        for (p = 0; p < 2; p++)
            if (g_slider[port][p]) { DestroyWindow(g_slider[port][p]); g_slider[port][p] = NULL; }
    if (!g_paddles_init) {
        for (port = 0; port < A800_PORTS; port++)
            for (p = 0; p < 2; p++)
                g_paddle_value[port][p] = PADDLE_MAX;   /* where the session starts them */
        g_paddles_init = 1;
    }

    g_built_machine = machine_now();
    g_built_ports = ports_now(g_built_machine);
    build_keyboard();
    total = g_built_ports * port_w() + (g_built_ports - 1) * S(PORT_GAP);
    x0 = S(MARGIN) + (g_content_w - total) / 2;
    for (port = 0; port < g_built_ports; port++)
        build_port(port, x0 + port * (port_w() + S(PORT_GAP)), g_ports_y);
    build_tips();
    if (g_panel) {
        InvalidateRect(g_panel, NULL, FALSE);
        refresh_held();
    }
}

/* ---- painting -------------------------------------------------------------- */

static void draw_face(HDC dc, const RECT *rc, int pushed, int accent, int inactive)
{
    RECT r = *rc;
    if (accent) {
        FillRect(dc, &r, g_accent_brush);
        FrameRect(dc, &r, (HBRUSH)GetStockObject(GRAY_BRUSH));
        SetTextColor(dc, RGB(255, 255, 255));
    } else {
        DrawFrameControl(dc, &r, DFC_BUTTON, DFCS_BUTTONPUSH | (pushed ? DFCS_PUSHED : 0)
                                             | (inactive ? DFCS_INACTIVE : 0));
        SetTextColor(dc, GetSysColor(inactive ? COLOR_GRAYTEXT : COLOR_BTNTEXT));
    }
}

/* A stick direction, as a triangle: no font has to have the arrows. */
static void draw_arrow(HDC dc, const RECT *r, int act, COLORREF col)
{
    const int cx = (r->left + r->right) / 2, cy = (r->top + r->bottom) / 2;
    const int a = S(8), b = S(6);
    POINT pt[3];
    HGDIOBJ oldb, oldp;
    switch (act) {
    case A800_ACT_UP:
        pt[0].x = cx; pt[0].y = cy - b; pt[1].x = cx + a; pt[1].y = cy + b; pt[2].x = cx - a; pt[2].y = cy + b;
        break;
    case A800_ACT_DOWN:
        pt[0].x = cx; pt[0].y = cy + b; pt[1].x = cx - a; pt[1].y = cy - b; pt[2].x = cx + a; pt[2].y = cy - b;
        break;
    case A800_ACT_LEFT:
        pt[0].x = cx - b; pt[0].y = cy; pt[1].x = cx + b; pt[1].y = cy - a; pt[2].x = cx + b; pt[2].y = cy + a;
        break;
    default:
        pt[0].x = cx + b; pt[0].y = cy; pt[1].x = cx - b; pt[1].y = cy + a; pt[2].x = cx - b; pt[2].y = cy - a;
        break;
    }
    oldb = SelectObject(dc, GetStockObject(DC_BRUSH));
    oldp = SelectObject(dc, GetStockObject(DC_PEN));
    SetDCBrushColor(dc, col);
    SetDCPenColor(dc, col);
    Polygon(dc, pt, 3);
    SelectObject(dc, oldp);
    SelectObject(dc, oldb);
}

/* In Map mode a target shows what drives it: the key, then the pad
 * button. */
static void binding_label(int target, char *out, int outsz, int whole)
{
    const a800_binding bind = a800session_binding_get(g_session, target);
    char key[32];
    key[0] = '\0';
    a800session_keysym_name(bind.keysym, key, sizeof key);
    if (whole) {
        char name[64];
        snprintf(name, sizeof name, "%s", a800_target_name(target));    /* a static buffer */
        if (bind.button != A800_PAD_BTN_NONE)
            snprintf(out, (size_t)outsz, "%s: %s, %s", name, key[0] ? key : "no key",
                     a800_pad_button_name(bind.button));
        else
            snprintf(out, (size_t)outsz, "%s: %s", name, key[0] ? key : "no key");
    } else if (bind.button != A800_PAD_BTN_NONE) {
        snprintf(out, (size_t)outsz, "%s\n%s", key[0] ? key : "\xe2\x80\x94", a800_pad_button_name(bind.button));
    } else {
        snprintf(out, (size_t)outsz, "%s", key[0] ? key : "\xe2\x80\x94");
    }
}

/* A key cap's legend: the shifted character small above the key's own. */
static void draw_legend(HDC dc, const control *c)
{
    const RECT *r = &c->rc;
    RECT t;
    if (c->shifted) {
        const int top = r->top + ((r->bottom - r->top) - g_key_small_h - g_key_h) / 2;
        SetRect(&t, r->left + 1, top, r->right - 1, top + g_key_small_h);
        SelectObject(dc, g_key_small);
        text_line(dc, c->shifted, &t, DT_CENTER | DT_END_ELLIPSIS);
        SetRect(&t, r->left + 1, t.bottom, r->right - 1, t.bottom + g_key_h);
        SelectObject(dc, g_key_font);
        text_line(dc, c->label, &t, DT_CENTER | DT_END_ELLIPSIS);
        return;
    }
    SelectObject(dc, g_key_font);
    if (g_sym_font && (unsigned char)c->label[0] >= 0x80) {
        wchar_t w[16];
        const int n = MultiByteToWideChar(CP_UTF8, 0, c->label, -1, w, 16) - 1;
        if (n > 0 && !font_has(dc, g_key_font, w, n)) SelectObject(dc, g_sym_font);
    }
    SetRect(&t, r->left + 1, r->top, r->right - 1, r->bottom);
    text_line(dc, c->label, &t, DT_CENTER);
}

static void paint_control(HDC dc, const control *c)
{
    const int mapping = g_map_state != -2;
    const int accent = mapping ? c->target >= 0 && c->target == g_map_state : c->lit;
    const int inactive = mapping && c->target < 0;

    draw_face(dc, &c->rc, 0, accent, inactive);
    if (mapping && c->target >= 0) {
        char text[96];
        binding_label(c->target, text, sizeof text, 0);
        SelectObject(dc, g_small);
        text_block(dc, text, &c->rc);
    } else if (c->kind == CT_KEY) {
        draw_legend(dc, c);
    } else if (c->kind == CT_STICK && c->act != A800_ACT_FIRE) {
        draw_arrow(dc, &c->rc, c->act, accent ? RGB(255, 255, 255)
                                              : GetSysColor(inactive ? COLOR_GRAYTEXT : COLOR_BTNTEXT));
    } else {
        SelectObject(dc, g_font);
        text_line(dc, c->label, &c->rc, DT_CENTER);
    }
}

static void paint_port(HDC dc, int port)
{
    const int dev = g_built_dev[port];
    RECT r = g_port_rc[port], t;
    char text[32];

    DrawEdge(dc, &r, EDGE_ETCHED, BF_RECT);
    snprintf(text, sizeof text, "Port %d", port + 1);
    SetRect(&t, r.left + S(PORT_PAD), r.top + S(PORT_PAD), r.right - S(PORT_PAD), r.top + S(PORT_PAD) + S(TITLE_H));
    SelectObject(dc, g_bold);
    SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
    text_line(dc, text, &t, DT_CENTER);
    SetRect(&t, t.left, t.bottom, t.right, t.bottom + S(DEVICE_H));
    SelectObject(dc, g_font);
    SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
    text_line(dc, a800_device_name(dev), &t, DT_CENTER | DT_END_ELLIPSIS);

    if (dev == A800_DEV_PADDLES) {
        int p;
        SelectObject(dc, g_small);
        for (p = 0; p < 2; p++) {
            snprintf(text, sizeof text, "Paddle %d", port * 2 + p);
            text_line(dc, text, &g_paddle_label_rc[port][p], DT_LEFT);
        }
    } else if (dev != A800_DEV_JOYSTICK && dev != A800_DEV_NONE) {
        text_block(dc, dev == A800_DEV_CX85 ? "Type on the host's numeric keypad."
                                            : "The pointer over the picture drives it.",
                   &g_body_rc[port]);
    }
}

/* One row per gamepad: its name, the port it drives now, and the
 * Automatic / Port 1-4 choice, the current one pushed in. */
static void paint_gamepads(HDC dc)
{
    static const char *const segs[5] = { "Automatic", "Port 1", "Port 2", "Port 3", "Port 4" };
    const int n = a800session_gamepad_count(g_session);
    int i, s;

    SelectObject(dc, g_bold);
    SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
    text_line(dc, "Gamepads", &g_gp_head_rc, DT_LEFT);
    SelectObject(dc, g_font);
    SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
    if (n == 0) {
        text_line(dc, "No gamepads connected", &g_gp_row_rc[0], DT_LEFT);
        return;
    }
    if (n > PAD_ROWS) {
        char more[48];
        snprintf(more, sizeof more, "%d more not shown", n - PAD_ROWS);
        text_line(dc, more, &g_gp_head_rc, DT_RIGHT);
    }
    for (i = 0; i < n && i < PAD_ROWS; i++) {
        const int assign = a800session_gamepad_assignment(g_session, i);
        const int eff = a800session_gamepad_effective_port(g_session, i);
        const int on = assign < 0 || assign >= A800_PORTS ? 0 : assign + 1;
        char name[128], line[32];
        RECT name_rc, eff_rc, seg0;

        gp_name_rect(i, &name_rc);
        segment_rect(i, 0, &seg0);
        a800session_gamepad_name(g_session, i, name, sizeof name);
        SelectObject(dc, g_font);
        SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
        text_line(dc, name, &name_rc, DT_LEFT | DT_END_ELLIPSIS);
        if (eff >= 0) snprintf(line, sizeof line, "drives port %d", eff + 1);
        else snprintf(line, sizeof line, "drives nothing");
        SetRect(&eff_rc, name_rc.right + S(8), name_rc.top, seg0.left - S(8), name_rc.bottom);
        SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
        text_line(dc, line, &eff_rc, DT_RIGHT);
        for (s = 0; s < 5; s++) {
            RECT r;
            segment_rect(i, s, &r);
            draw_face(dc, &r, s == on, 0, 0);
            text_line(dc, segs[s], &r, DT_CENTER);
        }
    }
}

static void paint_panel(HDC dc)
{
    RECT client;
    int i, port;

    GetClientRect(g_panel, &client);
    FillRect(dc, &client, GetSysColorBrush(COLOR_BTNFACE));
    SetBkMode(dc, TRANSPARENT);

    for (port = 0; port < g_built_ports; port++)
        paint_port(dc, port);
    for (i = 0; i < g_nctl; i++)
        paint_control(dc, &g_ctl[i]);
    paint_gamepads(dc);

    SelectObject(dc, g_font);
    draw_face(dc, &g_map_rc, 0, g_map_state != -2, 0);
    text_line(dc, g_map_state == -2 ? "Map" : "Done", &g_map_rc, DT_CENTER);
    draw_face(dc, &g_defaults_rc, 0, 0, 0);
    text_line(dc, "Defaults", &g_defaults_rc, DT_CENTER);
    SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
    text_line(dc, g_hint, &g_hint_rc, DT_LEFT | DT_END_ELLIPSIS);
}

/* ---- polling ---------------------------------------------------------------- */

/* What the Atari sees held, lit. */
static void refresh_held(void)
{
    unsigned held[A800_PORTS];
    int i, port;
    if (!g_panel || g_map_state != -2) return;
    for (port = 0; port < A800_PORTS; port++)
        held[port] = a800session_buttons_held(g_session, port);
    for (i = 0; i < g_nctl; i++) {
        control *c = &g_ctl[i];
        int on;
        if (c->kind == CT_KEY)
            on = a800session_keyboard_held(g_session, c->key) != 0;
        else if (c->kind == CT_STICK)
            on = (held[c->target / A800_ACT_PER_PORT] >> (c->target % A800_ACT_PER_PORT)) & 1;
        else
            on = g_paddle_fire[c->line / 2][c->line % 2];
        if (on != c->lit) {
            c->lit = on;
            InvalidateRect(g_panel, &c->rc, FALSE);
        }
    }
}

/* Hot-plugged pads, auto-assignment changes, and the machine or a port's
 * device changed in Settings. */
static void refresh_pads(void)
{
    pads_state now;
    int i;
    memset(&now, 0, sizeof now);
    now.generation = a800session_gamepad_generation(g_session);
    now.count = a800session_gamepad_count(g_session);
    for (i = 0; i < PAD_ROWS && i < now.count; i++) {
        now.assign[i] = a800session_gamepad_assignment(g_session, i);
        now.effective[i] = a800session_gamepad_effective_port(g_session, i);
    }
    if (memcmp(&now, &g_pads_shown, sizeof now) != 0) {
        g_pads_shown = now;
        InvalidateRect(g_panel, &g_gp_rc, FALSE);
    }
    if (layout_changed())
        rebuild();
}

/* ---- keys ------------------------------------------------------------------- */

static void hide_panel(void)
{
    set_map_state(-2);
    mouse_release();
    if (GetCapture() == g_panel) ReleaseCapture();
    g_swallow_vk = 0;
    ShowWindow(g_panel, SW_HIDE);
}

static int is_modifier(WPARAM vk)
{
    return vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU
        || vk == VK_LWIN || vk == VK_RWIN;
}

/* A key with the panel focused: 1 if taken. */
static int panel_key(UINT msg, WPARAM vk, LPARAM lp)
{
    const int down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
    uint32_t ks;
    int sa;

    if (g_swallow_vk && vk == g_swallow_vk) {
        if (!down) g_swallow_vk = 0;
        return 1;
    }
    if (vk == VK_F9) {
        if (down && !(lp & (1 << 30))) hide_panel();
        return 1;
    }
    /* Map: the next key is the binding (not F11/F12, the app's) */
    if (g_map_state >= 0 && down && vk != VK_F11 && vk != VK_F12) {
        char stolen[128], name[32];
        const int target = g_map_state;
        ks = a800_keysym_from_msg(vk, lp);
        if (!ks) return 1;          /* nothing to call it: keep waiting */
        stolen[0] = '\0';
        a800session_binding_set_key(g_session, target, ks, stolen, sizeof stolen);
        a800session_keysym_name(ks, name, sizeof name);
        g_swallow_vk = vk;
        set_map_state(-1);          /* stay armed: remapping several in a row is normal */
        bound_hint(name, stolen);
        return 1;
    }
    if (vk == VK_PAUSE || vk == VK_CANCEL)
        return a800_forward_key(g_session, msg, vk, lp);     /* BREAK */
    if (a800_app_key(vk)) {
        if (!down) a800_forward_key(g_session, msg, vk, lp);   /* let go of it, whatever it was */
        /* the panel's own ALT+F4 and ALT+Space; bare modifiers */
        if (is_modifier(vk) || ((msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP) && (vk == VK_F4 || vk == VK_SPACE)))
            return 0;
        /* the main window's shortcuts, as if it had the focus */
        if (g_parent) SendMessageA(g_parent, msg, vk, lp);
        return 1;
    }
    ks = a800_keysym_from_msg(vk, lp);
    if (down && ks && (sa = a800session_key_sysaction(g_session, ks)) >= 0) {
        /* the main window drains these */
        if (!(lp & (1 << 30))) a800session_sysaction_post(g_session, sa);
        return 1;
    }
    return a800_forward_key(g_session, msg, vk, lp);
}

static void tip_text(int id, char *out, int outsz)
{
    const control *c;
    out[0] = '\0';
    if (id >= TIP_PAD_ROW) {
        char name[128];
        if (id - TIP_PAD_ROW < a800session_gamepad_count(g_session)
            && a800session_gamepad_name(g_session, id - TIP_PAD_ROW, name, sizeof name) > 0)
            to_acp(name, out, outsz);
        return;
    }
    if (id < 0 || id >= g_nctl) return;
    c = &g_ctl[id];
    if (g_map_state != -2) {
        if (c->target >= 0) binding_label(c->target, out, outsz, 1);
    } else if (c->kind == CT_KEY && (!strcmp(c->label, "SHIFT") || !strcmp(c->label, "CTRL"))) {
        snprintf(out, (size_t)outsz, "Latches for the next key");
    } else if (c->kind == CT_STICK) {
        snprintf(out, (size_t)outsz, "%s", a800_target_name(c->target));
    }
}

/* Exactly the laid-out client size, whatever the frame is at this DPI. */
static void fit_client(void)
{
    RECT wr, cr;
    GetWindowRect(g_panel, &wr);
    GetClientRect(g_panel, &cr);
    if (cr.right == g_total_w && cr.bottom == g_total_h) return;
    SetWindowPos(g_panel, NULL, 0, 0, (wr.right - wr.left) - cr.right + g_total_w,
                 (wr.bottom - wr.top) - cr.bottom + g_total_h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

/* The window's outer size for the laid-out client. */
static void window_size(HWND hwnd, SIZE *sz)
{
    RECT wr, cr;
    if (hwnd && GetWindowRect(hwnd, &wr) && GetClientRect(hwnd, &cr) && cr.right > 0) {
        sz->cx = (wr.right - wr.left) - cr.right + g_total_w;
        sz->cy = (wr.bottom - wr.top) - cr.bottom + g_total_h;
    } else {
        SetRect(&wr, 0, 0, g_total_w, g_total_h);
        AdjustWindowRectEx(&wr, PANEL_STYLE, FALSE, PANEL_EXSTYLE);
        sz->cx = wr.right - wr.left;
        sz->cy = wr.bottom - wr.top;
    }
}

static void rescale(int dpi)
{
    g_dpi = dpi;
    make_fonts();
    layout_static();
    rebuild();
    if (g_tip) SendMessageA(g_tip, TTM_SETMAXTIPWIDTH, 0, S(320));
}

/* ---- the window ----------------------------------------------------------- */

static LRESULT CALLBACK panel_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        /* Off-screen first: the held highlights repaint at 30 Hz. */
        RECT c;
        HDC mem;
        HBITMAP bmp;
        HGDIOBJ oldbmp, oldfont;
        GetClientRect(hwnd, &c);
        mem = CreateCompatibleDC(dc);
        bmp = CreateCompatibleBitmap(dc, c.right, c.bottom);
        oldbmp = SelectObject(mem, bmp);
        oldfont = SelectObject(mem, g_font);
        paint_panel(mem);
        BitBlt(dc, 0, 0, c.right, c.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldfont);
        SelectObject(mem, oldbmp);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_CTLCOLORSTATIC:     /* the sliders, on the panel's face */
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);

    case WM_LBUTTONDOWN: {
        POINT p;
        int i, row, seg;
        p.x = GET_X_LPARAM(lp);
        p.y = GET_Y_LPARAM(lp);
        SetFocus(hwnd);         /* typing goes to the Atari again after a slider */
        if (PtInRect(&g_map_rc, p)) { set_map_state(g_map_state == -2 ? -1 : -2); return 0; }
        if (PtInRect(&g_defaults_rc, p)) {
            a800session_bindings_reset(g_session);
            if (g_map_state < 0) set_hint("Every key and button is back to its default");
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        for (row = 0; row < PAD_ROWS && row < a800session_gamepad_count(g_session); row++)
            for (seg = 0; seg < 5; seg++) {
                RECT r;
                segment_rect(row, seg, &r);
                if (PtInRect(&r, p)) {
                    /* 0 = automatic (connection order), 1-4 = that port */
                    a800session_gamepad_assign(g_session, row, seg - 1);
                    refresh_pads();
                    InvalidateRect(hwnd, &g_gp_rc, FALSE);
                    return 0;
                }
            }
        for (i = 0; i < g_nctl && !PtInRect(&g_ctl[i].rc, p); i++)
            ;
        if (i >= g_nctl) return 0;
        if (g_map_state != -2) {
            /* armed: picks (or changes) the target */
            if (g_ctl[i].target >= 0) set_map_state(g_ctl[i].target);
            return 0;
        }
        control_press(i);
        SetCapture(hwnd);
        refresh_held();
        return 0;
    }
    case WM_MOUSEMOVE:
        /* dragged off what it holds: let go, as a button does */
        if (g_mouse_ctl >= 0) {
            POINT p;
            p.x = GET_X_LPARAM(lp);
            p.y = GET_Y_LPARAM(lp);
            if (g_mouse_ctl >= g_nctl || !PtInRect(&g_ctl[g_mouse_ctl].rc, p)) {
                mouse_release();
                if (GetCapture() == hwnd) ReleaseCapture();
                refresh_held();
            }
        }
        return 0;
    case WM_LBUTTONUP:
        if (g_mouse_ctl >= 0) {
            mouse_release();
            refresh_held();
        }
        if (GetCapture() == hwnd) ReleaseCapture();
        return 0;
    case WM_CAPTURECHANGED:
        /* lost the mouse some other way (Alt+Tab mid-press): let go */
        if ((HWND)lp != hwnd && g_mouse_ctl >= 0) {
            mouse_release();
            refresh_held();
        }
        return 0;

    case WM_HSCROLL: {
        int port, p;
        for (port = 0; port < A800_PORTS; port++)
            for (p = 0; p < 2; p++)
                if (lp && (HWND)lp == g_slider[port][p]) {
                    const int pos = (int)SendMessageA(g_slider[port][p], TBM_GETPOS, 0, 0);
                    g_paddle_value[port][p] = PADDLE_MAX - pos;
                    paddle_send(port, p);
                    if (LOWORD(wp) == TB_ENDTRACK) SetFocus(hwnd);
                }
        return 0;
    }

    case WM_NOTIFY: {
        NMHDR *h = (NMHDR *)lp;
        if (g_tip && h->hwndFrom == g_tip && h->code == TTN_GETDISPINFOA) {
            NMTTDISPINFOA *di = (NMTTDISPINFOA *)lp;
            tip_text((int)h->idFrom, g_tip_text, sizeof g_tip_text);
            di->lpszText = g_tip_text;
            return 0;
        }
        break;
    }

    case WM_TIMER:
        if (wp == IDT_CAPTURE) {
            int button;
            if (g_map_state < 0) {
                KillTimer(hwnd, IDT_CAPTURE);
            } else if (a800session_gamepad_capture_poll(g_session, &button)) {
                char stolen[128];
                const int target = g_map_state;
                stolen[0] = '\0';
                a800session_binding_set_button(g_session, target, button, stolen, sizeof stolen);
                set_map_state(-1);   /* stay armed: remapping several in a row is normal */
                bound_hint(a800_pad_button_name(button), stolen);
            }
        } else if (wp == IDT_HELD) {
            refresh_held();
        } else if (wp == IDT_PADS) {
            refresh_pads();
        }
        return 0;

    case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_KEYUP: case WM_SYSKEYUP:
        if (panel_key(msg, wp, lp)) return 0;
        break;
    case WM_SYSCHAR:
        /* ALT+letter: the main window's, as when it has the focus */
        if (wp != ' ' && g_parent) { SendMessageA(g_parent, msg, wp, lp); return 0; }
        break;

    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) {
            const HWND other = (HWND)lp;
            mouse_release();
            if (GetCapture() == hwnd) ReleaseCapture();
            g_swallow_vk = 0;       /* its release goes elsewhere */
            /* Leaving the app: nothing typed here stays held (within it,
             * the window that gets the key-up lets go). */
            if (!other || GetWindowThreadProcessId(other, NULL) != GetCurrentThreadId())
                a800session_release_all(g_session);
            refresh_held();
        }
        break;                      /* DefWindowProc gives the panel the focus */
    case WM_SHOWWINDOW:
        if (wp) {
            refresh_held();
            SetTimer(hwnd, IDT_HELD, 33, NULL);
            SetTimer(hwnd, IDT_PADS, 250, NULL);
        } else {
            KillTimer(hwnd, IDT_HELD);
            KillTimer(hwnd, IDT_PADS);
        }
        break;
    case WM_GETMINMAXINFO:
        /* Fixed: the size limits say so too, so neither maximizing nor a
         * compositor (Wine passes them on) makes it any other size. */
        if (g_total_w) {
            MINMAXINFO *mm = (MINMAXINFO *)lp;
            SIZE sz;
            window_size(hwnd, &sz);
            mm->ptMinTrackSize.x = mm->ptMaxTrackSize.x = mm->ptMaxSize.x = sz.cx;
            mm->ptMinTrackSize.y = mm->ptMaxTrackSize.y = mm->ptMaxSize.y = sz.cy;
            return 0;
        }
        break;
    case WM_DPICHANGED: {
        const RECT *r = (const RECT *)lp;
        rescale(HIWORD(wp));
        SetWindowPos(hwnd, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        fit_client();
        return 0;
    }
    case WM_CLOSE:
        /* Hide, do not destroy: a remap and the position survive closing it. */
        hide_panel();
        return 0;
    case WM_DESTROY:
        /* with the main window: the next toggle starts afresh */
        mouse_release();
        set_map_state(-2);
        g_panel = NULL;
        g_tip = NULL;
        g_nctl = g_ntips = 0;
        memset(g_slider, 0, sizeof g_slider);
        g_built_machine = -1;
        break;
    default:
        break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static int create_panel(void)
{
    static int registered;
    HINSTANCE inst = GetModuleHandleA(NULL);
    INITCOMMONCONTROLSEX icc;
    RECT want;
    int dpi;

    icc.dwSize = sizeof icc;
    icc.dwICC = ICC_BAR_CLASSES;    /* the sliders and the tooltips */
    InitCommonControlsEx(&icc);
    if (!registered) {
        WNDCLASSEXA wc;
        memset(&wc, 0, sizeof wc);
        wc.cbSize = sizeof wc;
        wc.lpfnWndProc = panel_proc;    /* no CS_DBLCLKS: fast clicks stay press/release pairs */
        wc.hInstance = inst;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = PANEL_CLASS;
        registered = RegisterClassExA(&wc) != 0;
    }
    if (!g_accent_brush)
        g_accent_brush = CreateSolidBrush(RGB((A800SESSION_ACCENT_RGB >> 16) & 0xff,
                                              (A800SESSION_ACCENT_RGB >> 8) & 0xff,
                                              A800SESSION_ACCENT_RGB & 0xff));
    g_dpi = window_dpi(g_parent);
    make_fonts();
    layout_static();

    SetRect(&want, 0, 0, g_total_w, g_total_h);
    /* A tool window with a caption and no thick frame or maximize box: it
     * is exactly the size of its contents, and stays off the taskbar. */
    AdjustWindowRectEx(&want, PANEL_STYLE, FALSE, PANEL_EXSTYLE);
    g_panel = CreateWindowExA(PANEL_EXSTYLE, PANEL_CLASS, "Controllers", PANEL_STYLE,
                              CW_USEDEFAULT, CW_USEDEFAULT, want.right - want.left, want.bottom - want.top,
                              g_parent, NULL, inst, NULL);
    if (!g_panel) return 0;
    g_tip = CreateWindowExA(WS_EX_TOPMOST, TOOLTIPS_CLASSA, NULL, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                            g_panel, NULL, inst, NULL);
    dpi = window_dpi(g_panel);      /* placed on another monitor than the main window */
    rescale(dpi);
    fit_client();
    memset(&g_pads_shown, 0, sizeof g_pads_shown);
    set_map_state(-2);
    return 1;
}

static void show_panel(void)
{
    if (layout_changed())
        rebuild();
    refresh_pads();
    ShowWindow(g_panel, SW_SHOW);
    SetForegroundWindow(g_panel);
}

void a800_controller_window_toggle(HWND parent, a800session *session)
{
    g_session = session;
    if (parent)
        g_parent = parent;
    if (!g_panel && !create_panel())
        return;
    if (IsWindowVisible(g_panel))
        hide_panel();
    else
        show_panel();
}

int a800_controller_window_visible(void)
{
    return g_panel && IsWindowVisible(g_panel);
}

void a800_controller_window_refresh(void)
{
    if (!g_panel)
        return;
    if (IsWindowVisible(g_panel)) {
        refresh_pads();             /* rebuilds if the machine or a device changed */
        InvalidateRect(g_panel, NULL, FALSE);
    } else {
        g_built_machine = -1;       /* rebuilt when next shown */
    }
}

int a800_controller_pretranslate(MSG *msg)
{
    /* A slider has the focus after a drag: its keys are the panel's (they
     * type on the Atari), not the slider's. */
    if (!g_panel || !msg->hwnd || msg->hwnd == g_panel || !IsChild(g_panel, msg->hwnd))
        return 0;
    switch (msg->message) {
    case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP:
        SendMessageA(g_panel, msg->message, msg->wParam, msg->lParam);
        return 1;
    default:
        return 0;
    }
}
