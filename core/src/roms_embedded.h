/* ROM images a developer build embeds (WITH_ATARI800_ROMS; never shipped). */
#ifndef A800_ROMS_EMBEDDED_H
#define A800_ROMS_EMBEDDED_H

#include <stdint.h>

typedef struct {
    const char *key;        /* the ROM slot key, e.g. "os_bb01r2" */
    const uint8_t *data;
    uint32_t size;
} a800_embedded_rom;

extern const a800_embedded_rom a800_embedded_roms[];
extern const unsigned a800_embedded_rom_count;

#endif
