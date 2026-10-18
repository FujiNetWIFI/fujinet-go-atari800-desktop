/*
 * roms.c -- the ROM slots, identifying and importing a user's own images.
 *
 * Imported images are copied to <data>/roms/<slot key>.rom; a slot is
 * available when its file exists. System ROMs are recognised by size and
 * CRC-32 (atari800's own table, sysrom.c), including MAME's split chips and
 * the XEGS's combined 32K chip, from a file or a zip. Peripheral firmware has
 * no published CRCs: those files are offered the slots of their size.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "roms_embedded.h"
#include "session_internal.h"
#include "zipread.h"

typedef struct {
    a800_rom_slot pub;
    const char *config_key;     /* atari800's, NULL if none */
    uint32_t crc;               /* 0 = none known */
    uint32_t alt_size;          /* a second accepted size, or 0 */
} slot_info;

#define K *1024u

static const slot_info s_slots[ROM_COUNT] = {
    {{"os_a_ntsc", "400/800 OS rev. A (NTSC)", "400/800 OS", 10 K}, "ROM_OS_A_NTSC", 0xc1b3bb02, 0},
    {{"os_a_pal", "400/800 OS rev. A (PAL)", "400/800 OS", 10 K}, "ROM_OS_A_PAL", 0x72b3fed4, 0},
    {{"os_b_ntsc", "400/800 OS rev. B (NTSC)", "400/800 OS", 10 K}, "ROM_OS_B_NTSC", 0x0e86d61d, 0},
    {{"os_aa00r10", "1200XL OS rev. 10", "XL/XE OS", 16 K}, "ROM_OS_AA00R10", 0xc5c11546, 0},
    {{"os_aa01r11", "1200XL OS rev. 11", "XL/XE OS", 16 K}, "ROM_OS_AA00R11", 0x1a1d7b1b, 0},
    {{"os_bb00r1", "600XL OS rev. 1", "XL/XE OS", 16 K}, "ROM_OS_BB00R1", 0x643bcc98, 0},
    {{"os_bb01r2", "800XL OS rev. 2", "XL/XE OS", 16 K}, "ROM_OS_BB01R2", 0x1f9cd270, 0},
    {{"os_bb02r3", "1450XLD OS rev. 3", "XL/XE OS", 16 K}, "ROM_OS_BB02R3", 0x0d477aa1, 0},
    {{"os_bb02r3v4", "1450XLD OS rev. 3 v4", "XL/XE OS", 16 K}, "ROM_OS_BB02R3V4", 0xd425a9cf, 0},
    {{"os_cc01r4", "XL/XE OS rev. 5 (from source)", "XL/XE OS", 16 K}, "ROM_OS_CC01R4", 0x0e000b99, 0},
    {{"os_bb01r3", "65XE/130XE OS rev. 3", "XL/XE OS", 16 K}, "ROM_OS_BB01R3", 0x29f133f7, 0},
    {{"os_bb01r4", "XEGS OS rev. 4", "XL/XE OS", 16 K}, "ROM_OS_BB01R4", 0x1eaf4002, 0},
    {{"os_bb01r59", "Arabic 65XE OS rev. 59", "XL/XE OS", 16 K}, "ROM_OS_BB01R59", 0x45f47988, 0},
    {{"os_bb01r59a", "Arabic 65XE OS rev. 59 (alt.)", "XL/XE OS", 16 K}, "ROM_OS_BB01R59A", 0xf0a236d3, 0},
    {{"basic_a", "Atari BASIC rev. A", "BASIC", 8 K}, "ROM_BASIC_A", 0x4bec4de2, 0},
    {{"basic_b", "Atari BASIC rev. B", "BASIC", 8 K}, "ROM_BASIC_B", 0xf0202fb3, 0},
    {{"basic_c", "Atari BASIC rev. C", "BASIC", 8 K}, "ROM_BASIC_C", 0x7d684184, 0},
    {{"xegame", "XEGS Missile Command", "XEGS", 8 K}, "ROM_XEGAME", 0xbdca01fb, 0},
    {{"os800_custom", "Custom 400/800 OS", "400/800 OS", 10 K}, "ROM_400/800_CUSTOM", 0, 0},
    {{"osxl_custom", "Custom XL/XE OS", "XL/XE OS", 16 K}, "ROM_XL/XE_CUSTOM", 0, 0},
    {{"basic_custom", "Custom BASIC", "BASIC", 8 K}, "ROM_BASIC_CUSTOM", 0, 0},
    {{"xegame_custom", "Custom XEGS game", "XEGS", 8 K}, "ROM_XEGAME_CUSTOM", 0, 0},
    {{"xld_voice", "1400XL/1450XLD voice and modem ROM", "Peripherals", 4 K}, "XLD_V_ROM", 0, 0},
    {{"xld_disk", "1450XLD disk ROM", "Peripherals", 2 K}, "XLD_D_ROM", 0, 0},
    {{"mio", "ICD MIO ROM", "Peripherals", 16 K}, "MIO_ROM", 0, 0},
    {{"black_box", "CSS Black Box ROM", "Peripherals", 64 K}, "BLACK_BOX_ROM", 0, 16 K},
    {{"proto80", "Proto80 ROM", "Peripherals", 2 K}, "PROTO80_ROM", 0, 0},
    {{"af80", "Austin Franklin 80 ROM", "Peripherals", 4 K}, "AF80_ROM", 0, 0},
    {{"af80_charset", "Austin Franklin 80 character set", "Peripherals", 4 K}, "AF80_CHARSET", 0, 0},
    {{"bit3", "Bit3 Full-View 80 ROM", "Peripherals", 4 K}, "BIT3_ROM", 0, 0},
    {{"bit3_charset", "Bit3 Full-View 80 character set", "Peripherals", 4 K}, "BIT3_CHARSET", 0, 0},
    {{"xep80_charset", "XEP80 character set", "Peripherals", 8 K}, "XEP80_CHARSET", 0, 0},
};

int a800session_rom_slot_count(void)
{
    return ROM_COUNT;
}

const a800_rom_slot *a800session_rom_slot(int i)
{
    return i >= 0 && i < ROM_COUNT ? &s_slots[i].pub : NULL;
}

const char *roms_config_key(int slot)
{
    return slot >= 0 && slot < ROM_COUNT ? s_slots[slot].config_key : NULL;
}

static void slot_file(struct a800session *s, int slot, char *dst, size_t n)
{
    snprintf(dst, n, "%.900s/%s.rom", s->roms_dir, s_slots[slot].pub.key);
}

static int file_exists(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

int a800session_rom_available(a800session *s, int slot)
{
    char p[A800_PATH_MAX];
    if (slot < 0 || slot >= ROM_COUNT)
        return 0;
    slot_file(s, slot, p, sizeof p);
    return file_exists(p);
}

/* The imported file, or "" -- a static buffer per slot for emit(). */
const char *roms_path(struct a800session *s, int slot)
{
    static char paths[ROM_COUNT][A800_PATH_MAX];
    if (slot < 0 || slot >= ROM_COUNT)
        return "";
    slot_file(s, slot, paths[slot], sizeof paths[slot]);
    if (!file_exists(paths[slot]))
        paths[slot][0] = 0;
    return paths[slot];
}

/* ---- identification ------------------------------------------------------- */

uint32_t roms_crc32(const uint8_t *p, size_t n)
{
    static uint32_t table[256];
    static int init;
    uint32_t c = 0xffffffffu;
    if (!init) {
        uint32_t i, j, v;
        for (i = 0; i < 256; i++) {
            v = i;
            for (j = 0; j < 8; j++)
                v = v & 1 ? 0xedb88320u ^ (v >> 1) : v >> 1;
            table[i] = v;
        }
        init = 1;
    }
    while (n--)
        c = table[(c ^ *p++) & 0xff] ^ (c >> 8);
    return ~c;
}

static int slot_for_crc(uint32_t size, uint32_t crc)
{
    int i;
    for (i = 0; i < ROM_COUNT; i++)
        if (s_slots[i].crc && s_slots[i].crc == crc && s_slots[i].pub.size == size)
            return i;
    return -1;
}

static int size_fits(int slot, uint32_t size)
{
    return s_slots[slot].pub.size == size || (s_slots[slot].alt_size && s_slots[slot].alt_size == size);
}

/* MAME's chip sets: concatenated in address order they make atari800's image. */
typedef struct {
    int slot;
    int n;
    uint32_t crc[3];
    uint32_t size[3];
} chip_set;

static const chip_set s_chips[] = {
    {ROM_OS_B_NTSC, 3, {0x6a5d766e, 0xd818f3e8, 0xc1690a9b}, {2 K, 4 K, 4 K}},
    {ROM_OS_A_NTSC, 3, {0x6a5d766e, 0x29f64e17, 0xbc533f0c}, {2 K, 4 K, 4 K}},
    {ROM_OS_A_PAL, 3, {0x6a5d766e, 0x8e547f56, 0xbe55b413}, {2 K, 4 K, 4 K}},
    {ROM_OS_AA01R11, 2, {0x6e29ec8d, 0xd73ce29a}, {8 K, 8 K}},
    {ROM_OS_AA00R10, 2, {0x0391386b, 0xb502f1e7}, {8 K, 8 K}},
};

typedef struct {
    uint8_t *data;
    uint32_t size;
    uint32_t crc;
} blob;

static int write_slot(struct a800session *s, int slot, const uint8_t *data, uint32_t size)
{
    char p[A800_PATH_MAX], tmp[A800_PATH_MAX + 8];
    FILE *fp;
    slot_file(s, slot, p, sizeof p);
    snprintf(tmp, sizeof tmp, "%s.tmp", p);
    fp = fopen(tmp, "wb");
    if (!fp)
        return -1;
    if (fwrite(data, 1, size, fp) != size) {
        fclose(fp);
        remove(tmp);
        return -1;
    }
    fclose(fp);
#if defined(_WIN32)
    remove(p);
#endif
    return rename(tmp, p);
}

static int read_file(const char *path, uint8_t **data, uint32_t *size)
{
    FILE *fp = fopen(path, "rb");
    long n;
    if (!fp)
        return -1;
    fseek(fp, 0, SEEK_END);
    n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (n <= 0 || n > 4 * 1024 * 1024) {
        fclose(fp);
        return -1;
    }
    *data = malloc((size_t)n);
    if (!*data || fread(*data, 1, (size_t)n, fp) != (size_t)n) {
        free(*data);
        fclose(fp);
        return -1;
    }
    fclose(fp);
    *size = (uint32_t)n;
    return 0;
}

static void append_msg(char *msg, int msgsz, const char *what)
{
    size_t l;
    if (!msg || msgsz <= 0)
        return;
    l = strlen(msg);
    snprintf(msg + l, (size_t)msgsz - l, "%s%s", l ? ", " : "", what);
}

/* Imports every recognised image among blobs[]; returns how many. */
static int import_blobs(struct a800session *s, blob *b, int n, char *msg, int msgsz)
{
    int imported = 0, i, k;
    size_t c;

    for (i = 0; i < n; i++) {
        int slot = slot_for_crc(b[i].size, b[i].crc);
        if (slot >= 0 && write_slot(s, slot, b[i].data, b[i].size) == 0) {
            append_msg(msg, msgsz, s_slots[slot].pub.name);
            imported++;
        }
        /* the XEGS's chip: game, BASIC C, OS rev. 4 */
        if (b[i].size == 32 K) {
            static const struct { int slot; uint32_t off, size; } parts[] = {
                {ROM_XEGAME, 0, 8 K}, {ROM_BASIC_C, 8 K, 8 K}, {ROM_OS_BB01R4, 16 K, 16 K}};
            for (k = 0; k < 3; k++) {
                const uint8_t *p = b[i].data + parts[k].off;
                if (roms_crc32(p, parts[k].size) == s_slots[parts[k].slot].crc &&
                    write_slot(s, parts[k].slot, p, parts[k].size) == 0) {
                    append_msg(msg, msgsz, s_slots[parts[k].slot].pub.name);
                    imported++;
                }
            }
        }
    }
    for (c = 0; c < sizeof s_chips / sizeof s_chips[0]; c++) {
        const chip_set *cs = &s_chips[c];
        int found[3] = {-1, -1, -1};
        uint8_t img[16 K];
        uint32_t off = 0;
        for (k = 0; k < cs->n; k++)
            for (i = 0; i < n; i++)
                if (b[i].crc == cs->crc[k] && b[i].size == cs->size[k])
                    found[k] = i;
        for (k = 0; k < cs->n && found[k] >= 0; k++)
            ;
        if (k < cs->n)
            continue;
        for (k = 0; k < cs->n; k++) {
            memcpy(img + off, b[found[k]].data, cs->size[k]);
            off += cs->size[k];
        }
        /* MAME marks most of these chips BAD_DUMP: only a known image counts */
        if (roms_crc32(img, off) == s_slots[cs->slot].crc && write_slot(s, cs->slot, img, off) == 0) {
            append_msg(msg, msgsz, s_slots[cs->slot].pub.name);
            imported++;
        }
    }
    return imported;
}

static int load_blobs(const char *path, blob **out, int *count)
{
    blob *b = NULL;
    int n = 0;
    zipread *z = zipread_open(path);
    if (z) {
        int i, e = zipread_count(z);
        b = calloc((size_t)(e > 0 ? e : 1), sizeof *b);
        for (i = 0; b && i < e; i++) {
            uint8_t *d;
            uint32_t sz;
            if (zipread_extract(z, i, &d, &sz, 4 * 1024 * 1024) == 0) {
                b[n].data = d;
                b[n].size = sz;
                b[n].crc = roms_crc32(d, sz);
                n++;
            }
        }
        zipread_close(z);
    }
    else {
        b = calloc(1, sizeof *b);
        if (b && read_file(path, &b[0].data, &b[0].size) == 0) {
            b[0].crc = roms_crc32(b[0].data, b[0].size);
            n = 1;
        }
    }
    *out = b;
    *count = n;
    return n > 0 ? 0 : -1;
}

static void free_blobs(blob *b, int n)
{
    int i;
    for (i = 0; i < n; i++)
        free(b[i].data);
    free(b);
}

int roms_identify_file(const char *path)
{
    blob *b;
    int n, i, slot = -1;
    if (load_blobs(path, &b, &n) != 0)
        return -1;
    for (i = 0; i < n && slot < 0; i++) {
        slot = slot_for_crc(b[i].size, b[i].crc);
        if (slot < 0 && b[i].size == 32 K &&
            roms_crc32(b[i].data + 16 K, 16 K) == s_slots[ROM_OS_BB01R4].crc)
            slot = ROM_OS_BB01R4;
    }
    if (slot < 0 && n > 1) {
        size_t c;
        for (c = 0; c < sizeof s_chips / sizeof s_chips[0] && slot < 0; c++) {
            int k, ok = 1;
            for (k = 0; k < s_chips[c].n; k++) {
                int hit = 0;
                for (i = 0; i < n; i++)
                    hit |= b[i].crc == s_chips[c].crc[k];
                ok &= hit;
            }
            if (ok)
                slot = s_chips[c].slot;
        }
    }
    free_blobs(b, n);
    return slot;
}

int a800session_rom_probe(a800session *s, const char *path, int *slots, int max)
{
    blob *b;
    int n, i, k = 0;
    (void)s;
    i = roms_identify_file(path);
    if (i >= 0) {
        if (max > 0)
            slots[k++] = i;
        return k;
    }
    if (load_blobs(path, &b, &n) != 0)
        return 0;
    if (n == 1)
        for (i = 0; i < ROM_COUNT && k < max; i++)
            if (!s_slots[i].crc && size_fits(i, b[0].size))
                slots[k++] = i;
    free_blobs(b, n);
    return k;
}

int a800session_rom_import(a800session *s, const char *path, int slot, char *msg, int msgsz)
{
    blob *b;
    int n, imported;
    if (msg && msgsz > 0)
        msg[0] = 0;
    if (load_blobs(path, &b, &n) != 0) {
        if (msg)
            snprintf(msg, (size_t)msgsz, "Could not read %s", path);
        return -1;
    }
    if (slot < 0) {
        imported = import_blobs(s, b, n, msg, msgsz);
        if (!imported && msg)
            snprintf(msg, (size_t)msgsz, "Not a ROM this app recognises");
    }
    else if (slot >= ROM_COUNT || n != 1 || !size_fits(slot, b[0].size)) {
        imported = 0;
        if (msg)
            snprintf(msg, (size_t)msgsz, "That file is not the size a %s is",
                     slot < ROM_COUNT ? s_slots[slot].pub.name : "ROM");
    }
    else if (write_slot(s, slot, b[0].data, b[0].size) == 0) {
        imported = 1;
        append_msg(msg, msgsz, s_slots[slot].pub.name);
    }
    else {
        imported = 0;
        if (msg)
            snprintf(msg, (size_t)msgsz, "Could not write to %s", s->roms_dir);
    }
    free_blobs(b, n);
    if (imported)
        session_roms_changed(s);
    return imported > 0 ? imported : -1;
}

int a800session_media_is_rom(const char *path)
{
    return roms_identify_file(path) >= 0;
}

/* A developer build's embedded images, written out once (WITH_ATARI800_ROMS). */
void roms_provision_embedded(struct a800session *s)
{
    unsigned i;
    int slot;
    for (i = 0; i < a800_embedded_rom_count; i++) {
        for (slot = 0; slot < ROM_COUNT; slot++)
            if (!strcmp(s_slots[slot].pub.key, a800_embedded_roms[i].key))
                break;
        if (slot < ROM_COUNT && !a800session_rom_available(s, slot))
            write_slot(s, slot, a800_embedded_roms[i].data, a800_embedded_roms[i].size);
    }
}
