/*
 * The Win32 Controllers window (F9): what is plugged into the Atari's ports
 * drawn as a player sees it, from the session's layout (a800session_controls_
 * layout), so every frontend shows the same parts.
 *
 *   JOYSTICK  the CX40: a black textured square with a red fire button in a
 *             recessed teardrop at the top left, a ribbed rubber boot in a ring
 *             of tick marks and an octagonal shaft cap. The cap follows the
 *             pointer while you drag it, and what drives the machine (keys, a
 *             gamepad) the rest of the time. The same drawing as the Atari
 *             2600's.
 *   PADDLES   the CX30 pair: a black wedge each with a knurled knob you turn,
 *             a red button on the left edge and two turn arrows.
 *   MICE      the Atari ST and Amiga mice with a mat to drag on, the CX22
 *             Trak-Ball with its ball, the KoalaPad and the Touch Tablet with
 *             their pads (a touch is a point of the pad), the CX75 light pen
 *             and XG-1 light gun (aimed over the main picture), the CX85 keypad.
 *
 * Under them the console keys (START, SELECT, OPTION, HELP on an XL/XE, RESET,
 * BREAK) and the Atari keyboard of the machine: the 400's membrane, the 800's
 * brown keys, the XL/XE's charcoal ones (HELP a round key there), the 1200XL's
 * with F1-F4. Then the gamepads with the port each drives, and Map mode. The
 * machine and what is in each port come from the session; the window follows
 * them (a 400/800 shows four ports, an XL/XE two).
 *
 * MOUSE: a press on a button, a key or a turn arrow holds it; a press on the
 * stick follows the pointer (a800session_stick_from_vector) and lets go on
 * release; a press on a knob turns it to the pointer's angle (it stays where it
 * was left) and the wheel nudges it; a mouse mat and the Trak-Ball turn drag
 * into motion; a pad sends the point touched. Presses are the raw button-down
 * and button-up, never a click: the machine reads its controllers once a frame.
 * The mouse is captured on press; letting go anywhere, losing the capture,
 * hiding or deactivating the window releases whatever the pointer holds.
 *
 * What the machine sees held -- keyboard, gamepads and this window together
 * (a800session_buttons_held, console_held, keyboard_held) -- is polled every
 * 40 ms and painted in orange (#FFB04A), so the window doubles as a gamepad
 * tester.
 *
 * KEYS: a key typed in this window plays on the Atari exactly as typed in the
 * main window; only Map mode keeps keys for itself.
 *
 * MAP MODE: Map arms a two-step rebind -- click a part to pick its target (the
 * stick: the side of the ring), or pick any of the 30 targets from the list;
 * then press a host key OR a gamepad button; Esc cancels. While armed the parts
 * show what drives them and typing does not reach the machine; rebinding steals
 * the key from whatever held it. Clear takes the picked target's key and
 * gamepad button away; Show bindings prints the same legends outside Map mode.
 * F9, F11 and F12 cannot be bound. The paddles are driven by their knobs and
 * turn arrows, not by bindings.
 *
 * FIXED SIZE: a caption-only tool window with no thick frame and no maximize
 * box. It is as large as its contents: two or four ports, the keyboard when
 * shown, the gamepad rows and the Map row.
 *
 * The art is drawn at twice the size into a memory bitmap and shrunk with
 * HALFTONE (GDI has no anti-aliasing of its own); the text goes on after at the
 * window's own size.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include "controller_window.h"

/* GET_X_LPARAM / GET_Y_LPARAM live here, not in windows.h. */
#include <windowsx.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../help/help_window.h"
#include "../key_forward.h"
#include "../resource.h"

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

#define PANEL_CLASS   "FujiNetGoAtari800Controllers"
#define PANEL_STYLE   (WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN)
#define PANEL_EXSTYLE WS_EX_TOOLWINDOW

/* Sizes at 96 dpi; SCL() scales them to the window's. */
#define UNIT96      19      /* pixels per layout unit of the controllers */
#define MARGIN      12
#define CASE_PAD    12
#define BRAND_H     18      /* the strip along the top of the mat */
#define KBD_PAD     10
#define HEAD_H      22
#define NAME_H      26      /* a gamepad row: its name ... */
#define SEG_H       24      /* ... and its Automatic / Port line follow ... */
#define ROW_GAP      4
#define PAD_ROWS     4      /* four ports: up to four pads */
#define MAX_SEGS     5      /* Automatic, Port 1-4 */
#define BTN_H       28
#define PI_DEG 3.14159265358979323846
#define PADDLE_STEP 8       /* a wheel notch of a knob, of 228 */

#define IDT_CAPTURE 1
#define IDT_HELD    2
#define IDT_PADS    3

#define HX(v) RGB(((v) >> 16) & 0xFF, ((v) >> 8) & 0xFF, (v) & 0xFF)

/* The buttons under the pads: the Map row. */
enum { SB_MAP, SB_CLEAR, SB_DEFAULTS, SB_SHOW, SB_KEYBOARD, SB_HELP, SB_COUNT };

typedef struct {
    unsigned generation;
    int ports, devices[A800_PORTS];
    int count, assign[PAD_ROWS], effective[PAD_ROWS];
} pads_state;

#define MAX_KEYS 128

typedef struct {
    unsigned held[A800_PORTS];                  /* bit per a800_action, merged, per port */
    unsigned console;                           /* bit per a800_console */
    int paddle[A800_PORTS][2];                  /* a knob's position (228 left .. 0 right) */
    unsigned char key[MAX_KEYS];                /* a cap held now (or latched) */
} held_state;

static HWND g_panel, g_parent, g_combo;
static a800session *g_session;
static int g_dpi = 96;

/* Map mode: -2 idle, -1 armed and waiting for a target, >= 0 waiting for a
 * key or gamepad button to bind to that target. */
static int g_map_state = -2;
static int g_show_bindings;
static int g_show_keyboard = 1;
static WPARAM g_swallow_vk;                     /* the key Map took: its repeats and release too */
static char g_hint[300];
static int g_hover_part = -1;                   /* the part under the idle pointer: its bindings are the hint */
static int g_hover_key = -1;
static int g_tracking;

/* What the controllers layout was built for. */
static a800_control g_layout[A800_CONTROLS_MAX];
static int g_numlayout;
static float g_layw, g_layh;
static double g_unit, g_orgx, g_orgy;
static int g_machine = -1, g_nports, g_xl, g_family;
static int g_dev[A800_PORTS];

/* The keyboard table, and where it sits. */
static const a800_kbd_key *g_keys;
static int g_nkeys;
static float g_kbd_w, g_kbd_h;                  /* the table's extent, key units */
static double g_kunit, g_korgx, g_korgy;

/* What the pointer holds. */
static int g_mouse_part = -1;                   /* a pressed button / key / arrow (layout index) */
static int g_mouse_stick = -1;                  /* a stick drag: the layout index */
static int g_mouse_knob = -1;                   /* a knob drag: the layout index */
static int g_mouse_mat = -1;                    /* a mat or ball drag: the layout index */
static int g_mouse_pad = -1;                    /* a touch on a pad: the layout index */
static int g_mouse_key = -1;                    /* a keyboard cap held: the table index */
static POINT g_mat_last;
static double g_mat_carry_x, g_mat_carry_y;
static unsigned g_ptr_mask;                     /* the buttons of the pointer device this window holds */
static int g_ptr_port = -1;                     /* the port whose button was pressed: only its parts light */
static double g_stick_dx, g_stick_dy;           /* the dragged cap's offset, -1..1 of the reach */

/* What was last painted as held. */
static held_state g_shown;
static pads_state g_pads_shown;

static int g_total_w, g_total_h;
static RECT g_case_rc, g_brand_rc, g_kbd_rc, g_gp_rc, g_gp_head_rc, g_gp_row_rc[PAD_ROWS];
static RECT g_btn_rc[SB_COUNT], g_combo_rc, g_hint_rc, g_art_rc;

static HFONT g_font, g_bold, g_legend, g_tiny, g_brand, g_caption, g_key, g_keysm, g_sym, g_logo;

static void refresh_held(void);
static void refresh_pads(void);

/* ---- DPI and fonts ------------------------------------------------------------ */

static int SCL(int val) { return MulDiv(val, g_dpi, 96); }

static int system_dpi(void)
{
    HDC hdc = GetDC(NULL);
    int dpi = hdc ? GetDeviceCaps(hdc, LOGPIXELSY) : 96;
    if (hdc) ReleaseDC(NULL, hdc);
    return dpi > 0 ? dpi : 96;
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
        UINT dpi = fn(hwnd);
        if (dpi) return (int)dpi;
    }
    return system_dpi();
}

static void free_fonts(void)
{
    HFONT *const slots[] = { &g_font, &g_bold, &g_legend, &g_tiny, &g_brand, &g_caption, &g_key, &g_keysm, &g_sym,
                             &g_logo };
    size_t idx;
    for (idx = 0; idx < sizeof slots / sizeof slots[0]; idx++)
        if (*slots[idx]) { DeleteObject(*slots[idx]); *slots[idx] = NULL; }
}

static HFONT font_like(const LOGFONTA *base, int px, int weight, const char *face)
{
    LOGFONTA face_lf = *base;
    face_lf.lfHeight = -px;
    face_lf.lfWidth = 0;
    face_lf.lfWeight = weight;
    if (face) snprintf(face_lf.lfFaceName, sizeof face_lf.lfFaceName, "%s", face);
    return CreateFontIndirectA(&face_lf);
}

static int font_has(HDC hdc, HFONT font, const wchar_t *wide, int num)
{
    WORD idx[32];
    HGDIOBJ old;
    int pos, ok = 1;
    if (!font || num <= 0 || num > 32) return 0;
    old = SelectObject(hdc, font);
    if (GetGlyphIndicesW(hdc, wide, num, idx, GGI_MARK_NONEXISTING_GLYPHS) == GDI_ERROR) ok = 0;
    for (pos = 0; ok && pos < num; pos++)
        if (idx[pos] == 0xffff) ok = 0;
    SelectObject(hdc, old);
    return ok;
}

/* The message font (system metrics are at the system DPI), bold, and the key
 * caps' sizes. The Atari key's legend is a symbol the UI font may lack: the
 * first symbol face that has it draws it. */
static void make_fonts(void)
{
    static const char *const symbol_faces[] = { "Segoe UI Symbol", "DejaVu Sans" };
    NONCLIENTMETRICSA ncm;
    LOGFONTA base;
    HDC hdc;
    int px;
    size_t idx;

    free_fonts();
    memset(&ncm, 0, sizeof ncm);
    ncm.cbSize = sizeof ncm;
    if (SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0)) base = ncm.lfMessageFont;
    else GetObjectA(GetStockObject(DEFAULT_GUI_FONT), sizeof base, &base);
    px = MulDiv(abs(base.lfHeight ? base.lfHeight : 12), g_dpi, system_dpi());

    g_font = font_like(&base, px, FW_NORMAL, NULL);
    g_bold = font_like(&base, px, FW_BOLD, NULL);
    g_legend = font_like(&base, SCL(9), FW_BOLD, NULL);
    g_tiny = font_like(&base, SCL(9), FW_NORMAL, NULL);
    g_brand = font_like(&base, SCL(12), FW_BOLD, NULL);
    g_caption = font_like(&base, SCL(12), FW_BOLD, NULL);
    g_logo = font_like(&base, SCL(15), FW_BOLD, NULL);
    g_key = font_like(&base, SCL(11), FW_BOLD, NULL);
    g_keysm = font_like(&base, SCL(9), FW_NORMAL, NULL);

    hdc = GetDC(NULL);
    for (idx = 0; idx < sizeof symbol_faces / sizeof symbol_faces[0] && !g_sym; idx++) {
        HFONT face = font_like(&base, SCL(15), FW_NORMAL, symbol_faces[idx]);
        if (font_has(hdc, face, L"\x25d0", 1)) g_sym = face;
        else if (face) DeleteObject(face);
    }
    ReleaseDC(NULL, hdc);
}

/* ---- text -------------------------------------------------------------------- */

/* UTF-8 (key legends, gamepad names) through DrawTextW. */
static int text_out(HDC hdc, const char *utf8, RECT *rect, UINT fmt)
{
    wchar_t wide[256];
    if (!utf8 || !*utf8) return 0;
    if (MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, (int)(sizeof wide / sizeof wide[0])) <= 0) return 0;
    return DrawTextW(hdc, wide, -1, rect, fmt | DT_NOPREFIX);
}

static void text_line(HDC hdc, const char *utf8, const RECT *rect, UINT fmt)
{
    RECT copy = *rect;
    text_out(hdc, utf8, &copy, fmt | DT_SINGLELINE | DT_VCENTER);
}

/* The colours live here and only here; the geometry is the core's. From the
 * photos of the real controllers, console and keyboards (DESIGN-atari.md). */
#define COL_HELD_RING  HX(0xFFB04A)             /* the orange of a held part */
#define COL_HELD_FILL  HX(0xFFD9A0)
#define COL_ON_HELD    HX(0x1C1C1C)
#define COL_MAT_TOP    HX(0x8E897C)             /* the desk: light enough that black plastic reads */
#define COL_MAT_BOT    HX(0x6B675C)
#define COL_MAT_EDGE   HX(0x3D3A33)
#define COL_BRAND_INK  HX(0xF6F2E4)
#define COL_SHADOW     HX(0x403D36)
#define COL_CAPTION    HX(0xFBF8EC)
#define COL_DIM_TO     HX(0x7C776B)             /* unpicked parts fade toward the mat */
#define COL_PLASTIC    HX(0x1C1C1C)
#define COL_PLASTIC_HI HX(0x2A2A2A)
#define COL_PLASTIC_LO HX(0x101010)
#define COL_FIRE       HX(0xE23B1F)
#define COL_FIRE_HI    HX(0xF05A38)
#define COL_FIRE_DOWN  HX(0xB02A14)
#define COL_ORANGE     HX(0xE58A1C)
#define COL_PANEL      HX(0x151515)
#define NOFILL         ((COLORREF)0xFFFFFFFFu)

/* ---- art: GDI at a supersampled scale ------------------------------------------- */

/* One art pass draws the client rectangle with its top-left at (g_art_x,
 * g_art_y) into a bitmap g_art_k times the size and g_art_rows rows tall. */
static int g_art_k = 1, g_art_x, g_art_y, g_art_rows;

static int art_xpos(double val) { return (int)floor((val - g_art_x) * g_art_k + 0.5); }
static int art_ypos(double val) { return (int)floor((val - g_art_y) * g_art_k + 0.5); }
static int art_len(double val) { return (int)floor(val * g_art_k + 0.5); }

static COLORREF mix_color(COLORREF start, COLORREF finish, double frac)
{
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    return RGB((int)(GetRValue(start) + (GetRValue(finish) - GetRValue(start)) * frac + 0.5),
               (int)(GetGValue(start) + (GetGValue(finish) - GetGValue(start)) * frac + 0.5),
               (int)(GetBValue(start) + (GetBValue(finish) - GetBValue(start)) * frac + 0.5));
}

/* An unpicked part in Map mode: 40% of its own colours over the mat's. */
static COLORREF tone(COLORREF col, int dim)
{
    return dim ? mix_color(col, COL_DIM_TO, 0.6) : col;
}

static POINT apt(double xpos, double ypos)
{
    POINT pnt;
    pnt.x = art_xpos(xpos);
    pnt.y = art_ypos(ypos);
    return pnt;
}

static void poly(HDC hdc, const POINT *pnts, int num, COLORREF fill, COLORREF line, double width)
{
    HBRUSH brush = fill == NOFILL ? (HBRUSH)GetStockObject(NULL_BRUSH) : CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, width > 0 ? art_len(width) : 0, line);
    HGDIOBJ oldb = SelectObject(hdc, brush);
    HGDIOBJ oldp = SelectObject(hdc, width > 0 ? (HGDIOBJ)pen : GetStockObject(NULL_PEN));
    Polygon(hdc, pnts, num);
    SelectObject(hdc, oldp);
    SelectObject(hdc, oldb);
    DeleteObject(pen);
    if (fill != NOFILL) DeleteObject(brush);
}

static void ellipse(HDC hdc, double ctrx, double ctry, double radx, double rady, COLORREF fill, COLORREF line,
                    double width)
{
    HBRUSH brush = fill == NOFILL ? (HBRUSH)GetStockObject(NULL_BRUSH) : CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, width > 0 ? art_len(width) : 0, line);
    HGDIOBJ oldb = SelectObject(hdc, brush);
    HGDIOBJ oldp = SelectObject(hdc, width > 0 ? (HGDIOBJ)pen : GetStockObject(NULL_PEN));
    Ellipse(hdc, art_xpos(ctrx - radx), art_ypos(ctry - rady), art_xpos(ctrx + radx), art_ypos(ctry + rady));
    SelectObject(hdc, oldp);
    SelectObject(hdc, oldb);
    DeleteObject(pen);
    if (fill != NOFILL) DeleteObject(brush);
}

static void rrect(HDC hdc, double xpos, double ypos, double width, double height, double radius, COLORREF fill,
                  COLORREF line, double linew)
{
    HBRUSH brush = fill == NOFILL ? (HBRUSH)GetStockObject(NULL_BRUSH) : CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, linew > 0 ? art_len(linew) : 0, line);
    HGDIOBJ oldb = SelectObject(hdc, brush);
    HGDIOBJ oldp = SelectObject(hdc, linew > 0 ? (HGDIOBJ)pen : GetStockObject(NULL_PEN));
    RoundRect(hdc, art_xpos(xpos), art_ypos(ypos), art_xpos(xpos + width), art_ypos(ypos + height),
              art_len(radius * 2), art_len(radius * 2));
    SelectObject(hdc, oldp);
    SelectObject(hdc, oldb);
    DeleteObject(pen);
    if (fill != NOFILL) DeleteObject(brush);
}

static void line_seg(HDC hdc, double x0, double y0, double x1, double y1, COLORREF col, double width)
{
    HPEN pen = CreatePen(PS_SOLID, art_len(width) > 0 ? art_len(width) : 1, col);
    HGDIOBJ oldp = SelectObject(hdc, pen);
    MoveToEx(hdc, art_xpos(x0), art_ypos(y0), NULL);
    LineTo(hdc, art_xpos(x1), art_ypos(y1));
    SelectObject(hdc, oldp);
    DeleteObject(pen);
}

/* A rounded rectangle filled with a vertical gradient, then outlined. */
static void grad_rrect(HDC hdc, double xpos, double ypos, double width, double height, double radius,
                       COLORREF top, COLORREF bottom, COLORREF edge, double edgew)
{
    const int left = art_xpos(xpos), right = art_xpos(xpos + width);
    const int first = art_ypos(ypos), last = art_ypos(ypos + height);
    HRGN clip = CreateRoundRectRgn(left, first, right + 1, last + 1, art_len(radius * 2), art_len(radius * 2));
    int row;
    HGDIOBJ oldb;
    SelectClipRgn(hdc, clip);
    oldb = SelectObject(hdc, GetStockObject(DC_BRUSH));
    for (row = first < 0 ? 0 : first; row <= last && row < g_art_rows; row++) {
        SetDCBrushColor(hdc, mix_color(top, bottom, last > first ? (double)(row - first) / (last - first) : 0));
        PatBlt(hdc, left, row, right - left + 1, 1, PATCOPY);
    }
    SelectObject(hdc, oldb);
    SelectClipRgn(hdc, NULL);
    DeleteObject(clip);
    if (edgew > 0) rrect(hdc, xpos, ypos, width, height, radius, NOFILL, edge, edgew);
}

/* The pixel rectangle of a layout part. */
static void part_box(const a800_control *ctl, double *xpos, double *ypos, double *width, double *height)
{
    *xpos = g_orgx + (double)ctl->x * g_unit;
    *ypos = g_orgy + (double)ctl->y * g_unit;
    *width = (double)ctl->w * g_unit;
    *height = (double)ctl->h * g_unit;
}

static void part_rect(const a800_control *ctl, RECT *rect)
{
    double xpos, ypos, width, height;
    part_box(ctl, &xpos, &ypos, &width, &height);
    SetRect(rect, (int)floor(xpos), (int)floor(ypos), (int)ceil(xpos + width), (int)ceil(ypos + height));
}

static int binding_mode(void)
{
    return g_map_state != -2 || g_show_bindings;
}

/* ---- which parts are lit ------------------------------------------------------- */

static int part_index(const a800_control *ctl)
{
    return (int)(ctl - g_layout);
}

static int pressable(const a800_control *ctl)
{
    return ctl->kind != A800_CTL_BODY && ctl->kind != A800_CTL_CAPTION && ctl->kind != A800_CTL_NOTE;
}

/* The port a target belongs to, or -1 for the console's and the system's. */
static int target_port(int target)
{
    return target >= 0 && target < A800_PORTS * A800_ACT_PER_PORT ? target / A800_ACT_PER_PORT : -1;
}

/* Whether the machine sees a part held (from any source). */
static int part_held(const a800_control *ctl)
{
    const int port = ctl->port;
    switch (ctl->kind) {
    case A800_CTL_STICK:
        return (g_shown.held[port] & ((1u << A800_ACT_UP) | (1u << A800_ACT_DOWN) | (1u << A800_ACT_LEFT) |
                                      (1u << A800_ACT_RIGHT))) != 0 || g_mouse_stick == part_index(ctl);
    case A800_CTL_ROUND_BUTTON:
        return port >= 0 && ((g_shown.held[port] >> A800_ACT_FIRE) & 1u) != 0;
    case A800_CTL_CONSOLE_KEY:
        return ctl->target >= A800_TARGET_CONSOLE(0) &&
               ((g_shown.console >> (ctl->target - A800_TARGET_CONSOLE(0))) & 1u) != 0;
    case A800_CTL_KNOB:
        return g_mouse_knob == part_index(ctl);
    case A800_CTL_BALL:
    case A800_CTL_MAT:
        return g_mouse_mat == part_index(ctl);
    case A800_CTL_PAD:
        return g_mouse_pad == part_index(ctl);
    case A800_CTL_POINTER_BUTTON:
        return (g_ptr_mask & (unsigned)ctl->arg) != 0 && ctl->port == g_ptr_port;
    default:
        return g_mouse_part == part_index(ctl);
    }
}

/* Whether a part is shown lit: what the machine sees, in orange. In Map mode
 * the picked target is the one lit. */
static int part_lit(const a800_control *ctl)
{
    if (g_map_state != -2) {
        if (g_map_state < 0) return 0;
        switch (ctl->kind) {
        case A800_CTL_ROUND_BUTTON:
        case A800_CTL_CONSOLE_KEY: return ctl->target == g_map_state;
        case A800_CTL_STICK:
            return target_port(g_map_state) == ctl->port && g_map_state % A800_ACT_PER_PORT <= A800_ACT_RIGHT;
        default: return 0;
        }
    }
    return part_held(ctl) != 0;
}

/* Map mode with a target picked: every other part is drawn at 40%. */
static int part_dim(const a800_control *ctl)
{
    if (g_map_state < 0) return 0;
    if (!pressable(ctl)) return 0;
    return !part_lit(ctl);
}

/* A device's case in Map mode with a target picked elsewhere. */
static int body_dim(int port)
{
    return g_map_state >= 0 && target_port(g_map_state) != port;
}

/* ---- the CX40 joystick ----------------------------------------------------------- */

/* The cap's offset from rest, -1..1 of the reach: the pointer while dragging,
 * else what is held (diagonals normalised so the cap stays inside the boot). */
static void stick_offset(int port, double *offx, double *offy)
{
    int dirx, diry;
    if (g_mouse_stick >= 0 && g_layout[g_mouse_stick].port == port) { *offx = g_stick_dx; *offy = g_stick_dy; return; }
    dirx = (int)((g_shown.held[port] >> A800_ACT_RIGHT) & 1u) - (int)((g_shown.held[port] >> A800_ACT_LEFT) & 1u);
    diry = (int)((g_shown.held[port] >> A800_ACT_DOWN) & 1u) - (int)((g_shown.held[port] >> A800_ACT_UP) & 1u);
    *offx = dirx;
    *offy = diry;
    if (dirx && diry) { *offx *= 0.7071; *offy *= 0.7071; }
}

/* The angle (degrees clockwise from straight up) the held directions point
 * at, if any. */
static int held_angle(int port, double *ang)
{
    const unsigned held = g_shown.held[port];
    const int dirx = (int)((held >> A800_ACT_RIGHT) & 1u) - (int)((held >> A800_ACT_LEFT) & 1u);
    const int diry = (int)((held >> A800_ACT_DOWN) & 1u) - (int)((held >> A800_ACT_UP) & 1u);
    if (!dirx && !diry) return 0;
    *ang = atan2((double)dirx, (double)-diry) * 180.0 / PI_DEG;
    return 1;
}

static double angle_between(double one, double two)
{
    double delta = fmod(one - two, 360.0);
    if (delta > 180) delta -= 360;
    if (delta < -180) delta += 360;
    return fabs(delta);
}

/* The case, the fire button's recess, the ring of ticks: 8.0 x 8.0 u. */
static void art_joy_body(HDC hdc, const a800_control *ctl)
{
    double bx, by, bw, bh, ang = 0;
    const double u = g_unit;
    const int dim = body_dim(ctl->port), have = held_angle(ctl->port, &ang);
    int idx;
    part_box(ctl, &bx, &by, &bw, &bh);

    rrect(hdc, bx + u * 0.12, by + u * 0.2, bw, bh, u * 0.9, COL_SHADOW, COL_SHADOW, 0);
    rrect(hdc, bx, by, bw, bh, u * 0.9, tone(COL_PLASTIC, dim), tone(COL_PLASTIC_LO, dim), 1.2);
    rrect(hdc, bx + u * 0.25, by + u * 0.25, bw - u * 0.5, bh - u * 0.5, u * 0.7, NOFILL, tone(COL_PLASTIC_HI, dim), 1.0);
    /* the teardrop: the button's recess and its neck to the ring */
    {
        const double fromx = bx + u * 1.7, fromy = by + u * 1.7, tox = bx + u * 4.5, toy = by + u * 4.5;
        const double dirx = (tox - fromx) / hypot(tox - fromx, toy - fromy), diry = (toy - fromy) / hypot(tox - fromx, toy - fromy);
        const double half = u * 0.45;
        POINT neck[4];
        neck[0] = apt(fromx - diry * half, fromy + dirx * half);
        neck[1] = apt(tox - diry * half, toy + dirx * half);
        neck[2] = apt(tox + diry * half, toy - dirx * half);
        neck[3] = apt(fromx + diry * half, fromy - dirx * half);
        poly(hdc, neck, 4, tone(COL_PLASTIC_LO, dim), tone(COL_PLASTIC_LO, dim), 0);
        ellipse(hdc, fromx, fromy, u * 1.1, u * 1.1, tone(COL_PLASTIC_LO, dim), tone(COL_PLASTIC_LO, dim), 0);
    }
    ellipse(hdc, bx + u * 4.5, by + u * 4.5, u * 3.0, u * 3.0, tone(COL_PLASTIC_LO, dim), tone(HX(0x070707), dim), 1.0);
    for (idx = 0; idx < 36; idx++) {    /* the ring's tick marks; a held direction lights its sector */
        const double deg = idx * 10.0, rad = deg * PI_DEG / 180.0;
        const int lit = have && angle_between(deg, ang) <= 22.6;
        line_seg(hdc, bx + u * 4.5 + u * 2.52 * sin(rad), by + u * 4.5 - u * 2.52 * cos(rad),
                 bx + u * 4.5 + u * 2.86 * sin(rad), by + u * 4.5 - u * 2.86 * cos(rad),
                 lit ? COL_HELD_RING : tone(HX(0x3A3A3A), dim), lit ? u * 0.11 : u * 0.07);
    }
}

/* The ribbed boot and the octagonal shaft cap, tilted toward what is held. */
static void art_stick(HDC hdc, const a800_control *ctl)
{
    double bx, by, bw, bh, offx, offy, capx, capy;
    const double u = g_unit;
    const int dim = part_dim(ctl), lit = part_lit(ctl);
    POINT oct[8];
    int idx;
    part_box(ctl, &bx, &by, &bw, &bh);
    stick_offset(ctl->port, &offx, &offy);
    {
        const double len = sqrt(offx * offx + offy * offy);
        if (len > 1.0) { offx /= len; offy /= len; }
    }
    capx = bx + bw / 2 + offx * u * 1.0;
    capy = by + bh / 2 + offy * u * 1.0;
    /* the boot's rings squash toward the tilt for depth */
    ellipse(hdc, bx + bw / 2 + offx * u * 0.08, by + bh / 2 + offy * u * 0.08, u * 2.1, u * 2.1, tone(HX(0x151515), dim),
            tone(HX(0x0A0A0A), dim), 1.0);
    ellipse(hdc, bx + bw / 2 + offx * u * 0.2, by + bh / 2 + offy * u * 0.2, u * 1.7, u * 1.7, tone(HX(0x262626), dim),
            tone(HX(0x151515), dim), 1.0);
    ellipse(hdc, bx + bw / 2 + offx * u * 0.35, by + bh / 2 + offy * u * 0.35, u * 1.3, u * 1.3, tone(HX(0x151515), dim),
            tone(HX(0x262626), dim), 1.0);
    for (idx = 0; idx < 8; idx++) {     /* the octagonal shaft cap */
        const double rad = (idx * 45.0 + 22.5) * PI_DEG / 180.0;
        oct[idx] = apt(capx + u * 0.9 * sin(rad), capy - u * 0.9 * cos(rad));
    }
    poly(hdc, oct, 8, tone(HX(0x0C0C0C), dim), tone(COL_PLASTIC_HI, dim), u * 0.1);
    ellipse(hdc, capx - u * 0.15, capy - u * 0.18, u * 0.28, u * 0.2, tone(HX(0x232323), dim), tone(HX(0x232323), dim), 0);
    if (lit && g_map_state == -2)
        ellipse(hdc, capx, capy, u * 1.02, u * 1.02, NOFILL, COL_HELD_RING, u * 0.1);
}

/* The red fire button: a glossy dome in its recess. */
static void art_fire(HDC hdc, const a800_control *ctl)
{
    double bx, by, bw, bh;
    const double u = g_unit;
    const int dim = part_dim(ctl), lit = part_lit(ctl), pressed = lit && g_map_state == -2;
    part_box(ctl, &bx, &by, &bw, &bh);
    ellipse(hdc, bx + bw / 2, by + bh / 2, u * 0.8, u * 0.8, tone(pressed ? COL_FIRE_DOWN : COL_FIRE, dim),
            tone(COL_FIRE_DOWN, dim), 1.0);
    if (!pressed)
        ellipse(hdc, bx + bw / 2 - u * 0.25, by + bh / 2 - u * 0.28, u * 0.3, u * 0.22, tone(COL_FIRE_HI, dim),
                tone(COL_FIRE_HI, dim), 0);
    if (lit)
        ellipse(hdc, bx + bw / 2, by + bh / 2, u * 1.0, u * 1.0, NOFILL, COL_HELD_RING, u * 0.1);
}

/* ---- the CX30 paddle ------------------------------------------------------------- */

/* The wedge body and its label plate with the rackets: 7.0 x 6.0 u. */
static void art_paddle_body(HDC hdc, const a800_control *ctl)
{
    static const double shape[6][2] = { { 1.0, 0.3 }, { 5.6, 0.3 }, { 7.0, 5.0 }, { 6.2, 5.9 }, { 0.8, 5.9 }, { 0.0, 4.6 } };
    double bx, by, bw, bh;
    const double u = g_unit;
    const int dim = body_dim(ctl->port);
    const COLORREF gold = tone(HX(0xDCCB78), dim);
    POINT pts[6], shadow[6];
    int idx;
    part_box(ctl, &bx, &by, &bw, &bh);
    for (idx = 0; idx < 6; idx++) {
        pts[idx] = apt(bx + shape[idx][0] * u, by + shape[idx][1] * u);
        shadow[idx] = apt(bx + shape[idx][0] * u + u * 0.12, by + shape[idx][1] * u + u * 0.2);
    }
    poly(hdc, shadow, 6, COL_SHADOW, COL_SHADOW, 0);
    poly(hdc, pts, 6, tone(HX(0x1F1F1F), dim), tone(HX(0x2E2E2E), dim), u * 0.12);
    /* the label plate */
    rrect(hdc, bx + u * 3.6, by + u * 4.3, u * 2.8, u * 1.3, u * 0.15, tone(COL_PLASTIC_LO, dim), tone(HX(0x8A8A8A), dim), 1.0);
    /* the knob covers the plate's left end, so the picture sits where it stays visible:
     * two crossed rackets and a ball, in yellow */
    line_seg(hdc, bx + u * 4.12, by + u * 5.42, bx + u * 4.5, by + u * 5.0, gold, u * 0.07);
    line_seg(hdc, bx + u * 4.5, by + u * 5.42, bx + u * 4.12, by + u * 5.0, gold, u * 0.07);
    ellipse(hdc, bx + u * 4.1, by + u * 4.96, u * 0.13, u * 0.13, NOFILL, gold, u * 0.05);
    ellipse(hdc, bx + u * 4.52, by + u * 4.96, u * 0.13, u * 0.13, NOFILL, gold, u * 0.05);
}

/* The knurled knob, with the index mark where the pot is. */
static void art_knob(HDC hdc, const a800_control *ctl)
{
    double bx, by, bw, bh, cx, cy, ang;
    const double u = g_unit;
    const int dim = part_dim(ctl), lit = part_lit(ctl);
    int idx;
    part_box(ctl, &bx, &by, &bw, &bh);
    cx = bx + bw / 2;
    cy = by + bh / 2;
    ang = (double)a800session_knob_angle(g_shown.paddle[ctl->port][ctl->arg & 1]) * PI_DEG / 180.0;
    ellipse(hdc, cx + u * 0.08, cy + u * 0.12, u * 2.25, u * 2.25, COL_SHADOW, COL_SHADOW, 0);
    ellipse(hdc, cx, cy, u * 2.2, u * 2.2, tone(HX(0x0E0E0E), dim), tone(HX(0x2A2A2A), dim), 1.0);
    for (idx = 0; idx < 60; idx++) {    /* the knurled rim */
        const double rad = idx * 6.0 * PI_DEG / 180.0;
        line_seg(hdc, cx + u * 2.0 * sin(rad), cy - u * 2.0 * cos(rad), cx + u * 2.2 * sin(rad), cy - u * 2.2 * cos(rad),
                 tone(HX(0x2A2A2A), dim), u * 0.05);
    }
    ellipse(hdc, cx, cy, u * 1.75, u * 1.75, tone(HX(0x1A1A1A), dim), tone(HX(0x2A2A2A), dim), 1.0);
    ellipse(hdc, cx, cy, u * 1.65, u * 1.65, tone(HX(0x232323), dim), tone(HX(0x2E2E2E), dim), 1.0);
    line_seg(hdc, cx + u * 0.9 * sin(ang), cy - u * 0.9 * cos(ang), cx + u * 1.55 * sin(ang), cy - u * 1.55 * cos(ang),
             tone(HX(0xD8D8D8), dim), u * 0.13);
    if (lit && g_map_state == -2)
        ellipse(hdc, cx, cy, u * 2.34, u * 2.34, NOFILL, COL_HELD_RING, u * 0.1);
}

/* The red button on the paddle's left edge. */
static void art_edge_button(HDC hdc, const a800_control *ctl)
{
    double bx, by, bw, bh;
    const double u = g_unit;
    const int dim = part_dim(ctl), lit = part_lit(ctl), pressed = lit && g_map_state == -2;
    part_box(ctl, &bx, &by, &bw, &bh);
    rrect(hdc, bx, by + (pressed ? u * 0.06 : 0), bw, bh - (pressed ? u * 0.04 : 0), u * 0.14,
          tone(pressed ? COL_FIRE_DOWN : HX(0xD93A22), dim), tone(COL_FIRE_DOWN, dim), 1.0);
    if (!pressed)
        line_seg(hdc, bx + u * 0.1, by + u * 0.12, bx + bw - u * 0.1, by + u * 0.12, tone(HX(0xE8492C), dim), 1.5);
    if (lit)
        rrect(hdc, bx - u * 0.1, by - u * 0.1, bw + u * 0.2, bh + u * 0.2, u * 0.2, NOFILL, COL_HELD_RING, u * 0.1);
}

/* A turn arrow under the knob: a small triangle, held while turning. */
static void art_turn(HDC hdc, const a800_control *ctl)
{
    double bx, by, bw, bh;
    const double u = g_unit;
    const int dim = part_dim(ctl), lit = part_lit(ctl), right = ctl->dir > 0;
    POINT tri[3];
    part_box(ctl, &bx, &by, &bw, &bh);
    if (right) {
        tri[0] = apt(bx + u * 0.12, by + u * 0.1);
        tri[1] = apt(bx + bw - u * 0.06, by + bh / 2);
        tri[2] = apt(bx + u * 0.12, by + bh - u * 0.1);
    } else {
        tri[0] = apt(bx + bw - u * 0.12, by + u * 0.1);
        tri[1] = apt(bx + u * 0.06, by + bh / 2);
        tri[2] = apt(bx + bw - u * 0.12, by + bh - u * 0.1);
    }
    poly(hdc, tri, 3, lit ? COL_HELD_RING : tone(HX(0x3E3E3E), dim), lit ? COL_HELD_RING : tone(HX(0x5A5A5A), dim), 1.0);
}

/* ---- mice, Trak-Ball, Koala Pad, touch tablet --------------------------------------- */

/* One of the mouse's two buttons, a Koala's, a Trak-Ball's: a rounded key in the
 * device's own plastic, lit while held. */
static void art_ptr_button(HDC hdc, const a800_control *ctl)
{
    double bx, by, bw, bh;
    const double u = g_unit;
    const int dim = part_dim(ctl), lit = part_lit(ctl), dev = g_dev[ctl->port];
    COLORREF fill = HX(0xB7B1A2), edge = HX(0x8E887A);
    part_box(ctl, &bx, &by, &bw, &bh);
    if (dev == A800_DEV_AMIGA_MOUSE) fill = HX(0xB9B2A0);
    else if (dev == A800_DEV_KOALA) { fill = HX(0x151515); edge = HX(0x050505); }
    else if (dev == A800_DEV_TRAKBALL) { fill = HX(0x3A3A3E); edge = HX(0x111111); }
    rrect(hdc, bx, by + (lit ? u * 0.05 : 0), bw, bh - (lit ? u * 0.03 : 0), u * 0.25, tone(lit ? COL_HELD_FILL : fill, dim),
          tone(edge, dim), 1.0);
    if (lit)
        rrect(hdc, bx - u * 0.08, by - u * 0.08, bw + u * 0.16, bh + u * 0.16, u * 0.3, NOFILL, COL_HELD_RING, u * 0.1);
}

/* The Atari ST and Amiga mice: a rounded body with a fuji embossed low on the
 * back (the buttons are parts of their own). */
static void art_mouse_body(HDC hdc, const a800_control *ctl, int amiga)
{
    double bx, by, bw, bh;
    const double u = g_unit;
    const int dim = body_dim(ctl->port);
    const COLORREF skin = tone(amiga ? HX(0xCFC8B6) : HX(0xC4BEAF), dim), seam = tone(HX(0x8E887A), dim);
    int bar;
    part_box(ctl, &bx, &by, &bw, &bh);
    rrect(hdc, bx + u * 0.1, by + u * 0.16, bw, bh, u * 1.2, COL_SHADOW, COL_SHADOW, 0);
    rrect(hdc, bx, by, bw, bh, u * 1.2, skin, seam, 1.2);
    line_seg(hdc, bx + bw / 2, by + u * 0.25, bx + bw / 2, by + bh * 0.4, seam, 1.0);     /* the split between the buttons */
    if (!amiga) {
        for (bar = -1; bar <= 1; bar++) {       /* the fuji, embossed */
            POINT pts[4];
            const double ctrx = bx + bw / 2 + bar * u * 0.36, base = by + bh - u * 0.8, top = by + bh - u * 1.7;
            pts[0] = apt(ctrx - u * 0.08, base);
            pts[1] = apt(ctrx + u * 0.08, base);
            pts[2] = apt(ctrx - bar * u * 0.1 + u * 0.08, top);
            pts[3] = apt(ctrx - bar * u * 0.1 - u * 0.08, top);
            poly(hdc, pts, 4, tone(HX(0xB0AA9B), dim), tone(HX(0xB0AA9B), dim), 0);
        }
    }
}

/* The mat beside a mouse: a dark pad to drag on. */
static void art_mat(HDC hdc, const a800_control *ctl)
{
    double bx, by, bw, bh;
    const double u = g_unit;
    const int dim = part_dim(ctl), lit = part_lit(ctl);
    part_box(ctl, &bx, &by, &bw, &bh);
    rrect(hdc, bx, by, bw, bh, u * 0.5, tone(HX(0x4F4C44), dim), tone(lit ? COL_HELD_RING : HX(0x9A9588), dim), lit ? u * 0.12 : 1.5);
    rrect(hdc, bx + u * 0.3, by + u * 0.3, bw - u * 0.6, bh - u * 0.6, u * 0.3, NOFILL, tone(HX(0x6A665C), dim), 1.0);
}

/* The CX22 Trak-Ball: an angular black body, a silver strip, the ball in its ring. */
static void art_trak_body(HDC hdc, const a800_control *ctl)
{
    static const double shape[8][2] = { { 0.6, 0.0 }, { 8.4, 0.0 }, { 9.0, 0.8 }, { 9.0, 4.4 },
                                        { 8.4, 5.0 }, { 0.6, 5.0 }, { 0.0, 4.4 }, { 0.0, 0.8 } };
    double bx, by, bw, bh;
    const double u = g_unit;
    const int dim = body_dim(ctl->port);
    POINT pts[8], shadow[8];
    int idx;
    part_box(ctl, &bx, &by, &bw, &bh);
    for (idx = 0; idx < 8; idx++) {
        pts[idx] = apt(bx + shape[idx][0] * u, by + shape[idx][1] * u);
        shadow[idx] = apt(bx + shape[idx][0] * u + u * 0.1, by + shape[idx][1] * u + u * 0.16);
    }
    poly(hdc, shadow, 8, COL_SHADOW, COL_SHADOW, 0);
    poly(hdc, pts, 8, tone(HX(0x222222), dim), tone(HX(0x3A3A3A), dim), u * 0.12);
    ellipse(hdc, bx + u * 4.5, by + u * 2.6, u * 1.9, u * 1.9, tone(HX(0x111111), dim), tone(HX(0x3A3A3A), dim), 1.0);
    rrect(hdc, bx + u * 0.5, by + u * 4.05, u * 8.0, u * 0.7, u * 0.1, tone(HX(0xC9C9C9), dim), tone(HX(0x8E8E8E), dim), 1.0);
}

static void art_ball(HDC hdc, const a800_control *ctl)
{
    double bx, by, bw, bh, cx, cy;
    const double u = g_unit;
    const int dim = part_dim(ctl), lit = part_lit(ctl);
    part_box(ctl, &bx, &by, &bw, &bh);
    cx = bx + bw / 2;
    cy = by + bh / 2;
    ellipse(hdc, cx, cy, u * 1.5, u * 1.5, tone(HX(0x2E2E34), dim), tone(HX(0x4A4A52), dim), 1.0);
    ellipse(hdc, cx - u * 0.45, cy - u * 0.5, u * 0.55, u * 0.4, tone(HX(0x55555E), dim), tone(HX(0x55555E), dim), 0);
    if (lit) ellipse(hdc, cx, cy, u * 1.62, u * 1.62, NOFILL, COL_HELD_RING, u * 0.1);
}

static void art_koala_body(HDC hdc, const a800_control *ctl)
{
    double bx, by, bw, bh;
    const double u = g_unit;
    const int dim = body_dim(ctl->port);
    part_box(ctl, &bx, &by, &bw, &bh);
    rrect(hdc, bx + u * 0.1, by + u * 0.16, bw, bh, u * 0.5, COL_SHADOW, COL_SHADOW, 0);
    rrect(hdc, bx, by, bw, bh, u * 0.5, tone(HX(0xE4DDC8), dim), tone(HX(0xA69F8A), dim), 1.2);
}

static void art_tablet_body(HDC hdc, const a800_control *ctl)
{
    double bx, by, bw, bh;
    const double u = g_unit;
    const int dim = body_dim(ctl->port);
    part_box(ctl, &bx, &by, &bw, &bh);
    rrect(hdc, bx + u * 0.1, by + u * 0.16, bw, bh, u * 0.4, COL_SHADOW, COL_SHADOW, 0);
    rrect(hdc, bx, by, bw, bh, u * 0.4, tone(HX(0x1E1E1E), dim), tone(HX(0x0A0A0A), dim), 1.2);
    rrect(hdc, bx + u * 0.4, by + u * 0.3, u * 7.8, u * 0.65, u * 0.1, tone(HX(0xD8D8D4), dim), tone(HX(0x9A9A96), dim), 1.0);
    rrect(hdc, bx + u * 0.9, by + u * 4.95, u * 3.0, u * 0.12, u * 0.06, tone(HX(0xBDBDB8), dim), tone(HX(0x8A8A86), dim), 0);   /* the stylus */
}

/* The pad of a Koala Pad or the touch tablet: where a touch is a point. */
static void art_pad(HDC hdc, const a800_control *ctl)
{
    double bx, by, bw, bh;
    const double u = g_unit;
    const int dim = part_dim(ctl), lit = part_lit(ctl), tablet = ctl->arg == 1;
    part_box(ctl, &bx, &by, &bw, &bh);
    rrect(hdc, bx, by, bw, bh, u * 0.25, tone(tablet ? HX(0x2B2E30) : HX(0x3B3B3D), dim),
          tone(lit ? COL_HELD_RING : (tablet ? HX(0xC0408A) : HX(0x2A2A2C)), dim), lit ? u * 0.12 : (tablet ? u * 0.1 : 1.2));
}

/* The CX75 light pen: a thin black pen with a grey clip. */
static void art_pen_body(HDC hdc, const a800_control *ctl)
{
    double bx, by, bw, bh;
    const double u = g_unit;
    const int dim = body_dim(ctl->port);
    part_box(ctl, &bx, &by, &bw, &bh);
    rrect(hdc, bx + u * 0.08, by + u * 0.12, bw, bh, u * 0.4, COL_SHADOW, COL_SHADOW, 0);
    rrect(hdc, bx, by, bw, bh, u * 0.4, tone(HX(0x222222), dim), tone(HX(0x3A3A3A), dim), 1.0);
    rrect(hdc, bx + u * 0.12, by + u * 0.9, bw - u * 0.24, u * 1.8, u * 0.2, tone(HX(0x9A9A9A), dim), tone(HX(0x6A6A6A), dim), 1.0);
    line_seg(hdc, bx + bw / 2, by + bh - u * 0.1, bx + bw / 2, by + bh + u * 0.2, tone(HX(0x151515), dim), u * 0.12);
}

/* The XG-1 light gun: a grey pistol, barrel and sight, a ribbed grip, a trigger. */
static void art_gun_body(HDC hdc, const a800_control *ctl)
{
    static const double grip[4][2] = { { 4.3, 2.1 }, { 6.8, 2.1 }, { 7.4, 6.7 }, { 5.1, 6.9 } };
    double bx, by, bw, bh;
    const double u = g_unit;
    const int dim = body_dim(ctl->port);
    const COLORREF skin = tone(HX(0xB7B1A2), dim), edge = tone(HX(0x8A857A), dim);
    POINT pts[4];
    int idx;
    part_box(ctl, &bx, &by, &bw, &bh);
    for (idx = 0; idx < 4; idx++) pts[idx] = apt(bx + grip[idx][0] * u, by + grip[idx][1] * u);
    rrect(hdc, bx + u * 0.1, by + u * 0.46, u * 7.8, u * 2.0, u * 0.4, COL_SHADOW, COL_SHADOW, 0);
    poly(hdc, pts, 4, skin, edge, u * 0.1);
    rrect(hdc, bx, by + u * 0.3, u * 7.8, u * 2.0, u * 0.4, skin, edge, 1.2);
    rrect(hdc, bx + u * 0.1, by, u * 0.5, u * 0.5, u * 0.1, skin, edge, 1.0);             /* the front sight */
    rrect(hdc, bx + u * 3.4, by + u * 2.3, u * 0.5, u * 1.4, u * 0.2, tone(HX(0x6E6C66), dim), tone(HX(0x4A4944), dim), 1.0);  /* the trigger */
    for (idx = 0; idx < 4; idx++)               /* the grip's ribs */
        line_seg(hdc, bx + (4.9 + idx * 0.05) * u, by + (3.2 + idx * 0.9) * u, bx + (7.0 + idx * 0.05) * u,
                 by + (3.2 + idx * 0.9) * u, edge, 1.0);
}

/* ---- the CX85 keypad ------------------------------------------------------------------ */

static void art_cx85_body(HDC hdc, const a800_control *ctl)
{
    double bx, by, bw, bh;
    const double u = g_unit;
    const int dim = body_dim(ctl->port);
    part_box(ctl, &bx, &by, &bw, &bh);
    rrect(hdc, bx + u * 0.1, by + u * 0.16, bw, bh, u * 0.4, COL_SHADOW, COL_SHADOW, 0);
    rrect(hdc, bx, by, bw, bh, u * 0.4, tone(HX(0xD7CFB8), dim), tone(HX(0xA39A82), dim), 1.2);
    rrect(hdc, bx + u * 0.1, by + u * 0.1, bw - u * 0.2, u * 4.25, u * 0.3, tone(HX(0x55564F), dim), tone(HX(0x3A3B35), dim), 1.0);
}

static void art_cx85_key(HDC hdc, const a800_control *ctl)
{
    double bx, by, bw, bh;
    const double u = g_unit;
    const int dim = part_dim(ctl), lit = part_lit(ctl);
    part_box(ctl, &bx, &by, &bw, &bh);
    rrect(hdc, bx, by + (lit ? u * 0.04 : 0), bw, bh - (lit ? u * 0.03 : 0), u * 0.14, tone(lit ? COL_HELD_FILL : HX(0x3A352B), dim),
          tone(HX(0x26211A), dim), 1.0);
    if (lit)
        rrect(hdc, bx - u * 0.06, by - u * 0.06, bw + u * 0.12, bh + u * 0.12, u * 0.18, NOFILL, COL_HELD_RING, u * 0.08);
}

/* ---- the console keys ------------------------------------------------------------------ */

/* START SELECT OPTION HELP RESET BREAK: the colours the 800's console keys wear,
 * orange toward yellow. */
static COLORREF console_color(int con)
{
    switch (con) {
    case A800_CON_START: return HX(0xF6BE13);
    case A800_CON_SELECT: return HX(0xEEAC1D);
    case A800_CON_OPTION: return HX(0xE69C1E);
    case A800_CON_HELP: return HX(0xDDA11A);
    case A800_CON_RESET: return HX(0xD8921C);
    default: return HX(0xB4501E);
    }
}

static void art_console_body(HDC hdc, const a800_control *ctl)
{
    double bx, by, bw, bh;
    const double u = g_unit;
    const int dim = body_dim(-1);
    part_box(ctl, &bx, &by, &bw, &bh);
    rrect(hdc, bx + u * 0.1, by + u * 0.16, bw, bh, u * 0.4, COL_SHADOW, COL_SHADOW, 0);
    rrect(hdc, bx, by, bw, bh, u * 0.4, tone(COL_PANEL, dim), tone(HX(0x0A0A0A), dim), 1.2);
}

static void art_console_key(HDC hdc, const a800_control *ctl)
{
    double bx, by, bw, bh;
    const double u = g_unit;
    const int con = ctl->target - A800_TARGET_CONSOLE(0), dim = part_dim(ctl), lit = part_lit(ctl);
    const COLORREF top = console_color(con);
    part_box(ctl, &bx, &by, &bw, &bh);
    grad_rrect(hdc, bx, by + (lit ? u * 0.05 : 0), bw, bh - (lit ? u * 0.05 : 0), u * 0.18, tone(lit ? COL_HELD_FILL : top, dim),
               tone(lit ? top : mix_color(top, HX(0x7A4A10), 0.4), dim), tone(HX(0x2A1A06), dim), 1.0);
    if (lit)
        rrect(hdc, bx - u * 0.1, by - u * 0.08, bw + u * 0.2, bh + u * 0.16, u * 0.24, NOFILL, COL_HELD_RING, u * 0.1);
}

/* ---- the mat and every device in the layout -------------------------------------------- */

static void art_case(HDC hdc)
{
    int idx;
    grad_rrect(hdc, g_case_rc.left, g_case_rc.top, g_case_rc.right - g_case_rc.left,
               g_case_rc.bottom - g_case_rc.top, SCL(14), COL_MAT_TOP, COL_MAT_BOT, COL_MAT_EDGE, 1.5);
    {   /* the Fuji mark: three bars leaning together, then the name (text pass) */
        int bar;
        for (bar = 0; bar < 3; bar++) {
            POINT pts[4];
            const double ctrx = g_brand_rc.left + SCL(10) + (bar - 1) * SCL(5);
            const double lean = -(bar - 1) * SCL(2);
            pts[0] = apt(ctrx - SCL(1), g_brand_rc.bottom - SCL(3));
            pts[1] = apt(ctrx + SCL(1), g_brand_rc.bottom - SCL(3));
            pts[2] = apt(ctrx + lean + SCL(1), g_brand_rc.top + SCL(3));
            pts[3] = apt(ctrx + lean - SCL(1), g_brand_rc.top + SCL(3));
            poly(hdc, pts, 4, COL_ORANGE, COL_ORANGE, 0);
        }
    }
    for (idx = 0; idx < g_numlayout; idx++) {
        const a800_control *ctl = &g_layout[idx];
        switch (ctl->kind) {
        case A800_CTL_BODY:
            if (!strcmp(ctl->label, "Joystick")) art_joy_body(hdc, ctl);
            else if (!strcmp(ctl->label, "Paddle A") || !strcmp(ctl->label, "Paddle B")) art_paddle_body(hdc, ctl);
            else if (!strcmp(ctl->label, "Atari ST mouse")) art_mouse_body(hdc, ctl, 0);
            else if (!strcmp(ctl->label, "Amiga mouse")) art_mouse_body(hdc, ctl, 1);
            else if (!strcmp(ctl->label, "Trak-Ball")) art_trak_body(hdc, ctl);
            else if (!strcmp(ctl->label, "Koala Pad")) art_koala_body(hdc, ctl);
            else if (!strcmp(ctl->label, "Touch tablet")) art_tablet_body(hdc, ctl);
            else if (!strcmp(ctl->label, "Light pen")) art_pen_body(hdc, ctl);
            else if (!strcmp(ctl->label, "Light gun")) art_gun_body(hdc, ctl);
            else if (!strcmp(ctl->label, "CX85")) art_cx85_body(hdc, ctl);
            else if (!strcmp(ctl->label, "Console")) art_console_body(hdc, ctl);
            break;
        case A800_CTL_STICK: art_stick(hdc, ctl); break;
        case A800_CTL_ROUND_BUTTON: art_fire(hdc, ctl); break;
        case A800_CTL_KNOB: art_knob(hdc, ctl); break;
        case A800_CTL_PADDLE_FIRE: art_edge_button(hdc, ctl); break;
        case A800_CTL_TURN: art_turn(hdc, ctl); break;
        case A800_CTL_POINTER_BUTTON: art_ptr_button(hdc, ctl); break;
        case A800_CTL_MAT: art_mat(hdc, ctl); break;
        case A800_CTL_BALL: art_ball(hdc, ctl); break;
        case A800_CTL_PAD: art_pad(hdc, ctl); break;
        case A800_CTL_CX85_KEY: art_cx85_key(hdc, ctl); break;
        case A800_CTL_CONSOLE_KEY: art_console_key(hdc, ctl); break;
        default: break;
        }
    }
}

/* ---- the keyboard ------------------------------------------------------------------------- */

typedef struct {
    COLORREF top, bottom, edge;                 /* the case */
    COLORREF key, key_hi, key_edge;             /* a normal cap */
    COLORREF legend, shifted;                   /* its legends */
    COLORREF special;                           /* the Atari key, F1-F4 */
} kbd_palette;

static const kbd_palette *palette(void)
{
    static const kbd_palette p400 = { HX(0x1D1D1D), HX(0x111111), HX(0x050505), HX(0x232323), HX(0x303030), HX(0x0A0A0A),
                                      HX(0xF1ECDC), HX(0xE08A1F), HX(0x3A3A3A) };
    static const kbd_palette p800 = { HX(0xD4CDB8), HX(0xC1B9A2), HX(0x8E8672), HX(0x3B352B), HX(0x4A4437), HX(0x1F1B15),
                                      HX(0xE9E4D4), HX(0xC9BC9B), HX(0x6A6152) };
    static const kbd_palette pxl = { HX(0xE6E3DA), HX(0xD3CFC3), HX(0x9C988C), HX(0x2E2E31), HX(0x3E3E42), HX(0x161618),
                                     HX(0xF2F2F2), HX(0xC8C8CC), HX(0x56565B) };
    return g_family == A800_KBD_400 ? &p400 : g_family == A800_KBD_800 ? &p800 : &pxl;
}

/* The pixel box of a key cap. */
static void key_box(const a800_kbd_key *key, double *xpos, double *ypos, double *width, double *height)
{
    const double gap = g_kunit * 0.07;
    *xpos = g_korgx + key->x * g_kunit + gap;
    *ypos = g_korgy + key->y * g_kunit + gap;
    *width = key->w * g_kunit - 2 * gap;
    *height = key->h * g_kunit - 2 * gap;
}

static void key_rect(const a800_kbd_key *key, RECT *rect)
{
    double xpos, ypos, width, height;
    key_box(key, &xpos, &ypos, &width, &height);
    SetRect(rect, (int)floor(xpos), (int)floor(ypos), (int)ceil(xpos + width), (int)ceil(ypos + height));
}

/* The target a console cap stands for, or -1. */
static int key_target(const a800_kbd_key *key)
{
    return (key->kind == A800_KEY_CONSOLE || key->kind == A800_KEY_BREAK) && key->console >= 0
               ? A800_TARGET_CONSOLE(key->console) : -1;
}

static int key_lit(int idx)
{
    if (g_map_state != -2) {
        const int target = key_target(&g_keys[idx]);
        return g_map_state >= 0 && target == g_map_state;
    }
    return idx < MAX_KEYS && g_shown.key[idx];
}

static int key_dim(int idx)
{
    return g_map_state >= 0 && key_target(&g_keys[idx]) < 0;
}

static void art_keyboard(HDC hdc)
{
    const kbd_palette *pal = palette();
    int idx;
    if (!g_show_keyboard || !g_keys) return;
    grad_rrect(hdc, g_kbd_rc.left, g_kbd_rc.top, g_kbd_rc.right - g_kbd_rc.left, g_kbd_rc.bottom - g_kbd_rc.top, SCL(12),
               pal->top, pal->bottom, pal->edge, 1.5);
    for (idx = 0; idx < g_nkeys; idx++) {
        const a800_kbd_key *key = &g_keys[idx];
        double bx, by, bw, bh;
        const int lit = key_lit(idx), dim = key_dim(idx), con = key->kind == A800_KEY_CONSOLE;
        COLORREF fill = pal->key, hi = pal->key_hi, edge = pal->key_edge;
        const double rad = g_kunit * 0.16;
        key_box(key, &bx, &by, &bw, &bh);
        if (con && g_family <= A800_KBD_800) { fill = console_color(key->console); hi = mix_color(fill, HX(0xFFFFFF), 0.25); edge = HX(0x3A2408); }
        else if (con && key->console == A800_CON_HELP) { fill = HX(0xA9A79E); hi = HX(0xC4C2B8); edge = HX(0x5C5A52); }
        else if (con) { fill = HX(0x3A3A3E); hi = HX(0x4A4A50); }
        else if (key->kind == A800_KEY_ATARI || key->kind == A800_KEY_FUNCTION) { fill = pal->special; hi = mix_color(fill, HX(0xFFFFFF), 0.18); }
        if (lit) { fill = HX(0xE8A23C); hi = COL_HELD_FILL; edge = HX(0x6A4410); }
        rrect(hdc, bx, by + g_kunit * 0.05, bw, bh, rad, tone(edge, dim), tone(edge, dim), 0);
        if (con && key->console == A800_CON_HELP && g_family >= A800_KBD_XL)
            ellipse(hdc, bx + bw / 2, by + bh / 2, bw / 2, bh / 2, tone(fill, dim), tone(edge, dim), 1.0);
        else
            rrect(hdc, bx, by, bw, bh, rad, tone(fill, dim), tone(edge, dim), 1.0);
        if (con && g_family <= A800_KBD_800)
            line_seg(hdc, bx + rad, by + 1.5, bx + bw - rad, by + 1.5, tone(hi, dim), 1.5);
        else
            rrect(hdc, bx + g_kunit * 0.05, by + g_kunit * 0.04, bw - g_kunit * 0.1, bh - g_kunit * 0.12, rad * 0.8, NOFILL,
                  tone(hi, dim), 1.0);
        if (lit)
            rrect(hdc, bx - g_kunit * 0.04, by - g_kunit * 0.04, bw + g_kunit * 0.08, bh + g_kunit * 0.08, rad, NOFILL,
                  COL_HELD_RING, g_kunit * 0.05);
    }
}

/* In Map mode and with Show bindings a target shows what drives it. */
static void binding_label(int target, char *out, int outsz)
{
    const a800_binding bind = a800session_binding_get(g_session, target);
    out[0] = '\0';
    if (bind.keysym) {
        a800session_keysym_name(bind.keysym, out, outsz);
        /* the labels are small: "Keypad 5" reads "KP 5" */
        if (strncmp(out, "Keypad ", 7) == 0) {
            memmove(out + 3, out + 7, strlen(out + 7) + 1);
            memcpy(out, "KP ", 3);
        }
    }
    if (!out[0]) snprintf(out, (size_t)outsz, "%s", "\xe2\x80\x94");
}

/* A binding printed on its part. */
static void text_binding_on(HDC hdc, const a800_control *ctl, const RECT *rect)
{
    char legend[96];
    RECT box = *rect;
    if (!binding_mode() || ctl->target < 0) return;
    binding_label(ctl->target, legend, sizeof legend);
    SelectObject(hdc, g_tiny);
    box.left -= (int)(g_unit * 0.15);
    box.right += (int)(g_unit * 0.15);
    SetTextColor(hdc, g_map_state == ctl->target ? COL_HELD_RING : RGB(0xFF, 0xFF, 0xFF));
    text_line(hdc, legend, &box, DT_CENTER | DT_END_ELLIPSIS);
}

/* What a CX85 key says on its cap: the layout names are longer than the caps. */
static const char *cx85_legend(const char *label)
{
    if (!strcmp(label, "ESCAPE")) return "ESC";
    if (!strcmp(label, "DELETE")) return "DEL";
    if (!strcmp(label, "+ ENTER")) return "+";
    return label;
}

/* The labels over the art, at the window's own size. */
static void text_parts(HDC hdc)
{
    int idx;
    RECT brand = g_brand_rc;
    SetBkMode(hdc, TRANSPARENT);
    SelectObject(hdc, g_brand);
    SetTextColor(hdc, COL_BRAND_INK);
    brand.left += SCL(26);
    text_line(hdc, a800_machine_name(g_machine), &brand, DT_LEFT);
    for (idx = 0; idx < g_numlayout; idx++) {
        const a800_control *ctl = &g_layout[idx];
        RECT rect;
        const int dim = part_dim(ctl);
        const double u = g_unit;
        char legend[96];
        part_rect(ctl, &rect);
        switch (ctl->kind) {
        case A800_CTL_CAPTION: {
            char line[96];
            snprintf(line, sizeof line, "%s: %s", ctl->label, ctl->legend ? ctl->legend : "");
            SelectObject(hdc, g_caption);
            SetTextColor(hdc, COL_CAPTION);
            text_line(hdc, line, &rect, DT_CENTER);
            break;
        }
        case A800_CTL_BODY: {
            const int bdim = ctl->port >= 0 ? body_dim(ctl->port) : body_dim(-1);
            RECT box = rect;
            if (!strcmp(ctl->label, "Joystick")) {
                SelectObject(hdc, g_tiny);
                SetTextColor(hdc, tone(HX(0x3A3A3A), bdim));
                box.left += (int)(u * 3.3);
                box.right -= (int)(u * 3.3);
                box.top += (int)(u * 1.25);
                box.bottom = box.top + (int)(u * 0.5);
                text_line(hdc, "TOP", &box, DT_CENTER);
            } else if (!strcmp(ctl->label, "Paddle A") || !strcmp(ctl->label, "Paddle B")) {
                box.left += (int)(u * 4.85);
                box.right = rect.left + (int)(u * 6.45);
                box.top += (int)(u * 4.3);
                box.bottom = rect.top + (int)(u * 5.6);
                SelectObject(hdc, g_legend);
                SetTextColor(hdc, tone(HX(0xE6E6E6), bdim));
                text_line(hdc, "paddle", &box, DT_CENTER);
            } else if (!strcmp(ctl->label, "Trak-Ball")) {
                box.top += (int)(u * 4.05);
                box.bottom = rect.top + (int)(u * 4.75);
                SelectObject(hdc, g_legend);
                SetTextColor(hdc, tone(HX(0x222222), bdim));
                box.left = rect.left + (int)(u * 0.8);
                text_line(hdc, "ATARI", &box, DT_LEFT);
                box.left = rect.left + (int)(u * 4.6);
                box.right = rect.right - (int)(u * 0.5);
                text_line(hdc, "TRAK-BALL", &box, DT_CENTER);
            } else if (!strcmp(ctl->label, "Koala Pad")) {
                box.top += (int)(u * 7.35);
                box.bottom = rect.top + (int)(u * 8.1);
                SelectObject(hdc, g_legend);
                SetTextColor(hdc, tone(HX(0x6A624E), bdim));
                text_line(hdc, "KoalaPad", &box, DT_CENTER);
            } else if (!strcmp(ctl->label, "Touch tablet")) {
                box.top += (int)(u * 0.3);
                box.bottom = rect.top + (int)(u * 0.95);
                SelectObject(hdc, g_legend);
                SetTextColor(hdc, tone(HX(0x222222), bdim));
                box.left = rect.left + (int)(u * 0.7);
                text_line(hdc, "ATARI", &box, DT_LEFT);
                box.left = rect.left + (int)(u * 3.6);
                box.right = rect.right - (int)(u * 0.5);
                text_line(hdc, "TOUCH TABLET", &box, DT_CENTER);
            } else if (!strcmp(ctl->label, "CX85")) {
                box.top += (int)(u * 4.35);
                box.bottom = rect.top + (int)(u * 4.95);
                SelectObject(hdc, g_legend);
                SetTextColor(hdc, tone(HX(0xB07A10), bdim));
                box.left = rect.left + (int)(u * 0.3);
                text_line(hdc, "ATARI", &box, DT_LEFT);
                SetTextColor(hdc, tone(HX(0x55564F), bdim));
                box.left = rect.left + (int)(u * 4.0);
                box.right = rect.right - (int)(u * 0.2);
                text_line(hdc, "CX85", &box, DT_CENTER);
            } else if (!strcmp(ctl->label, "Nothing")) {
                SelectObject(hdc, g_tiny);
                SetTextColor(hdc, tone(HX(0xE6DFCF), bdim));
                box = rect;
                InflateRect(&box, (int)(u * 4), 0);     /* the body is only 3 units wide: the words are wider */
                text_line(hdc, "nothing plugged in", &box, DT_CENTER);
            }
            break;
        }
        case A800_CTL_ROUND_BUTTON:
            text_binding_on(hdc, ctl, &rect);
            break;
        case A800_CTL_CONSOLE_KEY: {
            SelectObject(hdc, g_legend);
            SetTextColor(hdc, part_lit(ctl) && g_map_state == -2 ? COL_ON_HELD : tone(HX(0x3A2A10), dim));
            if (binding_mode() && ctl->target >= 0) {
                binding_label(ctl->target, legend, sizeof legend);
                if (g_map_state == ctl->target) SetTextColor(hdc, HX(0x7A1A00));
                text_line(hdc, legend, &rect, DT_CENTER | DT_END_ELLIPSIS);
            } else {
                text_line(hdc, ctl->label, &rect, DT_CENTER);
            }
            break;
        }
        case A800_CTL_CX85_KEY: {
            SelectObject(hdc, g_tiny);
            SetTextColor(hdc, part_lit(ctl) ? COL_ON_HELD : tone(HX(0xE8E4D6), dim));
            text_line(hdc, cx85_legend(ctl->label), &rect, DT_CENTER);
            break;
        }
        case A800_CTL_MAT:
        case A800_CTL_BALL:
        case A800_CTL_PAD:
            if (ctl->legend && ctl->kind != A800_CTL_BALL) {
                RECT box = rect;
                box.top = rect.bottom - (int)(u * 0.9);
                box.bottom = rect.bottom - (int)(u * 0.2);
                SelectObject(hdc, g_tiny);
                SetTextColor(hdc, tone(HX(0xC9C4B4), dim));
                text_line(hdc, ctl->legend, &box, DT_CENTER);
            }
            break;
        case A800_CTL_NOTE:
            SelectObject(hdc, g_legend);
            SetTextColor(hdc, COL_CAPTION);
            text_line(hdc, ctl->label, &rect, DT_CENTER);
            break;
        case A800_CTL_STICK:
            if (binding_mode()) {
                /* the four directions' keys, round the ring */
                static const int dirs[4] = { A800_ACT_LEFT, A800_ACT_RIGHT, A800_ACT_UP, A800_ACT_DOWN };
                int dir;
                SelectObject(hdc, g_tiny);
                for (dir = 0; dir < 4; dir++) {
                    RECT box;
                    const int half = (int)(u * 0.7);
                    const int midx = (rect.left + rect.right) / 2, midy = (rect.top + rect.bottom) / 2;
                    const int target = A800_TARGET_PORT(ctl->port, dirs[dir]);
                    int posx = midx, posy = midy;
                    if (dirs[dir] == A800_ACT_LEFT) posx = rect.left - (int)(u * 0.55);
                    if (dirs[dir] == A800_ACT_RIGHT) posx = rect.right + (int)(u * 0.55);
                    if (dirs[dir] == A800_ACT_UP) posy = rect.top - (int)(u * 0.3);
                    if (dirs[dir] == A800_ACT_DOWN) posy = rect.bottom + (int)(u * 0.3);
                    SetRect(&box, posx - half, posy - SCL(7), posx + half, posy + SCL(7));
                    binding_label(target, legend, sizeof legend);
                    SetTextColor(hdc, g_map_state == target ? COL_HELD_RING : RGB(0xFF, 0xFF, 0xFF));
                    text_line(hdc, legend, &box, DT_CENTER | DT_END_ELLIPSIS);
                }
            }
            break;
        default:
            break;
        }
    }
}

/* One key cap's legends: the shifted character small above the key's own. */
static void text_key(HDC hdc, int idx)
{
    const a800_kbd_key *key = &g_keys[idx];
    const kbd_palette *pal = palette();
    const int lit = key_lit(idx), dim = key_dim(idx), con = key->kind == A800_KEY_CONSOLE;
    RECT rect, box;
    const char *label = key->label ? key->label : "";
    COLORREF ink = pal->legend, small = pal->shifted;
    key_rect(key, &rect);
    if (con && g_family <= A800_KBD_800) ink = HX(0x3A2A10);
    else if (con && key->console == A800_CON_HELP) ink = HX(0x25251F);
    if (key->kind == A800_KEY_ATARI) ink = pal->legend;
    if (lit) { ink = COL_ON_HELD; small = COL_ON_HELD; }
    ink = tone(ink, dim);
    small = tone(small, dim);

    if (g_map_state != -2 && key_target(key) >= 0) {       /* Map: what drives this console key */
        char legend[96];
        const int target = key_target(key);
        binding_label(target, legend, sizeof legend);
        SelectObject(hdc, g_keysm);
        SetTextColor(hdc, g_map_state == target ? HX(0x7A1A00) : ink);
        text_line(hdc, legend, &rect, DT_CENTER | DT_END_ELLIPSIS);
        return;
    }
    if (key->shifted && *key->shifted) {
        const int third = (rect.bottom - rect.top) / 2;
        SetRect(&box, rect.left, rect.top + SCL(1), rect.right, rect.top + third);
        SelectObject(hdc, g_keysm);
        SetTextColor(hdc, small);
        text_line(hdc, key->shifted, &box, DT_CENTER | DT_END_ELLIPSIS);
        SetRect(&box, rect.left, rect.top + third - SCL(2), rect.right, rect.bottom - SCL(1));
        SelectObject(hdc, g_key);
        SetTextColor(hdc, ink);
        text_line(hdc, label, &box, DT_CENTER | DT_END_ELLIPSIS);
        return;
    }
    SelectObject(hdc, g_key);
    SetTextColor(hdc, ink);
    if (g_sym && (unsigned char)label[0] >= 0x80) {
        wchar_t wide[16];
        const int num = MultiByteToWideChar(CP_UTF8, 0, label, -1, wide, 16) - 1;
        if (num > 0 && !font_has(hdc, g_key, wide, num)) SelectObject(hdc, g_sym);
    }
    box = rect;
    InflateRect(&box, -SCL(1), 0);
    text_line(hdc, label, &box, DT_CENTER | DT_END_ELLIPSIS);
}

static void text_keyboard(HDC hdc)
{
    int idx;
    if (!g_show_keyboard || !g_keys) return;
    SetBkMode(hdc, TRANSPARENT);
    for (idx = 0; idx < g_nkeys; idx++) text_key(hdc, idx);
}

/* ---- the strip: gamepads, Map --------------------------------------------------------- */

static HBRUSH accent_brush(void)
{
    static HBRUSH brush;
    if (!brush) brush = CreateSolidBrush(COL_HELD_RING);
    return brush;
}

static void draw_face(HDC hdc, const RECT *bounds, int pushed, int accent, int inactive)
{
    RECT rect = *bounds;
    if (accent) {
        FillRect(hdc, &rect, accent_brush());
        FrameRect(hdc, &rect, (HBRUSH)GetStockObject(GRAY_BRUSH));
        SetTextColor(hdc, COL_ON_HELD);
    } else {
        DrawFrameControl(hdc, &rect, DFC_BUTTON,
                         DFCS_BUTTONPUSH | (pushed ? DFCS_PUSHED : 0) | (inactive ? DFCS_INACTIVE : 0));
        SetTextColor(hdc, GetSysColor(inactive ? COLOR_GRAYTEXT : COLOR_BTNTEXT));
    }
}

static const char *strip_label(int btn)
{
    switch (btn) {
    case SB_MAP: return g_map_state == -2 ? "Map" : "Done";
    case SB_CLEAR: return "Clear";
    case SB_DEFAULTS: return "Defaults";
    case SB_SHOW: return "Show bindings";
    case SB_KEYBOARD: return "Keyboard";
    default: return "Help";
    }
}

/* Automatic, then one segment per port the machine has. */
static int segment_count(void)
{
    return 1 + (g_nports > 0 ? g_nports : 1);
}

static void segment_rect(int row, int seg, RECT *rect)
{
    const RECT *rowrc = &g_gp_row_rc[row];
    const int gap = SCL(4), width = SCL(62);
    const int count = segment_count();
    const int first = rowrc->right - count * width - (count - 1) * gap;
    const int left = first + seg * (width + gap);
    SetRect(rect, left, rowrc->top, left + width, rowrc->top + SCL(SEG_H));
}

/* One block per gamepad: its name and the port it drives now over the Automatic
 * / Port N choice, the current one in orange. */
static void paint_gamepads(HDC hdc)
{
    static const char *const segs[MAX_SEGS] = { "Automatic", "Port 1", "Port 2", "Port 3", "Port 4" };
    const int num = a800session_gamepad_count(g_session), count = segment_count();
    int idx, seg;

    SelectObject(hdc, g_bold);
    SetTextColor(hdc, GetSysColor(COLOR_BTNTEXT));
    text_line(hdc, "Gamepads", &g_gp_head_rc, DT_LEFT);
    SelectObject(hdc, g_font);
    SetTextColor(hdc, GetSysColor(COLOR_GRAYTEXT));
    if (num == 0) {
        text_line(hdc, "No gamepads connected - plug one in", &g_gp_row_rc[0], DT_LEFT);
        return;
    }
    if (num > PAD_ROWS) {
        char more[48];
        snprintf(more, sizeof more, "%d more not shown", num - PAD_ROWS);
        text_line(hdc, more, &g_gp_head_rc, DT_RIGHT);
    }
    for (idx = 0; idx < num && idx < PAD_ROWS; idx++) {
        const int assign = a800session_gamepad_assignment(g_session, idx);
        const int effective = a800session_gamepad_effective_port(g_session, idx);
        const int chosen = assign < 0 || assign >= g_nports ? 0 : assign + 1;
        char name[128], line[48];
        RECT first, name_rc, eff_rc;

        segment_rect(idx, 0, &first);
        name_rc = g_gp_row_rc[idx];
        name_rc.right = first.left - SCL(8);
        name_rc.bottom = name_rc.top + SCL(SEG_H);
        eff_rc = name_rc;
        eff_rc.left = name_rc.right - SCL(96);
        name_rc.right = eff_rc.left - SCL(4);
        a800session_gamepad_name(g_session, idx, name, sizeof name);
        SelectObject(hdc, g_font);
        SetTextColor(hdc, GetSysColor(COLOR_BTNTEXT));
        text_line(hdc, name, &name_rc, DT_LEFT | DT_END_ELLIPSIS);
        if (effective >= 0) snprintf(line, sizeof line, "plays port %d", effective + 1);
        else snprintf(line, sizeof line, "plays nothing");
        SetTextColor(hdc, GetSysColor(COLOR_GRAYTEXT));
        text_line(hdc, line, &eff_rc, DT_RIGHT);
        for (seg = 0; seg < count; seg++) {
            RECT rect;
            segment_rect(idx, seg, &rect);
            draw_face(hdc, &rect, 0, seg == chosen, 0);
            text_line(hdc, segs[seg], &rect, DT_CENTER);
        }
    }
}

/* What the hint line says about the part under an idle pointer. */
static void hover_text(int part, int key, char *out, int outsz)
{
    out[0] = '\0';
    if (key >= 0 && key < g_nkeys) {
        const a800_kbd_key *cap = &g_keys[key];
        if (cap->kind == A800_KEY_SHIFT || cap->kind == A800_KEY_CTRL)
            snprintf(out, (size_t)outsz, "%s latches for the next key.", cap->label ? cap->label : "Key");
        else if (key_target(cap) >= 0)
            snprintf(out, (size_t)outsz, "%s: a console key. Map rebinds it.", a800_target_name(key_target(cap)));
        return;
    }
    if (part >= 0 && part < g_numlayout) {
        const a800_control *ctl = &g_layout[part];
        switch (ctl->kind) {
        case A800_CTL_STICK:
            snprintf(out, (size_t)outsz, "Stick: drag to tilt it. Map: click a side of the ring to rebind that direction.");
            return;
        case A800_CTL_KNOB:
            snprintf(out, (size_t)outsz, "Paddle knob: drag or scroll to turn it; it stays where you leave it.");
            return;
        case A800_CTL_TURN:
            snprintf(out, (size_t)outsz, "Hold to keep turning the knob.");
            return;
        case A800_CTL_MAT:
            snprintf(out, (size_t)outsz, "Drag to move the mouse.");
            return;
        case A800_CTL_BALL:
            snprintf(out, (size_t)outsz, "Drag the ball.");
            return;
        case A800_CTL_PAD:
            snprintf(out, (size_t)outsz, "Press and drag: a touch is a point of the pad.");
            return;
        default:
            break;
        }
        if (ctl->target >= 0 && ctl->target < A800_TARGET_COUNT) {
            const a800_binding bind = a800session_binding_get(g_session, ctl->target);
            char keyname[48], padname[48];
            keyname[0] = '\0';
            if (bind.keysym) a800session_keysym_name(bind.keysym, keyname, sizeof keyname);
            snprintf(padname, sizeof padname, "%s", bind.button != A800_PAD_BTN_NONE ? a800_pad_button_name(bind.button) : "none");
            snprintf(out, (size_t)outsz, "%s   Key: %s   Gamepad: %s", a800_target_name(ctl->target),
                     keyname[0] ? keyname : "none", padname);
        }
    }
}

static void paint_strip(HDC hdc)
{
    int btn;
    char hover[200];
    SelectObject(hdc, g_font);
    for (btn = 0; btn < SB_COUNT; btn++) {
        const int armed = (btn == SB_MAP && g_map_state != -2) || (btn == SB_SHOW && g_show_bindings) ||
                          (btn == SB_KEYBOARD && g_show_keyboard);
        const int inactive = btn == SB_CLEAR && g_map_state < 0;
        draw_face(hdc, &g_btn_rc[btn], 0, armed, inactive);
        text_line(hdc, strip_label(btn), &g_btn_rc[btn], DT_CENTER);
    }
    SetTextColor(hdc, GetSysColor(COLOR_GRAYTEXT));
    hover_text(g_map_state >= 0 ? -1 : g_hover_part, g_map_state >= 0 ? -1 : g_hover_key, hover, sizeof hover);
    text_line(hdc, hover[0] ? hover : g_hint, &g_hint_rc, DT_LEFT | DT_END_ELLIPSIS);
}

/* The whole window: the art supersampled, its text, the strips. Only what the
 * update region needs is drawn. */
static void paint_rect(HDC hdc, const RECT *inval)
{
    RECT art;
    const int scale = 2;
    HDC mem;
    HBITMAP bmp;
    void *bits = NULL;
    BITMAPINFO bmi;
    HGDIOBJ old;

    FillRect(hdc, inval, GetSysColorBrush(COLOR_BTNFACE));
    SetBkMode(hdc, TRANSPARENT);

    /* the art: the intersection with the mat and the keyboard, at 2x */
    IntersectRect(&art, inval, &g_art_rc);
    if (!IsRectEmpty(&art)) {
        const int width = art.right - art.left, height = art.bottom - art.top;
        memset(&bmi, 0, sizeof bmi);
        bmi.bmiHeader.biSize = sizeof bmi.bmiHeader;
        bmi.bmiHeader.biWidth = width * scale;
        bmi.bmiHeader.biHeight = -height * scale;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        mem = CreateCompatibleDC(hdc);
        bmp = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
        if (bmp) {
            HBRUSH face = CreateSolidBrush(GetSysColor(COLOR_BTNFACE));
            RECT all = { 0, 0, width * scale, height * scale };
            old = SelectObject(mem, bmp);
            FillRect(mem, &all, face);
            DeleteObject(face);
            SetBkMode(mem, TRANSPARENT);
            g_art_k = scale;
            g_art_x = art.left;
            g_art_y = art.top;
            g_art_rows = height * scale;
            art_case(mem);
            art_keyboard(mem);
            g_art_k = 1;
            SetStretchBltMode(hdc, HALFTONE);
            SetBrushOrgEx(hdc, 0, 0, NULL);
            StretchBlt(hdc, art.left, art.top, width, height, mem, 0, 0, width * scale, height * scale, SRCCOPY);
            SelectObject(mem, old);
            DeleteObject(bmp);
        }
        DeleteDC(mem);
        SelectObject(hdc, g_font);
        text_parts(hdc);
        text_keyboard(hdc);
    }
    paint_gamepads(hdc);
    paint_strip(hdc);
}

/* ---- pressing ----------------------------------------------------------------- */

static void invalidate_part(int part)
{
    RECT rect;
    if (!g_panel || part < 0 || part >= g_numlayout) return;
    part_rect(&g_layout[part], &rect);
    InflateRect(&rect, SCL(20), SCL(20));
    InvalidateRect(g_panel, &rect, FALSE);
}

/* The mat: any of its parts changed. */
static void invalidate_view(void)
{
    if (!g_panel) return;
    InvalidateRect(g_panel, &g_case_rc, FALSE);
}

static void invalidate_keyboard(void)
{
    if (!g_panel || IsRectEmpty(&g_kbd_rc)) return;
    InvalidateRect(g_panel, &g_kbd_rc, FALSE);
}

/* The stick or a knob is being dragged: update what the pointer holds. */
static void stick_follow(const a800_control *ctl, POINT pnt)
{
    double bx, by, bw, bh, delx, dely, len;
    const double reach = (double)ctl->w * g_unit / 2;
    part_box(ctl, &bx, &by, &bw, &bh);
    delx = pnt.x - (bx + bw / 2);
    dely = pnt.y - (by + bh / 2);
    len = sqrt(delx * delx + dely * dely);
    g_stick_dx = delx / reach;
    g_stick_dy = dely / reach;
    if (len > reach) { g_stick_dx = delx / len; g_stick_dy = dely / len; }
    a800session_pointer_set(g_session, ctl->port, a800session_stick_from_vector((float)delx, (float)dely, (float)reach));
    refresh_held();
}

static void knob_follow(const a800_control *ctl, POINT pnt)
{
    double bx, by, bw, bh;
    part_box(ctl, &bx, &by, &bw, &bh);
    a800session_paddle_pointer(g_session, ctl->port, ctl->arg & 1,
                               a800session_knob_from_vector((float)(pnt.x - (bx + bw / 2)), (float)(pnt.y - (by + bh / 2))), 1);
    refresh_held();
}

/* A mouse mat or the Trak-Ball's ball: the drag's steps are the device's motion. */
static void mat_follow(POINT pnt)
{
    const double scale = 2.0 * 96.0 / g_dpi;
    int stepx, stepy;
    g_mat_carry_x += (pnt.x - g_mat_last.x) * scale;
    g_mat_carry_y += (pnt.y - g_mat_last.y) * scale;
    g_mat_last = pnt;
    stepx = (int)g_mat_carry_x;
    stepy = (int)g_mat_carry_y;
    g_mat_carry_x -= stepx;
    g_mat_carry_y -= stepy;
    if (stepx || stepy) a800session_pointer_motion(g_session, stepx, stepy, g_ptr_mask);
}

/* A touch on a Koala Pad or the touch tablet: where on its surface, 0..1. */
static void pad_follow(const a800_control *ctl, POINT pnt)
{
    double bx, by, bw, bh, hor, ver;
    part_box(ctl, &bx, &by, &bw, &bh);
    hor = (pnt.x - bx) / bw;
    ver = (pnt.y - by) / bh;
    if (hor < 0) hor = 0;
    if (hor > 1) hor = 1;
    if (ver < 0) ver = 0;
    if (ver > 1) ver = 1;
    /* the tablet's touch is its own button (bit 1); the Koala's are its two buttons */
    a800session_pointer_norm(g_session, (float)hor, (float)ver, 1, ctl->arg == 1 ? 1u : g_ptr_mask);
}

/* Lets go of what the pointer holds in this window (only that: the main
 * window releases its own keys). */
static void mouse_release(void)
{
    const int part = g_mouse_part, stick = g_mouse_stick, knob = g_mouse_knob, mat = g_mouse_mat, pad = g_mouse_pad;
    const int key = g_mouse_key;
    g_mouse_part = -1;
    g_mouse_stick = -1;
    g_mouse_knob = -1;
    g_mouse_mat = -1;
    g_mouse_pad = -1;
    g_mouse_key = -1;
    g_mat_carry_x = g_mat_carry_y = 0;
    if (part >= 0 && part < g_numlayout) {
        const a800_control *ctl = &g_layout[part];
        switch (ctl->kind) {
        case A800_CTL_PADDLE_FIRE: a800session_paddle_fire(g_session, ctl->port, ctl->arg & 1, 0); break;
        case A800_CTL_TURN: a800session_paddle_turn(g_session, ctl->port, ctl->arg & 1, 0); break;
        case A800_CTL_POINTER_BUTTON:
            g_ptr_mask &= ~(unsigned)ctl->arg;
            a800session_pointer_motion(g_session, 0, 0, g_ptr_mask);
            break;
        case A800_CTL_CX85_KEY: a800session_cx85_key(g_session, ctl->arg, 0); break;
        default:
            if (ctl->target >= 0) a800session_press(g_session, ctl->target, 0);
            break;
        }
    }
    if (stick >= 0 && stick < g_numlayout) a800session_pointer_set(g_session, g_layout[stick].port, 0);
    if (knob >= 0 && knob < g_numlayout) a800session_paddle_pointer(g_session, g_layout[knob].port, g_layout[knob].arg & 1, 0, 0);
    if (pad >= 0 && pad < g_numlayout) a800session_pointer_norm(g_session, 0, 0, 0, 0);
    if (key >= 0 && key < g_nkeys) a800session_keyboard_press(g_session, g_keys[key].index, 0);
    if (part >= 0) invalidate_part(part);
    if (stick >= 0 || knob >= 0 || mat >= 0 || pad >= 0) invalidate_view();
    if (key >= 0) invalidate_keyboard();
}

static void set_hint(const char *text)
{
    snprintf(g_hint, sizeof g_hint, "%s", text);
    g_hover_part = -1;
    g_hover_key = -1;
    if (g_panel) InvalidateRect(g_panel, &g_hint_rc, FALSE);
}

static const char *idle_hint(void)
{
    return "Drag the stick or a knob, press a button or a key to hold it. Map rebinds.";
}

static void set_map_state(int state)
{
    g_map_state = state;
    if (state < 0) {
        if (g_panel) KillTimer(g_panel, IDT_CAPTURE);
        a800session_gamepad_capture_cancel(g_session);
    }
    if (state == -2) {
        set_hint(idle_hint());
    } else if (state == -1) {
        set_hint("Click a control (or pick one from the list) to remap. Esc finishes.");
    } else {
        char msg[200];
        mouse_release();
        snprintf(msg, sizeof msg, "Press a key or a gamepad button for %s. Esc cancels.", a800_target_name(state));
        a800session_gamepad_capture_begin(g_session);
        set_hint(msg);
        if (g_panel) SetTimer(g_panel, IDT_CAPTURE, 50, NULL);
    }
    if (g_combo) {
        EnableWindow(g_combo, state != -2);
        SendMessageA(g_combo, CB_SETCURSEL, (WPARAM)(state >= 0 ? state + 1 : 0), 0);
    }
    if (g_panel) {
        InvalidateRect(g_panel, NULL, FALSE);
        refresh_held();
    }
}

/* After a rebind: what it is now, and whom it was taken from. */
static void bound_hint(const char *what, const char *stolen)
{
    char msg[260];
    if (stolen && stolen[0]) snprintf(msg, sizeof msg, "Bound %s (was %s)", what, stolen);
    else snprintf(msg, sizeof msg, "Bound %s", what);
    set_hint(msg);
}

/* ---- layout --------------------------------------------------------------------- */

static int machine_now(void)
{
    const int mach = a800session_get_int(g_session, "machine", A800_M_130XE);
    return mach >= 0 && mach < A800_M_COUNT ? mach : A800_M_130XE;
}

/* The layout for what is attached now: the machine's ports and devices, and its
 * keyboard. */
static void fetch_layout(void)
{
    int port, idx;
    float width = 0, height = 0;
    g_machine = machine_now();
    g_xl = a800_machine_is_xl(g_machine);
    g_nports = a800session_active_ports(g_session);
    if (g_nports < 1) g_nports = 1;
    if (g_nports > A800_PORTS) g_nports = A800_PORTS;
    for (port = 0; port < A800_PORTS; port++)
        g_dev[port] = port < g_nports ? a800session_port_device(g_session, port) : A800_DEV_NONE;
    g_numlayout = a800session_controls_layout(g_dev, g_nports, g_xl, g_layout, A800_CONTROLS_MAX, &g_layw, &g_layh);
    g_family = a800session_keyboard_family(g_session);
    g_nkeys = a800session_keyboard_layout(g_session, &g_keys);
    if (g_nkeys > MAX_KEYS) g_nkeys = MAX_KEYS;
    for (idx = 0; idx < g_nkeys; idx++) {
        if (g_keys[idx].x + g_keys[idx].w > width) width = g_keys[idx].x + g_keys[idx].w;
        if (g_keys[idx].y + g_keys[idx].h > height) height = g_keys[idx].y + g_keys[idx].h;
    }
    g_kbd_w = width > 0 ? width : 16.0f;
    g_kbd_h = height > 0 ? height : 6.0f;
}

static int layout_changed(void)
{
    int port;
    if (machine_now() != g_machine || a800session_active_ports(g_session) != g_nports) return 1;
    for (port = 0; port < g_nports; port++)
        if (a800session_port_device(g_session, port) != g_dev[port]) return 1;
    return 0;
}

/* The mat with the controllers in it, the keyboard, the gamepad rows, the Map
 * row: the window is this size at this DPI. */
static void layout_static(void)
{
    const int left = SCL(MARGIN);
    int ypos = SCL(MARGIN), idx, xpos, content_w;
    static const int widths[SB_COUNT] = { 52, 52, 70, 104, 74, 52 };

    fetch_layout();
    g_unit = (double)UNIT96 * g_dpi / 96.0;
    content_w = (int)ceil((double)g_layw * g_unit) + 2 * SCL(CASE_PAD);
    g_total_w = 2 * left + content_w;

    SetRect(&g_case_rc, left, ypos, left + content_w, 0);
    SetRect(&g_brand_rc, g_case_rc.left + SCL(12), g_case_rc.top + SCL(8), g_case_rc.right - SCL(12),
            g_case_rc.top + SCL(8) + SCL(BRAND_H));
    g_orgx = g_case_rc.left + (content_w - (double)g_layw * g_unit) / 2;
    g_orgy = g_brand_rc.bottom + SCL(8);
    g_case_rc.bottom = (int)ceil(g_orgy + (double)g_layh * g_unit) + SCL(CASE_PAD + 2);
    ypos = g_case_rc.bottom + SCL(8);

    if (g_show_keyboard && g_keys) {
        const int avail = content_w - 2 * SCL(KBD_PAD);
        g_kunit = floor(avail / (double)g_kbd_w);
        SetRect(&g_kbd_rc, left, ypos, left + content_w, ypos + (int)ceil(g_kbd_h * g_kunit) + 2 * SCL(KBD_PAD));
        g_korgx = left + (content_w - g_kbd_w * g_kunit) / 2;
        g_korgy = ypos + SCL(KBD_PAD);
        ypos = g_kbd_rc.bottom + SCL(8);
    } else {
        SetRectEmpty(&g_kbd_rc);
    }
    if (IsRectEmpty(&g_kbd_rc)) g_art_rc = g_case_rc;
    else UnionRect(&g_art_rc, &g_case_rc, &g_kbd_rc);

    SetRect(&g_gp_head_rc, left, ypos, left + content_w, ypos + SCL(HEAD_H));
    ypos += SCL(HEAD_H);
    for (idx = 0; idx < PAD_ROWS; idx++) {
        SetRect(&g_gp_row_rc[idx], left, ypos, left + content_w, ypos + SCL(SEG_H));
        ypos += SCL(SEG_H + ROW_GAP);
    }
    SetRect(&g_gp_rc, left, g_gp_head_rc.top, left + content_w, ypos);
    ypos += SCL(4);

    xpos = left;
    for (idx = 0; idx < SB_HELP; idx++) {
        SetRect(&g_btn_rc[idx], xpos, ypos, xpos + SCL(widths[idx]), ypos + SCL(BTN_H));
        xpos += SCL(widths[idx]) + SCL(6);
    }
    SetRect(&g_btn_rc[SB_HELP], left + content_w - SCL(widths[SB_HELP]), ypos, left + content_w, ypos + SCL(BTN_H));
    SetRect(&g_combo_rc, xpos, ypos, g_btn_rc[SB_HELP].left - SCL(6), ypos + SCL(BTN_H));
    ypos += SCL(BTN_H) + SCL(6);
    SetRect(&g_hint_rc, left, ypos, left + content_w, ypos + SCL(20));
    g_total_h = ypos + SCL(20) + SCL(MARGIN);

    if (g_combo) {
        MoveWindow(g_combo, g_combo_rc.left, g_combo_rc.top, g_combo_rc.right - g_combo_rc.left,
                   SCL(BTN_H) + SCL(300), TRUE);
        SendMessageA(g_combo, WM_SETFONT, (WPARAM)g_font, TRUE);
    }
}

/* Exactly the laid-out client size, whatever the frame is at this DPI. */
static void fit_client(void)
{
    RECT wnd, client;
    GetWindowRect(g_panel, &wnd);
    GetClientRect(g_panel, &client);
    if (client.right == g_total_w && client.bottom == g_total_h) return;
    SetWindowPos(g_panel, NULL, 0, 0, (wnd.right - wnd.left) - client.right + g_total_w,
                 (wnd.bottom - wnd.top) - client.bottom + g_total_h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

/* The window's outer size for the laid-out client. */
static void window_size(HWND hwnd, SIZE *dims)
{
    RECT wnd, client;
    if (hwnd && GetWindowRect(hwnd, &wnd) && GetClientRect(hwnd, &client) && client.right > 0) {
        dims->cx = (wnd.right - wnd.left) - client.right + g_total_w;
        dims->cy = (wnd.bottom - wnd.top) - client.bottom + g_total_h;
    } else {
        SetRect(&wnd, 0, 0, g_total_w, g_total_h);
        AdjustWindowRectEx(&wnd, PANEL_STYLE, FALSE, PANEL_EXSTYLE);
        dims->cx = wnd.right - wnd.left;
        dims->cy = wnd.bottom - wnd.top;
    }
}

static void rebuild(void)
{
    mouse_release();
    if (g_panel && GetCapture() == g_panel) ReleaseCapture();
    make_fonts();
    layout_static();
    g_hover_part = -1;
    g_hover_key = -1;
    if (g_map_state >= 0) set_map_state(-1);
    memset(&g_shown, 0, sizeof g_shown);
    if (g_panel) {
        fit_client();
        InvalidateRect(g_panel, NULL, FALSE);
        refresh_held();
    }
}

static void rescale(int dpi)
{
    g_dpi = dpi;
    rebuild();
}

/* ---- polling -------------------------------------------------------------------------- */

/* What the machine sees held, lit. */
static void refresh_held(void)
{
    held_state now, one, two;
    int port, pdl, idx;
    if (!g_panel) return;
    memset(&now, 0, sizeof now);
    for (port = 0; port < A800_PORTS; port++) {
        now.held[port] = a800session_buttons_held(g_session, port);
        for (pdl = 0; pdl < 2; pdl++) now.paddle[port][pdl] = a800session_paddle_value(g_session, port, pdl);
    }
    now.console = a800session_console_held(g_session);
    for (idx = 0; idx < g_nkeys && idx < MAX_KEYS; idx++)
        now.key[idx] = (unsigned char)(a800session_keyboard_held(g_session, g_keys[idx].index) != 0);
    if (memcmp(&now, &g_shown, sizeof now) != 0) {
        one = now;
        two = g_shown;
        memset(one.key, 0, sizeof one.key);
        memset(two.key, 0, sizeof two.key);
        if (memcmp(&one, &two, sizeof one) != 0) invalidate_view();
        if (memcmp(now.key, g_shown.key, sizeof now.key) != 0) invalidate_keyboard();
        g_shown = now;
    }
}

/* Hot-plugged pads, auto-assignment changes, and the machine or a port's
 * device changed in Settings. */
static void refresh_pads(void)
{
    pads_state now;
    int idx;
    if (layout_changed()) rebuild();
    memset(&now, 0, sizeof now);
    now.generation = a800session_gamepad_generation(g_session);
    now.ports = g_nports;
    for (idx = 0; idx < A800_PORTS; idx++) now.devices[idx] = g_dev[idx];
    now.count = a800session_gamepad_count(g_session);
    for (idx = 0; idx < PAD_ROWS && idx < now.count; idx++) {
        now.assign[idx] = a800session_gamepad_assignment(g_session, idx);
        now.effective[idx] = a800session_gamepad_effective_port(g_session, idx);
    }
    if (memcmp(&now, &g_pads_shown, sizeof now) != 0) {
        g_pads_shown = now;
        InvalidateRect(g_panel, &g_gp_rc, FALSE);
    }
}

/* ---- keys ------------------------------------------------------------------------------ */

static void hide_panel(void)
{
    set_map_state(-2);
    mouse_release();
    if (GetCapture() == g_panel) ReleaseCapture();
    g_swallow_vk = 0;
    a800session_release_all(g_session);
    ShowWindow(g_panel, SW_HIDE);
}

static int is_modifier(WPARAM vk)
{
    return vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU
        || vk == VK_LWIN || vk == VK_RWIN;
}

/* A key while Map is armed: 1 if Map took it. Waiting for a target, a key is
 * not typing; waiting for a binding, the key is the binding (not F9, F11 or
 * F12, which are the app's). Releases go on to the Atari, so a key held when Map
 * was armed does not stick. */
static int map_key(UINT msg, WPARAM vk, LPARAM lp)
{
    const int down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
    if (g_map_state == -2 || !down) return 0;
    if (vk == VK_F11 || vk == VK_F12 || vk == VK_F10) return 0;
    if (msg == WM_SYSKEYDOWN && vk == VK_F4) return 0;
    if (GetKeyState(VK_CONTROL) & 0x8000) return 0;       /* Ctrl chords are the app's */
    if (lp & (1 << 30)) return 1;
    if (vk == VK_ESCAPE) {
        set_map_state(g_map_state >= 0 ? -1 : -2);
        g_swallow_vk = vk;
        return 1;
    }
    if (g_map_state >= 0) {
        char stolen[128], name[48];
        const int target = g_map_state;
        const uint32_t keysym = a800_keysym_from_msg(vk, lp);
        if (!keysym) return 1;          /* nothing to call it: keep waiting */
        if (a800session_key_reserved(keysym)) {
            set_hint("F9, F11 and F12 are the app's and cannot be bound.");
            g_swallow_vk = vk;
            return 1;
        }
        stolen[0] = '\0';
        a800session_binding_set_key(g_session, target, keysym, stolen, sizeof stolen);
        a800session_keysym_name(keysym, name, sizeof name);
        g_swallow_vk = vk;
        set_map_state(-1);              /* stay armed: remapping several in a row is normal */
        bound_hint(name, stolen);
    }
    return 1;
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
    if (map_key(msg, vk, lp)) return 1;
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

/* ---- the mouse ------------------------------------------------------------------------ */

/* The layout part under a client point, or -1. */
static int part_at(POINT pnt)
{
    if (!g_numlayout) return -1;
    return a800session_controls_hit(g_layout, g_numlayout, (float)((pnt.x - g_orgx) / g_unit), (float)((pnt.y - g_orgy) / g_unit));
}

/* The keyboard cap under a client point, or -1. */
static int key_at(POINT pnt)
{
    int idx;
    if (!g_show_keyboard || !g_keys || !PtInRect(&g_kbd_rc, pnt)) return -1;
    for (idx = 0; idx < g_nkeys; idx++) {
        RECT rect;
        key_rect(&g_keys[idx], &rect);
        if (PtInRect(&rect, pnt)) return idx;
    }
    return -1;
}

/* The binding target Map picks for a click on a part, or -1 for a part that has none. */
static int map_target_for(const a800_control *ctl, POINT pnt)
{
    double xpos, ypos, width, height;
    switch (ctl->kind) {
    case A800_CTL_ROUND_BUTTON:
    case A800_CTL_CONSOLE_KEY:
        return ctl->target;
    case A800_CTL_STICK: {
        /* which side of the ring: the larger of the two offsets from its middle */
        double delx, dely;
        part_box(ctl, &xpos, &ypos, &width, &height);
        delx = (pnt.x - (xpos + width / 2)) / width;
        dely = (pnt.y - (ypos + height / 2)) / height;
        if (fabs(delx) > fabs(dely)) return A800_TARGET_PORT(ctl->port, delx < 0 ? A800_ACT_LEFT : A800_ACT_RIGHT);
        return A800_TARGET_PORT(ctl->port, dely < 0 ? A800_ACT_UP : A800_ACT_DOWN);
    }
    default:
        return -1;
    }
}

static void toggle_keyboard(void)
{
    g_show_keyboard = !g_show_keyboard;
    a800session_set_int(g_session, "controllers_keyboard", g_show_keyboard);
    mouse_release();
    layout_static();
    memset(&g_shown, 0, sizeof g_shown);
    if (g_panel) {
        fit_client();
        InvalidateRect(g_panel, NULL, FALSE);
        refresh_held();
    }
}

static void mouse_down(HWND hwnd, POINT pnt)
{
    int part, row, seg, btn, key;
    SetFocus(hwnd);
    if (g_hover_part >= 0 || g_hover_key >= 0) {
        g_hover_part = -1;
        g_hover_key = -1;
        InvalidateRect(hwnd, &g_hint_rc, FALSE);
    }

    for (btn = 0; btn < SB_COUNT; btn++) {
        if (!PtInRect(&g_btn_rc[btn], pnt)) continue;
        switch (btn) {
        case SB_MAP: set_map_state(g_map_state == -2 ? -1 : -2); return;
        case SB_CLEAR:
            if (g_map_state >= 0) {
                a800session_binding_clear(g_session, g_map_state);
                set_hint("Cleared. Press a key or a gamepad button to bind it again");
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return;
        case SB_DEFAULTS:
            a800session_bindings_reset(g_session);
            set_hint("Every key and button is back to its default");
            InvalidateRect(hwnd, NULL, FALSE);
            return;
        case SB_SHOW:
            g_show_bindings = !g_show_bindings;
            a800session_set_int(g_session, "controllers_show_bindings", g_show_bindings);
            InvalidateRect(hwnd, NULL, FALSE);
            return;
        case SB_KEYBOARD: toggle_keyboard(); return;
        default: a800_help_show(hwnd, "controllers"); return;
        }
    }
    for (row = 0; row < PAD_ROWS && row < a800session_gamepad_count(g_session); row++)
        for (seg = 0; seg < segment_count(); seg++) {
            RECT rect;
            segment_rect(row, seg, &rect);
            if (PtInRect(&rect, pnt)) {
                /* 0 = automatic (connection order), 1.. = that port */
                a800session_gamepad_assign(g_session, row, seg - 1);
                refresh_pads();
                InvalidateRect(hwnd, &g_gp_rc, FALSE);
                return;
            }
        }

    key = key_at(pnt);
    if (key >= 0) {
        if (g_map_state != -2) {
            const int target = key_target(&g_keys[key]);
            if (target >= 0) set_map_state(target);
            return;
        }
        mouse_release();
        g_mouse_key = key;
        a800session_keyboard_press(g_session, g_keys[key].index, 1);
        SetCapture(hwnd);
        refresh_held();
        invalidate_keyboard();
        return;
    }

    part = part_at(pnt);
    if (part < 0) return;
    {
        const a800_control *ctl = &g_layout[part];
        if (g_map_state != -2) {
            /* armed: picks (or changes) the target */
            const int target = map_target_for(ctl, pnt);
            if (target >= 0) set_map_state(target);
            else if (ctl->kind == A800_CTL_KNOB || ctl->kind == A800_CTL_TURN || ctl->kind == A800_CTL_PADDLE_FIRE)
                set_hint("Paddles have no bindings: the knob and its turn arrows drive them.");
            return;
        }
        mouse_release();
        switch (ctl->kind) {
        case A800_CTL_STICK:
            g_mouse_stick = part;
            stick_follow(ctl, pnt);
            break;
        case A800_CTL_KNOB:
            g_mouse_knob = part;
            knob_follow(ctl, pnt);
            break;
        case A800_CTL_PADDLE_FIRE:
            g_mouse_part = part;
            a800session_paddle_fire(g_session, ctl->port, ctl->arg & 1, 1);
            break;
        case A800_CTL_TURN:
            g_mouse_part = part;
            a800session_paddle_turn(g_session, ctl->port, ctl->arg & 1, ctl->dir);
            break;
        case A800_CTL_ROUND_BUTTON:
        case A800_CTL_CONSOLE_KEY:
            g_mouse_part = part;
            a800session_press(g_session, ctl->target, 1);
            break;
        case A800_CTL_POINTER_BUTTON:
            g_mouse_part = part;
            g_ptr_port = ctl->port;
            g_ptr_mask |= (unsigned)ctl->arg;
            a800session_pointer_motion(g_session, 0, 0, g_ptr_mask);
            break;
        case A800_CTL_MAT:
        case A800_CTL_BALL:
            g_mouse_mat = part;
            g_mat_last = pnt;
            g_mat_carry_x = g_mat_carry_y = 0;
            break;
        case A800_CTL_PAD:
            g_mouse_pad = part;
            pad_follow(ctl, pnt);
            break;
        case A800_CTL_CX85_KEY:
            g_mouse_part = part;
            a800session_cx85_key(g_session, ctl->arg, 1);
            break;
        default:
            return;
        }
        invalidate_part(part);
        SetCapture(hwnd);
        refresh_held();
        invalidate_view();
    }
}

static void mouse_move(HWND hwnd, POINT pnt)
{
    if (g_mouse_stick >= 0 && g_mouse_stick < g_numlayout) {
        stick_follow(&g_layout[g_mouse_stick], pnt);
        invalidate_view();
        return;
    }
    if (g_mouse_knob >= 0 && g_mouse_knob < g_numlayout) {
        knob_follow(&g_layout[g_mouse_knob], pnt);
        return;
    }
    if (g_mouse_mat >= 0 && g_mouse_mat < g_numlayout) {
        mat_follow(pnt);
        return;
    }
    if (g_mouse_pad >= 0 && g_mouse_pad < g_numlayout) {
        pad_follow(&g_layout[g_mouse_pad], pnt);
        return;
    }
    if (g_mouse_key >= 0) {
        /* dragged off the cap: let go, as a key does */
        if (key_at(pnt) != g_mouse_key) {
            mouse_release();
            if (GetCapture() == hwnd) ReleaseCapture();
            refresh_held();
        }
        return;
    }
    if (g_mouse_part >= 0) {
        /* dragged off what it holds: let go, as a button does */
        if (g_mouse_part >= g_numlayout || part_at(pnt) != g_mouse_part) {
            mouse_release();
            if (GetCapture() == hwnd) ReleaseCapture();
            refresh_held();
        }
        return;
    }
    {   /* idle: the hint line says what drives the part under the pointer */
        const int key = key_at(pnt);
        const int part = key >= 0 ? -1 : part_at(pnt);
        if (!g_tracking) {
            TRACKMOUSEEVENT track;
            memset(&track, 0, sizeof track);
            track.cbSize = sizeof track;
            track.dwFlags = TME_LEAVE;
            track.hwndTrack = hwnd;
            g_tracking = TrackMouseEvent(&track) != 0;
        }
        if (part != g_hover_part || key != g_hover_key) {
            g_hover_part = part;
            g_hover_key = key;
            InvalidateRect(hwnd, &g_hint_rc, FALSE);
        }
    }
}

static void mouse_up(HWND hwnd)
{
    if (g_mouse_part >= 0 || g_mouse_stick >= 0 || g_mouse_knob >= 0 || g_mouse_mat >= 0 || g_mouse_pad >= 0 || g_mouse_key >= 0) {
        mouse_release();
        refresh_held();
    }
    if (GetCapture() == hwnd) ReleaseCapture();
}

/* The wheel over a knob nudges it, and the position stays. */
static void mouse_wheel(HWND hwnd, int delta, POINT screen)
{
    POINT pnt = screen;
    int part;
    ScreenToClient(hwnd, &pnt);
    if (g_map_state != -2) return;
    part = part_at(pnt);
    if (part < 0 || g_layout[part].kind != A800_CTL_KNOB) return;
    {
        const a800_control *ctl = &g_layout[part];
        int value = a800session_paddle_value(g_session, ctl->port, ctl->arg & 1) - (int)((long)delta * PADDLE_STEP / WHEEL_DELTA);
        if (value < 0) value = 0;
        if (value > A800_PADDLE_MAX) value = A800_PADDLE_MAX;
        a800session_paddle_pointer(g_session, ctl->port, ctl->arg & 1, value, 1);
        a800session_paddle_pointer(g_session, ctl->port, ctl->arg & 1, value, 0);
        refresh_held();
    }
}

/* ---- the window ----------------------------------------------------------------------- */

static LRESULT CALLBACK panel_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT paint;
        HDC hdc = BeginPaint(hwnd, &paint);
        /* Off-screen first: the held highlights repaint at 25 Hz. */
        RECT inval = paint.rcPaint;
        HDC mem;
        HBITMAP bmp;
        HGDIOBJ oldbmp, oldfont;
        const int width = inval.right - inval.left, height = inval.bottom - inval.top;
        if (width <= 0 || height <= 0) { EndPaint(hwnd, &paint); return 0; }
        mem = CreateCompatibleDC(hdc);
        bmp = CreateCompatibleBitmap(hdc, width, height);
        oldbmp = SelectObject(mem, bmp);
        oldfont = SelectObject(mem, g_font);
        SetViewportOrgEx(mem, -inval.left, -inval.top, NULL);
        paint_rect(mem, &inval);
        SetViewportOrgEx(mem, 0, 0, NULL);
        BitBlt(hdc, inval.left, inval.top, width, height, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldfont);
        SelectObject(mem, oldbmp);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &paint);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;

    case WM_LBUTTONDOWN: {
        POINT pnt;
        pnt.x = GET_X_LPARAM(lparam);
        pnt.y = GET_Y_LPARAM(lparam);
        mouse_down(hwnd, pnt);
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT pnt;
        pnt.x = GET_X_LPARAM(lparam);
        pnt.y = GET_Y_LPARAM(lparam);
        mouse_move(hwnd, pnt);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        POINT pnt;
        pnt.x = GET_X_LPARAM(lparam);
        pnt.y = GET_Y_LPARAM(lparam);
        mouse_wheel(hwnd, GET_WHEEL_DELTA_WPARAM(wparam), pnt);
        return 0;
    }
    case WM_MOUSELEAVE:
        g_tracking = 0;
        if (g_hover_part >= 0 || g_hover_key >= 0) {
            g_hover_part = -1;
            g_hover_key = -1;
            InvalidateRect(hwnd, &g_hint_rc, FALSE);
        }
        return 0;
    case WM_LBUTTONUP:
        /* released wherever the mouse ended up: capture means this arrives
         * even if the pointer left the part */
        mouse_up(hwnd);
        return 0;
    case WM_CAPTURECHANGED:
        /* lost the mouse some other way (Alt+Tab mid-press): let go */
        if ((HWND)lparam != hwnd) {
            mouse_release();
            refresh_held();
        }
        return 0;

    case WM_COMMAND:
        if (LOWORD(wparam) == IDC_CTRL_TARGETS && HIWORD(wparam) == CBN_SELCHANGE) {
            const int sel = (int)SendMessageA(g_combo, CB_GETCURSEL, 0, 0);
            if (sel > 0 && g_map_state != -2) set_map_state(sel - 1);
            SetFocus(hwnd);
        }
        return 0;

    case WM_TIMER:
        if (wparam == IDT_CAPTURE) {
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
        } else if (wparam == IDT_HELD) {
            refresh_held();
        } else if (wparam == IDT_PADS) {
            refresh_pads();
        }
        return 0;

    /* WM_SYSKEY* too: Alt chords are the main window's */
    case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_KEYUP: case WM_SYSKEYUP:
        if (panel_key(msg, wparam, lparam)) return 0;
        break;
    case WM_CHAR:
        return 0;           /* typing arrives as keys, not characters */
    case WM_SYSCHAR:
        return 0;           /* Alt+letter is not a menu mnemonic here */

    case WM_ACTIVATE:
        if (LOWORD(wparam) == WA_INACTIVE) {
            mouse_release();
            if (GetCapture() == hwnd) ReleaseCapture();
            g_swallow_vk = 0;       /* its release goes elsewhere */
            a800session_release_all(g_session);
            refresh_held();
        }
        break;                      /* DefWindowProc gives the window the focus */
    case WM_SHOWWINDOW:
        if (wparam) {
            refresh_held();
            SetTimer(hwnd, IDT_HELD, 40, NULL);
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
            MINMAXINFO *limits = (MINMAXINFO *)lparam;
            SIZE dims;
            window_size(hwnd, &dims);
            limits->ptMinTrackSize.x = limits->ptMaxTrackSize.x = limits->ptMaxSize.x = dims.cx;
            limits->ptMinTrackSize.y = limits->ptMaxTrackSize.y = limits->ptMaxSize.y = dims.cy;
            return 0;
        }
        break;
    case WM_DPICHANGED: {
        const RECT *suggested = (const RECT *)lparam;
        rescale(HIWORD(wparam));
        SetWindowPos(hwnd, NULL, suggested->left, suggested->top, suggested->right - suggested->left,
                     suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
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
        g_combo = NULL;
        free_fonts();
        break;
    default:
        break;
    }
    return DefWindowProcA(hwnd, msg, wparam, lparam);
}

static int create_panel(void)
{
    static int registered;
    HINSTANCE inst = GetModuleHandleA(NULL);
    RECT want;
    int idx;

    g_show_bindings = a800session_get_int(g_session, "controllers_show_bindings", 0) != 0;
    g_show_keyboard = a800session_get_int(g_session, "controllers_keyboard", 1) != 0;

    if (!registered) {
        WNDCLASSEXA wcls;
        memset(&wcls, 0, sizeof wcls);
        wcls.cbSize = sizeof wcls;
        wcls.lpfnWndProc = panel_proc;      /* no CS_DBLCLKS: fast clicks stay press/release pairs */
        wcls.hInstance = inst;
        wcls.hCursor = LoadCursor(NULL, IDC_ARROW);
        wcls.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wcls.lpszClassName = PANEL_CLASS;
        registered = RegisterClassExA(&wcls) != 0;
    }
    g_dpi = window_dpi(g_parent);
    make_fonts();
    layout_static();

    SetRect(&want, 0, 0, g_total_w, g_total_h);
    /* A tool window with a caption and no thick frame or maximize box: it is
     * exactly the size of its contents, and stays off the taskbar. */
    AdjustWindowRectEx(&want, PANEL_STYLE, FALSE, PANEL_EXSTYLE);
    g_panel = CreateWindowExA(PANEL_EXSTYLE, PANEL_CLASS, "Controllers", PANEL_STYLE,
                              CW_USEDEFAULT, CW_USEDEFAULT, want.right - want.left, want.bottom - want.top,
                              g_parent, NULL, inst, NULL);
    if (!g_panel) return 0;
    /* the target list: every binding target */
    g_combo = CreateWindowExA(0, "COMBOBOX", "", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST,
                              g_combo_rc.left, g_combo_rc.top, g_combo_rc.right - g_combo_rc.left, SCL(BTN_H) + SCL(300),
                              g_panel, (HMENU)(INT_PTR)IDC_CTRL_TARGETS, inst, NULL);
    if (g_combo) {
        SendMessageA(g_combo, CB_ADDSTRING, 0, (LPARAM)"Pick a target to remap...");
        for (idx = 0; idx < A800_TARGET_COUNT; idx++)
            SendMessageA(g_combo, CB_ADDSTRING, 0, (LPARAM)a800_target_name(idx));
        SendMessageA(g_combo, CB_SETCURSEL, 0, 0);
        SendMessageA(g_combo, CB_SETDROPPEDWIDTH, (WPARAM)SCL(260), 0);
        SendMessageA(g_combo, WM_SETFONT, (WPARAM)g_font, TRUE);
        EnableWindow(g_combo, FALSE);
    }
    a800_help_set_context(g_panel, "controllers", 0);    /* keys here go to the Atari */
    rescale(window_dpi(g_panel));    /* placed on another monitor than the main window */
    fit_client();
    memset(&g_pads_shown, 0, sizeof g_pads_shown);
    memset(&g_shown, 0, sizeof g_shown);
    g_hover_part = -1;
    g_hover_key = -1;
    set_map_state(-2);
    return 1;
}

static void show_panel(void)
{
    if (layout_changed()) rebuild();
    refresh_pads();
    ShowWindow(g_panel, SW_SHOW);
    SetForegroundWindow(g_panel);
}

void a800_controller_window_show(HWND parent, a800session *session)
{
    g_session = session;
    if (parent)
        g_parent = parent;
    if (!g_panel && !create_panel())
        return;
    if (!IsWindowVisible(g_panel))
        show_panel();
    else
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
        g_machine = -1;             /* rebuilt when next shown */
    }
}

int a800_controller_pretranslate(MSG *msg)
{
    /* The target list is the window's only child: it keeps its own keys. */
    (void)msg;
    return 0;
}
