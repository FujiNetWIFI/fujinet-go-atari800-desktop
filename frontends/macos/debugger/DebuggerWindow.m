/*
 * Debugger window (AppKit) over the session's debugger engine and
 * atari800's own monitor, via core/include/a800debug.h. Mirrors the GNOME
 * and KDE ones tab for tab.
 *
 * Tabs: the Console (atari800's monitor a line at a time, with Tab
 * completion and a command history; its A assembler prompts for lines),
 * CPU & Memory (registers, flags, the beam, a hex dump and the last
 * instructions run), the Disassembly (click a line to select it for Run to
 * Cursor; click its left edge, or double-click it, for its breakpoint),
 * ANTIC and its display list, GTIA (players, missiles, colours, collisions
 * and the palette), POKEY & PIA, the Memory Map and cartridge, Breakpoints
 * and watchpoints, the SIO command trace, and FujiNet (every NetSIO byte
 * both ways as SIO packets, from session start, beside NetSIO's state and
 * POKEY's serial port). The toolbar carries the stepping controls, the
 * snapshots, symbol loading and file saves; the family's keys apply (F5
 * run/stop, F7 step, F8 step over, Shift+F8 step out, F9 frame, F12 close).
 *
 * Showing the window attaches (which stops the machine, as on every
 * sibling); closing it detaches and lets the machine run on.
 *
 * A short timer polls the engine's generation and refreshes the tab showing
 * when it moved; while the machine runs, the status, the registers and the
 * SIO and FujiNet traces refresh twice a second. At each stop the console
 * takes the banner and registers atari800 printed, as its own monitor shows
 * them.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "DebuggerWindow.h"

#import "../KeyForward.h"

#include <float.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "a800debug.h"

#define DISASM_WINDOW 64        /* lines at most; as many show as fit */
#define MEM_ROWS 256            /* 4K of memory, 16 bytes a row */
#define HISTORY_MAX 64
#define SIO_MAX 512
#define FUJI_MAX 512
#define BP_MAX 64
#define CONSOLE_KEEP 262144u    /* the console transcript's characters kept */
#define SNAPSHOTS 4
#define SWATCHES 9
#define PAL_AXIS_W 30           /* the palette's hue labels */
#define PAL_AXIS_H 18           /* ... and its luma labels */
#define PAL_CELL_W 28
#define PAL_CELL_H 16
#define NUM_SAVES 2

enum { TAB_CONSOLE, TAB_CPU, TAB_DISASM, TAB_ANTIC, TAB_GTIA, TAB_POKEY, TAB_MAP,
       TAB_BREAKS, TAB_SIO, TAB_FUJINET, TAB_COUNT };

/* The tabs, in order: the names A800_DEBUGGER_TAB takes, and their labels
 * (which it takes too). */
static const struct { const char *name, *label; } kTabs[TAB_COUNT] = {
    { "console", "Console" }, { "cpu", "CPU & Memory" }, { "disassembly", "Disassembly" },
    { "antic", "ANTIC" }, { "gtia", "GTIA" }, { "pokey", "POKEY & PIA" },
    { "map", "Memory Map" }, { "breakpoints", "Breakpoints" }, { "sio", "SIO" },
    { "fujinet", "FujiNet" },
};

static const int kRegs[6] = { A800_REG_PC, A800_REG_SP, A800_REG_A, A800_REG_X, A800_REG_Y, A800_REG_P };
static const int kFlags[6] = { A800_FLAG_N, A800_FLAG_V, A800_FLAG_D, A800_FLAG_I, A800_FLAG_Z, A800_FLAG_C };

static const char *const kSaveKinds[NUM_SAVES] = { "dis", "ram" };
static NSString *const kSaveTitles[NUM_SAVES] = {
    @"Disassembly (256 lines from the PC)", @"Memory (64K as the CPU sees it)"
};
static NSString *const kSaveFiles[NUM_SAVES] = { @"disassembly.asm", @"memory.bin" };

static NSString *const kFujinetNote =
    @"FujiNet is running: restoring a snapshot replays the SIO traffic since it was "
    @"taken, which FujiNet has already acted on (sectors written, files and "
    @"connections opened), so the Atari and FujiNet can disagree afterwards.";

/* ---- helpers --------------------------------------------------------------- */

/* Engine text for a view: control bytes dropped (a label from a symbol file
 * need not be valid UTF-8). */
static NSString *textFrom(const char *s)
{
    const size_t n = s ? strlen(s) : 0;
    size_t o = 0;
    char *buf = malloc(n + 1);
    if (!buf) return @"";
    for (size_t i = 0; i < n; i++) {
        const unsigned char c = (unsigned char)s[i];
        if (c >= 0x20 || c == '\n' || c == '\t') buf[o++] = (char)c;
    }
    buf[o] = '\0';
    NSString *r = [NSString stringWithUTF8String:buf];
    if (!r) r = [NSString stringWithCString:buf encoding:NSISOLatin1StringEncoding];
    free(buf);
    return r ?: @"";
}

/* A growing C string the views' text is formatted into. */
typedef struct {
    char *s;
    size_t len, cap;
} Text;

static BOOL tgrow(Text *t, size_t more)
{
    if (t->len + more + 1 <= t->cap) return YES;
    size_t cap = t->cap ? t->cap : 4096;
    while (cap < t->len + more + 1) cap *= 2;
    char *p = realloc(t->s, cap);
    if (!p) return NO;
    t->s = p;
    t->cap = cap;
    return YES;
}

static void tadd(Text *t, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void tadd(Text *t, const char *fmt, ...)
{
    va_list ap, again;
    va_start(ap, fmt);
    va_copy(again, ap);
    const int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n > 0 && tgrow(t, (size_t)n)) {
        vsnprintf(t->s + t->len, t->cap - t->len, fmt, again);
        t->len += (size_t)n;
    }
    va_end(again);
}

/* The text as a string for a view; the buffer is freed. */
static NSString *tdone(Text *t)
{
    NSString *r = textFrom(t->s ? t->s : "");
    free(t->s);
    t->s = NULL;
    t->len = t->cap = 0;
    return r;
}

static NSString *trimmed(NSString *s)
{
    return [s stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
}

static const char *onOff(int v) { return v ? "on" : "off"; }

static NSFont *monoFont(void)
{
    return [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
}

static NSDictionary *monoAttrs(void)
{
    return @{ NSFontAttributeName: monoFont(), NSForegroundColorAttributeName: NSColor.textColor };
}

/* The accent as a text colour, following the window's appearance. */
static NSColor *accentTextColor(void)
{
    return [NSColor colorWithName:nil dynamicProvider:^NSColor *(NSAppearance *appearance) {
        return A800AccentTextColor(appearance);
    }];
}

static NSColor *colorFromXrgb(uint32_t rgb)
{
    return [NSColor colorWithSRGBRed:((rgb >> 16) & 0xff) / 255.0
                               green:((rgb >> 8) & 0xff) / 255.0
                                blue:(rgb & 0xff) / 255.0
                               alpha:1.0];
}

/* A picture of XRGB pixels (+1: the caller owns it). */
static CGImageRef imageFromXrgb(const uint32_t *px, int w, int h)
{
    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    CFDataRef data = CFDataCreate(NULL, (const UInt8 *)px, (CFIndex)w * h * 4);
    CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
    CGImageRef img = CGImageCreate((size_t)w, (size_t)h, 8, 32, (size_t)w * 4, space,
                                   (CGBitmapInfo)kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little,
                                   provider, NULL, false, kCGRenderingIntentDefault);
    CGDataProviderRelease(provider);
    CFRelease(data);
    CGColorSpaceRelease(space);
    return img;
}

/* $0600, 0x0600, #1536; a bare number is hex, as everywhere in a 6502
 * debugger. */
static BOOL parseNum(NSString *text, long *out)
{
    NSString *t = trimmed(text);
    int base = 16;
    if ([t hasPrefix:@"$"]) t = [t substringFromIndex:1];
    else if ([t.lowercaseString hasPrefix:@"0x"]) t = [t substringFromIndex:2];
    else if ([t hasPrefix:@"#"]) { t = [t substringFromIndex:1]; base = 10; }
    if (t.length == 0) return NO;
    const char *s = t.UTF8String;
    char *end;
    const long v = strtol(s, &end, base);
    if (end == s || *end) return NO;
    *out = v;
    return YES;
}

static NSStackView *hstack(NSArray<NSView *> *views)
{
    NSStackView *s = [NSStackView stackViewWithViews:views];
    s.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    s.spacing = 6;
    [s setHuggingPriority:NSLayoutPriorityDefaultHigh forOrientation:NSLayoutConstraintOrientationVertical];
    [s setContentHuggingPriority:NSLayoutPriorityDefaultHigh forOrientation:NSLayoutConstraintOrientationVertical];
    return s;
}

/* A column whose low-hugging member (a scroll view) takes the height left. */
static NSStackView *vstack(NSArray<NSView *> *views)
{
    NSStackView *s = [NSStackView stackViewWithViews:views];
    s.orientation = NSUserInterfaceLayoutOrientationVertical;
    s.alignment = NSLayoutAttributeLeading;
    s.distribution = NSStackViewDistributionFill;
    s.spacing = 6;
    return s;
}

/* A member of a stack as wide as the stack (less its insets). */
static void fillWidth(NSStackView *stack, NSView *view, CGFloat inset)
{
    [view.widthAnchor constraintEqualToAnchor:stack.widthAnchor constant:-inset].active = YES;
}

/* A member of a row as tall as the row. */
static void fillHeight(NSStackView *stack, NSView *view)
{
    [view.heightAnchor constraintEqualToAnchor:stack.heightAnchor].active = YES;
}

/* A view that takes a stack's spare room along an axis ahead of the rest. */
static void stretchy(NSView *view, NSLayoutConstraintOrientation o)
{
    [view setContentHuggingPriority:NSLayoutPriorityDefaultLow - 1 forOrientation:o];
    if ([view isKindOfClass:[NSStackView class]])
        [(NSStackView *)view setHuggingPriority:NSLayoutPriorityDefaultLow - 1 forOrientation:o];
}

/* A read-only monospaced text view in a scroll view. The text view starts
 * the size of the clip view, which autoresizing then keeps it (it adds the
 * clip view's changes to its own size). */
static NSTextView *monoView(NSScrollView **scrollOut, BOOL wrap)
{
    NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 400, 300)];
    scroll.hasVerticalScroller = YES;
    scroll.hasHorizontalScroller = !wrap;
    const NSSize size = scroll.contentSize;
    NSTextView *view = [[NSTextView alloc] initWithFrame:NSMakeRect(0, 0, size.width, size.height)];
    view.editable = NO;
    view.richText = NO;
    view.font = monoFont();
    view.textColor = NSColor.textColor;
    view.backgroundColor = NSColor.textBackgroundColor;
    view.minSize = NSMakeSize(0, 0);
    view.maxSize = NSMakeSize(FLT_MAX, FLT_MAX);
    view.verticallyResizable = YES;
    if (wrap) {
        view.horizontallyResizable = NO;
        view.autoresizingMask = NSViewWidthSizable;
        view.textContainer.size = NSMakeSize(size.width, FLT_MAX);
        view.textContainer.widthTracksTextView = YES;
    } else {
        view.horizontallyResizable = YES;
        view.textContainer.widthTracksTextView = NO;
        view.textContainer.size = NSMakeSize(FLT_MAX, FLT_MAX);
        view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    }
    scroll.documentView = view;
    [scroll setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationVertical];
    *scrollOut = scroll;
    return view;
}

/* Replaces a view's text where the reader left it (a refresh must not throw
 * a scrolled view back to its top), or, following, at its end. */
static void setText(NSTextView *view, NSString *text, BOOL follow)
{
    if (![view.string isEqualToString:text]) {
        NSScrollView *scroll = view.enclosingScrollView;
        const NSPoint at = scroll ? scroll.contentView.bounds.origin : NSZeroPoint;
        view.string = text;
        if (scroll && !follow) {
            [scroll.contentView scrollToPoint:at];
            [scroll reflectScrolledClipView:scroll.contentView];
        }
    }
    if (follow) [view scrollRangeToVisible:NSMakeRange(view.string.length, 0)];
}

/* The same with attributes (the text is always set again). */
static void setAttributed(NSTextView *view, NSAttributedString *text)
{
    NSScrollView *scroll = view.enclosingScrollView;
    const NSPoint at = scroll ? scroll.contentView.bounds.origin : NSZeroPoint;
    [view.textStorage setAttributedString:text];
    if (scroll) {
        [scroll.contentView scrollToPoint:at];
        [scroll reflectScrolledClipView:scroll.contentView];
    }
}

/* Whether a view's reader is at its end (a log then follows new lines). */
static BOOL atEnd(NSTextView *view)
{
    NSScrollView *scroll = view.enclosingScrollView;
    if (!scroll) return YES;
    return NSMaxY(scroll.contentView.documentVisibleRect) >= NSHeight(view.frame) - 4;
}

/* A800_DEBUGGER_TAB: a page number, or a tab's name or label (any case), or
 * the start of one, or of a word of a label ("pia", "map" ...). */
static int tabFromEnv(void)
{
    const char *env = getenv("A800_DEBUGGER_TAB");
    char tab[64];
    if (!env) return -1;
    while (*env == ' ' || *env == '\t') env++;
    snprintf(tab, sizeof tab, "%s", env);
    size_t len = strlen(tab);
    while (len > 0 && (tab[len - 1] == ' ' || tab[len - 1] == '\t')) tab[--len] = '\0';
    if (len == 0) return -1;
    char *end;
    const long n = strtol(tab, &end, 10);
    if (end != tab && *end == '\0') return n >= 0 && n < TAB_COUNT ? (int)n : -1;
    for (int pass = 0; pass < 3; pass++)
        for (int i = 0; i < TAB_COUNT; i++) {
            if (pass == 0 && strncasecmp(tab, kTabs[i].name, len) == 0) return i;
            if (pass == 1 && strncasecmp(tab, kTabs[i].label, len) == 0) return i;
            if (pass == 2) {
                const char *p = kTabs[i].label;
                while ((p = strchr(p, ' ')) != NULL)
                    if (strncasecmp(tab, ++p, len) == 0) return i;
            }
        }
    return -1;
}

/* A GRAF byte's bits, left to right. */
static void grafBits(Text *t, unsigned v, int bits)
{
    for (int b = bits - 1; b >= 0; b--) tadd(t, "%c", ((v >> b) & 1) ? '#' : '.');
}

static void collisionRow(Text *t, const char *name, uint8_t pf, uint8_t pl, int own)
{
    tadd(t, "  %s        ", name);
    for (int j = 0; j < 4; j++) tadd(t, " %c  ", ((pf >> j) & 1) ? 'X' : '.');
    tadd(t, "   ");
    for (int j = 0; j < 4; j++) tadd(t, "%c  ", j == own ? '-' : ((pl >> j) & 1) ? 'X' : '.');
    tadd(t, "\n");
}

/* The IRQ bits by name, the set ones (or, activeLow, the clear ones). */
static void irqWords(Text *t, uint8_t v, int activeLow)
{
    static const char *const names[8] = {
        "timer 1", "timer 2", "timer 4", "serial output done", "serial output needed",
        "serial input ready", "key", "BREAK key" };
    int any = 0;
    for (int b = 7; b >= 0; b--) {
        const int set = (v >> b) & 1;
        if (activeLow ? !set : set) {
            tadd(t, "%s%s", any ? ", " : " ", names[b]);
            any = 1;
        }
    }
    if (!any) tadd(t, " none");
}

static void pokeyText(Text *t, const a800debug_pokey *p, const char *title)
{
    static const char *const dist[8] = {
        "5-bit + 17-bit poly", "5-bit poly", "5-bit + 4-bit poly", "5-bit poly",
        "17-bit poly", "pure tone", "4-bit poly", "pure tone" };
    /* AUDCTL bit 7 swaps the 17-bit poly for the 9-bit one */
    static const char *const dist9[8] = {
        "5-bit + 9-bit poly", "5-bit poly", "5-bit + 4-bit poly", "5-bit poly",
        "9-bit poly", "pure tone", "4-bit poly", "pure tone" };
    static const char *const audctl[8] = {
        "15 kHz clock", "high-pass 2 by 4", "high-pass 1 by 3", "4 joined to 3 (16-bit)",
        "2 joined to 1 (16-bit)", "3 at 1.79 MHz", "1 at 1.79 MHz", "9-bit poly" };
    tadd(t, "%s\n  Channel  AUDF  AUDC  sound                  volume\n", title);
    for (int ch = 0; ch < 4; ch++) {
        const int c = p->audc[ch], d = (c >> 5) & 7;
        tadd(t, "  %d        $%02X   $%02X   %-22s %2d%s\n", ch + 1, p->audf[ch], c,
             (p->audctl & 0x80) ? dist9[d] : dist[d], c & 15, (c & 0x10) ? " (volume only)" : "");
    }
    tadd(t, "  AUDCTL   $%02X  ", p->audctl);
    int any = 0;
    for (int b = 0; b < 8; b++)
        if (p->audctl & (1 << b)) {
            tadd(t, "%s%s", any ? ", " : " ", audctl[b]);
            any = 1;
        }
    tadd(t, "%s", any ? "\n" : " 64 kHz clock, nothing joined\n");
}

/* A joystick's lines (a PIA nibble, active low). */
static void stickWords(Text *t, int nibble)
{
    static const char *const dirs[4] = { "up", "down", "left", "right" };
    int any = 0;
    for (int b = 0; b < 4; b++)
        if (!((nibble >> b) & 1)) {
            tadd(t, "%s%s", any ? " " : "", dirs[b]);
            any = 1;
        }
    if (!any) tadd(t, "centred");
}

static const char *bpTypeName(int type)
{
    if (type & A800DEBUG_BP_EXEC) return "Execute";
    if ((type & (A800DEBUG_BP_READ | A800DEBUG_BP_WRITE)) == (A800DEBUG_BP_READ | A800DEBUG_BP_WRITE))
        return "Read/Write";
    return (type & A800DEBUG_BP_READ) ? "Read" : "Write";
}

/* ---- views ------------------------------------------------------------------ */

/* A colour register's swatch. */
@interface A800DbgSwatch : NSView
@property (nonatomic, strong) NSColor *color;
@end

@implementation A800DbgSwatch
- (void)setColor:(NSColor *)color
{
    _color = color;
    [self setNeedsDisplay:YES];
}

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    [(_color ?: NSColor.blackColor) setFill];
    NSRectFill(self.bounds);
    [[NSColor colorWithWhite:0.5 alpha:0.8] setStroke];
    [NSBezierPath strokeRect:NSInsetRect(self.bounds, 0.5, 0.5)];
}
@end

/* The 256 colours, 16 hues down and 16 lumas across, nearest-neighbour so
 * the cells stay crisp, with their hex digits along the edges. A click
 * picks one. */
@interface A800DbgPaletteView : NSView
@property (nonatomic) CGImageRef image;
@property (nonatomic, copy) void (^onPick)(int index);
@end

@implementation A800DbgPaletteView
- (void)setImage:(CGImageRef)image
{
    if (_image) CGImageRelease(_image);
    _image = image;
    [self setNeedsDisplay:YES];
}

- (void)dealloc
{
    if (_image) CGImageRelease(_image);
}

- (NSSize)intrinsicContentSize
{
    return NSMakeSize(PAL_AXIS_W + 16 * PAL_CELL_W, PAL_AXIS_H + 16 * PAL_CELL_H);
}

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    const NSRect b = self.bounds;
    const CGFloat gw = b.size.width - PAL_AXIS_W, gh = b.size.height - PAL_AXIS_H;
    const CGFloat cw = gw / 16.0, ch = gh / 16.0;
    NSDictionary *attrs = @{ NSFontAttributeName: [NSFont monospacedSystemFontOfSize:10 weight:NSFontWeightRegular],
                             NSForegroundColorAttributeName: NSColor.labelColor };
    /* lumas along the top, hues down the left (the view is not flipped) */
    for (int i = 0; i < 16; i++) {
        NSString *luma = [NSString stringWithFormat:@"%X", i];
        NSSize s = [luma sizeWithAttributes:attrs];
        [luma drawAtPoint:NSMakePoint(PAL_AXIS_W + i * cw + (cw - s.width) / 2, gh + (PAL_AXIS_H - s.height) / 2)
           withAttributes:attrs];
        NSString *hue = [NSString stringWithFormat:@"%X0", i];
        s = [hue sizeWithAttributes:attrs];
        [hue drawAtPoint:NSMakePoint(PAL_AXIS_W - 6 - s.width, gh - (i + 1) * ch + (ch - s.height) / 2)
          withAttributes:attrs];
    }
    if (!_image) return;
    CGContextRef dc = [[NSGraphicsContext currentContext] CGContext];
    CGContextSetInterpolationQuality(dc, kCGInterpolationNone);
    CGContextDrawImage(dc, CGRectMake(PAL_AXIS_W, 0, gw, gh), _image);
}

- (void)mouseDown:(NSEvent *)event
{
    const NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    const NSRect b = self.bounds;
    const CGFloat gw = b.size.width - PAL_AXIS_W, gh = b.size.height - PAL_AXIS_H;
    if (!self.onPick || gw <= 0 || gh <= 0 || p.x < PAL_AXIS_W || p.y < 0 || p.y >= gh) return;
    const int luma = (int)((p.x - PAL_AXIS_W) * 16 / gw);
    const int hue = (int)((gh - p.y) * 16 / gh);
    if (luma > 15 || hue > 15) return;
    self.onPick((hue << 4) | luma);
}
@end

/* The disassembly: a click reports the line and column clicked (and how
 * many clicks it was), the wheel the rows to move by. */
@interface A800DbgDasmView : NSTextView
@property (nonatomic, copy) void (^onClick)(int line, int column, NSInteger clicks);
@property (nonatomic, copy) void (^onScrollRows)(int rows);
@end

@implementation A800DbgDasmView {
    CGFloat _accum;
}

- (void)mouseDown:(NSEvent *)event
{
    const NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    NSString *text = self.string;
    NSUInteger idx = [self characterIndexForInsertionAtPoint:p], start = 0;
    int line = 0;
    if (idx > text.length) idx = text.length;
    for (NSUInteger i = 0; i < idx; i++)
        if ([text characterAtIndex:i] == '\n') {
            line++;
            start = i + 1;
        }
    if (self.onClick) self.onClick(line, (int)(idx - start), event.clickCount);
}

- (void)scrollWheel:(NSEvent *)event
{
    if (!self.onScrollRows) {
        [super scrollWheel:event];
        return;
    }
    /* a trackpad reports points, a wheel lines; up is positive */
    _accum += event.hasPreciseScrollingDeltas ? -event.scrollingDeltaY / 16.0 : -event.scrollingDeltaY * 3.0;
    const int rows = (int)_accum;
    _accum -= rows;
    if (rows != 0) self.onScrollRows(rows);
}
@end

/* ---- the window ----------------------------------------------------------------- */

@interface A800DebuggerWindow () <NSWindowDelegate, NSTextFieldDelegate, NSTabViewDelegate>
@end

@implementation A800DebuggerWindow {
    a800session *_session;
    a800debug *_dbg;
    NSWindow *_window;
    NSTimer *_tick;
    id _keyMonitor;
    unsigned _seenGen;
    BOOL _wasStopped;
    BOOL _attachedRunning;      /* attached while the machine was running */
    BOOL _placed;
    int _runningTicks;
    unsigned _messageSerial;

    NSTabView *_tabs;
    NSTextField *_status, *_message;
    NSButton *_runBtn, *_brkBtn;

    /* snapshots */
    NSPopover *_snapPopover;
    NSButton *_snapRestore[SNAPSHOTS];
    NSTextField *_snapState[SNAPSHOTS];
    NSTextField *_snapNote;

    /* console */
    NSTextView *_conOut;
    NSTextField *_conIn, *_conPrompt;
    BOOL _conAsm;               /* the A assembler wants its next line */
    NSString *_conPromptText;
    NSMutableArray<NSString *> *_history;
    NSInteger _historyPos;

    /* cpu + memory */
    NSTextField *_reg[6];       /* PC SP A X Y P */
    NSButton *_flag[6];         /* N V D I Z C */
    NSTextField *_beam;
    NSTextView *_mem, *_hist;
    NSTextField *_memGoto, *_memAddr, *_memVal;
    int _memTop;
    int _memMark;               /* the address Go to found; -1 none */

    /* disassembly */
    A800DbgDasmView *_disasm;
    NSButton *_followPc;
    NSTextField *_jump;
    int _disasmTop;
    int _selected;              /* the selected line's address, or -1 */
    uint16_t _lineAddr[DISASM_WINDOW];
    int _lineCount;

    /* antic, gtia */
    NSTextView *_antic, *_dlist, *_gtia;
    A800DbgSwatch *_swatch[SWATCHES];
    NSTextField *_swatchLabel[SWATCHES];
    uint32_t _swatchRgb[SWATCHES];
    A800DbgPaletteView *_palette;
    uint32_t *_palettePx;
    NSTextField *_paletteInfo;

    /* pokey + pia, the memory map */
    NSTextView *_pokey, *_map;

    /* breakpoints */
    NSStackView *_bpList;
    NSString *_bpSig;           /* what the list shows, to rebuild on change */
    NSTextField *_bpBudget, *_bpMsg;
    NSPopUpButton *_bpType;
    NSTextField *_bpStart, *_bpEnd, *_bpCond, *_bpLine;

    /* sio */
    NSTextView *_sio;
    NSTextField *_sioNote;
    int _sioCount;
    a800debug_sio _sioLast;

    /* fujinet */
    NSTextView *_fujiText, *_fujiState;
    NSTextField *_fujiNote;
    int _fujiCount;
    uint64_t _fujiLast;
}

/* ---- lifetime ---------------------------------------------------------------- */

- (instancetype)initWithSession:(a800session *)session
{
    self = [super init];
    if (!self) return nil;
    _session = session;
    _dbg = a800session_debugger(session);
    _history = [NSMutableArray array];
    _conPromptText = @"> ";
    _memMark = -1;
    _disasmTop = 0xE000;
    _selected = -1;
    _sioCount = -1;
    _fujiCount = -1;
    memset(_swatchRgb, 0xFF, sizeof _swatchRgb);
    memset(&_sioLast, 0, sizeof _sioLast);
    _palettePx = calloc((size_t)A800DEBUG_PALETTE_WIDTH * A800DEBUG_PALETTE_HEIGHT, sizeof *_palettePx);
    [self buildWindow];

    __weak A800DebuggerWindow *weakSelf = self;
    _tick = [NSTimer timerWithTimeInterval:0.1 repeats:YES block:^(NSTimer *t) {
        (void)t;
        [weakSelf onTick];
    }];
    [[NSRunLoop mainRunLoop] addTimer:_tick forMode:NSRunLoopCommonModes];

    /* F5/F7/F8/Shift+F8/F9/F12 wherever the focus is in this window, the
     * console's field included */
    _keyMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown
                                                        handler:^NSEvent *(NSEvent *event) {
        A800DebuggerWindow *s = weakSelf;
        if (!s || event.window != s->_window || !s->_window.isVisible) return event;
        return [s handleKey:event] ? nil : event;
    }];
    return self;
}

- (void)dealloc
{
    [_tick invalidate];
    if (_keyMonitor) [NSEvent removeMonitor:_keyMonitor];
    free(_palettePx);
}

- (void)show
{
    if (!_placed) {
        [_window center];
        _placed = YES;
    }
    if (_window.isMiniaturized) [_window deminiaturize:nil];
    [_window makeKeyAndOrderFront:nil];
    [_window.contentView layoutSubtreeIfNeeded];     /* the disassembly's rows */
    [self attachAndRefresh];
    if ([self currentTab] == TAB_CONSOLE) [_window makeFirstResponder:_conIn];
}

- (void)toggle
{
    if (_window.isVisible) {
        [_window close];        /* windowWillClose: detaches */
        return;
    }
    [self show];
}

- (BOOL)isVisible
{
    return _window.isVisible;
}

/* Closed by F12, the close button or the main window: the engine goes away
 * and the machine runs on. The window is kept, and comes back as it was. */
- (void)windowWillClose:(NSNotification *)note
{
    if (note.object != _window) return;
    [_snapPopover close];
    a800debug_detach(_dbg);     /* lets it run; a waiting assembler gives up */
    _attachedRunning = NO;
    [self setPrompt:NO text:nil];
}

- (void)windowDidResize:(NSNotification *)note
{
    if (note.object == _window && [self currentTab] == TAB_DISASM && a800session_is_running(_session))
        [self refreshDisasm];
}

- (void)tabView:(NSTabView *)tabView didSelectTabViewItem:(NSTabViewItem *)item
{
    (void)tabView;
    [item.view layoutSubtreeIfNeeded];
    [self refreshPage:[self currentTab]];
}

- (int)currentTab
{
    return (int)[_tabs indexOfTabViewItem:_tabs.selectedTabViewItem];
}

/* The family's keys: F5 run/stop, F7 step, F8 step over, Shift+F8 step out,
 * F9 frame, F12 close. Held, the steps repeat; the toggles do not. */
- (BOOL)handleKey:(NSEvent *)event
{
    NSString *chars = event.charactersIgnoringModifiers;
    const NSEventModifierFlags mods = event.modifierFlags &
        (NSEventModifierFlagShift | NSEventModifierFlagControl | NSEventModifierFlagOption |
         NSEventModifierFlagCommand);
    if (chars.length != 1) return NO;
    const unichar key = [chars characterAtIndex:0];
    const BOOL shift = mods == NSEventModifierFlagShift;
    if (mods != 0 && !(key == NSF8FunctionKey && shift)) return NO;
    switch (key) {
    case NSF5FunctionKey:
        if (!event.isARepeat) [self toggleRun:nil];
        return YES;
    case NSF7FunctionKey:
        [self step:nil];
        return YES;
    case NSF8FunctionKey:
        if (shift) [self stepOut:nil];
        else [self stepOver:nil];
        return YES;
    case NSF9FunctionKey:
        [self stepFrame:nil];
        return YES;
    case NSF12FunctionKey:
        if (!event.isARepeat) [_window close];
        return YES;
    default:
        return NO;
    }
}

/* ---- construction ------------------------------------------------------------ */

- (NSButton *)button:(NSString *)title action:(SEL)sel
{
    NSButton *b = [NSButton buttonWithTitle:title target:self action:sel];
    [b setRefusesFirstResponder:YES];
    return b;
}

- (NSTextField *)label:(NSString *)text
{
    return [NSTextField labelWithString:text];
}

/* Grey, and the first to give way when its row is short of room. */
- (NSTextField *)hint:(NSString *)text
{
    NSTextField *l = [NSTextField labelWithString:text];
    l.textColor = NSColor.secondaryLabelColor;
    l.lineBreakMode = NSLineBreakByTruncatingTail;
    if (text.length) l.toolTip = text;
    [l setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                forOrientation:NSLayoutConstraintOrientationHorizontal];
    return l;
}

- (NSTextField *)heading:(NSString *)text
{
    NSTextField *l = [NSTextField labelWithString:text];
    l.font = [NSFont boldSystemFontOfSize:[NSFont systemFontSize]];
    return l;
}

- (NSTextField *)field:(NSString *)placeholder width:(CGFloat)width action:(SEL)sel
{
    NSTextField *f = [[NSTextField alloc] init];
    f.placeholderString = placeholder;
    f.font = monoFont();
    f.target = self;
    f.action = sel;
    if (width > 0) [f.widthAnchor constraintEqualToConstant:width].active = YES;
    return f;
}

/* A pull-down button: `title` on its face, `items` underneath (each one's
 * tag is its index). */
- (NSPopUpButton *)pullDown:(NSString *)title items:(NSArray<NSString *> *)items action:(SEL)sel
{
    NSPopUpButton *p = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:YES];
    [p addItemWithTitle:title];
    p.lastItem.tag = -1;
    for (NSUInteger i = 0; i < items.count; i++) {
        [p addItemWithTitle:items[i]];
        p.lastItem.tag = (NSInteger)i;
    }
    p.target = self;
    p.action = sel;
    [p setRefusesFirstResponder:YES];
    return p;
}

/* Snapshot slots 0-3 in a popover: Save, Restore (when saved), and, with
 * FujiNet running, what restoring does to it. */
- (NSButton *)buildSnapshots
{
    NSMutableArray<NSView *> *rows = [NSMutableArray array];
    for (int i = 0; i < SNAPSHOTS; i++) {
        NSButton *save = [self button:@"Save" action:@selector(snapSave:)];
        save.tag = i;
        _snapRestore[i] = [self button:@"Restore" action:@selector(snapRestore:)];
        _snapRestore[i].tag = i;
        _snapState[i] = [self hint:@"empty"];
        _snapState[i].toolTip = nil;
        NSStackView *row = hstack(@[[self label:[NSString stringWithFormat:@"Slot %d", i]], save,
                                    _snapRestore[i], _snapState[i]]);
        row.spacing = 8;
        [rows addObject:row];
    }
    _snapNote = [NSTextField wrappingLabelWithString:kFujinetNote];
    _snapNote.preferredMaxLayoutWidth = 340;
    _snapNote.textColor = accentTextColor();
    [_snapNote.widthAnchor constraintEqualToConstant:340].active = YES;
    NSTextField *hint = [self hint:@"Kept until the app quits."];

    NSMutableArray<NSView *> *views = [NSMutableArray arrayWithObject:[self heading:@"Snapshots"]];
    [views addObjectsFromArray:rows];
    [views addObject:_snapNote];
    [views addObject:hint];
    NSStackView *box = vstack(views);
    box.edgeInsets = NSEdgeInsetsMake(12, 12, 12, 12);
    NSViewController *vc = [[NSViewController alloc] init];
    vc.view = box;
    _snapPopover = [[NSPopover alloc] init];
    _snapPopover.behavior = NSPopoverBehaviorTransient;
    _snapPopover.contentViewController = vc;

    NSButton *b = [self button:@"Snapshots" action:@selector(showSnapshots:)];
    b.toolTip = @"Temporary save states, slots 0-3";
    return b;
}

- (NSView *)buildToolbar
{
    _runBtn = [self button:@"Stop (F5)" action:@selector(toggleRun:)];
    _runBtn.toolTip = @"Run, or stop before the next instruction";
    NSButton *step = [self button:@"Step (F7)" action:@selector(step:)];
    step.toolTip = @"One instruction";
    NSButton *over = [self button:@"Step Over (F8)" action:@selector(stepOver:)];
    over.toolTip = @"Over a JSR, to the instruction after it";
    NSButton *outBtn = [self button:@"Step Out (⇧F8)" action:@selector(stepOut:)];
    outBtn.toolTip = @"On to the RTS or RTI";
    NSButton *frame = [self button:@"Frame (F9)" action:@selector(stepFrame:)];
    frame.toolTip = @"On to the next vertical blank";
    NSButton *runTo = [self button:@"Run to Cursor" action:@selector(runToCursor:)];
    runTo.toolTip = @"On to the line selected in the Disassembly";
    _brkBtn = [self button:@"Break on BRK" action:@selector(breakOnBrk:)];
    [_brkBtn setButtonType:NSButtonTypePushOnPushOff];
    _brkBtn.toolTip = @"Stop at a BRK instead of running it";

    /* the stop reason, in the room the buttons leave */
    _status = [self label:@""];
    _status.alignment = NSTextAlignmentRight;
    _status.textColor = NSColor.secondaryLabelColor;
    _status.lineBreakMode = NSLineBreakByTruncatingTail;
    [_status setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];
    [_status setContentCompressionResistancePriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];

    NSButton *snaps = [self buildSnapshots];
    NSButton *sym = [self button:@"Load Symbols…" action:@selector(loadSymbols:)];
    sym.toolTip = @"Labels from ld65 -Ln, VICE .lbl, MADS .lab or an atari800 label file, beside the OS's own";
    NSMutableArray<NSString *> *saves = [NSMutableArray array];
    for (int i = 0; i < NUM_SAVES; i++) [saves addObject:[kSaveTitles[i] stringByAppendingString:@"…"]];
    NSPopUpButton *save = [self pullDown:@"Save…" items:saves action:@selector(saveFile:)];
    [save.widthAnchor constraintEqualToConstant:84].active = YES;

    NSStackView *bar = hstack(@[_runBtn, step, over, outBtn, frame, runTo, _brkBtn, _status, snaps, sym, save]);
    bar.distribution = NSStackViewDistributionFill;
    return bar;
}

- (NSView *)buildConsole
{
    NSScrollView *scroll;
    _conOut = monoView(&scroll, YES);
    _conOut.string = @"atari800's monitor. Type HELP for its commands; Tab completes them, Up/Down recall. "
                     @"G, R, O and CONT let the machine go; A assembles a line at a time.\n";
    _conPrompt = [self label:_conPromptText];
    _conPrompt.font = monoFont();
    [_conPrompt setContentHuggingPriority:NSLayoutPriorityDefaultHigh
                           forOrientation:NSLayoutConstraintOrientationHorizontal];
    /* Enter, Tab and Up/Down come through the delegate */
    _conIn = [[NSTextField alloc] init];
    _conIn.font = monoFont();
    _conIn.delegate = self;
    [_conIn setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];
    NSStackView *row = hstack(@[_conPrompt, _conIn]);
    row.distribution = NSStackViewDistributionFill;
    [self setPrompt:NO text:nil];

    NSStackView *v = vstack(@[scroll, row]);
    fillWidth(v, scroll, 0);
    fillWidth(v, row, 0);
    return v;
}

- (NSView *)buildCpu
{
    static NSString *const names[6] = { @"PC", @"SP", @"A", @"X", @"Y", @"P" };
    static NSString *const fnames[6] = { @"N", @"V", @"D", @"I", @"Z", @"C" };
    NSMutableArray<NSView *> *regs = [NSMutableArray array];
    for (int i = 0; i < 6; i++) {
        _reg[i] = [self field:@"" width:60 action:@selector(applyRegister:)];
        _reg[i].tag = i;
        _reg[i].toolTip = @"Enter sets it";
        [regs addObject:[self hint:names[i]]];
        [regs addObject:_reg[i]];
    }
    NSMutableArray<NSView *> *flags = [NSMutableArray array];
    for (int i = 0; i < 6; i++) {
        _flag[i] = [NSButton checkboxWithTitle:fnames[i] target:self action:@selector(flagClicked:)];
        _flag[i].tag = i;
        [flags addObject:_flag[i]];
    }
    _beam = [self hint:@""];
    _beam.selectable = YES;
    [flags addObject:_beam];
    NSStackView *flagRow = hstack(flags);
    [flagRow setCustomSpacing:16 afterView:_flag[5]];

    /* the dump beside the last instructions */
    NSScrollView *memScroll, *histScroll;
    _mem = monoView(&memScroll, NO);
    _hist = monoView(&histScroll, NO);
    NSStackView *left = vstack(@[[self heading:@"Memory, as the CPU sees it (reads have no side effects)"], memScroll]);
    fillWidth(left, memScroll, 0);
    NSStackView *right = vstack(@[[self heading:@"Last instructions"], histScroll]);
    fillWidth(right, histScroll, 0);
    NSStackView *split = hstack(@[left, right]);
    split.distribution = NSStackViewDistributionFill;
    stretchy(split, NSLayoutConstraintOrientationVertical);
    stretchy(left, NSLayoutConstraintOrientationHorizontal);
    [left.widthAnchor constraintGreaterThanOrEqualToConstant:520].active = YES;
    [right.widthAnchor constraintEqualToConstant:340].active = YES;
    fillHeight(split, left);
    fillHeight(split, right);

    /* Go to, a page back or on, and a write */
    _memGoto = [self field:@"$0600 / label" width:130 action:@selector(memGoto:)];
    NSButton *back = [self button:@"▲" action:@selector(memPage:)];
    back.tag = -1;
    back.toolTip = @"Back 4K";
    NSButton *on = [self button:@"▼" action:@selector(memPage:)];
    on.tag = 1;
    on.toolTip = @"On 4K";
    _memAddr = [self field:@"$0600" width:110 action:@selector(memWrite:)];
    _memVal = [self field:@"$00" width:56 action:@selector(memWrite:)];
    NSStackView *nav = hstack(@[[self label:@"Go to"], _memGoto, back, on, [self label:@"Write"], _memAddr,
                                [self label:@"value"], _memVal,
                                [self hint:@"a CPU write: hardware registers take it as the 6502's would"]]);
    [nav setCustomSpacing:16 afterView:on];

    NSStackView *v = vstack(@[hstack(regs), flagRow, split, nav]);
    fillWidth(v, flagRow, 0);
    fillWidth(v, split, 0);
    fillWidth(v, nav, 0);
    return v;
}

- (NSView *)buildDisasm
{
    _followPc = [NSButton checkboxWithTitle:@"Follow PC" target:self action:@selector(followToggled:)];
    _followPc.state = NSControlStateValueOn;
    _jump = [self field:@"$E456 or CIOV" width:150 action:@selector(jumpTo:)];
    NSStackView *row = hstack(@[_followPc, [self label:@"Jump to"], _jump,
                                [self hint:@"Click a line to select it (Run to Cursor); click its left edge or "
                                           @"double-click it for a breakpoint (* on, o off); scroll to browse"]]);
    [row setCustomSpacing:12 afterView:_followPc];
    [row setCustomSpacing:12 afterView:_jump];

    /* no scrollbar: the wheel moves by instructions, as many lines as fit */
    NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 600, 600)];
    scroll.hasVerticalScroller = NO;
    scroll.hasHorizontalScroller = YES;
    [scroll setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationVertical];
    const NSSize size = scroll.contentSize;
    _disasm = [[A800DbgDasmView alloc] initWithFrame:NSMakeRect(0, 0, size.width, size.height)];
    _disasm.minSize = NSMakeSize(0, 0);
    _disasm.editable = NO;
    _disasm.richText = NO;
    _disasm.selectable = NO;
    _disasm.font = monoFont();
    _disasm.backgroundColor = NSColor.textBackgroundColor;
    _disasm.verticallyResizable = YES;
    _disasm.horizontallyResizable = YES;
    _disasm.maxSize = NSMakeSize(FLT_MAX, FLT_MAX);
    _disasm.textContainer.widthTracksTextView = NO;
    _disasm.textContainer.size = NSMakeSize(FLT_MAX, FLT_MAX);
    _disasm.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    __weak A800DebuggerWindow *weakSelf = self;
    /* A click selects a line (Run to Cursor); a click on its left edge, or
     * a double-click, toggles its breakpoint. */
    _disasm.onClick = ^(int line, int column, NSInteger clicks) {
        A800DebuggerWindow *s = weakSelf;
        if (!s || line < 0 || line >= s->_lineCount) return;
        const BOOL gutter = column <= 2;
        s->_selected = s->_lineAddr[line];
        if (gutter ? clicks == 1 : clicks == 2) [s toggleBreakpointAt:s->_lineAddr[line]];
        else [s refreshDisasm];
    };
    _disasm.onScrollRows = ^(int rows) {
        A800DebuggerWindow *s = weakSelf;
        if (!s || !a800session_is_running(s->_session)) return;
        s->_followPc.state = NSControlStateValueOff;
        s->_disasmTop = a800debug_row_address(s->_dbg, (uint16_t)s->_disasmTop, rows);
        [s refreshDisasm];
    };
    scroll.documentView = _disasm;

    NSStackView *v = vstack(@[row, scroll]);
    fillWidth(v, row, 0);
    fillWidth(v, scroll, 0);
    return v;
}

- (NSView *)buildAntic
{
    NSScrollView *regScroll, *dlScroll;
    _antic = monoView(&regScroll, NO);
    _dlist = monoView(&dlScroll, NO);
    NSStackView *left = vstack(@[[self heading:@"Registers"], regScroll]);
    fillWidth(left, regScroll, 0);
    NSStackView *right = vstack(@[[self heading:@"Display list"], dlScroll]);
    fillWidth(right, dlScroll, 0);
    NSStackView *h = hstack(@[left, right]);
    h.distribution = NSStackViewDistributionFill;
    h.spacing = 12;
    [right.widthAnchor constraintEqualToAnchor:h.widthAnchor multiplier:0.4 constant:-6].active = YES;
    fillHeight(h, left);
    fillHeight(h, right);
    return h;
}

- (NSView *)buildGtia
{
    static const char *const names[SWATCHES] = {
        "COLPM0", "COLPM1", "COLPM2", "COLPM3", "COLPF0", "COLPF1", "COLPF2", "COLPF3", "COLBK" };
    NSScrollView *scroll;
    _gtia = monoView(&scroll, NO);

    /* the colour registers: COLPM0-3, COLPF0-3, then COLBK */
    NSMutableArray<NSView *> *right = [NSMutableArray arrayWithObject:[self heading:@"Colour registers"]];
    for (int r = 0; r * 4 < SWATCHES; r++) {
        NSMutableArray<NSView *> *cells = [NSMutableArray array];
        for (int i = r * 4; i < SWATCHES && i < r * 4 + 4; i++) {
            _swatch[i] = [[A800DbgSwatch alloc] initWithFrame:NSMakeRect(0, 0, 22, 18)];
            [_swatch[i].widthAnchor constraintEqualToConstant:22].active = YES;
            [_swatch[i].heightAnchor constraintEqualToConstant:18].active = YES;
            _swatchLabel[i] = [self label:[NSString stringWithFormat:@"%-6s $00", names[i]]];
            _swatchLabel[i].font = monoFont();
            [cells addObject:hstack(@[_swatch[i], _swatchLabel[i]])];
        }
        NSStackView *row = hstack(cells);
        row.spacing = 12;
        [right addObject:row];
    }

    _palette = [[A800DbgPaletteView alloc] initWithFrame:NSMakeRect(0, 0, PAL_AXIS_W + 16 * PAL_CELL_W,
                                                                 PAL_AXIS_H + 16 * PAL_CELL_H)];
    [_palette.widthAnchor constraintEqualToConstant:PAL_AXIS_W + 16 * PAL_CELL_W].active = YES;
    [_palette.heightAnchor constraintEqualToConstant:PAL_AXIS_H + 16 * PAL_CELL_H].active = YES;
    __weak A800DebuggerWindow *weakSelf = self;
    _palette.onPick = ^(int index) {
        A800DebuggerWindow *s = weakSelf;
        if (!s) return;
        s->_paletteInfo.stringValue = [NSString stringWithFormat:@"Colour $%02X: hue %d, luma %d, #%06X", index,
                                       index >> 4, index & 15, a800debug_color(s->_dbg, (uint8_t)index) & 0xFFFFFF];
    };
    _paletteInfo = [self hint:@"Click a colour for its value"];
    _paletteInfo.selectable = YES;
    [right addObject:[self heading:@"Palette: hues down, lumas across"]];
    [right addObject:_palette];
    [right addObject:_paletteInfo];
    NSStackView *col = vstack(right);
    [col setCustomSpacing:14 afterView:right[right.count - 4]];
    [col setHuggingPriority:NSLayoutPriorityDefaultHigh forOrientation:NSLayoutConstraintOrientationHorizontal];
    [col setContentHuggingPriority:NSLayoutPriorityDefaultHigh forOrientation:NSLayoutConstraintOrientationHorizontal];

    NSStackView *h = hstack(@[scroll, col]);
    h.alignment = NSLayoutAttributeTop;
    h.distribution = NSStackViewDistributionFill;
    h.spacing = 12;
    stretchy(scroll, NSLayoutConstraintOrientationHorizontal);
    fillHeight(h, scroll);
    return h;
}

- (NSView *)buildBreaks
{
    _bpBudget = [self hint:@""];
    _bpBudget.toolTip = @"atari800 checks up to 20 entries before each instruction: a breakpoint takes one "
                        @"(two for a range), each condition one more, and one joins each to the next";
    [_bpBudget setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];
    NSStackView *top = hstack(@[_bpBudget, [self button:@"Clear All" action:@selector(bpClear:)]]);
    top.distribution = NSStackViewDistributionFill;

    /* The rows scroll: a session can collect more breakpoints than the tab
     * is tall. The stack is the scroll view's document, pinned to the top
     * and to the clip view's width so rows keep the tab's width. */
    _bpList = vstack(@[]);
    _bpList.distribution = NSStackViewDistributionGravityAreas;
    _bpList.translatesAutoresizingMaskIntoConstraints = NO;
    NSScrollView *bpScroll = [[NSScrollView alloc] init];
    bpScroll.hasVerticalScroller = YES;
    bpScroll.drawsBackground = NO;
    bpScroll.documentView = _bpList;
    NSClipView *clip = bpScroll.contentView;
    [_bpList.topAnchor constraintEqualToAnchor:clip.topAnchor].active = YES;
    [_bpList.leadingAnchor constraintEqualToAnchor:clip.leadingAnchor].active = YES;
    [_bpList.widthAnchor constraintEqualToAnchor:clip.widthAnchor].active = YES;
    [bpScroll setContentHuggingPriority:NSLayoutPriorityDefaultLow - 1
                         forOrientation:NSLayoutConstraintOrientationVertical];

    _bpType = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    [_bpType addItemsWithTitles:@[@"Execute", @"Read", @"Write", @"Read/Write"]];
    _bpStart = [self field:@"$0600 / label" width:130 action:@selector(bpAdd:)];
    _bpEnd = [self field:@"(end)" width:90 action:@selector(bpAdd:)];
    _bpCond = [self field:@"condition: A=$10 && X<4 && MEM[$D01F]=6 — optional" width:0 action:@selector(bpAdd:)];
    [_bpCond setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];
    NSStackView *row = hstack(@[_bpType, _bpStart, _bpEnd, _bpCond, [self button:@"Add" action:@selector(bpAdd:)]]);
    row.distribution = NSStackViewDistributionFill;

    _bpLine = [self field:@"100" width:60 action:@selector(bpLine:)];
    NSStackView *line = hstack(@[[self label:@"Break at scanline"], _bpLine,
                                 [self button:@"Set" action:@selector(bpLine:)],
                                 [self hint:@"atari800's BLINE: the next Run (or step) stops when the beam "
                                            @"reaches it, once"]]);

    _bpMsg = [self label:@""];
    _bpMsg.textColor = accentTextColor();
    _bpMsg.lineBreakMode = NSLineBreakByTruncatingTail;
    [_bpMsg setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                     forOrientation:NSLayoutConstraintOrientationHorizontal];

    NSStackView *v = vstack(@[top, bpScroll, row, line, _bpMsg]);
    for (NSView *each in @[top, bpScroll, row, line, _bpMsg]) fillWidth(v, each, 0);
    return v;
}

- (NSView *)buildSio
{
    _sioNote = [self hint:@""];
    [_sioNote setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];
    NSStackView *row = hstack(@[_sioNote, [self button:@"Clear" action:@selector(sioClear:)]]);
    row.distribution = NSStackViewDistributionFill;
    NSScrollView *scroll;
    _sio = monoView(&scroll, NO);
    NSStackView *v = vstack(@[row, scroll]);
    fillWidth(v, row, 0);
    fillWidth(v, scroll, 0);
    return v;
}

- (NSView *)buildFujinet
{
    _fujiNote = [self hint:@""];
    [_fujiNote setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];
    NSStackView *row = hstack(@[_fujiNote, [self button:@"Clear" action:@selector(fujiClear:)]]);
    row.distribution = NSStackViewDistributionFill;
    NSScrollView *fujiScroll, *stateScroll;
    _fujiText = monoView(&fujiScroll, NO);
    _fujiState = monoView(&stateScroll, NO);
    NSStackView *left = vstack(@[[self heading:@"SIO packets over NetSIO"], row, fujiScroll]);
    fillWidth(left, row, 0);
    fillWidth(left, fujiScroll, 0);
    NSStackView *right = vstack(@[[self heading:@"NetSIO and POKEY's serial port"], stateScroll]);
    fillWidth(right, stateScroll, 0);
    NSStackView *split = hstack(@[left, right]);
    split.distribution = NSStackViewDistributionFill;
    stretchy(left, NSLayoutConstraintOrientationHorizontal);
    [left.widthAnchor constraintGreaterThanOrEqualToConstant:520].active = YES;
    [right.widthAnchor constraintEqualToConstant:340].active = YES;
    fillHeight(split, left);
    fillHeight(split, right);
    return split;
}

- (void)buildWindow
{
    _window = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 1250, 800)
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                            NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _window.title = @"Debugger";
    _window.releasedWhenClosed = NO;
    _window.minSize = NSMakeSize(1080, 640);    /* the toolbar, and the GTIA tab's palette */
    _window.tabbingMode = NSWindowTabbingModeDisallowed;
    _window.delegate = self;

    NSView *toolbar = [self buildToolbar];
    NSScrollView *pokeyScroll, *mapScroll;
    _pokey = monoView(&pokeyScroll, NO);
    _map = monoView(&mapScroll, NO);
    NSView *pages[TAB_COUNT];
    pages[TAB_CONSOLE] = [self buildConsole];
    pages[TAB_CPU] = [self buildCpu];
    pages[TAB_DISASM] = [self buildDisasm];
    pages[TAB_ANTIC] = [self buildAntic];
    pages[TAB_GTIA] = [self buildGtia];
    pages[TAB_POKEY] = pokeyScroll;
    pages[TAB_MAP] = mapScroll;
    pages[TAB_BREAKS] = [self buildBreaks];
    pages[TAB_SIO] = [self buildSio];
    pages[TAB_FUJINET] = [self buildFujinet];

    _tabs = [[NSTabView alloc] init];
    for (int i = 0; i < TAB_COUNT; i++) {
        NSTabViewItem *item = [[NSTabViewItem alloc] initWithIdentifier:@(kTabs[i].name)];
        item.label = @(kTabs[i].label);
        NSView *content = pages[i];
        NSView *holder = [[NSView alloc] init];
        [holder addSubview:content];
        content.translatesAutoresizingMaskIntoConstraints = NO;
        [content.leadingAnchor constraintEqualToAnchor:holder.leadingAnchor constant:6].active = YES;
        [content.trailingAnchor constraintEqualToAnchor:holder.trailingAnchor constant:-6].active = YES;
        [content.topAnchor constraintEqualToAnchor:holder.topAnchor constant:6].active = YES;
        [content.bottomAnchor constraintEqualToAnchor:holder.bottomAnchor constant:-6].active = YES;
        item.view = holder;
        [_tabs addTabViewItem:item];
    }
    const int tab = tabFromEnv();
    if (tab >= 0) [_tabs selectTabViewItemAtIndex:tab];
    _tabs.delegate = self;
    [_tabs setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationVertical];

    /* messages (Load Symbols, Save, the snapshots ...) along the foot */
    _message = [self hint:@""];

    NSStackView *root = vstack(@[toolbar, _tabs, _message]);
    root.edgeInsets = NSEdgeInsetsMake(8, 8, 8, 8);
    fillWidth(root, toolbar, 16);
    fillWidth(root, _tabs, 16);
    fillWidth(root, _message, 16);
    _window.contentView = root;
}

/* ---- refreshing ------------------------------------------------------------------ */

- (void)refreshStatus
{
    const BOOL running = a800session_is_running(_session) != 0;
    const BOOL stopped = running && a800debug_is_stopped(_dbg);
    NSString *text;
    if (!running) {
        text = @"The Atari is not running";
    } else if (stopped) {
        char reason[160] = "";
        int addr = -1;
        a800debug_stop_reason(_dbg, reason, (int)sizeof reason, &addr);
        text = reason[0] ? [@"Stopped: " stringByAppendingString:textFrom(reason)] : @"Stopped";
    } else {
        text = @"Running";
    }
    _status.stringValue = text;
    _status.toolTip = text;
    _runBtn.title = stopped ? @"Run (F5)" : @"Stop (F5)";
    _runBtn.bezelColor = stopped ? A800AccentColor() : nil;
    _brkBtn.state = (running && a800debug_breaks_on_brk(_dbg)) ? NSControlStateValueOn : NSControlStateValueOff;
}

- (void)refreshCpu
{
    a800debug_cpu c;
    a800debug_cpu_get(_dbg, &c);
    /* a value being typed stays while the machine runs */
    const BOOL running = !a800debug_is_stopped(_dbg);
    const int vals[6] = { c.pc, c.sp, c.a, c.x, c.y, c.p };
    for (int i = 0; i < 6; i++)
        if (!(running && _reg[i].currentEditor))
            _reg[i].stringValue = i == 0 ? [NSString stringWithFormat:@"%04X", (unsigned)vals[i] & 0xFFFF]
                                         : [NSString stringWithFormat:@"%02X", (unsigned)vals[i] & 0xFF];
    const int flags[6] = { c.n, c.v, c.d, c.i, c.z, c.c };
    for (int i = 0; i < 6; i++) _flag[i].state = flags[i] ? NSControlStateValueOn : NSControlStateValueOff;
    _beam.stringValue = [NSString stringWithFormat:@"scanline %d   cycle %d   frame %u", c.scanline, c.cycle, c.frame];
}

- (void)refreshMem
{
    static uint8_t mem[MEM_ROWS * 16];
    Text t = { NULL, 0, 0 };
    size_t markAt = 0, markLen = 0;
    int markRow = -1;
    if (_memMark >= 0 && ((_memMark - _memTop) & 0xFFFF) < MEM_ROWS * 16)
        markRow = ((_memMark - _memTop) & 0xFFFF) / 16;
    a800debug_read(_dbg, (uint16_t)_memTop, mem, (int)sizeof mem);
    tadd(&t, "       ");
    for (int col = 0; col < 16; col++) tadd(&t, col == 8 ? " %X  " : "%X  ", col);
    tadd(&t, "\n");
    for (int row = 0; row < MEM_ROWS; row++) {
        const uint8_t *r = mem + row * 16;
        const size_t start = t.len;
        tadd(&t, "$%04X: ", (_memTop + row * 16) & 0xFFFF);
        for (int col = 0; col < 16; col++) tadd(&t, col == 8 ? " %02X " : "%02X ", r[col]);
        tadd(&t, " ");
        for (int col = 0; col < 16; col++) tadd(&t, "%c", (r[col] >= 0x20 && r[col] < 0x7F) ? r[col] : '.');
        if (row == markRow) {
            markAt = start;
            markLen = t.len - start;
        }
        if (row < MEM_ROWS - 1) tadd(&t, "\n");
    }
    /* the row Go to found stays marked across refreshes (the dump is ASCII:
     * byte offsets are character offsets) */
    NSMutableAttributedString *as = [[NSMutableAttributedString alloc] initWithString:tdone(&t)
                                                                           attributes:monoAttrs()];
    if (markLen && markAt + markLen <= as.length)
        [as addAttributes:@{ NSBackgroundColorAttributeName: A800AccentColor(),
                             NSForegroundColorAttributeName: NSColor.whiteColor }
                    range:NSMakeRange(markAt, markLen)];
    setAttributed(_mem, as);
}

/* A dump's line (the header is line 0) brought to about a third of the way
 * down. */
- (void)scrollMemToLine:(int)line
{
    NSString *s = _mem.string;
    NSUInteger at = 0;
    for (int l = 0; l < line && at < s.length; l++) {
        const NSRange nl = [s rangeOfString:@"\n" options:0 range:NSMakeRange(at, s.length - at)];
        if (nl.location == NSNotFound) break;
        at = nl.location + 1;
    }
    if (at >= s.length) return;
    NSLayoutManager *lm = _mem.layoutManager;
    NSClipView *clip = _mem.enclosingScrollView.contentView;
    if (!lm || !clip) return;
    [lm ensureLayoutForTextContainer:_mem.textContainer];
    const NSRange glyphs = [lm glyphRangeForCharacterRange:NSMakeRange(at, 1) actualCharacterRange:NULL];
    const NSRect r = [lm boundingRectForGlyphRange:glyphs inTextContainer:_mem.textContainer];
    CGFloat y = r.origin.y + _mem.textContainerOrigin.y - clip.bounds.size.height * 0.3;
    const CGFloat maxY = NSHeight(_mem.frame) - clip.bounds.size.height;
    if (y > maxY) y = maxY;
    if (y < 0) y = 0;
    [clip scrollToPoint:NSMakePoint(clip.bounds.origin.x, y)];
    [_mem.enclosingScrollView reflectScrolledClipView:clip];
}

- (void)refreshHistory
{
    static a800debug_history h[HISTORY_MAX];
    const int n = a800debug_history_get(_dbg, h, HISTORY_MAX);
    Text t = { NULL, 0, 0 };
    tadd(&t, "PC    line  cyc  instruction");
    for (int i = 0; i < n; i++)
        tadd(&t, "\n%04X   %3d  %3d  %s", h[i].pc, h[i].scanline, h[i].cycle, h[i].disasm);
    setText(_hist, tdone(&t), YES);
}

/* The disassembly lines that fit the view. */
- (int)visibleRows
{
    NSClipView *clip = _disasm.enclosingScrollView.contentView;
    const CGFloat line = [_disasm.layoutManager defaultLineHeightForFont:monoFont()];
    const CGFloat height = clip.bounds.size.height - 2 * _disasm.textContainerInset.height;
    if (!clip || line <= 0 || height <= 0) return 32;
    const int rows = (int)(height / line);
    return rows < 4 ? 4 : (rows > DISASM_WINDOW ? DISASM_WINDOW : rows);
}

- (void)refreshDisasm
{
    static a800debug_line lines[DISASM_WINDOW];
    const int rows = [self visibleRows];

    /* Following, the PC stays put while it shows, else sits a third of the
     * way down. */
    if (_followPc.state == NSControlStateValueOn) {
        a800debug_cpu c;
        a800debug_cpu_get(_dbg, &c);
        BOOL shows = NO;
        for (int i = 1; i < _lineCount && i < rows - 2; i++)
            if (_lineAddr[i] == c.pc) shows = YES;
        if (!shows) _disasmTop = a800debug_row_address(_dbg, (uint16_t)c.pc, -(rows / 3));
    }

    int pcLine = -1;
    const int n = a800debug_disassemble(_dbg, (uint16_t)_disasmTop, lines,
                                        rows + 1 < DISASM_WINDOW ? rows + 1 : DISASM_WINDOW, &pcLine);
    /* the label column is as wide as the longest label showing, within reason */
    int labelWidth = 0;
    for (int i = 0; i < n; i++) {
        const int len = (int)strlen(lines[i].label);
        if (len > labelWidth) labelWidth = len;
    }
    if (labelWidth > 16) labelWidth = 16;

    NSMutableArray<NSString *> *texts = [NSMutableArray array];
    NSUInteger width = 0;
    _lineCount = 0;
    for (int i = 0; i < n; i++) {
        const a800debug_line *l = &lines[i];
        Text t = { NULL, 0, 0 };
        tadd(&t, "%c%c %04X  %-8s  ", l->has_breakpoint == 2 ? 'o' : l->has_breakpoint ? '*' : ' ',
             l->is_pc ? '>' : ' ', l->address, l->bytes);
        if (labelWidth) tadd(&t, "%-*.*s ", labelWidth, labelWidth, l->label);
        tadd(&t, "%-16s%s%s", l->disasm, l->comment[0] ? " ; " : "", l->comment);
        NSString *s = tdone(&t);
        if (s.length > width) width = s.length;
        [texts addObject:s];
        _lineAddr[i] = l->address;
        _lineCount = i + 1;
    }

    /* each line padded to the widest, so the PC's bar is one width */
    NSMutableString *all = [NSMutableString string];
    NSRange ranges[DISASM_WINDOW];
    for (int i = 0; i < n; i++) {
        NSString *padded = [texts[(NSUInteger)i] stringByPaddingToLength:width withString:@" " startingAtIndex:0];
        ranges[i] = NSMakeRange(all.length, padded.length);
        [all appendString:padded];
        if (i < n - 1) [all appendString:@"\n"];
    }
    if (n == 0) [all appendString:@"(no disassembly)"];

    NSMutableAttributedString *as = [[NSMutableAttributedString alloc] initWithString:all attributes:monoAttrs()];
    NSFont *bold = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightBold];
    for (int i = 0; i < n; i++) {
        /* the gutter: a breakpoint red, a disabled one grey */
        if (lines[i].has_breakpoint == 2)
            [as addAttribute:NSForegroundColorAttributeName value:NSColor.systemGrayColor
                       range:NSMakeRange(ranges[i].location, 1)];
        else if (lines[i].has_breakpoint)
            [as addAttributes:@{ NSForegroundColorAttributeName: NSColor.systemRedColor, NSFontAttributeName: bold }
                        range:NSMakeRange(ranges[i].location, 1)];
        /* the selected line underlined */
        if ((int)lines[i].address == _selected && i != pcLine)
            [as addAttributes:@{ NSUnderlineStyleAttributeName: @(NSUnderlineStyleSingle), NSFontAttributeName: bold }
                        range:ranges[i]];
    }
    /* the PC line in the accent colour, last so its white text wins */
    if (pcLine >= 0 && pcLine < n) {
        [as addAttributes:@{ NSBackgroundColorAttributeName: A800AccentColor(),
                             NSForegroundColorAttributeName: NSColor.whiteColor }
                    range:ranges[pcLine]];
        if ((int)lines[pcLine].address == _selected)
            [as addAttribute:NSUnderlineStyleAttributeName value:@(NSUnderlineStyleSingle) range:ranges[pcLine]];
    }
    [_disasm.textStorage setAttributedString:as];
    NSScrollView *scroll = _disasm.enclosingScrollView;
    [scroll.contentView scrollToPoint:NSMakePoint(scroll.contentView.bounds.origin.x, 0)];
    [scroll reflectScrolledClipView:scroll.contentView];
}

- (void)refreshAntic
{
    static const char *const widths[4] = {
        "none", "narrow (32 bytes a line)", "normal (40 bytes a line)", "wide (48 bytes a line)" };
    static char dl[32768];
    a800debug_antic a;
    a800debug_antic_get(_dbg, &a);
    const int pm = a.pm_single_line ? (a.pmbase & 0xF8) << 8 : (a.pmbase & 0xFC) << 8;
    Text t = { NULL, 0, 0 };
    tadd(&t,
        "DMACTL  $%02X   playfield %s\n"
        "              display list DMA %s\n"
        "              players %s, missiles %s, %s-line resolution\n"
        "CHACTL  $%02X   inverse characters %s%s%s\n"
        "HSCROL  $%02X\n"
        "VSCROL  $%02X\n"
        "PMBASE  $%02X   players and missiles at $%04X\n"
        "CHBASE  $%02X   character set at $%04X\n"
        "NMIEN   $%02X   display list interrupts %s, vertical blank %s\n"
        "NMIST   $%02X  %s%s%s%s\n"
        "DLIST   $%04X (the list this frame began with)\n\n"
        "Beam    scanline %d, cycle %d, VCOUNT $%02X\n",
        a.dmactl, widths[a.playfield_width & 3], onOff(a.dl_dma),
        onOff(a.pm_dma), onOff(a.missile_dma), a.pm_single_line ? "single" : "double",
        a.chactl, (a.chactl & 2) ? "inverse" : "normal",
        (a.chactl & 1) ? ", blanked" : "", (a.chactl & 4) ? ", upside down" : "",
        a.hscrol, a.vscrol, a.pmbase, pm, a.chbase, a.chbase << 8,
        a.nmien, onOff(a.nmien & 0x80), onOff(a.nmien & 0x40),
        a.nmist, (a.nmist & 0x80) ? " DLI" : "", (a.nmist & 0x40) ? " VBI" : "",
        (a.nmist & 0x20) ? " RESET" : "", (a.nmist & 0xE0) ? " (last interrupts)" : " -",
        a.dlist, a.scanline, a.cycle, a.vcount & 0xFF);
    setText(_antic, tdone(&t), NO);

    NSString *d;
    if (a800debug_dlist_text(_dbg, dl, (int)sizeof dl) <= 0)
        d = @"(no display list)";
    else if (!a.dl_dma)
        d = [@"(display list DMA is off: ANTIC is not reading this)\n\n" stringByAppendingString:textFrom(dl)];
    else
        d = textFrom(dl);
    setText(_dlist, d, NO);
}

- (void)refreshGtia
{
    static const char *const sizes[4] = { "normal", "double", "normal", "quad" };
    static const char *const modes[4] = {
        "normal (ANTIC's colours)", "9 (16 lumas of one hue)", "10 (9 colours)", "11 (16 hues)" };
    static const char *const names[SWATCHES] = {
        "COLPM0", "COLPM1", "COLPM2", "COLPM3", "COLPF0", "COLPF1", "COLPF2", "COLPF3", "COLBK" };
    a800debug_gtia g;
    a800debug_gtia_get(_dbg, &g);
    Text t = { NULL, 0, 0 };
    tadd(&t, "Player  HPOS  SIZE    GRAF       COLPM\n");
    for (int i = 0; i < 4; i++) {
        tadd(&t, "P%d      $%02X   %-7s ", i, g.hposp[i], sizes[g.sizep[i] & 3]);
        grafBits(&t, g.grafp[i], 8);
        tadd(&t, "   $%02X\n", g.colpm[i]);
    }
    tadd(&t, "\nMissile HPOS  SIZE    GRAF\n");
    for (int i = 0; i < 4; i++) {
        tadd(&t, "M%d      $%02X   %-7s ", i, g.hposm[i], sizes[(g.sizem >> (i * 2)) & 3]);
        grafBits(&t, (g.grafm >> (i * 2)) & 3, 2);
        tadd(&t, "\n");
    }
    const char *prio;
    switch (g.prior & 0x0F) {
    case 1: prio = "players > playfield"; break;
    case 2: prio = "P0-1 > playfield > P2-3"; break;
    case 4: prio = "playfield > players"; break;
    case 8: prio = "PF0-1 > players > PF2-3"; break;
    case 0: prio = "none set (overlaps mix)"; break;
    default: prio = "several set (overlaps mix)"; break;
    }
    tadd(&t,
        "\nCOLPF0-3 $%02X $%02X $%02X $%02X   COLBK $%02X\n"
        "PRIOR   $%02X   priority %s\n"
        "              fifth player %s, multicolour players %s\n"
        "              GTIA mode %s\n"
        "VDELAY  $%02X   GRACTL $%02X (missiles %s, players %s, trigger latch %s)\n"
        "CONSOL  $%02X  %s%s%s%s\n"
        "TRIG0-3 %d %d %d %d (0 = pressed)\n\n",
        g.colpf[0], g.colpf[1], g.colpf[2], g.colpf[3], g.colbk,
        g.prior, prio, onOff(g.prior & 0x10), onOff(g.prior & 0x20), modes[g.prior >> 6],
        g.vdelay, g.gractl, onOff(g.gractl & 1), onOff(g.gractl & 2), onOff(g.gractl & 4),
        g.consol, (g.consol & 1) ? "" : " START", (g.consol & 2) ? "" : " SELECT",
        (g.consol & 4) ? "" : " OPTION", (g.consol & 7) == 7 ? " no console key held" : " held",
        g.trig[0] & 1, g.trig[1] & 1, g.trig[2] & 1, g.trig[3] & 1);
    tadd(&t, "Collisions  PF0 PF1 PF2 PF3    P0 P1 P2 P3\n");
    for (int i = 0; i < 4; i++) {
        const char n[3] = { 'M', (char)('0' + i), 0 };
        collisionRow(&t, n, g.mpf[i], g.mpl[i], -1);
    }
    for (int i = 0; i < 4; i++) {
        const char n[3] = { 'P', (char)('0' + i), 0 };
        collisionRow(&t, n, g.ppf[i], g.ppl[i], i);
    }
    setText(_gtia, tdone(&t), NO);

    /* the colour registers through the palette */
    const uint8_t vals[SWATCHES] = { g.colpm[0], g.colpm[1], g.colpm[2], g.colpm[3],
                                     g.colpf[0], g.colpf[1], g.colpf[2], g.colpf[3], g.colbk };
    for (int i = 0; i < SWATCHES; i++) {
        const uint32_t rgb = a800debug_color(_dbg, vals[i]) & 0xFFFFFF;
        _swatchLabel[i].stringValue = [NSString stringWithFormat:@"%-6s $%02X", names[i], vals[i]];
        if (rgb != _swatchRgb[i]) {
            _swatchRgb[i] = rgb;
            _swatch[i].color = colorFromXrgb(rgb);
        }
        _swatch[i].toolTip = [NSString stringWithFormat:@"%s $%02X: #%06X", names[i], vals[i], rgb];
    }
    if (_palettePx && a800debug_palette_image(_dbg, _palettePx))
        _palette.image = imageFromXrgb(_palettePx, A800DEBUG_PALETTE_WIDTH, A800DEBUG_PALETTE_HEIGHT);
}

- (void)refreshPokey
{
    a800debug_pokey p;
    a800debug_pia pia;
    a800debug_map map;
    const BOOL stereo = a800session_get_int(_session, "stereo", 0) != 0;
    Text t = { NULL, 0, 0 };

    a800debug_pokey_get(_dbg, 0, &p);
    pokeyText(&t, &p, stereo ? "POKEY 1 ($D200, left)" : "POKEY ($D200)");
    if (stereo) {
        a800debug_pokey p2;
        a800debug_pokey_get(_dbg, 1, &p2);
        tadd(&t, "\n");
        pokeyText(&t, &p2, "POKEY 2 ($D210, right)");
    }
    tadd(&t,
        "\nKBCODE  $%02X\n"
        "SKCTL   $%02X   keyboard scan %s, debounce %s, fast pot scan %s, two-tone %s, "
        "serial mode %d%s\n"
        "SKSTAT  $%02X  %s%s%s\n"
        "IRQEN   $%02X   enabled:",
        p.kbcode, p.skctl, onOff(p.skctl & 2), onOff(p.skctl & 1), onOff(p.skctl & 4),
        onOff(p.skctl & 8), (p.skctl >> 4) & 7, (p.skctl & 0x80) ? ", forcing a break" : "",
        p.skstat, (p.skstat & 8) ? "" : " SHIFT held", (p.skstat & 4) ? "" : " a key held",
        (p.skstat & 0x0C) == 0x0C ? " no key held" : "", p.irqen);
    irqWords(&t, p.irqen, 0);
    tadd(&t, "\nIRQST   $%02X   pending:", p.irqst);
    irqWords(&t, p.irqst, 1);
    tadd(&t, "\nRANDOM  $%02X\nPOT0-7 ", p.random);
    for (int i = 0; i < 8; i++) tadd(&t, " $%02X", p.pot[i]);

    /* the PIA: what the ports read is the joysticks' lines (and on XL/XE,
     * port B is the memory control) */
    a800debug_pia_get(_dbg, &pia);
    a800debug_map_get(_dbg, &map);
    tadd(&t, "\n\nPIA\nPORTA   out $%02X   DDR $%02X (1 = output)", pia.porta, (uint8_t)~pia.porta_mask);
    if (pia.pactl & 4) {
        uint8_t in = 0;
        a800debug_read(_dbg, 0xD300, &in, 1);
        tadd(&t, "   reads $%02X: stick 1 ", in);
        stickWords(&t, in & 15);
        tadd(&t, ", stick 2 ");
        stickWords(&t, in >> 4);
    }
    tadd(&t,
        "\nPACTL   $%02X   port %s, cassette motor (CA2) %s, CA1 interrupt %s\n"
        "PORTB   out $%02X   DDR $%02X",
        pia.pactl, (pia.pactl & 4) ? "data" : "direction", pia.ca2 ? "off" : "on",
        onOff(pia.pactl & 1), pia.portb, (uint8_t)~pia.portb_mask);
    if (map.xl) {
        tadd(&t, "   (memory control: see the Memory Map)");
    } else if (pia.pbctl & 4) {
        uint8_t in = 0;
        a800debug_read(_dbg, 0xD301, &in, 1);
        tadd(&t, "   reads $%02X: stick 3 ", in);
        stickWords(&t, in & 15);
        tadd(&t, ", stick 4 ");
        stickWords(&t, in >> 4);
    }
    tadd(&t,
        "\nPBCTL   $%02X   port %s, SIO command (CB2) %s, CB1 interrupt %s\n"
        "Lines   CA1 %d  CA2 %d  CB1 %d  CB2 %d   IRQ %s\n",
        pia.pbctl, (pia.pbctl & 4) ? "data" : "direction", pia.cb2 ? "idle" : "asserted",
        onOff(pia.pbctl & 1), pia.ca1, pia.ca2, pia.cb1, pia.cb2, pia.irq ? "asserted" : "-");
    setText(_pokey, tdone(&t), NO);
}

- (void)refreshMap
{
    a800debug_map m;
    a800debug_map_get(_dbg, &m);
    const int kb = m.ram_kb == 321 ? 320 : m.ram_kb;     /* atari800's Compy Shop 320K */
    Text t = { NULL, 0, 0 };
    tadd(&t, "Machine   %s, %dK RAM%s\n\n", m.machine, kb, m.xl ? ", XL/XE banking" : "");
    if (m.xl) {
        tadd(&t, "PORTB     $%02X = ", m.portb);
        for (int b = 7; b >= 0; b--) tadd(&t, "%d%s", (m.portb >> b) & 1, b == 4 ? " " : "");
        tadd(&t,
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

    tadd(&t, "What the CPU sees\n");
    if (m.xl) {
        tadd(&t, "  $0000-$3FFF  RAM\n");
        if (m.xe_cpu_bank >= 0) tadd(&t, "  $4000-$7FFF  extended RAM bank %d\n", m.xe_cpu_bank);
        else tadd(&t, "  $4000-$7FFF  RAM\n");
        if (m.xe_antic_bank >= 0) tadd(&t, "               (ANTIC reads extended bank %d here)\n", m.xe_antic_bank);
        if (m.selftest) tadd(&t, "  $5000-$57FF  self-test ROM\n");
        tadd(&t, "  $8000-$9FFF  %s\n", m.cart_type ? "RAM, or the cartridge" : "RAM");
        tadd(&t, "  $A000-$BFFF  %s\n", m.basic_rom ? "BASIC ROM" : m.cart_type ? "RAM, or the cartridge" : "RAM");
        tadd(&t, "  $C000-$CFFF  %s\n", m.os_rom ? "OS ROM" : "RAM");
        tadd(&t, "  $D000-$D7FF  hardware: GTIA $D000, POKEY $D200, PIA $D300, ANTIC $D400\n");
        tadd(&t, "  $D800-$FFFF  %s\n", m.os_rom ? "OS ROM (floating point at $D800)" : "RAM");
    } else {
        const int top = (kb > 48 ? 48 : kb) * 1024 - 1;
        if (top > 0) tadd(&t, "  $0000-$%04X  RAM\n", top);
        if (m.cart_type) tadd(&t, "  $8000-$BFFF  the cartridge, where its type maps it\n");
        if (m.basic_rom) tadd(&t, "  $A000-$BFFF  BASIC ROM\n");
        tadd(&t, "  $C000-$CFFF  %s\n", kb > 48 ? "RAM" : "(nothing)");
        tadd(&t, "  $D000-$D7FF  hardware: GTIA $D000, POKEY $D200, PIA $D300, ANTIC $D400\n");
        tadd(&t, "  $D800-$FFFF  OS ROM (floating point at $D800)\n");
    }

    if (m.cart_type)
        tadd(&t, "\nCartridge   type %d: %s, bank %d\n            %s\n", m.cart_type, m.cart_desc, m.cart_bank,
             m.cart_name);
    else
        tadd(&t, "\nCartridge   none\n");
    if (m.piggyback_type)
        tadd(&t, "Piggyback   type %d\n            %s\n", m.piggyback_type, m.piggyback_name);
    setText(_map, tdone(&t), NO);
}

- (void)refreshBps
{
    static a800debug_breakpoint bps[BP_MAX];
    int max = 0;
    const int n = a800debug_breakpoint_list(_dbg, bps, BP_MAX);
    const int used = a800debug_breakpoint_budget(_dbg, &max);
    _bpBudget.stringValue = [NSString stringWithFormat:@"atari800's condition table: %d of %d entries used", used, max];

    /* rebuilt only when it changed (each step refreshes) */
    Text sig = { NULL, 0, 0 };
    for (int i = 0; i < n; i++)
        tadd(&sig, "%d %d %d %d %d %s\n", bps[i].id, bps[i].type, bps[i].start, bps[i].end, bps[i].enabled,
             bps[i].condition);
    NSString *s = tdone(&sig);
    if (_bpSig && [s isEqualToString:_bpSig]) return;
    _bpSig = s;

    for (NSView *v in [_bpList.arrangedSubviews copy]) [_bpList removeView:v];
    if (n == 0)
        [_bpList addArrangedSubview:[self hint:@"No breakpoints or watchpoints. Click the left edge of a "
                                               @"disassembly line, or add one below."]];
    for (int i = 0; i < n; i++) {
        Text t = { NULL, 0, 0 };
        tadd(&t, "#%-4d %-11s ", bps[i].id, bpTypeName(bps[i].type));
        if (bps[i].end != bps[i].start) tadd(&t, "$%04X-$%04X", bps[i].start, bps[i].end);
        else tadd(&t, "$%04X      ", bps[i].start);
        if (bps[i].condition[0]) tadd(&t, "   if %s", bps[i].condition);
        NSButton *box = [NSButton checkboxWithTitle:tdone(&t) target:self action:@selector(bpEnabled:)];
        box.state = bps[i].enabled ? NSControlStateValueOn : NSControlStateValueOff;
        box.tag = bps[i].id;
        box.font = monoFont();
        box.toolTip = @"Enabled";
        NSButton *remove = [self button:@"Remove" action:@selector(bpRemove:)];
        remove.tag = bps[i].id;
        [_bpList addArrangedSubview:hstack(@[box, remove])];
    }
}

- (void)refreshSio:(BOOL)force
{
    static a800debug_sio e[SIO_MAX];
    const BOOL fuji = a800session_fujinet_running(_session) != 0;
    const int n = a800debug_sio_trace(_dbg, e, SIO_MAX);
    _sioNote.stringValue = fuji ? @"FujiNet is running: the Atari's commands go to it over NetSIO."
                                : @"FujiNet is off: atari800's own drives answer (\"patch\" is its fast SIO patch).";
    /* a log: set again only when it moved */
    if (!force && n == _sioCount && (n == 0 || memcmp(&e[n - 1], &_sioLast, sizeof _sioLast) == 0)) return;
    _sioCount = n;
    if (n > 0) _sioLast = e[n - 1];
    const BOOL follow = force || atEnd(_sio);
    Text t = { NULL, 0, 0 };
    tadd(&t, "  Frame  Path    Dev  Cmd  Aux1 Aux2   Command");
    for (int i = 0; i < n; i++)
        tadd(&t, "\n%7u  %-6s  $%02X  $%02X  $%02X  $%02X   %s", e[i].frame, e[i].path, e[i].device, e[i].command,
             e[i].aux1, e[i].aux2, e[i].text);
    if (n == 0) tadd(&t, "\n(no SIO commands since the trace was cleared)");
    setText(_sio, tdone(&t), follow);
}

- (void)refreshFujinet:(BOOL)force
{
    static a800_fuji_packet p[FUJI_MAX];
    static char state[2048];
    const int n = a800debug_fujinet_trace(_dbg, p, FUJI_MAX);
    if (a800session_fujinet_external(_session))
        _fujiNote.stringValue = [NSString stringWithFormat:@"External FujiNet on NetSIO %s, %s.",
                                 a800session_fujinet_endpoint(_session),
                                 a800session_fujinet_link(_session) ? "connected" : "not connected"];
    else if (!a800session_fujinet_running(_session))
        _fujiNote.stringValue = @"FujiNet is off (Settings ▸ FujiNet).";
    else
        _fujiNote.stringValue = a800session_fujinet_link(_session) ? @"FujiNet is on NetSIO, connected."
                                                                  : @"FujiNet is on NetSIO, not connected.";
    state[0] = '\0';
    a800debug_fujinet_state(_dbg, state, (int)sizeof state);
    setText(_fujiState, textFrom(state), NO);

    /* a log: set again only when it moved */
    if (!force && n == _fujiCount && (n == 0 || p[n - 1].serial == _fujiLast)) return;
    _fujiCount = n;
    _fujiLast = n > 0 ? p[n - 1].serial : 0;
    const BOOL follow = force || atEnd(_fujiText);
    Text t = { NULL, 0, 0 };
    for (int i = 0; i < n; i++) {
        tadd(&t, "%s%s\n     ", i ? "\n" : "", p[i].text);
        for (int k = 0; k < p[i].nbytes && k < (int)sizeof p[i].data; k++) tadd(&t, "%02X ", p[i].data[k]);
        if (p[i].nbytes > (int)sizeof p[i].data) tadd(&t, "…");
    }
    if (n == 0) tadd(&t, "(no SIO packets since the trace was cleared)");
    setText(_fujiText, tdone(&t), follow);
}

- (void)refreshSnapshots
{
    const BOOL fuji = a800session_fujinet_running(_session) != 0;
    for (int i = 0; i < SNAPSHOTS; i++) {
        const BOOL has = a800debug_snapshot_exists(_dbg, i) != 0;
        _snapRestore[i].enabled = has;
        _snapState[i].stringValue = has ? @"saved" : @"empty";
        _snapRestore[i].toolTip = fuji ? kFujinetNote : nil;
    }
    _snapNote.hidden = !fuji;
}

- (void)refreshPage:(int)page
{
    if (!a800session_is_running(_session)) return;
    switch (page) {
    case TAB_CPU: [self refreshCpu]; [self refreshMem]; [self refreshHistory]; break;
    case TAB_DISASM: [self refreshDisasm]; break;
    case TAB_ANTIC: [self refreshAntic]; break;
    case TAB_GTIA: [self refreshGtia]; break;
    case TAB_POKEY: [self refreshPokey]; break;
    case TAB_MAP: [self refreshMap]; break;
    case TAB_BREAKS: [self refreshBps]; break;
    case TAB_SIO: [self refreshSio:NO]; break;
    case TAB_FUJINET: [self refreshFujinet:NO]; break;
    default: break;
    }
}

- (void)refreshAll
{
    [self refreshStatus];
    [self refreshPage:[self currentTab]];
}

/* After anything that may have moved the engine: the console takes what
 * atari800 printed at a stop, then the views follow. The drain moves the
 * generation itself, so it is read after. */
- (void)sync
{
    _wasStopped = a800debug_is_stopped(_dbg) != 0;
    if (_wasStopped) [self consoleDrain];
    _seenGen = a800debug_generation(_dbg);
    _runningTicks = 0;
    [self refreshAll];
}

/* From inside a breakpoint row's own action: the list rebuilds after. */
- (void)syncLater
{
    __weak A800DebuggerWindow *weakSelf = self;
    dispatch_async(dispatch_get_main_queue(), ^{
        [weakSelf sync];
    });
}

/* A step takes microseconds: a moment's wait for the stop keeps the window
 * from flashing "Running" in between. */
- (void)settleAndSync
{
    for (int i = 0; i < 40 && !a800debug_is_stopped(_dbg); i++) usleep(1000);
    [self sync];
}

- (void)attachAndRefresh
{
    a800debug_attach(_dbg);     /* stops the machine */
    a800debug_stop(_dbg);       /* ... and so does showing it when it was up and running */
    _attachedRunning = a800session_is_running(_session) != 0;
    [self settleAndSync];
}

- (void)onTick
{
    if (!_window.isVisible) return;
    if (!a800session_is_running(_session)) {
        if (_attachedRunning) {
            _attachedRunning = NO;
            [self refreshStatus];
        }
        return;
    }
    /* Opened before the session started, or showing across a restart: the
     * engine attaches (stopping the machine) as it comes up. */
    if (!_attachedRunning) {
        a800debug_detach(_dbg);
        [self attachAndRefresh];
        return;
    }
    const unsigned gen = a800debug_generation(_dbg);
    const BOOL stopped = a800debug_is_stopped(_dbg) != 0;
    if (gen != _seenGen || stopped != _wasStopped) {
        [self sync];
    } else if (!stopped && ++_runningTicks >= 5) {
        /* live values twice a second while the machine runs */
        const int page = [self currentTab];
        _runningTicks = 0;
        [self refreshStatus];
        if (page == TAB_CPU) [self refreshCpu];
        if (page == TAB_SIO) [self refreshSio:NO];
        if (page == TAB_FUJINET) [self refreshFujinet:NO];
    }
}

/* ---- the console ------------------------------------------------------------------ */

/* Onto the console's transcript, which keeps its last CONSOLE_KEEP
 * characters, whole lines. */
- (void)consoleAppend:(NSString *)text
{
    NSTextStorage *ts = _conOut.textStorage;
    [ts appendAttributedString:[[NSAttributedString alloc] initWithString:text attributes:monoAttrs()]];
    if (ts.length > CONSOLE_KEEP) {
        NSString *s = ts.string;
        NSUInteger cut = s.length - CONSOLE_KEEP * 3 / 4;
        const NSRange nl = [s rangeOfString:@"\n" options:0 range:NSMakeRange(cut, s.length - cut)];
        if (nl.location != NSNotFound) cut = nl.location + 1;
        [ts deleteCharactersInRange:NSMakeRange(0, cut)];
    }
    [_conOut scrollRangeToVisible:NSMakeRange(ts.length, 0)];
}

/* Command output, ended with a newline. */
- (void)consoleOutput:(const char *)text
{
    NSString *t = textFrom(text);
    if (t.length == 0) return;
    [self consoleAppend:[t hasSuffix:@"\n"] ? t : [t stringByAppendingString:@"\n"]];
}

/* Messages from Load Symbols and Save land in the console and, since it may
 * not be the tab showing, along the foot. */
- (void)say:(NSString *)msg
{
    [self consoleAppend:[msg stringByAppendingString:@"\n"]];
    [self message:msg];
}

/* Along the window's foot for a few seconds. */
- (void)message:(NSString *)msg
{
    const unsigned serial = ++_messageSerial;
    _message.stringValue = msg;
    _message.toolTip = msg;
    __weak A800DebuggerWindow *weakSelf = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(6 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        A800DebuggerWindow *s = weakSelf;
        if (s && s->_messageSerial == serial) s->_message.stringValue = @"";
    });
}

- (void)setPrompt:(BOOL)asmMode text:(NSString *)prompt
{
    _conAsm = asmMode;
    if (asmMode && prompt.length)
        _conPromptText = prompt.length > 32 ? [prompt substringToIndex:32] : prompt;
    else
        _conPromptText = @"> ";
    _conPrompt.stringValue = _conPromptText;
    _conIn.placeholderString = asmMode
        ? @"Assembler: an instruction (LDA #$42), or an empty line to finish"
        : @"Monitor command (HELP, D, M, F, A, BLINE, DLIST, LABELS, CONT ...) — Tab completes, Up/Down recall";
}

/* The banner and registers atari800 printed at a stop: an empty line,
 * which its monitor ignores, hands them over. Not while the assembler is
 * waiting, where an empty line would end it. */
- (void)consoleDrain
{
    static char out[65536];
    if (_conAsm || !a800session_is_running(_session)) return;
    a800debug_command(_dbg, "", out, (int)sizeof out);
    [self consoleOutput:out];
}

/* The console's line. Enter, Tab and the arrows arrive mid-edit, so it is
 * read and written through the field editor, which keeps the focus. */
- (NSString *)consoleLine
{
    NSText *ed = _conIn.currentEditor;
    return ed ? ed.string : _conIn.stringValue;
}

- (void)setConsoleLine:(NSString *)text
{
    NSText *ed = _conIn.currentEditor;
    if (ed) {
        ed.string = text;
        [ed setSelectedRange:NSMakeRange(text.length, 0)];
    } else {
        _conIn.stringValue = text;
    }
}

- (void)runConsole
{
    static char out[65536];
    NSString *cmd = trimmed([self consoleLine]);
    /* an empty line only means something to the assembler (its end) */
    if (!_conAsm && cmd.length == 0) return;
    if (cmd.length && ![_history.lastObject isEqualToString:cmd]) [_history addObject:cmd];
    _historyPos = (NSInteger)_history.count;
    [self consoleAppend:[NSString stringWithFormat:@"%@%@\n", _conPromptText, cmd]];
    [self setConsoleLine:@""];

    const int rc = a800debug_command(_dbg, cmd.UTF8String, out, (int)sizeof out);
    if (rc == A800DBG_CMD_NEEDS_INPUT) {
        /* the output ends with the assembler's own prompt ("0642: ") */
        char *nl = strrchr(out, '\n');
        char *prompt = nl ? nl + 1 : out;
        NSString *saved = textFrom(prompt);
        *prompt = '\0';
        [self consoleOutput:out];
        [self setPrompt:YES text:saved];
    } else {
        [self consoleOutput:out];
        [self setPrompt:NO text:nil];
    }
    if (rc == A800DBG_CMD_RESUMED) [self settleAndSync];
    else [self sync];
}

- (void)showHistory
{
    [self setConsoleLine:_historyPos < (NSInteger)_history.count ? _history[(NSUInteger)_historyPos] : @""];
}

/* Tab completes the command word against atari800's commands (the longest
 * common start of several, which are listed). */
- (void)complete
{
    NSString *text = [self consoleLine];
    if (_conAsm || [text rangeOfString:@" "].location != NSNotFound) {
        NSBeep();
        return;
    }
    char comps[2048] = "";
    a800debug_completions(_dbg, text.UTF8String, comps, (int)sizeof comps);
    NSMutableArray<NSString *> *words = [NSMutableArray array];
    for (NSString *w in [textFrom(comps) componentsSeparatedByString:@" "])
        if (w.length) [words addObject:w];
    if (words.count == 0) {
        NSBeep();
        return;
    }
    if (words.count == 1) {
        [self setConsoleLine:[words[0] stringByAppendingString:@" "]];
    } else {
        NSString *first = words[0];
        NSUInteger len = first.length;
        for (NSString *w in words) {
            NSUInteger k = 0;
            while (k < len && k < w.length && [w characterAtIndex:k] == [first characterAtIndex:k]) k++;
            len = k;
        }
        if (len > text.length) [self setConsoleLine:[first substringToIndex:len]];
        [self consoleAppend:[[words componentsJoinedByString:@"  "] stringByAppendingString:@"\n"]];
    }
}

/* In the console, Enter runs the line, Tab completes, and Up and Down walk
 * the history. */
- (BOOL)control:(NSControl *)control textView:(NSTextView *)textView doCommandBySelector:(SEL)sel
{
    (void)textView;
    if (control != _conIn) return NO;
    if (sel == @selector(insertNewline:)) {
        [self runConsole];
        return YES;
    }
    if (sel == @selector(moveUp:)) {
        if (_historyPos > 0) {
            _historyPos--;
            [self showHistory];
        }
        return YES;
    }
    if (sel == @selector(moveDown:)) {
        if (_historyPos < (NSInteger)_history.count) {
            _historyPos++;
            [self showHistory];
        }
        return YES;
    }
    if (sel == @selector(insertTab:) || sel == @selector(insertBacktab:)) {
        [self complete];
        return YES;
    }
    return NO;
}

/* ---- the rest of the handlers ---------------------------------------------------- */

- (void)toggleRun:(id)sender
{
    (void)sender;
    if (a800debug_is_stopped(_dbg)) a800debug_resume(_dbg);
    else a800debug_stop(_dbg);
    [self sync];
}

- (void)step:(id)sender { (void)sender; a800debug_step(_dbg); [self settleAndSync]; }
- (void)stepOver:(id)sender { (void)sender; a800debug_step_over(_dbg); [self settleAndSync]; }
- (void)stepOut:(id)sender { (void)sender; a800debug_step_out(_dbg); [self settleAndSync]; }
- (void)stepFrame:(id)sender { (void)sender; a800debug_frame(_dbg); [self settleAndSync]; }

- (void)runToCursor:(id)sender
{
    (void)sender;
    if (_selected < 0) {
        [self message:@"Select a line in the Disassembly to run to"];
        return;
    }
    a800debug_run_to(_dbg, (uint16_t)_selected);
    [self settleAndSync];
}

- (void)breakOnBrk:(NSButton *)sender
{
    a800debug_break_on_brk(_dbg, sender.state == NSControlStateValueOn);
    [self sync];
}

- (void)toggleBreakpointAt:(uint16_t)addr
{
    if (a800debug_breakpoint_toggle(_dbg, addr) < 0)
        [self message:@"atari800's condition table is full: remove a breakpoint first"];
    [self sync];
}

- (void)showSnapshots:(NSButton *)sender
{
    if (_snapPopover.shown) {
        [_snapPopover close];
        return;
    }
    [self refreshSnapshots];
    NSView *box = _snapPopover.contentViewController.view;
    [box layoutSubtreeIfNeeded];
    _snapPopover.contentSize = box.fittingSize;
    [_snapPopover showRelativeToRect:sender.bounds ofView:sender preferredEdge:NSRectEdgeMaxY];
}

- (void)snapSave:(NSButton *)sender
{
    const int slot = (int)sender.tag;
    if (a800debug_snapshot_save(_dbg, slot) == 0)
        [self message:[NSString stringWithFormat:@"Snapshot %d saved", slot]];
    else
        [self message:[NSString stringWithFormat:@"Could not save snapshot %d", slot]];
    [self refreshSnapshots];
}

- (void)snapRestore:(NSButton *)sender
{
    const int slot = (int)sender.tag;
    [_snapPopover close];
    if (a800debug_snapshot_restore(_dbg, slot) == 0)
        [self message:[NSString stringWithFormat:@"Snapshot %d restored", slot]];
    else
        [self message:[NSString stringWithFormat:@"Could not restore snapshot %d", slot]];
    [self sync];
}

/* A label, or a number as parseNum reads it. */
- (BOOL)addressOf:(NSString *)text into:(long *)out
{
    NSString *t = trimmed(text);
    if (t.length == 0) return NO;
    const int a = a800debug_label_address(_dbg, t.UTF8String);
    if (a >= 0) {
        *out = a;
        return YES;
    }
    return parseNum(t, out);
}

- (void)applyRegister:(NSTextField *)sender
{
    const NSInteger i = sender.tag;
    long v;
    if (i < 0 || i >= 6) return;
    if (parseNum(sender.stringValue, &v))
        a800debug_cpu_set(_dbg, kRegs[i], (int)(v & (i == 0 ? 0xFFFF : 0xFF)));
    else
        [self message:@"Not a number ($ or plain hex, # decimal)"];
    [self sync];
}

- (void)flagClicked:(NSButton *)sender
{
    const NSInteger i = sender.tag;
    if (i >= 0 && i < 6 && a800debug_is_stopped(_dbg))
        a800debug_cpu_set(_dbg, kFlags[i], sender.state == NSControlStateValueOn);
    [self sync];
}

/* Go to: the dump from just above an address (or a label), its row marked
 * and scrolled to. */
- (void)memGoto:(id)sender
{
    (void)sender;
    long a;
    if (![self addressOf:_memGoto.stringValue into:&a]) {
        _memMark = -1;
        [self refreshMem];
        [self message:@"Not an address or a label"];
        return;
    }
    _memMark = (int)(a & 0xFFFF);
    _memTop = (_memMark - 0x100) & 0xFFF0;
    [self refreshMem];
    [self scrollMemToLine:1 + ((_memMark - _memTop) & 0xFFFF) / 16];
}

/* The dump a page of MEM_ROWS back or on. */
- (void)memPage:(NSButton *)sender
{
    _memTop = (_memTop + (int)sender.tag * MEM_ROWS * 16) & 0xFFFF;
    [self refreshMem];
}

- (void)memWrite:(id)sender
{
    (void)sender;
    long a, v;
    if (![self addressOf:_memAddr.stringValue into:&a]) {
        [self message:@"Not an address or a label"];
        return;
    }
    if (!parseNum(_memVal.stringValue, &v) || v < 0 || v > 0xFF) {
        [self message:@"The value is a byte ($00-$FF)"];
        return;
    }
    a800debug_write(_dbg, (uint16_t)a, (uint8_t)v);
    [self sync];
}

- (void)followToggled:(id)sender
{
    (void)sender;
    [self refreshDisasm];
}

- (void)jumpTo:(id)sender
{
    (void)sender;
    long a;
    if (![self addressOf:_jump.stringValue into:&a]) {
        [self message:@"Not an address or a label"];
        return;
    }
    _followPc.state = NSControlStateValueOff;
    _disasmTop = (int)(a & 0xFFFF);
    _selected = _disasmTop;
    [self refreshDisasm];
}

/* The list follows a moment later: these come from its own rows. */
- (void)bpEnabled:(NSButton *)sender
{
    a800debug_breakpoint_enable(_dbg, (int)sender.tag, sender.state == NSControlStateValueOn);
    [self syncLater];
}

- (void)bpRemove:(NSButton *)sender
{
    a800debug_breakpoint_remove(_dbg, (int)sender.tag);
    [self syncLater];
}

- (void)bpClear:(id)sender
{
    (void)sender;
    a800debug_breakpoint_clear(_dbg);
    [self sync];
}

- (void)bpAdd:(id)sender
{
    static const int types[4] = { A800DEBUG_BP_EXEC, A800DEBUG_BP_READ, A800DEBUG_BP_WRITE,
                                  A800DEBUG_BP_READ | A800DEBUG_BP_WRITE };
    char msg[200] = "";
    long a, b;
    (void)sender;
    if (![self addressOf:_bpStart.stringValue into:&a]) {
        _bpMsg.stringValue = @"Not an address or a label";
        return;
    }
    b = a;
    if (trimmed(_bpEnd.stringValue).length && ![self addressOf:_bpEnd.stringValue into:&b]) {
        _bpMsg.stringValue = @"The end is not an address or a label";
        return;
    }
    NSInteger sel = _bpType.indexOfSelectedItem;
    if (sel < 0 || sel > 3) sel = 0;
    if (a800debug_breakpoint_add(_dbg, types[sel], (uint16_t)a, (uint16_t)b, trimmed(_bpCond.stringValue).UTF8String,
                                 msg, (int)sizeof msg) < 0) {
        _bpMsg.stringValue = msg[0] ? textFrom(msg) : @"atari800 did not take it";
        return;
    }
    _bpMsg.stringValue = @"";
    _bpStart.stringValue = @"";
    _bpEnd.stringValue = @"";
    _bpCond.stringValue = @"";
    [self sync];
}

/* A scanline break is atari800's own (BLINE): the next Run stops there. */
- (void)bpLine:(id)sender
{
    static char out[1024];
    (void)sender;
    NSString *text = trimmed(_bpLine.stringValue);
    const char *s = text.UTF8String;
    char *end;
    const long n = strtol(s, &end, 10);
    if (text.length == 0 || end == s || *end || n < 0 || n > 311) {
        _bpMsg.stringValue = @"A scanline is 0-311 (decimal)";
        return;
    }
    /* the assembler would take the command as its next line */
    if (_conAsm) {
        _bpMsg.stringValue = @"Finish the assembler in the Console first (an empty line ends it)";
        return;
    }
    char cmd[32];
    snprintf(cmd, sizeof cmd, "BLINE %ld", n);
    a800debug_command(_dbg, cmd, out, (int)sizeof out);
    /* in the console's transcript too, as if typed there */
    [self consoleAppend:[NSString stringWithFormat:@"> %s\n", cmd]];
    [self consoleOutput:out];
    char *nl = strchr(out, '\n');
    if (nl) *nl = '\0';
    _bpMsg.stringValue = out[0] ? textFrom(out) : textFrom(cmd);
    [self sync];
}

- (void)sioClear:(id)sender
{
    (void)sender;
    a800debug_sio_clear(_dbg);
    [self refreshSio:YES];
}

- (void)fujiClear:(id)sender
{
    (void)sender;
    a800debug_fujinet_trace_clear(_dbg);
    [self refreshFujinet:YES];
}

- (void)loadSymbols:(id)sender
{
    (void)sender;
    NSOpenPanel *p = [NSOpenPanel openPanel];
    p.title = @"Load Symbols";
    p.message = @"Labels from ld65 -Ln, VICE .lbl, MADS .lab or an atari800 label file";
    p.canChooseDirectories = NO;
    p.allowsMultipleSelection = NO;
    if ([p runModal] != NSModalResponseOK || !p.URL) return;
    char msg[512] = "";
    if (a800debug_load_symbols(_dbg, p.URL.fileSystemRepresentation, msg, (int)sizeof msg) >= 0)
        [self say:[NSString stringWithFormat:@"Loaded %@ from %@", textFrom(msg), p.URL.lastPathComponent]];
    else
        [self say:[NSString stringWithFormat:@"No symbols loaded: %@", textFrom(msg)]];
    [self sync];
}

- (void)saveFile:(NSPopUpButton *)sender
{
    const NSInteger kind = sender.selectedItem.tag;
    if (kind < 0 || kind >= NUM_SAVES) return;
    NSSavePanel *p = [NSSavePanel savePanel];
    p.title = [NSString stringWithFormat:@"Save %@", kSaveTitles[kind]];
    p.nameFieldStringValue = kSaveFiles[kind];
    if ([p runModal] != NSModalResponseOK || !p.URL) return;
    char msg[512] = "";
    if (a800debug_save(_dbg, kSaveKinds[kind], p.URL.fileSystemRepresentation, msg, (int)sizeof msg) == 0)
        [self say:[NSString stringWithFormat:@"%@: %@", p.URL.lastPathComponent, textFrom(msg)]];
    else
        [self say:textFrom(msg)];
}
@end
