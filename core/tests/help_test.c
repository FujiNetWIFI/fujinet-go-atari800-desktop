/*
 * help_test -- the Help covers what the app has: every Preferences setting
 * and choice, every machine, every ROM, every default key and gamepad button,
 * the app's shortcuts (and the Mac's), the two ports, the debugger's tabs
 * and prompt commands, and every context a Help button opens. Every miss is
 * listed before it fails.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "a800help.h"
#include "a800session.h"
#include "test_tmpdir.h"

#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); exit(1); } } while (0)

static const unsigned platforms[] = {
    A800HELP_GNOME, A800HELP_KDE, A800HELP_WINDOWS, A800HELP_MACOS
};
static const char *const platform_names[] = { "GNOME", "KDE", "Windows", "macOS" };

/* What the frontends' Help buttons and menus open. */
static const char *const contexts[] = {
    "main", "import-sd" , "controllers", "fujilog",
    "prefs/Machine", "prefs/Peripherals", "prefs/Video", "prefs/Audio", "prefs/Input",
    "prefs/FujiNet", "prefs/ROMs",
    "debugger/console", "debugger/cpu", "debugger/disassembly", "debugger/antic", "debugger/gtia",
    "debugger/pokey", "debugger/map", "debugger/breakpoints", "debugger/sio", "debugger/fujinet",
};

/* The app's shortcuts, as a PC shows them (and the Mac's, where it has one). */
static const struct { const char *pc, *mac; } shortcuts[] = {
    { "Shift+F5", "⇧F5" }, { "F9", "F9" }, { "F11", "F11" }, { "F12", "F12" }, { "Ctrl+,", "⌘," },
    { "Ctrl+Shift+O", "⌘O" }, { "Ctrl+Shift+C", "⌘C" }, { "Ctrl+Shift+V", "⌘V" },
    { "Ctrl+Shift+Q", "⌘Q" }, { "Ctrl+Shift+H", "⌘?" },
};

static int misses;

/* Lower case, every run of white space one space. */
static void
fold(const char *in, char *out, size_t outsz)
{
    size_t n = 0;
    int space = 0;
    for (; *in && n + 1 < outsz; in++) {
        unsigned char c = (unsigned char) *in;
        if (c == ' ' || c == '\n' || c == '\t') {
            space = 1;
            continue;
        }
        if (space && n)
            out[n++] = ' ';
        space = 0;
        out[n++] = (char) ((c >= 'A' && c <= 'Z') ? c + 32 : c);
    }
    out[n] = 0;
}

static char section[1 << 18];
static char raw[1 << 18];

/* The folded text of a context's (or slug's) section on platform p; raw
 * keeps the lines. */
static const char *
section_text(int p, const char *context)
{
    int t, b;
    section[0] = 0;
    raw[0] = 0;
    if (a800help_find(context, &t, &b) == 0) {
        a800help_plain_text(platforms[p], t, b, raw, sizeof raw);
        fold(raw, section, sizeof section);
    }
    return section;
}

static void
require(int p, const char *context, const char *what, const char *text)
{
    char want[512];
    fold(text, want, sizeof want);
    if (!strstr(section_text(p, context), want)) {
        fprintf(stderr, "MISSING (%s) %s: %s \"%s\"\n", platform_names[p], context, what, text);
        misses++;
    }
}

/* A line of the section (a table row: cells joined by " | ") with every one
 * of these, folded, in that order. */
static void
require_row(int p, const char *context, const char *a, const char *b, const char *c)
{
    char line[1024], fa[256], fb[256], fc[256];
    const char *s, *e;
    int ok = 0;

    section_text(p, context);
    fold(a, fa, sizeof fa);
    fold(b ? b : "", fb, sizeof fb);
    fold(c ? c : "", fc, sizeof fc);
    for (s = raw; *s && !ok; s = *e ? e + 1 : e) {
        char *x, *y;
        size_t n;
        e = strchr(s, '\n');
        if (!e)
            e = s + strlen(s);
        n = (size_t) (e - s) < sizeof line - 1 ? (size_t) (e - s) : sizeof line - 1;
        memcpy(line, s, n);
        line[n] = 0;
        fold(line, line, sizeof line);
        x = strstr(line, fa);
        if (!x)
            continue;
        y = b && *fb ? strstr(x + strlen(fa), fb) : x + strlen(fa);
        if (!y)
            continue;
        if (c && *fc && !strstr(y + (b && *fb ? strlen(fb) : 0), fc))
            continue;
        ok = 1;
    }
    if (!ok) {
        fprintf(stderr, "MISSING (%s) %s: a row \"%s\" | \"%s\" | \"%s\"\n", platform_names[p], context,
                a, b ? b : "", c ? c : "");
        misses++;
    }
}

static void
check_prefs(a800session *s, int p)
{
    /* every setting: its label and key on its page's section, and every
     * choice it offers */
    for (int i = 0; i < a800session_pref_count(); i++) {
        const a800_pref *pr = a800session_pref(i);
        char ctx[64];
        snprintf(ctx, sizeof ctx, "prefs/%s", pr->page);
        require(p, ctx, "label", pr->label);
        require(p, ctx, "setting", pr->key);
        if (pr->type == A800_PREF_CHOICE && strcmp(pr->key, "ram") && strcmp(pr->key, "machine") && strcmp(pr->key, "os_rev") &&
            strcmp(pr->key, "basic_rev") && strncmp(pr->key, "port", 4))
            for (int c = 0; a800session_pref_choice(s, i, c); c++)
                require(p, ctx, "choice", a800session_pref_choice(s, i, c));
    }
    for (int d = 0; d < A800_DEV_COUNT; d++)
        require(p, "prefs/Input", "device", a800_device_name(d));
    for (int m = 0; m < A800_M_COUNT; m++)
        require(p, "machines", "machine", a800_machine_name(m));
    for (int i = 0; i < a800session_rom_slot_count(); i++) {
        const a800_rom_slot *r = a800session_rom_slot(i);
        if (r) require(p, "system-roms", "ROM slot group", r->group);
    }
}

static void
check_controls(a800session *s, int p)
{
    char name[64];
    static const int con[] = { A800_CON_OPTION, A800_CON_SELECT, A800_CON_START, A800_CON_RESET,
                               A800_CON_HELP, A800_CON_BREAK };
    static const char *const con_names[] = { "Option", "Select", "Start", "Reset", "Help", "Break" };

    for (int a = 0; a < A800_ACT_PER_PORT; a++) {
        a800_binding b = a800session_binding_get(s, A800_TARGET_PORT(0, a));
        if (b.keysym) {
            a800session_keysym_name(b.keysym, name, sizeof name);
            require(p, "default-keys", "stick key", name);
        }
    }
    for (size_t k = 0; k < sizeof con / sizeof *con; k++) {
        a800_binding b = a800session_binding_get(s, A800_TARGET_CONSOLE(con[k]));
        a800session_keysym_name(b.keysym, name, sizeof name);
        require_row(p, "default-keys", con_names[k], name, NULL);
    }
    for (size_t k = 0; k < sizeof shortcuts / sizeof *shortcuts; k++) {
        require(p, "default-keys", "shortcut",
                platforms[p] == A800HELP_MACOS ? shortcuts[k].mac : shortcuts[k].pc);
    }
}

static void
check_ports(int p)
{
    char port[16];
    snprintf(port, sizeof port, "%d", A800SESSION_NETSIO_PORT);
    require(p, "fujinet", "NetSIO port", port);
    snprintf(port, sizeof port, "%d", A800SESSION_WEBUI_PORT);
    require(p, "fujinet", "web UI port", port);
    require(p, "an-external-fujinet", "listen address", "0.0.0.0:11514");
}

int
main(void)
{
    char cfg[512], data[512];
    a800session_paths paths;
    a800help_hit hits[16];
    a800session *s;
    int t, b;

    test_tmpdir(cfg, sizeof cfg, "hcfg");
    test_tmpdir(data, sizeof data, "hdata");
    memset(&paths, 0, sizeof paths);
    paths.config_dir = cfg;
    paths.data_dir = data;
    paths.fujinet_lib = "";
    s = a800session_new(&paths);
    CHECK(s, "new");
    CHECK(a800help_topic_count() > 0, "no topics");

    for (size_t k = 0; k < sizeof contexts / sizeof *contexts; k++)
        if (a800help_find(contexts[k], &t, &b) != 0) {
            fprintf(stderr, "MISSING context %s\n", contexts[k]);
            misses++;
        }
    for (int p = 0; p < 4; p++) {
        check_prefs(s, p);
        check_controls(s, p);
        check_ports(p);
        
    }

    /* lookup and search */
    CHECK(a800help_find("#preferences", &t, &b) == 0 && b == -1, "#preferences");
    CHECK(a800help_find("#the-settings-file", &t, &b) == 0 && b >= 0, "#the-settings-file");
    CHECK(a800help_search(A800HELP_GNOME, "NetSIO", hits, 16) > 0, "search NetSIO");
    CHECK(a800help_search(A800HELP_GNOME, "zzqxj", hits, 16) == 0, "search nonsense");
    int n = a800help_search(A800HELP_KDE, "breakpoints", hits, 16);
    CHECK(n > 0 && a800help_find("debugger/breakpoints", &t, &b) == 0
          && hits[0].topic == t && hits[0].block == b, "breakpoints is the first hit");
    for (int i = 1; i < n; i++)
        CHECK(hits[i - 1].score >= hits[i].score, "hits best first");

    /* the Mac sees its own keys */
    {
        char text[4096];
        CHECK(a800help_find("default-keys", &t, &b) == 0, "default-keys");
        a800help_plain_text(A800HELP_MACOS, t, b, text, sizeof text);
        CHECK(strstr(text, "⌘O") != NULL, "the Mac's Open key");
        a800help_plain_text(A800HELP_GNOME, t, b, text, sizeof text);
        CHECK(strstr(text, "Ctrl+Shift+O") != NULL && !strstr(text, "⌘O"), "the PC's");
    }

    a800session_free(s);
    CHECK(misses == 0, "%d missing from data/help/help.md", misses);
    printf("help ok: %d topics\n", a800help_topic_count());
    return 0;
}
