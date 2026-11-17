/*
 * A800ControllersWindow -- the AppKit Controllers panel: what is plugged into
 * each controller port (the CX40 joystick, the CX30 paddle pair, a mouse, the
 * Trak-Ball, the Koala Pad, the touch tablet, the light pen or gun, the CX85
 * keypad), the console keys, an optional Atari keyboard drawn for the machine
 * (400, 800, XL/XE, 1200XL), the gamepads and the port each one plays, and
 * Map mode. Everything is drawn from the session's tables
 * (a800session_controls_layout, a800session_keyboard_layout) with
 * NSBezierPath, so every frontend shows the same machine; the core hit-tests
 * the layout and does the drag maths (a800session_stick_from_vector and
 * friends).
 *
 * The art follows ~/Workspace/fujinet-go-all/controllers-port/DESIGN-atari.md
 * (section numbers in the comments). The CX40 joystick and CX30 paddle are the
 * Atari 2600 desktop's drawing (the same hardware plugs into both machines).
 *
 * Gestures: press a button, a key or a console key and it is held until it is
 * released or dragged off; press in the joystick's ring and drag to steer it;
 * drag on a paddle's knob to turn it (a paddle holds where it is left) or hold
 * a turn arrow, the wheel nudges it; drag on a mouse mat, the Trak-Ball or a
 * pad to move the pointer device. SHIFT and CTRL on the keyboard latch for the
 * next key (the core does that). The pointer is its own input source, so the
 * panel lets go of everything the mouse holds when it is hidden or loses key
 * status. What the Atari sees held, from any source (the host keyboard, a
 * gamepad, the mouse), is painted in orange, so the panel doubles as an input
 * tester for a new gamepad.
 *
 * Keys typed with the panel key go to the Atari as in the main window
 * (A800ForwardKey, A800ForwardFlags); no view here takes the keyboard, so the
 * panel itself is the first responder.
 *
 * FIXED SIZE: an NSPanel without a resize mask, sized from its contents. Only
 * the number of ports, the Keyboard check box and more pads than the two rows
 * kept for them change its height (the top edge stays put). Hidden at start,
 * ordered out rather than closed, so its position survives. The 30 Hz poll runs
 * only while the panel can be seen.
 *
 * MAP MODE: Map arms a two-step rebind -- click a control on the picture (the
 * stick's four directions show as small labels; the fire button; the console
 * keys, on the strip or on the keyboard) or choose one from the list, then press
 * a key or a gamepad button; the next one is bound and whatever held it is
 * told so in the status line. Esc backs out of the pick. Rebinding steals the
 * key from its old target. F9, F11 and F12 are the app's and never bound.
 *
 * Written to the family's AppKit pattern (see the Atari 2600, Apple II, ADAM
 * and Astrocade panels); built and verified on CI only (no Apple toolchain
 * here).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "ControllersWindow.h"

#import "../KeyForward.h"
#import "../help/HelpWindow.h"

#include <math.h>
#include <string.h>

#define UNIT         21.0     /* points per layout unit */
#define WIN_W        664.0    /* the layout's 30.6 units, and a margin each side */
#define MARGIN       16.0
#define STAGE_PAD    10.0
#define KBD_UNIT     38.0     /* points per key unit */
#define KBD_GAP       3.0     /* between key caps */
#define KBD_PLATE     8.0     /* the keyboard's case around the caps */
#define TITLE_H      18.0
#define ROW_H        24.0
#define EFF_W       130.0
#define POPUP_W     130.0
#define BTN_H        28.0
#define MIN_PAD_ROWS 2        /* rows kept for pads, more when more are connected */
#define MAX_PAD_ROWS 4
#define SLOW_TICKS   8        /* the 30 Hz tick's 250 ms part */
#define PILL_W       58.0
#define PILL_H       18.0
#define MAX_PILLS    48
#define MAX_KEYS     96
#define NUDGE        8        /* the wheel's step of a paddle's value */

/* ---- colours (DESIGN-atari.md: [P] read from a photo, [I] inferred) -------------- */

#define COL_STAGE_TOP   0x6B6C72
#define COL_STAGE_BOT   0x55565B
#define COL_STAGE_EDGE  0x3A3B3F
#define COL_CAPTION     0xEEEEF2
#define COL_BLACK       0x1C1C1C   /* plastic black [P] */
#define COL_BLACK_TOP   0x262626
#define COL_BEVEL       0x2A2A2A
#define COL_DEEP        0x101010
#define COL_RED         0xE23B1F   /* fire red [P] */
#define COL_RED_HI      0xF05A38
#define COL_RED_DOWN    0xB02A14
#define COL_HELD        0xFFB04A   /* held highlight on dark plastic [I] */
#define COL_PILL        0x1A1A1C
#define COL_CONSOLE     0x2B2724   /* the console strip */

/* ---- helpers -------------------------------------------------------------------- */

static NSString *nsstr(const char *text)
{
    NSString *res = text ? [NSString stringWithUTF8String:text] : nil;
    return res ? res : @"";
}

static NSString *keyName(uint32_t keysym)
{
    char name[48];
    name[0] = '\0';
    if (keysym) a800session_keysym_name(keysym, name, (int)sizeof name);
    return nsstr(name);
}

static NSColor *rgb(uint32_t value)
{
    return [NSColor colorWithSRGBRed:((value >> 16) & 0xff) / 255.0
                               green:((value >> 8) & 0xff) / 255.0
                                blue:(value & 0xff) / 255.0
                               alpha:1.0];
}

static NSColor *rgba(uint32_t value, CGFloat alpha)
{
    return [NSColor colorWithSRGBRed:((value >> 16) & 0xff) / 255.0
                               green:((value >> 8) & 0xff) / 255.0
                                blue:(value & 0xff) / 255.0
                               alpha:alpha];
}

/* The colour a fraction of the way from a to b. */
static uint32_t mixRgb(uint32_t from, uint32_t to, double frac)
{
    uint32_t out = 0;
    for (int shift = 16; shift >= 0; shift -= 8) {
        const double ca = (double)((from >> shift) & 0xff), cb = (double)((to >> shift) & 0xff);
        out |= (uint32_t)lround(ca + (cb - ca) * frac) << shift;
    }
    return out;
}

/* The accent for text and fills in AppKit controls (the 800's rust, white text on it). */
static NSColor *accent(void) { return A800AccentColor(); }

/* Toggled-on look for a push button: accent bezel, white title. */
static void setButtonAccent(NSButton *button, BOOL on)
{
    if (!button) return;
    NSString *title = [button.title copy];
    if (on) {
        NSMutableParagraphStyle *para = [[NSMutableParagraphStyle alloc] init];
        para.alignment = NSTextAlignmentCenter;
        button.bezelColor = accent();
        button.attributedTitle = [[NSAttributedString alloc]
            initWithString:title
                attributes:@{
                    NSFontAttributeName : button.font ?: [NSFont systemFontOfSize:NSFont.systemFontSize],
                    NSForegroundColorAttributeName : NSColor.whiteColor,
                    NSParagraphStyleAttributeName : para
                }];
    } else {
        button.bezelColor = nil;
        button.title = title;
    }
}

/* A point and a rectangle in a device's own layout units, from its box's
 * top-left corner (DESIGN-atari.md draws every device in such a box). */
static inline NSPoint atp(NSPoint org, CGFloat uu, CGFloat vv)
{
    return NSMakePoint(org.x + uu * UNIT, org.y + vv * UNIT);
}

static inline NSRect atr(NSPoint org, CGFloat uu, CGFloat vv, CGFloat ww, CGFloat hh)
{
    return NSMakeRect(org.x + uu * UNIT, org.y + vv * UNIT, ww * UNIT, hh * UNIT);
}

/* The square round shapes sit in: centre (cu, cv), radius rr. */
static inline NSRect discRect(NSPoint org, CGFloat cu, CGFloat cv, CGFloat rr)
{
    return NSMakeRect(org.x + (cu - rr) * UNIT, org.y + (cv - rr) * UNIT, 2 * rr * UNIT, 2 * rr * UNIT);
}

/* A linear gradient clipped to a path, from a to b, with alpha at each end. */
static void fillLinear(NSBezierPath *path, NSPoint from, NSPoint to, uint32_t rgbA, CGFloat alphaA,
                       uint32_t rgbB, CGFloat alphaB)
{
    const CGFloat comps[8] = {
        ((rgbA >> 16) & 0xff) / 255.0, ((rgbA >> 8) & 0xff) / 255.0, (rgbA & 0xff) / 255.0, alphaA,
        ((rgbB >> 16) & 0xff) / 255.0, ((rgbB >> 8) & 0xff) / 255.0, (rgbB & 0xff) / 255.0, alphaB };
    CGContextRef ctx = [[NSGraphicsContext currentContext] CGContext];
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGGradientRef grad = CGGradientCreateWithColorComponents(space, comps, NULL, 2);
    [NSGraphicsContext saveGraphicsState];
    [path addClip];
    CGContextDrawLinearGradient(ctx, grad, CGPointMake(from.x, from.y), CGPointMake(to.x, to.y),
                                kCGGradientDrawsBeforeStartLocation | kCGGradientDrawsAfterEndLocation);
    [NSGraphicsContext restoreGraphicsState];
    CGGradientRelease(grad);
    CGColorSpaceRelease(space);
}

/* A vertical gradient clipped to a path (the views here are flipped, so "top"
 * is the smaller y). */
static void fillGradient(NSBezierPath *path, uint32_t topRgb, uint32_t botRgb)
{
    const NSRect bnd = [path bounds];
    fillLinear(path, NSMakePoint(NSMinX(bnd), NSMinY(bnd)), NSMakePoint(NSMinX(bnd), NSMaxY(bnd)),
               topRgb, 1.0, botRgb, 1.0);
}

/* Lines of text centred in rect, the font shrunk (to 5 pt) until they fit. */
static void drawFittedStyled(NSString *text, NSRect rect, CGFloat size, BOOL bold, BOOL oblique, NSColor *ink)
{
    NSArray<NSString *> *lines = [text componentsSeparatedByString:@"\n"];
    const CGFloat count = (CGFloat)[lines count];
    NSMutableParagraphStyle *para = [[NSMutableParagraphStyle alloc] init];
    para.alignment = NSTextAlignmentCenter;
    para.lineBreakMode = NSLineBreakByClipping;
    NSDictionary *attrs = nil;
    CGFloat lineHeight = 0;
    for (;;) {
        NSFont *font = bold ? [NSFont boldSystemFontOfSize:size] : [NSFont systemFontOfSize:size];
        NSMutableDictionary *dict = [NSMutableDictionary dictionaryWithDictionary:@{
            NSFontAttributeName: font,
            NSParagraphStyleAttributeName: para,
            NSForegroundColorAttributeName: ink }];
        if (oblique) dict[NSObliquenessAttributeName] = @(0.2);
        attrs = dict;
        CGFloat widest = 0;
        lineHeight = 0;
        for (NSString *line in lines) {
            const NSSize extent = [line sizeWithAttributes:attrs];
            if (extent.width > widest) widest = extent.width;
            if (extent.height > lineHeight) lineHeight = extent.height;
        }
        if ((widest <= rect.size.width && lineHeight * count <= rect.size.height + 2) || size <= 5) break;
        size -= 0.5;
    }
    CGFloat ypos = rect.origin.y + floor((rect.size.height - lineHeight * count) / 2);
    for (NSString *line in lines) {
        [line drawInRect:NSMakeRect(rect.origin.x, ypos, rect.size.width, lineHeight) withAttributes:attrs];
        ypos += lineHeight;
    }
}

static void drawFitted(NSString *text, NSRect rect, CGFloat size, BOOL bold, NSColor *ink)
{
    drawFittedStyled(text, rect, size, bold, NO, ink);
}

static void strokeLine(NSPoint from, NSPoint to, CGFloat width, NSColor *colour)
{
    NSBezierPath *line = [NSBezierPath bezierPath];
    [line moveToPoint:from];
    [line lineToPoint:to];
    line.lineWidth = width;
    line.lineCapStyle = NSLineCapStyleRound;
    [colour setStroke];
    [line stroke];
}

static void fillOval(NSRect rect, NSColor *colour)
{
    [colour setFill];
    [[NSBezierPath bezierPathWithOvalInRect:rect] fill];
}

/* A regular polygon with `sides` corners, one at angle `start` degrees from
 * straight up, clockwise (an octagon cap). */
static NSBezierPath *polygon(NSPoint ctr, CGFloat rad, int sides, double start)
{
    NSBezierPath *path = [NSBezierPath bezierPath];
    for (int idx = 0; idx < sides; idx++) {
        const double ang = (start + 360.0 * idx / sides) * M_PI / 180.0;
        const NSPoint pnt = NSMakePoint(ctr.x + rad * sin(ang), ctr.y - rad * cos(ang));
        if (idx == 0) [path moveToPoint:pnt]; else [path lineToPoint:pnt];
    }
    [path closePath];
    return path;
}

/* A label on a dark pill, filled with the accent when picked. */
static void drawPill(NSRect rect, NSString *text, BOOL picked)
{
    NSBezierPath *pill = [NSBezierPath bezierPathWithRoundedRect:rect xRadius:rect.size.height / 2
                                                         yRadius:rect.size.height / 2];
    [(picked ? accent() : rgba(COL_PILL, 0.85)) setFill];
    [pill fill];
    drawFitted(text, NSInsetRect(rect, 6, 1), 10, YES, NSColor.whiteColor);
}

/* What the stick is deflected to from the directions the Atari sees held (the
 * opposed pairs are already cancelled): -1..1 on each axis, at most 1 long. */
static void deflection(unsigned held, float *ax, float *ay)
{
    float xval = (float)(((held >> A800_ACT_RIGHT) & 1u)) - (float)(((held >> A800_ACT_LEFT) & 1u));
    float yval = (float)(((held >> A800_ACT_DOWN) & 1u)) - (float)(((held >> A800_ACT_UP) & 1u));
    const float len = hypotf(xval, yval);
    if (len > 1.0f) {
        xval /= len;
        yval /= len;
    }
    *ax = xval;
    *ay = yval;
}

/* Points from key units, rounded so neighbouring caps keep an even gap. */
static CGFloat px(float units)
{
    return floor(units * KBD_UNIT + 0.5);
}

/* ---- views ---------------------------------------------------------------------- */

/* Laid out from the top, like the other frontends' panels. */
@interface A800CtlFlippedView : NSView
@end

@implementation A800CtlFlippedView
- (BOOL)isFlipped { return YES; }
@end

/* Keys typed with the panel key go to its owner. */
@class A800ControllersWindow;
@interface A800ControllersPanel : NSPanel
@property (nonatomic, weak) A800ControllersWindow *owner;
@end

@interface A800ControllersWindow () <NSWindowDelegate>
- (void)mapPick:(int)target;
- (NSString *)legendForTarget:(int)target;
- (void)panelKeyDown:(NSEvent *)evt;
- (void)panelKeyUp:(NSEvent *)evt;
- (void)panelFlagsChanged:(NSEvent *)evt;
@end

/* One label the stage shows over a control while Map is armed or Show bindings
 * is on. */
typedef struct {
    NSRect rect;
    int target;
    char text[64];
} Pill;

/* The stage: every port's device and the console strip on a grey backdrop.
 * Everything is drawn here and hit-tested by the core (the stick, the knob and
 * the pads' drag maths are the core's too). */
@interface A800StageView : A800CtlFlippedView
@property (nonatomic) a800session *session;
@property (nonatomic, weak) A800ControllersWindow *owner;
@property (nonatomic) BOOL mapping;          /* Map armed: bindings shown */
@property (nonatomic) int mapTarget;         /* the control waiting for a key, or -1 */
@property (nonatomic) BOOL showKeys;         /* the bound host key as a small legend */
@property (nonatomic, readonly) NSSize contentSize;
- (void)configure;
/* Whether the layout is out of date: a port's device, the ports or HELP changed. */
- (BOOL)layoutStale;
/* Reads what the Atari sees held; redraws when anything changed. */
- (void)poll;
/* Lets go of everything the pointer holds. */
- (void)letGo;
@end

@implementation A800StageView {
    a800_control _ctls[A800_CONTROLS_MAX];
    int _count;
    float _dimW, _dimH;
    int _devs[A800_PORTS];       /* the device drawn on each port */
    int _nports;
    int _xl;
    NSSize _size;

    unsigned _held[A800_PORTS];  /* what the Atari sees held, bit per a800_action */
    unsigned _conHeld;           /* bit per a800_console */
    int _knobVal[A800_PORTS][2]; /* each paddle's value, 228 (left) .. 0 (right) */

    /* the mouse */
    int _downIdx;                /* the control pressed, or -1 */
    int _downTarget;             /* the target the pointer holds with a press, or -1 */
    BOOL _stickDrag, _knobDrag, _padDrag, _moveDrag, _firePressed, _turnPressed, _cxPressed;
    unsigned _ptrMask;           /* the pointer device's buttons the mouse holds */
    NSPoint _lastPt;             /* the previous drag point, for the relative devices */
    NSPoint _touch;              /* where a pad is touched, 0..1 */
    CGFloat _scrollAcc;
    NSTrackingArea *_tracking;
}

- (instancetype)initWithFrame:(NSRect)frame
{
    self = [super initWithFrame:frame];
    if (!self) return nil;
    _downIdx = -1;
    _downTarget = -1;
    _mapTarget = -1;
    return self;
}

- (BOOL)isOpaque { return NO; }
- (BOOL)acceptsFirstMouse:(NSEvent *)evt { (void)evt; return YES; }
- (BOOL)mouseDownCanMoveWindow { return NO; }

- (NSSize)contentSize { return _size; }

/* The machine the layout is for: the ports it has and whether it has a HELP key. */
- (int)machineIsXl
{
    const int machine = a800session_get_int(_session, "machine", A800_M_130XE);
    return a800_machine_is_xl(machine >= 0 && machine < A800_M_COUNT ? machine : A800_M_130XE) ? 1 : 0;
}

/* (Re)reads the layout for what the ports have plugged in. */
- (void)loadLayout
{
    _nports = a800session_active_ports(_session);
    if (_nports < 1) _nports = 1;
    if (_nports > A800_PORTS) _nports = A800_PORTS;
    _xl = [self machineIsXl];
    for (int port = 0; port < A800_PORTS; port++)
        _devs[port] = a800session_port_device(_session, port);
    _count = a800session_controls_layout(_devs, _nports, _xl, _ctls, A800_CONTROLS_MAX, &_dimW, &_dimH);
    if (_count < 0) _count = 0;
    _size = NSMakeSize(WIN_W, STAGE_PAD * 2 + ceil(_dimH * UNIT));
    for (int port = 0; port < A800_PORTS; port++)
        for (int pad = 0; pad < 2; pad++)
            _knobVal[port][pad] = a800session_paddle_value(_session, port, pad);
    [self setNeedsDisplay:YES];
}

- (void)configure
{
    [self loadLayout];
}

/* Whether the layout is out of date: a port's device, the ports or HELP changed. */
- (BOOL)layoutStale
{
    int nports = a800session_active_ports(_session);
    if (nports < 1) nports = 1;
    if (nports > A800_PORTS) nports = A800_PORTS;
    if (nports != _nports || [self machineIsXl] != _xl) return YES;
    for (int port = 0; port < nports; port++)
        if (a800session_port_device(_session, port) != _devs[port]) return YES;
    return NO;
}

- (void)updateTrackingAreas
{
    if (_tracking) [self removeTrackingArea:_tracking];
    _tracking = [[NSTrackingArea alloc]
        initWithRect:NSZeroRect
             options:(NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited |
                      NSTrackingActiveAlways | NSTrackingInVisibleRect)
               owner:self
            userInfo:nil];
    [self addTrackingArea:_tracking];
    [super updateTrackingAreas];
}

/* ---- geometry ------------------------------------------------------------------ */

/* Where the layout's origin is, in view points. */
- (NSPoint)origin
{
    return NSMakePoint(floor((WIN_W - _dimW * UNIT) / 2), STAGE_PAD);
}

- (NSRect)rectOf:(const a800_control *)ctl
{
    const NSPoint org = [self origin];
    return NSMakeRect(org.x + ctl->x * UNIT, org.y + ctl->y * UNIT, ctl->w * UNIT, ctl->h * UNIT);
}

- (const a800_control *)ctlAt:(int)idx
{
    return idx >= 0 && idx < _count ? &_ctls[idx] : NULL;
}

/* The pressable control under a view point, or -1. */
- (int)hit:(NSPoint)pnt
{
    const NSPoint org = [self origin];
    return a800session_controls_hit(_ctls, _count, (float)((pnt.x - org.x) / UNIT), (float)((pnt.y - org.y) / UNIT));
}

/* The case a part belongs to: the last BODY entry before it on its port (and,
 * for a paddle's parts, its paddle), or NULL. */
- (const a800_control *)bodyOf:(const a800_control *)ctl
{
    const a800_control *found = NULL;
    const BOOL paddlePart = ctl->kind == A800_CTL_KNOB || ctl->kind == A800_CTL_PADDLE_FIRE || ctl->kind == A800_CTL_TURN;
    for (int idx = 0; idx < _count; idx++) {
        if (&_ctls[idx] == ctl) break;
        if (_ctls[idx].kind != A800_CTL_BODY || _ctls[idx].port != ctl->port) continue;
        if (paddlePart && _ctls[idx].arg != ctl->arg) continue;
        found = &_ctls[idx];
    }
    return found;
}

/* Whether the Atari sees a target held (the window is a gamepad tester). */
- (BOOL)heldTarget:(int)target
{
    if (self.mapping || target < 0) return NO;
    if (target < A800_TARGET_CONSOLE(0))
        return ((_held[target / A800_ACT_PER_PORT] >> (target % A800_ACT_PER_PORT)) & 1u) != 0;
    if (target < A800_TARGET_SYSACT(0))
        return ((_conHeld >> (target - A800_TARGET_CONSOLE(0))) & 1u) != 0;
    return NO;
}

- (BOOL)isDown:(const a800_control *)ctl
{
    return _downIdx >= 0 && _downIdx < _count && &_ctls[_downIdx] == ctl;
}

/* ---- the labels shown over the controls ---------------------------------------- */

- (int)pills:(Pill *)out max:(int)max
{
    int num = 0;
    if (!(self.mapping || self.showKeys)) return 0;
#define ADD_PILL(cx, cy, wid, tgt, prefix) do { \
        if (num < max) { \
            Pill *pill = &out[num++]; \
            pill->rect = NSMakeRect((cx) - (wid) / 2, (cy) - PILL_H / 2, (wid), PILL_H); \
            pill->target = (tgt); \
            NSString *name = [self.owner legendForTarget:(tgt)]; \
            NSString *text = [NSString stringWithFormat:@"%@%@", prefix, name.length ? name : @"-"]; \
            strncpy(pill->text, text.UTF8String, sizeof pill->text - 1); \
            pill->text[sizeof pill->text - 1] = '\0'; \
        } } while (0)
    for (int idx = 0; idx < _count; idx++) {
        const a800_control *ctl = &_ctls[idx];
        const NSRect rect = [self rectOf:ctl];
        const NSPoint ctr = NSMakePoint(NSMidX(rect), NSMidY(rect));
        switch (ctl->kind) {
        case A800_CTL_STICK: {
            const int port = ctl->port;
            ADD_PILL(ctr.x - 2.35 * UNIT, ctr.y, PILL_W, A800_TARGET_PORT(port, A800_ACT_LEFT), @"◀ ");
            ADD_PILL(ctr.x + 2.35 * UNIT, ctr.y, PILL_W, A800_TARGET_PORT(port, A800_ACT_RIGHT), @"▶ ");
            ADD_PILL(ctr.x, ctr.y - 1.6 * UNIT, PILL_W, A800_TARGET_PORT(port, A800_ACT_UP), @"▲ ");
            ADD_PILL(ctr.x, ctr.y + 1.6 * UNIT, PILL_W, A800_TARGET_PORT(port, A800_ACT_DOWN), @"▼ ");
            break;
        }
        case A800_CTL_CONSOLE_KEY:
            if (ctl->target >= 0) ADD_PILL(ctr.x, NSMaxY(rect), 46.0, ctl->target, @"");
            break;
        default:
            break;
        }
    }
#undef ADD_PILL
    return num;
}

/* The target Map would pick at a point (view points), or -1: a label, the fire
 * button or a console key. */
- (int)mapTargetAt:(NSPoint)pnt
{
    Pill pills[MAX_PILLS];
    const int num = [self pills:pills max:MAX_PILLS];
    for (int idx = 0; idx < num; idx++)
        if (NSPointInRect(pnt, pills[idx].rect)) return pills[idx].target;
    const a800_control *ctl = [self ctlAt:[self hit:pnt]];
    if (!ctl) return -1;
    switch (ctl->kind) {
    case A800_CTL_ROUND_BUTTON:
    case A800_CTL_CONSOLE_KEY:
        return ctl->target;
    default:
        return -1;
    }
}

/* ---- polling ------------------------------------------------------------------- */

- (void)poll
{
    /* The layout is re-read by the window (syncLayouts), which also resizes the
     * panel to it: not here, or the window would never see it had changed. */
    BOOL dirty = NO;
    for (int port = 0; port < A800_PORTS; port++) {
        const unsigned held = (self.mapping || port >= _nports) ? 0u : a800session_buttons_held(_session, port);
        if (held != _held[port]) { _held[port] = held; dirty = YES; }
        for (int pad = 0; pad < 2; pad++) {
            const int val = a800session_paddle_value(_session, port, pad);
            if (val != _knobVal[port][pad]) { _knobVal[port][pad] = val; dirty = YES; }
        }
    }
    const unsigned con = self.mapping ? 0u : a800session_console_held(_session);
    if (con != _conHeld) { _conHeld = con; dirty = YES; }
    if (dirty) [self setNeedsDisplay:YES];
}

/* ---- drawing -------------------------------------------------------------------- */

- (NSString *)legendOfTarget:(int)target
{
    if (!(self.mapping || self.showKeys) || target < 0) return nil;
    return [self.owner legendForTarget:target];
}

- (BOOL)picked:(int)target
{
    return self.mapping && target >= 0 && target == self.mapTarget;
}

/* A held part: the orange outline. */
- (void)strokeHeld:(NSBezierPath *)path
{
    [rgb(COL_HELD) setStroke];
    path.lineWidth = 2.5;
    [path stroke];
}

/* The picked part in Map mode: the orange ring. */
- (void)ringPicked:(NSBezierPath *)path
{
    [rgb(COL_HELD) setStroke];
    path.lineWidth = 2.5;
    [path stroke];
}

/* A soft shadow on the stage under a shape. */
- (void)shadowOf:(NSBezierPath *)path
{
    NSBezierPath *shadow = [path copy];
    NSAffineTransform *move = [NSAffineTransform transform];
    [move translateXBy:2 yBy:4];
    [shadow transformUsingAffineTransform:move];
    [rgba(0x000000, 0.35) setFill];
    [shadow fill];
}

/* -- the CX40 joystick (DESIGN-atari.md section 2), seen from above in an 8 x 8 case -- */

- (void)drawJoystickCase:(NSRect)rect port:(int)port
{
    const NSPoint org = rect.origin;
    float defx = 0, defy = 0;
    deflection(port >= 0 && port < A800_PORTS ? _held[port] : 0u, &defx, &defy);

    NSBezierPath *casePath = [NSBezierPath bezierPathWithRoundedRect:rect xRadius:0.9 * UNIT yRadius:0.9 * UNIT];
    [self shadowOf:casePath];
    fillGradient(casePath, COL_BLACK_TOP, COL_BLACK);
    NSBezierPath *bevel = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(rect, 0.2 * UNIT, 0.2 * UNIT)
                                                          xRadius:0.75 * UNIT yRadius:0.75 * UNIT];
    bevel.lineWidth = 1.5;
    [rgb(COL_BEVEL) setStroke];
    [bevel stroke];

    /* the fire button's recess: a teardrop, its neck leading to the ring */
    strokeLine(atp(org, 1.7, 1.7), atp(org, 3.1, 3.1), 0.9 * UNIT, rgb(COL_DEEP));
    fillOval(discRect(org, 1.7, 1.7, 1.1), rgb(COL_DEEP));

    /* the ring: 36 tick dashes, the ones on the deflected side lit */
    fillOval(discRect(org, 4.5, 4.5, 3.0), rgb(COL_DEEP));
    const BOOL tilted = fabsf(defx) > 0.05f || fabsf(defy) > 0.05f;
    const double dirAngle = tilted ? atan2((double)defx, -(double)defy) * 180.0 / M_PI : 0;
    for (int tick = 0; tick < 36; tick++) {
        const double ang = tick * 10.0;
        double diff = fabs(fmod(ang - dirAngle + 540.0, 360.0) - 180.0);
        const BOOL lit = tilted && diff <= 25.0;
        const double rad = ang * M_PI / 180.0;
        strokeLine(atp(org, 4.5 + 2.55 * sin(rad), 4.5 - 2.55 * cos(rad)),
                   atp(org, 4.5 + 2.85 * sin(rad), 4.5 - 2.85 * cos(rad)), lit ? 2.5 : 1.4,
                   lit ? rgb(COL_HELD) : rgb(0x3A3A3A));
    }
    drawFitted(@"TOP", atr(org, 3.75, 1.6, 1.5, 0.6), 7, YES, rgb(COL_BEVEL));

    /* the boot: ribbed rings, squashed a little toward the tilt for depth */
    const CGFloat sx = defx * 0.14, sy = defy * 0.14;
    fillOval(discRect(org, 4.5 + sx, 4.5 + sy, 2.1), rgb(0x151515));
    fillOval(discRect(org, 4.5 + sx, 4.5 + sy, 1.7), rgb(0x262626));
    fillOval(discRect(org, 4.5 + sx * 1.3, 4.5 + sy * 1.3, 1.3), rgb(0x151515));
}

/* The shaft's cap: an octagon that slides toward the held direction. */
- (void)drawStickCap:(const a800_control *)ctl
{
    const NSRect rect = [self rectOf:ctl];
    float defx = 0, defy = 0;
    deflection(ctl->port >= 0 && ctl->port < A800_PORTS ? _held[ctl->port] : 0u, &defx, &defy);
    const NSPoint ctr = NSMakePoint(NSMidX(rect) + defx * 1.0 * UNIT, NSMidY(rect) + defy * 1.0 * UNIT);
    NSBezierPath *cap = polygon(ctr, 0.85 * UNIT, 8, 22.5);
    [rgb(0x0C0C0C) setFill];
    [cap fill];
    [rgb(COL_BEVEL) setStroke];
    cap.lineWidth = 1.5;
    [cap stroke];
    fillOval(NSMakeRect(ctr.x - 0.2 * UNIT, ctr.y - 0.3 * UNIT, 0.4 * UNIT, 0.28 * UNIT), rgba(0xFFFFFF, 0.1));
    if (fabsf(defx) > 0.05f || fabsf(defy) > 0.05f)
        [self strokeHeld:polygon(ctr, 0.98 * UNIT, 8, 22.5)];
}

/* The joystick's one red fire button. */
- (void)drawFireButton:(const a800_control *)ctl
{
    const NSRect rect = [self rectOf:ctl];
    const NSPoint ctr = NSMakePoint(NSMidX(rect), NSMidY(rect));
    const BOOL held = [self heldTarget:ctl->target];
    NSString *legend = [self legendOfTarget:ctl->target];
    const CGFloat rad = 0.8 * UNIT;
    NSRect face = NSMakeRect(ctr.x - rad, ctr.y - rad + (held ? 1.5 : 0), 2 * rad, 2 * rad);
    NSBezierPath *disc = [NSBezierPath bezierPathWithOvalInRect:face];
    fillGradient(disc, held ? COL_RED_DOWN : COL_RED_HI, held ? mixRgb(COL_RED_DOWN, 0x000000, 0.15) : COL_RED);
    [rgba(0x000000, 0.3) setStroke];
    disc.lineWidth = 0.75;
    [disc stroke];
    if (!held)
        fillOval(NSMakeRect(ctr.x - 0.55 * UNIT, ctr.y - 0.6 * UNIT, 0.5 * UNIT, 0.32 * UNIT), rgba(0xFFFFFF, 0.3));
    if (held) [self strokeHeld:[NSBezierPath bezierPathWithOvalInRect:NSInsetRect(face, -3, -3)]];
    if (legend.length) drawFitted(legend, NSInsetRect(face, 3, 3), 10, YES, NSColor.whiteColor);
    if ([self picked:ctl->target])
        [self ringPicked:[NSBezierPath bezierPathWithOvalInRect:NSInsetRect(NSMakeRect(ctr.x - 0.8 * UNIT, ctr.y - 0.8 * UNIT,
                                                                                       1.6 * UNIT, 1.6 * UNIT), -5, -5)]];
}

/* -- the CX30 paddle (section 3) ------------------------------------------------- */

- (void)drawPaddleCase:(const a800_control *)ctl rect:(NSRect)rect
{
    (void)ctl;
    const NSPoint org = rect.origin;
    /* the rounded trapezoid, narrower at the cable end; its corners are rounded by
     * a stroke of the same colour round a polygon pulled in toward its centre */
    static const CGFloat pts[6][2] = { {1.0, 0.3}, {5.6, 0.3}, {7.0, 5.0}, {6.2, 5.9}, {0.8, 5.9}, {0.0, 4.6} };
    const CGFloat cx = 3.4, cy = 3.1, shrink = 0.93;
    NSBezierPath *body = [NSBezierPath bezierPath];
    for (int idx = 0; idx < 6; idx++) {
        const NSPoint pnt = atp(org, cx + (pts[idx][0] - cx) * shrink, cy + (pts[idx][1] - cy) * shrink);
        if (idx == 0) [body moveToPoint:pnt]; else [body lineToPoint:pnt];
    }
    [body closePath];
    body.lineJoinStyle = NSLineJoinStyleRound;
    body.lineWidth = 0.8 * UNIT;
    [self shadowOf:body];
    [rgb(0x1F1F1F) setFill];
    [body fill];
    [rgb(0x1F1F1F) setStroke];
    [body stroke];
    NSBezierPath *edge = [body copy];
    edge.lineWidth = 1.0;
    [rgb(0x2E2E2E) setStroke];
    [edge stroke];

    /* the label plate: "paddle" with crossed tennis rackets */
    const NSRect plate = atr(org, 3.6, 4.3, 2.8, 1.3);
    NSBezierPath *platePath = [NSBezierPath bezierPathWithRoundedRect:plate xRadius:0.15 * UNIT yRadius:0.15 * UNIT];
    [rgb(COL_DEEP) setFill];
    [platePath fill];
    [rgb(0x8A8A8A) setStroke];
    platePath.lineWidth = 1;
    [platePath stroke];
    drawFitted(@"paddle", NSMakeRect(NSMinX(plate) + 0.12 * UNIT, NSMinY(plate) + 0.15 * UNIT, 1.5 * UNIT, 0.6 * UNIT),
               9, YES, rgb(0xE6E6E6));
    /* two crossed rackets and a ball */
    const NSPoint rk = atp(org, 5.45, 4.95);
    strokeLine(NSMakePoint(rk.x - 0.35 * UNIT, rk.y + 0.4 * UNIT), NSMakePoint(rk.x + 0.15 * UNIT, rk.y - 0.15 * UNIT), 1.2,
               rgb(0xDCCB78));
    strokeLine(NSMakePoint(rk.x + 0.35 * UNIT, rk.y + 0.4 * UNIT), NSMakePoint(rk.x - 0.15 * UNIT, rk.y - 0.15 * UNIT), 1.2,
               rgb(0xDCCB78));
    [rgb(0xDCCB78) setStroke];
    NSBezierPath *head = [NSBezierPath bezierPathWithOvalInRect:NSMakeRect(rk.x + 0.02 * UNIT, rk.y - 0.42 * UNIT,
                                                                           0.34 * UNIT, 0.34 * UNIT)];
    head.lineWidth = 1.1;
    [head stroke];
    fillOval(NSMakeRect(rk.x - 0.5 * UNIT, rk.y - 0.35 * UNIT, 0.14 * UNIT, 0.14 * UNIT), rgb(0xDCCB78));
}

/* The ribbed knob, turning +-135 degrees from straight up; the mark shows where
 * it is (a800session_knob_angle of the paddle's value). */
- (void)drawKnob:(const a800_control *)ctl
{
    const NSRect rect = [self rectOf:ctl];
    const NSPoint ctr = NSMakePoint(NSMidX(rect), NSMidY(rect));
    const CGFloat rad = NSWidth(rect) / 2;
    const int port = ctl->port, pad = ctl->arg;

    fillOval(NSMakeRect(ctr.x - rad, ctr.y - rad, 2 * rad, 2 * rad), rgb(0x0E0E0E));
    for (int rib = 0; rib < 60; rib++) {
        const double ang = rib * M_PI / 30.0;
        strokeLine(NSMakePoint(ctr.x + rad * 0.9 * sin(ang), ctr.y - rad * 0.9 * cos(ang)),
                   NSMakePoint(ctr.x + rad * 0.99 * sin(ang), ctr.y - rad * 0.99 * cos(ang)), 1.2, rgb(0x2A2A2A));
    }
    fillOval(NSMakeRect(ctr.x - rad * 0.8, ctr.y - rad * 0.8, rad * 1.6, rad * 1.6), rgb(0x1A1A1A));
    NSBezierPath *cap = [NSBezierPath bezierPathWithOvalInRect:NSMakeRect(ctr.x - rad * 0.75, ctr.y - rad * 0.75,
                                                                          rad * 1.5, rad * 1.5)];
    fillGradient(cap, 0x2B2B2B, 0x1C1C1C);
    /* the position mark (added art: the real knob has none) */
    const int val = (port >= 0 && port < A800_PORTS && pad >= 0 && pad < 2) ? _knobVal[port][pad] : A800_PADDLE_CENTRE;
    const double ang = (double)a800session_knob_angle(val) * M_PI / 180.0;
    strokeLine(NSMakePoint(ctr.x + rad * 0.4 * sin(ang), ctr.y - rad * 0.4 * cos(ang)),
               NSMakePoint(ctr.x + rad * 0.7 * sin(ang), ctr.y - rad * 0.7 * cos(ang)), 0.12 * UNIT, rgb(0xD8D8D8));
    if (_knobDrag && [self isDown:ctl])
        [self strokeHeld:[NSBezierPath bezierPathWithOvalInRect:NSInsetRect(NSMakeRect(ctr.x - rad, ctr.y - rad, 2 * rad, 2 * rad), -2, -2)]];
}

/* The paddle's red fire button, on the case's left edge. */
- (void)drawPaddleButton:(const a800_control *)ctl
{
    NSRect rect = [self rectOf:ctl];
    const BOOL held = _firePressed && [self isDown:ctl];
    if (held) rect.origin.x += 1;
    NSBezierPath *path = [NSBezierPath bezierPathWithRoundedRect:rect xRadius:0.2 * UNIT yRadius:0.2 * UNIT];
    fillGradient(path, held ? 0xB22E18 : 0xE8492C, held ? 0x9A2412 : 0xD93A22);
    [rgba(0x000000, 0.35) setStroke];
    path.lineWidth = 0.75;
    [path stroke];
    if (held) [self strokeHeld:[NSBezierPath bezierPathWithRoundedRect:NSInsetRect(rect, -3, -3) xRadius:6 yRadius:6]];
}

/* A small turn arrow under a knob: a triangle that lights while it is held. */
- (void)drawTurn:(const a800_control *)ctl
{
    const NSRect rect = [self rectOf:ctl];
    const BOOL held = _turnPressed && [self isDown:ctl];
    NSBezierPath *tri = [NSBezierPath bezierPath];
    if (ctl->dir < 0) {
        [tri moveToPoint:NSMakePoint(NSMaxX(rect), NSMinY(rect))];
        [tri lineToPoint:NSMakePoint(NSMinX(rect), NSMidY(rect))];
        [tri lineToPoint:NSMakePoint(NSMaxX(rect), NSMaxY(rect))];
    } else {
        [tri moveToPoint:NSMakePoint(NSMinX(rect), NSMinY(rect))];
        [tri lineToPoint:NSMakePoint(NSMaxX(rect), NSMidY(rect))];
        [tri lineToPoint:NSMakePoint(NSMinX(rect), NSMaxY(rect))];
    }
    [tri closePath];
    [(held ? rgb(COL_HELD) : rgb(0x6A6A6A)) setFill];
    [tri fill];
}

/* -- the pointer devices (section 8) --------------------------------------------- */

/* A mouse: the Atari ST's (grey-beige, the logo low on the back) or the
 * Amiga's (cream, the nose a wedge). The two buttons are parts of their own. */
- (void)drawMouseBody:(NSRect)rect amiga:(BOOL)amiga
{
    const NSPoint org = rect.origin;
    NSBezierPath *path = [NSBezierPath bezierPathWithRoundedRect:rect xRadius:1.2 * UNIT yRadius:1.2 * UNIT];
    [self shadowOf:path];
    fillGradient(path, amiga ? 0xDDD6C4 : 0xD2CCBD, amiga ? 0xC2BBA8 : 0xB9B3A3);
    [rgba(0x000000, 0.3) setStroke];
    path.lineWidth = 0.75;
    [path stroke];
    const CGFloat btnBottom = (amiga ? 6.4 : 6.0) * 0.4;
    strokeLine(atp(org, 2.0, 0.25), atp(org, 2.0, btnBottom), 1.0, rgba(0x000000, 0.25));
    strokeLine(atp(org, 0.3, btnBottom), atp(org, 3.7, btnBottom), 1.0, rgba(0x000000, 0.2));
    if (!amiga) {
        /* the embossed fuji: three short bars, slanted */
        for (int bar = 0; bar < 3; bar++)
            strokeLine(atp(org, 1.6 + bar * 0.4, 5.3), atp(org, 1.9 + bar * 0.4, 4.4), 2.0, rgb(0xB0AA9B));
    }
}

/* The mouse's two buttons: lit and sunken while the mouse holds one. */
- (void)drawPointerButton:(const a800_control *)ctl
{
    const NSRect rect = [self rectOf:ctl];
    const a800_control *body = [self bodyOf:ctl];
    const char *dev = body && body->label ? body->label : "";
    const BOOL held = (_ptrMask & (unsigned)ctl->arg) != 0 && _downIdx >= 0 && [self isDown:ctl];
    NSBezierPath *path = [NSBezierPath bezierPathWithRoundedRect:rect xRadius:0.25 * UNIT yRadius:0.25 * UNIT];
    uint32_t top, bot;
    if (!strcmp(dev, "Koala Pad")) { top = 0x2A2A2A; bot = 0x151515; }
    else if (!strcmp(dev, "Trak-Ball")) { top = 0x4A4A4A; bot = 0x333333; }
    else if (!strcmp(dev, "Amiga mouse")) { top = 0xCFC8B6; bot = 0xB9B2A0; }
    else { top = 0xCDC7B8; bot = 0xB5AF9F; }
    fillGradient(path, held ? mixRgb(top, 0x000000, 0.25) : top, held ? mixRgb(bot, 0x000000, 0.25) : bot);
    [rgba(0x000000, 0.4) setStroke];
    path.lineWidth = 0.75;
    [path stroke];
    if (held) [self strokeHeld:[NSBezierPath bezierPathWithRoundedRect:NSInsetRect(rect, -3, -3) xRadius:6 yRadius:6]];
}

/* The mat a mouse is dragged on. */
- (void)drawMat:(const a800_control *)ctl
{
    const NSRect rect = [self rectOf:ctl];
    NSBezierPath *path = [NSBezierPath bezierPathWithRoundedRect:rect xRadius:0.5 * UNIT yRadius:0.5 * UNIT];
    fillGradient(path, 0x33363A, 0x26282B);
    [rgb(0x4A4D52) setStroke];
    path.lineWidth = 1;
    [path stroke];
    drawFitted(nsstr(ctl->legend), rect, 11, NO, rgba(0xFFFFFF, 0.55));
    if (_moveDrag && [self isDown:ctl]) [self strokeHeld:[NSBezierPath bezierPathWithRoundedRect:NSInsetRect(rect, 1, 1)
                                                                                         xRadius:0.5 * UNIT yRadius:0.5 * UNIT]];
}

/* The Trak-Ball's body: an angular black case, a silver strip with its name. */
- (void)drawTrakBall:(NSRect)rect
{
    const NSPoint org = rect.origin;
    NSBezierPath *path = [NSBezierPath bezierPath];
    [path moveToPoint:atp(org, 0.6, 0.0)];
    [path lineToPoint:atp(org, 8.4, 0.0)];
    [path lineToPoint:atp(org, 9.0, 4.4)];
    [path lineToPoint:atp(org, 8.2, 5.0)];
    [path lineToPoint:atp(org, 0.8, 5.0)];
    [path lineToPoint:atp(org, 0.0, 4.4)];
    [path closePath];
    [self shadowOf:path];
    fillGradient(path, 0x2C2C2C, 0x1B1B1B);
    [rgb(0x3A3A3A) setStroke];
    path.lineWidth = 1;
    [path stroke];
    NSBezierPath *strip = [NSBezierPath bezierPathWithRoundedRect:atr(org, 0.8, 4.05, 7.4, 0.7) xRadius:3 yRadius:3];
    fillGradient(strip, 0xE0E0E0, 0xB8B8B8);
    drawFitted(@"ATARI", atr(org, 1.0, 4.07, 2.0, 0.66), 9, YES, rgb(0x222222));
    drawFitted(@"TRAK-BALL", atr(org, 5.3, 4.07, 2.7, 0.66), 9, YES, rgb(0x222222));
}

/* The ball in its ring (the BALL part: 3 x 3 units). */
- (void)drawBall:(const a800_control *)ctl
{
    const NSRect rect = [self rectOf:ctl];
    const NSPoint ctr = NSMakePoint(NSMidX(rect), NSMidY(rect));
    fillOval(NSMakeRect(ctr.x - 1.9 * UNIT, ctr.y - 1.9 * UNIT, 3.8 * UNIT, 3.8 * UNIT), rgb(0x333333));
    fillOval(NSMakeRect(ctr.x - 1.65 * UNIT, ctr.y - 1.65 * UNIT, 3.3 * UNIT, 3.3 * UNIT), rgb(0x0E0E0E));
    NSBezierPath *ball = [NSBezierPath bezierPathWithOvalInRect:rect];
    fillLinear(ball, NSMakePoint(NSMinX(rect), NSMinY(rect)), NSMakePoint(NSMaxX(rect), NSMaxY(rect)),
               0xC8203A, 1.0, 0x5A0C18, 1.0);
    fillOval(NSMakeRect(ctr.x - 0.75 * UNIT, ctr.y - 1.0 * UNIT, 0.9 * UNIT, 0.55 * UNIT), rgba(0xFFFFFF, 0.28));
    if (_moveDrag && [self isDown:ctl])
        [self strokeHeld:[NSBezierPath bezierPathWithOvalInRect:NSInsetRect(rect, -3, -3)]];
}

/* The Koala Pad: a cream case with two black buttons and a dark pad. */
- (void)drawKoala:(NSRect)rect
{
    const NSPoint org = rect.origin;
    NSBezierPath *path = [NSBezierPath bezierPathWithRoundedRect:rect xRadius:0.6 * UNIT yRadius:0.6 * UNIT];
    [self shadowOf:path];
    fillGradient(path, 0xEDE6D2, 0xDAD3BE);
    [rgba(0x000000, 0.3) setStroke];
    path.lineWidth = 0.75;
    [path stroke];
    drawFitted(@"KOALA PAD", atr(org, 0.6, 7.45, 5.2, 0.6), 9, YES, rgb(0x6A6352));
}

/* The touch tablet: a black case, a silver label strip and a magenta-edged pad. */
- (void)drawTablet:(NSRect)rect
{
    const NSPoint org = rect.origin;
    NSBezierPath *path = [NSBezierPath bezierPathWithRoundedRect:rect xRadius:0.4 * UNIT yRadius:0.4 * UNIT];
    [self shadowOf:path];
    fillGradient(path, 0x262626, 0x161616);
    [rgb(0x3A3A3A) setStroke];
    path.lineWidth = 1;
    [path stroke];
    NSBezierPath *strip = [NSBezierPath bezierPathWithRoundedRect:atr(org, 0.4, 0.25, 7.8, 0.7) xRadius:3 yRadius:3];
    fillGradient(strip, 0xE0E0DC, 0xBDBDB8);
    drawFitted(@"ATARI", atr(org, 0.6, 0.27, 2.2, 0.66), 9, YES, rgb(0x222222));
    drawFitted(@"TOUCH TABLET", atr(org, 4.4, 0.27, 3.6, 0.66), 9, YES, rgb(0x222222));
    /* the stylus, lying along the bottom edge */
    strokeLine(atp(org, 1.2, 4.95), atp(org, 6.0, 4.95), 3.0, rgb(0x8E8E8E));
    strokeLine(atp(org, 6.0, 4.95), atp(org, 6.3, 4.95), 3.0, rgb(0x2E2E2E));
}

/* The pad surface of the Koala Pad or the tablet, and where it is touched. */
- (void)drawPad:(const a800_control *)ctl
{
    const NSRect rect = [self rectOf:ctl];
    const BOOL tablet = ctl->arg == 1;
    NSBezierPath *path = [NSBezierPath bezierPathWithRoundedRect:rect xRadius:0.25 * UNIT yRadius:0.25 * UNIT];
    [rgb(tablet ? 0x2B2E30 : 0x3B3B3D) setFill];
    [path fill];
    [(tablet ? rgb(0xC0408A) : rgb(0x1E1E20)) setStroke];
    path.lineWidth = tablet ? 2.0 : 1.5;
    [path stroke];
    drawFitted(nsstr(ctl->legend), rect, 11, NO, rgba(0xFFFFFF, 0.4));
    if (_padDrag && [self isDown:ctl]) {
        const NSPoint hot = NSMakePoint(NSMinX(rect) + _touch.x * NSWidth(rect), NSMinY(rect) + _touch.y * NSHeight(rect));
        fillOval(NSMakeRect(hot.x - 5, hot.y - 5, 10, 10), rgba(COL_HELD, 0.9));
        strokeLine(NSMakePoint(hot.x - 9, hot.y), NSMakePoint(hot.x + 9, hot.y), 1, rgba(COL_HELD, 0.8));
        strokeLine(NSMakePoint(hot.x, hot.y - 9), NSMakePoint(hot.x, hot.y + 9), 1, rgba(COL_HELD, 0.8));
    }
}

/* The CX75 light pen: a thin black pen with a clip. */
- (void)drawLightPen:(NSRect)rect
{
    const NSPoint org = rect.origin;
    NSBezierPath *path = [NSBezierPath bezierPathWithRoundedRect:rect xRadius:0.45 * UNIT yRadius:0.45 * UNIT];
    [self shadowOf:path];
    fillGradient(path, 0x333333, 0x151515);
    strokeLine(atp(org, 0.78, 1.0), atp(org, 0.78, 4.0), 2.0, rgb(0x9A9A9A));
    NSBezierPath *tip = [NSBezierPath bezierPathWithRoundedRect:atr(org, 0.3, 7.2, 0.4, 0.7) xRadius:2 yRadius:2];
    [rgb(0x777777) setFill];
    [tip fill];
}

/* The XG-1 light gun: a grey pistol, the barrel and sight top left, a ribbed grip. */
- (void)drawLightGun:(NSRect)rect
{
    const NSPoint org = rect.origin;
    NSBezierPath *barrel = [NSBezierPath bezierPathWithRoundedRect:atr(org, 0.0, 0.7, 7.6, 2.2) xRadius:0.5 * UNIT yRadius:0.5 * UNIT];
    NSBezierPath *grip = [NSBezierPath bezierPath];
    [grip moveToPoint:atp(org, 4.6, 2.5)];
    [grip lineToPoint:atp(org, 7.4, 2.5)];
    [grip lineToPoint:atp(org, 6.8, 7.0)];
    [grip lineToPoint:atp(org, 4.3, 7.0)];
    [grip closePath];
    [self shadowOf:barrel];
    fillGradient(grip, 0xA8A398, 0x8E8A80);
    for (int rib = 0; rib < 5; rib++)
        strokeLine(atp(org, 4.6 + rib * 0.1, 3.6 + rib * 0.65), atp(org, 6.9 + rib * 0.1, 3.6 + rib * 0.65), 1.2, rgba(0x000000, 0.18));
    fillGradient(barrel, 0xC8C3B6, 0xAAA598);
    [rgba(0x000000, 0.3) setStroke];
    barrel.lineWidth = 0.75;
    [barrel stroke];
    fillOval(atr(org, 0.15, 0.25, 0.5, 0.45), rgb(0x8E8A80));
    NSBezierPath *trigger = [NSBezierPath bezierPathWithRoundedRect:atr(org, 3.9, 2.8, 0.55, 1.4) xRadius:3 yRadius:3];
    [rgb(0x6E6C66) setFill];
    [trigger fill];
}

/* The CX85 numeric keypad: a cream case round a dark face, its keys parts of their own. */
- (void)drawCX85:(NSRect)rect
{
    const NSPoint org = rect.origin;
    NSBezierPath *path = [NSBezierPath bezierPathWithRoundedRect:rect xRadius:0.35 * UNIT yRadius:0.35 * UNIT];
    [self shadowOf:path];
    fillGradient(path, 0xE0D8C1, 0xCBC3AC);
    NSBezierPath *face = [NSBezierPath bezierPathWithRoundedRect:atr(org, 0.1, 0.1, 6.2, 4.8) xRadius:0.25 * UNIT yRadius:0.25 * UNIT];
    [rgb(0x55564F) setFill];
    [face fill];
}

- (void)drawCX85Key:(const a800_control *)ctl
{
    const NSRect rect = [self rectOf:ctl];
    const BOOL held = _cxPressed && [self isDown:ctl];
    NSBezierPath *path = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(rect, 1, 1) xRadius:3 yRadius:3];
    fillGradient(path, held ? 0x59513F : 0x4A4436, held ? 0x2E2A21 : 0x3A352B);
    [rgba(0x000000, 0.4) setStroke];
    path.lineWidth = 0.75;
    [path stroke];
    NSString *label = nsstr(ctl->label);
    if ([label isEqualToString:@"ESCAPE"]) label = @"ESC";          /* the narrow left column */
    else if ([label isEqualToString:@"DELETE"]) label = @"DEL";
    drawFitted(label.length > 4 ? [label stringByReplacingOccurrencesOfString:@" " withString:@"\n"] : label,
               NSInsetRect(rect, 2, 2), label.length > 3 ? 8 : 11, YES, rgb(0xE8E4D6));
    if (held) [self strokeHeld:[NSBezierPath bezierPathWithRoundedRect:NSInsetRect(rect, -1, -1) xRadius:5 yRadius:5]];
}

/* -- the console strip ----------------------------------------------------------- */

- (void)drawConsolePanel:(NSRect)rect
{
    NSBezierPath *panel = [NSBezierPath bezierPathWithRoundedRect:rect xRadius:0.4 * UNIT yRadius:0.4 * UNIT];
    [self shadowOf:panel];
    fillGradient(panel, 0x34302C, COL_CONSOLE);
    [rgb(0x4A443E) setStroke];
    panel.lineWidth = 1;
    [panel stroke];
}

/* One of the console keys (START, SELECT, OPTION, HELP, RESET, BREAK): the
 * 800's orange-to-yellow keys; HELP is the XL's round grey one. */
- (void)drawConsoleKey:(const a800_control *)ctl
{
    NSRect rect = [self rectOf:ctl];
    const int con = ctl->target - A800_TARGET_CONSOLE(0);
    const BOOL held = [self heldTarget:ctl->target];
    uint32_t top = 0xF6BE13, bot = 0xD9A00C;
    switch (con) {
    case A800_CON_RESET: top = 0xE8A62A; bot = 0xC88218; break;
    case A800_CON_OPTION: top = 0xF0AC2A; bot = 0xD08A18; break;
    case A800_CON_SELECT: top = 0xF4B525; bot = 0xD59A14; break;
    case A800_CON_START: top = 0xF8C61A; bot = 0xDDA80E; break;
    case A800_CON_HELP: top = 0xB6B4AB; bot = 0x95938A; break;
    case A800_CON_BREAK: top = 0xCD4A33; bot = 0xA3331F; break;
    default: break;
    }
    if (held) rect.origin.y += 1.5;
    const CGFloat radius = con == A800_CON_HELP ? NSHeight(rect) / 2 : 4;
    NSBezierPath *path = [NSBezierPath bezierPathWithRoundedRect:rect xRadius:radius yRadius:radius];
    fillGradient(path, held ? mixRgb(top, 0x000000, 0.15) : top, held ? mixRgb(bot, 0x000000, 0.15) : bot);
    [rgba(0x000000, 0.4) setStroke];
    path.lineWidth = 0.75;
    [path stroke];
    drawFitted(nsstr(ctl->label), NSInsetRect(rect, 3, 2), 11, YES, rgb(con == A800_CON_BREAK ? 0xF6E9E4 : 0x3A2A10));
    if (held) [self strokeHeld:[NSBezierPath bezierPathWithRoundedRect:NSInsetRect(rect, -3, -3) xRadius:radius + 3 yRadius:radius + 3]];
    if ([self picked:ctl->target])
        [self ringPicked:[NSBezierPath bezierPathWithRoundedRect:NSInsetRect(rect, -5, -5) xRadius:radius + 5 yRadius:radius + 5]];
}

- (void)drawBody:(const a800_control *)ctl
{
    const NSRect rect = [self rectOf:ctl];
    const char *name = ctl->label ? ctl->label : "";
    if (!strcmp(name, "Joystick")) [self drawJoystickCase:rect port:ctl->port];
    else if (!strcmp(name, "Paddle A") || !strcmp(name, "Paddle B")) [self drawPaddleCase:ctl rect:rect];
    else if (!strcmp(name, "Atari ST mouse")) [self drawMouseBody:rect amiga:NO];
    else if (!strcmp(name, "Amiga mouse")) [self drawMouseBody:rect amiga:YES];
    else if (!strcmp(name, "Trak-Ball")) [self drawTrakBall:rect];
    else if (!strcmp(name, "Koala Pad")) [self drawKoala:rect];
    else if (!strcmp(name, "Touch tablet")) [self drawTablet:rect];
    else if (!strcmp(name, "Light pen")) [self drawLightPen:rect];
    else if (!strcmp(name, "Light gun")) [self drawLightGun:rect];
    else if (!strcmp(name, "CX85")) [self drawCX85:rect];
    else if (!strcmp(name, "Console")) [self drawConsolePanel:rect];
    else if (!strcmp(name, "Nothing")) drawFitted(@"Nothing plugged in", NSInsetRect(rect, -2 * UNIT, 0), 12, NO, rgba(0xFFFFFF, 0.5));
}

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    NSBezierPath *stage = [NSBezierPath bezierPathWithRoundedRect:NSMakeRect(0, 0, WIN_W, _size.height) xRadius:10 yRadius:10];
    fillGradient(stage, COL_STAGE_TOP, COL_STAGE_BOT);
    [rgb(COL_STAGE_EDGE) setStroke];
    stage.lineWidth = 1;
    [stage stroke];

    for (int idx = 0; idx < _count; idx++) {
        const a800_control *ctl = &_ctls[idx];
        switch (ctl->kind) {
        case A800_CTL_CAPTION: {
            NSString *text = ctl->legend && ctl->legend[0]
                ? [NSString stringWithFormat:@"%@  ·  %@", nsstr(ctl->label), nsstr(ctl->legend)]
                : nsstr(ctl->label);
            drawFitted(text, [self rectOf:ctl], 12, YES, rgb(COL_CAPTION));
            break;
        }
        case A800_CTL_BODY:
            [self drawBody:ctl];
            break;
        case A800_CTL_STICK:
            [self drawStickCap:ctl];
            break;
        case A800_CTL_ROUND_BUTTON:
            [self drawFireButton:ctl];
            break;
        case A800_CTL_KNOB:
            [self drawKnob:ctl];
            break;
        case A800_CTL_PADDLE_FIRE:
            [self drawPaddleButton:ctl];
            break;
        case A800_CTL_TURN:
            [self drawTurn:ctl];
            break;
        case A800_CTL_CONSOLE_KEY:
            [self drawConsoleKey:ctl];
            break;
        case A800_CTL_POINTER_BUTTON:
            [self drawPointerButton:ctl];
            break;
        case A800_CTL_MAT:
            [self drawMat:ctl];
            break;
        case A800_CTL_BALL:
            [self drawBall:ctl];
            break;
        case A800_CTL_PAD:
            [self drawPad:ctl];
            break;
        case A800_CTL_CX85_KEY:
            [self drawCX85Key:ctl];
            break;
        case A800_CTL_NOTE:
            drawFitted(nsstr(ctl->label), [self rectOf:ctl], 12, NO, rgba(0xFFFFFF, 0.8));
            break;
        default:
            break;
        }
    }

    /* the Map / Show bindings labels, over everything */
    Pill pills[MAX_PILLS];
    const int num = [self pills:pills max:MAX_PILLS];
    for (int idx = 0; idx < num; idx++)
        drawPill(pills[idx].rect, nsstr(pills[idx].text), [self picked:pills[idx].target]);
}

/* ---- the mouse ---------------------------------------------------------------------- */

- (void)letGo
{
    if (_downTarget >= 0) a800session_press(_session, _downTarget, 0);
    if (_downIdx >= 0 && _downIdx < _count) {
        const a800_control *ctl = &_ctls[_downIdx];
        if (_stickDrag) a800session_pointer_set(_session, ctl->port, 0);
        if (_knobDrag) a800session_paddle_pointer(_session, ctl->port, ctl->arg, 0, 0);
        if (_firePressed) a800session_paddle_fire(_session, ctl->port, ctl->arg, 0);
        if (_turnPressed) a800session_paddle_turn(_session, ctl->port, ctl->arg, 0);
        if (_cxPressed) a800session_cx85_key(_session, ctl->arg, 0);
        if (_padDrag) a800session_pointer_norm(_session, (float)_touch.x, (float)_touch.y, 0, 0);
        if (ctl->kind == A800_CTL_POINTER_BUTTON) {
            _ptrMask &= ~(unsigned)ctl->arg;
            a800session_pointer_motion(_session, 0, 0, _ptrMask);
        }
    }
    _downIdx = -1;
    _downTarget = -1;
    _stickDrag = _knobDrag = _padDrag = _moveDrag = _firePressed = _turnPressed = _cxPressed = NO;
    [self setNeedsDisplay:YES];
}

/* The stick follows the pointer: the core turns the offset into one of eight
 * directions (nothing near the middle). */
- (void)dragStickAt:(NSPoint)pnt
{
    const a800_control *ctl = [self ctlAt:_downIdx];
    if (!ctl) return;
    const NSRect rect = [self rectOf:ctl];
    const unsigned mask = a800session_stick_from_vector((float)(pnt.x - NSMidX(rect)), (float)(pnt.y - NSMidY(rect)),
                                                        (float)(NSWidth(rect) / 2));
    a800session_pointer_set(_session, ctl->port, mask);
}

/* The knob goes where the pointer's angle puts it, and holds there. */
- (void)dragKnobAt:(NSPoint)pnt
{
    const a800_control *ctl = [self ctlAt:_downIdx];
    if (!ctl) return;
    const NSRect rect = [self rectOf:ctl];
    const int value = a800session_knob_from_vector((float)(pnt.x - NSMidX(rect)), (float)(pnt.y - NSMidY(rect)));
    a800session_paddle_pointer(_session, ctl->port, ctl->arg, value, 1);
}

/* A touch on a pad: the point in 0..1 of the surface. The tablet's touch is its
 * own button (the part's arg is 1); the Koala Pad's buttons are parts of their own. */
- (void)touchPadAt:(NSPoint)pnt down:(BOOL)down
{
    const a800_control *ctl = [self ctlAt:_downIdx];
    if (!ctl) return;
    const NSRect rect = [self rectOf:ctl];
    CGFloat uu = (pnt.x - NSMinX(rect)) / NSWidth(rect), vv = (pnt.y - NSMinY(rect)) / NSHeight(rect);
    uu = fmin(1.0, fmax(0.0, uu));
    vv = fmin(1.0, fmax(0.0, vv));
    _touch = NSMakePoint(uu, vv);
    const unsigned mask = ctl->arg == 1 ? 1u : _ptrMask;
    a800session_pointer_norm(_session, (float)uu, (float)vv, down ? 1 : 0, down ? mask : 0u);
}

/* A mouse or a Trak-Ball: the drag's whole-point steps are the device's motion. */
- (void)moveAt:(NSPoint)pnt
{
    const int dx = (int)(pnt.x - _lastPt.x), dy = (int)(pnt.y - _lastPt.y);
    if (!dx && !dy) return;
    _lastPt.x += dx;
    _lastPt.y += dy;
    a800session_pointer_motion(_session, dx, dy, _ptrMask);
}

- (void)mouseDown:(NSEvent *)evt
{
    if (_downIdx >= 0) return;
    const NSPoint pnt = [self convertPoint:[evt locationInWindow] fromView:nil];

    if (self.mapping) {
        /* Map: a label, the fire button or a console key is picked */
        const int target = [self mapTargetAt:pnt];
        if (target >= 0) [self.owner mapPick:target];
        return;
    }

    const int idx = [self hit:pnt];
    const a800_control *ctl = [self ctlAt:idx];
    if (!ctl) return;
    switch (ctl->kind) {
    case A800_CTL_STICK:
        _downIdx = idx;
        _stickDrag = YES;
        [self dragStickAt:pnt];
        break;
    case A800_CTL_KNOB:
        _downIdx = idx;
        _knobDrag = YES;
        [self dragKnobAt:pnt];
        break;
    case A800_CTL_PADDLE_FIRE:
        _downIdx = idx;
        _firePressed = YES;
        a800session_paddle_fire(_session, ctl->port, ctl->arg, 1);
        break;
    case A800_CTL_TURN:
        _downIdx = idx;
        _turnPressed = YES;
        a800session_paddle_turn(_session, ctl->port, ctl->arg, ctl->dir);
        break;
    case A800_CTL_ROUND_BUTTON:
    case A800_CTL_CONSOLE_KEY:
        if (ctl->target < 0) break;
        _downIdx = idx;
        _downTarget = ctl->target;
        a800session_press(_session, ctl->target, 1);
        break;
    case A800_CTL_POINTER_BUTTON:
        _downIdx = idx;
        _ptrMask |= (unsigned)ctl->arg;
        a800session_pointer_motion(_session, 0, 0, _ptrMask);
        break;
    case A800_CTL_MAT:
    case A800_CTL_BALL:
        _downIdx = idx;
        _moveDrag = YES;
        _lastPt = pnt;
        break;
    case A800_CTL_PAD:
        _downIdx = idx;
        _padDrag = YES;
        [self touchPadAt:pnt down:YES];
        break;
    case A800_CTL_CX85_KEY:
        _downIdx = idx;
        _cxPressed = YES;
        a800session_cx85_key(_session, ctl->arg, 1);
        break;
    default:
        break;
    }
    [self setNeedsDisplay:YES];
    [self poll];
}

- (void)dragTo:(NSPoint)pnt
{
    if (_downIdx < 0) return;
    if (_knobDrag) {
        [self dragKnobAt:pnt];
        [self poll];
        return;
    }
    if (_stickDrag) {
        [self dragStickAt:pnt];
        [self poll];
        return;
    }
    if (_padDrag) {
        [self touchPadAt:pnt down:YES];
        [self setNeedsDisplay:YES];
        return;
    }
    if (_moveDrag) {
        [self moveAt:pnt];
        return;
    }
    /* a button, a key, a turn arrow: dragging off lets go */
    const a800_control *ctl = [self ctlAt:_downIdx];
    if (ctl && !NSPointInRect(pnt, [self rectOf:ctl])) {
        [self letGo];
        [self poll];
    }
}

- (void)mouseDragged:(NSEvent *)evt
{
    [self dragTo:[self convertPoint:[evt locationInWindow] fromView:nil]];
}

- (void)mouseUp:(NSEvent *)evt
{
    (void)evt;
    if (_downIdx < 0) return;
    [self letGo];
    [self poll];
}

/* The wheel over a knob nudges it (up turns it right). */
- (void)scrollWheel:(NSEvent *)evt
{
    if (self.mapping) return;
    const a800_control *ctl = [self ctlAt:[self hit:[self convertPoint:[evt locationInWindow] fromView:nil]]];
    if (!ctl || ctl->kind != A800_CTL_KNOB) return;
    _scrollAcc += [evt scrollingDeltaY];
    const CGFloat per = [evt hasPreciseScrollingDeltas] ? 6.0 : 1.0;
    int steps = 0;
    while (_scrollAcc >= per) { steps++; _scrollAcc -= per; }
    while (_scrollAcc <= -per) { steps--; _scrollAcc += per; }
    if (!steps) return;
    /* up turns it right: a lower value */
    int value = a800session_paddle_value(_session, ctl->port, ctl->arg) - steps * NUDGE;
    if (value < 0) value = 0;
    if (value > A800_PADDLE_MAX) value = A800_PADDLE_MAX;
    a800session_paddle_pointer(_session, ctl->port, ctl->arg, value, 1);
    a800session_paddle_pointer(_session, ctl->port, ctl->arg, value, 0);
    [self poll];
}

/* The tooltip: the target's name and what is bound to it. */
- (NSString *)view:(NSView *)view stringForToolTip:(NSToolTipTag)tag point:(NSPoint)pnt userData:(void *)data
{
    (void)view;
    (void)tag;
    (void)data;
    const a800_control *ctl = [self ctlAt:[self hit:pnt]];
    if (!ctl || ctl->target < 0) return nil;
    const a800_binding bind = a800session_binding_get(_session, ctl->target);
    NSString *key = bind.keysym ? keyName(bind.keysym) : @"none";
    NSString *padText = bind.button != A800_PAD_BTN_NONE ? nsstr(a800_pad_button_name(bind.button)) : @"none";
    return [NSString stringWithFormat:@"%@\nKey: %@    Gamepad: %@", nsstr(a800_target_name(ctl->target)), key, padText];
}

/* One tooltip rect over the whole stage; the string is made per point. */
- (void)setFrameSize:(NSSize)size
{
    [super setFrameSize:size];
    [self removeAllToolTips];
    [self addToolTipRect:[self bounds] owner:self userData:NULL];
}
@end

/* ---- the keyboard ------------------------------------------------------------------- */

/* The Atari keyboard of the machine, drawn from the session's table (16 x 6 key
 * units): the 400's black membrane, the 800's brown keys on a beige case, the
 * XL/XE's charcoal keys on white (HELP a round grey key), the 1200XL's the same
 * with F1-F4 (DESIGN-atari.md section 7). A cap is held while the mouse is on
 * it, as on the real keyboard; SHIFT and CTRL latch for the next key (the core
 * does that), and the console keys and BREAK can be picked in Map. */
@interface A800KeyboardView : A800CtlFlippedView
@property (nonatomic) a800session *session;
@property (nonatomic, weak) A800ControllersWindow *owner;
@property (nonatomic) BOOL mapping;
@property (nonatomic) int mapTarget;
@property (nonatomic, readonly) NSSize contentSize;
- (void)reload;
- (BOOL)stale;
- (void)poll;
- (void)letGo;
@end

typedef struct {
    uint32_t plate, edge, cap, capTop, legend, shifted;
} KbdPalette;

static KbdPalette kbdPalette(int family)
{
    switch (family) {
    case A800_KBD_400:
        return (KbdPalette){ 0x151515, 0x2C2C2C, 0x232323, 0x303030, 0xF0F0F0, 0xE08A1F };
    case A800_KBD_XL:
    case A800_KBD_1200XL:
        return (KbdPalette){ 0xE6E3DA, 0xC4C0B4, 0x2E2E31, 0x3C3C40, 0xF2F2F2, 0xC9C9CC };
    default:
        return (KbdPalette){ 0xD4CDB8, 0xB4AC96, 0x3B352B, 0x4A4437, 0xE9E4D4, 0xB9B3A2 };
    }
}

@implementation A800KeyboardView {
    a800_kbd_key _keys[MAX_KEYS];
    int _count;
    int _family;
    BOOL _lit[MAX_KEYS];
    NSSize _size;
    int _downKey;                /* the on-screen keyboard index held by the mouse, or -1 */
    int _downSlot;               /* its slot in _keys */
}

- (instancetype)initWithFrame:(NSRect)frame
{
    self = [super initWithFrame:frame];
    if (!self) return nil;
    _downKey = -1;
    _downSlot = -1;
    _mapTarget = -1;
    return self;
}

- (BOOL)acceptsFirstMouse:(NSEvent *)evt { (void)evt; return YES; }
- (BOOL)mouseDownCanMoveWindow { return NO; }
- (NSSize)contentSize { return _size; }

/* (Re)reads the table: it follows the machine (HELP on the XL/XE, F1-F4 on the 1200XL). */
- (void)reload
{
    const a800_kbd_key *keys = NULL;
    int count = a800session_keyboard_layout(_session, &keys);
    float extentW = 0, extentH = 0;
    if (!keys || count < 0) count = 0;
    if (count > MAX_KEYS) count = MAX_KEYS;
    _count = count;
    for (int idx = 0; idx < count; idx++) {
        _keys[idx] = keys[idx];
        if (keys[idx].x + keys[idx].w > extentW) extentW = keys[idx].x + keys[idx].w;
        if (keys[idx].y + keys[idx].h > extentH) extentH = keys[idx].y + keys[idx].h;
    }
    _family = a800session_keyboard_family(_session);
    _size = NSMakeSize(px(extentW) + 2 * KBD_PLATE, px(extentH) + 2 * KBD_PLATE);
    [self setNeedsDisplay:YES];
}

- (BOOL)stale
{
    const a800_kbd_key *keys = NULL;
    const int count = a800session_keyboard_layout(_session, &keys);
    return count != _count || a800session_keyboard_family(_session) != _family;
}

- (NSRect)capRect:(const a800_kbd_key *)key
{
    const CGFloat x0 = px(key->x), y0 = px(key->y), x1 = px(key->x + key->w), y1 = px(key->y + key->h);
    return NSMakeRect(KBD_PLATE + x0 + KBD_GAP / 2, KBD_PLATE + y0 + KBD_GAP / 2, x1 - x0 - KBD_GAP, y1 - y0 - KBD_GAP);
}

- (int)slotAt:(NSPoint)pnt
{
    for (int idx = 0; idx < _count; idx++)
        if (NSPointInRect(pnt, [self capRect:&_keys[idx]])) return idx;
    return -1;
}

- (void)poll
{
    BOOL dirty = NO;
    for (int idx = 0; idx < _count; idx++) {
        BOOL on = NO;
        if (!self.mapping)
            on = _keys[idx].index == _downKey || a800session_keyboard_held(_session, _keys[idx].index) != 0;
        if (on != _lit[idx]) { _lit[idx] = on; dirty = YES; }
    }
    if (dirty) [self setNeedsDisplay:YES];
}

/* The target Map would pick for a key: its console key, or -1. */
- (int)targetOf:(const a800_kbd_key *)key
{
    return (key->kind == A800_KEY_CONSOLE || key->kind == A800_KEY_BREAK) && key->console >= 0
        ? A800_TARGET_CONSOLE(key->console) : -1;
}

- (void)drawKey:(const a800_kbd_key *)key slot:(int)slot
{
    const KbdPalette pal = kbdPalette(_family);
    const NSRect rect = [self capRect:key];
    const BOOL lit = _lit[slot];
    const int target = [self targetOf:key];
    const BOOL pickable = target >= 0;
    const BOOL dim = self.mapping && !pickable;
    const BOOL xl = _family == A800_KBD_XL || _family == A800_KBD_1200XL;
    const BOOL help = key->kind == A800_KEY_CONSOLE && key->console == A800_CON_HELP;
    uint32_t top = pal.capTop, bot = pal.cap, ink = pal.legend;
    CGFloat radius = 4;

    if (key->kind == A800_KEY_CONSOLE && !help) {
        if (xl) {
            ink = 0xE58A1C;                         /* charcoal keys, orange legends */
        } else {
            static const uint32_t tops[A800_CON_COUNT] = { 0xF8C61A, 0xF4B525, 0xF0AC2A, 0xB6B4AB, 0xE8A62A, 0xCD4A33 };
            static const uint32_t bots[A800_CON_COUNT] = { 0xDDA80E, 0xD59A14, 0xD08A18, 0x95938A, 0xC88218, 0xA3331F };
            const int con = key->console >= 0 && key->console < A800_CON_COUNT ? key->console : A800_CON_START;
            top = tops[con];
            bot = bots[con];
            ink = 0x3A2A10;
        }
    } else if (help) {
        top = 0xB6B4AB;                             /* the XL's round grey HELP key */
        bot = 0xA09E94;
        ink = 0x2A2A2A;
        radius = NSHeight(rect) / 2;
    } else if (key->kind == A800_KEY_BREAK) {
        top = xl ? 0x3C3C40 : pal.capTop;
        ink = 0xE6735E;
    }

    CGContextRef ctx = [[NSGraphicsContext currentContext] CGContext];
    if (dim) {
        [NSGraphicsContext saveGraphicsState];
        CGContextSetAlpha(ctx, 0.35);
    }
    NSBezierPath *path = [NSBezierPath bezierPathWithRoundedRect:rect xRadius:radius yRadius:radius];
    fillGradient(path, lit ? mixRgb(top, 0xFFB04A, 0.35) : top, lit ? mixRgb(bot, 0xFFB04A, 0.25) : bot);
    [rgba(0x000000, 0.45) setStroke];
    path.lineWidth = 0.75;
    [path stroke];

    NSString *face = nsstr(key->label);
    NSString *shifted = key->shifted ? nsstr(key->shifted) : nil;
    const NSRect inner = NSInsetRect(rect, 2, 2);
    if (self.mapping && pickable) {
        NSString *bound = [self.owner legendForTarget:target];
        drawFitted([NSString stringWithFormat:@"%@\n%@", face, bound.length ? bound : @"-"], inner, 10, YES, rgb(ink));
    } else if (shifted.length) {
        const CGFloat half = floor(inner.size.height / 2);
        drawFitted(shifted, NSMakeRect(inner.origin.x, inner.origin.y, inner.size.width, half), 8, NO, rgb(pal.shifted));
        drawFitted(face, NSMakeRect(inner.origin.x, inner.origin.y + half, inner.size.width, inner.size.height - half),
                   11, YES, rgb(ink));
    } else if (key->kind == A800_KEY_SHIFT || key->kind == A800_KEY_CTRL) {
        /* the boxed legends of SHIFT and CONTROL */
        const NSRect box = NSInsetRect(inner, 3, 5);
        NSBezierPath *boxPath = [NSBezierPath bezierPathWithRoundedRect:box xRadius:2 yRadius:2];
        [rgb(ink) setStroke];
        boxPath.lineWidth = 1;
        [boxPath stroke];
        drawFitted(face, NSInsetRect(box, 1, 0), 9, YES, rgb(ink));
    } else {
        drawFitted(face, inner, face.length > 3 ? 9 : 12, YES, rgb(ink));
    }
    if (lit) {
        NSBezierPath *ring = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(rect, -1, -1) xRadius:radius + 1 yRadius:radius + 1];
        [rgb(COL_HELD) setStroke];
        ring.lineWidth = 2.0;
        [ring stroke];
    }
    if (self.mapping && pickable && target == self.mapTarget) {
        NSBezierPath *ring = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(rect, -3, -3) xRadius:radius + 3 yRadius:radius + 3];
        [rgb(COL_HELD) setStroke];
        ring.lineWidth = 2.5;
        [ring stroke];
    }
    if (dim) [NSGraphicsContext restoreGraphicsState];
}

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    const KbdPalette pal = kbdPalette(_family);
    NSBezierPath *plate = [NSBezierPath bezierPathWithRoundedRect:NSMakeRect(0, 0, _size.width, _size.height) xRadius:9 yRadius:9];
    fillGradient(plate, mixRgb(pal.plate, 0xFFFFFF, _family == A800_KBD_400 ? 0.04 : 0.1), pal.plate);
    [rgb(pal.edge) setStroke];
    plate.lineWidth = 1.5;
    [plate stroke];
    for (int idx = 0; idx < _count; idx++) [self drawKey:&_keys[idx] slot:idx];
}

- (void)letGo
{
    if (_downKey >= 0) a800session_keyboard_press(_session, _downKey, 0);
    _downKey = -1;
    _downSlot = -1;
    [self setNeedsDisplay:YES];
}

- (void)mouseDown:(NSEvent *)evt
{
    if (_downKey >= 0) return;
    const int slot = [self slotAt:[self convertPoint:[evt locationInWindow] fromView:nil]];
    if (slot < 0) return;
    if (self.mapping) {
        const int target = [self targetOf:&_keys[slot]];
        if (target >= 0) [self.owner mapPick:target];
        return;
    }
    _downSlot = slot;
    _downKey = _keys[slot].index;
    a800session_keyboard_press(_session, _downKey, 1);
    [self poll];
}

/* Dragging off a cap lets it go. */
- (void)mouseDragged:(NSEvent *)evt
{
    if (_downKey < 0) return;
    const NSPoint pnt = [self convertPoint:[evt locationInWindow] fromView:nil];
    if (_downSlot >= 0 && NSPointInRect(pnt, [self capRect:&_keys[_downSlot]])) return;
    [self letGo];
    [self poll];
}

- (void)mouseUp:(NSEvent *)evt
{
    (void)evt;
    if (_downKey < 0) return;
    [self letGo];
    [self poll];
}
@end

@implementation A800ControllersPanel
- (void)keyDown:(NSEvent *)evt { [self.owner panelKeyDown:evt]; }
- (void)keyUp:(NSEvent *)evt { [self.owner panelKeyUp:evt]; }
- (void)flagsChanged:(NSEvent *)evt { [self.owner panelFlagsChanged:evt]; }
/* Escape is the Atari's ESC (or Map's back-out), not "close the panel". */
- (void)cancelOperation:(id)sender { (void)sender; }
@end

/* ---- the panel ------------------------------------------------------------------- */

@implementation A800ControllersWindow {
    a800session *_session;
    A800ControllersPanel *_panel;
    A800CtlFlippedView *_content;
    A800StageView *_stage;
    A800KeyboardView *_kbd;
    BOOL _showKbd;
    NSTextField *_padsTitle;
    NSTextField *_noPads;
    NSTextField *_padName[MAX_PAD_ROWS];
    NSTextField *_padEff[MAX_PAD_ROWS];     /* each pad's "plays port N" */
    NSPopUpButton *_padAssign[MAX_PAD_ROWS];
    int _padRows;                           /* the rows showing a pad */
    int _reservedRows;                      /* the rows the panel has room for */
    unsigned _padGeneration;
    NSButton *_mapButton;
    NSButton *_clearButton;
    NSButton *_defaultsButton;
    NSButton *_showKeysBox;
    NSButton *_kbdBox;
    NSTextField *_pickLabel;
    NSPopUpButton *_pickPopup;              /* every target, for Map */
    NSButton *_helpButton;
    NSTextField *_hint;
    NSTimer *_timer;
    int _slow;
    uint32_t _swallowKey;                   /* the key Map took: its repeats and release too */
    /* -2 idle, -1 armed and waiting for a control, >= 0 waiting for a key or
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
    _padGeneration = (unsigned)-1;
    _reservedRows = MIN_PAD_ROWS;

    _panel = [[A800ControllersPanel alloc]
        initWithContentRect:NSMakeRect(0, 0, WIN_W, 700)
                  styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskUtilityWindow)
                    backing:NSBackingStoreBuffered
                      defer:NO];
    [_panel setTitle:@"Controllers"];
    A800SetHelpContext(_panel, @"controllers");
    /* Floats above the machine's window, a full-screen one too. */
    [_panel setLevel:NSFloatingWindowLevel];
    [_panel setCollectionBehavior:NSWindowCollectionBehaviorFullScreenAuxiliary];
    [_panel setReleasedWhenClosed:NO];
    [_panel setBecomesKeyOnlyIfNeeded:NO];
    _panel.owner = self;
    [_panel setDelegate:self];

    _content = [[A800CtlFlippedView alloc] initWithFrame:NSMakeRect(0, 0, WIN_W, 700)];
    [_content setAutoresizesSubviews:NO];
    [_panel setContentView:_content];

    _stage = [[A800StageView alloc] initWithFrame:NSZeroRect];
    _stage.owner = self;
    _stage.session = _session;
    _stage.mapTarget = -1;
    _stage.showKeys = a800session_get_int(_session, "controllers_show_bindings", 0) > 0;
    [_content addSubview:_stage];

    _showKbd = a800session_get_int(_session, "controllers_keyboard", 0) > 0;
    _kbd = [[A800KeyboardView alloc] initWithFrame:NSZeroRect];
    _kbd.owner = self;
    _kbd.session = _session;
    _kbd.mapTarget = -1;
    [_content addSubview:_kbd];

    /* Gamepads: one row per pad, its name, the port it plays and the one it is
     * set to; rows reserved, so pads coming and going keep the size. */
    _padsTitle = [NSTextField labelWithString:@"Gamepads"];
    [_padsTitle setFont:[NSFont boldSystemFontOfSize:12]];
    [_content addSubview:_padsTitle];
    _noPads = [NSTextField labelWithString:@"No gamepads connected - plug one in"];
    [_noPads setTextColor:[NSColor secondaryLabelColor]];
    [_content addSubview:_noPads];
    for (int row = 0; row < MAX_PAD_ROWS; row++) {
        _padName[row] = [NSTextField labelWithString:@""];
        [_padName[row] setFont:[NSFont systemFontOfSize:11]];
        [_padName[row] setLineBreakMode:NSLineBreakByTruncatingTail];
        [_content addSubview:_padName[row]];
        _padEff[row] = [NSTextField labelWithString:@""];
        [_padEff[row] setFont:[NSFont systemFontOfSize:11]];
        [_padEff[row] setTextColor:[NSColor secondaryLabelColor]];
        [_padEff[row] setAlignment:NSTextAlignmentRight];
        [_content addSubview:_padEff[row]];
        /* 0 = automatic (connection order), 1-4 = that port */
        _padAssign[row] = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
        [_padAssign[row] addItemsWithTitles:@[@"Automatic", @"Port 1", @"Port 2", @"Port 3", @"Port 4"]];
        _padAssign[row].tag = row;
        _padAssign[row].target = self;
        _padAssign[row].action = @selector(padAssignChanged:);
        [_padAssign[row] setRefusesFirstResponder:YES];
        [_content addSubview:_padAssign[row]];
    }

    _mapButton = [NSButton buttonWithTitle:@"Map" target:self action:@selector(toggleMap:)];
    [_mapButton setRefusesFirstResponder:YES];
    [_content addSubview:_mapButton];
    _clearButton = [NSButton buttonWithTitle:@"Clear" target:self action:@selector(clearBinding:)];
    [_clearButton setRefusesFirstResponder:YES];
    [_content addSubview:_clearButton];
    _defaultsButton = [NSButton buttonWithTitle:@"Defaults" target:self action:@selector(restoreDefaults:)];
    [_defaultsButton setRefusesFirstResponder:YES];
    [_content addSubview:_defaultsButton];
    _showKeysBox = [NSButton checkboxWithTitle:@"Show bindings" target:self action:@selector(toggleShowKeys:)];
    [_showKeysBox setState:_stage.showKeys ? NSControlStateValueOn : NSControlStateValueOff];
    [_showKeysBox setRefusesFirstResponder:YES];
    [_content addSubview:_showKeysBox];
    _kbdBox = [NSButton checkboxWithTitle:@"Keyboard" target:self action:@selector(toggleKeyboard:)];
    [_kbdBox setState:_showKbd ? NSControlStateValueOn : NSControlStateValueOff];
    [_kbdBox setRefusesFirstResponder:YES];
    [_content addSubview:_kbdBox];
    _pickLabel = [NSTextField labelWithString:@"Control"];
    [_content addSubview:_pickLabel];
    _pickPopup = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    _pickPopup.target = self;
    _pickPopup.action = @selector(pickChanged:);
    [_pickPopup setRefusesFirstResponder:YES];
    [_content addSubview:_pickPopup];
    _helpButton = A800HelpButton(@"controllers");
    [_helpButton setRefusesFirstResponder:YES];
    [_content addSubview:_helpButton];
    _hint = [NSTextField wrappingLabelWithString:@""];
    [_hint setFont:[NSFont systemFontOfSize:[NSFont smallSystemFontSize]]];
    [_hint setTextColor:[NSColor secondaryLabelColor]];
    [_hint setMaximumNumberOfLines:2];
    [[_hint cell] setTruncatesLastVisibleLine:YES];
    [_content addSubview:_hint];

    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(occlusionChanged:)
                                                 name:NSWindowDidChangeOcclusionStateNotification
                                               object:_panel];

    [_stage configure];
    [_kbd reload];
    [self rebuildPickList];
    [self setMapState:-2];
    [self relayout];
    return self;
}

- (void)dealloc
{
    [[NSNotificationCenter defaultCenter] removeObserver:self];
    [_timer invalidate];
    [_panel setDelegate:nil];
}

/* ---- layout ------------------------------------------------------------------------- */

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
    const NSSize ssize = _stage.contentSize;
    const NSSize ksize = _kbd.contentSize;
    const CGFloat inner = WIN_W - 2 * MARGIN;
    CGFloat ypos = ssize.height + 8;
    const CGFloat kbdY = ypos;
    if (_showKbd) ypos += ksize.height + 8;
    const CGFloat padsY = ypos;
    const CGFloat rowsY = padsY + TITLE_H;
    const CGFloat mapY = rowsY + _reservedRows * ROW_H + 6;
    const CGFloat pickY = mapY + BTN_H + 6;
    const CGFloat hintY = pickY + 26 + 4;
    const CGFloat total = hintY + 30 + MARGIN;

    [self fitPanel:NSMakeSize(WIN_W, total)];
    [_stage setFrame:NSMakeRect(0, 0, ssize.width, ssize.height)];
    [_kbd setHidden:!_showKbd];
    [_kbd setFrame:NSMakeRect(floor((WIN_W - ksize.width) / 2), kbdY, ksize.width, _showKbd ? ksize.height : 0)];
    [_padsTitle setFrame:NSMakeRect(MARGIN, padsY, inner, 16)];
    [_noPads setFrame:NSMakeRect(MARGIN, rowsY + 3, inner, 18)];
    for (int row = 0; row < MAX_PAD_ROWS; row++) {
        const CGFloat rowY = rowsY + row * ROW_H;
        [_padName[row] setFrame:NSMakeRect(MARGIN, rowY + 4, inner - POPUP_W - EFF_W - 12, 16)];
        [_padEff[row] setFrame:NSMakeRect(MARGIN + inner - POPUP_W - EFF_W - 6, rowY + 4, EFF_W, 16)];
        [_padAssign[row] setFrame:NSMakeRect(MARGIN + inner - POPUP_W, rowY - 1, POPUP_W, 25)];
    }
    [_mapButton setFrame:NSMakeRect(MARGIN, mapY, 56, BTN_H)];
    [_clearButton setFrame:NSMakeRect(MARGIN + 62, mapY, 56, BTN_H)];
    [_defaultsButton setFrame:NSMakeRect(MARGIN + 124, mapY, 72, BTN_H)];
    [_showKeysBox setFrame:NSMakeRect(MARGIN + 206, mapY + 3, 122, 22)];
    [_kbdBox setFrame:NSMakeRect(MARGIN + 336, mapY + 3, 96, 22)];
    [_helpButton setFrameOrigin:NSMakePoint(WIN_W - MARGIN - _helpButton.frame.size.width, mapY + 2)];
    [_pickLabel setFrame:NSMakeRect(MARGIN, pickY + 4, 50, 18)];
    [_pickPopup setFrame:NSMakeRect(MARGIN + 54, pickY, inner - 54, 25)];
    [_hint setFrame:NSMakeRect(MARGIN, hintY, inner, 30)];
    [_stage setNeedsDisplay:YES];
    [_kbd setNeedsDisplay:YES];
}

/* Lets go of what the mouse holds in this panel. */
- (void)letGo
{
    [_stage letGo];
    [_kbd letGo];
}

/* The ports, the machine's keyboard or a port's device changed (Settings, a
 * power cycle): the layouts are read again; the panel changes size only if the
 * ports or the keyboard do. */
- (void)syncLayouts
{
    BOOL changed = NO;
    if ([_stage layoutStale]) {
        [self letGo];
        [_stage configure];
        changed = YES;
    }
    if ([_kbd stale]) {
        [_kbd letGo];
        [_kbd reload];
        changed = YES;
    }
    if (changed) [self relayout];
}

/* ---- Map mode --------------------------------------------------------------------- */

- (void)rebuildPickList
{
    [_pickPopup removeAllItems];
    for (int target = 0; target < A800_TARGET_COUNT; target++)
        [[_pickPopup menu] addItemWithTitle:nsstr(a800_target_name(target)) action:nil keyEquivalent:@""];
}

/* The bound host key as the picture's legend. */
- (NSString *)legendForTarget:(int)target
{
    return keyName(a800session_binding_get(_session, target).keysym);
}

/* What is bound to a target, for the status line. */
- (NSString *)bindingText:(int)target
{
    const a800_binding bind = a800session_binding_get(_session, target);
    NSString *key = bind.keysym ? keyName(bind.keysym) : @"no key";
    if (bind.button != A800_PAD_BTN_NONE)
        return [NSString stringWithFormat:@"%@, pad %@", key, nsstr(a800_pad_button_name(bind.button))];
    return key;
}

/* In Map mode nothing is lit and every control shows what is bound to it. */
- (void)refreshLabels
{
    _stage.mapping = _mapState != -2;
    _stage.mapTarget = _mapState >= 0 ? _mapState : -1;
    _kbd.mapping = _mapState != -2;
    _kbd.mapTarget = _mapState >= 0 ? _mapState : -1;
    [_stage setNeedsDisplay:YES];
    [_kbd setNeedsDisplay:YES];
    [_stage poll];
    [_kbd poll];
}

- (void)setMapState:(int)state
{
    _mapState = state;
    if (state >= 0) {
        [self letGo];
        a800session_release_all(_session);
        a800session_gamepad_capture_begin(_session);
        _hint.stringValue = [NSString stringWithFormat:@"Press a key or a gamepad button for %@ (now: %@). Esc backs out.",
                             nsstr(a800_target_name(state)), [self bindingText:state]];
    } else {
        a800session_gamepad_capture_cancel(_session);
        _hint.stringValue = state == -1
            ? @"Click a control, a console key or a label, or choose one from the list, to remap it."
            : @"Hold a button, drag the stick or a knob, press a key. Keys and gamepads work here too.";
    }
    [_mapButton setTitle:(state == -2 ? @"Map" : @"Done")];
    setButtonAccent(_mapButton, state != -2);
    _clearButton.enabled = state >= 0;
    _pickPopup.enabled = state != -2;
    if (state >= 0 && state < _pickPopup.numberOfItems) [_pickPopup selectItemAtIndex:state];
    [self refreshLabels];
}

/* A control clicked on the picture in Map mode. */
- (void)mapPick:(int)target
{
    if (_mapState != -2 && target >= 0 && target < A800_TARGET_COUNT) [self setMapState:target];
}

- (void)pickChanged:(NSPopUpButton *)sender
{
    if (_mapState == -2) return;
    const NSInteger sel = sender.indexOfSelectedItem;
    if (sel >= 0 && sel < A800_TARGET_COUNT) [self setMapState:(int)sel];
}

- (void)showBound:(NSString *)name target:(int)target stolen:(const char *)stolen
{
    NSString *what = nsstr(a800_target_name(target));
    _hint.stringValue = stolen && stolen[0]
        ? [NSString stringWithFormat:@"Bound %@ to %@ (was %@)", name, what, nsstr(stolen)]
        : [NSString stringWithFormat:@"Bound %@ to %@", name, what];
}

- (void)bindKey:(uint32_t)keysym swallow:(BOOL)swallow
{
    const int target = _mapState;
    char stolen[128];
    stolen[0] = '\0';
    a800session_binding_set_key(_session, target, keysym, stolen, (int)sizeof stolen);
    if (swallow) _swallowKey = keysym;
    [self setMapState:-1];   /* stay armed: remapping several in a row is normal */
    [self showBound:keyName(keysym) target:target stolen:stolen];
}

- (void)pollCapture
{
    int button = A800_PAD_BTN_NONE;
    if (_mapState < 0 || !a800session_gamepad_capture_poll(_session, &button)) return;
    const int target = _mapState;
    char stolen[128];
    stolen[0] = '\0';
    a800session_binding_set_button(_session, target, button, stolen, (int)sizeof stolen);
    [self setMapState:-1];   /* stay armed: remapping several in a row is normal */
    [self showBound:nsstr(a800_pad_button_name(button)) target:target stolen:stolen];
}

- (void)toggleMap:(id)sender
{
    (void)sender;
    [self setMapState:(_mapState == -2 ? -1 : -2)];
}

/* Clear: the picked control loses its key and its gamepad button. */
- (void)clearBinding:(id)sender
{
    (void)sender;
    if (_mapState < 0) return;
    const int target = _mapState;
    a800session_binding_clear(_session, target);
    [self setMapState:-1];
    _hint.stringValue = [NSString stringWithFormat:@"Cleared %@", nsstr(a800_target_name(target))];
}

- (void)restoreDefaults:(id)sender
{
    (void)sender;
    a800session_bindings_reset(_session);
    [self refreshLabels];
    _hint.stringValue = @"Default bindings restored";
}

- (void)toggleShowKeys:(id)sender
{
    (void)sender;
    const BOOL show = [_showKeysBox state] == NSControlStateValueOn;
    a800session_set_int(_session, "controllers_show_bindings", show ? 1 : 0);
    _stage.showKeys = show;
    [_stage setNeedsDisplay:YES];
}

/* The Atari keyboard under the ports: shown or folded away, and remembered. */
- (void)toggleKeyboard:(id)sender
{
    (void)sender;
    _showKbd = [_kbdBox state] == NSControlStateValueOn;
    a800session_set_int(_session, "controllers_keyboard", _showKbd ? 1 : 0);
    if (!_showKbd) [_kbd letGo];
    [self relayout];
}

/* ---- gamepads ----------------------------------------------------------------------- */

- (void)refreshPadPorts
{
    for (int row = 0; row < _padRows; row++) {
        const int port = a800session_gamepad_effective_port(_session, row);
        NSString *text = port < 0 ? @"plays nothing" : [NSString stringWithFormat:@"plays port %d", port + 1];
        if (![_padEff[row].stringValue isEqualToString:text]) _padEff[row].stringValue = text;
    }
}

- (void)refreshPads
{
    const int count = a800session_gamepad_count(_session);
    int nports = a800session_active_ports(_session);
    if (nports < 1) nports = 1;
    if (nports > A800_PORTS) nports = A800_PORTS;
    _padGeneration = a800session_gamepad_generation(_session);
    _padRows = count < MAX_PAD_ROWS ? (count > 0 ? count : 0) : MAX_PAD_ROWS;
    /* room for the pads there are (two rows to start with, more when more
     * pads are connected); the panel changes size only then */
    const int rooms = _padRows > MIN_PAD_ROWS ? _padRows : MIN_PAD_ROWS;
    if (rooms != _reservedRows) {
        _reservedRows = rooms;
        [self relayout];
    }
    _noPads.hidden = count > 0;
    _padsTitle.stringValue = count > MAX_PAD_ROWS
        ? [NSString stringWithFormat:@"Gamepads (%d of %d shown)", MAX_PAD_ROWS, count]
        : @"Gamepads";
    for (int row = 0; row < MAX_PAD_ROWS; row++) {
        const BOOL shown = row < _padRows;
        _padName[row].hidden = !shown;
        _padEff[row].hidden = !shown;
        _padAssign[row].hidden = !shown;
        if (!shown) continue;
        char name[128];
        name[0] = '\0';
        a800session_gamepad_name(_session, row, name, (int)sizeof name);
        _padName[row].stringValue = nsstr(name);
        _padName[row].toolTip = nsstr(name);
        const int assign = a800session_gamepad_assignment(_session, row);
        /* ports the machine does not have are not offered (a pad still assigned to one stays shown) */
        for (int item = 1; item <= A800_PORTS; item++) {
            NSMenuItem *entry = [_padAssign[row] itemAtIndex:item];
            [entry setEnabled:item - 1 < nports || item - 1 == assign];
        }
        [_padAssign[row] selectItemAtIndex:(assign < 0 || assign >= A800_PORTS ? 0 : assign + 1)];
    }
    [self refreshPadPorts];
}

- (void)padAssignChanged:(NSPopUpButton *)sender
{
    const int sel = (int)sender.indexOfSelectedItem;
    a800session_gamepad_assign(_session, (int)sender.tag, sel <= 0 ? -1 : sel - 1);
    [self refreshPadPorts];
}

/* ---- the poll ----------------------------------------------------------------------- */

/* 30 Hz while the panel can be seen: what is held and the gamepad capture;
 * every 250 ms the hot-plugged pads, auto-assignment, and the machine or a
 * port's device changed in Settings. */
- (void)tick
{
    if (_mapState >= 0) [self pollCapture];
    if (++_slow >= SLOW_TICKS) {
        _slow = 0;
        if (a800session_gamepad_generation(_session) != _padGeneration) [self refreshPads];
        else [self refreshPadPorts];
        [self syncLayouts];
    }
    [_stage poll];
    if (_showKbd) [_kbd poll];
}

/* The timer runs only while the panel is on screen and not occluded or
 * hidden with its app. */
- (void)updateTimer
{
    const BOOL want = [_panel isVisible] && ([_panel occlusionState] & NSWindowOcclusionStateVisible);
    if (want && !_timer) {
        __weak A800ControllersWindow *weakSelf = self;
        _timer = [NSTimer scheduledTimerWithTimeInterval:1.0 / 30 repeats:YES block:^(NSTimer *tmr) {
            (void)tmr;
            [weakSelf tick];
        }];
        _timer.tolerance = 0.005;
    } else if (!want && _timer) {
        [_timer invalidate];
        _timer = nil;
    }
}

- (void)occlusionChanged:(NSNotification *)note
{
    (void)note;
    [self updateTimer];
}

/* ---- the keyboard --------------------------------------------------------------- */

/* The app's own keys, which A800ForwardKey leaves to the caller: F9 hides
 * this panel; F11, F12 and Shift+F5 do what they do in the main window. */
- (BOOL)appKey:(NSEvent *)evt keysym:(uint32_t)keysym
{
    const NSEventModifierFlags flags = [evt modifierFlags];
    if (flags & NSEventModifierFlagCommand) return NO;
    if (keysym == A800_KEYSYM_F9) {
        if (![evt isARepeat]) [self toggle];
        return YES;
    }
    if (keysym == A800_KEYSYM_F11 || keysym == A800_KEYSYM_F12 ||
        (keysym == A800_KEYSYM_F5 && (flags & NSEventModifierFlagShift))) {
        NSWindow *mainWindow = [NSApp mainWindow];
        if (![evt isARepeat] && mainWindow && mainWindow != _panel) [[mainWindow firstResponder] keyDown:evt];
        return YES;
    }
    return NO;
}

/* A key typed while Map waits for one: Esc backs out of the pick; a Ctrl chord
 * and the app's F9, F11 and F12 are not bindings. */
- (void)mapKey:(uint32_t)keysym event:(NSEvent *)evt
{
    if ([evt modifierFlags] & NSEventModifierFlagControl) return;
    if (a800session_key_reserved(keysym)) return;
    if (keysym == A800_KEYSYM_ESCAPE) {
        [self setMapState:-1];
        _swallowKey = keysym;
        return;
    }
    [self bindKey:keysym swallow:YES];
}

- (void)panelKeyDown:(NSEvent *)evt
{
    const uint32_t keysym = A800KeysymFromEvent(evt);
    if (_swallowKey && keysym == _swallowKey) return;
    if ([self appKey:evt keysym:keysym]) return;
    const BOOL plain = ![evt isARepeat] && !([evt modifierFlags] & NSEventModifierFlagCommand);
    if (_mapState >= 0) {
        /* the next key is the binding */
        if (plain && keysym) [self mapKey:keysym event:evt];
        return;
    }
    if (_mapState == -1) return;    /* armed: typing does nothing */
    if (plain && keysym) {
        /* a key bound to a system action goes to the app's queue, as a gamepad's does */
        const int sysact = a800session_key_sysaction(_session, keysym);
        if (sysact >= 0) {
            a800session_sysaction_post(_session, sysact);
            return;
        }
    }
    (void)A800ForwardKey(_session, evt, YES);
}

- (void)panelKeyUp:(NSEvent *)evt
{
    const uint32_t keysym = A800KeysymFromEvent(evt);
    if (_swallowKey && keysym == _swallowKey) {
        _swallowKey = 0;
        return;
    }
    if (_mapState != -2) return;
    (void)A800ForwardKey(_session, evt, NO);
}

/* Shift, Control and Option bind too (Command is the menus'); otherwise
 * they are the Atari's. A release always goes through. */
- (void)panelFlagsChanged:(NSEvent *)evt
{
    if (_mapState >= 0) {
        const uint32_t keysym = a800session_keysym_from_macos_keycode([evt keyCode]);
        NSEventModifierFlags mask = 0;
        if (keysym == A800_KEYSYM_LSHIFT || keysym == A800_KEYSYM_RSHIFT) mask = NSEventModifierFlagShift;
        else if (keysym == A800_KEYSYM_LCTRL || keysym == A800_KEYSYM_RCTRL) mask = NSEventModifierFlagControl;
        else if (keysym == A800_KEYSYM_LALT || keysym == A800_KEYSYM_RALT) mask = NSEventModifierFlagOption;
        if (mask && ([evt modifierFlags] & mask)) [self bindKey:keysym swallow:NO];
        return;
    }
    if (_mapState == -1) return;
    A800ForwardFlags(_session, evt);
}

/* ---- the window --------------------------------------------------------------------- */

/* Hidden: Map disarmed, nothing held, no polling. */
- (void)quiesce
{
    [self setMapState:-2];
    [self letGo];
    a800session_release_all(_session);
    _swallowKey = 0;
    [_timer invalidate];
    _timer = nil;
}

- (void)show
{
    [self syncLayouts];
    [self rebuildPickList];
    [self refreshPads];
    [self setMapState:-2];
    if (!_placed) {
        [_panel center];
        _placed = YES;
    }
    [_panel makeFirstResponder:nil];    /* the panel itself takes the keys */
    [_panel makeKeyAndOrderFront:nil];
    [self updateTimer];
}

- (void)hide
{
    [_panel orderOut:nil];
    [self quiesce];
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

/* The machine, a port's device, the bindings or the gamepads changed elsewhere
 * (Settings, a hot-plug): read again. */
- (void)refresh
{
    [self syncLayouts];
    [self refreshPads];
    [self refreshLabels];
}

/* The panel's mouse and keyboard holds: the main window releases its own. */
- (void)windowDidResignKey:(NSNotification *)note
{
    (void)note;
    [self letGo];
    a800session_release_all(_session);
    _swallowKey = 0;    /* its release goes elsewhere */
}

- (void)windowWillClose:(NSNotification *)note
{
    (void)note;
    [self quiesce];
}
@end
