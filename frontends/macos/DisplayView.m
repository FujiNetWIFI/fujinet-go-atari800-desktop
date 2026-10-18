/*
 * A800DisplayView -- the framebuffer, on a CVDisplayLink.
 *
 * The Atari paces itself (core/atari800/A800Host.cpp) and publishes a frame
 * when one is done; the display link says when the panel refreshes, which
 * the emulator phase-locks to. Its callback runs on its OWN thread, not the
 * main one, so it does the cheap thing (pull the frame) and asks AppKit to
 * redraw on the main thread.
 *
 * The frame's size changes with the visible area, the NTSC filter and the
 * 80-column cards; its pixel aspect (x1000) says how wide each pixel is
 * against the Atari's own. TV aspect then gives the Atari's pixels their
 * shape on a TV (NTSC 6:7, PAL a little wide) and shows an 80-column
 * card's screen 4:3, like the monitor it drove.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "DisplayView.h"

#import <CoreVideo/CoreVideo.h>

#include <stdlib.h>
#include <string.h>

#define FB_PIXELS ((size_t)A800SESSION_FB_MAX_WIDTH * A800SESSION_FB_MAX_HEIGHT)

@implementation A800DisplayView {
    a800session *_session;
    CVDisplayLinkRef _link;
    uint32_t *_fb;              /* written by the link's thread */
    uint32_t *_shown;           /* what drawRect: draws (main thread) */
    int _fw, _fh, _fpar;        /* _fb's frame (under _lock) */
    int _w, _h, _par;           /* _shown's */
    uint64_t _serial;
    uint64_t _frames;
    BOOL _fresh;                /* _fb holds a frame drawRect: has not taken */
    NSLock *_lock;
    CGColorSpaceRef _cs;
    BOOL _tv;
    BOOL _smooth;
    NSRect _picture;            /* where the picture is, for the pointer */
    NSPoint _last;
    BOOL _haveLast;
    NSTrackingArea *_tracking;
}

static CVReturn displayCallback(CVDisplayLinkRef link, const CVTimeStamp *now,
                                const CVTimeStamp *out, CVOptionFlags flagsIn,
                                CVOptionFlags *flagsOut, void *ctx)
{
    (void)link; (void)now; (void)out; (void)flagsIn; (void)flagsOut;
    A800DisplayView *self = (__bridge A800DisplayView *)ctx;
    [self tick];
    return kCVReturnSuccess;
}

- (instancetype)initWithSession:(a800session *)session
{
    self = [super initWithFrame:NSMakeRect(0, 0, 864, 720)];
    if (!self) return nil;
    _session = session;
    _tv = YES;
    _lock = [[NSLock alloc] init];
    _fb = calloc(FB_PIXELS, sizeof *_fb);
    _shown = calloc(FB_PIXELS, sizeof *_shown);
    _w = _fw = 336;
    _h = _fh = 240;
    _par = _fpar = 1000;
    _cs = CGColorSpaceCreateDeviceRGB();

    CVDisplayLinkCreateWithActiveCGDisplays(&_link);
    CVDisplayLinkSetOutputCallback(_link, displayCallback, (__bridge void *)self);
    CVDisplayLinkStart(_link);
    return self;
}

- (void)stop
{
    if (_link) {
        CVDisplayLinkStop(_link);
        CVDisplayLinkRelease(_link);
        _link = NULL;
    }
}

- (void)dealloc
{
    [self stop];
    if (_cs) CGColorSpaceRelease(_cs);
    free(_fb);
    free(_shown);
}

- (void)tick
{
    int w = 0, h = 0, par = 1000;
    a800session_notify_vsync(_session, 0);   /* the host stamps its own clock */
    [_lock lock];
    const int got = a800session_copy_frame(_session, _fb, &w, &h, &par, &_serial) && w > 0 && h > 0;
    if (got) {
        _fw = w;
        _fh = h;
        _fpar = par > 0 ? par : 1000;
        _fresh = YES;
    }
    [_lock unlock];
    if (!got) return;
    /* AppKit is main-thread only; the display link's callback is not. */
    dispatch_async(dispatch_get_main_queue(), ^{ [self setNeedsDisplay:YES]; });
}

- (void)setTvAspect:(BOOL)tv { _tv = tv; [self setNeedsDisplay:YES]; }
- (void)setSmooth:(BOOL)smooth { _smooth = smooth; [self setNeedsDisplay:YES]; }
- (uint64_t)frames { return _frames; }

- (BOOL)isOpaque { return YES; }

- (double)pictureAspect
{
    const double w = _w * _par / 1000.0, h = _h;
    if (!_tv) return w / h;
    if (_w >= 560 && _par == 500) return 4.0 / 3.0;   /* an 80-column card's monitor */
    /* an Atari pixel on an NTSC TV is 6/7 as wide as tall; on PAL, 1.03 */
    return w * (a800session_refresh_rate(_session) > 55000 ? 6.0 / 7.0 : 1.03) / h;
}

- (CGImageRef)newImage
{
    /* The session's pixels are 0x00RRGGBB in host order: a little-endian
     * 32-bit image with the alpha byte skipped reads them as they are. */
    CGDataProviderRef dp = CGDataProviderCreateWithData(NULL, _shown, (size_t)_w * _h * 4, NULL);
    if (!dp) return NULL;
    CGImageRef img = CGImageCreate((size_t)_w, (size_t)_h, 8, 32, (size_t)_w * 4, _cs,
                                   (CGBitmapInfo)kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little,
                                   dp, NULL, false, kCGRenderingIntentDefault);
    CGDataProviderRelease(dp);
    return img;
}

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    CGContextRef dc = [[NSGraphicsContext currentContext] CGContext];
    const NSRect b = [self bounds];

    CGContextSetRGBFillColor(dc, 0, 0, 0, 1);
    CGContextFillRect(dc, b);

    [_lock lock];
    if (_fresh) {
        memcpy(_shown, _fb, (size_t)_fw * _fh * sizeof *_fb);
        _w = _fw;
        _h = _fh;
        _par = _fpar;
        _fresh = NO;
        _frames++;
    }
    [_lock unlock];
    CGImageRef img = [self newImage];
    if (!img) return;

    const double want = [self pictureAspect];
    const double w = b.size.width, h = b.size.height;
    double sw, sh;
    if (w / h > want) { sh = h; sw = sh * want; }
    else              { sw = w; sh = sw / want; }
    _picture = NSMakeRect((w - sw) / 2, (h - sh) / 2, sw, sh);

    CGContextSetInterpolationQuality(dc, _smooth ? kCGInterpolationHigh : kCGInterpolationNone);
    CGContextDrawImage(dc, NSRectToCGRect(_picture), img);
    CGImageRelease(img);
}

- (BOOL)savePng:(NSString *)path
{
    CGImageRef img = [self newImage];
    if (!img) return NO;
    NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithCGImage:img];
    CGImageRelease(img);
    NSData *png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    return png && [png writeToFile:path atomically:YES];
}

/* ---- the pointer: mice, Koala Pad, light pen and gun ---------------------- */

- (void)updateTrackingAreas
{
    [super updateTrackingAreas];
    if (_tracking) [self removeTrackingArea:_tracking];
    _tracking = [[NSTrackingArea alloc] initWithRect:[self bounds]
        options:NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited | NSTrackingActiveInKeyWindow
        owner:self userInfo:nil];
    [self addTrackingArea:_tracking];
}

- (void)pointer:(NSEvent *)e
{
    if (!a800session_pointer_device_active(_session) || _picture.size.width <= 0) return;
    const NSUInteger pressed = [NSEvent pressedMouseButtons];
    const unsigned buttons = ((pressed & 1) ? 1u : 0u) | ((pressed & 2) ? 2u : 0u);
    NSPoint p = [self convertPoint:[e locationInWindow] fromView:nil];
    /* AppKit counts up from the bottom; the frame counts down from the top */
    const int fx = (int)((p.x - _picture.origin.x) * _w / _picture.size.width);
    const int fy = (int)((_picture.origin.y + _picture.size.height - p.y) * _h / _picture.size.height);
    const int inside = fx >= 0 && fy >= 0 && fx < _w && fy < _h;
    a800session_pointer(_session, fx < 0 ? 0 : fx >= _w ? _w - 1 : fx,
                        fy < 0 ? 0 : fy >= _h ? _h - 1 : fy, inside, buttons);
    if (_haveLast) {
        /* mice count movement: in frame pixels, as the picture is drawn */
        const int dx = (int)((p.x - _last.x) * _w / _picture.size.width);
        const int dy = (int)((_last.y - p.y) * _h / _picture.size.height);
        if (dx || dy) a800session_pointer_motion(_session, dx, dy, buttons);
        if (!dx) p.x = _last.x;
        if (!dy) p.y = _last.y;
    }
    _last = p;
    _haveLast = YES;
}

- (void)mouseMoved:(NSEvent *)e { [self pointer:e]; }
- (void)mouseDragged:(NSEvent *)e { [self pointer:e]; }
- (void)rightMouseDragged:(NSEvent *)e { [self pointer:e]; }
- (void)mouseDown:(NSEvent *)e { [self pointer:e]; }
- (void)mouseUp:(NSEvent *)e { [self pointer:e]; }
- (void)rightMouseDown:(NSEvent *)e { [self pointer:e]; }
- (void)rightMouseUp:(NSEvent *)e { [self pointer:e]; }

- (void)mouseExited:(NSEvent *)e
{
    (void)e;
    _haveLast = NO;
    if (a800session_pointer_device_active(_session))
        a800session_pointer(_session, 0, 0, 0, 0);
}
@end
