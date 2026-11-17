/*
 * A800SettingsWindow -- see the header.
 *
 * A tab per page of a800session_pref, a heading per group, a row per
 * preference, and a ROMs tab with every slot, so the four frontends show
 * the same settings. Rows that apply LIVE (the picture, the sound, the
 * ports) take effect at once; the machine -- model, memory, ROMs,
 * peripherals -- waits and is applied together when the window closes (one
 * power cycle), as are the host options (FujiNet, sound, gamepads: a
 * session restart). Rows follow each other as they change: memory sizes by
 * model, ROM revisions by what has been imported.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "SettingsWindow.h"
#import "help/HelpWindow.h"

#include <string.h>

#define PAGE_W 600.0
#define ROW_H 30.0
#define HEAD_H 30.0
#define LABEL_W 220.0
#define CTRL_X 240.0

/* A page's document view: rows from the top. */
@interface A800FlippedView : NSView
@end
@implementation A800FlippedView
- (BOOL)isFlipped { return YES; }
@end

static NSString *str(const char *s)
{
    return s ? [NSString stringWithUTF8String:s] : @"";
}

@implementation A800SettingsWindow {
    a800session *_session;
    void (^_onLive)(void);
    void (^_onClosed)(int);
    NSWindow *_window;
    NSMutableDictionary<NSNumber *, NSView *> *_controls;  /* by pref */
    NSMutableDictionary<NSNumber *, NSTextField *> *_labels;
    NSMutableDictionary<NSNumber *, NSTextField *> *_paths;
    NSMutableDictionary<NSNumber *, NSString *> *_choiceLabels;
    NSMutableArray<NSTextField *> *_romStates;
    BOOL _syncing;
    int _pending;
    /* the Input page's gamepad block */
    NSButton *_padsBox;
    NSPopUpButton *_padList;
    NSPopUpButton *_padPort;
    NSTimer *_padTimer;
    unsigned _padGeneration;
}

- (instancetype)initWithSession:(a800session *)session
                         onLive:(void (^)(void))onLive
                       onClosed:(void (^)(int applied))onClosed
{
    self = [super init];
    if (!self) return nil;
    _session = session;
    _onLive = onLive;
    _onClosed = onClosed;
    return self;
}

- (NSTextField *)label:(NSString *)text frame:(NSRect)r bold:(BOOL)bold
{
    NSTextField *l = [NSTextField labelWithString:text];
    [l setFrame:r];
    if (bold) [l setFont:[NSFont boldSystemFontOfSize:[NSFont systemFontSize]]];
    [l setLineBreakMode:NSLineBreakByTruncatingTail];
    return l;
}

- (void)changed:(int)i
{
    const a800_pref *p = a800session_pref(i);
    if (p->applies > _pending) _pending = p->applies;
    if (_pending == A800_APPLY_LIVE) {
        a800session_apply_settings(_session);
        if (_onLive) _onLive();
    }
    [self refresh];
}

/* ---- the rows' actions ---------------------------------------------------- */

- (void)boolChanged:(NSButton *)b
{
    if (_syncing) return;
    const int i = (int)[b tag];
    a800session_set_int(_session, a800session_pref(i)->key, [b state] == NSControlStateValueOn);
    [self changed:i];
}

- (void)choiceChanged:(NSPopUpButton *)b
{
    if (_syncing) return;
    const int i = (int)[b tag];
    const int c = (int)[b indexOfSelectedItem];
    if (c < 0) return;
    if (!a800session_pref_choice_enabled(_session, i, c)) {
        /* not imported, or not on this machine: put it back */
        NSAlert *a = [[NSAlert alloc] init];
        [a setMessageText:@"Not Available"];
        [a setInformativeText:@"Import its ROM first (the ROMs tab), or choose a machine that takes it."];
        [a beginSheetModalForWindow:_window completionHandler:nil];
        [self refresh];
        return;
    }
    a800session_pref_set_choice(_session, i, c);
    [self changed:i];
}

- (void)intChanged:(id)sender
{
    if (_syncing) return;
    const int i = (int)[sender tag];
    const a800_pref *p = a800session_pref(i);
    int v = [sender intValue];
    if (v < p->min) v = p->min;
    if (v > p->max) v = p->max;
    if (v == a800session_get_int(_session, p->key, p->def)) return;
    a800session_set_int(_session, p->key, v);
    [self changed:i];
}

/* A text row: valid text is stored and marks the change; anything else is
 * shown in red and not stored, so the last good text stays in force. */
- (void)textChanged:(NSTextField *)f
{
    if (_syncing) return;
    const int i = (int)[f tag];
    NSString *text = [[f stringValue]
        stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
    char err[200];
    if (a800session_pref_set_text(_session, i, [text UTF8String], err, sizeof err) != 0) {
        [f setTextColor:[NSColor systemRedColor]];
        [f setToolTip:str(err)];
        return;
    }
    [f setTextColor:[NSColor controlTextColor]];
    [f setToolTip:str(a800session_pref(i)->help)];
    [self changed:i];
}

- (void)controlTextDidChange:(NSNotification *)note
{
    if ([[note object] isKindOfClass:[NSTextField class]]) [self textChanged:[note object]];
}

- (void)choosePath:(NSButton *)b
{
    const int i = (int)[b tag];
    const a800_pref *p = a800session_pref(i);
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    [panel setMessage:str(p->label)];
    [panel setCanChooseDirectories:p->type == A800_PREF_DIR];
    [panel setCanChooseFiles:p->type != A800_PREF_DIR];
    [panel beginSheetModalForWindow:_window completionHandler:^(NSModalResponse r) {
        if (r != NSModalResponseOK) return;
        a800session_set_str(self->_session, p->key, [[[panel URL] path] fileSystemRepresentation]);
        [self changed:i];
    }];
}

- (void)clearPath:(NSButton *)b
{
    const int i = (int)[b tag];
    a800session_set_str(_session, a800session_pref(i)->key, "");
    [self changed:i];
}

- (void)importRom:(NSButton *)b
{
    const int slot = (int)[b tag];
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    [panel setMessage:[NSString stringWithFormat:@"Import %@", str(a800session_rom_slot(slot)->name)]];
    [panel beginSheetModalForWindow:_window completionHandler:^(NSModalResponse r) {
        if (r != NSModalResponseOK) return;
        char msg[512];
        if (a800session_rom_import(self->_session, [[[panel URL] path] fileSystemRepresentation], slot,
                                   msg, sizeof msg) > 0) {
            /* revisions that need it become choosable */
            if (self->_pending < A800_APPLY_POWER) self->_pending = A800_APPLY_POWER;
        } else {
            NSAlert *a = [[NSAlert alloc] init];
            [a setMessageText:@"Nothing Imported"];
            [a setInformativeText:str(msg)];
            [a runModal];
        }
        [self refresh];
    }];
}

/* ---- everything from the store ---------------------------------------------- */

- (NSString *)choiceLabels:(int)i
{
    NSMutableString *all = [NSMutableString string];
    const char *l;
    for (int c = 0; (l = a800session_pref_choice(_session, i, c)) != NULL; c++) {
        [all appendString:str(l)];
        if (!a800session_pref_choice_enabled(_session, i, c)) [all appendString:@" (not available)"];
        [all appendString:@"\n"];
    }
    return all;
}

- (void)refresh
{
    _syncing = YES;
    for (int i = 0; i < a800session_pref_count(); i++) {
        const a800_pref *p = a800session_pref(i);
        NSNumber *key = @(i);
        NSView *ctrl = _controls[key];
        if (!ctrl) continue;
        const BOOL on = a800session_pref_enabled(_session, i) != 0;
        if ([ctrl isKindOfClass:[NSControl class]]) [(NSControl *)ctrl setEnabled:on];
        [_labels[key] setTextColor:on ? [NSColor labelColor] : [NSColor disabledControlTextColor]];
        switch (p->type) {
        case A800_PREF_BOOL:
            [(NSButton *)ctrl setState:a800session_get_int(_session, p->key, p->def)
                                           ? NSControlStateValueOn : NSControlStateValueOff];
            break;
        case A800_PREF_INT: {
            const int v = a800session_get_int(_session, p->key, p->def);
            [(NSTextField *)ctrl setIntValue:v];
            for (NSView *sib in [[ctrl superview] subviews])
                if ([sib isKindOfClass:[NSStepper class]] && [(NSStepper *)sib tag] == i) {
                    [(NSStepper *)sib setIntValue:v];
                    [(NSStepper *)sib setEnabled:on];
                }
            break;
        }
        case A800_PREF_CHOICE: {
            NSPopUpButton *pop = (NSPopUpButton *)ctrl;
            NSString *labels = [self choiceLabels:i];
            if (![labels isEqualToString:_choiceLabels[key] ? _choiceLabels[key] : @""]) {
                [pop removeAllItems];
                for (NSString *l in [labels componentsSeparatedByString:@"\n"])
                    if ([l length]) [pop addItemWithTitle:l];
                _choiceLabels[key] = labels;
            }
            const int sel = a800session_pref_get_choice(_session, i);
            [pop selectItemAtIndex:sel >= 0 && sel < [pop numberOfItems] ? sel : 0];
            break;
        }
        case A800_PREF_TEXT: {
            /* the user's own text: filled in once, never rewritten mid-edit */
            if (![[ctrl identifier] isEqualToString:@"a800-text-set"]) {
                [(NSTextField *)ctrl setStringValue:str(a800session_pref_get_text(_session, i))];
                [ctrl setIdentifier:@"a800-text-set"];
            }
            break;
        }
        default: {
            NSString *v = str(a800session_get_str(_session, p->key, ""));
            NSTextField *name = _paths[key];
            [name setStringValue:[v length] ? [v lastPathComponent] : @"None"];
            [name setToolTip:[v length] ? v : nil];
            break;
        }
        }
    }
    for (NSUInteger s = 0; s < [_romStates count]; s++)
        [_romStates[s] setStringValue:a800session_rom_available(_session, (int)s) ? @"Imported" : @"Not imported"];
    if (_padsBox)
        [_padsBox setState:a800session_get_int(_session, "enable_gamepad", 1) ? NSControlStateValueOn : NSControlStateValueOff];
    _syncing = NO;
}

/* ---- the Input page's gamepad block ----------------------------------------------- */

/* Enable gamepads: the pad thread starts or stops with the session, so it
 * applies when this window closes (the same row as FujiNet > Host's). */
- (void)padsToggled:(NSButton *)box
{
    if (_syncing) return;
    a800session_set_int(_session, "enable_gamepad", [box state] == NSControlStateValueOn ? 1 : 0);
    if (_pending < A800_APPLY_RESTART) _pending = A800_APPLY_RESTART;
    [self refresh];
}

/* The connected gamepads, and the port the chosen one is set to. */
- (void)refreshPadList
{
    const NSInteger keep = [_padList indexOfSelectedItem];
    const int count = a800session_gamepad_count(_session);
    _padGeneration = a800session_gamepad_generation(_session);
    [_padList removeAllItems];
    for (int idx = 0; idx < count; idx++) {
        char name[128];
        name[0] = '\0';
        a800session_gamepad_name(_session, idx, name, (int)sizeof name);
        const int eff = a800session_gamepad_effective_port(_session, idx);
        NSString *where = eff >= 0 ? [NSString stringWithFormat:@"port %d", eff + 1] : @"no port";
        /* items with equal titles collapse: the index keeps them apart */
        [[_padList menu] addItemWithTitle:[NSString stringWithFormat:@"%d: %@  [%@]", idx + 1,
                                           strlen(name) ? str(name) : @"Gamepad", where]
                                   action:nil keyEquivalent:@""];
    }
    if (count == 0) [[_padList menu] addItemWithTitle:@"No gamepads connected" action:nil keyEquivalent:@""];
    [_padList setEnabled:count > 0];
    [_padPort setEnabled:count > 0];
    [_padList selectItemAtIndex:keep >= 0 && keep < count ? keep : 0];
    [self padSelected:_padList];
}

- (void)padSelected:(id)sender
{
    (void)sender;
    int assign = -1;
    const NSInteger sel = [_padList indexOfSelectedItem];
    if (sel >= 0 && sel < a800session_gamepad_count(_session))
        assign = a800session_gamepad_assignment(_session, (int)sel);
    [_padPort selectItemAtIndex:assign < 0 || assign >= A800_PORTS ? 0 : assign + 1];
}

- (void)padPortChanged:(id)sender
{
    (void)sender;
    const NSInteger sel = [_padList indexOfSelectedItem];
    if (sel >= 0 && sel < a800session_gamepad_count(_session))
        a800session_gamepad_assign(_session, (int)sel, (int)[_padPort indexOfSelectedItem] - 1);
    [self refreshPadList];
    if (_controlsChanged) _controlsChanged();
}

/* Reset Controls: the default keys and gamepad buttons, every gamepad back on Automatic. */
- (void)resetControls:(id)sender
{
    (void)sender;
    a800session_bindings_reset(_session);
    for (int idx = 0; idx < a800session_gamepad_count(_session); idx++)
        a800session_gamepad_assign(_session, idx, -1);
    [self refreshPadList];
    if (_controlsChanged) _controlsChanged();
}

- (void)openControllersPressed:(id)sender
{
    (void)sender;
    if (_openControllers) _openControllers();
}

/* Appends the block to the Input page: Enable gamepads, the gamepad and the port
 * it plays, Reset Controls and Open Controllers. Hot-plugged pads are picked up
 * twice a second while the window is open. */
- (void)addGamepadRowsTo:(NSScrollView *)scroll
{
    NSView *doc = [scroll documentView];
    CGFloat y = NSHeight([doc frame]) - 12;
    [doc addSubview:[self label:@"Gamepads" frame:NSMakeRect(16, y + 6, PAGE_W - 32, 20) bold:YES]];
    y += HEAD_H;

    [doc addSubview:[self label:@"Gamepads" frame:NSMakeRect(28, y + 5, LABEL_W - 12, 20) bold:NO]];
    _padsBox = [NSButton checkboxWithTitle:@"Enable (applies when Settings closes)" target:self action:@selector(padsToggled:)];
    [_padsBox setFrame:NSMakeRect(CTRL_X, y + 3, PAGE_W - CTRL_X - 16, 22)];
    [doc addSubview:_padsBox];
    y += ROW_H;

    [doc addSubview:[self label:@"Gamepad" frame:NSMakeRect(28, y + 5, LABEL_W - 12, 20) bold:NO]];
    _padList = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(CTRL_X, y + 1, 230, 26) pullsDown:NO];
    [_padList setTarget:self];
    [_padList setAction:@selector(padSelected:)];
    [doc addSubview:_padList];
    _padPort = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(CTRL_X + 238, y + 1, 110, 26) pullsDown:NO];
    [_padPort addItemsWithTitles:@[@"Automatic", @"Port 1", @"Port 2", @"Port 3", @"Port 4"]];
    [_padPort setTarget:self];
    [_padPort setAction:@selector(padPortChanged:)];
    [doc addSubview:_padPort];
    y += ROW_H + 4;

    NSButton *reset = [NSButton buttonWithTitle:@"Reset Controls" target:self action:@selector(resetControls:)];
    [reset setFrame:NSMakeRect(CTRL_X, y, 130, 28)];
    [doc addSubview:reset];
    NSButton *open = [NSButton buttonWithTitle:@"Open Controllers" target:self action:@selector(openControllersPressed:)];
    [open setFrame:NSMakeRect(CTRL_X + 138, y, 150, 28)];
    [doc addSubview:open];
    y += 38;

    NSTextField *note = [NSTextField wrappingLabelWithString:
        @"The port a gamepad plays applies at once. Keys and gamepad buttons are re-mapped in the "
        @"Controllers window (View > Controllers, Cmd+K or F9): click Map, choose a control, then press "
        @"the key or the gamepad button. Reset Controls puts the defaults back."];
    [note setFont:[NSFont systemFontOfSize:[NSFont smallSystemFontSize]]];
    [note setTextColor:[NSColor secondaryLabelColor]];
    [note setFrame:NSMakeRect(16, y, PAGE_W - 32, 64)];
    [doc addSubview:note];
    y += 72;

    [doc setFrame:NSMakeRect(0, 0, PAGE_W, y + 12)];
    [self refreshPadList];
    [_padTimer invalidate];
    __weak A800SettingsWindow *weakSelf = self;
    _padTimer = [NSTimer scheduledTimerWithTimeInterval:0.5 repeats:YES block:^(NSTimer *tmr) {
        (void)tmr;
        A800SettingsWindow *strong = weakSelf;
        if (strong && a800session_gamepad_generation(strong->_session) != strong->_padGeneration)
            [strong refreshPadList];
    }];
    [_padTimer setTolerance:0.25];
}

/* ---- building it ------------------------------------------------------------------ */

/* A page's rows, as a scrolling column. */
- (NSScrollView *)pageForRows:(NSArray<NSNumber *> *)rows note:(NSString *)note
{
    A800FlippedView *doc = [[A800FlippedView alloc] initWithFrame:NSMakeRect(0, 0, PAGE_W, 100)];
    CGFloat y = 12;
    const char *lastGroup = NULL;
    if (note) {
        NSTextField *n = [NSTextField wrappingLabelWithString:note];
        [n setFrame:NSMakeRect(16, y, PAGE_W - 32, 34)];
        [doc addSubview:n];
        y += 40;
    }
    for (NSNumber *num in rows) {
        const int i = [num intValue];
        const a800_pref *p = a800session_pref(i);
        if (!lastGroup || strcmp(lastGroup, p->group)) {
            [doc addSubview:[self label:str(p->group) frame:NSMakeRect(16, y + 6, PAGE_W - 32, 20) bold:YES]];
            y += HEAD_H;
            lastGroup = p->group;
        }
        NSTextField *label = [self label:str(p->label) frame:NSMakeRect(28, y + 5, LABEL_W - 12, 20) bold:NO];
        if (p->help) [label setToolTip:str(p->help)];
        [doc addSubview:label];
        _labels[num] = label;
        NSView *ctrl = nil;
        switch (p->type) {
        case A800_PREF_BOOL: {
            NSButton *b = [NSButton checkboxWithTitle:p->help ? str(p->help) : @""
                                               target:self action:@selector(boolChanged:)];
            [b setFrame:NSMakeRect(CTRL_X, y + 3, PAGE_W - CTRL_X - 16, 22)];
            ctrl = b;
            break;
        }
        case A800_PREF_INT: {
            NSTextField *f = [NSTextField textFieldWithString:@""];
            [f setFrame:NSMakeRect(CTRL_X, y + 3, 70, 22)];
            [f setTarget:self];
            [f setAction:@selector(intChanged:)];
            NSStepper *st = [[NSStepper alloc] initWithFrame:NSMakeRect(CTRL_X + 74, y + 2, 19, 24)];
            [st setMinValue:p->min];
            [st setMaxValue:p->max];
            [st setIncrement:p->step > 0 ? p->step : 1];
            [st setTag:i];
            [st setTarget:self];
            [st setAction:@selector(intChanged:)];
            [doc addSubview:st];
            if (p->help)
                [doc addSubview:[self label:str(p->help) frame:NSMakeRect(CTRL_X + 104, y + 5, PAGE_W - CTRL_X - 120, 20) bold:NO]];
            ctrl = f;
            break;
        }
        case A800_PREF_CHOICE: {
            NSPopUpButton *pop = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(CTRL_X, y + 1, PAGE_W - CTRL_X - 16, 26)
                                                            pullsDown:NO];
            [pop setTarget:self];
            [pop setAction:@selector(choiceChanged:)];
            ctrl = pop;
            break;
        }
        case A800_PREF_TEXT: {
            NSTextField *f = [NSTextField textFieldWithString:@""];
            [f setFrame:NSMakeRect(CTRL_X, y + 3, PAGE_W - CTRL_X - 16, 22)];
            [f setPlaceholderString:str(a800session_pref_placeholder(i))];
            [f setToolTip:str(p->help)];
            [f setDelegate:self];
            [f setTarget:self];
            [f setAction:@selector(textChanged:)];
            ctrl = f;
            break;
        }
        default: {
            NSTextField *name = [self label:@"None" frame:NSMakeRect(CTRL_X, y + 5, PAGE_W - CTRL_X - 200, 20) bold:NO];
            [doc addSubview:name];
            _paths[num] = name;
            NSButton *choose = [NSButton buttonWithTitle:@"Choose…" target:self action:@selector(choosePath:)];
            [choose setFrame:NSMakeRect(PAGE_W - 196, y + 1, 96, 26)];
            [choose setTag:i];
            NSButton *clear = [NSButton buttonWithTitle:@"Clear" target:self action:@selector(clearPath:)];
            [clear setFrame:NSMakeRect(PAGE_W - 96, y + 1, 80, 26)];
            [clear setTag:i];
            [doc addSubview:clear];
            ctrl = choose;
            break;
        }
        }
        if ([ctrl isKindOfClass:[NSControl class]]) [(NSControl *)ctrl setTag:i];
        [doc addSubview:ctrl];
        _controls[num] = ctrl;
        y += ROW_H;
    }
    [doc setFrame:NSMakeRect(0, 0, PAGE_W, y + 12)];
    NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, PAGE_W, 560)];
    [scroll setHasVerticalScroller:YES];
    [scroll setDrawsBackground:NO];
    [scroll setDocumentView:doc];
    return scroll;
}

- (NSScrollView *)romsPage
{
    A800FlippedView *doc = [[A800FlippedView alloc] initWithFrame:NSMakeRect(0, 0, PAGE_W, 100)];
    CGFloat y = 12;
    const char *lastGroup = NULL;
    NSTextField *n = [NSTextField wrappingLabelWithString:[NSString stringWithFormat:
        @"The built-in AltirraOS and Altirra BASIC run everything without these. Machine ▸ Import "
        @"System ROMs recognises Atari's own ROMs and MAME's sets; a peripheral's ROM goes in its "
        @"slot here. Kept in %@.", str(a800session_roms_path(_session))]];
    [n setFrame:NSMakeRect(16, y, PAGE_W - 32, 52)];
    [doc addSubview:n];
    y += 60;
    _romStates = [NSMutableArray array];
    for (int s = 0; s < a800session_rom_slot_count(); s++) {
        const a800_rom_slot *slot = a800session_rom_slot(s);
        if (!lastGroup || strcmp(lastGroup, slot->group)) {
            [doc addSubview:[self label:str(slot->group) frame:NSMakeRect(16, y + 6, PAGE_W - 32, 20) bold:YES]];
            y += HEAD_H;
            lastGroup = slot->group;
        }
        [doc addSubview:[self label:str(slot->name) frame:NSMakeRect(28, y + 5, 300, 20) bold:NO]];
        NSTextField *state = [self label:@"" frame:NSMakeRect(340, y + 5, 120, 20) bold:NO];
        [doc addSubview:state];
        [_romStates addObject:state];
        NSButton *b = [NSButton buttonWithTitle:@"Import…" target:self action:@selector(importRom:)];
        [b setFrame:NSMakeRect(PAGE_W - 116, y + 1, 100, 26)];
        [b setTag:s];
        [b setToolTip:[NSString stringWithFormat:@"%u bytes", slot->size]];
        [doc addSubview:b];
        y += ROW_H;
    }
    [doc setFrame:NSMakeRect(0, 0, PAGE_W, y + 12)];
    NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, PAGE_W, 560)];
    [scroll setHasVerticalScroller:YES];
    [scroll setDrawsBackground:NO];
    [scroll setDocumentView:doc];
    return scroll;
}

- (void)show
{
    if (_window) {
        [_window makeKeyAndOrderFront:nil];
        return;
    }
    _controls = [NSMutableDictionary dictionary];
    _labels = [NSMutableDictionary dictionary];
    _paths = [NSMutableDictionary dictionary];
    _choiceLabels = [NSMutableDictionary dictionary];
    _pending = A800_APPLY_LIVE;

    NSTabView *tabs = [[NSTabView alloc] initWithFrame:NSMakeRect(0, 0, PAGE_W + 20, 600)];
    NSMutableArray<NSNumber *> *rows = [NSMutableArray array];
    const char *page = NULL;
    for (int i = 0; i <= a800session_pref_count(); i++) {
        const a800_pref *p = i < a800session_pref_count() ? a800session_pref(i) : NULL;
        if (page && (!p || strcmp(page, p->page))) {
            NSTabViewItem *item = [[NSTabViewItem alloc] initWithIdentifier:str(page)];
            [item setLabel:str(page)];
            [item setView:[self pageForRows:rows note:!strcmp(page, "Machine")
                ? @"Machine settings apply together when Settings closes: the Atari power-cycles once."
                : nil]];
            if (!strcmp(page, "Input")) [self addGamepadRowsTo:(NSScrollView *)item.view];
            [self addHelpButtonTo:item.view];
            [tabs addTabViewItem:item];
            if (!strcmp(page, "Peripherals")) {
                /* the ROMs tab follows Peripherals, as in the other frontends */
                NSTabViewItem *roms = [[NSTabViewItem alloc] initWithIdentifier:@"ROMs"];
                [roms setLabel:@"ROMs"];
                [roms setView:[self romsPage]];
                [self addHelpButtonTo:roms.view];
                [tabs addTabViewItem:roms];
            }
            [rows removeAllObjects];
        }
        if (!p) break;
        page = p->page;
        [rows addObject:@(i)];
    }

    _window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, PAGE_W + 20, 600)
                                          styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                            backing:NSBackingStoreBuffered
                                              defer:NO];
    [_window setTitle:@"Settings"];
    [_window setReleasedWhenClosed:NO];
    [_window setDelegate:self];
    [_window setContentView:tabs];
    tabs.delegate = self;
    A800SetHelpContext(_window, [@"prefs/" stringByAppendingString:tabs.tabViewItems.firstObject.identifier]);
    [_window center];
    [self refresh];
    [_window makeKeyAndOrderFront:nil];
}

/* A page's Help button, bottom left; the tab's identifier names its topic. */
- (void)addHelpButtonTo:(NSView *)page
{
    NSButton *help = A800HelpButton(nil);
    help.translatesAutoresizingMaskIntoConstraints = NO;
    [page addSubview:help];
    [help.leadingAnchor constraintEqualToAnchor:page.leadingAnchor constant:20].active = YES;
    [help.bottomAnchor constraintEqualToAnchor:page.bottomAnchor constant:-16].active = YES;
}

/* The Help button and Command+? open the page being shown. */
- (void)tabView:(NSTabView *)tabView didSelectTabViewItem:(NSTabViewItem *)item
{
    (void)tabView;
    A800SetHelpContext(_window, [@"prefs/" stringByAppendingString:item.identifier]);
}

- (void)windowWillClose:(NSNotification *)note
{
    (void)note;
    a800session_settings_flush(_session);
    [_padTimer invalidate];
    _padTimer = nil;
    _padsBox = nil;
    _padList = nil;
    _padPort = nil;
    const int did = _pending > A800_APPLY_LIVE ? a800session_apply_settings(_session) : 0;
    _window = nil;
    if (_onClosed) _onClosed(did);
}
@end
