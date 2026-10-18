/*
 * prefs.c -- the Preferences table and how each setting reaches atari800.
 *
 * Every frontend builds its dialog from s_prefs[]. Values live in the
 * settings store; prefs_emit_options() turns them into atari800 config
 * options (applied with CFG_ApplyOption) and prefs_build_argv() into the
 * command-line flags that have no config key (the peripherals).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "session_internal.h"

/* ---- machines ---------------------------------------------------------------- */

static const int ram_400[] = {8, 16, 32, 48, 0};
static const int ram_800[] = {8, 16, 24, 32, 40, 48, 52, 0};
static const int ram_600xl[] = {16, 64, 0};
static const int ram_64[] = {64, 0};
static const int ram_128[] = {128, 0};
static const int ram_compy[] = {321, 0};
static const int ram_rambo[] = {320, 0};
static const int ram_576[] = {576, 0};
static const int ram_1088[] = {1088, 0};

typedef struct {
    const char *name;
    int xl;                 /* XL/XE machine type */
    const int *rams;
    int ram_def;
    int builtin_basic, leds, fkeys, game;
    int xld;                /* 0, 1 (1400XL), 2 (1450XLD) */
} machine_info;

static const machine_info s_machines[A800_M_COUNT] = {
    {"Atari 400", 0, ram_400, 16, 0, 0, 0, 0, 0},
    {"Atari 800", 0, ram_800, 48, 0, 0, 0, 0, 0},
    {"Atari 1200XL", 1, ram_64, 64, 0, 1, 1, 0, 0},
    {"Atari 600XL", 1, ram_600xl, 16, 1, 0, 0, 0, 0},
    {"Atari 800XL", 1, ram_64, 64, 1, 0, 0, 0, 0},
    {"Atari 65XE", 1, ram_64, 64, 1, 0, 0, 0, 0},
    {"Atari 130XE", 1, ram_128, 128, 1, 0, 0, 0, 0},
    {"Atari 320XE (Compy Shop)", 1, ram_compy, 321, 1, 0, 0, 0, 0},
    {"Atari 320XE (Rambo)", 1, ram_rambo, 320, 1, 0, 0, 0, 0},
    {"Atari 576XE", 1, ram_576, 576, 1, 0, 0, 0, 0},
    {"Atari 1088XE", 1, ram_1088, 1088, 1, 0, 0, 0, 0},
    {"Atari XEGS", 1, ram_64, 64, 1, 0, 0, 1, 0},
    {"Atari 1400XL", 1, ram_64, 64, 1, 0, 0, 0, 1},
    {"Atari 1450XLD", 1, ram_64, 64, 1, 0, 0, 0, 2},
};

const char *a800_machine_name(int m)
{
    return m >= 0 && m < A800_M_COUNT ? s_machines[m].name : "";
}

int a800_machine_is_xl(int m)
{
    return m >= 0 && m < A800_M_COUNT && s_machines[m].xl;
}

int prefs_machine(struct a800session *s)
{
    int m = a800session_get_int(s, "machine", A800_M_130XE);
    return m >= 0 && m < A800_M_COUNT ? m : A800_M_130XE;
}

static int machine_ram(struct a800session *s, int m)
{
    char key[32];
    const int *r;
    int v;
    snprintf(key, sizeof key, "ram_%d", m);
    v = a800session_get_int(s, key, s_machines[m].ram_def);
    for (r = s_machines[m].rams; *r; r++)
        if (*r == v)
            return v;
    return s_machines[m].ram_def;
}

static const char *ram_label(int kb)
{
    static char buf[32];
    if (kb == 320)
        return "320 KB (Rambo)";
    if (kb == 321)
        return "320 KB (Compy Shop)";
    snprintf(buf, sizeof buf, "%d KB", kb);
    return buf;
}

/* ---- OS and BASIC revisions ------------------------------------------------- */

typedef struct {
    const char *value;      /* atari800's revision name */
    const char *label;
    int rom_slot;           /* -1: always there */
} rev_info;

static const rev_info s_os800[] = {
    {"AUTO", "Automatic", -1},
    {"ALTIRRA", "AltirraOS (built in)", -1},
    {"B-NTSC", "Rev. B (NTSC)", ROM_OS_B_NTSC},
    {"A-NTSC", "Rev. A (NTSC)", ROM_OS_A_NTSC},
    {"A-PAL", "Rev. A (PAL)", ROM_OS_A_PAL},
    {"CUSTOM", "Custom", ROM_OS800_CUSTOM},
    {NULL, NULL, 0}
};

static const rev_info s_osxl[] = {
    {"AUTO", "Automatic", -1},
    {"ALTIRRA", "AltirraOS (built in)", -1},
    {"2", "Rev. 2 (800XL)", ROM_OS_BB01R2},
    {"3", "Rev. 3 (65XE/130XE)", ROM_OS_BB01R3},
    {"4", "Rev. 4 (XEGS)", ROM_OS_BB01R4},
    {"1", "Rev. 1 (600XL)", ROM_OS_BB00R1},
    {"10", "Rev. 10 (1200XL)", ROM_OS_AA00R10},
    {"11", "Rev. 11 (1200XL)", ROM_OS_AA01R11},
    {"3A", "Rev. 3 (1450XLD)", ROM_OS_BB02R3},
    {"3B", "Rev. 3 v4 (1450XLD)", ROM_OS_BB02R3V4},
    {"5", "Rev. 5 (from source)", ROM_OS_CC01R4},
    {"59", "Rev. 59 (Arabic 65XE)", ROM_OS_BB01R59},
    {"59A", "Rev. 59 alt. (Arabic 65XE)", ROM_OS_BB01R59A},
    {"CUSTOM", "Custom", ROM_OSXL_CUSTOM},
    {NULL, NULL, 0}
};

static const rev_info s_basic[] = {
    {"AUTO", "Automatic", -1},
    {"ALTIRRA", "Altirra BASIC (built in)", -1},
    {"C", "Atari BASIC rev. C", ROM_BASIC_C},
    {"B", "Atari BASIC rev. B", ROM_BASIC_B},
    {"A", "Atari BASIC rev. A", ROM_BASIC_A},
    {"CUSTOM", "Custom", ROM_BASIC_CUSTOM},
    {NULL, NULL, 0}
};

static const rev_info *os_revs(int m)
{
    return s_machines[m].xl ? s_osxl : s_os800;
}

static const char *os_rev_key(int m)
{
    return s_machines[m].xl ? "os_rev_xl" : "os_rev_800";
}

static int rev_count(const rev_info *r)
{
    int n = 0;
    while (r[n].value)
        n++;
    return n;
}

static int rev_index(const rev_info *r, const char *v)
{
    int i;
    for (i = 0; v && r[i].value; i++)
        if (!strcmp(r[i].value, v))
            return i;
    return 0;
}

/* ---- the table ------------------------------------------------------------------ */

static const char *const ch_tv[] = {"NTSC", "PAL", NULL};
static const char *const ch_col80[] = {"None", "XEP80 (joystick port 2)", "Austin Franklin 80",
                                       "Bit3 Full-View 80", "Proto80", NULL};
static const char *const ch_pbi[] = {"None", "ICD MIO", "CSS Black Box", NULL};
static const char *const ch_voicebox[] = {"None", "Alien Group Voice Box", "Voice Box II", NULL};
static const char *const ch_art_ntsc[] = {"Off", "Artifacts (old)", "Artifacts (new)",
                                          "NTSC filter", NULL};
static const char *const ch_art_pal[] = {"Off", "Simple blending", "Accurate blending", NULL};
static const char *const ch_preset[] = {"Standard", "Deep black", "Vibrant", NULL};
static const char *const ch_ntsc_preset[] = {"Composite", "S-Video", "RGB", "Monochrome", NULL};
static const char *const ch_area[] = {"Narrow (320 \xc3\x97 200)", "TV (336 \xc3\x97 224/240)",
                                      "Overscan (384 \xc3\x97 240)", NULL};
static const char *const ch_aspect[] = {"TV (4:3)", "Square pixels", NULL};
static const char *const ch_autofire[] = {"Off", "While fire is held", "Always", NULL};
static const char *const ch_kbd_mode[] = {"Characters (type what you see)",
                                          "Positions (like the Atari's own keys)", NULL};
static const char *const ch_xep80_port[] = {"Port 1", "Port 2", NULL};

#define P_BOOL(k, pg, gr, lb, hp, d, ap) {k, pg, gr, lb, hp, A800_PREF_BOOL, d, 0, 1, 1, ap}
#define P_CHOICE(k, pg, gr, lb, hp, d, ap) {k, pg, gr, lb, hp, A800_PREF_CHOICE, d, 0, 0, 0, ap}
#define P_INT(k, pg, gr, lb, hp, d, mn, mx, st, ap) {k, pg, gr, lb, hp, A800_PREF_INT, d, mn, mx, st, ap}
#define P_FILE(k, pg, gr, lb, hp, ap) {k, pg, gr, lb, hp, A800_PREF_FILE, 0, 0, 0, 0, ap}
#define P_DIR(k, pg, gr, lb, hp, ap) {k, pg, gr, lb, hp, A800_PREF_DIR, 0, 0, 0, 0, ap}

static const a800_pref s_prefs[] = {
    /* Machine */
    P_CHOICE("machine", "Machine", "Computer", "Model", NULL, A800_M_130XE, A800_APPLY_POWER),
    P_CHOICE("ram", "Machine", "Computer", "Memory", NULL, 0, A800_APPLY_POWER),
    P_CHOICE("tv", "Machine", "Computer", "TV system", NULL, 0, A800_APPLY_POWER),
    P_BOOL("basic", "Machine", "Computer", "BASIC",
           "Start in BASIC (off holds OPTION at power-on, as FujiNet's CONFIG wants)",
           0, A800_APPLY_POWER),
    P_CHOICE("os_rev", "Machine", "System ROMs", "Operating system", NULL, 0, A800_APPLY_POWER),
    P_CHOICE("basic_rev", "Machine", "System ROMs", "BASIC", NULL, 0, A800_APPLY_POWER),
    P_BOOL("xegs_keyboard", "Machine", "XEGS", "Keyboard attached", NULL, 1, A800_APPLY_LIVE),
    P_BOOL("jumper_1200", "Machine", "1200XL", "J1 jumper fitted",
           "Disables the 1200XL's LEDs and changes the OS's keyboard handling",
           0, A800_APPLY_POWER),
    P_INT("axlon_banks", "Machine", "Memory expansions", "Axlon RAM banks",
          "400/800 only; 0 = none", 0, 0, 256, 8, A800_APPLY_POWER),
    P_INT("mosaic_banks", "Machine", "Memory expansions", "Mosaic RAM banks",
          "400/800 only; 0 = none", 0, 0, 64, 1, A800_APPLY_POWER),
    P_BOOL("mapram", "Machine", "Memory expansions", "MapRAM", "XL/XE only", 0, A800_APPLY_POWER),
    P_BOOL("sio_patch", "Machine", "Speed", "Fast disk access",
           "Patches the OS's SIO routine (internal drives only, not FujiNet)",
           1, A800_APPLY_LIVE),
    P_BOOL("slow_xex", "Machine", "Speed", "Load programs at disk speed", NULL, 0, A800_APPLY_LIVE),

    /* Peripherals */
    P_CHOICE("col80", "Peripherals", "80-column display", "Card", NULL, 0, A800_APPLY_POWER),
    P_CHOICE("xep80_port", "Peripherals", "80-column display", "XEP80 port", NULL, 1,
             A800_APPLY_POWER),
    P_CHOICE("pbi", "Peripherals", "Parallel bus", "Device", "XL/XE only", 0, A800_APPLY_POWER),
    P_FILE("pbi_disk", "Peripherals", "Parallel bus", "SCSI hard disk image", NULL,
           A800_APPLY_POWER),
    P_CHOICE("voicebox", "Peripherals", "Speech", "Voice Box", NULL, 0, A800_APPLY_POWER),
    P_FILE("ide_image", "Peripherals", "Hard disk", "IDE image (MyIDE)", NULL, A800_APPLY_POWER),
    P_BOOL("ide_cf", "Peripherals", "Hard disk", "CompactFlash", NULL, 0, A800_APPLY_POWER),
    P_BOOL("rtime", "Peripherals", "Clock", "R-Time 8 cartridge", NULL, 0, A800_APPLY_LIVE),
    P_DIR("h1_dir", "Peripherals", "H: devices", "H1:", NULL, A800_APPLY_LIVE),
    P_DIR("h2_dir", "Peripherals", "H: devices", "H2:", NULL, A800_APPLY_LIVE),
    P_DIR("h3_dir", "Peripherals", "H: devices", "H3:", NULL, A800_APPLY_LIVE),
    P_DIR("h4_dir", "Peripherals", "H: devices", "H4:", NULL, A800_APPLY_LIVE),
    P_BOOL("h_read_only", "Peripherals", "H: devices", "Read only", NULL, 1, A800_APPLY_LIVE),

    /* Video */
    P_CHOICE("artifact_ntsc", "Video", "Colour", "NTSC artifacting", NULL, 0, A800_APPLY_LIVE),
    P_CHOICE("artifact_pal", "Video", "Colour", "PAL blending", NULL, 0, A800_APPLY_LIVE),
    P_CHOICE("palette_preset", "Video", "Colour", "Palette", NULL, 0, A800_APPLY_LIVE),
    P_INT("saturation", "Video", "Colour", "Saturation", NULL, 0, -100, 100, 1, A800_APPLY_LIVE),
    P_INT("contrast", "Video", "Colour", "Contrast", NULL, 0, -100, 100, 1, A800_APPLY_LIVE),
    P_INT("brightness", "Video", "Colour", "Brightness", NULL, 0, -100, 100, 1, A800_APPLY_LIVE),
    P_INT("gamma", "Video", "Colour", "Gamma", NULL, 0, -100, 100, 1, A800_APPLY_LIVE),
    P_INT("hue", "Video", "Colour", "Tint", NULL, 0, -100, 100, 1, A800_APPLY_LIVE),
    P_FILE("palette_file", "Video", "Colour", "External palette (.act)", NULL, A800_APPLY_LIVE),
    P_CHOICE("ntsc_preset", "Video", "NTSC filter", "Signal", NULL, 0, A800_APPLY_LIVE),
    P_INT("ntsc_sharpness", "Video", "NTSC filter", "Sharpness", NULL, 0, -100, 100, 1,
          A800_APPLY_LIVE),
    P_INT("ntsc_resolution", "Video", "NTSC filter", "Resolution", NULL, 0, -100, 100, 1,
          A800_APPLY_LIVE),
    P_INT("ntsc_artifacts", "Video", "NTSC filter", "Artifacts", NULL, 0, -100, 100, 1,
          A800_APPLY_LIVE),
    P_INT("ntsc_fringing", "Video", "NTSC filter", "Fringing", NULL, 0, -100, 100, 1,
          A800_APPLY_LIVE),
    P_INT("ntsc_bleed", "Video", "NTSC filter", "Bleed", NULL, 0, -100, 100, 1, A800_APPLY_LIVE),
    P_CHOICE("area", "Video", "Picture", "Visible area", NULL, 1, A800_APPLY_LIVE),
    P_CHOICE("aspect", "Video", "Picture", "Aspect", NULL, 0, A800_APPLY_LIVE),
    P_BOOL("smooth", "Video", "Picture", "Smooth scaling", NULL, 0, A800_APPLY_LIVE),

    /* Audio */
    P_INT("volume", "Audio", "Sound", "Volume", NULL, 80, 0, 100, 5, A800_APPLY_LIVE),
    P_BOOL("stereo", "Audio", "Sound", "Stereo POKEY", "A second POKEY at $D210",
           0, A800_APPLY_POWER),
    P_BOOL("speaker", "Audio", "Sound", "Console speaker", "Key clicks and the 400/800's buzzer",
           1, A800_APPLY_LIVE),
    P_BOOL("mz_pokey", "Audio", "Sound", "High-fidelity POKEY", NULL, 1, A800_APPLY_POWER),

    /* Input */
    P_CHOICE("port1_device", "Input", "Controller ports", "Port 1", NULL, A800_DEV_JOYSTICK,
             A800_APPLY_LIVE),
    P_CHOICE("port2_device", "Input", "Controller ports", "Port 2", NULL, A800_DEV_JOYSTICK,
             A800_APPLY_LIVE),
    P_CHOICE("port3_device", "Input", "Controller ports", "Port 3", "400/800 only",
             A800_DEV_JOYSTICK, A800_APPLY_LIVE),
    P_CHOICE("port4_device", "Input", "Controller ports", "Port 4", "400/800 only",
             A800_DEV_JOYSTICK, A800_APPLY_LIVE),
    P_BOOL("multijoy", "Input", "Controller ports", "MultiJoy4 interface",
           "Four joysticks on an XL/XE", 0, A800_APPLY_LIVE),
    P_CHOICE("autofire", "Input", "Joysticks", "Autofire", NULL, 0, A800_APPLY_LIVE),
    P_BOOL("analog_stick", "Input", "Joysticks", "Gamepad sticks steer the joystick", NULL,
           1, A800_APPLY_LIVE),
    P_INT("mouse_speed", "Input", "Mice and paddles", "Speed", NULL, 3, 1, 9, 1, A800_APPLY_LIVE),
    P_CHOICE("kbd_mode", "Input", "Keyboard", "Keys", NULL, 0, A800_APPLY_LIVE),

    /* FujiNet */
    P_BOOL("enable_fujinet", "FujiNet", "FujiNet", "FujiNet",
           "The built-in FujiNet, on the Atari's SIO port", 1, A800_APPLY_RESTART),
    P_BOOL("fujinet_power", "FujiNet", "FujiNet", "Restart FujiNet on power cycle",
           "As unplugging both would", 1, A800_APPLY_LIVE),
    P_BOOL("enable_audio", "FujiNet", "Host", "Sound output", NULL, 1, A800_APPLY_RESTART),
    P_BOOL("enable_gamepad", "FujiNet", "Host", "Gamepads", NULL, 1, A800_APPLY_RESTART),
};

#define N_PREFS ((int)(sizeof s_prefs / sizeof s_prefs[0]))

int a800session_pref_count(void)
{
    return N_PREFS;
}

const a800_pref *a800session_pref(int i)
{
    return i >= 0 && i < N_PREFS ? &s_prefs[i] : NULL;
}

int a800session_pref_find(const char *key)
{
    int i;
    for (i = 0; i < N_PREFS; i++)
        if (!strcmp(s_prefs[i].key, key))
            return i;
    return -1;
}

static int pref_is(int i, const char *key)
{
    return i >= 0 && i < N_PREFS && !strcmp(s_prefs[i].key, key);
}

static const char *const *static_choices(const char *key)
{
    static const struct { const char *key; const char *const *ch; } t[] = {
        {"tv", ch_tv}, {"col80", ch_col80}, {"pbi", ch_pbi}, {"voicebox", ch_voicebox},
        {"artifact_ntsc", ch_art_ntsc}, {"artifact_pal", ch_art_pal},
        {"palette_preset", ch_preset}, {"ntsc_preset", ch_ntsc_preset}, {"area", ch_area},
        {"aspect", ch_aspect}, {"autofire", ch_autofire}, {"kbd_mode", ch_kbd_mode},
        {"xep80_port", ch_xep80_port},
    };
    size_t i;
    for (i = 0; i < sizeof t / sizeof t[0]; i++)
        if (!strcmp(t[i].key, key))
            return t[i].ch;
    return NULL;
}

const char *a800session_pref_choice(a800session *s, int i, int c)
{
    const char *const *ch;
    if (i < 0 || i >= N_PREFS || c < 0)
        return NULL;
    if (pref_is(i, "machine"))
        return c < A800_M_COUNT ? s_machines[c].name : NULL;
    if (pref_is(i, "ram")) {
        const int *r = s_machines[prefs_machine(s)].rams;
        int n = 0;
        while (r[n])
            n++;
        return c < n ? ram_label(r[c]) : NULL;
    }
    if (pref_is(i, "os_rev")) {
        const rev_info *r = os_revs(prefs_machine(s));
        return c < rev_count(r) ? r[c].label : NULL;
    }
    if (pref_is(i, "basic_rev"))
        return c < rev_count(s_basic) ? s_basic[c].label : NULL;
    if (!strncmp(s_prefs[i].key, "port", 4) && strstr(s_prefs[i].key, "_device"))
        return c < A800_DEV_COUNT ? a800_device_name(c) : NULL;
    ch = static_choices(s_prefs[i].key);
    if (!ch)
        return NULL;
    while (c > 0 && *ch) {
        ch++;
        c--;
    }
    return *ch;
}

int a800session_pref_get_choice(a800session *s, int i)
{
    int m = prefs_machine(s);
    if (pref_is(i, "ram")) {
        const int *r = s_machines[m].rams;
        int v = machine_ram(s, m), n;
        for (n = 0; r[n]; n++)
            if (r[n] == v)
                return n;
        return 0;
    }
    if (pref_is(i, "os_rev"))
        return rev_index(os_revs(m), a800session_get_str(s, os_rev_key(m), "AUTO"));
    if (pref_is(i, "basic_rev"))
        return rev_index(s_basic, a800session_get_str(s, "basic_rev", "AUTO"));
    if (i < 0 || i >= N_PREFS)
        return 0;
    return a800session_get_int(s, s_prefs[i].key, s_prefs[i].def);
}

void a800session_pref_set_choice(a800session *s, int i, int c)
{
    int m = prefs_machine(s);
    char key[32];
    if (pref_is(i, "ram")) {
        const int *r = s_machines[m].rams;
        int n = 0;
        while (r[n])
            n++;
        if (c >= 0 && c < n) {
            snprintf(key, sizeof key, "ram_%d", m);
            a800session_set_int(s, key, r[c]);
        }
        return;
    }
    if (pref_is(i, "os_rev")) {
        const rev_info *r = os_revs(m);
        if (c >= 0 && c < rev_count(r))
            a800session_set_str(s, os_rev_key(m), r[c].value);
        return;
    }
    if (pref_is(i, "basic_rev")) {
        if (c >= 0 && c < rev_count(s_basic))
            a800session_set_str(s, "basic_rev", s_basic[c].value);
        return;
    }
    if (i >= 0 && i < N_PREFS)
        a800session_set_int(s, s_prefs[i].key, c);
}

static int fitted_80col_ok(a800session *s, int card)
{
    switch (card) {
    case 1: return a800session_rom_available(s, ROM_XEP80_CHARSET);
    case 2: return a800session_rom_available(s, ROM_AF80) &&
                   a800session_rom_available(s, ROM_AF80_CHARSET);
    case 3: return a800session_rom_available(s, ROM_BIT3) &&
                   a800session_rom_available(s, ROM_BIT3_CHARSET);
    case 4: return a800session_rom_available(s, ROM_PROTO80);
    default: return 1;
    }
}

int a800session_pref_enabled(a800session *s, int i)
{
    int m = prefs_machine(s);
    const char *k;
    if (i < 0 || i >= N_PREFS)
        return 0;
    k = s_prefs[i].key;
    if (!strcmp(k, "ram"))
        return s_machines[m].rams[1] != 0;
    if (!strcmp(k, "xegs_keyboard"))
        return m == A800_M_XEGS;
    if (!strcmp(k, "jumper_1200"))
        return m == A800_M_1200XL;
    if (!strcmp(k, "axlon_banks") || !strcmp(k, "mosaic_banks"))
        return !s_machines[m].xl;
    if (!strcmp(k, "mapram"))
        return s_machines[m].xl;
    if (!strcmp(k, "pbi") || !strcmp(k, "pbi_disk"))
        return s_machines[m].xl && !s_machines[m].xld;
    if (!strcmp(k, "xep80_port"))
        return a800session_get_int(s, "col80", 0) == 1;
    if (!strcmp(k, "port3_device") || !strcmp(k, "port4_device"))
        return !s_machines[m].xl || a800session_get_int(s, "multijoy", 0);
    if (!strcmp(k, "sio_patch"))
        return !s->fujinet_wanted;
    if (!strncmp(k, "ntsc_", 5) && strcmp(k, "ntsc_preset"))
        return a800session_get_int(s, "artifact_ntsc", 0) == 3;
    return 1;
}

int a800session_pref_choice_enabled(a800session *s, int i, int c)
{
    int m = prefs_machine(s);
    if (pref_is(i, "os_rev")) {
        const rev_info *r = os_revs(m);
        if (c < 0 || c >= rev_count(r))
            return 0;
        return r[c].rom_slot < 0 || a800session_rom_available(s, r[c].rom_slot);
    }
    if (pref_is(i, "basic_rev")) {
        if (c < 0 || c >= rev_count(s_basic))
            return 0;
        return s_basic[c].rom_slot < 0 || a800session_rom_available(s, s_basic[c].rom_slot);
    }
    if (pref_is(i, "col80"))
        return fitted_80col_ok(s, c);
    if (pref_is(i, "pbi")) {
        if (c == 1)
            return a800session_rom_available(s, ROM_MIO);
        if (c == 2)
            return a800session_rom_available(s, ROM_BLACK_BOX);
        return 1;
    }
    if (pref_is(i, "machine") && c >= 0 && c < A800_M_COUNT && s_machines[c].xld)
        return a800session_rom_available(s, ROM_XLD_VOICE) &&
               (s_machines[c].xld < 2 || a800session_rom_available(s, ROM_XLD_DISK));
    return 1;
}

/* ---- to atari800 ------------------------------------------------------------- */

typedef void (*emit_fn)(void *u, const char *key, const char *val);

static void emitf(emit_fn emit, void *u, const char *key, const char *fmt, ...)
{
    char buf[1100];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    emit(u, key, buf);
}

/* The machine, its ROMs and the ROM paths every peripheral reads at start. */
void prefs_emit_options(struct a800session *s, emit_fn emit, void *u)
{
    int m = prefs_machine(s);
    const machine_info *mi = &s_machines[m];
    int ram = machine_ram(s, m);
    const char *osrev, *basrev;
    int i;

    emit(u, "MACHINE_TYPE", mi->xl ? "Atari XL/XE" : "Atari 400/800");
    if (ram == 320)
        emit(u, "RAM_SIZE", "320 (RAMBO)");
    else if (ram == 321)
        emit(u, "RAM_SIZE", "320 (COMPY SHOP)");
    else
        emitf(emit, u, "RAM_SIZE", "%d", ram);
    emit(u, "DEFAULT_TV_MODE", a800session_get_int(s, "tv", 0) ? "PAL" : "NTSC");
    emitf(emit, u, "BUILTIN_BASIC", "%d", mi->builtin_basic);
    emitf(emit, u, "KEYBOARD_LEDS", "%d", mi->leds);
    emitf(emit, u, "F_KEYS", "%d", mi->fkeys);
    emitf(emit, u, "BUILTIN_GAME", "%d", mi->game);
    emitf(emit, u, "KEYBOARD_DETACHED", "%d",
          m == A800_M_XEGS && !a800session_get_int(s, "xegs_keyboard", 1));
    emitf(emit, u, "1200XL_JUMPER", "%d",
          m == A800_M_1200XL && a800session_get_int(s, "jumper_1200", 0));
    emitf(emit, u, "DISABLE_BASIC", "%d", !a800session_get_int(s, "basic", 0));
    emitf(emit, u, "AXLON_RAM_NUM_BANKS", "%d",
          mi->xl ? 0 : a800session_get_int(s, "axlon_banks", 0));
    emitf(emit, u, "MOSAIC_RAM_NUM_BANKS", "%d",
          mi->xl ? 0 : a800session_get_int(s, "mosaic_banks", 0));
    emitf(emit, u, "ENABLE_MAPRAM", "%d", mi->xl && a800session_get_int(s, "mapram", 0));

    /* ROMs: every slot, so a removed ROM is forgotten on the next start */
    for (i = 0; i < a800session_rom_slot_count(); i++) {
        const char *ck = roms_config_key(i);
        if (ck)
            emit(u, ck, roms_path(s, i));
    }
    osrev = a800session_get_str(s, os_rev_key(m), "AUTO");
    basrev = a800session_get_str(s, "basic_rev", "AUTO");
    {
        const rev_info *r = os_revs(m);
        int k = rev_index(r, osrev);
        if (r[k].rom_slot >= 0 && !a800session_rom_available(s, r[k].rom_slot))
            k = 0;
        emit(u, mi->xl ? "OS_XL/XE_VERSION" : "OS_400/800_VERSION", r[k].value);
        k = rev_index(s_basic, basrev);
        if (s_basic[k].rom_slot >= 0 && !a800session_rom_available(s, s_basic[k].rom_slot))
            k = 0;
        emit(u, "BASIC_VERSION", s_basic[k].value);
    }

    emitf(emit, u, "ENABLE_NEW_POKEY", "%d", a800session_get_int(s, "mz_pokey", 1));
    emitf(emit, u, "STEREO_POKEY", "%d", a800session_get_int(s, "stereo", 0));
    emitf(emit, u, "SPEAKER_SOUND", "%d", a800session_get_int(s, "speaker", 1));
    emitf(emit, u, "RTIME", "%d", a800session_get_int(s, "rtime", 0));
    emitf(emit, u, "ENABLE_SLOW_XEX_LOADING", "%d", a800session_get_int(s, "slow_xex", 0));

    if (*a800session_get_str(s, "pbi_disk", "")) {
        emit(u, "MIO_SCSI_DISK", a800session_get_str(s, "pbi_disk", ""));
        emit(u, "BB_SCSI_DISK", a800session_get_str(s, "pbi_disk", ""));
    }
    prefs_emit_live(s, emit, u);
}

/* What can change while running (also part of every start). */
void prefs_emit_live(struct a800session *s, emit_fn emit, void *u)
{
    static const char *const art_ntsc[] = {"NONE", "NTSC-OLD", "NTSC-NEW", "NTSC-FULL"};
    static const char *const art_pal[] = {"NONE", "PAL-SIMPLE", "PAL-BLEND"};
    int an = a800session_get_int(s, "artifact_ntsc", 0);
    int ap = a800session_get_int(s, "artifact_pal", 0);
    const char *pal = a800session_get_str(s, "palette_file", "");
    char key[16];
    int h;

    emit(u, "ARTIFACT_NTSC", art_ntsc[an >= 0 && an < 4 ? an : 0]);
    emit(u, "ARTIFACT_PAL", art_pal[ap >= 0 && ap < 3 ? ap : 0]);
    emit(u, "COLOURS_NTSC_EXTERNAL_PALETTE", pal);
    emit(u, "COLOURS_PAL_EXTERNAL_PALETTE", pal);
    emitf(emit, u, "COLOURS_NTSC_EXTERNAL_PALETTE_LOADED", "%d", *pal != 0);
    emitf(emit, u, "COLOURS_PAL_EXTERNAL_PALETTE_LOADED", "%d", *pal != 0);
    for (h = 1; h <= 4; h++) {
        char pk[8];
        snprintf(key, sizeof key, "H%d_DIR", h);
        snprintf(pk, sizeof pk, "h%d_dir", h);
        emit(u, key, a800session_get_str(s, pk, ""));
    }
    emitf(emit, u, "HD_READ_ONLY", "%d", a800session_get_int(s, "h_read_only", 1));
}

/* Peripherals atari800 only takes as command-line flags. */
int prefs_build_argv(struct a800session *s, char **argv, int max)
{
    int m = prefs_machine(s), n = 0;
    int col80 = a800session_get_int(s, "col80", 0);
    int pbi = a800session_get_int(s, "pbi", 0);
    int vb = a800session_get_int(s, "voicebox", 0);
    const char *ide = a800session_get_str(s, "ide_image", "");

    if (n < max)
        argv[n++] = "fujinet-go-atari800";
    if (s_machines[m].xld == 1 && n < max)
        argv[n++] = "-1400";
    if (s_machines[m].xld == 2 && n < max)
        argv[n++] = "-xld";
    if (!fitted_80col_ok(s, col80))
        col80 = 0;
    switch (col80) {
    case 1:
        if (n + 3 <= max) {
            argv[n++] = "-xep80";
            argv[n++] = "-xep80port";
            argv[n++] = a800session_get_int(s, "xep80_port", 1) ? "1" : "0";
        }
        break;
    case 2: if (n < max) argv[n++] = "-af80"; break;
    case 3: if (n < max) argv[n++] = "-bit3"; break;
    case 4: if (n < max) argv[n++] = "-proto80"; break;
    default: break;
    }
    if (s_machines[m].xl && !s_machines[m].xld) {
        if (pbi == 1 && a800session_rom_available(s, ROM_MIO) && n < max)
            argv[n++] = "-mio";
        if (pbi == 2 && a800session_rom_available(s, ROM_BLACK_BOX) && n < max)
            argv[n++] = "-bb";
    }
    if (vb == 1 && n < max)
        argv[n++] = "-voicebox";
    if (vb == 2 && n < max)
        argv[n++] = "-voiceboxii";
    if (*ide && n + 3 <= max) {
        argv[n++] = "-ide";
        argv[n++] = (char *)ide;
        if (a800session_get_int(s, "ide_cf", 0))
            argv[n++] = "-ide_cf";
    }
    {
        /* the CX85 keypad: atari800 takes its port only at start */
        int p;
        static char cx85[2][4];
        for (p = 0; p < A800_PORTS; p++) {
            char key[16];
            snprintf(key, sizeof key, "port%d_device", p + 1);
            if (a800session_get_int(s, key, A800_DEV_JOYSTICK) == A800_DEV_CX85 && n + 2 <= max) {
                snprintf(cx85[0], sizeof cx85[0], "%d", p + 1);
                argv[n++] = "-cx85";
                argv[n++] = cx85[0];
                break;
            }
        }
    }
    if (n < max)
        argv[n] = NULL;
    return n;
}
