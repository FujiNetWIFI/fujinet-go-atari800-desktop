/* Tiny Atari programs the tests write themselves: each puts HELLO on the
   GR.0 screen and loops. No third-party bytes. */
#ifndef A800_TEST_FILES_H
#define A800_TEST_FILES_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* LDA SAVMSC / STA $CB ... copy "HELLO" (screen codes) and JMP * */
static void hello_code(uint8_t *p, unsigned org)
{
    unsigned msg = org + 0x17, done = org + 0x14;
    const uint8_t c[] = {
        0xa5, 0x58, 0x85, 0xcb, 0xa5, 0x59, 0x85, 0xcc, 0xa0, 0x00,
        0xb9, (uint8_t)msg, (uint8_t)(msg >> 8), 0xf0, 0x05, 0x91, 0xcb, 0xc8, 0xd0, 0xf6,
        0x4c, (uint8_t)done, (uint8_t)(done >> 8),
        0x28, 0x25, 0x2c, 0x2c, 0x2f, 0x00};
    memcpy(p, c, sizeof c);
}

static int write_hello_xex(const char *path)
{
    uint8_t code[29], hdr[6] = {0xff, 0xff, 0x00, 0x20, 0x00 + 28, 0x20};
    const uint8_t run[6] = {0xe0, 0x02, 0xe1, 0x02, 0x00, 0x20};
    FILE *fp = fopen(path, "wb");
    if (!fp)
        return -1;
    hello_code(code, 0x2000);
    fwrite(hdr, 1, 6, fp);
    fwrite(code, 1, 29, fp);
    fwrite(run, 1, 6, fp);
    return fclose(fp);
}

/* A 90K single-density ATR whose boot sector is the hello program. */
static int write_hello_atr(const char *path)
{
    static uint8_t img[16 + 720 * 128];
    uint8_t *boot = img + 16;
    FILE *fp;
    memset(img, 0, sizeof img);
    img[0] = 0x96; img[1] = 0x02;
    img[2] = (uint8_t)((720 * 128 / 16) & 0xff); img[3] = (uint8_t)((720 * 128 / 16) >> 8);
    img[4] = 128;
    /* flags, 1 sector, load at $0700, init $071A; code runs at $0706 */
    boot[0] = 0; boot[1] = 1; boot[2] = 0x00; boot[3] = 0x07; boot[4] = 0x1a; boot[5] = 0x07;
    hello_code(boot + 6, 0x0706);
    fp = fopen(path, "wb");
    if (!fp)
        return -1;
    fwrite(img, 1, sizeof img, fp);
    return fclose(fp);
}

/* A .car with a 5200 type. */
static int write_5200_car(const char *path)
{
    static uint8_t img[16 + 32768];
    FILE *fp;
    memset(img, 0, sizeof img);
    memcpy(img, "CART", 4);
    img[7] = 4;     /* CARTRIDGE_5200_32 */
    fp = fopen(path, "wb");
    if (!fp)
        return -1;
    fwrite(img, 1, sizeof img, fp);
    return fclose(fp);
}

/* A raw 8K ROM: ambiguous, the user picks a type. */
static int write_raw_8k(const char *path)
{
    static uint8_t img[8192];
    FILE *fp;
    memset(img, 0xea, sizeof img);
    fp = fopen(path, "wb");
    if (!fp)
        return -1;
    fwrite(img, 1, sizeof img, fp);
    return fclose(fp);
}

#endif
