/* ROM slots: recognised images by CRC (forged filler, no Atari bytes), the
   XEGS's combined chip, a zip, unknown images by size, and the Preferences
   choices that light up once imported. */
#include "test_util.h"
#include <stdint.h>

static uint32_t table[256];

static void crc_init(void)
{
    uint32_t i, j, c;
    for (i = 0; i < 256; i++) {
        for (c = i, j = 0; j < 8; j++)
            c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
        table[i] = c;
    }
}

static uint32_t crc_reg(const uint8_t *p, size_t n, uint32_t reg)
{
    while (n--)
        reg = table[(reg ^ *p++) & 0xff] ^ (reg >> 8);
    return reg;
}

/* Fills p[0..n) with filler whose CRC-32 is want (the last 4 bytes chosen;
   CRC-32 is linear). */
static void forge(uint8_t *p, size_t n, uint32_t want, unsigned seed)
{
    size_t i;
    uint32_t reg0, r;
    int k, j;
    for (i = 0; i < n - 4; i++)
        p[i] = (uint8_t)((i * 31 + seed * 7) & 0xff);
    reg0 = crc_reg(p, n - 4, 0xffffffffu);
    r = ~want;
    for (k = 0; k < 4; k++) {
        for (j = 0; j < 256 && (table[j] >> 24) != (r >> 24); j++)
            ;
        r = ((r ^ table[j]) << 8) | (uint32_t)j;
    }
    r ^= reg0;
    for (k = 0; k < 4; k++)
        p[n - 4 + k] = (uint8_t)(r >> (8 * k));
    CHECK((~crc_reg(p, n, 0xffffffffu)) == want, "forge %08x", want);
}

static void write_file(const char *path, const uint8_t *p, size_t n)
{
    FILE *fp = fopen(path, "wb");
    CHECK(fp && fwrite(p, 1, n, fp) == n, "write %s", path);
    fclose(fp);
}

static void put16(uint8_t *p, unsigned v) { p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; }
static void put32(uint8_t *p, uint32_t v) { put16(p, v & 0xffff); put16(p + 2, v >> 16); }

/* A one-entry zip, stored. */
static void write_zip(const char *path, const char *name, const uint8_t *d, uint32_t n)
{
    uint8_t lh[30], ch[46], eo[22];
    uint32_t crc = ~crc_reg(d, n, 0xffffffffu), nl = (uint32_t)strlen(name);
    FILE *fp = fopen(path, "wb");
    memset(lh, 0, sizeof lh); memset(ch, 0, sizeof ch); memset(eo, 0, sizeof eo);
    put32(lh, 0x04034b50); put16(lh + 4, 10); put32(lh + 14, crc); put32(lh + 18, n);
    put32(lh + 22, n); put16(lh + 26, nl);
    put32(ch, 0x02014b50); put16(ch + 4, 20); put16(ch + 6, 10); put32(ch + 16, crc);
    put32(ch + 20, n); put32(ch + 24, n); put16(ch + 28, nl); put32(ch + 42, 0);
    put32(eo, 0x06054b50); put16(eo + 8, 1); put16(eo + 10, 1); put32(eo + 12, 46 + nl);
    put32(eo + 16, 30 + nl + n);
    fwrite(lh, 1, 30, fp); fwrite(name, 1, nl, fp); fwrite(d, 1, n, fp);
    fwrite(ch, 1, 46, fp); fwrite(name, 1, nl, fp); fwrite(eo, 1, 22, fp);
    fclose(fp);
}

static int slot_by_key(const char *key)
{
    int i;
    for (i = 0; i < a800session_rom_slot_count(); i++)
        if (!strcmp(a800session_rom_slot(i)->key, key))
            return i;
    return -1;
}

int main(void)
{
    a800session *s = test_session("roms");
    static uint8_t os[16384], xegs[32768], basic[8192], four[4096];
    char path[1100], msg[512];
    int slots[16], n, i, osrev, found;

    crc_init();
    CHECK(s, "new");

    /* a recognised OS */
    forge(os, sizeof os, 0x1f9cd270, 1);
    snprintf(path, sizeof path, "%s/os.rom", a800session_data_path(s));
    write_file(path, os, sizeof os);
    n = a800session_rom_probe(s, path, slots, 16);
    CHECK(n == 1 && slots[0] == slot_by_key("os_bb01r2"), "probe OS rev 2: %d", n);
    CHECK(a800session_rom_import(s, path, -1, msg, sizeof msg) == 1, "import: %s", msg);
    CHECK(a800session_rom_available(s, slot_by_key("os_bb01r2")), "rev 2 available");

    /* ... and Preferences now offers it on an XL */
    test_set_pref(s, "machine", A800_M_800XL);
    osrev = a800session_pref_find("os_rev");
    for (i = 0, found = 0; a800session_pref_choice(s, osrev, i); i++)
        if (strstr(a800session_pref_choice(s, osrev, i), "Rev. 2")) {
            CHECK(a800session_pref_choice_enabled(s, osrev, i), "rev 2 choice enabled");
            found = 1;
        }
        else if (strstr(a800session_pref_choice(s, osrev, i), "Rev. 3 ("))
            CHECK(!a800session_pref_choice_enabled(s, osrev, i), "rev 3 greyed");
    CHECK(found, "rev 2 listed");

    /* the XEGS's combined chip: game, BASIC C, OS rev. 4 */
    forge(xegs, 8192, 0xbdca01fb, 2);
    forge(xegs + 8192, 8192, 0x7d684184, 3);
    forge(xegs + 16384, 16384, 0x1eaf4002, 4);
    snprintf(path, sizeof path, "%s/xegs.rom", a800session_data_path(s));
    write_file(path, xegs, sizeof xegs);
    CHECK(a800session_rom_import(s, path, -1, msg, sizeof msg) == 3, "XEGS split: %s", msg);
    CHECK(a800session_rom_available(s, slot_by_key("xegame"))
          && a800session_rom_available(s, slot_by_key("basic_c"))
          && a800session_rom_available(s, slot_by_key("os_bb01r4")), "XEGS parts");

    /* from a zip */
    forge(basic, sizeof basic, 0xf0202fb3, 5);
    snprintf(path, sizeof path, "%s/a800xl.zip", a800session_data_path(s));
    write_zip(path, "co60302a.rom", basic, sizeof basic);
    CHECK(a800session_rom_import(s, path, -1, msg, sizeof msg) == 1, "zip: %s", msg);
    CHECK(a800session_rom_available(s, slot_by_key("basic_b")), "BASIC B from zip");

    /* an unknown 4K image: the 4K peripheral slots, the user picks */
    for (i = 0; i < (int)sizeof four; i++)
        four[i] = (uint8_t)(i ^ 0x5a);
    snprintf(path, sizeof path, "%s/af80.rom", a800session_data_path(s));
    write_file(path, four, sizeof four);
    n = a800session_rom_probe(s, path, slots, 16);
    CHECK(n == 5, "4K candidates %d", n);
    CHECK(a800session_rom_import(s, path, -1, msg, sizeof msg) < 0, "unknown not auto-imported");
    CHECK(a800session_rom_import(s, path, slot_by_key("af80"), msg, sizeof msg) == 1, "as AF80: %s", msg);
    CHECK(a800session_rom_import(s, path, slot_by_key("mio"), msg, sizeof msg) < 0, "size checked");

    /* the imported OS boots (it is filler, so just that the machine comes up
       with it chosen and the session survives) */
    a800session_pref_set_choice(s, osrev, 2);
    test_start(s, 0);
    test_sleep_ms(300);
    a800session_stop(s);

    a800session_free(s);
    puts("roms: ok");
    return 0;
}
