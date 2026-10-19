/*
 * a800debug -- the debugger engine every frontend's debugger window drives:
 * atari800's 6502 stopped and stepped, its memory, ANTIC, GTIA, POKEY, PIA,
 * the memory map and cartridge, breakpoints and watchpoints, the SIO
 * command trace, FujiNet's NetSIO traffic, and atari800's own monitor as a
 * console.
 *
 * Attaching stops the machine; detaching lets it run (breakpoints stay,
 * disarmed). Calls are safe from any thread.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef A800DEBUG_H
#define A800DEBUG_H

#include <stdint.h>

#include "a800session.h"

#ifdef __cplusplus
extern "C" {
#endif

a800debug *a800debug_get(a800session *s);

void a800debug_attach(a800debug *d);
void a800debug_detach(a800debug *d);
int  a800debug_is_attached(a800debug *d);
int  a800debug_is_stopped(a800debug *d);
void a800debug_stop(a800debug *d);          /* break before the next instruction */
void a800debug_resume(a800debug *d);        /* F5 */
/* Why it stopped ("Breakpoint 2", "Step", "BRK", "CIM" ...); the PC. */
int  a800debug_stop_reason(a800debug *d, char *dst, int dstsz, int *address);
/* Moves on every stop and resume (frontends refresh on a change). */
unsigned a800debug_generation(a800debug *d);

/* atari800's monitor: one command line; its output into dst. Returns
   A800DBG_CMD_OK, _RESUMED (the command let the machine go: G, R, O,
   CONT) or _NEEDS_INPUT (it is asking for more: the A assembler). */
enum { A800DBG_CMD_OK = 0, A800DBG_CMD_RESUMED = 1, A800DBG_CMD_NEEDS_INPUT = 2 };
int  a800debug_command(a800debug *d, const char *line, char *dst, int dstsz);
/* The monitor commands starting with prefix, space-separated; returns the
   text's length. */
int  a800debug_completions(a800debug *d, const char *prefix, char *dst, int dstsz);

void a800debug_step(a800debug *d);          /* F7: one instruction */
void a800debug_step_over(a800debug *d);     /* F8: over a JSR or branch */
void a800debug_step_out(a800debug *d);      /* Shift+F8: to the RTS/RTI */
void a800debug_frame(a800debug *d);         /* to the next vertical blank */
void a800debug_run_to(a800debug *d, uint16_t addr);
void a800debug_break_on_brk(a800debug *d, int on);
int  a800debug_breaks_on_brk(a800debug *d);

/* ---- the CPU ------------------------------------------------------------- */
typedef struct {
    int pc, sp, a, x, y, p;
    int n, v, d, i, z, c;
    int scanline, cycle;        /* ANTIC's ypos and xpos */
    uint32_t frame;
} a800debug_cpu;
void a800debug_cpu_get(a800debug *d, a800debug_cpu *out);
typedef enum { A800_REG_PC, A800_REG_SP, A800_REG_A, A800_REG_X, A800_REG_Y, A800_REG_P,
               A800_FLAG_N, A800_FLAG_V, A800_FLAG_D, A800_FLAG_I, A800_FLAG_Z,
               A800_FLAG_C } a800debug_reg;
void a800debug_cpu_set(a800debug *d, int reg, int value);

/* The last instructions executed, oldest first. */
typedef struct {
    uint16_t pc;
    int scanline, cycle;
    char disasm[64];
} a800debug_history;
int  a800debug_history_get(a800debug *d, a800debug_history *out, int max);

/* ---- memory and code ------------------------------------------------------ */
/* As the CPU sees it (banking applied; no side effects on hardware). */
int  a800debug_read(a800debug *d, uint16_t addr, uint8_t *dst, int n);
void a800debug_write(a800debug *d, uint16_t addr, uint8_t value);

typedef struct {
    uint16_t address;
    int is_pc;
    int has_breakpoint;         /* 1 enabled, 2 disabled */
    char bytes[16];
    char label[48];
    char disasm[64];
    char comment[64];
} a800debug_line;
int  a800debug_disassemble(a800debug *d, uint16_t addr, a800debug_line *out, int max,
                           int *pc_line);
/* An address rows instructions before (rows < 0) or after addr. */
int  a800debug_row_address(a800debug *d, uint16_t addr, int rows);
int  a800debug_label_address(a800debug *d, const char *label);
int  a800debug_address_label(a800debug *d, uint16_t addr, char *dst, int dstsz);
/* Labels from a ca65/ld65 (-Ln) or VICE .lbl, MADS .lab or atari800 label
   file. */
int  a800debug_load_symbols(a800debug *d, const char *path, char *msg, int msgsz);

/* ---- ANTIC --------------------------------------------------------------- */
typedef struct {
    uint8_t dmactl, chactl, hscrol, vscrol, pmbase, chbase, nmien, nmist;
    uint16_t dlist;             /* DLISTL/H */
    int scanline, cycle, vcount;
    int playfield_width;        /* 0 none, 1 narrow, 2 normal, 3 wide */
    int dl_dma, pm_dma, missile_dma, pm_single_line;
} a800debug_antic;
void a800debug_antic_get(a800debug *d, a800debug_antic *out);
/* The display list, one instruction a line ("$9C20  LMS 4  $9C40 ..."). */
int  a800debug_dlist_text(a800debug *d, char *dst, int dstsz);

/* ---- GTIA ---------------------------------------------------------------- */
typedef struct {
    uint8_t hposp[4], hposm[4], sizep[4], sizem, grafp[4], grafm;
    uint8_t colpm[4], colpf[4], colbk, prior, vdelay, gractl, consol;
    uint8_t mpf[4], ppf[4], mpl[4], ppl[4];   /* collisions */
    uint8_t trig[4];
} a800debug_gtia;
void a800debug_gtia_get(a800debug *d, a800debug_gtia *out);
uint32_t a800debug_color(a800debug *d, uint8_t index);     /* XRGB */
#define A800DEBUG_PALETTE_WIDTH  256
#define A800DEBUG_PALETTE_HEIGHT 64
/* The 256 colours, 16 hues down and 16 lumas across. */
int  a800debug_palette_image(a800debug *d, uint32_t *dst);

/* ---- POKEY and PIA ------------------------------------------------------- */
typedef struct {
    uint8_t audf[4], audc[4], audctl;
    uint8_t kbcode, skstat, skctl, irqen, irqst, random;
    uint8_t pot[8];
    int second;                 /* a stereo POKEY's second chip */
} a800debug_pokey;
void a800debug_pokey_get(a800debug *d, int chip, a800debug_pokey *out);
typedef struct {
    uint8_t porta, portb, pactl, pbctl;
    uint8_t porta_mask, portb_mask;
    int ca1, ca2, cb1, cb2, irq;
} a800debug_pia;
void a800debug_pia_get(a800debug *d, a800debug_pia *out);

/* ---- the memory map and the cartridge ------------------------------------- */
typedef struct {
    int xl;                     /* XL/XE banking (PORTB) */
    uint8_t portb;
    int os_rom, basic_rom, selftest, xe_cpu_bank, xe_antic_bank;
    int ram_kb;
    char machine[40];
    int cart_type;              /* 0 none */
    char cart_name[96];
    char cart_desc[64];
    int cart_bank;
    int piggyback_type;
    char piggyback_name[96];
} a800debug_map;
void a800debug_map_get(a800debug *d, a800debug_map *out);

/* ---- breakpoints and watchpoints ------------------------------------------ */
typedef enum { A800DEBUG_BP_EXEC = 1, A800DEBUG_BP_READ = 2, A800DEBUG_BP_WRITE = 4 } a800debug_bp_type;
typedef struct {
    int id;                     /* breakpoints 1..; watchpoints 1001.. */
    int type;                   /* a800debug_bp_type bits */
    uint16_t start, end;
    int enabled;
    char condition[96];         /* "A=$10 && X<4" ("" none) */
} a800debug_breakpoint;
int  a800debug_breakpoint_toggle(a800debug *d, uint16_t addr);
int  a800debug_breakpoint_check(a800debug *d, uint16_t addr);
/* The new id, or -1 with the reason in msg (a bad condition, or atari800's
   table of 20 conditions full). */
int  a800debug_breakpoint_add(a800debug *d, int type, uint16_t start, uint16_t end,
                              const char *condition, char *msg, int msgsz);
void a800debug_breakpoint_remove(a800debug *d, int id);
void a800debug_breakpoint_enable(a800debug *d, int id, int enabled);
int  a800debug_breakpoint_list(a800debug *d, a800debug_breakpoint *out, int max);
void a800debug_breakpoint_clear(a800debug *d);
/* Entries of atari800's condition table in use, of max. */
int  a800debug_breakpoint_budget(a800debug *d, int *max);

/* ---- the SIO command trace ------------------------------------------------ */
typedef struct {
    uint32_t frame;
    uint8_t device, command, aux1, aux2;
    char path[8];               /* "NetSIO", "serial", "patch" */
    char text[48];              /* "D1: READ SECTOR 361" */
} a800debug_sio;
int  a800debug_sio_trace(a800debug *d, a800debug_sio *out, int max);
void a800debug_sio_clear(a800debug *d);

/* ---- FujiNet: every NetSIO byte, as SIO packets (always on) --------------- */
typedef struct {
    uint64_t serial;
    int      to_host;       /* 1: FujiNet -> Atari, 0: Atari -> FujiNet */
    int      device, command;   /* the exchange's command frame, -1 none */
    int      length;        /* data bytes (checksum not counted), -1 none */
    int      nbytes;        /* bytes on the wire */
    uint8_t  data[64];      /* their first bytes */
    char     text[96];      /* "-> CMD 70 E8 00 00 [59 ok] FujiNet GET ADAPTERCONFIG" */
    uint32_t repeat;        /* the same exchange this many times in a row */
} a800_fuji_packet;
int  a800debug_fujinet_trace(a800debug *d, a800_fuji_packet *out, int max);  /* oldest first */
void a800debug_fujinet_trace_clear(a800debug *d);
/* NetSIO's state and POKEY's serial port, as text lines. */
int  a800debug_fujinet_state(a800debug *d, char *dst, int dstsz);

/* ---- snapshots (temporary save states) ------------------------------------ */
/* Slots 0-3. With FujiNet on, restoring replays SIO traffic FujiNet has
   already acted on: the frontends say so. */
int  a800debug_snapshot_save(a800debug *d, int slot);
int  a800debug_snapshot_restore(a800debug *d, int slot);
int  a800debug_snapshot_exists(a800debug *d, int slot);

/* "dis" (the disassembly window), "ram" (64K as the CPU sees it). */
int  a800debug_save(a800debug *d, const char *kind, const char *path, char *msg, int msgsz);

#ifdef __cplusplus
}
#endif

#endif
