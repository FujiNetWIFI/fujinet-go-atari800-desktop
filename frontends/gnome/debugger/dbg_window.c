/*
 * Debugger window (GTK4/libadwaita) over the session's debugger engine and
 * atari800's own monitor, via core/include/a800debug.h.
 *
 * Tabs: the Console (atari800's monitor a line at a time, with tab
 * completion and a command history; its A assembler prompts for lines),
 * CPU & Memory (registers, flags, the beam, a hex dump and the last
 * instructions run), the disassembly (click a line to select it for Run to
 * Cursor; click its left edge, or double-click it, for its breakpoint),
 * ANTIC and its display list, GTIA (players, missiles, colours, collisions
 * and the palette), POKEY & PIA, the memory map and cartridge, breakpoints
 * & watchpoints, the SIO command trace, and FujiNet (every NetSIO byte both
 * ways as SIO packets, from session start, beside NetSIO's state and
 * POKEY's serial port). A toolbar carries the stepping controls, the header
 * bar the snapshots, symbol loading and file saves; the family's keys apply
 * (F5 run/stop, F7 step, F8 step over, Shift+F8 step out, F9 frame, F12
 * close).
 *
 * Showing this window attaches (which stops the machine, as on every
 * sibling); hiding it detaches and lets the machine run on.
 *
 * The window polls the engine's generation counter on a short timer and
 * refreshes the tab showing only when something changed; while the machine
 * runs it refreshes the status, the registers and the SIO and FujiNet
 * traces twice a second. At each stop the console takes the banner and
 * registers atari800 printed, as its own monitor shows them.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "dbg_window.h"

#include <stdlib.h>
#include <string.h>

#include "a800debug.h"

#define DISASM_WINDOW 64        /* lines fetched; as many show as fit */
#define MEM_ROWS 256            /* 4K of memory, 16 bytes a row */
#define HISTORY_MAX 64
#define SIO_MAX 512
#define FUJI_MAX 512
#define BP_MAX 64
#define CONSOLE_KEEP (256 * 1024)
#define SNAPSHOTS 4
#define SWATCHES 9
#define PAL_AXIS_W 30           /* the palette's hue labels */
#define PAL_AXIS_H 18           /* ... and its luma labels */
#define PAL_CELL_W 28
#define PAL_CELL_H 16

enum { TAB_CONSOLE, TAB_CPU, TAB_DISASM, TAB_ANTIC, TAB_GTIA, TAB_POKEY, TAB_MAP,
       TAB_BREAKS, TAB_SIO, TAB_FUJINET, TAB_COUNT };

/* The tabs, in order: the names A800_DEBUGGER_TAB takes, and their labels
 * (which it takes too). */
static const struct { const char *name, *label; } k_tabs[TAB_COUNT] = {
    { "console", "Console" }, { "cpu", "CPU & Memory" }, { "disassembly", "Disassembly" },
    { "antic", "ANTIC" }, { "gtia", "GTIA" }, { "pokey", "POKEY & PIA" },
    { "map", "Memory Map" }, { "breakpoints", "Breakpoints" }, { "sio", "SIO" },
    { "fujinet", "FujiNet" },
};

static const int k_regs[6] = { A800_REG_PC, A800_REG_SP, A800_REG_A, A800_REG_X,
                               A800_REG_Y, A800_REG_P };
static const int k_flags[6] = { A800_FLAG_N, A800_FLAG_V, A800_FLAG_D, A800_FLAG_I,
                                A800_FLAG_Z, A800_FLAG_C };

static const char k_fujinet_note[] =
    "FujiNet is running: restoring a snapshot replays the SIO traffic since it was "
    "taken, which FujiNet has already acted on (sectors written, files and "
    "connections opened), so the Atari and FujiNet can disagree afterwards.";

typedef struct {
    GtkWindow *win;
    a800session *session;
    a800debug *dbg;
    unsigned seen_generation;
    gboolean was_stopped;
    gboolean attached_running;  /* attached while the machine was running */
    gboolean updating;          /* a refresh is setting widgets */
    int running_ticks;
    guint timer;

    GtkLabel *status;
    GtkButton *run_btn;
    GtkToggleButton *brk_btn;
    AdwToastOverlay *toasts;
    GtkNotebook *notebook;

    /* snapshots */
    GtkWidget *snap_restore[SNAPSHOTS];
    GtkLabel *snap_state[SNAPSHOTS];
    GtkWidget *snap_note;

    /* console */
    GtkTextView *con_out;
    GtkEntry *con_in;
    GtkLabel *con_prompt;
    gboolean con_asm;           /* the A assembler wants its next line */
    char con_prompt_text[32];
    GPtrArray *history;
    guint history_pos;

    /* cpu + memory */
    GtkEntry *reg[6];           /* PC SP A X Y P */
    GtkCheckButton *flag[6];    /* N V D I Z C */
    GtkLabel *beam;
    GtkTextView *mem_view;
    GtkEntry *mem_goto, *mem_addr, *mem_val;
    int mem_top;
    int mem_mark;               /* the address Go to found; -1 none */
    GtkTextView *hist_view;

    /* disassembly */
    GtkTextView *disasm;
    GtkWidget *disasm_scroll;
    GtkCheckButton *follow_pc;
    GtkEntry *jump;
    int disasm_top;             /* address of the first line */
    int selected;               /* the selected line's address, or -1 */
    uint16_t line_addr[DISASM_WINDOW];
    int line_count;

    /* antic */
    GtkTextView *antic_text, *dlist_text;

    /* gtia */
    GtkTextView *gtia_text;
    GtkWidget *swatch[SWATCHES];
    GtkLabel *swatch_label[SWATCHES];
    guint32 swatch_rgb[SWATCHES];
    GtkWidget *palette;
    GtkLabel *palette_info;
    guint32 *palette_px;
    gboolean palette_ok;

    /* pokey + pia, the memory map */
    GtkTextView *pokey_text;
    GtkTextView *map_text;

    /* breakpoints */
    GtkListBox *bp_list;
    char *bp_sig;               /* what the list shows, to rebuild on change */
    GtkDropDown *bp_type;
    GtkEntry *bp_start, *bp_end, *bp_cond, *bp_line;
    GtkLabel *bp_msg, *bp_budget;

    /* sio */
    GtkTextView *sio_text;
    GtkLabel *sio_note;
    int sio_count;
    a800debug_sio sio_last;

    /* fujinet */
    GtkTextView *fuji_text, *fuji_state;
    GtkLabel *fuji_note;
    int fuji_count;
    uint64_t fuji_last;
} DbgWin;

static DbgWin *g_win;

/* ---- helpers -------------------------------------------------------------- */

/* Engine text for a text buffer: valid UTF-8, control bytes dropped. */
static char *clean_text(const char *text)
{
    char *v = g_utf8_make_valid(text, -1), *s, *d;
    for (s = d = v; *s; s++)
        if ((unsigned char)*s >= 0x20 || *s == '\n' || *s == '\t') *d++ = *s;
    *d = '\0';
    return v;
}

static GtkAdjustment *vadj(GtkTextView *view)
{
    GtkWidget *parent = gtk_widget_get_parent(GTK_WIDGET(view));
    return GTK_IS_SCROLLED_WINDOW(parent)
        ? gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(parent)) : NULL;
}

static gboolean at_end(GtkAdjustment *adj)
{
    return gtk_adjustment_get_value(adj) + gtk_adjustment_get_page_size(adj) >=
           gtk_adjustment_get_upper(adj) - 2.0;
}

/* Logs (the console, the last instructions, the SIO and FujiNet traces)
 * follow their end while their reader is there. New text grows the view
 * only once it is laid out (or its tab first shows), so the end is pinned
 * whenever the view's height changes; whether to follow is decided as text
 * arrives.
 * (GtkTextView moves the value itself while it lays text out, so the
 * value's own changes cannot say whether the reader scrolled away.) */
static gboolean following(GtkTextView *view)
{
    return GPOINTER_TO_INT(g_object_get_data(G_OBJECT(view), "follow")) != 0;
}

static void on_log_resized(GtkAdjustment *adj, gpointer view)
{
    if (following(view))
        gtk_adjustment_set_value(adj, gtk_adjustment_get_upper(adj) -
                                      gtk_adjustment_get_page_size(adj));
}

static void make_log(GtkTextView *view)
{
    g_object_set_data(G_OBJECT(view), "follow", GINT_TO_POINTER(1));
    g_signal_connect(vadj(view), "changed", G_CALLBACK(on_log_resized), view);
}

/* Before new text: follow if the reader is at the end (or never looked). */
static void log_note_reader(GtkTextView *view)
{
    GtkAdjustment *adj = vadj(view);
    g_object_set_data(G_OBJECT(view), "follow", GINT_TO_POINTER(
        gtk_adjustment_get_page_size(adj) <= 0.0 || at_end(adj)));
}

static void log_to_end(GtkTextView *view)
{
    g_object_set_data(G_OBJECT(view), "follow", GINT_TO_POINTER(1));
    on_log_resized(vadj(view), view);
}

/* Replaces a view's text where the reader left it (a refresh must not throw
 * a scrolled view back to its top), or, following, at its end. */
static void set_text_at(GtkTextView *view, const char *text, gboolean follow)
{
    GtkTextBuffer *b = gtk_text_view_get_buffer(view);
    GtkAdjustment *adj = vadj(view);
    char *clean = clean_text(text);
    GtkTextMark *mark;
    GtkTextIter it;
    int top = 0;

    if (!follow && adj && gtk_adjustment_get_value(adj) > 0.0) {
        gtk_text_view_get_line_at_y(view, &it, (int)gtk_adjustment_get_value(adj), NULL);
        top = gtk_text_iter_get_line(&it);
    }
    gtk_text_buffer_set_text(b, clean, -1);
    g_free(clean);
    if (follow) {
        log_to_end(view);
        return;
    }
    if (top <= 0) return;
    /* scroll_to_mark waits for the new text's layout */
    gtk_text_buffer_get_iter_at_line(b, &it, top);
    mark = gtk_text_buffer_create_mark(b, NULL, &it, TRUE);
    gtk_text_view_scroll_to_mark(view, mark, 0.0, TRUE, 0.0, 0.0);
    gtk_text_buffer_delete_mark(b, mark);
}

static void set_text(GtkTextView *view, const char *text)
{
    set_text_at(view, text, FALSE);
}

static int parse_num(const char *text, long *out)
{
    char *end;
    long v;
    while (*text == ' ') text++;
    if (*text == '$') v = strtol(text + 1, &end, 16);
    else if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) v = strtol(text + 2, &end, 16);
    else if (*text == '#') v = strtol(text + 1, &end, 10);
    else v = strtol(text, &end, 16);
    while (*end == ' ') end++;
    if (end == text || *end) return 0;
    *out = v;
    return 1;
}

/* A label or a number, as typed into an address box. */
static int parse_addr(DbgWin *w, const char *text, long *out)
{
    int a;
    while (*text == ' ') text++;
    if (!*text) return 0;
    a = a800debug_label_address(w->dbg, text);
    if (a >= 0) { *out = a; return 1; }
    return parse_num(text, out);
}

/* The live refresh does not overwrite a value being typed: while the
 * machine runs, a focused entry keeps what is in it. */
static void set_entry(DbgWin *w, GtkEntry *e, const char *text)
{
    GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(e));
    GtkWidget *focus = root ? gtk_root_get_focus(root) : NULL;
    if (focus && !a800debug_is_stopped(w->dbg) &&
        (focus == GTK_WIDGET(e) || gtk_widget_is_ancestor(focus, GTK_WIDGET(e))))
        return;
    gtk_editable_set_text(GTK_EDITABLE(e), text);
}

static GtkWidget *mono_view(GtkTextView **out, gboolean editable)
{
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *view = gtk_text_view_new();
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(view), TRUE);
    gtk_text_view_set_editable(GTK_TEXT_VIEW(view), editable);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(view), editable);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(view), 6);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), view);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_widget_set_hexpand(scroll, TRUE);
    *out = GTK_TEXT_VIEW(view);
    return scroll;
}

static GtkWidget *labeled(const char *text, GtkWidget *child)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *l = gtk_label_new(text);
    gtk_widget_add_css_class(l, "dim-label");
    gtk_box_append(GTK_BOX(box), l);
    gtk_box_append(GTK_BOX(box), child);
    return box;
}

static GtkWidget *padded(GtkWidget *w)
{
    gtk_widget_set_margin_top(w, 8);
    gtk_widget_set_margin_bottom(w, 8);
    gtk_widget_set_margin_start(w, 8);
    gtk_widget_set_margin_end(w, 8);
    return w;
}

static GtkWidget *hint_label(const char *text)
{
    GtkWidget *l = gtk_label_new(text);
    gtk_widget_add_css_class(l, "dim-label");
    gtk_label_set_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    return l;
}

static GtkWidget *title_label(const char *text)
{
    GtkWidget *l = gtk_label_new(text);
    gtk_widget_add_css_class(l, "heading");
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    return l;
}

static GtkEntry *small_entry(const char *placeholder, int chars)
{
    GtkEntry *e = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(e, placeholder);
    gtk_editable_set_width_chars(GTK_EDITABLE(e), chars);
    gtk_editable_set_max_width_chars(GTK_EDITABLE(e), chars);
    return e;
}

/* The accent fill with white text: the PC line, a marked row. */
static void accent_tag(GtkTextView *view, const char *name)
{
    char rgb[16];
    g_snprintf(rgb, sizeof rgb, "#%06x", A800SESSION_ACCENT_RGB);
    gtk_text_buffer_create_tag(gtk_text_view_get_buffer(view), name,
                               "paragraph-background", rgb, "foreground", "#ffffff", NULL);
}

static void toast(DbgWin *w, const char *msg)
{
    adw_toast_overlay_add_toast(w->toasts, adw_toast_new(msg));
}

static const char *on_off(int v) { return v ? "on" : "off"; }

/* ---- the console ---------------------------------------------------------- */

/* Onto the console's transcript, which follows its end while the reader
 * is there. */
static void console_append(DbgWin *w, const char *text)
{
    GtkTextBuffer *b = gtk_text_view_get_buffer(w->con_out);
    char *clean = clean_text(text);
    GtkTextIter it;
    int count;

    log_note_reader(w->con_out);
    gtk_text_buffer_get_end_iter(b, &it);
    gtk_text_buffer_insert(b, &it, clean, -1);
    g_free(clean);
    /* long sessions keep the last CONSOLE_KEEP characters, whole lines */
    count = gtk_text_buffer_get_char_count(b);
    if (count > CONSOLE_KEEP) {
        GtkTextIter s, e;
        gtk_text_buffer_get_start_iter(b, &s);
        gtk_text_buffer_get_iter_at_offset(b, &e, count - CONSOLE_KEEP * 3 / 4);
        gtk_text_iter_forward_line(&e);
        gtk_text_buffer_delete(b, &s, &e);
    }
}

/* Command output, ended with a newline. */
static void console_output(DbgWin *w, const char *text)
{
    size_t n = strlen(text);
    if (!n) return;
    console_append(w, text);
    if (text[n - 1] != '\n') console_append(w, "\n");
}

/* Messages from Load Symbols, Save and the snapshots land in the console
 * and, since it may not be the tab showing, in a toast. */
static void say(DbgWin *w, const char *msg)
{
    console_output(w, msg);
    toast(w, msg);
}

static void set_prompt(DbgWin *w, gboolean asm_mode, const char *prompt)
{
    w->con_asm = asm_mode;
    g_strlcpy(w->con_prompt_text, asm_mode ? prompt : "> ", sizeof w->con_prompt_text);
    gtk_label_set_text(w->con_prompt, w->con_prompt_text);
    gtk_entry_set_placeholder_text(w->con_in, asm_mode
        ? "Assembler: an instruction (LDA #$42), or an empty line to finish"
        : "Monitor command (HELP, D, M, F, A, BLINE, DLIST, LABELS, CONT ...) "
          "\xe2\x80\x94 Tab completes, Up/Down recall");
}

/* The banner and registers atari800 printed at a stop: an empty line,
 * which its monitor ignores, hands them over. Not while the assembler is
 * waiting, where an empty line would end it. */
static void console_drain(DbgWin *w)
{
    static char out[65536];
    if (w->con_asm || !a800session_is_running(w->session)) return;
    a800debug_command(w->dbg, "", out, sizeof out);
    console_output(w, out);
}

/* ---- refresh -------------------------------------------------------------- */

static void refresh_status(DbgWin *w)
{
    char reason[160], text[220];
    int addr = -1;
    gboolean running = a800session_is_running(w->session) != 0;
    gboolean stopped = running && a800debug_is_stopped(w->dbg);

    if (!running)
        g_strlcpy(text, "The Atari is not running", sizeof text);
    else if (stopped) {
        a800debug_stop_reason(w->dbg, reason, sizeof reason, &addr);
        g_snprintf(text, sizeof text, "Stopped%s%s", reason[0] ? ": " : "", reason);
    } else
        g_strlcpy(text, "Running", sizeof text);
    gtk_label_set_text(w->status, text);
    gtk_widget_set_tooltip_text(GTK_WIDGET(w->status), text);
    gtk_button_set_label(w->run_btn, stopped ? "Run (F5)" : "Stop (F5)");
    if (stopped) gtk_widget_add_css_class(GTK_WIDGET(w->run_btn), "a800-accent");
    else gtk_widget_remove_css_class(GTK_WIDGET(w->run_btn), "a800-accent");
    w->updating = TRUE;
    gtk_toggle_button_set_active(w->brk_btn, running && a800debug_breaks_on_brk(w->dbg));
    w->updating = FALSE;
}

static void refresh_cpu(DbgWin *w)
{
    a800debug_cpu c;
    char buf[160];
    int i;
    a800debug_cpu_get(w->dbg, &c);
    {
        const int vals[6] = { c.pc, c.sp, c.a, c.x, c.y, c.p };
        for (i = 0; i < 6; i++) {
            g_snprintf(buf, sizeof buf, i == 0 ? "%04X" : "%02X",
                       (unsigned)vals[i] & (i == 0 ? 0xFFFFu : 0xFFu));
            set_entry(w, w->reg[i], buf);
        }
    }
    {
        const int flags[6] = { c.n, c.v, c.d, c.i, c.z, c.c };
        w->updating = TRUE;
        for (i = 0; i < 6; i++)
            gtk_check_button_set_active(w->flag[i], flags[i] != 0);
        w->updating = FALSE;
    }
    g_snprintf(buf, sizeof buf, "scanline %d   cycle %d   frame %u", c.scanline, c.cycle, c.frame);
    gtk_label_set_text(w->beam, buf);
}

static void refresh_mem(DbgWin *w)
{
    static uint8_t mem[MEM_ROWS * 16];
    GtkTextBuffer *b = gtk_text_view_get_buffer(w->mem_view);
    GString *t = g_string_sized_new(MEM_ROWS * 80);
    int row, col;

    a800debug_read(w->dbg, (uint16_t)w->mem_top, mem, (int)sizeof mem);
    g_string_append(t, "       ");
    for (col = 0; col < 16; col++)
        g_string_append_printf(t, col == 8 ? " %X  " : "%X  ", col);
    g_string_append_c(t, '\n');
    for (row = 0; row < MEM_ROWS; row++) {
        const uint8_t *r = mem + row * 16;
        g_string_append_printf(t, "$%04X: ", (w->mem_top + row * 16) & 0xFFFF);
        for (col = 0; col < 16; col++)
            g_string_append_printf(t, col == 8 ? " %02X " : "%02X ", r[col]);
        g_string_append_c(t, ' ');
        for (col = 0; col < 16; col++)
            g_string_append_c(t, (char)(r[col] >= 0x20 && r[col] < 0x7F ? r[col] : '.'));
        g_string_append_c(t, '\n');
    }
    set_text(w->mem_view, t->str);
    g_string_free(t, TRUE);

    /* the row Go to found stays marked across refreshes */
    if (w->mem_mark >= 0) {
        int off = (w->mem_mark - w->mem_top) & 0xFFFF;
        if (off < MEM_ROWS * 16) {
            GtkTextIter s, e;
            gtk_text_buffer_get_iter_at_line(b, &s, 1 + off / 16);
            gtk_text_buffer_get_iter_at_line(b, &e, 2 + off / 16);
            gtk_text_buffer_apply_tag_by_name(b, "mark", &s, &e);
        }
    }
}

static void refresh_history(DbgWin *w)
{
    static a800debug_history h[HISTORY_MAX];
    GString *t = g_string_new("PC     line cyc  instruction");
    int n = a800debug_history_get(w->dbg, h, HISTORY_MAX), i;
    for (i = 0; i < n; i++)
        g_string_append_printf(t, "\n%04X   %3d  %3d  %s", h[i].pc, h[i].scanline, h[i].cycle,
                               h[i].disasm);
    set_text_at(w->hist_view, t->str, TRUE);
    g_string_free(t, TRUE);
}

/* The disassembly lines that fit the view. */
static int visible_rows(DbgWin *w)
{
    GtkTextIter it;
    int y = 0, h = 0, height = gtk_widget_get_height(w->disasm_scroll);
    gtk_text_buffer_get_start_iter(gtk_text_view_get_buffer(w->disasm), &it);
    gtk_text_view_get_line_yrange(w->disasm, &it, &y, &h);
    if (h <= 0 || height <= 0) return 32;
    return CLAMP(height / h, 4, DISASM_WINDOW);
}

static void refresh_disasm(DbgWin *w)
{
    static a800debug_line lines[DISASM_WINDOW];
    int bp[DISASM_WINDOW];
    GtkTextBuffer *b = gtk_text_view_get_buffer(w->disasm);
    GString *t = g_string_sized_new(DISASM_WINDOW * 100);
    GtkTextIter s, e;
    int n, pc_line = -1, i, rows = visible_rows(w);
    char *clean;

    /* Following, the PC stays put while it shows, else sits a third of the
     * way down. */
    if (gtk_check_button_get_active(w->follow_pc)) {
        a800debug_cpu c;
        gboolean shows = FALSE;
        a800debug_cpu_get(w->dbg, &c);
        for (i = 1; i < w->line_count && i < rows - 2; i++)
            if (w->line_addr[i] == c.pc) shows = TRUE;
        if (!shows)
            w->disasm_top = a800debug_row_address(w->dbg, (uint16_t)c.pc, -(rows / 3));
    }

    n = a800debug_disassemble(w->dbg, (uint16_t)w->disasm_top, lines, DISASM_WINDOW, &pc_line);
    w->line_count = n;
    for (i = 0; i < n; i++) {
        const a800debug_line *l = &lines[i];
        bp[i] = l->has_breakpoint;   /* 1 on, 2 off */
        w->line_addr[i] = l->address;
        g_string_append_printf(t, "%c%c %04X  %-8s  %-10s %-16s%s%s\n",
                               bp[i] == 2 ? 'o' : bp[i] ? '*' : ' ', l->is_pc ? '>' : ' ',
                               l->address, l->bytes, l->label, l->disasm,
                               l->comment[0] ? " ; " : "", l->comment);
    }
    if (n == 0)
        g_string_append(t, "(no disassembly)\n");
    clean = clean_text(t->str);
    gtk_text_buffer_set_text(b, clean, -1);
    g_free(clean);
    g_string_free(t, TRUE);

    for (i = 0; i < n; i++) {
        /* the gutter: a breakpoint red, a disabled one grey */
        if (bp[i]) {
            gtk_text_buffer_get_iter_at_line(b, &s, i);
            e = s;
            gtk_text_iter_forward_char(&e);
            gtk_text_buffer_apply_tag_by_name(b, bp[i] == 2 ? "bpoff" : "bp", &s, &e);
        }
        /* the selected line underlined */
        if (lines[i].address == w->selected) {
            gtk_text_buffer_get_iter_at_line(b, &s, i);
            gtk_text_buffer_get_iter_at_line(b, &e, i + 1);
            gtk_text_buffer_apply_tag_by_name(b, "sel", &s, &e);
        }
    }
    /* the PC line in the accent colour */
    if (pc_line >= 0 && pc_line < n) {
        gtk_text_buffer_get_iter_at_line(b, &s, pc_line);
        gtk_text_buffer_get_iter_at_line(b, &e, pc_line + 1);
        gtk_text_buffer_apply_tag_by_name(b, "pc", &s, &e);
    }
    /* the wheel scrolls by instructions: the view itself stays at its top */
    gtk_adjustment_set_value(
        gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(w->disasm_scroll)), 0.0);
}

static void refresh_antic(DbgWin *w)
{
    static const char *const widths[4] = {
        "none", "narrow (32 bytes a line)", "normal (40 bytes a line)", "wide (48 bytes a line)" };
    static char dl[32768];
    a800debug_antic a;
    GString *t = g_string_new(NULL);
    int pm;

    a800debug_antic_get(w->dbg, &a);
    pm = a.pm_single_line ? (a.pmbase & 0xF8) << 8 : (a.pmbase & 0xFC) << 8;
    g_string_append_printf(t,
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
    set_text(w->antic_text, t->str);
    g_string_free(t, TRUE);

    if (a800debug_dlist_text(w->dbg, dl, sizeof dl) <= 0)
        g_strlcpy(dl, "(no display list)", sizeof dl);
    else if (!a.dl_dma) {
        t = g_string_new("(display list DMA is off: ANTIC is not reading this)\n\n");
        g_string_append(t, dl);
        g_strlcpy(dl, t->str, sizeof dl);
        g_string_free(t, TRUE);
    }
    set_text(w->dlist_text, dl);
}

/* A GRAF byte's bits, left to right. */
static void graf_bits(GString *t, unsigned v, int bits)
{
    int b;
    for (b = bits - 1; b >= 0; b--)
        g_string_append_c(t, (v >> b) & 1 ? '#' : '.');
}

static void collision_row(GString *t, const char *name, uint8_t pf, uint8_t pl, int self)
{
    int j;
    g_string_append_printf(t, "  %s        ", name);
    for (j = 0; j < 4; j++)
        g_string_append_printf(t, " %c  ", (pf >> j) & 1 ? 'X' : '.');
    g_string_append(t, "   ");
    for (j = 0; j < 4; j++)
        g_string_append_printf(t, "%c  ", j == self ? '-' : (pl >> j) & 1 ? 'X' : '.');
    g_string_append_c(t, '\n');
}

static void refresh_gtia(DbgWin *w)
{
    static const char *const sizes[4] = { "normal", "double", "normal", "quad" };
    static const char *const modes[4] = {
        "normal (ANTIC's colours)", "9 (16 lumas of one hue)", "10 (9 colours)",
        "11 (16 hues)" };
    static const char *const names[SWATCHES] = {
        "COLPM0", "COLPM1", "COLPM2", "COLPM3", "COLPF0", "COLPF1", "COLPF2", "COLPF3", "COLBK" };
    a800debug_gtia g;
    GString *t = g_string_new(NULL);
    const char *prio;
    int i;

    a800debug_gtia_get(w->dbg, &g);
    g_string_append(t, "Player  HPOS  SIZE    GRAF       COLPM\n");
    for (i = 0; i < 4; i++) {
        g_string_append_printf(t, "P%d      $%02X   %-7s ", i, g.hposp[i], sizes[g.sizep[i] & 3]);
        graf_bits(t, g.grafp[i], 8);
        g_string_append_printf(t, "   $%02X\n", g.colpm[i]);
    }
    g_string_append(t, "\nMissile HPOS  SIZE    GRAF\n");
    for (i = 0; i < 4; i++) {
        g_string_append_printf(t, "M%d      $%02X   %-7s ", i, g.hposm[i],
                               sizes[(g.sizem >> (i * 2)) & 3]);
        graf_bits(t, (g.grafm >> (i * 2)) & 3, 2);
        g_string_append_c(t, '\n');
    }
    switch (g.prior & 0x0F) {
    case 1: prio = "players > playfield"; break;
    case 2: prio = "P0-1 > playfield > P2-3"; break;
    case 4: prio = "playfield > players"; break;
    case 8: prio = "PF0-1 > players > PF2-3"; break;
    case 0: prio = "none set (overlaps mix)"; break;
    default: prio = "several set (overlaps mix)"; break;
    }
    g_string_append_printf(t,
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
    g_string_append(t, "Collisions  PF0 PF1 PF2 PF3    P0 P1 P2 P3\n");
    for (i = 0; i < 4; i++) {
        char n[4] = { 'M', (char)('0' + i), 0, 0 };
        collision_row(t, n, g.mpf[i], g.mpl[i], -1);
    }
    for (i = 0; i < 4; i++) {
        char n[4] = { 'P', (char)('0' + i), 0, 0 };
        collision_row(t, n, g.ppf[i], g.ppl[i], i);
    }
    set_text(w->gtia_text, t->str);
    g_string_free(t, TRUE);

    /* the colour registers through the palette */
    {
        const uint8_t vals[SWATCHES] = { g.colpm[0], g.colpm[1], g.colpm[2], g.colpm[3],
                                         g.colpf[0], g.colpf[1], g.colpf[2], g.colpf[3],
                                         g.colbk };
        for (i = 0; i < SWATCHES; i++) {
            char text[32];
            guint32 rgb = a800debug_color(w->dbg, vals[i]);
            g_snprintf(text, sizeof text, "%s $%02X", names[i], vals[i]);
            gtk_label_set_text(w->swatch_label[i], text);
            if (rgb != w->swatch_rgb[i]) {
                w->swatch_rgb[i] = rgb;
                gtk_widget_queue_draw(w->swatch[i]);
            }
        }
    }
    w->palette_ok = a800debug_palette_image(w->dbg, w->palette_px) != 0;
    gtk_widget_queue_draw(w->palette);
}

/* The IRQ bits by name, the set ones (or, active_low, the clear ones). */
static void irq_words(GString *t, uint8_t v, gboolean active_low)
{
    static const char *const names[8] = {
        "timer 1", "timer 2", "timer 4", "serial output done", "serial output needed",
        "serial input ready", "key", "BREAK key" };
    gboolean any = FALSE;
    int b;
    for (b = 7; b >= 0; b--)
        if (((v >> b) & 1) != active_low) {
            g_string_append_printf(t, "%s%s", any ? ", " : " ", names[b]);
            any = TRUE;
        }
    if (!any) g_string_append(t, " none");
}

static void pokey_text(GString *t, const a800debug_pokey *p, const char *title)
{
    static const char *const dist[8] = {
        "5-bit + 17-bit poly", "5-bit poly", "5-bit + 4-bit poly", "5-bit poly",
        "17-bit poly", "pure tone", "4-bit poly", "pure tone" };
    static const char *const audctl[8] = {
        "15 kHz clock", "high-pass 2 by 4", "high-pass 1 by 3", "4 joined to 3 (16-bit)",
        "2 joined to 1 (16-bit)", "3 at 1.79 MHz", "1 at 1.79 MHz", "9-bit poly" };
    int ch, b;
    gboolean any = FALSE;

    g_string_append_printf(t, "%s\n  Channel  AUDF  AUDC  sound                  volume\n", title);
    for (ch = 0; ch < 4; ch++) {
        int c = p->audc[ch], d = c >> 5;
        char sound[32];
        g_strlcpy(sound, dist[d], sizeof sound);
        if ((p->audctl & 0x80) && (d == 0 || d == 4)) {
            /* AUDCTL bit 7 swaps the 17-bit poly for the 9-bit one */
            char *at = strstr(sound, "17-bit");
            if (at) {
                at[0] = '9';
                memmove(at + 1, at + 2, strlen(at + 2) + 1);
            }
        }
        g_string_append_printf(t, "  %d        $%02X   $%02X   %-22s %2d%s\n", ch + 1,
                               p->audf[ch], c, sound, c & 15, (c & 0x10) ? " (volume only)" : "");
    }
    g_string_append_printf(t, "  AUDCTL   $%02X  ", p->audctl);
    for (b = 0; b < 8; b++)
        if (p->audctl & (1 << b)) {
            g_string_append_printf(t, "%s%s", any ? ", " : " ", audctl[b]);
            any = TRUE;
        }
    g_string_append(t, any ? "\n" : " 64 kHz clock, nothing joined\n");
}

/* A joystick's lines (a PIA nibble, active low). */
static void stick_words(GString *t, int nibble)
{
    static const char *const dirs[4] = { "up", "down", "left", "right" };
    gboolean any = FALSE;
    int b;
    for (b = 0; b < 4; b++)
        if (!((nibble >> b) & 1)) {
            g_string_append_printf(t, "%s%s", any ? " " : "", dirs[b]);
            any = TRUE;
        }
    if (!any) g_string_append(t, "centred");
}

static void refresh_pokey(DbgWin *w)
{
    a800debug_pokey p;
    a800debug_pia pia;
    a800debug_map map;
    GString *t = g_string_new(NULL);
    gboolean stereo = a800session_get_int(w->session, "stereo", 0) != 0;
    int i;

    a800debug_pokey_get(w->dbg, 0, &p);
    pokey_text(t, &p, stereo ? "POKEY 1 ($D200, left)" : "POKEY ($D200)");
    if (stereo) {
        a800debug_pokey p2;
        a800debug_pokey_get(w->dbg, 1, &p2);
        g_string_append_c(t, '\n');
        pokey_text(t, &p2, "POKEY 2 ($D210, right)");
    }
    g_string_append_printf(t,
        "\nKBCODE  $%02X\n"
        "SKCTL   $%02X   keyboard scan %s, debounce %s, fast pot scan %s, two-tone %s, "
        "serial mode %d%s\n"
        "SKSTAT  $%02X  %s%s%s\n"
        "IRQEN   $%02X   enabled:",
        p.kbcode, p.skctl, on_off(p.skctl & 2), on_off(p.skctl & 1), on_off(p.skctl & 4),
        on_off(p.skctl & 8), (p.skctl >> 4) & 7, (p.skctl & 0x80) ? ", forcing a break" : "",
        p.skstat, (p.skstat & 8) ? "" : " SHIFT held", (p.skstat & 4) ? "" : " a key held",
        (p.skstat & 0x0C) == 0x0C ? " no key held" : "", p.irqen);
    irq_words(t, p.irqen, FALSE);
    g_string_append_printf(t, "\nIRQST   $%02X   pending:", p.irqst);
    irq_words(t, p.irqst, TRUE);
    g_string_append_printf(t, "\nRANDOM  $%02X\nPOT0-7 ", p.random);
    for (i = 0; i < 8; i++)
        g_string_append_printf(t, " $%02X", p.pot[i]);

    /* the PIA: what the ports read is the joysticks' lines (and on XL/XE,
     * port B is the memory control) */
    a800debug_pia_get(w->dbg, &pia);
    a800debug_map_get(w->dbg, &map);
    g_string_append_printf(t,
        "\n\nPIA\nPORTA   out $%02X   DDR $%02X (1 = output)",
        pia.porta, (uint8_t)~pia.porta_mask);
    if (pia.pactl & 4) {
        uint8_t in = 0;
        a800debug_read(w->dbg, 0xD300, &in, 1);
        g_string_append_printf(t, "   reads $%02X: stick 1 ", in);
        stick_words(t, in & 15);
        g_string_append(t, ", stick 2 ");
        stick_words(t, in >> 4);
    }
    g_string_append_printf(t,
        "\nPACTL   $%02X   port %s, cassette motor (CA2) %s, CA1 interrupt %s\n"
        "PORTB   out $%02X   DDR $%02X",
        pia.pactl, (pia.pactl & 4) ? "data" : "direction", pia.ca2 ? "off" : "on",
        on_off(pia.pactl & 1), pia.portb, (uint8_t)~pia.portb_mask);
    if (map.xl)
        g_string_append(t, "   (memory control: see the Memory Map)");
    else if (pia.pbctl & 4) {
        uint8_t in = 0;
        a800debug_read(w->dbg, 0xD301, &in, 1);
        g_string_append_printf(t, "   reads $%02X: stick 3 ", in);
        stick_words(t, in & 15);
        g_string_append(t, ", stick 4 ");
        stick_words(t, in >> 4);
    }
    g_string_append_printf(t,
        "\nPBCTL   $%02X   port %s, SIO command (CB2) %s, CB1 interrupt %s\n"
        "Lines   CA1 %d  CA2 %d  CB1 %d  CB2 %d   IRQ %s\n",
        pia.pbctl, (pia.pbctl & 4) ? "data" : "direction", pia.cb2 ? "idle" : "asserted",
        on_off(pia.pbctl & 1), pia.ca1, pia.ca2, pia.cb1, pia.cb2,
        pia.irq ? "asserted" : "-");
    set_text(w->pokey_text, t->str);
    g_string_free(t, TRUE);
}

static void refresh_map(DbgWin *w)
{
    a800debug_map m;
    GString *t = g_string_new(NULL);
    int kb, b;

    a800debug_map_get(w->dbg, &m);
    kb = m.ram_kb == 321 ? 320 : m.ram_kb;     /* atari800's Compy Shop 320K */
    g_string_append_printf(t, "Machine   %s, %dK RAM%s\n\n", m.machine, kb,
                           m.xl ? ", XL/XE banking" : "");
    if (m.xl) {
        g_string_append_printf(t, "PORTB     $%02X = ", m.portb);
        for (b = 7; b >= 0; b--)
            g_string_append_printf(t, "%d%s", (m.portb >> b) & 1, b == 4 ? " " : "");
        g_string_append_printf(t,
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

    g_string_append(t, "What the CPU sees\n");
    if (m.xl) {
        g_string_append(t, "  $0000-$3FFF  RAM\n");
        if (m.xe_cpu_bank >= 0)
            g_string_append_printf(t, "  $4000-$7FFF  extended RAM bank %d\n", m.xe_cpu_bank);
        else
            g_string_append(t, "  $4000-$7FFF  RAM\n");
        if (m.xe_antic_bank >= 0)
            g_string_append_printf(t, "               (ANTIC reads extended bank %d here)\n",
                                   m.xe_antic_bank);
        if (m.selftest)
            g_string_append(t, "  $5000-$57FF  self-test ROM\n");
        g_string_append_printf(t, "  $8000-$9FFF  %s\n", m.cart_type ? "RAM, or the cartridge" : "RAM");
        g_string_append_printf(t, "  $A000-$BFFF  %s\n",
                               m.basic_rom ? "BASIC ROM" : m.cart_type ? "RAM, or the cartridge" : "RAM");
        g_string_append_printf(t, "  $C000-$CFFF  %s\n", m.os_rom ? "OS ROM" : "RAM");
        g_string_append(t, "  $D000-$D7FF  hardware: GTIA $D000, POKEY $D200, PIA $D300, ANTIC $D400\n");
        g_string_append_printf(t, "  $D800-$FFFF  %s\n", m.os_rom ? "OS ROM (floating point at $D800)" : "RAM");
    } else {
        int top = (kb > 48 ? 48 : kb) * 1024 - 1;
        if (top > 0)
            g_string_append_printf(t, "  $0000-$%04X  RAM\n", top);
        if (m.cart_type)
            g_string_append(t, "  $8000-$BFFF  the cartridge, where its type maps it\n");
        if (m.basic_rom)
            g_string_append(t, "  $A000-$BFFF  BASIC ROM\n");
        g_string_append_printf(t, "  $C000-$CFFF  %s\n", kb > 48 ? "RAM" : "(nothing)");
        g_string_append(t, "  $D000-$D7FF  hardware: GTIA $D000, POKEY $D200, PIA $D300, ANTIC $D400\n");
        g_string_append(t, "  $D800-$FFFF  OS ROM (floating point at $D800)\n");
    }

    if (m.cart_type)
        g_string_append_printf(t, "\nCartridge   type %d: %s, bank %d\n            %s\n",
                               m.cart_type, m.cart_desc, m.cart_bank, m.cart_name);
    else
        g_string_append(t, "\nCartridge   none\n");
    if (m.piggyback_type)
        g_string_append_printf(t, "Piggyback   type %d\n            %s\n", m.piggyback_type,
                               m.piggyback_name);
    set_text(w->map_text, t->str);
    g_string_free(t, TRUE);
}

static void bp_enable_toggled(GtkCheckButton *b, gpointer ud);
static void bp_remove_clicked(GtkButton *b, gpointer ud);

static const char *bp_type_name(int type)
{
    if (type & A800DEBUG_BP_EXEC) return "Execute";
    if ((type & (A800DEBUG_BP_READ | A800DEBUG_BP_WRITE)) == (A800DEBUG_BP_READ | A800DEBUG_BP_WRITE))
        return "Read/Write";
    return (type & A800DEBUG_BP_READ) ? "Read" : "Write";
}

static void refresh_bps(DbgWin *w)
{
    static a800debug_breakpoint bps[BP_MAX];
    GString *sig = g_string_new(NULL);
    GtkWidget *child;
    char text[160];
    int n = a800debug_breakpoint_list(w->dbg, bps, BP_MAX), i, max = 0;
    int used = a800debug_breakpoint_budget(w->dbg, &max);

    g_snprintf(text, sizeof text, "atari800's condition table: %d of %d entries used", used, max);
    gtk_label_set_text(w->bp_budget, text);

    /* rebuilt only when it changed (each step refreshes) */
    for (i = 0; i < n; i++)
        g_string_append_printf(sig, "%d %d %d %d %d %s\n", bps[i].id, bps[i].type, bps[i].start,
                               bps[i].end, bps[i].enabled, bps[i].condition);
    if (w->bp_sig && strcmp(w->bp_sig, sig->str) == 0) {
        g_string_free(sig, TRUE);
        return;
    }
    g_free(w->bp_sig);
    w->bp_sig = g_string_free(sig, FALSE);

    while ((child = gtk_widget_get_first_child(GTK_WIDGET(w->bp_list))) != NULL)
        gtk_list_box_remove(w->bp_list, child);
    if (n == 0) {
        GtkWidget *l = gtk_label_new("No breakpoints or watchpoints. Click the left edge of a "
                                     "disassembly line, or add one below.");
        gtk_widget_add_css_class(l, "dim-label");
        gtk_widget_set_margin_top(l, 12);
        gtk_widget_set_margin_bottom(l, 12);
        gtk_list_box_append(w->bp_list, l);
        return;
    }
    for (i = 0; i < n; i++) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *en = gtk_check_button_new();
        GtkWidget *rm = gtk_button_new_from_icon_name("user-trash-symbolic");
        GtkWidget *l;
        char line[320], range[32];
        if (bps[i].end != bps[i].start)
            g_snprintf(range, sizeof range, "$%04X-$%04X", bps[i].start, bps[i].end);
        else
            g_snprintf(range, sizeof range, "$%04X", bps[i].start);
        g_snprintf(line, sizeof line, "#%-5d %-10s %-12s %s%s", bps[i].id,
                   bp_type_name(bps[i].type), range,
                   bps[i].condition[0] ? "if " : "", bps[i].condition);
        l = gtk_label_new(line);
        gtk_label_set_xalign(GTK_LABEL(l), 0);
        gtk_widget_set_hexpand(l, TRUE);
        gtk_widget_add_css_class(l, "monospace");
        gtk_check_button_set_active(GTK_CHECK_BUTTON(en), bps[i].enabled != 0);
        gtk_widget_set_tooltip_text(en, "Enabled");
        g_object_set_data(G_OBJECT(en), "bp", GINT_TO_POINTER(bps[i].id));
        g_object_set_data(G_OBJECT(rm), "bp", GINT_TO_POINTER(bps[i].id));
        g_signal_connect(en, "toggled", G_CALLBACK(bp_enable_toggled), w);
        g_signal_connect(rm, "clicked", G_CALLBACK(bp_remove_clicked), w);
        gtk_widget_add_css_class(rm, "flat");
        gtk_widget_set_tooltip_text(rm, "Remove");
        gtk_box_append(GTK_BOX(row), en);
        gtk_box_append(GTK_BOX(row), l);
        gtk_box_append(GTK_BOX(row), rm);
        gtk_list_box_append(w->bp_list, row);
    }
}

static void refresh_sio(DbgWin *w, gboolean force)
{
    static a800debug_sio e[SIO_MAX];
    GString *t;
    gboolean fuji = a800session_fujinet_running(w->session) != 0;
    int n = a800debug_sio_trace(w->dbg, e, SIO_MAX), i;

    gtk_label_set_text(w->sio_note, fuji
        ? "FujiNet is running: the Atari's commands go to it over NetSIO."
        : "FujiNet is off: atari800's own drives answer (\"patch\" is its fast SIO patch).");
    /* a log: set again only when it moved */
    if (!force && n == w->sio_count &&
        (n == 0 || memcmp(&e[n - 1], &w->sio_last, sizeof w->sio_last) == 0))
        return;
    w->sio_count = n;
    if (n > 0) w->sio_last = e[n - 1];
    log_note_reader(w->sio_text);
    t = g_string_new("  Frame  Path    Dev  Cmd  Aux1 Aux2   Command");
    for (i = 0; i < n; i++)
        g_string_append_printf(t, "\n%7u  %-6s  $%02X  $%02X  $%02X  $%02X   %s", e[i].frame,
                               e[i].path, e[i].device, e[i].command, e[i].aux1, e[i].aux2,
                               e[i].text);
    if (n == 0)
        g_string_append(t, "\n(no SIO commands since the trace was cleared)");
    set_text_at(w->sio_text, t->str, force || following(w->sio_text));
    g_string_free(t, TRUE);
}

static void refresh_fujinet(DbgWin *w, gboolean force)
{
    static a800_fuji_packet p[FUJI_MAX];
    static char state[2048];
    GString *t;
    int n = a800debug_fujinet_trace(w->dbg, p, FUJI_MAX), i;

    if (!a800session_fujinet_running(w->session))
        gtk_label_set_text(w->fuji_note, "FujiNet is off (Preferences \xe2\x96\xb8 FujiNet).");
    else
        gtk_label_set_text(w->fuji_note, a800session_fujinet_link(w->session)
            ? "FujiNet is on NetSIO, connected." : "FujiNet is on NetSIO, not connected.");
    a800debug_fujinet_state(w->dbg, state, sizeof state);
    set_text(w->fuji_state, state);

    /* a log: set again only when it moved */
    if (!force && n == w->fuji_count && (n == 0 || p[n - 1].serial == w->fuji_last))
        return;
    w->fuji_count = n;
    w->fuji_last = n > 0 ? p[n - 1].serial : 0;
    log_note_reader(w->fuji_text);
    t = g_string_new(NULL);
    for (i = 0; i < n; i++) {
        int k;
        g_string_append_printf(t, "%s%s\n     ", i ? "\n" : "", p[i].text);
        for (k = 0; k < p[i].nbytes && k < (int)sizeof p[i].data; k++)
            g_string_append_printf(t, "%02X ", p[i].data[k]);
        if (p[i].nbytes > (int)sizeof p[i].data) g_string_append(t, "\xe2\x80\xa6");
    }
    if (n == 0)
        g_string_append(t, "(no SIO packets since the trace was cleared)");
    set_text_at(w->fuji_text, t->str, force || following(w->fuji_text));
    g_string_free(t, TRUE);
}

static void refresh_snapshots(DbgWin *w)
{
    gboolean fuji = a800session_fujinet_running(w->session) != 0;
    int i;
    for (i = 0; i < SNAPSHOTS; i++) {
        gboolean has = a800debug_snapshot_exists(w->dbg, i) != 0;
        gtk_widget_set_sensitive(w->snap_restore[i], has);
        gtk_label_set_text(w->snap_state[i], has ? "saved" : "empty");
        gtk_widget_set_tooltip_text(w->snap_restore[i], fuji ? k_fujinet_note : NULL);
    }
    gtk_widget_set_visible(w->snap_note, fuji);
}

static void refresh_page(DbgWin *w, int page)
{
    if (!a800session_is_running(w->session)) return;
    switch (page) {
    case TAB_CPU: refresh_cpu(w); refresh_mem(w); refresh_history(w); break;
    case TAB_DISASM: refresh_disasm(w); break;
    case TAB_ANTIC: refresh_antic(w); break;
    case TAB_GTIA: refresh_gtia(w); break;
    case TAB_POKEY: refresh_pokey(w); break;
    case TAB_MAP: refresh_map(w); break;
    case TAB_BREAKS: refresh_bps(w); break;
    case TAB_SIO: refresh_sio(w, FALSE); break;
    case TAB_FUJINET: refresh_fujinet(w, FALSE); break;
    default: break;
    }
}

static void refresh_all(DbgWin *w)
{
    refresh_status(w);
    refresh_page(w, gtk_notebook_get_current_page(w->notebook));
}

/* After anything that may have moved the engine: the console takes what
 * atari800 printed at a stop, then the views follow. The drain moves the
 * generation itself, so it is read after. */
static void resync(DbgWin *w)
{
    w->was_stopped = a800debug_is_stopped(w->dbg) != 0;
    if (w->was_stopped) console_drain(w);
    w->seen_generation = a800debug_generation(w->dbg);
    w->running_ticks = 0;
    refresh_all(w);
}

/* A step takes microseconds: a moment's wait for the stop keeps the window
 * from flashing "Running" in between. */
static void settle_and_sync(DbgWin *w)
{
    int i;
    for (i = 0; i < 40 && !a800debug_is_stopped(w->dbg); i++)
        g_usleep(1000);
    resync(w);
}

static void attach_and_refresh(DbgWin *w)
{
    a800debug_attach(w->dbg);     /* stops the machine */
    w->attached_running = a800session_is_running(w->session) != 0;
    settle_and_sync(w);
}

static gboolean tick_refresh(gpointer ud)
{
    DbgWin *w = ud;
    unsigned gen;
    gboolean stopped;
    if (!gtk_widget_get_visible(GTK_WIDGET(w->win))) return G_SOURCE_CONTINUE;
    if (!a800session_is_running(w->session)) {
        if (w->attached_running) {
            w->attached_running = FALSE;
            refresh_status(w);
        }
        return G_SOURCE_CONTINUE;
    }
    /* Opened before the session started, or showing across a restart: the
     * engine attaches (stopping the machine) as it comes up. */
    if (!w->attached_running) {
        a800debug_detach(w->dbg);
        attach_and_refresh(w);
        return G_SOURCE_CONTINUE;
    }
    gen = a800debug_generation(w->dbg);
    stopped = a800debug_is_stopped(w->dbg) != 0;
    if (gen != w->seen_generation || stopped != w->was_stopped) {
        resync(w);
    } else if (!stopped && ++w->running_ticks >= 5) {
        /* live values twice a second while the machine runs */
        int page = gtk_notebook_get_current_page(w->notebook);
        w->running_ticks = 0;
        refresh_status(w);
        if (page == TAB_CPU) refresh_cpu(w);
        if (page == TAB_SIO) refresh_sio(w, FALSE);
        if (page == TAB_FUJINET) refresh_fujinet(w, FALSE);
    }
    return G_SOURCE_CONTINUE;
}

/* ---- handlers ------------------------------------------------------------- */

static void on_run(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    (void)b;
    if (a800debug_is_stopped(w->dbg)) a800debug_resume(w->dbg);
    else a800debug_stop(w->dbg);
    resync(w);
}

static void on_step(GtkButton *b, gpointer ud) { (void)b; a800debug_step(((DbgWin *)ud)->dbg); settle_and_sync(ud); }
static void on_step_over(GtkButton *b, gpointer ud) { (void)b; a800debug_step_over(((DbgWin *)ud)->dbg); settle_and_sync(ud); }
static void on_step_out(GtkButton *b, gpointer ud) { (void)b; a800debug_step_out(((DbgWin *)ud)->dbg); settle_and_sync(ud); }
static void on_frame(GtkButton *b, gpointer ud) { (void)b; a800debug_frame(((DbgWin *)ud)->dbg); settle_and_sync(ud); }

static void on_run_to(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    (void)b;
    if (w->selected < 0) {
        toast(w, "Select a line in the Disassembly to run to");
        return;
    }
    a800debug_run_to(w->dbg, (uint16_t)w->selected);
    settle_and_sync(w);
}

static void on_brk_toggled(GtkToggleButton *b, gpointer ud)
{
    DbgWin *w = ud;
    if (w->updating) return;
    a800debug_break_on_brk(w->dbg, gtk_toggle_button_get_active(b));
    resync(w);
}

static void toggle_breakpoint(DbgWin *w, uint16_t addr)
{
    if (a800debug_breakpoint_toggle(w->dbg, addr) < 0)
        toast(w, "atari800's condition table is full: remove a breakpoint first");
    resync(w);
}

static void on_snap_save(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    int slot = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "slot"));
    char msg[80];
    if (a800debug_snapshot_save(w->dbg, slot) == 0)
        g_snprintf(msg, sizeof msg, "Snapshot %d saved", slot);
    else
        g_snprintf(msg, sizeof msg, "Could not save snapshot %d", slot);
    toast(w, msg);
    refresh_snapshots(w);
}

static void on_snap_restore(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    int slot = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "slot"));
    GtkWidget *pop = gtk_widget_get_ancestor(GTK_WIDGET(b), GTK_TYPE_POPOVER);
    char msg[80];
    if (pop) gtk_popover_popdown(GTK_POPOVER(pop));
    if (a800debug_snapshot_restore(w->dbg, slot) == 0)
        g_snprintf(msg, sizeof msg, "Snapshot %d restored", slot);
    else
        g_snprintf(msg, sizeof msg, "Could not restore snapshot %d", slot);
    toast(w, msg);
    resync(w);
}

static void on_snap_show(GtkWidget *pop, gpointer ud)
{
    (void)pop;
    refresh_snapshots(ud);
}

static void on_console(GtkEntry *entry, gpointer ud)
{
    DbgWin *w = ud;
    static char out[65536];
    char *cmd = g_strdup(gtk_editable_get_text(GTK_EDITABLE(entry)));
    char line[600];
    int rc;

    /* an empty line only means something to the assembler (its end) */
    if (!w->con_asm && !*g_strstrip(cmd)) {
        g_free(cmd);
        return;
    }
    if (*cmd && (w->history->len == 0 ||
                 strcmp(g_ptr_array_index(w->history, w->history->len - 1), cmd) != 0))
        g_ptr_array_add(w->history, g_strdup(cmd));
    w->history_pos = w->history->len;
    g_snprintf(line, sizeof line, "%s%s\n", w->con_prompt_text, cmd);
    log_to_end(w->con_out);     /* a command brings the reader back down */
    console_append(w, line);
    gtk_editable_set_text(GTK_EDITABLE(entry), "");

    rc = a800debug_command(w->dbg, cmd, out, sizeof out);
    g_free(cmd);
    if (rc == A800DBG_CMD_NEEDS_INPUT) {
        /* the output ends with the assembler's own prompt ("0642: ") */
        char *nl = strrchr(out, '\n');
        char *prompt = nl ? nl + 1 : out;
        char saved[32];
        g_strlcpy(saved, prompt, sizeof saved);
        *prompt = '\0';
        console_output(w, out);
        set_prompt(w, TRUE, saved);
    } else {
        console_output(w, out);
        set_prompt(w, FALSE, NULL);
    }
    if (rc == A800DBG_CMD_RESUMED) settle_and_sync(w);
    else resync(w);
}

static void history_show(DbgWin *w)
{
    const char *text = w->history_pos < w->history->len
                           ? g_ptr_array_index(w->history, w->history_pos) : "";
    gtk_editable_set_text(GTK_EDITABLE(w->con_in), text);
    gtk_editable_set_position(GTK_EDITABLE(w->con_in), -1);
}

/* Tab completes the command word against atari800's commands (the longest
 * common start of several, which are listed); Up and Down walk the
 * history. */
static gboolean on_console_key(GtkEventControllerKey *c, guint keyval, guint code,
                               GdkModifierType st, gpointer ud)
{
    DbgWin *w = ud;
    char comps[2048];
    const char *text;
    char **words;
    guint n, i;
    (void)c; (void)code; (void)st;
    if (keyval == GDK_KEY_Up || keyval == GDK_KEY_KP_Up) {
        if (w->history_pos > 0) {
            w->history_pos--;
            history_show(w);
        }
        return TRUE;
    }
    if (keyval == GDK_KEY_Down || keyval == GDK_KEY_KP_Down) {
        if (w->history_pos < w->history->len) {
            w->history_pos++;
            history_show(w);
        }
        return TRUE;
    }
    if (keyval != GDK_KEY_Tab && keyval != GDK_KEY_ISO_Left_Tab) return FALSE;
    text = gtk_editable_get_text(GTK_EDITABLE(w->con_in));
    if (w->con_asm || strchr(text, ' ')) {
        gtk_widget_error_bell(GTK_WIDGET(w->con_in));
        return TRUE;
    }
    a800debug_completions(w->dbg, text, comps, sizeof comps);
    words = g_strsplit(comps, " ", -1);
    n = 0;
    for (i = 0; words[i]; i++)
        if (*words[i]) words[n++] = words[i];
        else g_free(words[i]);
    words[n] = NULL;
    if (n == 1) {
        char merged[64];
        g_snprintf(merged, sizeof merged, "%s ", words[0]);
        gtk_editable_set_text(GTK_EDITABLE(w->con_in), merged);
        gtk_editable_set_position(GTK_EDITABLE(w->con_in), -1);
    } else if (n > 1) {
        size_t len = strlen(words[0]);
        char *list;
        for (i = 1; i < n; i++) {
            size_t k = 0;
            while (k < len && words[i][k] == words[0][k]) k++;
            len = k;
        }
        if (len > strlen(text)) {
            char *pre = g_strndup(words[0], len);
            gtk_editable_set_text(GTK_EDITABLE(w->con_in), pre);
            gtk_editable_set_position(GTK_EDITABLE(w->con_in), -1);
            g_free(pre);
        }
        list = g_strjoinv("  ", words);
        console_output(w, list);
        g_free(list);
    } else {
        gtk_widget_error_bell(GTK_WIDGET(w->con_in));
    }
    g_strfreev(words);
    return TRUE;
}

static void on_reg_activate(GtkEntry *entry, gpointer ud)
{
    DbgWin *w = ud;
    int i = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(entry), "reg"));
    long v;
    if (parse_num(gtk_editable_get_text(GTK_EDITABLE(entry)), &v))
        a800debug_cpu_set(w->dbg, k_regs[i], (int)(v & (i == 0 ? 0xFFFF : 0xFF)));
    else
        toast(w, "Not a number ($ or plain hex, # decimal)");
    resync(w);
}

static void on_flag_toggled(GtkCheckButton *b, gpointer ud)
{
    DbgWin *w = ud;
    int i = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "flag"));
    if (w->updating || !a800debug_is_stopped(w->dbg)) return;
    a800debug_cpu_set(w->dbg, k_flags[i], gtk_check_button_get_active(b));
    resync(w);
}

/* Go to: the dump from just above an address (or a label), its row marked
 * and scrolled to. */
static void on_mem_goto(GtkEntry *entry, gpointer ud)
{
    DbgWin *w = ud;
    GtkTextBuffer *b = gtk_text_view_get_buffer(w->mem_view);
    GtkTextMark *mark;
    GtkTextIter it;
    long a;
    if (!parse_addr(w, gtk_editable_get_text(GTK_EDITABLE(entry)), &a)) {
        w->mem_mark = -1;
        refresh_mem(w);
        toast(w, "Not an address or a label");
        return;
    }
    w->mem_mark = (int)(a & 0xFFFF);
    w->mem_top = (w->mem_mark - 0x100) & 0xFFF0;
    refresh_mem(w);
    gtk_text_buffer_get_iter_at_line(b, &it, 1 + ((w->mem_mark - w->mem_top) & 0xFFFF) / 16);
    mark = gtk_text_buffer_create_mark(b, NULL, &it, TRUE);
    gtk_text_view_scroll_to_mark(w->mem_view, mark, 0.0, TRUE, 0.0, 0.3);
    gtk_text_buffer_delete_mark(b, mark);
}

/* The dump a page of MEM_ROWS on or back. */
static void on_mem_page(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    int dir = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "dir"));
    w->mem_top = (w->mem_top + dir * MEM_ROWS * 16) & 0xFFFF;
    refresh_mem(w);
}

static void on_mem_write(GtkEntry *entry, gpointer ud)
{
    DbgWin *w = ud;
    long a, v;
    (void)entry;
    if (!parse_addr(w, gtk_editable_get_text(GTK_EDITABLE(w->mem_addr)), &a)) {
        toast(w, "Not an address or a label");
        return;
    }
    if (!parse_num(gtk_editable_get_text(GTK_EDITABLE(w->mem_val)), &v) || v < 0 || v > 0xFF) {
        toast(w, "The value is a byte ($00-$FF)");
        return;
    }
    a800debug_write(w->dbg, (uint16_t)a, (uint8_t)v);
    resync(w);
}

/* The selected line's underline moved, without disassembling again. */
static void mark_selected(DbgWin *w)
{
    GtkTextBuffer *b = gtk_text_view_get_buffer(w->disasm);
    GtkTextIter s, e;
    int i;
    gtk_text_buffer_get_bounds(b, &s, &e);
    gtk_text_buffer_remove_tag_by_name(b, "sel", &s, &e);
    for (i = 0; i < w->line_count; i++)
        if (w->line_addr[i] == w->selected) {
            gtk_text_buffer_get_iter_at_line(b, &s, i);
            gtk_text_buffer_get_iter_at_line(b, &e, i + 1);
            gtk_text_buffer_apply_tag_by_name(b, "sel", &s, &e);
        }
}

/* A click selects a line (Run to Cursor); a click on its left edge, or a
 * double-click, toggles its breakpoint. */
static void on_disasm_click(GtkGestureClick *g, int n, double x, double y, gpointer ud)
{
    DbgWin *w = ud;
    GtkTextIter it;
    int bx, by, line;
    gboolean gutter;
    (void)g;
    gtk_text_view_window_to_buffer_coords(w->disasm, GTK_TEXT_WINDOW_WIDGET, (int)x, (int)y, &bx, &by);
    gtk_text_view_get_iter_at_location(w->disasm, &it, bx, by);
    line = gtk_text_iter_get_line(&it);
    if (line < 0 || line >= w->line_count) return;
    gutter = gtk_text_iter_get_line_offset(&it) <= 2;
    w->selected = w->line_addr[line];
    if (gutter ? n == 1 : n == 2)
        toggle_breakpoint(w, w->line_addr[line]);
    else
        mark_selected(w);
}

static void on_follow_toggled(GtkCheckButton *b, gpointer ud)
{
    (void)b;
    refresh_disasm(ud);
}

static void on_jump(GtkEntry *entry, gpointer ud)
{
    DbgWin *w = ud;
    long a;
    if (!parse_addr(w, gtk_editable_get_text(GTK_EDITABLE(entry)), &a)) {
        toast(w, "Not an address or a label");
        return;
    }
    gtk_check_button_set_active(w->follow_pc, FALSE);
    w->disasm_top = (int)(a & 0xFFFF);
    w->selected = w->disasm_top;
    refresh_disasm(w);
}

static gboolean on_disasm_scroll(GtkEventControllerScroll *c, double dx, double dy, gpointer ud)
{
    DbgWin *w = ud;
    int rows = (int)(dy * 3);
    (void)c; (void)dx;
    if (!rows) rows = dy > 0 ? 1 : -1;
    gtk_check_button_set_active(w->follow_pc, FALSE);
    w->disasm_top = a800debug_row_address(w->dbg, (uint16_t)w->disasm_top, rows);
    refresh_disasm(w);
    return TRUE;
}

static void draw_swatch(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer ud)
{
    DbgWin *w = g_object_get_data(G_OBJECT(area), "dbg");
    int i = GPOINTER_TO_INT(ud);
    guint32 rgb = w ? w->swatch_rgb[i] : 0;
    cairo_rectangle(cr, 0.5, 0.5, width - 1, height - 1);
    cairo_set_source_rgb(cr, ((rgb >> 16) & 0xFF) / 255.0, ((rgb >> 8) & 0xFF) / 255.0,
                         (rgb & 0xFF) / 255.0);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 0.5, 0.5, 0.5, 0.8);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
}

/* The 256 colours, 16 hues down and 16 lumas across, nearest-neighbour so
 * the cells stay crisp, with their hex digits along the edges. */
static void draw_palette(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer ud)
{
    DbgWin *w = ud;
    const double cw = (width - PAL_AXIS_W) / 16.0, ch = (height - PAL_AXIS_H) / 16.0;
    cairo_surface_t *surf;
    GdkRGBA fg;
    int i;

    gtk_widget_get_color(GTK_WIDGET(area), &fg);
    gdk_cairo_set_source_rgba(cr, &fg);
    cairo_select_font_face(cr, "monospace", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 11);
    for (i = 0; i < 16; i++) {
        char d[4];
        g_snprintf(d, sizeof d, "%X", i);
        cairo_move_to(cr, PAL_AXIS_W + i * cw + cw / 2 - 3, PAL_AXIS_H - 5);
        cairo_show_text(cr, d);
        g_snprintf(d, sizeof d, "%X0", i);
        cairo_move_to(cr, 4, PAL_AXIS_H + i * ch + ch / 2 + 4);
        cairo_show_text(cr, d);
    }
    if (!w->palette_ok) return;
    surf = cairo_image_surface_create_for_data((unsigned char *)w->palette_px, CAIRO_FORMAT_RGB24,
                                               A800DEBUG_PALETTE_WIDTH, A800DEBUG_PALETTE_HEIGHT,
                                               A800DEBUG_PALETTE_WIDTH * 4);
    cairo_save(cr);
    cairo_translate(cr, PAL_AXIS_W, PAL_AXIS_H);
    cairo_scale(cr, (width - PAL_AXIS_W) / (double)A800DEBUG_PALETTE_WIDTH,
                (height - PAL_AXIS_H) / (double)A800DEBUG_PALETTE_HEIGHT);
    cairo_set_source_surface(cr, surf, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
    cairo_paint(cr);
    cairo_restore(cr);
    cairo_surface_destroy(surf);
}

static void on_palette_click(GtkGestureClick *g, int n, double x, double y, gpointer ud)
{
    DbgWin *w = ud;
    const int width = gtk_widget_get_width(w->palette), height = gtk_widget_get_height(w->palette);
    int hue, luma, index;
    char text[96];
    (void)g; (void)n;
    if (x < PAL_AXIS_W || y < PAL_AXIS_H || width <= PAL_AXIS_W || height <= PAL_AXIS_H) return;
    luma = (int)((x - PAL_AXIS_W) * 16 / (width - PAL_AXIS_W));
    hue = (int)((y - PAL_AXIS_H) * 16 / (height - PAL_AXIS_H));
    if (luma > 15 || hue > 15) return;
    index = hue << 4 | luma;
    g_snprintf(text, sizeof text, "Colour $%02X: hue %d, luma %d, #%06X", index, hue, luma,
               a800debug_color(w->dbg, (uint8_t)index));
    gtk_label_set_text(w->palette_info, text);
}

static void bp_enable_toggled(GtkCheckButton *b, gpointer ud)
{
    DbgWin *w = ud;
    /* the list itself follows on the next tick (it rebuilds) */
    a800debug_breakpoint_enable(w->dbg, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "bp")),
                                gtk_check_button_get_active(b));
}

static void bp_remove_clicked(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    a800debug_breakpoint_remove(w->dbg, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "bp")));
}

static void on_bp_add(GtkWidget *wgt, gpointer ud)
{
    DbgWin *w = ud;
    static const int types[4] = { A800DEBUG_BP_EXEC, A800DEBUG_BP_READ, A800DEBUG_BP_WRITE,
                                  A800DEBUG_BP_READ | A800DEBUG_BP_WRITE };
    guint sel = gtk_drop_down_get_selected(w->bp_type);
    const char *end = gtk_editable_get_text(GTK_EDITABLE(w->bp_end));
    char msg[200] = "";
    long a, b;
    int id;
    (void)wgt;
    if (sel >= G_N_ELEMENTS(types)) sel = 0;
    if (!parse_addr(w, gtk_editable_get_text(GTK_EDITABLE(w->bp_start)), &a)) {
        gtk_label_set_text(w->bp_msg, "Not an address or a label");
        return;
    }
    b = a;
    if (end && *end && !parse_addr(w, end, &b)) {
        gtk_label_set_text(w->bp_msg, "The end is not an address or a label");
        return;
    }
    id = a800debug_breakpoint_add(w->dbg, types[sel], (uint16_t)a, (uint16_t)b,
                                  gtk_editable_get_text(GTK_EDITABLE(w->bp_cond)), msg, sizeof msg);
    if (id < 0) {
        gtk_label_set_text(w->bp_msg, msg[0] ? msg : "atari800 did not take it");
        return;
    }
    gtk_label_set_text(w->bp_msg, "");
    gtk_editable_set_text(GTK_EDITABLE(w->bp_start), "");
    gtk_editable_set_text(GTK_EDITABLE(w->bp_end), "");
    gtk_editable_set_text(GTK_EDITABLE(w->bp_cond), "");
    resync(w);
}

static void on_bp_clear(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    (void)b;
    a800debug_breakpoint_clear(w->dbg);
    resync(w);
}

/* A scanline break is atari800's own (BLINE): the next Run stops there. */
static void on_bp_line(GtkWidget *wgt, gpointer ud)
{
    DbgWin *w = ud;
    static char out[1024];
    char cmd[32], *nl;
    long n;
    const char *text = gtk_editable_get_text(GTK_EDITABLE(w->bp_line));
    char *end;
    (void)wgt;
    n = strtol(text, &end, 10);
    while (*end == ' ') end++;
    if (end == text || *end || n < 0 || n > 311) {
        gtk_label_set_text(w->bp_msg, "A scanline is 0-311 (decimal)");
        return;
    }
    if (w->con_asm) {
        gtk_label_set_text(w->bp_msg, "The Console's assembler is waiting: finish it first");
        return;
    }
    g_snprintf(cmd, sizeof cmd, "BLINE %ld", n);
    a800debug_command(w->dbg, cmd, out, sizeof out);
    /* in the console's transcript too, as if typed there */
    {
        char line[48];
        g_snprintf(line, sizeof line, "> %s\n", cmd);
        console_append(w, line);
        console_output(w, out);
    }
    if ((nl = strchr(out, '\n')) != NULL) *nl = '\0';
    gtk_label_set_text(w->bp_msg, out[0] ? out : cmd);
    resync(w);
}

static void on_sio_clear(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    (void)b;
    a800debug_sio_clear(w->dbg);
    refresh_sio(w, TRUE);
}

static void on_fuji_clear(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    (void)b;
    a800debug_fujinet_trace_clear(w->dbg);
    refresh_fujinet(w, TRUE);
}

static void on_symbols_chosen(GObject *src, GAsyncResult *res, gpointer ud)
{
    DbgWin *w = ud;
    g_autoptr(GFile) file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
    g_autofree char *path = NULL;
    char msg[512], line[700];
    if (!file) return;
    path = g_file_get_path(file);
    if (!path) return;
    if (a800debug_load_symbols(w->dbg, path, msg, sizeof msg) >= 0) {
        g_autofree char *base = g_path_get_basename(path);
        g_snprintf(line, sizeof line, "Loaded %s from %s", msg, base);
    } else {
        g_snprintf(line, sizeof line, "No symbols loaded: %s", msg);
    }
    say(w, line);
    resync(w);
}

static void on_symbols(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    GtkFileDialog *dlg = gtk_file_dialog_new();
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    GtkFileFilter *syms = gtk_file_filter_new();
    GtkFileFilter *all = gtk_file_filter_new();
    (void)b;
    gtk_file_filter_set_name(syms, "Label files (*.lbl, *.lab, *.sym, *.txt)");
    gtk_file_filter_add_suffix(syms, "lbl");
    gtk_file_filter_add_suffix(syms, "lab");
    gtk_file_filter_add_suffix(syms, "sym");
    gtk_file_filter_add_suffix(syms, "txt");
    gtk_file_filter_set_name(all, "All files");
    gtk_file_filter_add_pattern(all, "*");
    g_list_store_append(filters, syms);
    g_list_store_append(filters, all);
    gtk_file_dialog_set_title(dlg, "Load Symbols (ld65 -Ln, VICE .lbl, MADS .lab, atari800 labels)");
    gtk_file_dialog_set_filters(dlg, G_LIST_MODEL(filters));
    gtk_file_dialog_open(dlg, w->win, NULL, on_symbols_chosen, w);
    g_object_unref(syms);
    g_object_unref(all);
    g_object_unref(filters);
    g_object_unref(dlg);
}

static void on_save_chosen(GObject *src, GAsyncResult *res, gpointer ud)
{
    DbgWin *w = ud;
    g_autoptr(GFile) file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(src), res, NULL);
    g_autofree char *path = NULL;
    const char *kind = g_object_get_data(src, "kind");
    char msg[512], line[700];
    if (!file) return;
    path = g_file_get_path(file);
    if (!path || !kind) return;
    if (a800debug_save(w->dbg, kind, path, msg, sizeof msg) == 0) {
        g_autofree char *base = g_path_get_basename(path);
        g_snprintf(line, sizeof line, "%s: %s", base, msg);
        say(w, line);
    } else {
        say(w, msg);
    }
}

static void on_save(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    GtkFileDialog *dlg = gtk_file_dialog_new();
    GtkWidget *pop = gtk_widget_get_ancestor(GTK_WIDGET(b), GTK_TYPE_POPOVER);
    char title[96];
    if (pop) gtk_popover_popdown(GTK_POPOVER(pop));
    g_snprintf(title, sizeof title, "Save %s", (const char *)g_object_get_data(G_OBJECT(b), "title"));
    gtk_file_dialog_set_title(dlg, title);
    gtk_file_dialog_set_initial_name(dlg, g_object_get_data(G_OBJECT(b), "file"));
    g_object_set_data(G_OBJECT(dlg), "kind", g_object_get_data(G_OBJECT(b), "kind"));
    gtk_file_dialog_save(dlg, w->win, NULL, on_save_chosen, w);
    g_object_unref(dlg);
}

static void on_switch_page(GtkNotebook *nb, GtkWidget *page, guint num, gpointer ud)
{
    (void)nb; (void)page;
    refresh_page(ud, (int)num);
}

static void hide_window(DbgWin *w)
{
    a800debug_detach(w->dbg);     /* lets it run; a waiting assembler gives up */
    w->attached_running = FALSE;
    set_prompt(w, FALSE, NULL);
    gtk_widget_set_visible(GTK_WIDGET(w->win), FALSE);
}

static gboolean on_key(GtkEventControllerKey *c, guint keyval, guint code,
                       GdkModifierType st, gpointer ud)
{
    DbgWin *w = ud;
    (void)c; (void)code;
    switch (keyval) {
    case GDK_KEY_F5: on_run(NULL, w); return TRUE;
    case GDK_KEY_F7: on_step(NULL, w); return TRUE;
    case GDK_KEY_F8:
        if (st & GDK_SHIFT_MASK) on_step_out(NULL, w); else on_step_over(NULL, w);
        return TRUE;
    case GDK_KEY_F9: on_frame(NULL, w); return TRUE;
    case GDK_KEY_F12: hide_window(w); return TRUE;
    default: return FALSE;
    }
}

static gboolean on_close(GtkWindow *win, gpointer ud)
{
    (void)win;
    hide_window(ud);
    return TRUE;
}

/* ---- construction --------------------------------------------------------- */

static GtkWidget *build_toolbar(DbgWin *w)
{
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *b;
    gtk_widget_set_margin_start(bar, 8);
    gtk_widget_set_margin_end(bar, 8);
    gtk_widget_set_margin_top(bar, 6);
    gtk_widget_set_margin_bottom(bar, 6);
#define TB(label, tip, cb) do { b = gtk_button_new_with_label(label); \
    gtk_widget_set_tooltip_text(b, tip); \
    g_signal_connect(b, "clicked", G_CALLBACK(cb), w); gtk_box_append(GTK_BOX(bar), b); } while (0)
    w->run_btn = GTK_BUTTON(gtk_button_new_with_label("Stop (F5)"));
    g_signal_connect(w->run_btn, "clicked", G_CALLBACK(on_run), w);
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(w->run_btn));
    TB("Step (F7)", "One instruction", on_step);
    TB("Step Over (F8)", "Over a JSR, to the instruction after it", on_step_over);
    TB("Step Out (\xe2\x87\xa7""F8)", "On to the RTS or RTI", on_step_out);
    TB("Frame (F9)", "On to the next vertical blank", on_frame);
    TB("Run to Cursor", "On to the line selected in the Disassembly", on_run_to);
#undef TB
    w->brk_btn = GTK_TOGGLE_BUTTON(gtk_toggle_button_new_with_label("Break on BRK"));
    gtk_widget_set_tooltip_text(GTK_WIDGET(w->brk_btn), "Stop at a BRK instead of running it");
    g_signal_connect(w->brk_btn, "toggled", G_CALLBACK(on_brk_toggled), w);
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(w->brk_btn));

    w->status = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(w->status), "dim-label");
    gtk_widget_set_hexpand(GTK_WIDGET(w->status), TRUE);
    gtk_label_set_xalign(w->status, 1.0);
    gtk_label_set_ellipsize(w->status, PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(w->status));
    return bar;
}

/* The header bar's end: snapshot slots, Load Symbols, Save. */
static void build_header(DbgWin *w, AdwHeaderBar *header)
{
    static const struct { const char *kind, *title, *file; } saves[] = {
        { "dis", "Disassembly (256 lines from the PC)", "disassembly.asm" },
        { "ram", "Memory (64K as the CPU sees it)", "memory.bin" } };
    GtkWidget *snap = gtk_menu_button_new(), *pop = gtk_popover_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *grid = gtk_grid_new();
    GtkWidget *sym, *save, *savepop, *savebox;
    unsigned i;

    gtk_menu_button_set_label(GTK_MENU_BUTTON(snap), "Snapshots");
    gtk_widget_set_tooltip_text(snap, "Temporary save states, slots 0-3");
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    for (i = 0; i < SNAPSHOTS; i++) {
        char name[16];
        GtkWidget *l, *sb, *rb;
        g_snprintf(name, sizeof name, "Slot %u", i);
        l = gtk_label_new(name);
        gtk_label_set_xalign(GTK_LABEL(l), 0);
        sb = gtk_button_new_with_label("Save");
        rb = gtk_button_new_with_label("Restore");
        g_object_set_data(G_OBJECT(sb), "slot", GINT_TO_POINTER(i));
        g_object_set_data(G_OBJECT(rb), "slot", GINT_TO_POINTER(i));
        g_signal_connect(sb, "clicked", G_CALLBACK(on_snap_save), w);
        g_signal_connect(rb, "clicked", G_CALLBACK(on_snap_restore), w);
        w->snap_restore[i] = rb;
        w->snap_state[i] = GTK_LABEL(gtk_label_new("empty"));
        gtk_widget_add_css_class(GTK_WIDGET(w->snap_state[i]), "dim-label");
        gtk_grid_attach(GTK_GRID(grid), l, 0, (int)i, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), sb, 1, (int)i, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), rb, 2, (int)i, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(w->snap_state[i]), 3, (int)i, 1, 1);
    }
    w->snap_note = gtk_label_new(k_fujinet_note);
    gtk_label_set_wrap(GTK_LABEL(w->snap_note), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(w->snap_note), 44);
    gtk_label_set_xalign(GTK_LABEL(w->snap_note), 0);
    gtk_widget_add_css_class(w->snap_note, "a800-accent-text");
    gtk_box_append(GTK_BOX(box), title_label("Snapshots"));
    gtk_box_append(GTK_BOX(box), grid);
    gtk_box_append(GTK_BOX(box), w->snap_note);
    gtk_box_append(GTK_BOX(box), hint_label("Kept until the app quits."));
    padded(box);
    gtk_popover_set_child(GTK_POPOVER(pop), box);
    g_signal_connect(pop, "show", G_CALLBACK(on_snap_show), w);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(snap), pop);

    sym = gtk_button_new_with_label("Load Symbols\xe2\x80\xa6");
    gtk_widget_set_tooltip_text(sym, "Labels from ld65 -Ln, VICE .lbl, MADS .lab or an atari800 "
                                     "label file, beside the OS's own");
    gtk_widget_add_css_class(sym, "flat");
    g_signal_connect(sym, "clicked", G_CALLBACK(on_symbols), w);

    save = gtk_menu_button_new();
    gtk_menu_button_set_label(GTK_MENU_BUTTON(save), "Save\xe2\x80\xa6");
    savepop = gtk_popover_new();
    savebox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    for (i = 0; i < G_N_ELEMENTS(saves); i++) {
        GtkWidget *sb = gtk_button_new_with_label(saves[i].title);
        gtk_widget_add_css_class(sb, "flat");
        g_object_set_data(G_OBJECT(sb), "kind", (gpointer)saves[i].kind);
        g_object_set_data(G_OBJECT(sb), "title", (gpointer)saves[i].title);
        g_object_set_data(G_OBJECT(sb), "file", (gpointer)saves[i].file);
        g_signal_connect(sb, "clicked", G_CALLBACK(on_save), w);
        gtk_box_append(GTK_BOX(savebox), sb);
    }
    gtk_popover_set_child(GTK_POPOVER(savepop), savebox);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(save), savepop);

    adw_header_bar_pack_end(header, save);
    adw_header_bar_pack_end(header, sym);
    adw_header_bar_pack_end(header, snap);
}

static GtkWidget *build_console(DbgWin *w)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *scroll = mono_view(&w->con_out, FALSE);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkTextBuffer *b = gtk_text_view_get_buffer(w->con_out);
    GtkEventController *keys;

    gtk_text_view_set_wrap_mode(w->con_out, GTK_WRAP_WORD_CHAR);
    gtk_text_buffer_set_text(b, "atari800's monitor. Type HELP for its commands; Tab completes "
                                "them, Up/Down recall. G, R, O and CONT let the machine go; "
                                "A assembles a line at a time.\n", -1);
    make_log(w->con_out);

    w->con_prompt = GTK_LABEL(gtk_label_new("> "));
    gtk_widget_add_css_class(GTK_WIDGET(w->con_prompt), "monospace");
    w->con_in = GTK_ENTRY(gtk_entry_new());
    gtk_widget_add_css_class(GTK_WIDGET(w->con_in), "monospace");
    gtk_widget_set_hexpand(GTK_WIDGET(w->con_in), TRUE);
    g_signal_connect(w->con_in, "activate", G_CALLBACK(on_console), w);
    /* ahead of the entry's own handling, so Tab and Up/Down are ours */
    keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_console_key), w);
    gtk_widget_add_controller(GTK_WIDGET(w->con_in), keys);
    w->history = g_ptr_array_new_with_free_func(g_free);
    set_prompt(w, FALSE, NULL);

    gtk_box_append(GTK_BOX(row), GTK_WIDGET(w->con_prompt));
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(w->con_in));
    gtk_box_append(GTK_BOX(box), scroll);
    gtk_box_append(GTK_BOX(box), row);
    return padded(box);
}

static GtkWidget *build_cpu(DbgWin *w)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *regs = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *flags = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *mem = mono_view(&w->mem_view, FALSE);
    GtkWidget *hist = mono_view(&w->hist_view, FALSE);
    GtkWidget *nav = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *b;
    static const char *const names[6] = { "PC", "SP", "A", "X", "Y", "P" };
    static const char *const fnames[6] = { "N", "V", "D", "I", "Z", "C" };
    int i;

    for (i = 0; i < 6; i++) {
        w->reg[i] = GTK_ENTRY(gtk_entry_new());
        gtk_entry_set_max_length(w->reg[i], 6);
        gtk_editable_set_width_chars(GTK_EDITABLE(w->reg[i]), i == 0 ? 5 : 3);
        gtk_editable_set_max_width_chars(GTK_EDITABLE(w->reg[i]), i == 0 ? 5 : 3);
        gtk_widget_add_css_class(GTK_WIDGET(w->reg[i]), "monospace");
        gtk_widget_set_tooltip_text(GTK_WIDGET(w->reg[i]), "Enter sets it");
        g_object_set_data(G_OBJECT(w->reg[i]), "reg", GINT_TO_POINTER(i));
        g_signal_connect(w->reg[i], "activate", G_CALLBACK(on_reg_activate), w);
        gtk_box_append(GTK_BOX(regs), labeled(names[i], GTK_WIDGET(w->reg[i])));
    }
    for (i = 0; i < 6; i++) {
        w->flag[i] = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(fnames[i]));
        g_object_set_data(G_OBJECT(w->flag[i]), "flag", GINT_TO_POINTER(i));
        g_signal_connect(w->flag[i], "toggled", G_CALLBACK(on_flag_toggled), w);
        gtk_box_append(GTK_BOX(flags), GTK_WIDGET(w->flag[i]));
    }
    w->beam = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(w->beam), "dim-label");
    gtk_widget_set_margin_start(GTK_WIDGET(w->beam), 16);
    gtk_box_append(GTK_BOX(flags), GTK_WIDGET(w->beam));

    /* the dump: Go to, a page back or on, and a write */
    w->mem_goto = small_entry("$0600 / label", 12);
    g_signal_connect(w->mem_goto, "activate", G_CALLBACK(on_mem_goto), w);
    gtk_box_append(GTK_BOX(nav), labeled("Go to", GTK_WIDGET(w->mem_goto)));
    b = gtk_button_new_from_icon_name("go-up-symbolic");
    gtk_widget_set_tooltip_text(b, "Back 4K");
    g_object_set_data(G_OBJECT(b), "dir", GINT_TO_POINTER(-1));
    g_signal_connect(b, "clicked", G_CALLBACK(on_mem_page), w);
    gtk_box_append(GTK_BOX(nav), b);
    b = gtk_button_new_from_icon_name("go-down-symbolic");
    gtk_widget_set_tooltip_text(b, "On 4K");
    g_object_set_data(G_OBJECT(b), "dir", GINT_TO_POINTER(1));
    g_signal_connect(b, "clicked", G_CALLBACK(on_mem_page), w);
    gtk_box_append(GTK_BOX(nav), b);
    w->mem_addr = small_entry("$0600", 10);
    w->mem_val = small_entry("$00", 4);
    gtk_widget_set_margin_start(GTK_WIDGET(w->mem_addr), 12);
    g_signal_connect(w->mem_addr, "activate", G_CALLBACK(on_mem_write), w);
    g_signal_connect(w->mem_val, "activate", G_CALLBACK(on_mem_write), w);
    gtk_box_append(GTK_BOX(nav), labeled("Write", GTK_WIDGET(w->mem_addr)));
    gtk_box_append(GTK_BOX(nav), labeled("value", GTK_WIDGET(w->mem_val)));
    gtk_box_append(GTK_BOX(nav), hint_label("a CPU write: hardware registers take it as the "
                                            "6502's would"));

    gtk_box_append(GTK_BOX(left), title_label("Memory, as the CPU sees it (reads have no side effects)"));
    gtk_box_append(GTK_BOX(left), mem);
    gtk_box_append(GTK_BOX(right), title_label("Last instructions"));
    gtk_box_append(GTK_BOX(right), hist);
    gtk_paned_set_start_child(GTK_PANED(paned), left);
    gtk_paned_set_end_child(GTK_PANED(paned), right);
    gtk_paned_set_shrink_start_child(GTK_PANED(paned), FALSE);
    gtk_paned_set_shrink_end_child(GTK_PANED(paned), FALSE);
    gtk_paned_set_resize_end_child(GTK_PANED(paned), FALSE);
    gtk_widget_set_size_request(left, 600, -1);
    gtk_widget_set_size_request(right, 340, -1);
    gtk_widget_set_vexpand(paned, TRUE);

    gtk_box_append(GTK_BOX(box), regs);
    gtk_box_append(GTK_BOX(box), flags);
    gtk_box_append(GTK_BOX(box), paned);
    gtk_box_append(GTK_BOX(box), nav);
    accent_tag(w->mem_view, "mark");
    make_log(w->hist_view);
    w->mem_top = 0x0000;
    w->mem_mark = -1;
    return padded(box);
}

static GtkWidget *build_disasm(DbgWin *w)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *scroll = mono_view(&w->disasm, FALSE);
    GtkGesture *click = gtk_gesture_click_new();
    GtkEventController *scrollc = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    GtkTextBuffer *b = gtk_text_view_get_buffer(w->disasm);
    GtkWidget *hint;

    /* created in priority order: the PC line's white text wins */
    gtk_text_buffer_create_tag(b, "bp", "foreground", "#e01b24", "weight", PANGO_WEIGHT_BOLD, NULL);
    gtk_text_buffer_create_tag(b, "bpoff", "foreground", "#9a9996", NULL);
    gtk_text_buffer_create_tag(b, "sel", "underline", PANGO_UNDERLINE_SINGLE,
                               "weight", PANGO_WEIGHT_BOLD, NULL);
    accent_tag(w->disasm, "pc");

    w->follow_pc = GTK_CHECK_BUTTON(gtk_check_button_new_with_label("Follow PC"));
    gtk_check_button_set_active(w->follow_pc, TRUE);
    g_signal_connect(w->follow_pc, "toggled", G_CALLBACK(on_follow_toggled), w);
    w->jump = small_entry("$E456 or CIOV", 14);
    g_signal_connect(w->jump, "activate", G_CALLBACK(on_jump), w);
    hint = hint_label("Click a line to select it (Run to Cursor); click its left edge or "
                      "double-click it for a breakpoint (* on, o off); scroll to browse");
    gtk_widget_set_hexpand(hint, TRUE);
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(w->follow_pc));
    gtk_box_append(GTK_BOX(row), labeled("Jump to", GTK_WIDGET(w->jump)));
    gtk_box_append(GTK_BOX(row), hint);

    g_signal_connect(click, "pressed", G_CALLBACK(on_disasm_click), w);
    gtk_widget_add_controller(GTK_WIDGET(w->disasm), GTK_EVENT_CONTROLLER(click));
    g_signal_connect(scrollc, "scroll", G_CALLBACK(on_disasm_scroll), w);
    gtk_widget_add_controller(GTK_WIDGET(w->disasm), scrollc);
    /* no scrollbar and no height of its own: as many lines show as fit */
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_EXTERNAL);
    w->disasm_scroll = scroll;

    gtk_box_append(GTK_BOX(box), row);
    gtk_box_append(GTK_BOX(box), scroll);
    w->disasm_top = 0xE000;
    w->selected = -1;
    return padded(box);
}

static GtkWidget *build_antic(DbgWin *w)
{
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);

    gtk_box_append(GTK_BOX(left), title_label("Registers"));
    gtk_box_append(GTK_BOX(left), mono_view(&w->antic_text, FALSE));
    gtk_box_append(GTK_BOX(right), title_label("Display list"));
    gtk_box_append(GTK_BOX(right), mono_view(&w->dlist_text, FALSE));
    gtk_widget_set_size_request(left, 520, -1);
    gtk_paned_set_start_child(GTK_PANED(paned), left);
    gtk_paned_set_end_child(GTK_PANED(paned), right);
    gtk_paned_set_shrink_start_child(GTK_PANED(paned), FALSE);
    gtk_paned_set_shrink_end_child(GTK_PANED(paned), FALSE);
    gtk_widget_set_size_request(right, 240, -1);
    return padded(paned);
}

static GtkWidget *build_gtia(DbgWin *w)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *grid = gtk_grid_new();
    GtkGesture *click = gtk_gesture_click_new();
    int i;

    gtk_box_append(GTK_BOX(box), mono_view(&w->gtia_text, FALSE));

    /* the colour registers: COLPM0-3, COLPF0-3, then COLBK */
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 6);
    for (i = 0; i < SWATCHES; i++) {
        GtkWidget *cell = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        w->swatch[i] = gtk_drawing_area_new();
        gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(w->swatch[i]), 22);
        gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(w->swatch[i]), 18);
        gtk_widget_set_valign(w->swatch[i], GTK_ALIGN_CENTER);
        g_object_set_data(G_OBJECT(w->swatch[i]), "dbg", w);
        gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(w->swatch[i]), draw_swatch,
                                       GINT_TO_POINTER(i), NULL);
        w->swatch_label[i] = GTK_LABEL(gtk_label_new(""));
        gtk_widget_add_css_class(GTK_WIDGET(w->swatch_label[i]), "monospace");
        gtk_box_append(GTK_BOX(cell), w->swatch[i]);
        gtk_box_append(GTK_BOX(cell), GTK_WIDGET(w->swatch_label[i]));
        gtk_grid_attach(GTK_GRID(grid), cell, i & 3, i / 4, 1, 1);
    }

    w->palette = gtk_drawing_area_new();
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(w->palette), PAL_AXIS_W + 16 * PAL_CELL_W);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(w->palette), PAL_AXIS_H + 16 * PAL_CELL_H);
    gtk_widget_set_halign(w->palette, GTK_ALIGN_START);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(w->palette), draw_palette, w, NULL);
    g_signal_connect(click, "pressed", G_CALLBACK(on_palette_click), w);
    gtk_widget_add_controller(w->palette, GTK_EVENT_CONTROLLER(click));
    w->palette_info = GTK_LABEL(hint_label("Click a colour for its value"));
    gtk_label_set_selectable(w->palette_info, TRUE);

    gtk_box_append(GTK_BOX(right), title_label("Colour registers"));
    gtk_box_append(GTK_BOX(right), grid);
    gtk_box_append(GTK_BOX(right), title_label("Palette: hues down, lumas across"));
    gtk_box_append(GTK_BOX(right), w->palette);
    gtk_box_append(GTK_BOX(right), GTK_WIDGET(w->palette_info));
    gtk_box_append(GTK_BOX(box), right);
    w->palette_px = g_new0(guint32, A800DEBUG_PALETTE_WIDTH * A800DEBUG_PALETTE_HEIGHT);
    return padded(box);
}

static GtkWidget *build_breaks(DbgWin *w)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *add, *clear, *set;
    static const char *const types[] = { "Execute", "Read", "Write", "Read/Write", NULL };

    w->bp_budget = GTK_LABEL(hint_label(""));
    gtk_widget_set_hexpand(GTK_WIDGET(w->bp_budget), TRUE);
    gtk_widget_set_tooltip_text(GTK_WIDGET(w->bp_budget),
        "atari800 checks up to 20 entries before each instruction: a breakpoint takes one "
        "(two for a range), each condition one more, and one joins each to the next");
    clear = gtk_button_new_with_label("Clear All");
    g_signal_connect(clear, "clicked", G_CALLBACK(on_bp_clear), w);
    gtk_box_append(GTK_BOX(top), GTK_WIDGET(w->bp_budget));
    gtk_box_append(GTK_BOX(top), clear);

    w->bp_list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(w->bp_list, GTK_SELECTION_NONE);
    gtk_widget_add_css_class(GTK_WIDGET(w->bp_list), "boxed-list");
    gtk_widget_set_valign(GTK_WIDGET(w->bp_list), GTK_ALIGN_START);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(w->bp_list));
    gtk_widget_set_vexpand(scroll, TRUE);

    w->bp_type = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(types));
    w->bp_start = small_entry("$0600 / label", 12);
    w->bp_end = small_entry("(end)", 8);
    w->bp_cond = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(w->bp_cond,
        "condition: A=$10 && X<4 && MEM[$D01F]=6 \xe2\x80\x94 optional");
    gtk_widget_set_hexpand(GTK_WIDGET(w->bp_cond), TRUE);
    g_signal_connect(w->bp_start, "activate", G_CALLBACK(on_bp_add), w);
    g_signal_connect(w->bp_end, "activate", G_CALLBACK(on_bp_add), w);
    g_signal_connect(w->bp_cond, "activate", G_CALLBACK(on_bp_add), w);
    add = gtk_button_new_with_label("Add");
    g_signal_connect(add, "clicked", G_CALLBACK(on_bp_add), w);
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(w->bp_type));
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(w->bp_start));
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(w->bp_end));
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(w->bp_cond));
    gtk_box_append(GTK_BOX(row), add);

    w->bp_line = small_entry("100", 5);
    g_signal_connect(w->bp_line, "activate", G_CALLBACK(on_bp_line), w);
    set = gtk_button_new_with_label("Set");
    g_signal_connect(set, "clicked", G_CALLBACK(on_bp_line), w);
    gtk_box_append(GTK_BOX(line), labeled("Break at scanline", GTK_WIDGET(w->bp_line)));
    gtk_box_append(GTK_BOX(line), set);
    gtk_box_append(GTK_BOX(line), hint_label("atari800's BLINE: the next Run (or step) stops "
                                             "when the beam reaches it, once"));

    w->bp_msg = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(w->bp_msg), "a800-accent-text");
    gtk_label_set_xalign(w->bp_msg, 0);
    gtk_label_set_wrap(w->bp_msg, TRUE);

    gtk_box_append(GTK_BOX(box), top);
    gtk_box_append(GTK_BOX(box), scroll);
    gtk_box_append(GTK_BOX(box), row);
    gtk_box_append(GTK_BOX(box), line);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(w->bp_msg));
    return padded(box);
}

static GtkWidget *build_sio(DbgWin *w)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *clear = gtk_button_new_with_label("Clear");

    w->sio_note = GTK_LABEL(hint_label(""));
    gtk_widget_set_hexpand(GTK_WIDGET(w->sio_note), TRUE);
    g_signal_connect(clear, "clicked", G_CALLBACK(on_sio_clear), w);
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(w->sio_note));
    gtk_box_append(GTK_BOX(row), clear);
    gtk_box_append(GTK_BOX(box), row);
    gtk_box_append(GTK_BOX(box), mono_view(&w->sio_text, FALSE));
    make_log(w->sio_text);
    w->sio_count = -1;
    return padded(box);
}

static GtkWidget *build_fujinet(DbgWin *w)
{
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *clear = gtk_button_new_with_label("Clear");

    w->fuji_note = GTK_LABEL(hint_label(""));
    gtk_widget_set_hexpand(GTK_WIDGET(w->fuji_note), TRUE);
    g_signal_connect(clear, "clicked", G_CALLBACK(on_fuji_clear), w);
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(w->fuji_note));
    gtk_box_append(GTK_BOX(row), clear);
    gtk_box_append(GTK_BOX(left), title_label("SIO packets over NetSIO"));
    gtk_box_append(GTK_BOX(left), row);
    gtk_box_append(GTK_BOX(left), mono_view(&w->fuji_text, FALSE));
    make_log(w->fuji_text);

    gtk_box_append(GTK_BOX(right), title_label("NetSIO and POKEY's serial port"));
    gtk_box_append(GTK_BOX(right), mono_view(&w->fuji_state, FALSE));

    gtk_paned_set_start_child(GTK_PANED(paned), left);
    gtk_paned_set_end_child(GTK_PANED(paned), right);
    gtk_paned_set_shrink_start_child(GTK_PANED(paned), FALSE);
    gtk_paned_set_shrink_end_child(GTK_PANED(paned), FALSE);
    gtk_paned_set_resize_end_child(GTK_PANED(paned), FALSE);
    gtk_widget_set_size_request(left, 620, -1);
    gtk_widget_set_size_request(right, 340, -1);
    w->fuji_count = -1;
    return padded(paned);
}

static void on_destroy(GtkWidget *widget, gpointer ud)
{
    DbgWin *w = ud;
    (void)widget;
    if (w->timer) g_source_remove(w->timer);
    a800debug_detach(w->dbg);
    g_free(w->palette_px);
    g_free(w->bp_sig);
    if (w->history) g_ptr_array_unref(w->history);
    if (g_win == w) g_win = NULL;
    g_free(w);
}

/* A800_DEBUGGER_TAB: a page number, or a tab's name or label (any case), or
 * the start of one, or of a word of a label ("pia", "memory"...). */
static int tab_from_env(void)
{
    const char *tab = g_getenv("A800_DEBUGGER_TAB");
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
            if (pass == 0 && g_ascii_strncasecmp(tab, k_tabs[i].name, len) == 0) return i;
            if (pass == 1 && g_ascii_strncasecmp(tab, k_tabs[i].label, len) == 0) return i;
            if (pass == 2) {
                const char *p = k_tabs[i].label;
                while ((p = strchr(p, ' ')) != NULL)
                    if (g_ascii_strncasecmp(tab, ++p, len) == 0) return i;
            }
        }
    return -1;
}

void a800_debugger_show(GtkWindow *parent, a800session *session)
{
    DbgWin *w;
    GtkWidget *toolbar, *header, *root, *notebook, *overlay;
    GtkEventController *keys;
    int tab;

    if (g_win) {
        gtk_window_present(g_win->win);
        attach_and_refresh(g_win);
        return;
    }
    w = g_new0(DbgWin, 1);
    w->session = session;
    w->dbg = a800session_debugger(session);
    g_win = w;

    w->win = GTK_WINDOW(adw_window_new());
    gtk_window_set_title(w->win, "Debugger");
    gtk_window_set_default_size(w->win, 1150, 800);
    if (parent) {
        gtk_window_set_transient_for(w->win, parent);
        gtk_window_set_application(w->win, gtk_window_get_application(parent));
        gtk_window_set_destroy_with_parent(w->win, TRUE);
    }
    g_signal_connect(w->win, "close-request", G_CALLBACK(on_close), w);
    g_signal_connect(w->win, "destroy", G_CALLBACK(on_destroy), w);

    root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(root), build_toolbar(w));
    notebook = gtk_notebook_new();
    w->notebook = GTK_NOTEBOOK(notebook);
    gtk_notebook_set_scrollable(w->notebook, TRUE);
    gtk_widget_set_vexpand(notebook, TRUE);
    {
        GtkWidget *pages[TAB_COUNT];
        int i;
        pages[TAB_CONSOLE] = build_console(w);
        pages[TAB_CPU] = build_cpu(w);
        pages[TAB_DISASM] = build_disasm(w);
        pages[TAB_ANTIC] = build_antic(w);
        pages[TAB_GTIA] = build_gtia(w);
        pages[TAB_POKEY] = padded(mono_view(&w->pokey_text, FALSE));
        pages[TAB_MAP] = padded(mono_view(&w->map_text, FALSE));
        pages[TAB_BREAKS] = build_breaks(w);
        pages[TAB_SIO] = build_sio(w);
        pages[TAB_FUJINET] = build_fujinet(w);
        for (i = 0; i < TAB_COUNT; i++)
            gtk_notebook_append_page(w->notebook, pages[i], gtk_label_new(k_tabs[i].label));
    }
    gtk_box_append(GTK_BOX(root), notebook);
    tab = tab_from_env();
    if (tab >= 0) gtk_notebook_set_current_page(w->notebook, tab);
    g_signal_connect(notebook, "switch-page", G_CALLBACK(on_switch_page), w);

    overlay = adw_toast_overlay_new();
    w->toasts = ADW_TOAST_OVERLAY(overlay);
    adw_toast_overlay_set_child(w->toasts, root);

    header = adw_header_bar_new();
    build_header(w, ADW_HEADER_BAR(header));
    toolbar = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), overlay);
    adw_window_set_content(ADW_WINDOW(w->win), toolbar);

    keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key), w);
    gtk_widget_add_controller(GTK_WIDGET(w->win), keys);

    gtk_window_present(w->win);
    attach_and_refresh(w);
    w->timer = g_timeout_add(100, tick_refresh, w);
}

void a800_debugger_toggle(GtkWindow *parent, a800session *session)
{
    if (g_win && gtk_widget_get_visible(GTK_WIDGET(g_win->win)))
        hide_window(g_win);
    else
        a800_debugger_show(parent, session);
}
