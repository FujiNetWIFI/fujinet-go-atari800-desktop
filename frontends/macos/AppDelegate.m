/*
 * A800AppDelegate -- the window, the menu bar and the session's lifetime.
 *
 * The same app as the GNOME, KDE and Win32 frontends: the Atari's picture
 * with a status line under it (the FujiNet link, the machine), menus for
 * the machine, its drives, cartridges and tape, the console keys, the
 * clipboard, the view and FujiNet, and the Controllers, Settings and
 * debugger windows. Command is the Mac's shortcut key, so Control stays
 * the Atari's (its CONTROL+letter keys are graphics characters).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "AppDelegate.h"

#import <Carbon/Carbon.h>   /* kVK_* virtual key codes */

#import "DisplayView.h"
#import "KeyForward.h"
#import "SettingsWindow.h"
#import "controllers/ControllersWindow.h"
#import "debugger/DebuggerWindow.h"

#include <stdlib.h>
#include <string.h>

#ifndef A800_VERSION_STRING
#define A800_VERSION_STRING "unknown"
#endif

#define APP_TITLE @"FujiNet Go Atari 800"
#define STATUS_HEIGHT 24.0
#define MESSAGE_SECONDS 4.0

/* Drive menu items: the tag is the drive and the action. */
enum { DRIVE_INSERT = 0, DRIVE_EJECT = 1, DRIVE_WP = 2, DRIVE_BLANK = 3 /* + density */ };
#define DRIVE_TAG(d, action) ((d) * 8 + (action))

static NSString *str(const char *s)
{
    return s ? [NSString stringWithUTF8String:s] : @"";
}

@class A800AppDelegate;

/* The content view sits between AppKit and the session: it forwards keys
 * and the drop, and leaves the display purely about pixels. */
@interface A800ContentView : NSView
@property (nonatomic) a800session *session;
@property (nonatomic, weak) A800AppDelegate *owner;
@end

@interface A800AppDelegate ()
- (void)loadMedia:(NSString *)path;
- (void)runSysaction:(int)sa;
@end

@implementation A800ContentView

- (BOOL)acceptsFirstResponder { return YES; }

- (void)keyDown:(NSEvent *)e
{
    if (!([e modifierFlags] & NSEventModifierFlagCommand) && ![e isARepeat]) {
        switch ([e keyCode]) {
        case kVK_F9: [self.owner toggleControllers:nil]; return;
        case kVK_F11: [self.owner toggleFullScreen:nil]; return;
        case kVK_F12: [self.owner toggleDebugger:nil]; return;
        case kVK_F5:
            if ([e modifierFlags] & NSEventModifierFlagShift) { [self.owner powerCycle:nil]; return; }
            break;
        default:
            break;
        }
        const uint32_t ks = A800KeysymFromEvent(e);
        const int sa = ks ? a800session_key_sysaction(self.session, ks) : -1;
        if (sa >= 0) { [self.owner runSysaction:sa]; return; }
    }
    if (!A800ForwardKey(self.session, e, YES)) [super keyDown:e];
}

- (void)keyUp:(NSEvent *)e
{
    if (!A800ForwardKey(self.session, e, NO)) [super keyUp:e];
}

- (void)flagsChanged:(NSEvent *)e
{
    A800ForwardFlags(self.session, e);
    [super flagsChanged:e];
}

- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender
{
    (void)sender;
    return NSDragOperationCopy;
}

- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender
{
    NSArray *urls = [[sender draggingPasteboard] readObjectsForClasses:@[[NSURL class]] options:nil];
    if ([urls count] == 0) return NO;
    [self.owner loadMedia:[(NSURL *)urls[0] path]];
    return YES;
}
@end

/* A round light for the status line. */
@interface A800Light : NSView
@property (nonatomic) BOOL on;
@end
@implementation A800Light
- (void)setOn:(BOOL)on
{
    if (on == _on) return;
    _on = on;
    [self setNeedsDisplay:YES];
}
- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    const NSRect b = NSInsetRect([self bounds], 1, 1);
    [(_on ? A800AccentColor() : [NSColor tertiaryLabelColor]) setFill];
    [[NSBezierPath bezierPathWithOvalInRect:b] fill];
}
@end

@implementation A800AppDelegate {
    a800session *_session;
    const char *_mediaPath;
    NSString *_pendingMedia;     /* a file Finder handed over before launch finished */
    BOOL _launched;
    NSWindow *_window;
    A800DisplayView *_display;
    A800ContentView *_content;
    A800Light *_linkLight;
    NSTextField *_statusText;
    NSString *_message;          /* a passing message in the status line */
    NSDate *_messageUntil;
    NSTimer *_statusTimer, *_fastTimer;
    unsigned _padGeneration, _fujinetGeneration;
    BOOL _noticeOpen;
    NSString *_shotPath;
    uint64_t _shotFrames, _quitFrames;

    NSMenu *_drivesMenu, *_cartMenu, *_tapeMenu;
    NSMenuItem *_aspectItem, *_smoothItem, *_warpItem, *_col80Item;

    A800ControllersWindow *_controllers;
    A800DebuggerWindow *_debugger;
    A800SettingsWindow *_settings;

    NSWindow *_logWindow;
    NSTextView *_logView;
    NSTimer *_logTimer;
}

- (instancetype)initWithSession:(a800session *)session mediaPath:(const char *)mediaPath
{
    self = [super init];
    if (!self) return nil;
    _session = session;
    _mediaPath = mediaPath;
    return self;
}

- (NSString *)lastError
{
    const char *e = a800session_last_error(_session);
    return (e && e[0]) ? str(e) : @"The Atari is not running.";
}

- (void)alert:(NSString *)title text:(NSString *)text
{
    NSAlert *a = [[NSAlert alloc] init];
    [a setMessageText:title];
    if (text) [a setInformativeText:text];
    [a runModal];
}

/* A passing message in the status line. */
- (void)say:(NSString *)message
{
    _message = message;
    _messageUntil = [NSDate dateWithTimeIntervalSinceNow:MESSAGE_SECONDS];
    [self updateStatus];
}

/* ---- launch ------------------------------------------------------------------- */

- (void)applicationDidFinishLaunching:(NSNotification *)note
{
    (void)note;

    /* 336 x 240 at three times, a TV's shape */
    const NSRect frame = NSMakeRect(0, 0, 864, 720 + STATUS_HEIGHT);
    _window = [[NSWindow alloc]
        initWithContentRect:frame
                  styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                             NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable)
                    backing:NSBackingStoreBuffered
                      defer:NO];
    [_window setTitle:APP_TITLE];
    [_window setCollectionBehavior:NSWindowCollectionBehaviorFullScreenPrimary];
    [_window setContentMinSize:NSMakeSize(320, 240 + STATUS_HEIGHT)];
    [_window center];

    _content = [[A800ContentView alloc] initWithFrame:frame];
    _content.session = _session;
    _content.owner = self;
    [_content setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    [_content registerForDraggedTypes:@[NSPasteboardTypeFileURL]];

    _display = [[A800DisplayView alloc] initWithSession:_session];
    [_display setFrame:NSMakeRect(0, STATUS_HEIGHT, frame.size.width, frame.size.height - STATUS_HEIGHT)];
    [_display setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    [_content addSubview:_display];

    /* the status line: the link light, the words */
    _linkLight = [[A800Light alloc] initWithFrame:NSMakeRect(8, 7, 10, 10)];
    _linkLight.toolTip = @"FujiNet link";
    [_content addSubview:_linkLight];
    _statusText = [NSTextField labelWithString:@""];
    _statusText.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
    _statusText.lineBreakMode = NSLineBreakByTruncatingTail;
    _statusText.frame = NSMakeRect(26, 4, frame.size.width - 34, 16);
    _statusText.autoresizingMask = NSViewWidthSizable;
    [_content addSubview:_statusText];

    [_window setContentView:_content];
    [self buildMenu];
    [self applyView];
    [_window makeKeyAndOrderFront:nil];
    [_window makeFirstResponder:_content];
    [_window setDelegate:self];

    _controllers = [[A800ControllersWindow alloc] initWithSession:_session];
    _debugger = [[A800DebuggerWindow alloc] initWithSession:_session];

    /* FujiNet first, then the machine (the built-in OS needs no ROMs) */
    [self start];
    _launched = YES;

    /* A file from the command line or from Finder goes where Open puts it. */
    NSString *media = _pendingMedia;
    if (!media && _mediaPath)
        media = [[NSFileManager defaultManager] stringWithFileSystemRepresentation:_mediaPath
                                                                            length:strlen(_mediaPath)];
    _pendingMedia = nil;
    if (media) [self loadMedia:media];

    /* the current pads and FujiNet state are the baseline: no messages */
    _padGeneration = a800session_gamepad_generation(_session);
    _fujinetGeneration = a800session_fujinet_generation(_session);

    /* The family's launch hooks: A800_OPEN_CONTROLLERS / _DEBUGGER /
     * _SETTINGS; A800_SCREENSHOT=file.png after A800_SCREENSHOT_FRAMES
     * (300); A800_AUTOQUIT_FRAMES. */
    if (getenv("A800_OPEN_CONTROLLERS")) [self toggleControllers:nil];
    if (getenv("A800_OPEN_DEBUGGER")) [_debugger show];
    if (getenv("A800_OPEN_SETTINGS")) [self showSettings:nil];
    if (getenv("A800_SCREENSHOT") && *getenv("A800_SCREENSHOT")) {
        _shotPath = str(getenv("A800_SCREENSHOT"));
        _shotFrames = getenv("A800_SCREENSHOT_FRAMES") ? strtoull(getenv("A800_SCREENSHOT_FRAMES"), NULL, 10) : 300;
    }
    if (getenv("A800_AUTOQUIT_FRAMES")) _quitFrames = strtoull(getenv("A800_AUTOQUIT_FRAMES"), NULL, 10);

    _statusTimer = [NSTimer scheduledTimerWithTimeInterval:1.0 repeats:YES
                                                     block:^(NSTimer *t) { (void)t; [self updateStatus]; }];
    _fastTimer = [NSTimer scheduledTimerWithTimeInterval:0.1 repeats:YES
                                                   block:^(NSTimer *t) { (void)t; [self fastTick]; }];
    [self updateStatus];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app
{
    (void)app;
    return YES;
}

- (void)applicationWillTerminate:(NSNotification *)note
{
    (void)note;
    [_statusTimer invalidate];
    [_fastTimer invalidate];
    [_logTimer invalidate];
    [_display stop];
    a800session_stop(_session);
    a800session_settings_flush(_session);
    a800session_free(_session);
    _session = NULL;
}

- (BOOL)application:(NSApplication *)app openFile:(NSString *)filename
{
    (void)app;
    /* Opening a file from Finder launches the app and delivers the file
     * before applicationDidFinishLaunching:, when there is no machine yet
     * to put it in: hold it until there is. */
    if (!_launched) {
        _pendingMedia = filename;
        return YES;
    }
    [self loadMedia:filename];
    return YES;
}

- (void)windowDidResignKey:(NSNotification *)note
{
    /* focus gone: nothing stays held (the Controllers panel takes typing too) */
    if (note.object == _window && ![_controllers isVisible])
        a800session_release_all(_session);
}

/* ---- the machine ---------------------------------------------------------------- */

- (void)start
{
    if (a800session_is_running(_session)) return;
    a800session_start_opts opts;
    a800session_default_opts(_session, &opts);
    if (a800session_start(_session, &opts) != 0) {
        NSString *why = [self lastError];
        /* after launch finishes: a modal alert inside it stalls the menu bar */
        dispatch_async(dispatch_get_main_queue(), ^{ [self alert:@"Cannot Start the Atari" text:why]; });
    }
    [self updateStatus];
}

- (void)powerCycle:(id)sender
{
    (void)sender;
    if (!a800session_is_running(_session)) {
        [self start];
        return;
    }
    if (a800session_power_cycle(_session) != 0)
        [self alert:@"Cannot Power Cycle" text:[self lastError]];
    [self updateStatus];
}

- (void)resetMachine:(id)sender
{
    (void)sender;
    a800session_reset(_session);
}

- (void)rebootConfig:(id)sender
{
    (void)sender;
    if (a800session_reboot_to_config(_session) == 0) [self say:@"Back to FujiNet's CONFIG"];
}

- (void)toggleWarp:(id)sender
{
    (void)sender;
    a800session_set_warp(_session, !a800session_warp(_session));
    [self updateStatus];
}

- (void)runSysaction:(int)sa
{
    switch (sa) {
    case A800_SYSACT_PAUSE: [_debugger show]; break;
    case A800_SYSACT_POWER: [self powerCycle:nil]; break;
    case A800_SYSACT_REBOOT_CONFIG: [self rebootConfig:nil]; break;
    case A800_SYSACT_WARP: [self toggleWarp:nil]; break;
    default: break;
    }
}

/* A console key from the menu: held for a tenth of a second, long enough
 * for the OS's once-a-frame look. */
- (void)consoleKey:(NSMenuItem *)sender
{
    const int key = (int)[sender tag];
    a800session_console(_session, key, 1);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.12 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        if (self->_session) a800session_console(self->_session, key, 0);
    });
}

- (void)saveState:(id)sender
{
    (void)sender;
    NSSavePanel *panel = [NSSavePanel savePanel];
    [panel setNameFieldStringValue:@"atari.a8s"];
    NSString *dir = [str(a800session_data_path(_session)) stringByAppendingPathComponent:@"states"];
    [[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
    [panel setDirectoryURL:[NSURL fileURLWithPath:dir]];
    [panel beginSheetModalForWindow:_window completionHandler:^(NSModalResponse r) {
        if (r != NSModalResponseOK) return;
        if (a800session_state_save(self->_session, [[[panel URL] path] fileSystemRepresentation]) != 0)
            [self alert:@"Cannot Save the State" text:[self lastError]];
        else
            [self say:@"State saved"];
    }];
}

- (void)loadState:(id)sender
{
    (void)sender;
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    NSString *dir = [str(a800session_data_path(_session)) stringByAppendingPathComponent:@"states"];
    [panel setDirectoryURL:[NSURL fileURLWithPath:dir]];
    [panel beginSheetModalForWindow:_window completionHandler:^(NSModalResponse r) {
        if (r != NSModalResponseOK) return;
        if (a800session_state_load(self->_session, [[[panel URL] path] fileSystemRepresentation]) != 0)
            [self alert:@"Cannot Load the State" text:[self lastError]];
        else
            [self say:@"State loaded"];
    }];
}

/* ---- picking from a list: a cartridge's type, a ROM's slot ------------------------ */

/* A modal alert with a pop-up of names: the index chosen, or -1. */
- (NSInteger)pick:(NSString *)title text:(NSString *)text names:(NSArray<NSString *> *)names
{
    NSAlert *a = [[NSAlert alloc] init];
    [a setMessageText:title];
    [a setInformativeText:text];
    NSPopUpButton *pop = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(0, 0, 320, 26) pullsDown:NO];
    [pop addItemsWithTitles:names];
    [a setAccessoryView:pop];
    [a addButtonWithTitle:@"OK"];
    [a addButtonWithTitle:@"Cancel"];
    if ([a runModal] != NSAlertFirstButtonReturn) return -1;
    return [pop indexOfSelectedItem];
}

/* A raw image: the user says which board it is. */
- (void)askCartType:(NSString *)path piggyback:(BOOL)piggyback
{
    const char *p = [path fileSystemRepresentation];
    const int kb = a800session_cart_needs_type(p);
    int types[64];
    const int n = a800session_cart_types_for(kb, types, 64);
    if (n <= 0) {
        [self alert:@"Cannot Use the Cartridge" text:@"No Atari computer cartridge is that size."];
        return;
    }
    NSMutableArray<NSString *> *names = [NSMutableArray array];
    for (int i = 0; i < n; i++) [names addObject:str(a800session_cart_type_name(types[i]))];
    const NSInteger c = [self pick:@"Which Cartridge Is It?"
                              text:[NSString stringWithFormat:@"%@ is a raw %d KB image: the file does not "
                                    @"say which cartridge board it came from. A .car file would.",
                                    [path lastPathComponent], kb]
                             names:names];
    if (c < 0) return;
    char msg[512];
    const int rc = piggyback ? a800session_cart_insert_piggyback(_session, p, types[c], msg, sizeof msg)
                             : a800session_cart_insert(_session, p, types[c], msg, sizeof msg);
    if (rc == 0) [self say:msg[0] ? str(msg) : @"Cartridge inserted"];
    else [self alert:@"Cannot Use the Cartridge" text:str(msg)];
}

/* ---- opening things -------------------------------------------------------------- */

- (void)rememberFolder:(NSString *)path
{
    a800session_set_str(_session, "last_dir", [[path stringByDeletingLastPathComponent] fileSystemRepresentation]);
}

- (NSURL *)mediaFolder
{
    NSString *last = str(a800session_get_str(_session, "last_dir", ""));
    BOOL dir = NO;
    if ([last length] && [[NSFileManager defaultManager] fileExistsAtPath:last isDirectory:&dir] && dir)
        return [NSURL fileURLWithPath:last];
    return [NSURL fileURLWithPath:str(a800session_media_path(_session))];
}

/* Open, a drop, Finder: a disk boots, a program runs, a cartridge goes in,
 * a tape or a state loads, a ROM is imported. */
- (void)loadMedia:(NSString *)path
{
    if (!a800session_is_running(_session)) {
        [self say:@"The Atari is off"];
        return;
    }
    char msg[1024];
    const int rc = a800session_open(_session, [path fileSystemRepresentation], msg, sizeof msg);
    if (rc == 1) {
        [self askCartType:path piggyback:NO];
        return;
    }
    if (rc != 0) {
        [self alert:@"Cannot Open It" text:msg[0] ? str(msg) : [self lastError]];
        return;
    }
    [self rememberFolder:path];
    [self say:msg[0] ? str(msg) : [path lastPathComponent]];
}

- (void)openMedia:(id)sender
{
    (void)sender;
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    [panel setDirectoryURL:[self mediaFolder]];
    [panel beginSheetModalForWindow:_window completionHandler:^(NSModalResponse r) {
        if (r == NSModalResponseOK) [self loadMedia:[[panel URL] path]];
    }];
}

- (void)importRoms:(id)sender
{
    (void)sender;
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    [panel setAllowsMultipleSelection:YES];
    [panel setMessage:@"ROM images and MAME sets"];
    [panel beginSheetModalForWindow:_window completionHandler:^(NSModalResponse r) {
        if (r != NSModalResponseOK) return;
        /* after the sheet: the slot question is an alert of its own */
        NSArray<NSURL *> *urls = [panel URLs];
        dispatch_async(dispatch_get_main_queue(), ^{ [self importRomFiles:urls]; });
    }];
}

- (void)importRomFiles:(NSArray<NSURL *> *)urls
{
    NSMutableArray<NSString *> *log = [NSMutableArray array];
    int imported = 0, unknown = 0;
    NSString *unknownPath = nil;
    for (NSURL *u in urls) {
        char msg[1024];
        const int n = a800session_rom_import(_session, [[u path] fileSystemRepresentation], -1, msg, sizeof msg);
        if (n > 0) {
            imported += n;
            [log addObject:str(msg)];
        } else {
            unknown++;
            unknownPath = [u path];
        }
    }
    if (unknown == 1 && imported == 0) {
        /* not known by its checksum: a peripheral's ROM fits slots by size */
        int slots[16];
        const int n = a800session_rom_probe(_session, [unknownPath fileSystemRepresentation], slots, 16);
        if (n > 0) {
            NSMutableArray<NSString *> *names = [NSMutableArray array];
            for (int i = 0; i < n; i++) [names addObject:str(a800session_rom_slot(slots[i])->name)];
            const NSInteger c = [self pick:@"Which ROM Is It?"
                                      text:[NSString stringWithFormat:@"%@ is not one of the ROMs this app "
                                            @"knows by its checksum, but it is the size of these. Which is it?",
                                            [unknownPath lastPathComponent]]
                                     names:names];
            if (c < 0) return;
            char msg[512];
            if (a800session_rom_import(_session, [unknownPath fileSystemRepresentation], slots[c], msg, sizeof msg) > 0)
                [self say:[NSString stringWithFormat:@"Imported: %@", str(msg)]];
            else
                [self alert:@"Nothing Imported" text:str(msg)];
            return;
        }
    }
    if (unknown)
        [log addObject:[NSString stringWithFormat:@"%d file%@ not recognised: a peripheral's ROM goes in "
                        @"its slot from Settings ▸ ROMs.", unknown, unknown == 1 ? @" was" : @"s were"]];
    [self alert:imported ? @"ROMs Imported" : @"Nothing Imported" text:[log componentsJoinedByString:@"\n"]];
}

/* ---- drives, cartridges, tape ----------------------------------------------------- */

- (void)insert:(NSString *)path into:(int)drive
{
    char msg[512];
    if (a800session_drive_insert(_session, drive, [path fileSystemRepresentation], 0, msg, sizeof msg) != 0) {
        [self alert:@"Cannot Insert the Disk" text:str(msg)];
        return;
    }
    [self rememberFolder:path];
    [self say:msg[0] ? str(msg) : [NSString stringWithFormat:@"%@ is in D%d:", [path lastPathComponent], drive + 1]];
}

- (void)driveAction:(NSMenuItem *)sender
{
    const int drive = (int)[sender tag] / 8, action = (int)[sender tag] % 8;
    const BOOL fuji = a800session_fujinet_running(_session) != 0;
    if (action == DRIVE_INSERT) {
        NSOpenPanel *panel = [NSOpenPanel openPanel];
        [panel setMessage:[NSString stringWithFormat:@"%@ D%d:", fuji ? @"Mount in" : @"Insert in", drive + 1]];
        [panel setDirectoryURL:fuji ? [NSURL fileURLWithPath:str(a800session_sd_path(_session))] : [self mediaFolder]];
        [panel beginSheetModalForWindow:_window completionHandler:^(NSModalResponse r) {
            if (r == NSModalResponseOK) [self insert:[[panel URL] path] into:drive];
        }];
    } else if (action == DRIVE_EJECT) {
        a800session_drive_eject(_session, drive);
    } else if (action == DRIVE_WP) {
        a800_drive_info di;
        if (a800session_drive_info(_session, drive, &di) == 0 && di.path[0])
            a800session_drive_set_read_only(_session, drive, !di.read_only);
    } else if (action >= DRIVE_BLANK) {
        const int density = action - DRIVE_BLANK;
        NSSavePanel *panel = [NSSavePanel savePanel];
        [panel setNameFieldStringValue:@"blank.atr"];
        [panel setDirectoryURL:[self mediaFolder]];
        [panel beginSheetModalForWindow:_window completionHandler:^(NSModalResponse r) {
            if (r != NSModalResponseOK) return;
            NSString *path = [[panel URL] path];
            if (a800session_new_blank_disk([path fileSystemRepresentation], density) != 0)
                [self alert:@"Cannot Create the Disk" text:path];
            else
                [self insert:path into:drive];
        }];
    }
}

- (void)importToSD:(id)sender
{
    (void)sender;
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    [panel setAllowsMultipleSelection:YES];
    [panel setDirectoryURL:[self mediaFolder]];
    [panel beginSheetModalForWindow:_window completionHandler:^(NSModalResponse r) {
        if (r != NSModalResponseOK) return;
        int ok = 0, failed = 0;
        for (NSURL *u in [panel URLs]) {
            char dest[1024];
            if (a800session_import_to_sd(self->_session, [[u path] fileSystemRepresentation], dest, sizeof dest) == 0)
                ok++;
            else
                failed++;
        }
        [self say:failed ? [NSString stringWithFormat:@"%d copied to FujiNet's SD card, %d could not be", ok, failed]
                         : [NSString stringWithFormat:@"%d copied to FujiNet's SD card: mount from CONFIG or Drives", ok]];
    }];
}

- (void)openFolder:(NSString *)path
{
    [[NSFileManager defaultManager] createDirectoryAtPath:path withIntermediateDirectories:YES attributes:nil error:nil];
    [[NSWorkspace sharedWorkspace] openURL:[NSURL fileURLWithPath:path]];
}

- (void)openSDFolder:(id)sender { (void)sender; [self openFolder:str(a800session_sd_path(_session))]; }
- (void)openMediaFolder:(id)sender { (void)sender; [self openFolder:str(a800session_media_path(_session))]; }

- (void)insertCart:(NSMenuItem *)sender
{
    const BOOL piggyback = [sender tag] != 0;
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    [panel setMessage:piggyback ? @"Insert a Piggyback Cartridge" : @"Insert a Cartridge"];
    [panel setDirectoryURL:[self mediaFolder]];
    [panel beginSheetModalForWindow:_window completionHandler:^(NSModalResponse r) {
        if (r != NSModalResponseOK) return;
        NSString *path = [[panel URL] path];
        char msg[512];
        const char *p = [path fileSystemRepresentation];
        const int rc = piggyback ? a800session_cart_insert_piggyback(self->_session, p, 0, msg, sizeof msg)
                                 : a800session_cart_insert(self->_session, p, 0, msg, sizeof msg);
        [self rememberFolder:path];
        if (rc > 0)
            dispatch_async(dispatch_get_main_queue(), ^{ [self askCartType:path piggyback:piggyback]; });
        else if (rc < 0)
            [self alert:@"Cannot Use the Cartridge" text:str(msg)];
        else
            [self say:msg[0] ? str(msg) : @"Cartridge inserted"];
    }];
}

- (void)removeCart:(NSMenuItem *)sender
{
    if ([sender tag]) a800session_cart_remove_piggyback(_session);
    else a800session_cart_remove(_session);
}

- (void)loadTape:(id)sender
{
    (void)sender;
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    [panel setDirectoryURL:[self mediaFolder]];
    [panel beginSheetModalForWindow:_window completionHandler:^(NSModalResponse r) {
        if (r != NSModalResponseOK) return;
        char msg[512];
        NSString *path = [[panel URL] path];
        if (a800session_tape_insert(self->_session, [path fileSystemRepresentation], msg, sizeof msg) != 0) {
            [self alert:@"Cannot Load the Tape" text:str(msg)];
            return;
        }
        [self rememberFolder:path];
        [self say:msg[0] ? str(msg) : @"Tape loaded: Boot Tape, or CLOAD / RUN \"C:\""];
    }];
}

- (void)recordTape:(id)sender
{
    (void)sender;
    NSSavePanel *panel = [NSSavePanel savePanel];
    [panel setNameFieldStringValue:@"tape.cas"];
    [panel setDirectoryURL:[self mediaFolder]];
    [panel beginSheetModalForWindow:_window completionHandler:^(NSModalResponse r) {
        if (r != NSModalResponseOK) return;
        if (a800session_tape_record_new(self->_session, [[[panel URL] path] fileSystemRepresentation]) != 0)
            [self alert:@"Cannot Record" text:[self lastError]];
        else
            [self say:@"Recording: CSAVE or LIST \"C:\" writes to the tape"];
    }];
}

- (void)tapeAction:(NSMenuItem *)sender
{
    switch ([sender tag]) {
    case 0: a800session_tape_rewind(_session); break;
    case 1: a800session_tape_boot(_session); break;
    default: a800session_tape_eject(_session); break;
    }
}

/* ---- text in and out ---------------------------------------------------------------- */

- (void)copyScreenText:(id)sender
{
    (void)sender;
    static char buf[8192];
    if (a800session_copy_screen_text(_session, buf, sizeof buf) <= 0) {
        [self say:@"No text screen to copy"];
        return;
    }
    NSPasteboard *pb = [NSPasteboard generalPasteboard];
    [pb clearContents];
    [pb setString:str(buf) forType:NSPasteboardTypeString];
    [self say:@"The screen's text is on the clipboard"];
}

- (void)pasteToAtari:(id)sender
{
    (void)sender;
    NSString *t = [[NSPasteboard generalPasteboard] stringForType:NSPasteboardTypeString];
    if ([t length]) a800session_paste(_session, [t UTF8String]);
}

/* ---- the view ------------------------------------------------------------------------- */

- (void)applyView
{
    const int i = a800session_pref_find("aspect");
    const BOOL tv = i < 0 || a800session_pref_get_choice(_session, i) == 0;
    const BOOL smooth = a800session_get_int(_session, "smooth", 0) != 0;
    [_display setTvAspect:tv];
    [_display setSmooth:smooth];
    [_aspectItem setState:tv ? NSControlStateValueOn : NSControlStateValueOff];
    [_smoothItem setState:smooth ? NSControlStateValueOn : NSControlStateValueOff];
}

- (void)toggleAspect:(id)sender
{
    (void)sender;
    const int i = a800session_pref_find("aspect");
    if (i >= 0) a800session_pref_set_choice(_session, i, a800session_pref_get_choice(_session, i) == 0 ? 1 : 0);
    [self applyView];
}

- (void)toggleSmooth:(id)sender
{
    (void)sender;
    a800session_set_int(_session, "smooth", !a800session_get_int(_session, "smooth", 0));
    [self applyView];
}

- (void)toggle80col:(id)sender
{
    (void)sender;
    a800session_set_80col_display(_session, !a800session_80col_display(_session));
}

- (void)toggleFullScreen:(id)sender
{
    [_window toggleFullScreen:sender];
}

- (void)toggleControllers:(id)sender
{
    (void)sender;
    [_controllers toggle];
}

- (void)toggleDebugger:(id)sender
{
    (void)sender;
    [_debugger toggle];
}

- (void)showSettings:(id)sender
{
    (void)sender;
    if (!_settings) {
        __weak A800AppDelegate *weakSelf = self;
        _settings = [[A800SettingsWindow alloc] initWithSession:_session
            onLive:^{ [weakSelf applyView]; }
            onClosed:^(int applied) {
                A800AppDelegate *me = weakSelf;
                if (!me) return;
                [me applyView];
                [me->_controllers refresh];
                if (applied == A800_APPLY_POWER) [me say:@"Power cycled with the new configuration"];
                else if (applied == A800_APPLY_RESTART) [me say:@"Restarted with the new options"];
                [me updateStatus];
            }];
    }
    [_settings show];
}

/* ---- status, and news from the other threads ------------------------------------------ */

- (void)updateStatus
{
    if (!_session) return;
    NSString *text;
    if (_message && [_messageUntil timeIntervalSinceNow] > 0) {
        text = _message;
    } else {
        _message = nil;
        if (a800session_is_running(_session)) {
            char buf[512];
            a800session_status(_session, buf, sizeof buf);
            text = str(buf);
        } else {
            text = @"Off";
        }
    }
    [_statusText setStringValue:text];
    [_linkLight setOn:a800session_is_running(_session) && a800session_fujinet_link(_session)];
}

- (void)showNotice:(NSString *)text
{
    _noticeOpen = YES;
    NSAlert *a = [[NSAlert alloc] init];
    [a setMessageText:@"The Atari Stopped"];
    [a setInformativeText:text];
    [a addButtonWithTitle:@"Power Cycle"];
    [a addButtonWithTitle:@"Open the Debugger"];
    [a addButtonWithTitle:@"Leave It"];
    const NSModalResponse r = [a runModal];
    _noticeOpen = NO;
    if (r == NSAlertFirstButtonReturn) [self powerCycle:nil];
    else if (r == NSAlertSecondButtonReturn) [_debugger show];
}

/* The gamepad thread cannot touch AppKit; it posts system actions and this
 * timer takes them. It also says when gamepads come and go, when FujiNet
 * restarted itself, and when the Atari crashed with no debugger open. */
- (void)fastTick
{
    if (!_session) return;
    char text[512];
    int sa;
    while (a800session_sysaction_take(_session, &sa)) [self runSysaction:sa];
    unsigned gen = a800session_gamepad_generation(_session);
    if (gen != _padGeneration) {
        _padGeneration = gen;
        if (a800session_gamepad_last_event(_session, text, sizeof text) > 0) [self say:str(text)];
        [_controllers refresh];
    }
    gen = a800session_fujinet_generation(_session);
    if (gen != _fujinetGeneration) {
        _fujinetGeneration = gen;
        if (a800session_fujinet_last_event(_session, text, sizeof text) > 0) [self say:str(text)];
    }
    if (!_noticeOpen && a800session_take_notice(_session, text, sizeof text) > 0) {
        NSString *t = str(text);
        dispatch_async(dispatch_get_main_queue(), ^{ [self showNotice:t]; });
    }
    if (_shotPath && [_display frames] >= _shotFrames) {
        if (![_display savePng:_shotPath]) NSLog(@"A800_SCREENSHOT: could not write %@", _shotPath);
        _shotPath = nil;
    }
    if (_quitFrames && [_display frames] >= _quitFrames) {
        _quitFrames = 0;
        [NSApp terminate:nil];
    }
}

/* ---- FujiNet ---------------------------------------------------------------------------- */

- (void)openWebUI:(id)sender
{
    (void)sender;
    if (!a800session_fujinet_running(_session)) {
        [self say:@"FujiNet is off (Settings ▸ FujiNet)"];
        return;
    }
    [[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:str(a800session_fujinet_webui_url(_session))]];
}

- (void)restartFujiNet:(id)sender
{
    (void)sender;
    if (a800session_fujinet_restart(_session) != 0)
        [self alert:@"Cannot Restart FujiNet" text:[self lastError]];
    else
        [self say:@"FujiNet restarted"];
}

- (void)refreshLog:(NSTimer *)timer
{
    (void)timer;
    static char buf[128 * 1024];
    const int n = a800session_fujinet_copy_log(_session, buf, sizeof buf);
    NSScrollView *scroll = (NSScrollView *)_logView.enclosingScrollView;
    const BOOL atEnd = !scroll || (NSMaxY(scroll.contentView.documentVisibleRect) >=
                                   NSMaxY(((NSView *)scroll.documentView).frame) - 4.0);
    NSString *text = n > 0 ? [NSString stringWithUTF8String:buf] : nil;
    [_logView setString:(text ?: @"(no FujiNet output yet)")];
    if (atEnd) [_logView scrollRangeToVisible:NSMakeRange(_logView.string.length, 0)];
}

- (void)showFujiNetLog:(id)sender
{
    (void)sender;
    if (!_logWindow) {
        NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 860, 600)];
        scroll.hasVerticalScroller = YES;
        scroll.autohidesScrollers = NO;
        _logView = [[NSTextView alloc] initWithFrame:scroll.bounds];
        _logView.editable = NO;
        _logView.richText = NO;
        _logView.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
        _logView.autoresizingMask = NSViewWidthSizable;
        scroll.documentView = _logView;
        _logWindow = [[NSWindow alloc]
            initWithContentRect:NSMakeRect(0, 0, 860, 600)
                      styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                        backing:NSBackingStoreBuffered
                          defer:NO];
        _logWindow.title = @"FujiNet Console Log";
        _logWindow.releasedWhenClosed = NO;
        _logWindow.delegate = self;
        _logWindow.contentView = scroll;
        [_logWindow center];
    }
    if (!_logTimer)
        _logTimer = [NSTimer scheduledTimerWithTimeInterval:1.0 target:self
                                                   selector:@selector(refreshLog:) userInfo:nil repeats:YES];
    [self refreshLog:nil];
    [_logWindow makeKeyAndOrderFront:nil];
}

- (void)windowWillClose:(NSNotification *)note
{
    if (note.object == _logWindow) {
        [_logTimer invalidate];
        _logTimer = nil;
    }
}

- (void)showAbout:(id)sender
{
    (void)sender;
    NSAlert *a = [[NSAlert alloc] init];
    [a setMessageText:[NSString stringWithFormat:@"%@ %s", APP_TITLE, A800_VERSION_STRING]];
    [a setInformativeText:
        @"The Atari 400, 800, XL and XE computers with a FujiNet on the SIO port. The emulator is "
        @"atari800 (GPL-2.0-or-later, the Atari800 Development Team), with the FujiNet firmware "
        @"(GPL-3.0-or-later) connected over NetSIO. AltirraOS and Altirra BASIC (Avery Lee) are "
        @"built in; Atari's own ROMs can be imported.\n\n"
        @"Copyright © 2026 Thomas Cherryhomes — GPL-3.0-or-later\nhttps://fujinet.online/"];
    [a runModal];
}

/* ---- the menu bar ------------------------------------------------------------------------- */

- (NSMenuItem *)item:(NSMenu *)menu title:(NSString *)title action:(SEL)sel key:(NSString *)key
{
    NSMenuItem *it = [menu addItemWithTitle:title action:sel keyEquivalent:key];
    [it setTarget:self];
    return it;
}

- (NSMenu *)addMenu:(NSString *)title to:(NSMenu *)bar
{
    NSMenuItem *top = [[NSMenuItem alloc] init];
    NSMenu *m = [[NSMenu alloc] initWithTitle:title];
    [top setSubmenu:m];
    [bar addItem:top];
    return m;
}

- (void)buildMenu
{
    NSMenu *bar = [[NSMenu alloc] init];

    NSMenu *appMenu = [self addMenu:APP_TITLE to:bar];
    [self item:appMenu title:@"About " APP_TITLE action:@selector(showAbout:) key:@""];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [self item:appMenu title:@"Settings…" action:@selector(showSettings:) key:@","];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [appMenu addItemWithTitle:@"Hide " APP_TITLE action:@selector(hide:) keyEquivalent:@"h"];
    NSMenuItem *others = [appMenu addItemWithTitle:@"Hide Others"
                                            action:@selector(hideOtherApplications:) keyEquivalent:@"h"];
    [others setKeyEquivalentModifierMask:NSEventModifierFlagOption | NSEventModifierFlagCommand];
    [appMenu addItemWithTitle:@"Show All" action:@selector(unhideAllApplications:) keyEquivalent:@""];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [appMenu addItemWithTitle:@"Quit " APP_TITLE action:@selector(terminate:) keyEquivalent:@"q"];

    NSMenu *machine = [self addMenu:@"Machine" to:bar];
    [self item:machine title:@"Open…" action:@selector(openMedia:) key:@"o"];
    [machine addItem:[NSMenuItem separatorItem]];
    NSMenuItem *pc = [self item:machine title:@"Power Cycle (Shift+F5)" action:@selector(powerCycle:) key:@"r"];
    [pc setKeyEquivalentModifierMask:NSEventModifierFlagShift | NSEventModifierFlagCommand];
    [self item:machine title:@"Reset (F5)" action:@selector(resetMachine:) key:@"r"];
    [self item:machine title:@"Reboot to CONFIG" action:@selector(rebootConfig:) key:@""];
    _warpItem = [self item:machine title:@"Warp Speed" action:@selector(toggleWarp:) key:@""];
    [machine addItem:[NSMenuItem separatorItem]];
    [self item:machine title:@"Save State…" action:@selector(saveState:) key:@""];
    [self item:machine title:@"Load State…" action:@selector(loadState:) key:@""];
    [self item:machine title:@"Import System ROMs…" action:@selector(importRoms:) key:@""];

    _drivesMenu = [self addMenu:@"Drives" to:bar];
    _drivesMenu.delegate = self;
    [self menuNeedsUpdate:_drivesMenu];
    _cartMenu = [self addMenu:@"Cartridge" to:bar];
    _cartMenu.delegate = self;
    [self menuNeedsUpdate:_cartMenu];
    _tapeMenu = [self addMenu:@"Program Recorder" to:bar];
    _tapeMenu.delegate = self;
    [self menuNeedsUpdate:_tapeMenu];

    NSMenu *console = [self addMenu:@"Console" to:bar];
    NSArray<NSString *> *titles = @[ @"Option (F2)", @"Select (F3)", @"Start (F4)", @"Help (F6)",
                                     @"Break (F7, F13)" ];
    const int keys[] = { A800_CON_OPTION, A800_CON_SELECT, A800_CON_START, A800_CON_HELP, A800_CON_BREAK };
    for (NSUInteger i = 0; i < [titles count]; i++)
        [[self item:console title:titles[i] action:@selector(consoleKey:) key:@""] setTag:keys[i]];

    NSMenu *edit = [self addMenu:@"Edit" to:bar];
    [self item:edit title:@"Copy Screen Text" action:@selector(copyScreenText:) key:@"c"];
    [self item:edit title:@"Paste" action:@selector(pasteToAtari:) key:@"v"];

    NSMenu *view = [self addMenu:@"View" to:bar];
    /* F9, F11 and F12 are the main window's (see A800ContentView): a menu
     * equivalent would take them from the Controllers and debugger windows. */
    [self item:view title:@"Controllers (F9)" action:@selector(toggleControllers:) key:@"k"];
    [self item:view title:@"Debugger (F12)" action:@selector(toggleDebugger:) key:@"d"];
    [view addItem:[NSMenuItem separatorItem]];
    _col80Item = [self item:view title:@"80-Column Display" action:@selector(toggle80col:) key:@""];
    _aspectItem = [self item:view title:@"TV Aspect" action:@selector(toggleAspect:) key:@""];
    _smoothItem = [self item:view title:@"Smooth Scaling" action:@selector(toggleSmooth:) key:@""];
    NSMenuItem *fs = [self item:view title:@"Enter Full Screen" action:@selector(toggleFullScreen:) key:@"f"];
    [fs setKeyEquivalentModifierMask:NSEventModifierFlagControl | NSEventModifierFlagCommand];

    NSMenu *fuji = [self addMenu:@"FujiNet" to:bar];
    [self item:fuji title:@"Web UI" action:@selector(openWebUI:) key:@""];
    [self item:fuji title:@"Console Log" action:@selector(showFujiNetLog:) key:@""];
    [self item:fuji title:@"Restart FujiNet" action:@selector(restartFujiNet:) key:@""];

    /* Window and Help: AppKit fills these in once registered as such. */
    NSMenu *windows = [self addMenu:@"Window" to:bar];
    [windows addItemWithTitle:@"Minimize" action:@selector(performMiniaturize:) keyEquivalent:@"m"];
    [windows addItemWithTitle:@"Zoom" action:@selector(performZoom:) keyEquivalent:@""];
    [windows addItem:[NSMenuItem separatorItem]];
    [windows addItemWithTitle:@"Bring All to Front" action:@selector(arrangeInFront:) keyEquivalent:@""];

    NSMenu *help = [self addMenu:@"Help" to:bar];
    [self item:help title:@"About " APP_TITLE action:@selector(showAbout:) key:@""];

    [NSApp setMainMenu:bar];
    [NSApp setWindowsMenu:windows];
    [NSApp setHelpMenu:help];
}

/* The Drives, Cartridge and Program Recorder menus, rebuilt as they open. */
- (void)menuNeedsUpdate:(NSMenu *)menu
{
    if (!_session) return;
    if (menu == _drivesMenu) {
        const BOOL fuji = a800session_fujinet_running(_session) != 0;
        [menu removeAllItems];
        /* a heading (no action: shown, never chosen) */
        [menu addItemWithTitle:fuji ? @"FujiNet's drives" : @"atari800's drives" action:nil keyEquivalent:@""];
        for (int d = 0; d < A800_DRIVES; d++) {
            a800_drive_info di;
            if (a800session_drive_info(_session, d, &di) != 0) memset(&di, 0, sizeof di);
            NSMenu *sub = [[NSMenu alloc] initWithTitle:[NSString stringWithFormat:@"D%d:", d + 1]];
            [[self item:sub title:fuji ? @"Mount from the SD Card…" : @"Insert…"
                 action:@selector(driveAction:) key:@""] setTag:DRIVE_TAG(d, DRIVE_INSERT)];
            NSMenu *blank = [[NSMenu alloc] initWithTitle:@"New Blank Disk"];
            [[self item:blank title:@"Single Density (90K)…" action:@selector(driveAction:) key:@""]
                setTag:DRIVE_TAG(d, DRIVE_BLANK + 0)];
            [[self item:blank title:@"Enhanced Density (130K)…" action:@selector(driveAction:) key:@""]
                setTag:DRIVE_TAG(d, DRIVE_BLANK + 1)];
            [[self item:blank title:@"Double Density (180K)…" action:@selector(driveAction:) key:@""]
                setTag:DRIVE_TAG(d, DRIVE_BLANK + 2)];
            [[sub addItemWithTitle:@"New Blank Disk" action:nil keyEquivalent:@""] setSubmenu:blank];
            if (di.path[0]) {
                [[self item:sub title:@"Eject" action:@selector(driveAction:) key:@""] setTag:DRIVE_TAG(d, DRIVE_EJECT)];
                if (!fuji) {
                    NSMenuItem *wp = [self item:sub title:@"Write Protect" action:@selector(driveAction:) key:@""];
                    [wp setTag:DRIVE_TAG(d, DRIVE_WP)];
                    [wp setState:di.read_only ? NSControlStateValueOn : NSControlStateValueOff];
                }
            }
            NSString *title = [NSString stringWithFormat:@"D%d: %@", d + 1,
                               di.path[0] ? [str(di.path) lastPathComponent] : @"empty"];
            [[menu addItemWithTitle:title action:nil keyEquivalent:@""] setSubmenu:sub];
        }
        [menu addItem:[NSMenuItem separatorItem]];
        if (fuji) {
            [self item:menu title:@"Import to SD Card…" action:@selector(importToSD:) key:@""];
            [self item:menu title:@"Open SD Folder" action:@selector(openSDFolder:) key:@""];
        } else {
            [self item:menu title:@"Open Media Folder" action:@selector(openMediaFolder:) key:@""];
        }
    } else if (menu == _cartMenu) {
        char info[256];
        [menu removeAllItems];
        if (a800session_cart_info(_session, info, sizeof info) > 0)
            [menu addItemWithTitle:[NSString stringWithFormat:@"In it: %@", str(info)] action:nil keyEquivalent:@""];
        [[self item:menu title:@"Insert Cartridge…" action:@selector(insertCart:) key:@""] setTag:0];
        [[self item:menu title:@"Insert Piggyback…" action:@selector(insertCart:) key:@""] setTag:1];
        [[self item:menu title:@"Remove Cartridge" action:@selector(removeCart:) key:@""] setTag:0];
        [[self item:menu title:@"Remove Piggyback" action:@selector(removeCart:) key:@""] setTag:1];
    } else if (menu == _tapeMenu) {
        int block = 0, blocks = 0;
        [menu removeAllItems];
        if (a800session_tape_position(_session, &block, &blocks) == 0 && blocks > 0)
            [menu addItemWithTitle:[NSString stringWithFormat:@"Block %d of %d", block, blocks]
                            action:nil keyEquivalent:@""];
        [self item:menu title:@"Load Tape…" action:@selector(loadTape:) key:@""];
        [self item:menu title:@"Record New Tape…" action:@selector(recordTape:) key:@""];
        [[self item:menu title:@"Rewind" action:@selector(tapeAction:) key:@""] setTag:0];
        [[self item:menu title:@"Boot Tape (Start held at power-on)" action:@selector(tapeAction:) key:@""] setTag:1];
        [[self item:menu title:@"Eject" action:@selector(tapeAction:) key:@""] setTag:2];
    }
}

- (BOOL)validateMenuItem:(NSMenuItem *)item
{
    const SEL a = [item action];
    const BOOL running = _session && a800session_is_running(_session);
    if (a == @selector(toggleWarp:)) {
        [item setState:(running && a800session_warp(_session)) ? NSControlStateValueOn : NSControlStateValueOff];
        return running;
    }
    if (a == @selector(toggle80col:)) {
        [item setState:(_session && a800session_80col_display(_session)) ? NSControlStateValueOn : NSControlStateValueOff];
        return _session && a800session_80col_fitted(_session);
    }
    if (a == @selector(rebootConfig:) || a == @selector(openWebUI:))
        return _session && a800session_fujinet_running(_session);
    if (a == @selector(saveState:) || a == @selector(loadState:))
        return running && a800session_states_available(_session);
    if (a == @selector(resetMachine:) || a == @selector(copyScreenText:) || a == @selector(pasteToAtari:) ||
        a == @selector(openMedia:) || a == @selector(driveAction:) || a == @selector(insertCart:) ||
        a == @selector(consoleKey:) || a == @selector(loadTape:) || a == @selector(recordTape:))
        return running;
    return YES;
}
@end
