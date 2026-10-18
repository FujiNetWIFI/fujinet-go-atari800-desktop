/*
 * A800ControllersWindow -- the AppKit Controllers panel: the on-screen Atari
 * keyboard (drawn from the session's table, so every frontend shows the same
 * keys for the machine: HELP on the XL/XE, F1-F4 on the 1200XL), the
 * controller ports with what is plugged into each (a stick and fire for a
 * joystick, a slider and fire per paddle), the gamepads and the port each
 * drives, and Map mode.
 *
 * Keys and controls are caps (A800Cap) that act on mouseDown AND mouseUp,
 * not on a click: the Atari scans its keyboard and samples the sticks once a
 * frame, so a press present only for the instant of a click falls between
 * frames. A cap is HELD while the mouse is down on it, exactly like the real
 * key; dragging off lets it go (mouseDragged and mouseUp come to the view
 * that got mouseDown). SHIFT and CTRL latch for the next key instead: a
 * mouse cannot hold two keys. Losing the key window, hiding and rebuilding
 * let go of what the mouse holds here (only that: the main window releases
 * its own keys).
 *
 * What the Atari sees held -- host keyboard, gamepads and this panel -- is
 * filled with the accent (white text), so the panel doubles as an input
 * tester for a new gamepad. Keys typed with the panel key go to the Atari as
 * in the main window (A800ForwardKey, A800ForwardFlags); no view here takes
 * the keyboard, so the panel itself is the first responder.
 *
 * FIXED SIZE: an NSPanel without a resize mask, sized from its contents.
 * Only a machine or port device change resizes it (the keyboard and the
 * ports are rebuilt; the top edge stays put). Hidden at start, ordered out
 * rather than closed, so its position survives.
 *
 * MAP MODE: Map arms a two-step rebind -- click a joystick control or a
 * console key (RESET, OPTION, SELECT, START, HELP, BREAK), then press a key
 * or a gamepad button. Rebinding steals the key from whatever held it.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "ControllersWindow.h"

#import "../KeyForward.h"

#include <math.h>
#include <string.h>

#define KBD_UNIT     38.0   /* points per key unit */
#define KBD_GAP       3.0   /* between key caps */
#define CELL_W       84.0   /* a joystick control: two lines of Map label fit */
#define CELL_H       40.0
#define CELL_GAP      4.0
#define JOY_W        (3 * CELL_W + 2 * CELL_GAP)
#define JOY_H        (3 * CELL_H + 2 * CELL_GAP)
#define PORT_W      290.0
#define PORT_PAD     10.0
#define PORT_GAP     16.0
#define TITLE_H      18.0
#define PORT_H       (PORT_PAD + TITLE_H + 4 + JOY_H + PORT_PAD)
#define PADDLE_ROW_H 60.0
#define PADDLE_MAX   228
#define MARGIN       12.0
#define ROW_H        26.0
#define EFF_W       110.0
#define POPUP_W     150.0
#define MAP_W        76.0
#define DEFAULTS_W   96.0
#define MAP_ROW_H    30.0
#define MAX_PAD_ROWS 4
#define SLOW_TICKS   8      /* the 30 Hz tick's 250 ms part */

static NSString *nsstr(const char *s)
{
    NSString *r = s ? [NSString stringWithUTF8String:s] : nil;
    return r ? r : @"";
}

static NSString *keyName(uint32_t keysym)
{
    char name[48];
    name[0] = '\0';
    if (keysym) a800session_keysym_name(keysym, name, (int)sizeof name);
    return nsstr(name);
}

/* The keys that are console keys as far as Map is concerned. The layout
 * table carries labels only, so they are matched by label. */
static int consoleTarget(const char *label)
{
    static const struct { const char *label; int con; } con[] = {
        { "RESET", A800_CON_RESET }, { "OPTION", A800_CON_OPTION },
        { "SELECT", A800_CON_SELECT }, { "START", A800_CON_START },
        { "HELP", A800_CON_HELP }, { "BREAK", A800_CON_BREAK },
    };
    if (!label) return -1;
    for (size_t i = 0; i < sizeof con / sizeof con[0]; i++)
        if (!strcmp(label, con[i].label)) return A800_TARGET_CONSOLE(con[i].con);
    return -1;
}

/* Points from key units, rounded so neighbouring caps keep an even gap. */
static CGFloat px(float units)
{
    return floor(units * KBD_UNIT + 0.5);
}

/* Lines of text centred in r, the font shrunk (to 6 pt) until they fit. */
static void drawFitted(NSString *text, NSRect r, CGFloat size, BOOL bold, NSColor *ink)
{
    NSArray<NSString *> *lines = [text componentsSeparatedByString:@"\n"];
    const CGFloat nlines = (CGFloat)lines.count;
    NSMutableParagraphStyle *para = [[NSMutableParagraphStyle alloc] init];
    para.alignment = NSTextAlignmentCenter;
    para.lineBreakMode = NSLineBreakByClipping;
    NSDictionary *attrs = nil;
    CGFloat lineH = 0;
    for (;;) {
        NSFont *font = bold ? [NSFont boldSystemFontOfSize:size] : [NSFont systemFontOfSize:size];
        attrs = @{ NSFontAttributeName: font,
                   NSParagraphStyleAttributeName: para,
                   NSForegroundColorAttributeName: ink };
        CGFloat w = 0;
        lineH = 0;
        for (NSString *l in lines) {
            const NSSize s = [l sizeWithAttributes:attrs];
            if (s.width > w) w = s.width;
            if (s.height > lineH) lineH = s.height;
        }
        if ((w <= r.size.width && lineH * nlines <= r.size.height + 2) || size <= 6) break;
        size -= 0.5;
    }
    CGFloat y = r.origin.y + floor((r.size.height - lineH * nlines) / 2);
    for (NSString *l in lines) {
        [l drawInRect:NSMakeRect(r.origin.x, y, r.size.width, lineH) withAttributes:attrs];
        y += lineH;
    }
}

/* ---- views ------------------------------------------------------------------- */

/* Laid out from the top, like the other frontends' tables. */
@interface A800CtlFlippedView : NSView
@end

@implementation A800CtlFlippedView
- (BOOL)isFlipped { return YES; }
@end

/* The ports, each in a rounded frame. */
@interface A800PortsView : A800CtlFlippedView
@property (nonatomic, copy) NSArray<NSValue *> *frames;
@end

@implementation A800PortsView
- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    [[NSColor separatorColor] setStroke];
    for (NSValue *v in self.frames) {
        NSBezierPath *path = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect([v rectValue], 0.5, 0.5)
                                                             xRadius:6 yRadius:6];
        [path stroke];
    }
}
@end

/* A key cap or a joystick control: drawn here, so it is exactly the size
 * given, lit as a fill, and can show two lines of Map label. */
@interface A800Cap : NSView
@property (nonatomic, weak) A800ControllersWindow *owner;
@property (nonatomic, copy) NSString *face;
@property (nonatomic, copy) NSString *shifted;  /* the key's shifted legend, or nil */
@property (nonatomic, copy) NSString *mapText;  /* Map mode: the binding, drawn instead */
@property (nonatomic) int kbdIndex;             /* on-screen keyboard index, or -1 */
@property (nonatomic) int padTarget;            /* what Map rebinds (a stick presses it), or -1 */
@property (nonatomic) int paddleLine;           /* a paddle's fire: port * 2 + paddle, or -1 */
@property (nonatomic) BOOL keyCap;              /* a key of the keyboard: smaller, bold */
@property (nonatomic) BOOL latches;             /* SHIFT, CTRL */
@property (nonatomic) BOOL lit;
@property (nonatomic) BOOL greyed;
@property (nonatomic) BOOL down;                /* the mouse holds it */
@end

/* Keys typed with the panel key go to its owner. */
@interface A800ControllersPanel : NSPanel
@property (nonatomic, weak) A800ControllersWindow *owner;
@end

@interface A800ControllersWindow () <NSWindowDelegate>
- (void)capPressed:(A800Cap *)cap down:(BOOL)down;
- (void)panelKeyDown:(NSEvent *)e;
- (void)panelKeyUp:(NSEvent *)e;
- (void)panelFlagsChanged:(NSEvent *)e;
@end

@implementation A800Cap

- (instancetype)initWithFrame:(NSRect)frame
{
    self = [super initWithFrame:frame];
    if (!self) return nil;
    _kbdIndex = -1;
    _padTarget = -1;
    _paddleLine = -1;
    return self;
}

- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent *)e { (void)e; return YES; }

- (void)setLit:(BOOL)lit
{
    if (lit == _lit) return;
    _lit = lit;
    [self setNeedsDisplay:YES];
}

- (void)setGreyed:(BOOL)greyed
{
    if (greyed == _greyed) return;
    _greyed = greyed;
    [self setNeedsDisplay:YES];
}

- (void)setMapText:(NSString *)text
{
    if (text == _mapText || [text isEqualToString:_mapText]) return;
    _mapText = [text copy];
    [self setNeedsDisplay:YES];
}

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    const NSRect b = [self bounds];
    NSBezierPath *path = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(b, 0.5, 0.5) xRadius:4 yRadius:4];
    NSColor *ink;
    if (self.lit) {
        [A800AccentColor() setFill];
        [path fill];
        ink = [NSColor whiteColor];
    } else if (self.greyed) {
        ink = [NSColor tertiaryLabelColor];
    } else {
        [[NSColor controlColor] setFill];
        [path fill];
        ink = [NSColor labelColor];
    }
    [[NSColor separatorColor] setStroke];
    [path stroke];

    const NSRect inner = NSInsetRect(b, 3, 2);
    if (self.mapText) {
        drawFitted(self.mapText, inner, 10, NO, ink);
    } else if (self.shifted) {
        const CGFloat half = floor(inner.size.height / 2);
        drawFitted(self.shifted, NSMakeRect(inner.origin.x, inner.origin.y, inner.size.width, half), 8, NO, ink);
        drawFitted(self.face, NSMakeRect(inner.origin.x, inner.origin.y + half, inner.size.width,
                                         inner.size.height - half), 10, YES, ink);
    } else if (self.face.length) {
        drawFitted(self.face, inner, self.keyCap ? 10 : 13, self.keyCap, ink);
    }
}

- (void)mouseDown:(NSEvent *)e
{
    (void)e;
    if (self.down) return;
    self.down = YES;
    [self.owner capPressed:self down:YES];
}

/* Dragging off a cap must let it go. */
- (void)mouseDragged:(NSEvent *)e
{
    if (!self.down) return;
    const NSPoint p = [self convertPoint:[e locationInWindow] fromView:nil];
    if (NSPointInRect(p, [self bounds])) return;
    self.down = NO;
    [self.owner capPressed:self down:NO];
}

- (void)mouseUp:(NSEvent *)e
{
    (void)e;
    if (!self.down) return;
    self.down = NO;
    [self.owner capPressed:self down:NO];
}
@end

@implementation A800ControllersPanel
- (void)keyDown:(NSEvent *)e { [self.owner panelKeyDown:e]; }
- (void)keyUp:(NSEvent *)e { [self.owner panelKeyUp:e]; }
- (void)flagsChanged:(NSEvent *)e { [self.owner panelFlagsChanged:e]; }
/* Escape is the Atari's ESC, not "close the panel". */
- (void)cancelOperation:(id)sender { (void)sender; }
@end

/* ---- the panel ------------------------------------------------------------------ */

@implementation A800ControllersWindow {
    a800session *_session;
    A800ControllersPanel *_panel;
    A800CtlFlippedView *_kbdView;
    A800PortsView *_portsView;
    NSMutableArray<A800Cap *> *_caps;       /* the keys and the joystick controls */
    NSSlider *_paddleSlider[A800_PORTS][2];
    A800Cap *_fireCap[A800_PORTS][2];       /* each paddle's fire */
    int _paddleValue[A800_PORTS][2];        /* kept across rebuilds */
    BOOL _paddleHeld[A800_PORTS][2];
    NSTextField *_padsTitle;
    NSTextField *_noPads;
    NSTextField *_padName[MAX_PAD_ROWS];
    NSTextField *_padEff[MAX_PAD_ROWS];     /* each pad's "drives port N" */
    NSPopUpButton *_padAssign[MAX_PAD_ROWS];
    int _padRows;
    unsigned _padGeneration;
    NSButton *_mapButton;
    NSButton *_defaultsButton;
    NSTextField *_hint;
    NSTimer *_timer;
    int _slow;
    NSSize _kbdSize;
    CGFloat _portsHeight;
    /* what the keyboard and the ports were built for */
    int _builtMachine;
    int _builtPorts;
    int _builtDev[A800_PORTS];
    /* what the mouse holds here */
    int _mouseKey;                          /* on-screen keyboard index */
    int _mouseTarget;                       /* joystick target */
    uint32_t _swallowKey;                   /* the key Map took: its repeats and release too */
    /* -2 idle, -1 armed and waiting for a target, >= 0 waiting for a key or
     * gamepad button to bind to that target */
    int _mapState;
    BOOL _placed;
}

- (instancetype)initWithSession:(a800session *)session
{
    self = [super init];
    if (!self) return nil;
    _session = session;
    _mapState = -2;
    _mouseKey = -1;
    _mouseTarget = -1;
    _builtMachine = -1;
    _padGeneration = (unsigned)-1;
    for (int port = 0; port < A800_PORTS; port++)
        for (int p = 0; p < 2; p++)
            _paddleValue[port][p] = PADDLE_MAX;     /* where the session starts them */
    _caps = [NSMutableArray array];

    _panel = [[A800ControllersPanel alloc]
        initWithContentRect:NSMakeRect(0, 0, 640, 600)
                  styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskUtilityWindow)
                    backing:NSBackingStoreBuffered
                      defer:NO];
    [_panel setTitle:@"Controllers"];
    /* Floats above the machine's window, a full-screen one too. */
    [_panel setLevel:NSFloatingWindowLevel];
    [_panel setCollectionBehavior:NSWindowCollectionBehaviorFullScreenAuxiliary];
    [_panel setReleasedWhenClosed:NO];
    [_panel setBecomesKeyOnlyIfNeeded:NO];
    _panel.owner = self;
    [_panel setDelegate:self];

    A800CtlFlippedView *content = [[A800CtlFlippedView alloc] initWithFrame:NSMakeRect(0, 0, 640, 600)];
    [content setAutoresizesSubviews:NO];
    [_panel setContentView:content];

    _kbdView = [[A800CtlFlippedView alloc] initWithFrame:NSZeroRect];
    [content addSubview:_kbdView];
    _portsView = [[A800PortsView alloc] initWithFrame:NSZeroRect];
    [content addSubview:_portsView];

    /* Gamepads: one row per pad, its name, the port it drives and the port
     * it is set to; rows reserved, so pads coming and going keep the size. */
    _padsTitle = [NSTextField labelWithString:@"Gamepads"];
    [_padsTitle setFont:[NSFont boldSystemFontOfSize:12]];
    [content addSubview:_padsTitle];
    _noPads = [NSTextField labelWithString:@"No gamepads connected"];
    [_noPads setTextColor:[NSColor secondaryLabelColor]];
    [content addSubview:_noPads];
    for (int i = 0; i < MAX_PAD_ROWS; i++) {
        _padName[i] = [NSTextField labelWithString:@""];
        [_padName[i] setLineBreakMode:NSLineBreakByTruncatingTail];
        [content addSubview:_padName[i]];
        _padEff[i] = [NSTextField labelWithString:@""];
        [_padEff[i] setTextColor:[NSColor secondaryLabelColor]];
        [_padEff[i] setAlignment:NSTextAlignmentRight];
        [content addSubview:_padEff[i]];
        /* 0 = automatic (connection order), 1-4 = that port */
        _padAssign[i] = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
        [_padAssign[i] addItemsWithTitles:@[@"Automatic", @"Port 1", @"Port 2", @"Port 3", @"Port 4"]];
        _padAssign[i].tag = i;
        _padAssign[i].target = self;
        _padAssign[i].action = @selector(padAssignChanged:);
        [_padAssign[i] setRefusesFirstResponder:YES];
        [content addSubview:_padAssign[i]];
    }

    _mapButton = [NSButton buttonWithTitle:@"Map" target:self action:@selector(toggleMap:)];
    [_mapButton setRefusesFirstResponder:YES];
    [content addSubview:_mapButton];
    _defaultsButton = [NSButton buttonWithTitle:@"Defaults" target:self action:@selector(restoreDefaults:)];
    [_defaultsButton setRefusesFirstResponder:YES];
    [content addSubview:_defaultsButton];
    _hint = [NSTextField wrappingLabelWithString:@""];
    [_hint setFont:[NSFont systemFontOfSize:[NSFont smallSystemFontSize]]];
    [_hint setTextColor:[NSColor secondaryLabelColor]];
    [_hint setMaximumNumberOfLines:2];
    [[_hint cell] setTruncatesLastVisibleLine:YES];
    [content addSubview:_hint];

    [self setMapState:-2];
    [self rebuild];
    return self;
}

- (void)dealloc
{
    [_timer invalidate];
    [_panel setDelegate:nil];
}

/* ---- the keyboard and the ports ------------------------------------------------- */

- (int)machineNow
{
    const int m = a800session_get_int(_session, "machine", A800_M_130XE);
    return m >= 0 && m < A800_M_COUNT ? m : A800_M_130XE;
}

/* The 400 and 800 have four ports, the XL/XE two (four with a MultiJoy). */
- (int)portsFor:(int)machine
{
    return a800_machine_is_xl(machine) && !a800session_get_int(_session, "multijoy", 0) ? 2 : A800_PORTS;
}

- (BOOL)layoutChanged
{
    const int m = [self machineNow], n = [self portsFor:m];
    if (m != _builtMachine || n != _builtPorts) return YES;
    for (int port = 0; port < n; port++)
        if (a800session_port_device(_session, port) != _builtDev[port]) return YES;
    return NO;
}

- (CGFloat)innerWidth
{
    return MAX(_kbdSize.width, 2 * PORT_W + PORT_GAP);
}

- (A800Cap *)capWithFace:(NSString *)face frame:(NSRect)frame
{
    A800Cap *c = [[A800Cap alloc] initWithFrame:frame];
    c.face = face;
    c.owner = self;
    return c;
}

- (void)buildKeyboard
{
    const a800_kbd_key *keys = NULL;
    const int n = a800session_keyboard_layout(_session, &keys);
    float ew = 0, eh = 0;

    [_kbdView setSubviews:@[]];
    for (int i = 0; keys && i < n; i++) {
        const a800_kbd_key *k = &keys[i];
        const CGFloat x0 = px(k->x), y0 = px(k->y);
        const CGFloat x1 = px(k->x + k->w), y1 = px(k->y + k->h);
        A800Cap *c = [self capWithFace:nsstr(k->label)
                                 frame:NSMakeRect(x0 + KBD_GAP / 2, y0 + KBD_GAP / 2,
                                                  x1 - x0 - KBD_GAP, y1 - y0 - KBD_GAP)];
        c.shifted = k->shifted ? nsstr(k->shifted) : nil;
        c.kbdIndex = k->index;
        c.padTarget = consoleTarget(k->label);
        c.keyCap = YES;
        c.latches = k->label && (!strcmp(k->label, "SHIFT") || !strcmp(k->label, "CTRL"));
        [_kbdView addSubview:c];
        [_caps addObject:c];
        if (k->x + k->w > ew) ew = k->x + k->w;
        if (k->y + k->h > eh) eh = k->y + k->h;
    }
    _kbdSize = NSMakeSize(px(ew), px(eh));
}

- (void)stickCap:(NSString *)face port:(int)port action:(int)act col:(int)col row:(int)row
               x:(CGFloat)x y:(CGFloat)y
{
    A800Cap *c = [self capWithFace:face frame:NSMakeRect(x + col * (CELL_W + CELL_GAP),
                                                         y + row * (CELL_H + CELL_GAP), CELL_W, CELL_H)];
    c.padTarget = A800_TARGET_PORT(port, act);
    [_portsView addSubview:c];
    [_caps addObject:c];
}

/* A pair of paddles: a slider each (228 at the left .. 0 at the right, as
 * the POT counters read: clockwise lowers it) and a fire button, numbered as
 * Atari BASIC's PADDLE() does. */
- (void)buildPaddles:(int)port inRect:(NSRect)r
{
    for (int p = 0; p < 2; p++) {
        const int line = port * 2 + p;
        const CGFloat y = r.origin.y + p * PADDLE_ROW_H;

        NSTextField *name = [NSTextField labelWithString:[NSString stringWithFormat:@"Paddle %d", line]];
        [name setTextColor:[NSColor secondaryLabelColor]];
        [name setFrame:NSMakeRect(r.origin.x, y, 120, 16)];
        [_portsView addSubview:name];

        /* the slider runs 0 at the left: the value is PADDLE_MAX less it */
        NSSlider *s = [[NSSlider alloc] initWithFrame:NSMakeRect(r.origin.x, y + 22,
                                                                 r.size.width - CELL_W - CELL_GAP - 4, 24)];
        [s setMinValue:0];
        [s setMaxValue:PADDLE_MAX];
        [s setDoubleValue:PADDLE_MAX - _paddleValue[port][p]];
        [s setContinuous:YES];
        [s setTarget:self];
        [s setAction:@selector(paddleMoved:)];
        [s setTag:line];
        [s setRefusesFirstResponder:YES];
        [_portsView addSubview:s];
        _paddleSlider[port][p] = s;

        A800Cap *fire = [self capWithFace:@"Fire" frame:NSMakeRect(r.origin.x + r.size.width - CELL_W, y + 18,
                                                                   CELL_W, 32)];
        fire.paddleLine = line;
        fire.lit = _paddleHeld[port][p];
        [_portsView addSubview:fire];
        _fireCap[port][p] = fire;
    }
}

- (void)buildPort:(int)port inRect:(NSRect)r
{
    const int dev = _builtDev[port];

    NSTextField *title = [NSTextField labelWithString:[NSString stringWithFormat:@"Port %d", port + 1]];
    [title setFont:[NSFont boldSystemFontOfSize:12]];
    [title setFrame:NSMakeRect(r.origin.x, r.origin.y, 70, 16)];
    [_portsView addSubview:title];
    NSTextField *device = [NSTextField labelWithString:nsstr(a800_device_name(dev))];
    [device setTextColor:[NSColor secondaryLabelColor]];
    [device setAlignment:NSTextAlignmentRight];
    [device setLineBreakMode:NSLineBreakByTruncatingTail];
    [device setFrame:NSMakeRect(r.origin.x + 70, r.origin.y, r.size.width - 70, 16)];
    [_portsView addSubview:device];

    const CGFloat top = r.origin.y + TITLE_H + 4;
    if (dev == A800_DEV_JOYSTICK) {
        /* the stick's four directions around its fire button */
        const CGFloat jx = r.origin.x + floor((r.size.width - JOY_W) / 2);
        [self stickCap:@"▲" port:port action:A800_ACT_UP col:1 row:0 x:jx y:top];
        [self stickCap:@"◀" port:port action:A800_ACT_LEFT col:0 row:1 x:jx y:top];
        [self stickCap:@"Fire" port:port action:A800_ACT_FIRE col:1 row:1 x:jx y:top];
        [self stickCap:@"▶" port:port action:A800_ACT_RIGHT col:2 row:1 x:jx y:top];
        [self stickCap:@"▼" port:port action:A800_ACT_DOWN col:1 row:2 x:jx y:top];
    } else if (dev == A800_DEV_PADDLES) {
        [self buildPaddles:port inRect:NSMakeRect(r.origin.x, top, r.size.width, JOY_H)];
    } else {
        NSString *text = dev == A800_DEV_CX85 ? @"Type on the host's numeric keypad."
                       : dev == A800_DEV_NONE ? @""
                       : @"The pointer over the picture drives it.";
        NSTextField *hint = [NSTextField wrappingLabelWithString:text];
        [hint setTextColor:[NSColor secondaryLabelColor]];
        [hint setAlignment:NSTextAlignmentCenter];
        [hint setFrame:NSMakeRect(r.origin.x, top + 24, r.size.width, 48)];
        [_portsView addSubview:hint];
    }
}

/* Two ports to a row: four side by side would be wider than the keyboard. */
- (void)buildPorts
{
    const CGFloat x0 = floor(([self innerWidth] - (2 * PORT_W + PORT_GAP)) / 2);
    const int rows = (_builtPorts + 1) / 2;
    NSMutableArray<NSValue *> *frames = [NSMutableArray array];

    [_portsView setSubviews:@[]];
    for (int port = 0; port < _builtPorts; port++) {
        const NSRect r = NSMakeRect(x0 + (port % 2) * (PORT_W + PORT_GAP), (port / 2) * (PORT_H + PORT_GAP),
                                    PORT_W, PORT_H);
        _builtDev[port] = a800session_port_device(_session, port);
        [frames addObject:[NSValue valueWithRect:r]];
        [self buildPort:port inRect:NSInsetRect(r, PORT_PAD, PORT_PAD)];
    }
    _portsView.frames = frames;
    _portsHeight = rows > 0 ? rows * PORT_H + (rows - 1) * PORT_GAP : 0;
    [_portsView setNeedsDisplay:YES];
}

/* A new content size with the top-left corner where it was. */
- (void)fitPanel:(NSSize)size
{
    const NSRect frame = [_panel frame];
    NSRect content = [_panel contentRectForFrameRect:frame];
    if (NSEqualSizes(content.size, size)) return;
    content.size = size;
    NSRect next = [_panel frameRectForContentRect:content];
    next.origin.x = frame.origin.x;
    next.origin.y = NSMaxY(frame) - next.size.height;
    [_panel setFrame:next display:YES];
}

- (void)relayout
{
    const CGFloat w = [self innerWidth];
    const CGFloat kbdY = MARGIN;
    const CGFloat portsY = kbdY + _kbdSize.height + MARGIN;
    const CGFloat padsY = portsY + _portsHeight + MARGIN;
    const CGFloat rowsY = padsY + TITLE_H;
    const CGFloat mapY = rowsY + MAX_PAD_ROWS * ROW_H + MARGIN;
    const CGFloat hintX = MAP_W + 6 + DEFAULTS_W + 10;

    [self fitPanel:NSMakeSize(MARGIN + w + MARGIN, mapY + MAP_ROW_H + MARGIN)];
    [_kbdView setFrame:NSMakeRect(MARGIN + floor((w - _kbdSize.width) / 2), kbdY,
                                  _kbdSize.width, _kbdSize.height)];
    [_portsView setFrame:NSMakeRect(MARGIN, portsY, w, _portsHeight)];
    [_padsTitle setFrame:NSMakeRect(MARGIN, padsY, 200, 16)];
    [_noPads setFrame:NSMakeRect(MARGIN, rowsY + 4, w, 18)];
    for (int i = 0; i < MAX_PAD_ROWS; i++) {
        const CGFloat y = rowsY + i * ROW_H;
        [_padName[i] setFrame:NSMakeRect(MARGIN, y + 4, w - POPUP_W - EFF_W - 16, 18)];
        [_padEff[i] setFrame:NSMakeRect(MARGIN + w - POPUP_W - EFF_W - 8, y + 4, EFF_W, 18)];
        [_padAssign[i] setFrame:NSMakeRect(MARGIN + w - POPUP_W, y, POPUP_W, 25)];
    }
    [_mapButton setFrame:NSMakeRect(MARGIN, mapY, MAP_W, MAP_ROW_H)];
    [_defaultsButton setFrame:NSMakeRect(MARGIN + MAP_W + 6, mapY, DEFAULTS_W, MAP_ROW_H)];
    [_hint setFrame:NSMakeRect(MARGIN + hintX, mapY + 1, w - hintX, MAP_ROW_H)];
}

- (void)rebuild
{
    [self letGo];
    [_caps removeAllObjects];
    for (int port = 0; port < A800_PORTS; port++)
        for (int p = 0; p < 2; p++) {
            _paddleSlider[port][p] = nil;
            _fireCap[port][p] = nil;
        }
    _builtMachine = [self machineNow];
    _builtPorts = [self portsFor:_builtMachine];
    [self buildKeyboard];
    [self buildPorts];
    [self relayout];
    [self refreshLabels];
}

/* ---- pressing ------------------------------------------------------------------- */

- (void)paddleSend:(int)line
{
    const int port = line / 2, p = line % 2;
    a800session_paddle(_session, port, p, _paddleValue[port][p], _paddleHeld[port][p]);
}

- (void)paddleMoved:(NSSlider *)sender
{
    const int line = (int)sender.tag;
    if (line < 0 || line >= 2 * A800_PORTS) return;
    _paddleValue[line / 2][line % 2] = PADDLE_MAX - (int)(sender.doubleValue + 0.5);
    [self paddleSend:line];
}

/* Lets go of everything the mouse holds in this panel. */
- (void)letGo
{
    if (_mouseKey >= 0) {
        a800session_keyboard_press(_session, _mouseKey, 0);
        _mouseKey = -1;
    }
    if (_mouseTarget >= 0) {
        a800session_press(_session, _mouseTarget, 0);
        _mouseTarget = -1;
    }
    for (int line = 0; line < 2 * A800_PORTS; line++) {
        if (!_paddleHeld[line / 2][line % 2]) continue;
        _paddleHeld[line / 2][line % 2] = NO;
        [self paddleSend:line];
        _fireCap[line / 2][line % 2].lit = NO;
    }
}

- (void)capPressed:(A800Cap *)c down:(BOOL)down
{
    if (c.paddleLine >= 0) {
        const int line = c.paddleLine;
        if (down && _mapState != -2) return;
        if (_paddleHeld[line / 2][line % 2] == down) return;
        _paddleHeld[line / 2][line % 2] = down;
        [self paddleSend:line];
        c.lit = down;
        return;
    }
    if (_mapState != -2) {
        /* armed: picks (or changes) the target */
        if (down && c.padTarget >= 0) [self setMapState:c.padTarget];
        return;
    }
    if (c.kbdIndex >= 0) {
        if (down) {
            [self letGo];
            _mouseKey = c.kbdIndex;
            a800session_keyboard_press(_session, c.kbdIndex, 1);
        } else if (_mouseKey == c.kbdIndex) {
            _mouseKey = -1;
            a800session_keyboard_press(_session, c.kbdIndex, 0);
        }
    } else if (c.padTarget >= 0) {
        if (down) {
            [self letGo];
            _mouseTarget = c.padTarget;
            a800session_press(_session, c.padTarget, 1);
        } else if (_mouseTarget == c.padTarget) {
            _mouseTarget = -1;
            a800session_press(_session, c.padTarget, 0);
        }
    }
    [self pollHeld];
}

/* What the Atari sees held -- host keyboard, gamepads and this panel -- lit. */
- (void)pollHeld
{
    unsigned held[A800_PORTS];
    if (_mapState != -2) return;
    for (int port = 0; port < A800_PORTS; port++)
        held[port] = a800session_buttons_held(_session, port);
    for (A800Cap *c in _caps) {
        BOOL on = NO;
        if (c.kbdIndex >= 0)
            on = c.kbdIndex == _mouseKey || a800session_keyboard_held(_session, c.kbdIndex);
        else if (c.padTarget >= 0 && c.padTarget < A800_TARGET_CONSOLE(0))
            on = c.padTarget == _mouseTarget
                 || ((held[c.padTarget / A800_ACT_PER_PORT] >> (c.padTarget % A800_ACT_PER_PORT)) & 1u);
        c.lit = on;
    }
}

/* ---- Map mode ------------------------------------------------------------------- */

- (NSString *)bindingText:(int)target
{
    const a800_binding b = a800session_binding_get(_session, target);
    NSString *key = keyName(b.keysym);
    NSString *text = key.length ? key : @"—";
    if (b.button != A800_PAD_BTN_NONE)
        text = [NSString stringWithFormat:@"%@\n%@", text, nsstr(a800_pad_button_name(b.button))];
    return text;
}

- (NSString *)bindingTip:(int)target
{
    const a800_binding b = a800session_binding_get(_session, target);
    NSString *key = keyName(b.keysym);
    NSString *tip = [NSString stringWithFormat:@"%@: %@", nsstr(a800_target_name(target)),
                                               key.length ? key : @"no key"];
    if (b.button != A800_PAD_BTN_NONE)
        tip = [NSString stringWithFormat:@"%@, %@", tip, nsstr(a800_pad_button_name(b.button))];
    return tip;
}

/* In Map mode each remappable control shows what is bound to it, so
 * choosing what to change does not take remembering the whole table; the
 * keys Map cannot rebind are greyed. */
- (void)refreshLabels
{
    const BOOL mapping = _mapState != -2;
    for (A800Cap *c in _caps) {
        if (mapping && c.padTarget >= 0) {
            c.mapText = [self bindingText:c.padTarget];
            c.toolTip = [self bindingTip:c.padTarget];
            c.greyed = NO;
            c.lit = c.padTarget == _mapState;
        } else {
            c.mapText = nil;
            c.toolTip = (!mapping && c.latches) ? @"Latches for the next key" : nil;
            c.greyed = mapping;
            c.lit = NO;
        }
    }
    for (int port = 0; port < A800_PORTS; port++)
        for (int p = 0; p < 2; p++) {
            [_paddleSlider[port][p] setEnabled:!mapping];
            _fireCap[port][p].greyed = mapping;
        }
    [self pollHeld];
}

- (void)setMapState:(int)state
{
    _mapState = state;
    if (state >= 0) {
        [self letGo];
        a800session_gamepad_capture_begin(_session);
        _hint.stringValue = [NSString stringWithFormat:@"Press a key or gamepad button for %@",
                                                       nsstr(a800_target_name(state))];
    } else {
        a800session_gamepad_capture_cancel(_session);
        _hint.stringValue = state == -1 ? @"Click a joystick control or console key to remap"
                                        : @"Click and hold to press. SHIFT and CTRL latch for the next key.";
    }
    [_mapButton setTitle:(state == -2 ? @"Map" : @"Done")];
    _mapButton.bezelColor = state == -2 ? nil : A800AccentColor();
    [self refreshLabels];
}

- (void)showBound:(NSString *)name stolen:(const char *)stolen
{
    _hint.stringValue = stolen[0] ? [NSString stringWithFormat:@"Bound %@ (was %@)", name, nsstr(stolen)]
                                  : [NSString stringWithFormat:@"Bound %@", name];
}

- (void)bindKey:(uint32_t)keysym swallow:(BOOL)swallow
{
    char stolen[128];
    stolen[0] = '\0';
    a800session_binding_set_key(_session, _mapState, keysym, stolen, (int)sizeof stolen);
    if (swallow) _swallowKey = keysym;
    [self setMapState:-1];   /* stay armed: remapping several in a row is normal */
    [self showBound:keyName(keysym) stolen:stolen];
}

- (void)pollCapture
{
    int button = A800_PAD_BTN_NONE;
    char stolen[128];
    if (_mapState < 0 || !a800session_gamepad_capture_poll(_session, &button)) return;
    stolen[0] = '\0';
    a800session_binding_set_button(_session, _mapState, button, stolen, (int)sizeof stolen);
    [self setMapState:-1];   /* stay armed: remapping several in a row is normal */
    [self showBound:nsstr(a800_pad_button_name(button)) stolen:stolen];
}

- (void)toggleMap:(id)sender
{
    (void)sender;
    [self setMapState:(_mapState == -2 ? -1 : -2)];
}

- (void)restoreDefaults:(id)sender
{
    (void)sender;
    a800session_bindings_reset(_session);
    [self refreshLabels];
    _hint.stringValue = @"Default bindings restored";
}

/* ---- gamepads ------------------------------------------------------------------- */

- (void)refreshPadPorts
{
    for (int i = 0; i < _padRows; i++) {
        const int eff = a800session_gamepad_effective_port(_session, i);
        NSString *text = eff >= 0 ? [NSString stringWithFormat:@"drives port %d", eff + 1] : @"drives nothing";
        if (![_padEff[i].stringValue isEqualToString:text]) _padEff[i].stringValue = text;
    }
}

- (void)refreshPads
{
    const int n = a800session_gamepad_count(_session);
    _padGeneration = a800session_gamepad_generation(_session);
    _padRows = n < MAX_PAD_ROWS ? (n > 0 ? n : 0) : MAX_PAD_ROWS;
    _noPads.hidden = n > 0;
    for (int i = 0; i < MAX_PAD_ROWS; i++) {
        const BOOL shown = i < _padRows;
        _padName[i].hidden = !shown;
        _padEff[i].hidden = !shown;
        _padAssign[i].hidden = !shown;
        if (!shown) continue;
        char name[128];
        name[0] = '\0';
        a800session_gamepad_name(_session, i, name, (int)sizeof name);
        _padName[i].stringValue = nsstr(name);
        _padName[i].toolTip = nsstr(name);
        const int assign = a800session_gamepad_assignment(_session, i);
        [_padAssign[i] selectItemAtIndex:(assign < 0 || assign >= A800_PORTS ? 0 : assign + 1)];
    }
    [self refreshPadPorts];
}

- (void)padAssignChanged:(NSPopUpButton *)sender
{
    const int sel = (int)sender.indexOfSelectedItem;
    a800session_gamepad_assign(_session, (int)sender.tag, sel <= 0 ? -1 : sel - 1);
    [self refreshPadPorts];
}

/* 30 Hz while shown: what is held and the gamepad capture; every 250 ms the
 * hot-plugged pads, auto-assignment, and the machine or a port's device
 * changed in Preferences. */
- (void)tick
{
    if (_mapState >= 0) [self pollCapture];
    if (++_slow >= SLOW_TICKS) {
        _slow = 0;
        if (a800session_gamepad_generation(_session) != _padGeneration) [self refreshPads];
        else [self refreshPadPorts];
        if ([self layoutChanged]) [self rebuild];
    }
    [self pollHeld];
}

/* ---- the keyboard --------------------------------------------------------------- */

/* The app's own keys, which A800ForwardKey leaves to the caller: F9 hides
 * this panel; F11, F12 and Shift+F5 do what they do in the main window. */
- (BOOL)appKey:(NSEvent *)e keysym:(uint32_t)ks
{
    const NSEventModifierFlags flags = [e modifierFlags];
    if (flags & NSEventModifierFlagCommand) return NO;
    if (ks == A800_KEYSYM_F9) {
        if (![e isARepeat]) [self toggle];
        return YES;
    }
    if (ks == A800_KEYSYM_F11 || ks == A800_KEYSYM_F12 ||
        (ks == A800_KEYSYM_F5 && (flags & NSEventModifierFlagShift))) {
        NSWindow *mainWindow = [NSApp mainWindow];
        if (![e isARepeat] && mainWindow && mainWindow != _panel) [[mainWindow firstResponder] keyDown:e];
        return YES;
    }
    return NO;
}

- (void)panelKeyDown:(NSEvent *)e
{
    const uint32_t ks = A800KeysymFromEvent(e);
    if (_swallowKey && ks == _swallowKey) return;
    if ([self appKey:e keysym:ks]) return;
    const BOOL plain = ![e isARepeat] && !([e modifierFlags] & NSEventModifierFlagCommand);
    if (_mapState >= 0) {
        /* the next key is the binding */
        if (plain && ks) [self bindKey:ks swallow:YES];
        return;
    }
    if (plain && ks) {
        /* a key bound to a system action goes to the app's queue, as a
         * gamepad's does */
        const int sa = a800session_key_sysaction(_session, ks);
        if (sa >= 0) {
            a800session_sysaction_post(_session, sa);
            return;
        }
    }
    (void)A800ForwardKey(_session, e, YES);
}

- (void)panelKeyUp:(NSEvent *)e
{
    const uint32_t ks = A800KeysymFromEvent(e);
    if (_swallowKey && ks == _swallowKey) {
        _swallowKey = 0;
        return;
    }
    (void)A800ForwardKey(_session, e, NO);
}

/* Shift, Control and Option bind too (Command is the menus'); otherwise
 * they are the Atari's. A release always goes through. */
- (void)panelFlagsChanged:(NSEvent *)e
{
    if (_mapState >= 0) {
        const uint32_t ks = a800session_keysym_from_macos_keycode([e keyCode]);
        NSEventModifierFlags mask = 0;
        if (ks == A800_KEYSYM_LSHIFT || ks == A800_KEYSYM_RSHIFT) mask = NSEventModifierFlagShift;
        else if (ks == A800_KEYSYM_LCTRL || ks == A800_KEYSYM_RCTRL) mask = NSEventModifierFlagControl;
        else if (ks == A800_KEYSYM_LALT || ks == A800_KEYSYM_RALT) mask = NSEventModifierFlagOption;
        if (mask && ([e modifierFlags] & mask)) {
            [self bindKey:ks swallow:NO];
            return;
        }
    }
    A800ForwardFlags(_session, e);
}

/* ---- the window ----------------------------------------------------------------- */

/* Hidden: Map disarmed, nothing held, no polling. */
- (void)quiesce
{
    [self setMapState:-2];
    [self letGo];
    _swallowKey = 0;
    [_timer invalidate];
    _timer = nil;
}

- (void)show
{
    if ([self layoutChanged]) [self rebuild];
    [self refreshPads];
    [self setMapState:-2];
    if (!_placed) {
        [_panel center];
        _placed = YES;
    }
    if (!_timer) {
        __weak A800ControllersWindow *weakSelf = self;
        _timer = [NSTimer scheduledTimerWithTimeInterval:1.0 / 30 repeats:YES block:^(NSTimer *t) {
            (void)t;
            [weakSelf tick];
        }];
    }
    [_panel makeFirstResponder:nil];    /* the panel itself takes the keys */
    [_panel makeKeyAndOrderFront:nil];
}

- (void)hide
{
    [self quiesce];
    [_panel orderOut:nil];
}

- (void)toggle
{
    if ([self isVisible]) [self hide];
    else [self show];
}

- (BOOL)isVisible
{
    return [_panel isVisible];
}

- (void)refresh
{
    if ([self isVisible]) {
        [self rebuild];
        [self refreshPads];
    } else {
        _builtMachine = -1;     /* rebuilt when next shown */
    }
}

/* Only what this panel's mouse holds: the main window releases its own. */
- (void)windowDidResignKey:(NSNotification *)note
{
    (void)note;
    [self letGo];
    _swallowKey = 0;    /* its release goes elsewhere */
}

- (void)windowWillClose:(NSNotification *)note
{
    (void)note;
    [self quiesce];
}
@end
