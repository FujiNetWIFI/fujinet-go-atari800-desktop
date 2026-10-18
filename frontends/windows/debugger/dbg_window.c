/*
 * Debugger window (Win32) over the session's debugger engine and atari800's
 * own monitor, via core/include/a800debug.h. Mirrors the GTK and Qt
 * debuggers tab for tab -- Console, CPU & Memory, Disassembly, ANTIC, GTIA,
 * POKEY & PIA, Memory Map, Breakpoints, SIO -- built from plain common
 * controls, with the disassembly, the colour registers and the palette
 * drawn by hand.
 *
 * The Console is atari800's monitor a line at a time, with Tab completion
 * and a command history; its A assembler prompts for lines. A click on a
 * disassembly line selects it for Run to Cursor; a click on its left edge,
 * or a double-click, toggles its breakpoint. The toolbar carries the
 * stepping controls, the snapshots, symbol loading and file saves; the
 * family's keys apply (F5 run/stop, F7 step, F8 step over, Shift+F8 step
 * out, F9 frame, F12 close).
 *
 * Showing this window attaches (which stops the machine, as on every
 * sibling); hiding it detaches and lets the machine run on. A timer polls
 * the engine's generation counter and refreshes the tab showing only when
 * something changed; while the machine runs it refreshes the status, the
 * registers and the SIO trace twice a second. At each stop the console
 * takes the banner and registers atari800 printed, as its own monitor
 * shows them.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "dbg_window.h"

#include <commctrl.h>
#include <commdlg.h>
#include <windowsx.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "a800debug.h"

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

#define DISASM_WINDOW 64        /* lines fetched; as many show as fit */
#define MEM_ROWS 256            /* 4K of memory, 16 bytes a row */
#define HISTORY_MAX 64          /* the last instructions shown */
#define CMD_HISTORY 100         /* console lines Up/Down recall */
#define SIO_MAX 512
#define BP_MAX 64
#define CONSOLE_KEEP (256 * 1024)
#define SNAPSHOTS 4
#define SWATCHES 9
#define NOTICE_MS 4000          /* how long a notice holds the status line */

/* Sizes at 96 dpi; S() scales them to the window's. */
#define PAL_AXIS_W 30           /* the palette's hue labels */
#define PAL_AXIS_H 18           /* ... and its luma labels */
#define PAL_CELL_W 28
#define PAL_CELL_H 16
#define COL_TITLE_H 22          /* the GTIA colour panel: a title's row */
#define COL_ROW_H 26            /* ... and a row of swatches */
#define DIS_MARGIN 6            /* the disassembly's left margin */
#define MONO_PX 13              /* the monospace font */

#define WM_DBG_ACCEPT  (WM_APP + 2)   /* wp: control id -- Enter in an edit */
#define WM_DBG_TAB     (WM_APP + 3)   /* Tab in the console: complete */
#define WM_DBG_HISTORY (WM_APP + 4)   /* wp: 0 Up (older), 1 Down (newer) */
#define WM_DBG_SYNC    (WM_APP + 5)   /* resync once a list's notification is over */

#define TIMER_REFRESH 1

enum { TAB_CONSOLE, TAB_CPU, TAB_DISASM, TAB_ANTIC, TAB_GTIA, TAB_POKEY, TAB_MAP,
       TAB_BREAKS, TAB_SIO, TAB_COUNT };

/* The tabs, in order: the names A800_DEBUGGER_TAB takes, and their labels
 * (which it takes too). */
static const struct { const char *name, *label; } k_tabs[TAB_COUNT] = {
    { "console", "Console" }, { "cpu", "CPU & Memory" }, { "disassembly", "Disassembly" },
    { "antic", "ANTIC" }, { "gtia", "GTIA" }, { "pokey", "POKEY & PIA" },
    { "map", "Memory Map" }, { "breakpoints", "Breakpoints" }, { "sio", "SIO" },
};

enum {
    IDC_RUN = 1000, IDC_STEP, IDC_OVER, IDC_OUT, IDC_FRAME, IDC_RUN_TO, IDC_BRK,
    IDC_STATUS, IDC_SNAPSHOTS, IDC_SYMBOLS, IDC_SAVE, IDC_TABS,
    IDC_CON_OUT, IDC_CON_PROMPT, IDC_CON_IN,
    IDC_REG0, IDC_REG_LAST = IDC_REG0 + 5,
    IDC_FLAG0, IDC_FLAG_LAST = IDC_FLAG0 + 5,
    IDC_MEM_VIEW, IDC_HIST_VIEW, IDC_MEM_GOTO, IDC_MEM_BACK, IDC_MEM_ON, IDC_MEM_ADDR, IDC_MEM_VAL,
    IDC_FOLLOW_PC, IDC_JUMP, IDC_DISASM,
    IDC_ANTIC_TEXT, IDC_DLIST_TEXT, IDC_GTIA_TEXT, IDC_COLOURS, IDC_PALETTE_INFO,
    IDC_POKEY_TEXT, IDC_MAP_TEXT,
    IDC_BP_LIST, IDC_BP_REMOVE, IDC_BP_CLEAR, IDC_BP_TYPE, IDC_BP_START, IDC_BP_END, IDC_BP_COND,
    IDC_BP_ADD, IDC_BP_LINE, IDC_BP_LINE_SET,
    IDC_SIO_CLEAR, IDC_SIO_TEXT,
    IDC_SNAP_SAVE0, IDC_SNAP_SAVE_LAST = IDC_SNAP_SAVE0 + SNAPSHOTS - 1,
    IDC_SNAP_RESTORE0, IDC_SNAP_RESTORE_LAST = IDC_SNAP_RESTORE0 + SNAPSHOTS - 1,
    IDC_SAVE_DIS, IDC_SAVE_RAM,
    IDC_LABEL_FIRST
};

static const int k_regs[6] = { A800_REG_PC, A800_REG_SP, A800_REG_A, A800_REG_X,
                               A800_REG_Y, A800_REG_P };
static const int k_flags[6] = { A800_FLAG_N, A800_FLAG_V, A800_FLAG_D, A800_FLAG_I,
                                A800_FLAG_Z, A800_FLAG_C };
static const char *const k_reg_names[6] = { "PC", "SP", "A", "X", "Y", "P" };
static const char *const k_flag_names[6] = { "N", "V", "D", "I", "Z", "C" };

static const char k_fujinet_note[] =
    "FujiNet is running: restoring a snapshot replays the SIO traffic since it was "
    "taken, which FujiNet has already acted on (sectors written, files and "
    "connections opened), so the Atari and FujiNet can disagree afterwards.";

/* Save...: what, the dialog's title, and the name it offers. */
static const struct { const char *kind, *title, *file; } k_saves[2] = {
    { "dis", "Disassembly (256 lines from the PC)", "disassembly.asm" },
    { "ram", "Memory (64K as the CPU sees it)", "memory.bin" },
};

typedef struct {
    HWND hwnd, tabs, tip;
    HWND run_btn, step_btn, over_btn, out_btn, frame_btn, run_to_btn, brk_btn, status;
    HWND snap_btn, sym_btn, save_btn;

    /* console */
    HWND con_out, con_prompt, con_in;
    int con_asm;                /* the A assembler wants its next line */
    char con_prompt_text[32];
    char history[CMD_HISTORY][256];
    int nhistory, history_pos;

    /* cpu + memory */
    HWND reg_label[6], reg[6], flag[6], beam;
    HWND mem_title, mem_view, hist_title, hist_view;
    HWND goto_label, mem_goto, mem_back, mem_on, write_label, mem_addr, value_label, mem_val, mem_hint;
    int mem_top;
    int mem_mark;               /* the address Go to found; -1 none */

    /* disassembly */
    HWND follow_pc, jump_label, jump, disasm_hint, disasm;
    int disasm_top;             /* address of the first line */
    int selected;               /* the selected line's address, or -1 */
    a800debug_line lines[DISASM_WINDOW];
    int line_count;

    /* antic */
    HWND antic_title, antic_text, dlist_title, dlist_text;

    /* gtia */
    HWND gtia_text, colours, palette_info;
    uint8_t swatch_val[SWATCHES];
    uint32_t swatch_rgb[SWATCHES];
    uint32_t palette_px[A800DEBUG_PALETTE_WIDTH * A800DEBUG_PALETTE_HEIGHT];
    int palette_ok;

    /* pokey + pia, the memory map */
    HWND pokey_text, map_text;

    /* breakpoints */
    HWND bp_budget, bp_remove, bp_clear, bp_list, bp_empty;
    HWND bp_type, bp_start, bp_end, bp_cond, bp_add;
    HWND bp_line_label, bp_line, bp_line_set, bp_line_hint, bp_msg;
    a800debug_breakpoint bps[BP_MAX];
    int nbps;
    char bp_sig[BP_MAX * 160];  /* what the list shows, to rebuild on change */
    int bp_sig_ok;

    /* sio */
    HWND sio_note, sio_clear, sio_text;
    int sio_count;
    a800debug_sio sio_last;

    HWND views[10];             /* the read-only text views, drawn on the window colour */
    int nviews;
    HWND dim[40];               /* captions and hints, drawn grey */
    int ndim, nlabels;

    HFONT ui, ui_bold, mono, mono_bold, mono_sel;
    int dpi;
    int mono_w, mono_h, line_h;
    HACCEL accel;
    HBRUSH accent;

    a800session *session;
    a800debug *dbg;

    int page;
    unsigned seen_generation;
    int was_stopped;
    int attached_running;       /* attached while the machine was running */
    int running_ticks;
    int updating;               /* a refresh is filling the breakpoint list */

    char notice[200];           /* a message holding the status line a moment */
    DWORD notice_until;
} debugger;

static debugger *g_dbg;

/* ---- helpers ---------------------------------------------------------------- */

static void set_text(HWND h, const char *text) { SetWindowTextA(h, text); }
static void edit_text(HWND h, char *out, int outsz) { GetWindowTextA(h, out, outsz); }

/* A label set only when it changed (no flicker on the live refresh). */
static void set_label(HWND h, const char *text)
{
    char cur[512];
    GetWindowTextA(h, cur, sizeof cur);
    if (strcmp(cur, text) != 0) set_text(h, text);
}

/* EDIT controls want CRLF and render control characters as boxes. */
static char *to_crlf(const char *text)
{
    size_t n = strlen(text), i, o = 0;
    char *buf = malloc(n * 2 + 1);
    if (!buf) return NULL;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c == '\n') { buf[o++] = '\r'; buf[o++] = '\n'; }
        else if (c >= 0x20 || c == '\t') buf[o++] = (char)c;
    }
    buf[o] = '\0';
    return buf;
}

static size_t addf(char *s, size_t len, size_t cap, const char *fmt, ...)
{
    va_list ap;
    int n;
    if (len >= cap) return len;
    va_start(ap, fmt);
    n = vsnprintf(s + len, cap - len, fmt, ap);
    va_end(ap);
    if (n < 0) return len;
    return len + (size_t)n < cap ? len + (size_t)n : cap - 1;
}

/* Blanks off both ends, in place. */
static char *strip(char *s)
{
    char *e;
    while (*s == ' ' || *s == '\t') s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = '\0';
    return s;
}

static const char *base_name(const char *path)
{
    const char *a = strrchr(path, '\\'), *b = strrchr(path, '/');
    if (b && (!a || b > a)) a = b;
    return a ? a + 1 : path;
}

/* $hex, 0xhex, #dec, or bare hex, as the monitor reads numbers. */
static int parse_num(const char *text, long *out)
{
    const char *p = text;
    char *end;
    long v;
    int base = 16;
    while (*p == ' ') p++;
    if (*p == '$') p++;
    else if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
    else if (*p == '#') { p++; base = 10; }
    v = strtol(p, &end, base);
    if (end == p) return 0;
    while (*end == ' ') end++;
    if (*end) return 0;
    *out = v;
    return 1;
}

/* A label or a number, as typed into an address box. */
static int parse_addr(debugger *d, const char *text, long *out)
{
    int a;
    while (*text == ' ') text++;
    if (!*text) return 0;
    a = a800debug_label_address(d->dbg, text);
    if (a >= 0) { *out = a; return 1; }
    return parse_num(text, out);
}

/* ---- scale and fonts --------------------------------------------------------- */

static int S(const debugger *d, int v)
{
    return MulDiv(v, d->dpi, 96);
}

static int system_dpi(void)
{
    HDC dc = GetDC(NULL);
    int v = dc ? GetDeviceCaps(dc, LOGPIXELSY) : 96;
    if (dc) ReleaseDC(NULL, dc);
    return v > 0 ? v : 96;
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
        UINT v = fn(hwnd);
        if (v) return (int)v;
    }
    return system_dpi();
}

static HFONT font_like(const LOGFONTA *base, int px, int weight, int underline, const char *face)
{
    LOGFONTA lf = *base;
    lf.lfHeight = -px;
    lf.lfWidth = 0;
    lf.lfWeight = weight;
    lf.lfUnderline = (BYTE)underline;
    if (face) {
        snprintf(lf.lfFaceName, sizeof lf.lfFaceName, "%s", face);
        lf.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
    }
    return CreateFontIndirectA(&lf);
}

static BOOL CALLBACK refont_child(HWND h, LPARAM lp)
{
    const HFONT *map = (const HFONT *)lp;     /* old ui, bold, mono; new ui, bold, mono */
    const HFONT f = (HFONT)SendMessageA(h, WM_GETFONT, 0, 0);
    int i;
    for (i = 0; i < 3; i++)
        if (f && f == map[i]) SendMessageA(h, WM_SETFONT, (WPARAM)map[i + 3], TRUE);
    return TRUE;
}

/* The message font (system metrics are at the system DPI) and Consolas, at
 * the window's DPI; the controls already made take the new ones. */
static void make_fonts(debugger *d)
{
    NONCLIENTMETRICSA ncm;
    LOGFONTA lf;
    HFONT old[5] = { d->ui, d->ui_bold, d->mono, d->mono_bold, d->mono_sel };
    HDC dc;
    TEXTMETRICA tm;
    HGDIOBJ prev;
    int px, i;

    memset(&ncm, 0, sizeof ncm);
    ncm.cbSize = sizeof ncm;
    if (SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0)) lf = ncm.lfMessageFont;
    else GetObjectA(GetStockObject(DEFAULT_GUI_FONT), sizeof lf, &lf);
    px = MulDiv(abs(lf.lfHeight ? lf.lfHeight : 12), d->dpi, system_dpi());
    d->ui = font_like(&lf, px, FW_NORMAL, 0, NULL);
    d->ui_bold = font_like(&lf, px, FW_BOLD, 0, NULL);
    d->mono = font_like(&lf, S(d, MONO_PX), FW_NORMAL, 0, "Consolas");
    d->mono_bold = font_like(&lf, S(d, MONO_PX), FW_BOLD, 0, "Consolas");
    d->mono_sel = font_like(&lf, S(d, MONO_PX), FW_BOLD, 1, "Consolas");

    /* the monospace cell, for the hand-drawn views */
    dc = GetDC(d->hwnd);
    prev = SelectObject(dc, d->mono);
    d->mono_w = S(d, 7);
    d->mono_h = S(d, 15);
    if (GetTextMetricsA(dc, &tm)) {
        d->mono_w = tm.tmAveCharWidth;
        d->mono_h = tm.tmHeight;
    }
    d->line_h = d->mono_h + S(d, 2);
    SelectObject(dc, prev);
    ReleaseDC(d->hwnd, dc);

    if (old[0]) {
        HFONT map[6] = { old[0], old[1], old[2], d->ui, d->ui_bold, d->mono };
        EnumChildWindows(d->hwnd, refont_child, (LPARAM)map);
    }
    for (i = 0; i < 5; i++)
        if (old[i]) DeleteObject(old[i]);
}

static int text_w(debugger *d, HFONT f, const char *text)
{
    HDC dc = GetDC(d->hwnd);
    HGDIOBJ old = SelectObject(dc, f);
    SIZE sz = { 0, 0 };
    GetTextExtentPoint32A(dc, text, (int)strlen(text), &sz);
    SelectObject(dc, old);
    ReleaseDC(d->hwnd, dc);
    return sz.cx;
}

static int btn_w(debugger *d, const char *text)
{
    const int w = text_w(d, d->ui, text) + S(d, 24);
    return w < S(d, 64) ? S(d, 64) : w;
}

/* The GTIA colour panel: the palette's top, and the panel's size. */
static int pal_top(const debugger *d) { return 2 * S(d, COL_TITLE_H) + 3 * S(d, COL_ROW_H) + S(d, 8); }
static int colours_w(const debugger *d) { return S(d, PAL_AXIS_W) + 16 * S(d, PAL_CELL_W); }
static int colours_h(const debugger *d) { return pal_top(d) + S(d, PAL_AXIS_H) + 16 * S(d, PAL_CELL_H) + S(d, 2); }

static COLORREF accent_rgb(void)
{
    return RGB((A800SESSION_ACCENT_RGB >> 16) & 0xff, (A800SESSION_ACCENT_RGB >> 8) & 0xff,
               A800SESSION_ACCENT_RGB & 0xff);
}

static int in_list(HWND h, const HWND *list, int n)
{
    int i;
    for (i = 0; i < n; i++)
        if (list[i] == h) return 1;
    return 0;
}

/* ---- the text views ------------------------------------------------------------ */

/* The rows an EDIT view shows. */
static int edit_rows(debugger *d, HWND h)
{
    RECT c;
    GetClientRect(h, &c);
    return d->mono_h > 0 && c.bottom > d->mono_h ? c.bottom / d->mono_h : 1;
}

/* Logs (the console, the last instructions, the SIO trace) follow their
 * end while their reader is there. */
static int edit_at_end(debugger *d, HWND h)
{
    const int first = (int)SendMessageA(h, EM_GETFIRSTVISIBLELINE, 0, 0);
    const int count = (int)SendMessageA(h, EM_GETLINECOUNT, 0, 0);
    return first + edit_rows(d, h) >= count;
}

static void edit_to_end(HWND h)
{
    SendMessageA(h, EM_LINESCROLL, 0, SendMessageA(h, EM_GETLINECOUNT, 0, 0));
}

static void edit_scroll_to(HWND h, int first)
{
    SendMessageA(h, EM_LINESCROLL, 0, first - (int)SendMessageA(h, EM_GETFIRSTVISIBLELINE, 0, 0));
}

/* Replaces a view's text where the reader left it (a refresh must not throw
 * a scrolled view back to its top), or, following, at its end. */
static void set_text_at(HWND h, const char *text, int follow)
{
    const int first = (int)SendMessageA(h, EM_GETFIRSTVISIBLELINE, 0, 0);
    char *buf = to_crlf(text);
    set_text(h, buf ? buf : text);
    free(buf);
    if (follow) edit_to_end(h);
    else edit_scroll_to(h, first);
}

/* The live refresh does not overwrite a value being typed: while the
 * machine runs, a focused field keeps what is in it. */
static void set_entry(debugger *d, HWND h, const char *text)
{
    if (GetFocus() == h && !a800debug_is_stopped(d->dbg)) return;
    set_label(h, text);
}

static void refresh_status(debugger *d);

static int notice_showing(debugger *d)
{
    return d->notice[0] && (LONG)(GetTickCount() - d->notice_until) < 0;
}

/* The other frontends' toasts: the message holds the status line a moment. */
static void toast(debugger *d, const char *msg)
{
    snprintf(d->notice, sizeof d->notice, "%s", msg);
    d->notice_until = GetTickCount() + NOTICE_MS;
    refresh_status(d);
}

static const char *on_off(int v) { return v ? "on" : "off"; }

/* ---- the console ---------------------------------------------------------------- */

/* Long sessions keep the last CONSOLE_KEEP characters, whole lines. */
static void console_trim(debugger *d)
{
    const int len = GetWindowTextLengthA(d->con_out);
    char *all, *nl;
    int cut;
    if (len <= CONSOLE_KEEP) return;
    all = malloc((size_t)len + 1);
    if (!all) return;
    GetWindowTextA(d->con_out, all, len + 1);
    cut = len - CONSOLE_KEEP * 3 / 4;
    nl = strchr(all + cut, '\n');
    if (nl) cut = (int)(nl - all) + 1;
    free(all);
    SendMessageA(d->con_out, EM_SETSEL, 0, (LPARAM)cut);
    SendMessageA(d->con_out, EM_REPLACESEL, FALSE, (LPARAM)"");
}

/* Onto the console's transcript, which follows its end while the reader
 * is there. */
static void console_append(debugger *d, const char *text)
{
    const int follow = edit_at_end(d, d->con_out);
    const int first = (int)SendMessageA(d->con_out, EM_GETFIRSTVISIBLELINE, 0, 0);
    char *buf = to_crlf(text);
    int len;
    if (!buf) return;
    len = GetWindowTextLengthA(d->con_out);
    SendMessageA(d->con_out, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    SendMessageA(d->con_out, EM_REPLACESEL, FALSE, (LPARAM)buf);
    free(buf);
    console_trim(d);
    if (follow) edit_to_end(d->con_out);
    else edit_scroll_to(d->con_out, first);
}

/* Command output, ended with a newline. */
static void console_output(debugger *d, const char *text)
{
    const size_t n = strlen(text);
    if (!n) return;
    console_append(d, text);
    if (text[n - 1] != '\n') console_append(d, "\n");
}

/* Messages from Load Symbols, Save and the snapshots land in the console
 * and, since it may not be the tab showing, on the status line. */
static void say(debugger *d, const char *msg)
{
    console_output(d, msg);
    toast(d, msg);
}

static void layout(debugger *d);

static void set_prompt(debugger *d, int asm_mode, const char *prompt)
{
    d->con_asm = asm_mode;
    snprintf(d->con_prompt_text, sizeof d->con_prompt_text, "%s", asm_mode && prompt ? prompt : "> ");
    set_text(d->con_prompt, d->con_prompt_text);
    SendMessageA(d->con_in, EM_SETCUEBANNER, TRUE, asm_mode
        ? (LPARAM)L"Assembler: an instruction (LDA #$42), or an empty line to finish"
        : (LPARAM)L"Monitor command (HELP, D, M, F, A, BLINE, DLIST, LABELS, CONT ...) "
                  L"\x2014 Tab completes, Up/Down recall");
    layout(d);      /* the prompt's width */
}

/* The banner and registers atari800 printed at a stop: an empty line,
 * which its monitor ignores, hands them over. Not while the assembler is
 * waiting, where an empty line would end it. */
static void console_drain(debugger *d)
{
    static char out[65536];
    if (d->con_asm || !a800session_is_running(d->session)) return;
    a800debug_command(d->dbg, "", out, sizeof out);
    console_output(d, out);
}

/* ---- refresh ---------------------------------------------------------------------- */

static void refresh_status(debugger *d)
{
    char reason[160], text[220];
    int addr = -1;
    const int running = a800session_is_running(d->session) != 0;
    const int stopped = running && a800debug_is_stopped(d->dbg);

    if (notice_showing(d))
        snprintf(text, sizeof text, "%s", d->notice);
    else if (!running)
        snprintf(text, sizeof text, "The Atari is not running");
    else if (stopped) {
        a800debug_stop_reason(d->dbg, reason, sizeof reason, &addr);
        snprintf(text, sizeof text, "Stopped%s%s", reason[0] ? ": " : "", reason);
    } else
        snprintf(text, sizeof text, "Running");
    set_label(d->status, text);
    if (d->tip) {
        TOOLINFOA ti;
        memset(&ti, 0, sizeof ti);
        ti.cbSize = TTTOOLINFOA_V2_SIZE;
        ti.hwnd = d->hwnd;
        ti.uId = (UINT_PTR)d->status;
        ti.lpszText = text;
        SendMessageA(d->tip, TTM_UPDATETIPTEXTA, 0, (LPARAM)&ti);
    }
    set_label(d->run_btn, stopped ? "Run (F5)" : "Stop (F5)");
    InvalidateRect(d->run_btn, NULL, FALSE);
    InvalidateRect(d->status, NULL, TRUE);
    SendMessageA(d->brk_btn, BM_SETCHECK,
                 running && a800debug_breaks_on_brk(d->dbg) ? BST_CHECKED : BST_UNCHECKED, 0);
}

static void refresh_cpu(debugger *d)
{
    a800debug_cpu c;
    char buf[160];
    int i;
    a800debug_cpu_get(d->dbg, &c);
    {
        const int vals[6] = { c.pc, c.sp, c.a, c.x, c.y, c.p };
        for (i = 0; i < 6; i++) {
            snprintf(buf, sizeof buf, i == 0 ? "%04X" : "%02X",
                     (unsigned)vals[i] & (i == 0 ? 0xFFFFu : 0xFFu));
            set_entry(d, d->reg[i], buf);
        }
    }
    {
        const int flags[6] = { c.n, c.v, c.d, c.i, c.z, c.c };
        for (i = 0; i < 6; i++)
            SendMessageA(d->flag[i], BM_SETCHECK, flags[i] ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    snprintf(buf, sizeof buf, "scanline %d   cycle %d   frame %u", c.scanline, c.cycle, (unsigned)c.frame);
    set_label(d->beam, buf);
}

static void refresh_mem(debugger *d)
{
    static uint8_t mem[MEM_ROWS * 16];
    static char t[(MEM_ROWS + 1) * 84];
    const int first = (int)SendMessageA(d->mem_view, EM_GETFIRSTVISIBLELINE, 0, 0);
    size_t len = 0;
    int row, col;

    a800debug_read(d->dbg, (uint16_t)d->mem_top, mem, (int)sizeof mem);
    len = addf(t, len, sizeof t, "       ");
    for (col = 0; col < 16; col++)
        len = addf(t, len, sizeof t, col == 8 ? " %X  " : "%X  ", col);
    len = addf(t, len, sizeof t, "\r\n");
    for (row = 0; row < MEM_ROWS; row++) {
        const uint8_t *r = mem + row * 16;
        len = addf(t, len, sizeof t, "$%04X: ", (d->mem_top + row * 16) & 0xFFFF);
        for (col = 0; col < 16; col++)
            len = addf(t, len, sizeof t, col == 8 ? " %02X " : "%02X ", r[col]);
        len = addf(t, len, sizeof t, " ");
        for (col = 0; col < 16; col++)
            len = addf(t, len, sizeof t, "%c", r[col] >= 0x20 && r[col] < 0x7F ? r[col] : '.');
        len = addf(t, len, sizeof t, "\r\n");
    }
    set_text(d->mem_view, t);

    /* the row Go to found stays marked (selected) across refreshes */
    if (d->mem_mark >= 0) {
        const int off = (d->mem_mark - d->mem_top) & 0xFFFF;
        if (off < MEM_ROWS * 16) {
            const int line = 1 + off / 16;
            const int s = (int)SendMessageA(d->mem_view, EM_LINEINDEX, (WPARAM)line, 0);
            const int e = s + (int)SendMessageA(d->mem_view, EM_LINELENGTH, (WPARAM)s, 0);
            SendMessageA(d->mem_view, EM_SETSEL, (WPARAM)s, (LPARAM)e);
        }
    }
    edit_scroll_to(d->mem_view, first);
}

static void refresh_history(debugger *d)
{
    static a800debug_history h[HISTORY_MAX];
    static char t[HISTORY_MAX * 96 + 64];
    size_t len = 0;
    int n = a800debug_history_get(d->dbg, h, HISTORY_MAX), i;
    len = addf(t, len, sizeof t, "PC     line cyc  instruction");
    for (i = 0; i < n; i++)
        len = addf(t, len, sizeof t, "\n%04X   %3d  %3d  %s", h[i].pc, h[i].scanline, h[i].cycle, h[i].disasm);
    set_text_at(d->hist_view, t, 1);
}

/* The disassembly lines that fit the view. */
static int visible_rows(debugger *d)
{
    RECT c;
    int rows;
    GetClientRect(d->disasm, &c);
    if (d->line_h <= 0 || c.bottom <= 0) return 32;
    rows = c.bottom / d->line_h;
    return rows < 4 ? 4 : rows > DISASM_WINDOW ? DISASM_WINDOW : rows;
}

static void refresh_disasm(debugger *d)
{
    int pc_line = -1, i;
    const int rows = visible_rows(d);

    /* Following, the PC stays put while it shows, else sits a third of the
     * way down. */
    if (SendMessageA(d->follow_pc, BM_GETCHECK, 0, 0) == BST_CHECKED) {
        a800debug_cpu c;
        int shows = 0;
        a800debug_cpu_get(d->dbg, &c);
        for (i = 1; i < d->line_count && i < rows - 2; i++)
            if (d->lines[i].address == c.pc) shows = 1;
        if (!shows)
            d->disasm_top = a800debug_row_address(d->dbg, (uint16_t)c.pc, -(rows / 3));
    }
    d->line_count = a800debug_disassemble(d->dbg, (uint16_t)d->disasm_top, d->lines, DISASM_WINDOW,
                                          &pc_line);
    if (d->line_count < 0) d->line_count = 0;
    InvalidateRect(d->disasm, NULL, FALSE);
}

static void refresh_antic(debugger *d)
{
    static const char *const widths[4] = {
        "none", "narrow (32 bytes a line)", "normal (40 bytes a line)", "wide (48 bytes a line)" };
    static char t[2048], dl[32768 + 128];
    a800debug_antic a;
    size_t len = 0;
    int pm;

    a800debug_antic_get(d->dbg, &a);
    pm = a.pm_single_line ? (a.pmbase & 0xF8) << 8 : (a.pmbase & 0xFC) << 8;
    len = addf(t, len, sizeof t,
        "DMACTL  $%02X   playfield %s\n"
        "              display list DMA %s\n"
        "              players %s, missiles %s, %s-line resolution\n"
        "CHACTL  $%02X   inverse characters shown %s%s%s\n"
        "HSCROL  $%02X\n"
        "VSCROL  $%02X\n"
        "PMBASE  $%02X   players and missiles at $%04X\n"
        "CHBASE  $%02X   character set at $%04X\n"
        "NMIEN   $%02X   display list interrupts %s, vertical blank %s\n"
        "NMIST   $%02X  %s%s%s%s\n"
        "DLIST   $%04X (the list this frame began with)\n\n"
        "Beam    scanline %d, cycle %d, VCOUNT $%02X\n",
        a.dmactl, widths[a.playfield_width & 3], on_off(a.dl_dma),
        on_off(a.pm_dma), on_off(a.missile_dma), a.pm_single_line ? "single" : "double",
        a.chactl, (a.chactl & 2) ? "inverse" : "normal",
        (a.chactl & 1) ? ", blanked" : "", (a.chactl & 4) ? ", upside down" : "",
        a.hscrol, a.vscrol, a.pmbase, pm, a.chbase, a.chbase << 8,
        a.nmien, on_off(a.nmien & 0x80), on_off(a.nmien & 0x40),
        a.nmist, (a.nmist & 0x80) ? " DLI" : "", (a.nmist & 0x40) ? " VBI" : "",
        (a.nmist & 0x20) ? " RESET" : "", (a.nmist & 0xE0) ? " (last interrupts)" : " -",
        a.dlist, a.scanline, a.cycle, a.vcount & 0xFF);
    set_text_at(d->antic_text, t, 0);

    len = 0;
    if (!a.dl_dma)
        len = addf(dl, len, sizeof dl, "(display list DMA is off: ANTIC is not reading this)\n\n");
    if (a800debug_dlist_text(d->dbg, dl + len, (int)(sizeof dl - len)) <= 0)
        snprintf(dl, sizeof dl, "(no display list)");
    set_text_at(d->dlist_text, dl, 0);
}

/* A GRAF byte's bits, left to right. */
static size_t graf_bits(char *t, size_t len, size_t cap, unsigned v, int bits)
{
    int b;
    for (b = bits - 1; b >= 0; b--)
        len = addf(t, len, cap, "%c", (v >> b) & 1 ? '#' : '.');
    return len;
}

static size_t collision_row(char *t, size_t len, size_t cap, const char *name, uint8_t pf, uint8_t pl,
                            int self)
{
    int j;
    len = addf(t, len, cap, "  %s        ", name);
    for (j = 0; j < 4; j++)
        len = addf(t, len, cap, " %c  ", (pf >> j) & 1 ? 'X' : '.');
    len = addf(t, len, cap, "   ");
    for (j = 0; j < 4; j++)
        len = addf(t, len, cap, "%c  ", j == self ? '-' : (pl >> j) & 1 ? 'X' : '.');
    return addf(t, len, cap, "\n");
}

static void refresh_gtia(debugger *d)
{
    static const char *const sizes[4] = { "normal", "double", "normal", "quad" };
    static const char *const modes[4] = {
        "normal (ANTIC's colours)", "9 (16 lumas of one hue)", "10 (9 colours)", "11 (16 hues)" };
    static char t[4096];
    a800debug_gtia g;
    const char *prio;
    size_t len = 0;
    int i;

    a800debug_gtia_get(d->dbg, &g);
    len = addf(t, len, sizeof t, "Player  HPOS  SIZE    GRAF       COLPM\n");
    for (i = 0; i < 4; i++) {
        len = addf(t, len, sizeof t, "P%d      $%02X   %-7s ", i, g.hposp[i], sizes[g.sizep[i] & 3]);
        len = graf_bits(t, len, sizeof t, g.grafp[i], 8);
        len = addf(t, len, sizeof t, "   $%02X\n", g.colpm[i]);
    }
    len = addf(t, len, sizeof t, "\nMissile HPOS  SIZE    GRAF\n");
    for (i = 0; i < 4; i++) {
        len = addf(t, len, sizeof t, "M%d      $%02X   %-7s ", i, g.hposm[i], sizes[(g.sizem >> (i * 2)) & 3]);
        len = graf_bits(t, len, sizeof t, (g.grafm >> (i * 2)) & 3, 2);
        len = addf(t, len, sizeof t, "\n");
    }
    switch (g.prior & 0x0F) {
    case 1: prio = "players > playfield"; break;
    case 2: prio = "P0-1 > playfield > P2-3"; break;
    case 4: prio = "playfield > players"; break;
    case 8: prio = "PF0-1 > players > PF2-3"; break;
    case 0: prio = "none set (overlaps mix)"; break;
    default: prio = "several set (overlaps mix)"; break;
    }
    len = addf(t, len, sizeof t,
        "\nCOLPF0-3 $%02X $%02X $%02X $%02X   COLBK $%02X\n"
        "PRIOR   $%02X   priority %s\n"
        "              fifth player %s, multicolour players %s\n"
        "              GTIA mode %s\n"
        "VDELAY  $%02X   GRACTL $%02X (missiles %s, players %s, trigger latch %s)\n"
        "CONSOL  $%02X  %s%s%s%s\n"
        "TRIG0-3 %d %d %d %d (0 = pressed; on XL/XE TRIG3 is the cartridge sense line)\n\n",
        g.colpf[0], g.colpf[1], g.colpf[2], g.colpf[3], g.colbk,
        g.prior, prio, on_off(g.prior & 0x10), on_off(g.prior & 0x20), modes[g.prior >> 6],
        g.vdelay, g.gractl, on_off(g.gractl & 1), on_off(g.gractl & 2), on_off(g.gractl & 4),
        g.consol, (g.consol & 1) ? "" : " START", (g.consol & 2) ? "" : " SELECT",
        (g.consol & 4) ? "" : " OPTION", (g.consol & 7) == 7 ? " no console key held" : " held",
        g.trig[0] & 1, g.trig[1] & 1, g.trig[2] & 1, g.trig[3] & 1);
    len = addf(t, len, sizeof t, "Collisions  PF0 PF1 PF2 PF3    P0 P1 P2 P3\n");
    for (i = 0; i < 4; i++) {
        char n[4] = { 'M', (char)('0' + i), 0, 0 };
        len = collision_row(t, len, sizeof t, n, g.mpf[i], g.mpl[i], -1);
    }
    for (i = 0; i < 4; i++) {
        char n[4] = { 'P', (char)('0' + i), 0, 0 };
        len = collision_row(t, len, sizeof t, n, g.ppf[i], g.ppl[i], i);
    }
    set_text_at(d->gtia_text, t, 0);

    /* the colour registers through the palette */
    {
        const uint8_t vals[SWATCHES] = { g.colpm[0], g.colpm[1], g.colpm[2], g.colpm[3],
                                         g.colpf[0], g.colpf[1], g.colpf[2], g.colpf[3], g.colbk };
        for (i = 0; i < SWATCHES; i++) {
            d->swatch_val[i] = vals[i];
            d->swatch_rgb[i] = a800debug_color(d->dbg, vals[i]);
        }
    }
    d->palette_ok = a800debug_palette_image(d->dbg, d->palette_px) != 0;
    InvalidateRect(d->colours, NULL, FALSE);
}

/* The IRQ bits by name, the set ones (or, active_low, the clear ones). */
static size_t irq_words(char *t, size_t len, size_t cap, uint8_t v, int active_low)
{
    static const char *const names[8] = {
        "timer 1", "timer 2", "timer 4", "serial output done", "serial output needed",
        "serial input ready", "key", "BREAK key" };
    int any = 0, b;
    for (b = 7; b >= 0; b--)
        if (((v >> b) & 1) != active_low) {
            len = addf(t, len, cap, "%s%s", any ? ", " : " ", names[b]);
            any = 1;
        }
    if (!any) len = addf(t, len, cap, " none");
    return len;
}

static size_t pokey_text(char *t, size_t len, size_t cap, const a800debug_pokey *p, const char *title)
{
    static const char *const dist[8] = {
        "5-bit + 17-bit poly", "5-bit poly", "5-bit + 4-bit poly", "5-bit poly",
        "17-bit poly", "pure tone", "4-bit poly", "pure tone" };
    static const char *const audctl[8] = {
        "15 kHz clock", "high-pass 2 by 4", "high-pass 1 by 3", "4 joined to 3 (16-bit)",
        "2 joined to 1 (16-bit)", "3 at 1.79 MHz", "1 at 1.79 MHz", "9-bit poly" };
    int ch, b, any = 0;

    len = addf(t, len, cap, "%s\n  Channel  AUDF  AUDC  sound                  volume\n", title);
    for (ch = 0; ch < 4; ch++) {
        const int c = p->audc[ch], dd = c >> 5;
        char sound[32];
        snprintf(sound, sizeof sound, "%s", dist[dd]);
        if ((p->audctl & 0x80) && (dd == 0 || dd == 4)) {
            /* AUDCTL bit 7 swaps the 17-bit poly for the 9-bit one */
            char *at = strstr(sound, "17-bit");
            if (at) {
                at[0] = '9';
                memmove(at + 1, at + 2, strlen(at + 2) + 1);
            }
        }
        len = addf(t, len, cap, "  %d        $%02X   $%02X   %-22s %2d%s\n", ch + 1, p->audf[ch], c, sound,
                   c & 15, (c & 0x10) ? " (volume only)" : "");
    }
    len = addf(t, len, cap, "  AUDCTL   $%02X  ", p->audctl);
    for (b = 0; b < 8; b++)
        if (p->audctl & (1 << b)) {
            len = addf(t, len, cap, "%s%s", any ? ", " : " ", audctl[b]);
            any = 1;
        }
    return addf(t, len, cap, any ? "\n" : " 64 kHz clock, nothing joined\n");
}

/* A joystick's lines (a PIA nibble, active low). */
static size_t stick_words(char *t, size_t len, size_t cap, int nibble)
{
    static const char *const dirs[4] = { "up", "down", "left", "right" };
    int any = 0, b;
    for (b = 0; b < 4; b++)
        if (!((nibble >> b) & 1)) {
            len = addf(t, len, cap, "%s%s", any ? " " : "", dirs[b]);
            any = 1;
        }
    if (!any) len = addf(t, len, cap, "centred");
    return len;
}

static void refresh_pokey(debugger *d)
{
    static char t[8192];
    const size_t cap = sizeof t;
    a800debug_pokey p;
    a800debug_pia pia;
    a800debug_map map;
    const int stereo = a800session_get_int(d->session, "stereo", 0) != 0;
    size_t len = 0;
    int i;

    a800debug_pokey_get(d->dbg, 0, &p);
    len = pokey_text(t, len, cap, &p, stereo ? "POKEY 1 ($D200, left)" : "POKEY ($D200)");
    if (stereo) {
        a800debug_pokey p2;
        a800debug_pokey_get(d->dbg, 1, &p2);
        len = addf(t, len, cap, "\n");
        len = pokey_text(t, len, cap, &p2, "POKEY 2 ($D210, right)");
    }
    len = addf(t, len, cap,
        "\nKBCODE  $%02X\n"
        "SKCTL   $%02X   keyboard scan %s, debounce %s, fast pot scan %s, two-tone %s, "
        "serial mode %d%s\n"
        "SKSTAT  $%02X  %s%s%s\n"
        "IRQEN   $%02X   enabled:",
        p.kbcode, p.skctl, on_off(p.skctl & 2), on_off(p.skctl & 1), on_off(p.skctl & 4),
        on_off(p.skctl & 8), (p.skctl >> 4) & 7, (p.skctl & 0x80) ? ", forcing a break" : "",
        p.skstat, (p.skstat & 8) ? "" : " SHIFT held", (p.skstat & 4) ? "" : " a key held",
        (p.skstat & 0x0C) == 0x0C ? " no key held" : "", p.irqen);
    len = irq_words(t, len, cap, p.irqen, 0);
    len = addf(t, len, cap, "\nIRQST   $%02X   pending:", p.irqst);
    len = irq_words(t, len, cap, p.irqst, 1);
    len = addf(t, len, cap, "\nRANDOM  $%02X\nPOT0-7 ", p.random);
    for (i = 0; i < 8; i++)
        len = addf(t, len, cap, " $%02X", p.pot[i]);

    /* the PIA: what the ports read is the joysticks' lines (and on XL/XE,
     * port B is the memory control) */
    a800debug_pia_get(d->dbg, &pia);
    a800debug_map_get(d->dbg, &map);
    len = addf(t, len, cap, "\n\nPIA\nPORTA   out $%02X   DDR $%02X (1 = output)",
               pia.porta, (uint8_t)~pia.porta_mask);
    if (pia.pactl & 4) {
        uint8_t in = 0;
        a800debug_read(d->dbg, 0xD300, &in, 1);
        len = addf(t, len, cap, "   reads $%02X: stick 1 ", in);
        len = stick_words(t, len, cap, in & 15);
        len = addf(t, len, cap, ", stick 2 ");
        len = stick_words(t, len, cap, in >> 4);
    }
    len = addf(t, len, cap,
        "\nPACTL   $%02X   port %s, cassette motor (CA2) %s, CA1 interrupt %s\n"
        "PORTB   out $%02X   DDR $%02X",
        pia.pactl, (pia.pactl & 4) ? "data" : "direction", pia.ca2 ? "off" : "on",
        on_off(pia.pactl & 1), pia.portb, (uint8_t)~pia.portb_mask);
    if (map.xl)
        len = addf(t, len, cap, "   (memory control: see the Memory Map)");
    else if (pia.pbctl & 4) {
        uint8_t in = 0;
        a800debug_read(d->dbg, 0xD301, &in, 1);
        len = addf(t, len, cap, "   reads $%02X: stick 3 ", in);
        len = stick_words(t, len, cap, in & 15);
        len = addf(t, len, cap, ", stick 4 ");
        len = stick_words(t, len, cap, in >> 4);
    }
    len = addf(t, len, cap,
        "\nPBCTL   $%02X   port %s, SIO command (CB2) %s, CB1 interrupt %s\n"
        "Lines   CA1 %d  CA2 %d  CB1 %d  CB2 %d   IRQ %s\n",
        pia.pbctl, (pia.pbctl & 4) ? "data" : "direction", pia.cb2 ? "idle" : "asserted",
        on_off(pia.pbctl & 1), pia.ca1, pia.ca2, pia.cb1, pia.cb2, pia.irq ? "asserted" : "-");
    set_text_at(d->pokey_text, t, 0);
}

static void refresh_map(debugger *d)
{
    static char t[4096];
    const size_t cap = sizeof t;
    a800debug_map m;
    size_t len = 0;
    int kb, b;

    a800debug_map_get(d->dbg, &m);
    kb = m.ram_kb == 321 ? 320 : m.ram_kb;     /* atari800's Compy Shop 320K */
    len = addf(t, len, cap, "Machine   %s, %dK RAM%s\n\n", m.machine, kb, m.xl ? ", XL/XE banking" : "");
    if (m.xl) {
        len = addf(t, len, cap, "PORTB     $%02X = ", m.portb);
        for (b = 7; b >= 0; b--)
            len = addf(t, len, cap, "%d%s", (m.portb >> b) & 1, b == 4 ? " " : "");
        len = addf(t, len, cap,
            "\n  bit 0     OS ROM               %s\n"
            "  bit 1     BASIC ROM            %s (0 = in)\n"
            "  bits 2-3  extended bank        %d\n"
            "  bit 4     CPU bank access      %s (0 = on)\n"
            "  bit 5     ANTIC bank access    %s (0 = on)\n"
            "  bit 7     self-test ROM        %s (0 = in)\n\n",
            m.os_rom ? "in" : "out (RAM)", m.basic_rom ? "in" : "out",
            (m.portb >> 2) & 3, m.xe_cpu_bank >= 0 ? "on" : "off",
            m.xe_antic_bank >= 0 ? "on" : "off", m.selftest ? "in" : "out");
    }

    len = addf(t, len, cap, "What the CPU sees\n");
    if (m.xl) {
        len = addf(t, len, cap, "  $0000-$3FFF  RAM\n");
        if (m.xe_cpu_bank >= 0)
            len = addf(t, len, cap, "  $4000-$7FFF  extended RAM bank %d\n", m.xe_cpu_bank);
        else
            len = addf(t, len, cap, "  $4000-$7FFF  RAM\n");
        if (m.xe_antic_bank >= 0)
            len = addf(t, len, cap, "               (ANTIC reads extended bank %d here)\n", m.xe_antic_bank);
        if (m.selftest)
            len = addf(t, len, cap, "  $5000-$57FF  self-test ROM\n");
        len = addf(t, len, cap, "  $8000-$9FFF  %s\n", m.cart_type ? "RAM, or the cartridge" : "RAM");
        len = addf(t, len, cap, "  $A000-$BFFF  %s\n",
                   m.basic_rom ? "BASIC ROM" : m.cart_type ? "RAM, or the cartridge" : "RAM");
        len = addf(t, len, cap, "  $C000-$CFFF  %s\n", m.os_rom ? "OS ROM" : "RAM");
        len = addf(t, len, cap, "  $D000-$D7FF  hardware: GTIA $D000, POKEY $D200, PIA $D300, ANTIC $D400\n");
        len = addf(t, len, cap, "  $D800-$FFFF  %s\n", m.os_rom ? "OS ROM (floating point at $D800)" : "RAM");
    } else {
        const int top = (kb > 48 ? 48 : kb) * 1024 - 1;
        if (top > 0)
            len = addf(t, len, cap, "  $0000-$%04X  RAM\n", top);
        if (m.cart_type)
            len = addf(t, len, cap, "  $8000-$BFFF  the cartridge, where its type maps it\n");
        if (m.basic_rom)
            len = addf(t, len, cap, "  $A000-$BFFF  BASIC ROM\n");
        len = addf(t, len, cap, "  $C000-$CFFF  %s\n", kb > 48 ? "RAM" : "(nothing)");
        len = addf(t, len, cap, "  $D000-$D7FF  hardware: GTIA $D000, POKEY $D200, PIA $D300, ANTIC $D400\n");
        len = addf(t, len, cap, "  $D800-$FFFF  OS ROM (floating point at $D800)\n");
    }

    if (m.cart_type)
        len = addf(t, len, cap, "\nCartridge   type %d: %s, bank %d\n            %s\n", m.cart_type,
                   m.cart_desc, m.cart_bank, m.cart_name);
    else
        len = addf(t, len, cap, "\nCartridge   none\n");
    if (m.piggyback_type)
        len = addf(t, len, cap, "Piggyback   type %d\n            %s\n", m.piggyback_type, m.piggyback_name);
    set_text_at(d->map_text, t, 0);
}

static const char *bp_type_name(int type)
{
    if (type & A800DEBUG_BP_EXEC) return "Execute";
    if ((type & (A800DEBUG_BP_READ | A800DEBUG_BP_WRITE)) == (A800DEBUG_BP_READ | A800DEBUG_BP_WRITE))
        return "Read/Write";
    return (type & A800DEBUG_BP_READ) ? "Read" : "Write";
}

/* The list, or (empty) the hint in its place. */
static void show_bp_list(debugger *d)
{
    const int on = d->page == TAB_BREAKS;
    ShowWindow(d->bp_list, on && d->nbps > 0 ? SW_SHOW : SW_HIDE);
    ShowWindow(d->bp_empty, on && d->nbps == 0 ? SW_SHOW : SW_HIDE);
}

static void lv_set(HWND lv, int row, int col, const char *text)
{
    LVITEMA it;
    memset(&it, 0, sizeof it);
    it.iSubItem = col;
    it.pszText = (char *)text;
    SendMessageA(lv, LVM_SETITEMTEXTA, (WPARAM)row, (LPARAM)&it);
}

static void refresh_bps(debugger *d)
{
    char sig[sizeof d->bp_sig], text[160];
    size_t len = 0;
    int i, max = 0;
    const int used = a800debug_breakpoint_budget(d->dbg, &max);

    snprintf(text, sizeof text, "atari800's condition table: %d of %d entries used", used, max);
    set_label(d->bp_budget, text);

    /* rebuilt only when it changed (each step refreshes) */
    d->nbps = a800debug_breakpoint_list(d->dbg, d->bps, BP_MAX);
    if (d->nbps < 0) d->nbps = 0;
    sig[0] = '\0';
    for (i = 0; i < d->nbps; i++)
        len = addf(sig, len, sizeof sig, "%d %d %d %d %d %s\n", d->bps[i].id, d->bps[i].type, d->bps[i].start,
                   d->bps[i].end, d->bps[i].enabled, d->bps[i].condition);
    if (d->bp_sig_ok && strcmp(sig, d->bp_sig) == 0) return;
    memcpy(d->bp_sig, sig, sizeof sig);
    d->bp_sig_ok = 1;

    d->updating = 1;
    SendMessageA(d->bp_list, LVM_DELETEALLITEMS, 0, 0);
    for (i = 0; i < d->nbps; i++) {
        const a800debug_breakpoint *b = &d->bps[i];
        LVITEMA it;
        char id[16], range[32];
        snprintf(id, sizeof id, "#%d", b->id);
        if (b->end != b->start) snprintf(range, sizeof range, "$%04X-$%04X", b->start, b->end);
        else snprintf(range, sizeof range, "$%04X", b->start);
        memset(&it, 0, sizeof it);
        it.mask = LVIF_TEXT;
        it.iItem = i;
        it.pszText = id;
        SendMessageA(d->bp_list, LVM_INSERTITEMA, 0, (LPARAM)&it);
        lv_set(d->bp_list, i, 1, bp_type_name(b->type));
        lv_set(d->bp_list, i, 2, range);
        lv_set(d->bp_list, i, 3, b->condition);
        memset(&it, 0, sizeof it);
        it.stateMask = LVIS_STATEIMAGEMASK;
        it.state = INDEXTOSTATEIMAGEMASK(b->enabled ? 2 : 1);
        SendMessageA(d->bp_list, LVM_SETITEMSTATE, (WPARAM)i, (LPARAM)&it);
    }
    d->updating = 0;
    show_bp_list(d);
}

static void refresh_sio(debugger *d, int force)
{
    static a800debug_sio e[SIO_MAX];
    static char t[SIO_MAX * 100 + 128];
    const int fuji = a800session_fujinet_running(d->session) != 0;
    const int n = a800debug_sio_trace(d->dbg, e, SIO_MAX);
    size_t len = 0;
    int i, follow;

    set_label(d->sio_note, fuji
        ? "FujiNet is running: the Atari's commands go to it over NetSIO."
        : "FujiNet is off: atari800's own drives answer (\"patch\" is its fast SIO patch).");
    /* a log: set again only when it moved */
    if (!force && n == d->sio_count &&
        (n == 0 || memcmp(&e[n - 1], &d->sio_last, sizeof d->sio_last) == 0))
        return;
    follow = force || edit_at_end(d, d->sio_text);
    d->sio_count = n;
    if (n > 0) d->sio_last = e[n - 1];
    len = addf(t, len, sizeof t, "  Frame  Path    Dev  Cmd  Aux1 Aux2   Command");
    for (i = 0; i < n; i++)
        len = addf(t, len, sizeof t, "\n%7u  %-6s  $%02X  $%02X  $%02X  $%02X   %s", (unsigned)e[i].frame,
                   e[i].path, e[i].device, e[i].command, e[i].aux1, e[i].aux2, e[i].text);
    if (n == 0)
        len = addf(t, len, sizeof t, "\n(no SIO commands since the trace was cleared)");
    set_text_at(d->sio_text, t, follow);
}

static void refresh_page(debugger *d, int page)
{
    if (!a800session_is_running(d->session)) return;
    switch (page) {
    case TAB_CPU: refresh_cpu(d); refresh_mem(d); refresh_history(d); break;
    case TAB_DISASM: refresh_disasm(d); break;
    case TAB_ANTIC: refresh_antic(d); break;
    case TAB_GTIA: refresh_gtia(d); break;
    case TAB_POKEY: refresh_pokey(d); break;
    case TAB_MAP: refresh_map(d); break;
    case TAB_BREAKS: refresh_bps(d); break;
    case TAB_SIO: refresh_sio(d, 0); break;
    default: break;
    }
}

static void refresh_all(debugger *d)
{
    refresh_status(d);
    refresh_page(d, d->page);
}

/* After anything that may have moved the engine: the console takes what
 * atari800 printed at a stop, then the views follow. The drain moves the
 * generation itself, so it is read after. */
static void resync(debugger *d)
{
    d->was_stopped = a800debug_is_stopped(d->dbg) != 0;
    if (d->was_stopped) console_drain(d);
    d->seen_generation = a800debug_generation(d->dbg);
    d->running_ticks = 0;
    refresh_all(d);
}

/* A step takes microseconds: a moment's wait for the stop keeps the window
 * from flashing "Running" in between. */
static void settle_and_sync(debugger *d)
{
    int i;
    d->notice[0] = '\0';        /* the machine moved: its new state replaces a notice */
    for (i = 0; i < 40 && !a800debug_is_stopped(d->dbg); i++)
        Sleep(1);
    resync(d);
}

static void attach_and_refresh(debugger *d)
{
    a800debug_attach(d->dbg);     /* stops the machine */
    a800debug_stop(d->dbg);       /* ... and so does showing it when it was up and running */
    d->attached_running = a800session_is_running(d->session) != 0;
    settle_and_sync(d);
}

static void tick(debugger *d)
{
    unsigned gen;
    int stopped;
    if (!IsWindowVisible(d->hwnd)) return;
    if (d->notice[0] && !notice_showing(d)) {
        d->notice[0] = '\0';
        refresh_status(d);
    }
    if (!a800session_is_running(d->session)) {
        if (d->attached_running) {
            d->attached_running = 0;
            refresh_status(d);
        }
        return;
    }
    /* Opened before the session started, or showing across a restart: the
     * engine attaches (stopping the machine) as it comes up. */
    if (!d->attached_running) {
        a800debug_detach(d->dbg);
        attach_and_refresh(d);
        return;
    }
    gen = a800debug_generation(d->dbg);
    stopped = a800debug_is_stopped(d->dbg) != 0;
    if (gen != d->seen_generation || stopped != d->was_stopped) {
        if (stopped != d->was_stopped) d->notice[0] = '\0';   /* a stop (or a go) says so at once */
        resync(d);
    } else if (!stopped && ++d->running_ticks >= 5) {
        /* live values twice a second while the machine runs */
        d->running_ticks = 0;
        refresh_status(d);
        if (d->page == TAB_CPU) refresh_cpu(d);
        if (d->page == TAB_SIO) refresh_sio(d, 0);
    }
}

/* ---- actions -------------------------------------------------------------------- */

static void on_run(debugger *d)
{
    if (a800debug_is_stopped(d->dbg)) a800debug_resume(d->dbg);
    else a800debug_stop(d->dbg);
    d->notice[0] = '\0';
    resync(d);
}

static void on_run_to(debugger *d)
{
    if (d->selected < 0) {
        toast(d, "Select a line in the Disassembly to run to");
        return;
    }
    a800debug_run_to(d->dbg, (uint16_t)d->selected);
    settle_and_sync(d);
}

static void toggle_breakpoint(debugger *d, uint16_t addr)
{
    if (a800debug_breakpoint_toggle(d->dbg, addr) < 0)
        toast(d, "atari800's condition table is full: remove a breakpoint first");
    resync(d);
}

static void on_snap_save(debugger *d, int slot)
{
    char msg[80];
    if (a800debug_snapshot_save(d->dbg, slot) == 0) snprintf(msg, sizeof msg, "Snapshot %d saved", slot);
    else snprintf(msg, sizeof msg, "Could not save snapshot %d", slot);
    toast(d, msg);
}

static void on_snap_restore(debugger *d, int slot)
{
    char msg[80];
    if (a800debug_snapshot_restore(d->dbg, slot) == 0) snprintf(msg, sizeof msg, "Snapshot %d restored", slot);
    else snprintf(msg, sizeof msg, "Could not restore snapshot %d", slot);
    toast(d, msg);
    resync(d);
}

static void on_console(debugger *d)
{
    static char out[65536];
    char buf[256], line[300], *cmd;
    int rc;

    edit_text(d->con_in, buf, sizeof buf);
    cmd = strip(buf);
    /* an empty line only means something to the assembler (its end) */
    if (!d->con_asm && !*cmd) return;
    if (*cmd && (d->nhistory == 0 || strcmp(d->history[d->nhistory - 1], cmd) != 0)) {
        if (d->nhistory == CMD_HISTORY) {
            memmove(d->history[0], d->history[1], sizeof d->history[0] * (CMD_HISTORY - 1));
            d->nhistory--;
        }
        snprintf(d->history[d->nhistory++], sizeof d->history[0], "%s", cmd);
    }
    d->history_pos = d->nhistory;
    snprintf(line, sizeof line, "%s%s\n", d->con_prompt_text, cmd);
    edit_to_end(d->con_out);     /* a command brings the reader back down */
    console_append(d, line);
    set_text(d->con_in, "");

    rc = a800debug_command(d->dbg, cmd, out, sizeof out);
    if (rc == A800DBG_CMD_NEEDS_INPUT) {
        /* the output ends with the assembler's own prompt ("0642: ") */
        char *nl = strrchr(out, '\n');
        char *prompt = nl ? nl + 1 : out;
        char saved[32];
        snprintf(saved, sizeof saved, "%.31s", prompt);
        *prompt = '\0';
        console_output(d, out);
        set_prompt(d, 1, saved);
    } else {
        console_output(d, out);
        set_prompt(d, 0, NULL);
    }
    if (rc == A800DBG_CMD_RESUMED) settle_and_sync(d);
    else resync(d);
}

/* Up and Down walk the history; past the newest is an empty line again. */
static void history_step(debugger *d, int newer)
{
    int len;
    if (newer) {
        if (d->history_pos >= d->nhistory) return;
        d->history_pos++;
    } else {
        if (d->history_pos <= 0) return;
        d->history_pos--;
    }
    set_text(d->con_in, d->history_pos < d->nhistory ? d->history[d->history_pos] : "");
    len = GetWindowTextLengthA(d->con_in);
    SendMessageA(d->con_in, EM_SETSEL, (WPARAM)len, (LPARAM)len);
}

/* Tab completes the command word against atari800's commands (the longest
 * common start of several, which are listed). */
static void complete(debugger *d)
{
    char text[256], comps[2048], *words[128], *p;
    int n = 0, i;

    edit_text(d->con_in, text, sizeof text);
    if (d->con_asm || strchr(text, ' ')) {
        MessageBeep(MB_OK);
        return;
    }
    a800debug_completions(d->dbg, text, comps, sizeof comps);
    for (p = strtok(comps, " "); p && n < 128; p = strtok(NULL, " "))
        words[n++] = p;
    if (n == 1) {
        char merged[64];
        snprintf(merged, sizeof merged, "%s ", words[0]);
        set_text(d->con_in, merged);
        SendMessageA(d->con_in, EM_SETSEL, (WPARAM)strlen(merged), (LPARAM)strlen(merged));
    } else if (n > 1) {
        char list[2048] = "";
        size_t len = strlen(words[0]), l = 0;
        for (i = 1; i < n; i++) {
            size_t k = 0;
            while (k < len && words[i][k] == words[0][k]) k++;
            len = k;
        }
        if (len > strlen(text)) {
            char pre[64];
            snprintf(pre, sizeof pre, "%.*s", (int)len, words[0]);
            set_text(d->con_in, pre);
            SendMessageA(d->con_in, EM_SETSEL, (WPARAM)strlen(pre), (LPARAM)strlen(pre));
        }
        for (i = 0; i < n; i++)
            l = addf(list, l, sizeof list, "%s%s", i ? "  " : "", words[i]);
        console_output(d, list);
    } else {
        MessageBeep(MB_OK);
    }
}

static void on_reg(debugger *d, int i)
{
    char buf[32];
    long v;
    edit_text(d->reg[i], buf, sizeof buf);
    if (parse_num(buf, &v))
        a800debug_cpu_set(d->dbg, k_regs[i], (int)(v & (i == 0 ? 0xFFFF : 0xFF)));
    else
        toast(d, "Not a number ($ or plain hex, # decimal)");
    resync(d);
}

static void on_flag(debugger *d, int i)
{
    if (!a800debug_is_stopped(d->dbg)) {
        refresh_cpu(d);     /* running: the box goes back to the CPU's */
        return;
    }
    a800debug_cpu_set(d->dbg, k_flags[i], SendMessageA(d->flag[i], BM_GETCHECK, 0, 0) == BST_CHECKED);
    resync(d);
}

/* Go to: the dump from just above an address (or a label), its row marked
 * and scrolled to. */
static void on_mem_goto(debugger *d)
{
    char buf[64];
    long a;
    int top;
    edit_text(d->mem_goto, buf, sizeof buf);
    if (!parse_addr(d, buf, &a)) {
        d->mem_mark = -1;
        refresh_mem(d);
        toast(d, "Not an address or a label");
        return;
    }
    d->mem_mark = (int)(a & 0xFFFF);
    d->mem_top = (d->mem_mark - 0x100) & 0xFFF0;
    refresh_mem(d);
    /* the row 30% of the way down */
    top = 1 + ((d->mem_mark - d->mem_top) & 0xFFFF) / 16 - edit_rows(d, d->mem_view) * 3 / 10;
    edit_scroll_to(d->mem_view, top > 0 ? top : 0);
}

/* The dump a page of MEM_ROWS on or back. */
static void on_mem_page(debugger *d, int dir)
{
    d->mem_top = (d->mem_top + dir * MEM_ROWS * 16) & 0xFFFF;
    refresh_mem(d);
}

static void on_mem_write(debugger *d)
{
    char abuf[64], vbuf[32];
    long a, v;
    edit_text(d->mem_addr, abuf, sizeof abuf);
    edit_text(d->mem_val, vbuf, sizeof vbuf);
    if (!parse_addr(d, abuf, &a)) {
        toast(d, "Not an address or a label");
        return;
    }
    if (!parse_num(vbuf, &v) || v < 0 || v > 0xFF) {
        toast(d, "The value is a byte ($00-$FF)");
        return;
    }
    a800debug_write(d->dbg, (uint16_t)a, (uint8_t)v);
    resync(d);
}

static void on_jump(debugger *d)
{
    char buf[64];
    long a;
    edit_text(d->jump, buf, sizeof buf);
    if (!parse_addr(d, buf, &a)) {
        toast(d, "Not an address or a label");
        return;
    }
    SendMessageA(d->follow_pc, BM_SETCHECK, BST_UNCHECKED, 0);
    d->disasm_top = (int)(a & 0xFFFF);
    d->selected = d->disasm_top;
    refresh_disasm(d);
}

/* The selected breakpoint row's index, or -1. */
static int bp_selected(debugger *d)
{
    const int i = (int)SendMessageA(d->bp_list, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
    return i >= 0 && i < d->nbps ? i : -1;
}

static void on_bp_remove(debugger *d)
{
    const int i = bp_selected(d);
    if (i < 0) {
        set_text(d->bp_msg, "Select a breakpoint to remove");
        return;
    }
    a800debug_breakpoint_remove(d->dbg, d->bps[i].id);
    resync(d);
}

static void on_bp_add(debugger *d)
{
    static const int types[4] = { A800DEBUG_BP_EXEC, A800DEBUG_BP_READ, A800DEBUG_BP_WRITE,
                                  A800DEBUG_BP_READ | A800DEBUG_BP_WRITE };
    char start[64], end[64], cond[128], msg[200] = "";
    int sel = (int)SendMessageA(d->bp_type, CB_GETCURSEL, 0, 0), id;
    long a, b;
    if (sel < 0 || sel > 3) sel = 0;
    edit_text(d->bp_start, start, sizeof start);
    edit_text(d->bp_end, end, sizeof end);
    edit_text(d->bp_cond, cond, sizeof cond);
    if (!parse_addr(d, start, &a)) {
        set_text(d->bp_msg, "Not an address or a label");
        return;
    }
    b = a;
    if (*strip(end) && !parse_addr(d, end, &b)) {
        set_text(d->bp_msg, "The end is not an address or a label");
        return;
    }
    id = a800debug_breakpoint_add(d->dbg, types[sel], (uint16_t)a, (uint16_t)b, cond, msg, sizeof msg);
    if (id < 0) {
        set_text(d->bp_msg, msg[0] ? msg : "atari800 did not take it");
        return;
    }
    set_text(d->bp_msg, "");
    set_text(d->bp_start, "");
    set_text(d->bp_end, "");
    set_text(d->bp_cond, "");
    resync(d);
}

/* A scanline break is atari800's own (BLINE): the next Run stops there. */
static void on_bp_line(debugger *d)
{
    static char out[1024];
    char text[32], cmd[32], line[48], *end, *nl;
    long n;
    edit_text(d->bp_line, text, sizeof text);
    n = strtol(text, &end, 10);
    while (*end == ' ') end++;
    if (end == text || *end || n < 0 || n > 311) {
        set_text(d->bp_msg, "A scanline is 0-311 (decimal)");
        return;
    }
    if (d->con_asm) {
        set_text(d->bp_msg, "The Console's assembler is waiting: finish it first");
        return;
    }
    snprintf(cmd, sizeof cmd, "BLINE %ld", n);
    a800debug_command(d->dbg, cmd, out, sizeof out);
    /* in the console's transcript too, as if typed there */
    snprintf(line, sizeof line, "> %s\n", cmd);
    console_append(d, line);
    console_output(d, out);
    if ((nl = strchr(out, '\n')) != NULL) *nl = '\0';
    set_text(d->bp_msg, out[0] ? out : cmd);
    resync(d);
}

static int pick_file(debugger *d, int save, const char *title, const char *filter, const char *initial,
                     char *path, DWORD pathsz)
{
    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof ofn);
    snprintf(path, pathsz, "%s", initial ? initial : "");
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = d->hwnd;
    ofn.lpstrFile = path;
    ofn.nMaxFile = pathsz;
    ofn.lpstrTitle = title;
    ofn.lpstrFilter = filter;
    if (save) {
        ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        return GetSaveFileNameA(&ofn) ? 1 : 0;
    }
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameA(&ofn) ? 1 : 0;
}

static void on_symbols(debugger *d)
{
    char path[MAX_PATH], msg[512], line[512 + MAX_PATH + 32];
    if (!pick_file(d, 0, "Load Symbols (ld65 -Ln, VICE .lbl, MADS .lab, atari800 labels)",
                   "Label files (*.lbl, *.lab, *.sym, *.txt)\0*.lbl;*.lab;*.sym;*.txt\0All files\0*.*\0\0",
                   NULL, path, sizeof path))
        return;
    if (a800debug_load_symbols(d->dbg, path, msg, sizeof msg) >= 0)
        snprintf(line, sizeof line, "Loaded %s from %s", msg, base_name(path));
    else
        snprintf(line, sizeof line, "No symbols loaded: %s", msg);
    say(d, line);
    resync(d);
}

static void on_save(debugger *d, int which)
{
    char path[MAX_PATH], msg[512], line[700], title[96];
    snprintf(title, sizeof title, "Save %s", k_saves[which].title);
    if (!pick_file(d, 1, title, "All files\0*.*\0\0", k_saves[which].file, path, sizeof path)) return;
    if (a800debug_save(d->dbg, k_saves[which].kind, path, msg, sizeof msg) == 0) {
        snprintf(line, sizeof line, "%s: %s", base_name(path), msg);
        say(d, line);
    } else {
        say(d, msg);
    }
}

static void popup_below(debugger *d, HWND button, HMENU m)
{
    RECT r;
    GetWindowRect(button, &r);
    TrackPopupMenu(m, TPM_LEFTALIGN | TPM_TOPALIGN, r.left, r.bottom, 0, d->hwnd, NULL);
    DestroyMenu(m);
}

static void save_menu(debugger *d)
{
    HMENU m = CreatePopupMenu();
    char text[96];
    int i;
    for (i = 0; i < 2; i++) {
        snprintf(text, sizeof text, "%s...", k_saves[i].title);
        AppendMenuA(m, MF_STRING, (UINT_PTR)(IDC_SAVE_DIS + i), text);
    }
    popup_below(d, d->save_btn, m);
}

/* Lines of grey text in a menu, wrapped at about width characters. */
static void menu_note(HMENU m, const char *text, int width)
{
    char line[128];
    while (*text) {
        int n = (int)strlen(text), cut = n;
        if (n > width) {
            for (cut = width; cut > 0 && text[cut] != ' '; cut--) {}
            if (cut == 0) cut = width;
        }
        snprintf(line, sizeof line, "%.*s", cut, text);
        AppendMenuA(m, MF_STRING | MF_GRAYED, 0, line);
        text += cut;
        while (*text == ' ') text++;
    }
}

/* Slots 0-3, each saved over or restored; with FujiNet running, what a
 * restore means for it. */
static void snapshots_menu(debugger *d)
{
    HMENU m = CreatePopupMenu();
    char text[64];
    int i;
    for (i = 0; i < SNAPSHOTS; i++) {
        snprintf(text, sizeof text, "Save to Slot %d", i);
        AppendMenuA(m, MF_STRING, (UINT_PTR)(IDC_SNAP_SAVE0 + i), text);
    }
    AppendMenuA(m, MF_SEPARATOR, 0, NULL);
    for (i = 0; i < SNAPSHOTS; i++) {
        const int has = a800debug_snapshot_exists(d->dbg, i) != 0;
        snprintf(text, sizeof text, "Restore Slot %d\t%s", i, has ? "saved" : "empty");
        AppendMenuA(m, MF_STRING | (has ? 0 : MF_GRAYED), (UINT_PTR)(IDC_SNAP_RESTORE0 + i), text);
    }
    AppendMenuA(m, MF_SEPARATOR, 0, NULL);
    if (a800session_fujinet_running(d->session)) {
        menu_note(m, k_fujinet_note, 52);
        AppendMenuA(m, MF_SEPARATOR, 0, NULL);
    }
    AppendMenuA(m, MF_STRING | MF_GRAYED, 0, "Kept until the app quits.");
    popup_below(d, d->snap_btn, m);
}

static void select_page(debugger *d, int page);

static void on_bp_activate(debugger *d, int i)
{
    if (i < 0 || i >= d->nbps || !(d->bps[i].type & A800DEBUG_BP_EXEC)) return;
    select_page(d, TAB_DISASM);
    SendMessageA(d->follow_pc, BM_SETCHECK, BST_UNCHECKED, 0);
    d->disasm_top = d->selected = d->bps[i].start;
    refresh_disasm(d);
}

static void on_accept(debugger *d, int id)
{
    if (id >= IDC_REG0 && id <= IDC_REG_LAST) {
        on_reg(d, id - IDC_REG0);
        return;
    }
    switch (id) {
    case IDC_CON_IN: on_console(d); break;
    case IDC_MEM_GOTO: on_mem_goto(d); break;
    case IDC_MEM_ADDR:
    case IDC_MEM_VAL: on_mem_write(d); break;
    case IDC_JUMP: on_jump(d); break;
    case IDC_BP_START:
    case IDC_BP_END:
    case IDC_BP_COND: on_bp_add(d); break;
    case IDC_BP_LINE: on_bp_line(d); break;
    default: break;
    }
}

/* ---- the disassembly -------------------------------------------------------------- */

/* Drawn by hand, a line an instruction as the other frontends show it:
 * the breakpoint mark (* on, red; o off, grey), > at the PC, address,
 * bytes, label, instruction and comment. The PC's line is filled with the
 * accent, the selected one bold and underlined. */
static void paint_disasm(debugger *d, HWND hwnd, HDC dc)
{
    RECT c;
    int i;
    GetClientRect(hwnd, &c);
    FillRect(dc, &c, GetSysColorBrush(COLOR_WINDOW));
    SetBkMode(dc, TRANSPARENT);
    if (d->line_count == 0) {
        SelectObject(dc, d->mono);
        SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
        TextOutA(dc, S(d, DIS_MARGIN), 0, "(no disassembly)", 16);
        return;
    }
    for (i = 0; i < d->line_count && i * d->line_h < c.bottom; i++) {
        const a800debug_line *l = &d->lines[i];
        const int y = i * d->line_h, ty = y + (d->line_h - d->mono_h) / 2;
        const int sel = l->address == d->selected;
        COLORREF fg = GetSysColor(COLOR_WINDOWTEXT);
        RECT row;
        char text[256];
        int len;
        SetRect(&row, 0, y, c.right, y + d->line_h);
        if (l->is_pc) {
            FillRect(dc, &row, d->accent);
            fg = RGB(255, 255, 255);
        }
        len = snprintf(text, sizeof text, "%c %04X  %-8s  %-10s %-16s%s%s", l->is_pc ? '>' : ' ',
                       l->address, l->bytes, l->label, l->disasm, l->comment[0] ? " ; " : "", l->comment);
        if (len > (int)sizeof text - 1) len = (int)sizeof text - 1;
        SelectObject(dc, sel ? d->mono_sel : d->mono);
        SetTextColor(dc, fg);
        TextOutA(dc, S(d, DIS_MARGIN) + d->mono_w, ty, text, len);
        if (l->has_breakpoint) {
            const int off = l->has_breakpoint == 2;
            SelectObject(dc, sel ? d->mono_sel : off ? d->mono : d->mono_bold);
            SetTextColor(dc, l->is_pc ? fg : off ? RGB(0x9a, 0x99, 0x96) : RGB(0xe0, 0x1b, 0x24));
            TextOutA(dc, S(d, DIS_MARGIN), ty, off ? "o" : "*", 1);
        }
    }
}

/* A click selects a line (Run to Cursor); a click on its left edge, or a
 * double-click, toggles its breakpoint. The wheel and the keys browse by
 * instructions (and turn Follow PC off). */
static LRESULT CALLBACK disasm_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    debugger *d = g_dbg;
    if (!d) return DefWindowProcA(hwnd, msg, wp, lp);
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT c;
        HDC mem;
        HBITMAP bmp;
        HGDIOBJ oldbmp, oldfont;
        GetClientRect(hwnd, &c);
        mem = CreateCompatibleDC(dc);
        bmp = CreateCompatibleBitmap(dc, c.right > 0 ? c.right : 1, c.bottom > 0 ? c.bottom : 1);
        oldbmp = SelectObject(mem, bmp);
        oldfont = SelectObject(mem, d->mono);
        paint_disasm(d, hwnd, mem);
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
    case WM_SIZE:
        /* more or fewer lines fit now */
        if (d->disasm == hwnd && d->page == TAB_DISASM) refresh_page(d, TAB_DISASM);
        return 0;
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        const int line = d->line_h > 0 ? GET_Y_LPARAM(lp) / d->line_h : -1;
        const int gutter = GET_X_LPARAM(lp) < S(d, DIS_MARGIN) + 3 * d->mono_w;
        SetFocus(hwnd);
        if (line < 0 || line >= d->line_count) return 0;
        d->selected = d->lines[line].address;
        if (gutter ? msg == WM_LBUTTONDOWN : msg == WM_LBUTTONDBLCLK)
            toggle_breakpoint(d, d->lines[line].address);
        else
            InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        const int delta = GET_WHEEL_DELTA_WPARAM(wp);
        int rows = -delta * 3 / WHEEL_DELTA;
        if (!rows) rows = delta > 0 ? -1 : 1;
        SendMessageA(d->follow_pc, BM_SETCHECK, BST_UNCHECKED, 0);
        d->disasm_top = a800debug_row_address(d->dbg, (uint16_t)d->disasm_top, rows);
        refresh_disasm(d);
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_PRIOR || wp == VK_NEXT || wp == VK_UP || wp == VK_DOWN) {
            const int page = visible_rows(d) / 2 > 0 ? visible_rows(d) / 2 : 1;
            const int rows = wp == VK_PRIOR ? -page : wp == VK_NEXT ? page : wp == VK_UP ? -1 : 1;
            SendMessageA(d->follow_pc, BM_SETCHECK, BST_UNCHECKED, 0);
            d->disasm_top = a800debug_row_address(d->dbg, (uint16_t)d->disasm_top, rows);
            refresh_disasm(d);
            return 0;
        }
        break;
    default: break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* ---- the GTIA colours ------------------------------------------------------------- */

/* The colour registers (COLPM0-3, COLPF0-3, then COLBK) as swatches, and
 * the 256 colours, 16 hues down and 16 lumas across, nearest-neighbour so
 * the cells stay crisp, with their hex digits along the edges. */
static void paint_colours(debugger *d, HWND hwnd, HDC dc)
{
    static const char *const names[SWATCHES] = {
        "COLPM0", "COLPM1", "COLPM2", "COLPM3", "COLPF0", "COLPF1", "COLPF2", "COLPF3", "COLBK" };
    static const char k_regs_title[] = "Colour registers";
    static const char k_pal_title[] = "Palette: hues down, lumas across";
    const int cw = colours_w(d) / 4, top = pal_top(d), ax = S(d, PAL_AXIS_W), ay = S(d, PAL_AXIS_H);
    const int pw = S(d, PAL_CELL_W), ph = S(d, PAL_CELL_H);
    RECT c;
    int i;

    GetClientRect(hwnd, &c);
    FillRect(dc, &c, GetSysColorBrush(COLOR_BTNFACE));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
    SelectObject(dc, d->ui_bold);
    TextOutA(dc, 0, S(d, 3), k_regs_title, (int)strlen(k_regs_title));
    TextOutA(dc, 0, top - S(d, COL_TITLE_H) + S(d, 3), k_pal_title, (int)strlen(k_pal_title));

    SelectObject(dc, d->mono);
    for (i = 0; i < SWATCHES; i++) {
        const int x = (i & 3) * cw, y = S(d, COL_TITLE_H) + (i / 4) * S(d, COL_ROW_H);
        const uint32_t rgb = d->swatch_rgb[i];
        HBRUSH b = CreateSolidBrush(RGB((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff));
        RECT sw;
        char text[24];
        SetRect(&sw, x, y + S(d, 4), x + S(d, 22), y + S(d, 22));
        FillRect(dc, &sw, b);
        DeleteObject(b);
        FrameRect(dc, &sw, (HBRUSH)GetStockObject(GRAY_BRUSH));
        snprintf(text, sizeof text, "%s $%02X", names[i], d->swatch_val[i]);
        TextOutA(dc, x + S(d, 28), y + S(d, 13) - d->mono_h / 2, text, (int)strlen(text));
    }

    for (i = 0; i < 16; i++) {
        char t[4];
        snprintf(t, sizeof t, "%X", i);
        TextOutA(dc, ax + i * pw + (pw - d->mono_w) / 2, top + ay - d->mono_h - 1, t, 1);
        snprintf(t, sizeof t, "%X0", i);
        TextOutA(dc, S(d, 4), top + ay + i * ph + (ph - d->mono_h) / 2, t, 2);
    }
    if (d->palette_ok) {
        BITMAPINFO bmi;
        memset(&bmi, 0, sizeof bmi);
        bmi.bmiHeader.biSize = sizeof bmi.bmiHeader;
        bmi.bmiHeader.biWidth = A800DEBUG_PALETTE_WIDTH;
        bmi.bmiHeader.biHeight = -A800DEBUG_PALETTE_HEIGHT;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        SetStretchBltMode(dc, COLORONCOLOR);
        StretchDIBits(dc, ax, top + ay, 16 * pw, 16 * ph,
                      0, 0, A800DEBUG_PALETTE_WIDTH, A800DEBUG_PALETTE_HEIGHT, d->palette_px, &bmi,
                      DIB_RGB_COLORS, SRCCOPY);
    }
}

static LRESULT CALLBACK colours_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    debugger *d = g_dbg;
    if (msg == WM_PAINT && d) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT c;
        HDC mem;
        HBITMAP bmp;
        HGDIOBJ oldbmp, oldfont;
        GetClientRect(hwnd, &c);
        mem = CreateCompatibleDC(dc);
        bmp = CreateCompatibleBitmap(dc, c.right > 0 ? c.right : 1, c.bottom > 0 ? c.bottom : 1);
        oldbmp = SelectObject(mem, bmp);
        oldfont = SelectObject(mem, d->mono);
        paint_colours(d, hwnd, mem);
        BitBlt(dc, 0, 0, c.right, c.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldfont);
        SelectObject(mem, oldbmp);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    if (msg == WM_ERASEBKGND) return 1;
    if (msg == WM_LBUTTONDOWN && d) {
        /* a colour's value, from the palette */
        const int x = GET_X_LPARAM(lp) - S(d, PAL_AXIS_W), y = GET_Y_LPARAM(lp) - pal_top(d) - S(d, PAL_AXIS_H);
        int hue, luma, index;
        char text[96];
        if (x < 0 || y < 0) return 0;
        luma = x / S(d, PAL_CELL_W);
        hue = y / S(d, PAL_CELL_H);
        if (luma > 15 || hue > 15) return 0;
        index = hue << 4 | luma;
        snprintf(text, sizeof text, "Colour $%02X: hue %d, luma %d, #%06X", index, hue, luma,
                 (unsigned)a800debug_color(d->dbg, (uint8_t)index) & 0xFFFFFFu);
        set_text(d->palette_info, text);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* ---- edit subclassing ------------------------------------------------------------- */

static WNDPROC g_edit_proc;

/* Enter in a single-line field means "apply this value"; Tab in the console
 * completes and Up/Down recall. None may beep its way through the default
 * handler. */
static LRESULT CALLBACK edit_subclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    const int id = (int)GetWindowLongPtrA(hwnd, GWLP_ID);
    if (msg == WM_KEYDOWN && wp == VK_RETURN) {
        PostMessageA(GetAncestor(hwnd, GA_ROOT), WM_DBG_ACCEPT, (WPARAM)id, 0);
        return 0;
    }
    if (msg == WM_CHAR && (wp == '\r' || wp == '\n')) return 0;
    if (id == IDC_CON_IN) {
        if (msg == WM_KEYDOWN && wp == VK_TAB) {
            PostMessageA(GetAncestor(hwnd, GA_ROOT), WM_DBG_TAB, 0, 0);
            return 0;
        }
        if (msg == WM_CHAR && wp == '\t') return 0;
        if (msg == WM_KEYDOWN && (wp == VK_UP || wp == VK_DOWN)) {
            PostMessageA(GetAncestor(hwnd, GA_ROOT), WM_DBG_HISTORY, wp == VK_DOWN, 0);
            return 0;
        }
        if (msg == WM_GETDLGCODE) return DLGC_WANTALLKEYS;
    }
    return CallWindowProcA(g_edit_proc, hwnd, msg, wp, lp);
}

/* ---- construction ----------------------------------------------------------------- */

static HWND child_ex(debugger *d, DWORD ex, const char *cls, const char *text, DWORD style, int id, HFONT font)
{
    HWND h = CreateWindowExA(ex, cls, text, WS_CHILD | style, 0, 0, 10, 10, d->hwnd, (HMENU)(INT_PTR)id,
                             (HINSTANCE)GetWindowLongPtrA(d->hwnd, GWLP_HINSTANCE), NULL);
    SendMessageA(h, WM_SETFONT, (WPARAM)font, TRUE);
    return h;
}

static HWND child(debugger *d, const char *cls, const char *text, DWORD style, int id, HFONT font)
{
    return child_ex(d, 0, cls, text, style, id, font);
}

/* A read-only monospace text view. */
static HWND view(debugger *d, int id, DWORD extra)
{
    HWND h = child(d, "EDIT", "",
                   WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | extra, id, d->mono);
    SendMessageA(h, EM_SETLIMITTEXT, 0, 0);
    if (d->nviews < (int)(sizeof d->views / sizeof d->views[0])) d->views[d->nviews++] = h;
    return h;
}

static HWND field(debugger *d, int id, const wchar_t *cue)
{
    HWND h = child(d, "EDIT", "", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, id, d->mono);
    g_edit_proc = (WNDPROC)SetWindowLongPtrA(h, GWLP_WNDPROC, (LONG_PTR)edit_subclass);
    if (cue) SendMessageA(h, EM_SETCUEBANNER, TRUE, (LPARAM)cue);
    return h;
}

static HWND label(debugger *d, const char *text, HFONT font, DWORD style)
{
    return child(d, "STATIC", text, style, IDC_LABEL_FIRST + d->nlabels++, font);
}

/* A caption or hint, drawn grey. */
static HWND dim_label(debugger *d, const char *text, DWORD style)
{
    HWND h = label(d, text, d->ui, style);
    if (d->ndim < (int)(sizeof d->dim / sizeof d->dim[0])) d->dim[d->ndim++] = h;
    return h;
}

static HWND button(debugger *d, const char *text, int id)
{
    return child(d, "BUTTON", text, BS_PUSHBUTTON | WS_TABSTOP, id, d->ui);
}

static void tip(debugger *d, HWND h, const char *text)
{
    TOOLINFOA ti;
    memset(&ti, 0, sizeof ti);
    ti.cbSize = TTTOOLINFOA_V2_SIZE;
    ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd = d->hwnd;
    ti.uId = (UINT_PTR)h;
    ti.lpszText = (char *)text;
    SendMessageA(d->tip, TTM_ADDTOOLA, 0, (LPARAM)&ti);
}

static void size_columns(debugger *d)
{
    static const int widths[4] = { 90, 100, 130, 420 };
    int i;
    for (i = 0; i < 4; i++)
        SendMessageA(d->bp_list, LVM_SETCOLUMNWIDTH, (WPARAM)i, S(d, widths[i]));
}

static void build_controls(debugger *d)
{
    static const char *const bp_types[4] = { "Execute", "Read", "Write", "Read/Write" };
    HINSTANCE inst = (HINSTANCE)GetWindowLongPtrA(d->hwnd, GWLP_HINSTANCE);
    TCITEMA item;
    int i;

    d->tip = CreateWindowExA(WS_EX_TOPMOST, TOOLTIPS_CLASSA, NULL, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                             CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, d->hwnd, NULL, inst, NULL);
    SendMessageA(d->tip, TTM_SETMAXTIPWIDTH, 0, S(d, 420));

    d->tabs = child(d, WC_TABCONTROLA, "", WS_VISIBLE | WS_CLIPSIBLINGS, IDC_TABS, d->ui);
    memset(&item, 0, sizeof item);
    item.mask = TCIF_TEXT;
    for (i = 0; i < TAB_COUNT; i++) {
        /* "&" would underline the next letter */
        char text[32];
        const char *s;
        size_t o = 0;
        for (s = k_tabs[i].label; *s && o < sizeof text - 2; s++) {
            if (*s == '&') text[o++] = '&';
            text[o++] = *s;
        }
        text[o] = '\0';
        item.pszText = text;
        SendMessageA(d->tabs, TCM_INSERTITEMA, (WPARAM)i, (LPARAM)&item);
    }

    /* The toolbar. Run/Stop is owner-drawn so it can wear the accent colour
     * while the machine is stopped; Break on BRK stays down while it is on. */
    d->run_btn = child(d, "BUTTON", "Stop (F5)", BS_OWNERDRAW | WS_TABSTOP, IDC_RUN, d->ui);
    d->step_btn = button(d, "Step (F7)", IDC_STEP);
    d->over_btn = button(d, "Step Over (F8)", IDC_OVER);
    d->out_btn = button(d, "Step Out (Shift+F8)", IDC_OUT);
    d->frame_btn = button(d, "Frame (F9)", IDC_FRAME);
    d->run_to_btn = button(d, "Run to Cursor", IDC_RUN_TO);
    d->brk_btn = child(d, "BUTTON", "Break on BRK", BS_AUTOCHECKBOX | BS_PUSHLIKE | WS_TABSTOP, IDC_BRK, d->ui);
    d->status = child(d, "STATIC", "", SS_RIGHT | SS_ENDELLIPSIS | SS_NOTIFY, IDC_STATUS, d->ui);
    if (d->ndim < (int)(sizeof d->dim / sizeof d->dim[0])) d->dim[d->ndim++] = d->status;
    d->snap_btn = button(d, "Snapshots...", IDC_SNAPSHOTS);
    d->sym_btn = button(d, "Load Symbols...", IDC_SYMBOLS);
    d->save_btn = button(d, "Save...", IDC_SAVE);
    {
        HWND bar[11] = { d->run_btn, d->step_btn, d->over_btn, d->out_btn, d->frame_btn, d->run_to_btn,
                         d->brk_btn, d->status, d->snap_btn, d->sym_btn, d->save_btn };
        for (i = 0; i < 11; i++) ShowWindow(bar[i], SW_SHOW);
    }
    tip(d, d->step_btn, "One instruction");
    tip(d, d->over_btn, "Over a JSR, to the instruction after it");
    tip(d, d->out_btn, "On to the RTS or RTI");
    tip(d, d->frame_btn, "On to the next vertical blank");
    tip(d, d->run_to_btn, "On to the line selected in the Disassembly");
    tip(d, d->brk_btn, "Stop at a BRK instead of running it");
    tip(d, d->status, "");
    tip(d, d->snap_btn, "Temporary save states, slots 0-3");
    tip(d, d->sym_btn, "Labels from ld65 -Ln, VICE .lbl, MADS .lab or an atari800 label file, "
                       "beside the OS's own");

    /* Console */
    d->con_out = view(d, IDC_CON_OUT, 0);
    set_text(d->con_out, "atari800's monitor. Type HELP for its commands; Tab completes them, Up/Down "
                         "recall. G, R, O and CONT let the machine go; A assembles a line at a time.\r\n");
    d->con_prompt = child(d, "STATIC", "> ", SS_LEFT, IDC_CON_PROMPT, d->mono);
    d->con_in = field(d, IDC_CON_IN, NULL);

    /* CPU & Memory */
    for (i = 0; i < 6; i++) {
        d->reg_label[i] = dim_label(d, k_reg_names[i], SS_LEFT);
        d->reg[i] = field(d, IDC_REG0 + i, NULL);
        SendMessageA(d->reg[i], EM_SETLIMITTEXT, 6, 0);
        tip(d, d->reg[i], "Enter sets it");
    }
    for (i = 0; i < 6; i++)
        d->flag[i] = child(d, "BUTTON", k_flag_names[i], BS_AUTOCHECKBOX | WS_TABSTOP, IDC_FLAG0 + i, d->ui);
    d->beam = dim_label(d, "", SS_LEFT);
    d->mem_title = label(d, "Memory, as the CPU sees it (reads have no side effects)", d->ui_bold, SS_LEFT);
    d->mem_view = view(d, IDC_MEM_VIEW, WS_HSCROLL | ES_NOHIDESEL);
    d->hist_title = label(d, "Last instructions", d->ui_bold, SS_LEFT);
    d->hist_view = view(d, IDC_HIST_VIEW, WS_HSCROLL);
    d->goto_label = dim_label(d, "Go to", SS_LEFT);
    d->mem_goto = field(d, IDC_MEM_GOTO, L"$0600 / label");
    d->mem_back = button(d, "Back 4K", IDC_MEM_BACK);
    d->mem_on = button(d, "On 4K", IDC_MEM_ON);
    d->write_label = dim_label(d, "Write", SS_LEFT);
    d->mem_addr = field(d, IDC_MEM_ADDR, L"$0600");
    d->value_label = dim_label(d, "value", SS_LEFT);
    d->mem_val = field(d, IDC_MEM_VAL, L"$00");
    d->mem_hint = dim_label(d, "a CPU write: hardware registers take it as the 6502's would",
                            SS_LEFT | SS_ENDELLIPSIS);

    /* Disassembly */
    d->follow_pc = child(d, "BUTTON", "Follow PC", BS_AUTOCHECKBOX | WS_TABSTOP, IDC_FOLLOW_PC, d->ui);
    SendMessageA(d->follow_pc, BM_SETCHECK, BST_CHECKED, 0);
    d->jump_label = dim_label(d, "Jump to", SS_LEFT);
    d->jump = field(d, IDC_JUMP, L"$E456 or CIOV");
    d->disasm_hint = dim_label(d, "Click a line to select it (Run to Cursor); click its left edge or "
                                  "double-click it for a breakpoint (* on, o off); scroll to browse",
                               SS_LEFT | SS_ENDELLIPSIS);
    d->disasm = CreateWindowExA(WS_EX_CLIENTEDGE, "A800DbgDisasm", "", WS_CHILD | WS_TABSTOP, 0, 0, 10, 10,
                                d->hwnd, (HMENU)(INT_PTR)IDC_DISASM, inst, NULL);

    /* ANTIC */
    d->antic_title = label(d, "Registers", d->ui_bold, SS_LEFT);
    d->antic_text = view(d, IDC_ANTIC_TEXT, WS_HSCROLL);
    d->dlist_title = label(d, "Display list", d->ui_bold, SS_LEFT);
    d->dlist_text = view(d, IDC_DLIST_TEXT, WS_HSCROLL);

    /* GTIA */
    d->gtia_text = view(d, IDC_GTIA_TEXT, WS_HSCROLL);
    d->colours = child(d, "A800DbgColours", "", 0, IDC_COLOURS, d->ui);
    d->palette_info = child(d, "EDIT", "Click a colour for its value", ES_READONLY | ES_AUTOHSCROLL,
                            IDC_PALETTE_INFO, d->ui);
    if (d->ndim < (int)(sizeof d->dim / sizeof d->dim[0])) d->dim[d->ndim++] = d->palette_info;

    /* POKEY & PIA, Memory Map */
    d->pokey_text = view(d, IDC_POKEY_TEXT, WS_HSCROLL);
    d->map_text = view(d, IDC_MAP_TEXT, WS_HSCROLL);

    /* Breakpoints */
    d->bp_budget = dim_label(d, "", SS_LEFT | SS_NOTIFY);
    tip(d, d->bp_budget, "atari800 checks up to 20 entries before each instruction: a breakpoint takes "
                         "one (two for a range), each condition one more, and one joins each to the next");
    d->bp_remove = button(d, "Remove", IDC_BP_REMOVE);
    d->bp_clear = button(d, "Clear All", IDC_BP_CLEAR);
    d->bp_list = child_ex(d, WS_EX_CLIENTEDGE, WC_LISTVIEWA, "",
                          WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_SINGLESEL, IDC_BP_LIST, d->mono);
    SendMessageA(d->bp_list, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT);
    {
        static const char *const cols[4] = { "#", "Type", "Address", "Condition" };
        LVCOLUMNA col;
        memset(&col, 0, sizeof col);
        col.mask = LVCF_TEXT;
        for (i = 0; i < 4; i++) {
            col.pszText = (char *)cols[i];
            SendMessageA(d->bp_list, LVM_INSERTCOLUMNA, (WPARAM)i, (LPARAM)&col);
        }
        size_columns(d);
    }
    d->bp_empty = dim_label(d, "No breakpoints or watchpoints. Click the left edge of a disassembly line, "
                               "or add one below.", SS_CENTER);
    d->bp_type = child(d, "COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDC_BP_TYPE, d->ui);
    for (i = 0; i < 4; i++) SendMessageA(d->bp_type, CB_ADDSTRING, 0, (LPARAM)bp_types[i]);
    SendMessageA(d->bp_type, CB_SETCURSEL, 0, 0);
    d->bp_start = field(d, IDC_BP_START, L"$0600 / label");
    d->bp_end = field(d, IDC_BP_END, L"(end)");
    d->bp_cond = field(d, IDC_BP_COND, L"condition: A=$10 && X<4 && MEM[$D01F]=6 \x2014 optional");
    d->bp_add = button(d, "Add", IDC_BP_ADD);
    d->bp_line_label = dim_label(d, "Break at scanline", SS_LEFT);
    d->bp_line = field(d, IDC_BP_LINE, L"100");
    d->bp_line_set = button(d, "Set", IDC_BP_LINE_SET);
    d->bp_line_hint = dim_label(d, "atari800's BLINE: the next Run (or step) stops when the beam reaches it, once",
                                SS_LEFT | SS_ENDELLIPSIS);
    d->bp_msg = label(d, "", d->ui, SS_LEFT | SS_ENDELLIPSIS);

    /* SIO */
    d->sio_note = dim_label(d, "", SS_LEFT | SS_ENDELLIPSIS);
    d->sio_clear = button(d, "Clear", IDC_SIO_CLEAR);
    d->sio_text = view(d, IDC_SIO_TEXT, WS_HSCROLL);
}

static void show_page(debugger *d, int page)
{
    HWND console[] = { d->con_out, d->con_prompt, d->con_in };
    HWND cpu[] = { d->beam, d->mem_title, d->mem_view, d->hist_title, d->hist_view, d->goto_label, d->mem_goto,
                   d->mem_back, d->mem_on, d->write_label, d->mem_addr, d->value_label, d->mem_val, d->mem_hint };
    HWND dis[] = { d->follow_pc, d->jump_label, d->jump, d->disasm_hint, d->disasm };
    HWND antic[] = { d->antic_title, d->antic_text, d->dlist_title, d->dlist_text };
    HWND gtia[] = { d->gtia_text, d->colours, d->palette_info };
    HWND brk[] = { d->bp_budget, d->bp_remove, d->bp_clear, d->bp_type, d->bp_start, d->bp_end, d->bp_cond,
                   d->bp_add, d->bp_line_label, d->bp_line, d->bp_line_set, d->bp_line_hint, d->bp_msg };
    HWND sio[] = { d->sio_note, d->sio_clear, d->sio_text };
    size_t i;
    d->page = page;
#define SHOW(arr, p) for (i = 0; i < sizeof(arr) / sizeof((arr)[0]); i++) \
                         ShowWindow((arr)[i], page == (p) ? SW_SHOW : SW_HIDE)
    SHOW(console, TAB_CONSOLE);
    SHOW(cpu, TAB_CPU);
    SHOW(d->reg_label, TAB_CPU);
    SHOW(d->reg, TAB_CPU);
    SHOW(d->flag, TAB_CPU);
    SHOW(dis, TAB_DISASM);
    SHOW(antic, TAB_ANTIC);
    SHOW(gtia, TAB_GTIA);
    ShowWindow(d->pokey_text, page == TAB_POKEY ? SW_SHOW : SW_HIDE);
    ShowWindow(d->map_text, page == TAB_MAP ? SW_SHOW : SW_HIDE);
    SHOW(brk, TAB_BREAKS);
    SHOW(sio, TAB_SIO);
#undef SHOW
    show_bp_list(d);
}

static void select_page(debugger *d, int page)
{
    SendMessageA(d->tabs, TCM_SETCURSEL, (WPARAM)page, 0);
    show_page(d, page);
    refresh_page(d, page);
}

static void place(HWND h, int x, int y, int w, int hgt)
{
    MoveWindow(h, x, y, w > 1 ? w : 1, hgt > 1 ? hgt : 1, TRUE);
}

static void layout(debugger *d)
{
    RECT client, page;
    int x, y, w, i, px, py, pw, ph;
    const int bh = S(d, 26), eh = S(d, 24), lh = S(d, 18), gap = S(d, 6);

    if (!d->tabs) return;
    GetClientRect(d->hwnd, &client);

    /* The toolbar: stepping on the left, the snapshots and files on the
     * right, the status between. */
    {
        HWND left[7] = { d->run_btn, d->step_btn, d->over_btn, d->out_btn, d->frame_btn, d->run_to_btn,
                         d->brk_btn };
        const char *ltext[7] = { "Stop (F5)", "Step (F7)", "Step Over (F8)", "Step Out (Shift+F8)",
                                 "Frame (F9)", "Run to Cursor", "Break on BRK" };
        HWND rgt[3] = { d->snap_btn, d->sym_btn, d->save_btn };
        const char *rtext[3] = { "Snapshots...", "Load Symbols...", "Save..." };
        int right = client.right - S(d, 8);
        x = S(d, 8);
        for (i = 0; i < 7; i++) {
            w = btn_w(d, ltext[i]);
            place(left[i], x, S(d, 8), w, bh);
            x += w + gap;
        }
        for (i = 2; i >= 0; i--) {
            w = btn_w(d, rtext[i]);
            right -= w;
            place(rgt[i], right, S(d, 8), w, bh);
            right -= gap;
        }
        y = S(d, 8) + bh + S(d, 8);
        if (right - x - 2 * gap >= S(d, 200)) {
            place(d->status, x + gap, S(d, 8) + (bh - lh) / 2, right - x - 2 * gap, lh);
        } else {
            /* narrow: the status on a line of its own */
            place(d->status, S(d, 8), y - S(d, 2), client.right - S(d, 16), lh);
            y += lh + S(d, 4);
        }
    }

    /* The tab control is the strip alone: the pages sit on the window's own
     * background, which their captions and boxes are drawn on. */
    SetRect(&page, 0, 0, S(d, 400), S(d, 400));
    SendMessageA(d->tabs, TCM_ADJUSTRECT, FALSE, (LPARAM)&page);
    place(d->tabs, S(d, 8), y, client.right - S(d, 16), page.top);
    px = S(d, 12);
    py = y + page.top + S(d, 6);
    pw = client.right - 2 * px;
    ph = client.bottom - S(d, 10) - py;
    if (pw < S(d, 300)) pw = S(d, 300);
    if (ph < S(d, 240)) ph = S(d, 240);

    /* Console: the transcript, then the prompt and its line */
    w = d->mono_w * (int)strlen(d->con_prompt_text) + S(d, 6);
    place(d->con_out, px, py, pw, ph - eh - S(d, 8));
    place(d->con_prompt, px, py + ph - eh + (eh - d->mono_h) / 2, w, d->mono_h);
    place(d->con_in, px + w + S(d, 2), py + ph - eh, pw - w - S(d, 2), eh);

    /* CPU & Memory: the registers, the flags and the beam; the dump and the
     * last instructions side by side; Go to and Write under them */
    x = px;
    for (i = 0; i < 6; i++) {
        w = text_w(d, d->ui, k_reg_names[i]) + S(d, 6);
        place(d->reg_label[i], x, py + (eh - lh) / 2, w, lh);
        x += w;
        w = d->mono_w * (i == 0 ? 6 : 4) + S(d, 10);
        place(d->reg[i], x, py, w, eh);
        x += w + S(d, 14);
    }
    y = py + eh + S(d, 6);
    x = px;
    for (i = 0; i < 6; i++) {
        place(d->flag[i], x, y, S(d, 38), S(d, 22));
        x += S(d, 42);
    }
    place(d->beam, x + S(d, 16), y + (S(d, 22) - lh) / 2, pw - (x - px) - S(d, 16), lh);
    y += S(d, 30);
    {
        const int bottom = py + ph - eh;
        int rw = S(d, 380), lw;
        if (pw - rw - S(d, 8) < S(d, 560)) rw = pw - S(d, 568) > S(d, 240) ? pw - S(d, 568) : S(d, 240);
        lw = pw - rw - S(d, 8);
        place(d->mem_title, px, y, lw, lh);
        place(d->hist_title, px + lw + S(d, 8), y, rw, lh);
        y += lh + S(d, 4);
        place(d->mem_view, px, y, lw, bottom - S(d, 6) - y);
        place(d->hist_view, px + lw + S(d, 8), y, rw, bottom - S(d, 6) - y);
        y = bottom + (eh - lh) / 2;
        x = px;
        w = text_w(d, d->ui, "Go to") + S(d, 6);
        place(d->goto_label, x, y, w, lh);
        x += w;
        place(d->mem_goto, x, bottom, S(d, 110), eh);
        x += S(d, 116);
        w = btn_w(d, "Back 4K");
        place(d->mem_back, x, bottom - S(d, 1), w, bh);
        x += w + S(d, 4);
        w = btn_w(d, "On 4K");
        place(d->mem_on, x, bottom - S(d, 1), w, bh);
        x += w + S(d, 18);
        w = text_w(d, d->ui, "Write") + S(d, 6);
        place(d->write_label, x, y, w, lh);
        x += w;
        place(d->mem_addr, x, bottom, S(d, 90), eh);
        x += S(d, 96);
        w = text_w(d, d->ui, "value") + S(d, 6);
        place(d->value_label, x, y, w, lh);
        x += w;
        place(d->mem_val, x, bottom, S(d, 44), eh);
        x += S(d, 54);
        place(d->mem_hint, x, y, px + pw - x, lh);
    }

    /* Disassembly: Follow PC and Jump to over the lines */
    w = text_w(d, d->ui, "Follow PC") + S(d, 24);
    place(d->follow_pc, px, py + S(d, 1), w, S(d, 22));
    x = px + w + S(d, 12);
    w = text_w(d, d->ui, "Jump to") + S(d, 6);
    place(d->jump_label, x, py + (eh - lh) / 2, w, lh);
    x += w;
    place(d->jump, x, py, S(d, 140), eh);
    x += S(d, 152);
    place(d->disasm_hint, x, py + (eh - lh) / 2, px + pw - x, lh);
    place(d->disasm, px, py + eh + S(d, 6), pw, ph - eh - S(d, 6));

    /* ANTIC: the registers left, the display list right */
    w = pw * 58 / 100 > S(d, 360) ? pw * 58 / 100 : S(d, 360);
    place(d->antic_title, px, py, w, lh);
    place(d->antic_text, px, py + lh + S(d, 4), w, ph - lh - S(d, 4));
    place(d->dlist_title, px + w + S(d, 8), py, pw - w - S(d, 8), lh);
    place(d->dlist_text, px + w + S(d, 8), py + lh + S(d, 4), pw - w - S(d, 8), ph - lh - S(d, 4));

    /* GTIA: the registers left, the colours and the palette right */
    {
        const int rw = colours_w(d) + S(d, 8);
        const int lw = pw - rw - S(d, 12) > S(d, 300) ? pw - rw - S(d, 12) : S(d, 300);
        place(d->gtia_text, px, py, lw, ph);
        place(d->colours, px + lw + S(d, 12), py, rw, colours_h(d));
        place(d->palette_info, px + lw + S(d, 12), py + colours_h(d) + S(d, 8), rw, lh + S(d, 2));
    }

    /* POKEY & PIA, Memory Map */
    place(d->pokey_text, px, py, pw, ph);
    place(d->map_text, px, py, pw, ph);

    /* Breakpoints: the budget and the list; Add, the scanline and a message
     * under them */
    {
        const int msg_y = py + ph - lh, line_y = msg_y - S(d, 8) - eh, add_y = line_y - S(d, 8) - eh;
        const int list_y = py + bh + S(d, 6);
        int bw = btn_w(d, "Clear All"), rbw = btn_w(d, "Remove");
        place(d->bp_clear, px + pw - bw, py, bw, bh);
        place(d->bp_remove, px + pw - bw - gap - rbw, py, rbw, bh);
        place(d->bp_budget, px, py + (bh - lh) / 2, pw - bw - rbw - 3 * gap, lh);
        place(d->bp_list, px, list_y, pw, add_y - gap - list_y);
        place(d->bp_empty, px, list_y + S(d, 12), pw, lh);
        x = px;
        place(d->bp_type, x, add_y, S(d, 110), S(d, 200));
        x += S(d, 116);
        place(d->bp_start, x, add_y, S(d, 110), eh);
        x += S(d, 116);
        place(d->bp_end, x, add_y, S(d, 70), eh);
        x += S(d, 76);
        bw = btn_w(d, "Add");
        place(d->bp_cond, x, add_y, px + pw - bw - gap - x, eh);
        place(d->bp_add, px + pw - bw, add_y - S(d, 1), bw, bh);
        x = px;
        w = text_w(d, d->ui, "Break at scanline") + S(d, 6);
        place(d->bp_line_label, x, line_y + (eh - lh) / 2, w, lh);
        x += w;
        place(d->bp_line, x, line_y, S(d, 54), eh);
        x += S(d, 60);
        bw = btn_w(d, "Set");
        place(d->bp_line_set, x, line_y - S(d, 1), bw, bh);
        x += bw + S(d, 8);
        place(d->bp_line_hint, x, line_y + (eh - lh) / 2, px + pw - x, lh);
        place(d->bp_msg, px, msg_y, pw, lh);
    }

    /* SIO: the note and Clear over the trace */
    w = btn_w(d, "Clear");
    place(d->sio_note, px, py + (bh - lh) / 2, pw - w - S(d, 12), lh);
    place(d->sio_clear, px + pw - w, py, w, bh);
    place(d->sio_text, px, py + bh + S(d, 6), pw, ph - bh - S(d, 6));
}

/* ---- the window ------------------------------------------------------------------- */

static void hide_window(debugger *d)
{
    KillTimer(d->hwnd, TIMER_REFRESH);
    if (d->con_asm) {
        /* A waiting assembler ends with the empty line it asks for: given up
         * by the detach instead, the engine took no command after it. */
        static char out[4096];
        a800debug_command(d->dbg, "", out, sizeof out);
        console_output(d, out);
    }
    a800debug_detach(d->dbg);     /* lets it run */
    d->attached_running = 0;
    set_prompt(d, 0, NULL);
    ShowWindow(d->hwnd, SW_HIDE);
}

static void draw_run_button(debugger *d, const DRAWITEMSTRUCT *di)
{
    char text[32];
    const int stopped = a800session_is_running(d->session) && a800debug_is_stopped(d->dbg);
    RECT r = di->rcItem;
    if (stopped) {
        FillRect(di->hDC, &r, d->accent);
        FrameRect(di->hDC, &r, (HBRUSH)GetStockObject(GRAY_BRUSH));
        SetTextColor(di->hDC, RGB(255, 255, 255));
    } else {
        DrawFrameControl(di->hDC, &r, DFC_BUTTON,
                         DFCS_BUTTONPUSH | ((di->itemState & ODS_SELECTED) ? DFCS_PUSHED : 0));
        SetTextColor(di->hDC, GetSysColor(COLOR_BTNTEXT));
    }
    SetBkMode(di->hDC, TRANSPARENT);
    GetWindowTextA(di->hwndItem, text, sizeof text);
    DrawTextA(di->hDC, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if (di->itemState & ODS_FOCUS) {
        InflateRect(&r, -3, -3);
        DrawFocusRect(di->hDC, &r);
    }
}

static LRESULT on_notify(debugger *d, NMHDR *nh)
{
    if (nh->hwndFrom == d->tabs && nh->code == (UINT)TCN_SELCHANGE) {
        const int page = (int)SendMessageA(d->tabs, TCM_GETCURSEL, 0, 0);
        show_page(d, page);
        refresh_page(d, page);
        return 0;
    }
    if (nh->hwndFrom != d->bp_list) return 0;
    if (nh->code == (UINT)LVN_ITEMCHANGED) {
        const NMLISTVIEW *nm = (const NMLISTVIEW *)nh;
        /* a row's box: its breakpoint on or off (the list follows after) */
        if (!d->updating && nm->iItem >= 0 && nm->iItem < d->nbps && (nm->uChanged & LVIF_STATE) &&
            ((nm->uNewState ^ nm->uOldState) & LVIS_STATEIMAGEMASK) && (nm->uOldState & LVIS_STATEIMAGEMASK)) {
            const int on = ((nm->uNewState & LVIS_STATEIMAGEMASK) >> 12) == 2;
            if (on != (d->bps[nm->iItem].enabled != 0)) {
                a800debug_breakpoint_enable(d->dbg, d->bps[nm->iItem].id, on);
                PostMessageA(d->hwnd, WM_DBG_SYNC, 0, 0);
            }
        }
    } else if (nh->code == (UINT)NM_DBLCLK) {
        on_bp_activate(d, ((const NMITEMACTIVATE *)nh)->iItem);
    } else if (nh->code == (UINT)LVN_KEYDOWN) {
        /* after the list's own handling: removing rebuilds it */
        if (((const NMLVKEYDOWN *)nh)->wVKey == VK_DELETE)
            PostMessageA(d->hwnd, WM_COMMAND, MAKEWPARAM(IDC_BP_REMOVE, BN_CLICKED), 0);
    }
    return 0;
}

static LRESULT CALLBACK dbg_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    debugger *d = g_dbg;
    if (msg == WM_GETMINMAXINFO) {
        const int dpi = d && d->dpi ? d->dpi : 96;
        ((MINMAXINFO *)lp)->ptMinTrackSize.x = MulDiv(860, dpi, 96);
        ((MINMAXINFO *)lp)->ptMinTrackSize.y = MulDiv(560, dpi, 96);
        return 0;
    }
    if (!d || d->hwnd != hwnd) return DefWindowProcA(hwnd, msg, wp, lp);

    switch (msg) {
    case WM_SIZE:
        layout(d);
        return 0;
    case WM_DPICHANGED: {
        const RECT *r = (const RECT *)lp;
        d->dpi = HIWORD(wp);
        make_fonts(d);
        size_columns(d);
        SendMessageA(d->tip, TTM_SETMAXTIPWIDTH, 0, S(d, 420));
        SetWindowPos(hwnd, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        layout(d);
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    }
    case WM_TIMER:
        if (wp == TIMER_REFRESH) tick(d);
        return 0;
    case WM_DBG_ACCEPT:
        on_accept(d, (int)wp);
        return 0;
    case WM_DBG_TAB:
        complete(d);
        return 0;
    case WM_DBG_HISTORY:
        history_step(d, (int)wp);
        return 0;
    case WM_DBG_SYNC:
        resync(d);
        return 0;
    case WM_NOTIFY:
        return on_notify(d, (NMHDR *)lp);
    case WM_DRAWITEM:
        if (wp == IDC_RUN) {
            draw_run_button(d, (const DRAWITEMSTRUCT *)lp);
            return TRUE;
        }
        break;
    case WM_CTLCOLORSTATIC: {
        HWND h = (HWND)lp;
        HDC dc = (HDC)wp;
        if (in_list(h, d->views, d->nviews)) {
            /* read-only views on the window colour, as text views are */
            SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
            SetBkColor(dc, GetSysColor(COLOR_WINDOW));
            return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
        }
        if (h == d->bp_msg || (h == d->status && notice_showing(d))) {
            SetTextColor(dc, accent_rgb());
            SetBkColor(dc, GetSysColor(COLOR_BTNFACE));
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
        }
        if (in_list(h, d->dim, d->ndim)) {
            SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
            SetBkColor(dc, GetSysColor(COLOR_BTNFACE));
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
        }
        break;
    }
    case WM_MOUSEWHEEL: {
        /* the disassembly scrolls under the pointer, focused or not */
        POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        RECT r;
        GetWindowRect(d->disasm, &r);
        if (d->page == TAB_DISASM && PtInRect(&r, p)) return SendMessageA(d->disasm, msg, wp, lp);
        break;
    }
    case WM_COMMAND: {
        const int id = LOWORD(wp);
        if (id >= IDC_FLAG0 && id <= IDC_FLAG_LAST && HIWORD(wp) == BN_CLICKED) {
            on_flag(d, id - IDC_FLAG0);
            return 0;
        }
        if (id >= IDC_SNAP_SAVE0 && id <= IDC_SNAP_SAVE_LAST) {
            on_snap_save(d, id - IDC_SNAP_SAVE0);
            return 0;
        }
        if (id >= IDC_SNAP_RESTORE0 && id <= IDC_SNAP_RESTORE_LAST) {
            on_snap_restore(d, id - IDC_SNAP_RESTORE0);
            return 0;
        }
        switch (id) {
        case IDC_RUN: on_run(d); return 0;
        case IDC_STEP: a800debug_step(d->dbg); settle_and_sync(d); return 0;
        case IDC_OVER: a800debug_step_over(d->dbg); settle_and_sync(d); return 0;
        case IDC_OUT: a800debug_step_out(d->dbg); settle_and_sync(d); return 0;
        case IDC_FRAME: a800debug_frame(d->dbg); settle_and_sync(d); return 0;
        case IDC_RUN_TO: on_run_to(d); return 0;
        case IDC_BRK:
            if (HIWORD(wp) == BN_CLICKED) {
                a800debug_break_on_brk(d->dbg, SendMessageA(d->brk_btn, BM_GETCHECK, 0, 0) == BST_CHECKED);
                resync(d);
            }
            return 0;
        case IDC_SNAPSHOTS: snapshots_menu(d); return 0;
        case IDC_SYMBOLS: on_symbols(d); return 0;
        case IDC_SAVE: save_menu(d); return 0;
        case IDC_SAVE_DIS: on_save(d, 0); return 0;
        case IDC_SAVE_RAM: on_save(d, 1); return 0;
        case IDC_MEM_BACK: on_mem_page(d, -1); return 0;
        case IDC_MEM_ON: on_mem_page(d, 1); return 0;
        case IDC_FOLLOW_PC: refresh_disasm(d); return 0;
        case IDC_BP_ADD: on_bp_add(d); return 0;
        case IDC_BP_REMOVE: on_bp_remove(d); return 0;
        case IDC_BP_CLEAR:
            a800debug_breakpoint_clear(d->dbg);
            resync(d);
            return 0;
        case IDC_BP_LINE_SET: on_bp_line(d); return 0;
        case IDC_SIO_CLEAR:
            a800debug_sio_clear(d->dbg);
            refresh_sio(d, 1);
            return 0;
        case IDCANCEL: hide_window(d); return 0;
        default: break;
        }
        break;
    }
    case WM_CLOSE:
        /* Hide and let the machine run; F12 brings it back, stopped. */
        hide_window(d);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, TIMER_REFRESH);
        a800debug_detach(d->dbg);
        DeleteObject(d->ui);
        DeleteObject(d->ui_bold);
        DeleteObject(d->mono);
        DeleteObject(d->mono_bold);
        DeleteObject(d->mono_sel);
        DeleteObject(d->accent);
        DestroyAcceleratorTable(d->accel);
        free(d);
        g_dbg = NULL;
        return 0;
    default: break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* ---- entry points ----------------------------------------------------------------- */

static void register_class(HINSTANCE inst, const char *name, WNDPROC proc, HBRUSH background, UINT style)
{
    WNDCLASSA wc;
    memset(&wc, 0, sizeof wc);
    wc.style = style;
    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = background;
    wc.lpszClassName = name;
    RegisterClassA(&wc);
}

/* A800_DEBUGGER_TAB: a page number, or a tab's name or label (any case), or
 * the start of one, or of a word of a label ("pia", "memory"...). */
static int tab_from_env(void)
{
    const char *tab = getenv("A800_DEBUGGER_TAB");
    char *end;
    long n;
    size_t len;
    int i, pass;
    if (!tab || !*tab) return -1;
    n = strtol(tab, &end, 10);
    if (end != tab && *end == '\0')
        return (n >= 0 && n < TAB_COUNT) ? (int)n : -1;
    len = strlen(tab);
    for (pass = 0; pass < 3; pass++)
        for (i = 0; i < TAB_COUNT; i++) {
            if (pass == 0 && _strnicmp(tab, k_tabs[i].name, len) == 0) return i;
            if (pass == 1 && _strnicmp(tab, k_tabs[i].label, len) == 0) return i;
            if (pass == 2) {
                const char *p = k_tabs[i].label;
                while ((p = strchr(p, ' ')) != NULL)
                    if (_strnicmp(tab, ++p, len) == 0) return i;
            }
        }
    return -1;
}

void a800_debugger_show(HWND parent, a800session *session)
{
    INITCOMMONCONTROLSEX icc;
    HINSTANCE inst;
    debugger *d;
    ACCEL accels[6];
    int tab;

    if (g_dbg) {
        d = g_dbg;
        if (d->session != session) {
            d->session = session;
            d->dbg = a800session_debugger(session);
        }
        ShowWindow(d->hwnd, IsIconic(d->hwnd) ? SW_RESTORE : SW_SHOW);
        SetForegroundWindow(d->hwnd);
        SetTimer(d->hwnd, TIMER_REFRESH, 100, NULL);
        attach_and_refresh(d);
        return;
    }

    icc.dwSize = sizeof icc;
    icc.dwICC = ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES;
    InitCommonControlsEx(&icc);
    inst = (HINSTANCE)GetWindowLongPtrA(parent, GWLP_HINSTANCE);
    if (!inst) inst = GetModuleHandleA(NULL);
    register_class(inst, "A800DebuggerWindow", dbg_proc, (HBRUSH)(COLOR_BTNFACE + 1), 0);
    register_class(inst, "A800DbgDisasm", disasm_proc, NULL, CS_DBLCLKS);
    register_class(inst, "A800DbgColours", colours_proc, NULL, 0);

    d = calloc(1, sizeof *d);
    if (!d) return;
    d->session = session;
    d->dbg = a800session_debugger(session);
    d->disasm_top = 0xE000;
    d->selected = -1;
    d->mem_mark = -1;
    d->sio_count = -1;
    snprintf(d->con_prompt_text, sizeof d->con_prompt_text, "> ");
    g_dbg = d;

    d->dpi = window_dpi(parent);
    d->hwnd = CreateWindowExA(0, "A800DebuggerWindow", "Debugger", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                              CW_USEDEFAULT, CW_USEDEFAULT, S(d, 1180), S(d, 820), NULL, NULL, inst, NULL);
    if (!d->hwnd) {
        free(d);
        g_dbg = NULL;
        return;
    }

    if (window_dpi(d->hwnd) != d->dpi) {
        /* placed on a monitor of another DPI than the parent's */
        d->dpi = window_dpi(d->hwnd);
        SetWindowPos(d->hwnd, NULL, 0, 0, S(d, 1180), S(d, 820), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    make_fonts(d);
    d->accent = CreateSolidBrush(accent_rgb());

    build_controls(d);
    set_prompt(d, 0, NULL);

    /* F5/F7/F8/Shift+F8/F9/F12 work wherever the focus is in the window. */
    accels[0].fVirt = FVIRTKEY;          accels[0].key = VK_F5;  accels[0].cmd = IDC_RUN;
    accels[1].fVirt = FVIRTKEY;          accels[1].key = VK_F7;  accels[1].cmd = IDC_STEP;
    accels[2].fVirt = FVIRTKEY;          accels[2].key = VK_F8;  accels[2].cmd = IDC_OVER;
    accels[3].fVirt = FVIRTKEY | FSHIFT; accels[3].key = VK_F8;  accels[3].cmd = IDC_OUT;
    accels[4].fVirt = FVIRTKEY;          accels[4].key = VK_F9;  accels[4].cmd = IDC_FRAME;
    accels[5].fVirt = FVIRTKEY;          accels[5].key = VK_F12; accels[5].cmd = IDCANCEL;
    d->accel = CreateAcceleratorTableA(accels, 6);

    tab = tab_from_env();
    d->page = tab >= 0 ? tab : TAB_CONSOLE;
    SendMessageA(d->tabs, TCM_SETCURSEL, (WPARAM)d->page, 0);
    show_page(d, d->page);
    layout(d);

    ShowWindow(d->hwnd, SW_SHOW);
    if (d->page == TAB_CONSOLE) SetFocus(d->con_in);
    SetTimer(d->hwnd, TIMER_REFRESH, 100, NULL);
    attach_and_refresh(d);
}

void a800_debugger_toggle(HWND parent, a800session *session)
{
    if (a800_debugger_visible()) hide_window(g_dbg);
    else a800_debugger_show(parent, session);
}

int a800_debugger_visible(void)
{
    return g_dbg && IsWindowVisible(g_dbg->hwnd);
}

int a800_debugger_pretranslate(MSG *msg)
{
    if (!g_dbg || !g_dbg->accel) return 0;
    if (msg->hwnd != g_dbg->hwnd && !IsChild(g_dbg->hwnd, msg->hwnd)) return 0;
    if (msg->message == WM_KEYDOWN && msg->wParam == VK_F12) {
        hide_window(g_dbg);
        return 1;
    }
    return TranslateAcceleratorA(g_dbg->hwnd, g_dbg->accel, msg) ? 1 : 0;
}
