/*
 * FujiNet Go Atari 800 -- the macOS (AppKit) frontend.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import <Cocoa/Cocoa.h>

#import "AppDelegate.h"

#include "a800session.h"

int main(int argc, const char **argv)
{
    @autoreleasepool {
        /* The runtime is packed beside the executable (see CMakeLists.txt);
         * tell the session where, so a fresh install provisions its tree
         * from the bundle rather than hunting. */
        a800session_paths paths = { NULL, NULL, NULL, NULL };
        NSString *exeDir = [[[NSBundle mainBundle] executablePath] stringByDeletingLastPathComponent];
        NSString *runtime = [exeDir stringByAppendingPathComponent:@"fujinet"];
        if ([[NSFileManager defaultManager] fileExistsAtPath:runtime])
            paths.fujinet_runtime_src = [runtime fileSystemRepresentation];

        a800session *session = a800session_new(&paths);
        if (!session) {
            NSLog(@"Could not create the session (unusable config or data directories?)");
            return 1;
        }

        NSApplication *app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];

        /* A path on the command line is routed as a dropped file would be:
         * a disk boots, a program runs, a cartridge goes in, a ROM is imported
         * (see -[A800AppDelegate loadMedia:]). macOS adds -psn_
         * and -NS* arguments of its own: those are not files. */
        A800AppDelegate *delegate = [[A800AppDelegate alloc]
            initWithSession:session mediaPath:(argc > 1 && argv[1][0] != '-' ? argv[1] : NULL)];
        [app setDelegate:delegate];
        [app activateIgnoringOtherApps:YES];
        [app run];
    }
    return 0;
}
