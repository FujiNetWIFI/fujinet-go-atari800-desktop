/*
 * DisplayExtras.inc -- the pointer (mice, Koala Pad, light pen and gun) and the
 * screenshot hooks, included into the display view's @implementation (see
 * DisplayGlue.h).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/* Frames pulled since the view was made (A800_AUTOQUIT_FRAMES). */
- (uint64_t)frames { return atomic_load(&_frameCount); }

/* The last frame as a PNG (A800_SCREENSHOT). */
- (BOOL)savePng:(NSString *)path
{
    os_unfair_lock_lock(&_fbLock);
    const int w = _width, h = _height;
    NSMutableData *copy = w > 0 && h > 0 ? [NSMutableData dataWithBytes:_fb length:(size_t)w * h * 4] : nil;
    os_unfair_lock_unlock(&_fbLock);
    if (!copy) return NO;
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGDataProviderRef dp = CGDataProviderCreateWithCFData((__bridge CFDataRef)copy);
    CGImageRef img = CGImageCreate((size_t)w, (size_t)h, 8, 32, (size_t)w * 4, cs,
                                   (CGBitmapInfo)kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little,
                                   dp, NULL, false, kCGRenderingIntentDefault);
    CGDataProviderRelease(dp);
    CGColorSpaceRelease(cs);
    if (!img) return NO;
    NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithCGImage:img];
    CGImageRelease(img);
    NSData *png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    return png && [png writeToFile:path atomically:YES];
}

/* ---- the pointer: mice, Koala Pad, light pen and gun ---------------------- */

- (void)extraInit {}

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
    const NSRect picture = [self pictureRect];
    if (!a800session_pointer_device_active(_session) || picture.size.width <= 0) return;
    const NSUInteger pressed = [NSEvent pressedMouseButtons];
    const unsigned buttons = ((pressed & 1) ? 1u : 0u) | ((pressed & 2) ? 2u : 0u);
    NSPoint p = [self convertPoint:[e locationInWindow] fromView:nil];
    /* AppKit counts up from the bottom; the frame counts down from the top */
    const int fx = (int)((p.x - picture.origin.x) * _width / picture.size.width);
    const int fy = (int)((picture.origin.y + picture.size.height - p.y) * _height / picture.size.height);
    const int inside = fx >= 0 && fy >= 0 && fx < _width && fy < _height;
    a800session_pointer(_session, fx < 0 ? 0 : fx >= _width ? _width - 1 : fx,
                        fy < 0 ? 0 : fy >= _height ? _height - 1 : fy, inside, buttons);
    if (_haveLast) {
        /* mice count movement: in frame pixels, as the picture is drawn */
        const int dx = (int)((p.x - _last.x) * _width / picture.size.width);
        const int dy = (int)((_last.y - p.y) * _height / picture.size.height);
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
