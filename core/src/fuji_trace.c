/*
 * fuji_trace.c -- NetSIO's bytes into SIO packets for the FujiNet tab.
 *
 * An exchange is the command frame (the bytes while the command line is
 * asserted), then the replies, split where the direction turns: ACK and,
 * for a read, COMPLETE, data and checksum; for a write, ACK, the data and
 * checksum, then ACK COMPLETE. Anything else (netstream, an 850's
 * concurrent mode) is a stream run. An exchange identical to the one
 * before it folds into it as "xN".
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fuji_trace.h"
#include "fuji_names.h"

#define ARROW_OUT "\xe2\x86\x92"
#define ARROW_IN "\xe2\x86\x90"
#define EX_MAX 4                /* CMD, ACK, DATA, ACK COMPLETE */

enum { PK_CMD, PK_REPLY, PK_WRITE, PK_STREAM };

typedef struct {
    a800_fuji_packet p;
    int kind;
    uint64_t hash;              /* every byte, to spot a repeat */
    uint8_t first[2];
    int dstart;                 /* where the data starts (after ACK, COMPLETE) */
    uint8_t sum, last;          /* the data's checksum but for the last byte; the last */
    int no_response;
} pkt;

struct fuji_trace {
    pkt ring[FUJI_TRACE_MAX];
    uint64_t total;             /* packets ever committed */
    int count;
    uint64_t serial;
    pkt frame;                  /* the command frame being sent */
    int in_frame;
    pkt cur[EX_MAX];            /* the exchange (or stream run) going on */
    int ncur;
    uint64_t prev_first;        /* the last exchange committed */
    int prev_n;
    uint64_t n_out, n_in, n_none, n_cmd;
};

const char *sio_command_name(int dev, int cmd)
{
    if (dev >= 0x31 && dev <= 0x38) {
        switch (cmd) {
        case 0x52: return "READ SECTOR";
        case 0x50: return "PUT SECTOR";
        case 0x57: return "WRITE SECTOR";
        case 0x53: return "STATUS";
        case 0x21: return "FORMAT";
        case 0x22: return "FORMAT ENHANCED";
        case 0x4e: return "READ PERCOM";
        case 0x4f: return "WRITE PERCOM";
        case 0x3f: return "HSIO INDEX";
        default: break;
        }
    }
    if (dev == 0x70)
        return "FUJINET";
    if (dev >= 0x71 && dev <= 0x78)
        return "NETWORK";
    return "";
}

static const char *fuji_command_name(int id)
{
    size_t i;
    for (i = 0; i < sizeof fuji_command_names / sizeof fuji_command_names[0]; i++)
        if (fuji_command_names[i].id == id)
            return fuji_command_names[i].name;
    return "";
}

/* N: commands, from fujinet-firmware include/fujiCommandID.h (NET_*). */
static const char *net_command_name(int id)
{
    switch (id) {
    case 0x4F: return "OPEN";
    case 0x43: return "CLOSE";
    case 0x52: return "READ";
    case 0x57: return "WRITE";
    case 0x53: return "STATUS";
    case 0x50: return "PARSE";
    case 0x51: return "QUERY";
    case 0x45: return "GET ERROR";
    case 0x2C: return "CHDIR";
    case 0x30: return "GETCWD";
    case 0x21: return "DELETE";
    case 0x20: return "RENAME";
    case 0xFC: return "SET PARSER";
    case 0xFF: return "GET DSTATS";
    default: return "";
    }
}

static uint8_t chk_add(uint8_t sum, uint8_t b)
{
    unsigned s = (unsigned)sum + b;
    return (uint8_t)(s > 255 ? (s & 0xff) + 1 : s);
}

static int is_status(uint8_t b)
{
    return b == 'A' || b == 'N' || b == 'C' || b == 'E';
}

static const char *status_word(uint8_t b)
{
    switch (b) {
    case 'A': return "ACK";
    case 'N': return "NAK";
    case 'C': return "COMPLETE";
    case 'E': return "ERROR";
    default: return "";
    }
}

static void device_name(char *dst, size_t sz, int dev, int cmd, int aux)
{
    const char *name;
    if (dev >= 0x31 && dev <= 0x3F) {
        name = sio_command_name(0x31, cmd);
        if (cmd == 0x52 || cmd == 0x50 || cmd == 0x57)
            snprintf(dst, sz, "D%d: %s %d", dev - 0x30, name, aux);
        else if (*name)
            snprintf(dst, sz, "D%d: %s", dev - 0x30, name);
        else
            snprintf(dst, sz, "D%d: $%02X", dev - 0x30, cmd);
    }
    else if (dev == 0x70) {
        name = fuji_command_name(cmd);
        if (*name)
            snprintf(dst, sz, "FujiNet %s", name);
        else
            snprintf(dst, sz, "FujiNet $%02X", cmd);
    }
    else if (dev >= 0x71 && dev <= 0x78) {
        name = net_command_name(cmd);
        if (*name)
            snprintf(dst, sz, "N%d: %s", dev - 0x70, name);
        else
            snprintf(dst, sz, "N%d: $%02X", dev - 0x70, cmd);
    }
    else if (dev >= 0x40 && dev <= 0x43)
        snprintf(dst, sz, "P%d:", dev - 0x3F);
    else if (dev >= 0x50 && dev <= 0x53)
        snprintf(dst, sz, "R%d:", dev - 0x4F);
    else
        dst[0] = '\0';
}

/* The packet's text line, shown repeat times in a row. */
static void retext(pkt *k, uint32_t repeat)
{
    char *t = k->p.text;
    const size_t sz = sizeof k->p.text;
    const int n = k->p.nbytes, rest = n - k->dstart;
    const char *ck = k->sum == k->last ? "ok" : "bad";
    size_t len;

    switch (k->kind) {
    case PK_CMD:
        if (n < 5)
            snprintf(t, sz, ARROW_OUT " CMD short, not sent (%d B)", n);
        else {
            char name[48];
            device_name(name, sizeof name, k->p.data[0], k->p.data[1],
                        k->p.data[2] | (k->p.data[3] << 8));
            snprintf(t, sz, ARROW_OUT " CMD %02X %02X %02X %02X [%02X %s] %s", k->p.data[0],
                     k->p.data[1], k->p.data[2], k->p.data[3], k->p.data[4], ck, name);
            if (n > 5) {
                len = strlen(t);
                snprintf(t + len, sz - len, " +%d B", n - 5);
            }
        }
        break;
    case PK_REPLY:
        snprintf(t, sz, ARROW_IN);
        if (k->dstart >= 1) {
            len = strlen(t);
            snprintf(t + len, sz - len, " %s", status_word(k->first[0]));
        }
        if (k->dstart >= 2) {
            len = strlen(t);
            snprintf(t + len, sz - len, " %s", status_word(k->first[1]));
        }
        len = strlen(t);
        if (rest >= 2)
            snprintf(t + len, sz - len, " %d B ck %s", rest - 1, ck);
        else if (rest == 1)
            snprintf(t + len, sz - len, " $%02X", k->last);
        break;
    case PK_WRITE:
        snprintf(t, sz, ARROW_OUT " DATA %d B ck %s", n - 1, ck);
        break;
    default:
        snprintf(t, sz, "%s STREAM %d B", k->p.to_host ? ARROW_IN : ARROW_OUT, n);
        break;
    }
    len = strlen(t);
    if (k->no_response)
        snprintf(t + len, sz - len, " (no response)");
    len = strlen(t);
    if (repeat > 1)
        snprintf(t + len, sz - len, " \xc3\x97%u", repeat);
}

static void start(pkt *k, int kind, int to_host, int device, int command)
{
    memset(k, 0, sizeof *k);
    k->kind = kind;
    k->p.to_host = to_host;
    k->p.device = device;
    k->p.command = command;
    k->p.length = -1;
    k->p.repeat = 1;
    k->hash = 0xcbf29ce484222325ull;
}

static void append(fuji_trace *t, pkt *k, uint8_t b)
{
    const int i = k->p.nbytes;
    if (i < (int)sizeof k->p.data)
        k->p.data[i] = b;
    k->p.nbytes++;
    k->hash = (k->hash ^ b) * 0x100000001b3ull;
    if (i < 2)
        k->first[i] = b;
    if (k->kind == PK_REPLY) {
        if (i == 0)
            k->dstart = is_status(b) ? 1 : 0;
        else if (i == 1 && k->first[0] == 'A' && (b == 'C' || b == 'E'))
            k->dstart = 2;
    }
    if (i >= k->dstart) {
        if (i > k->dstart)
            k->sum = chk_add(k->sum, k->last);
        k->last = b;
    }
    if (k->kind == PK_REPLY || k->kind == PK_WRITE)
        k->p.length = k->p.nbytes - k->dstart >= 2 ? k->p.nbytes - k->dstart - 1 : -1;
    k->p.serial = ++t->serial;
    retext(k, 1);
}

static pkt *ring_at(fuji_trace *t, uint64_t i)
{
    return &t->ring[i % FUJI_TRACE_MAX];
}

static void push(fuji_trace *t, const pkt *k)
{
    *ring_at(t, t->total) = *k;
    t->total++;
    if (t->count < FUJI_TRACE_MAX)
        t->count++;
}

static int same(const pkt *a, const pkt *b)
{
    return a->kind == b->kind && a->p.to_host == b->p.to_host && a->p.nbytes == b->p.nbytes &&
           a->hash == b->hash;
}

/* The exchange going on is byte for byte the last one committed. */
static int repeats_prev(fuji_trace *t)
{
    int i;
    if (t->ncur == 0 || t->cur[0].kind != PK_CMD || t->ncur != t->prev_n ||
        t->prev_first < t->total - (uint64_t)t->count)
        return 0;
    for (i = 0; i < t->ncur; i++)
        if (!same(ring_at(t, t->prev_first + (uint64_t)i), &t->cur[i]))
            return 0;
    return 1;
}

static void commit(fuji_trace *t)
{
    int i;
    if (t->ncur == 0)
        return;
    if (t->cur[0].kind == PK_CMD && t->ncur == 1 && t->cur[0].p.nbytes >= 5) {
        t->cur[0].no_response = 1;
        retext(&t->cur[0], 1);
    }
    if (repeats_prev(t)) {
        for (i = 0; i < t->prev_n; i++) {
            pkt *k = ring_at(t, t->prev_first + (uint64_t)i);
            k->p.repeat++;
            k->p.serial = ++t->serial;
            if (i == 0)
                retext(k, k->p.repeat);
        }
    }
    else if (t->cur[0].kind == PK_CMD) {
        t->prev_first = t->total;
        t->prev_n = t->ncur;
        for (i = 0; i < t->ncur; i++)
            push(t, &t->cur[i]);
    }
    else {
        for (i = 0; i < t->ncur; i++)
            push(t, &t->cur[i]);
        t->prev_n = 0;
    }
    t->ncur = 0;
}

/* A byte outside the command frame. */
static void data_byte(fuji_trace *t, int to_host, uint8_t b, int stream)
{
    pkt *open = t->ncur ? &t->cur[t->ncur - 1] : NULL;
    int kind = PK_STREAM;

    if (!stream && open && open->kind != PK_CMD && open->p.to_host == to_host) {
        append(t, open, b);
        return;
    }
    if (stream && open && open->kind == PK_STREAM && open->p.to_host == to_host) {
        append(t, open, b);
        return;
    }
    /* a new run: the exchange's next step, or a stream run */
    if (!stream && open && t->cur[0].kind == PK_CMD) {
        if (open->kind == PK_CMD && to_host && open->p.nbytes >= 5)
            kind = PK_REPLY;
        else if (open->kind == PK_REPLY && !to_host && t->ncur == 2 && open->p.nbytes == 1 &&
                 open->first[0] == 'A')
            kind = PK_WRITE;
        else if (open->kind == PK_WRITE && to_host)
            kind = PK_REPLY;
    }
    if (kind == PK_STREAM) {
        commit(t);
        start(&t->cur[0], PK_STREAM, to_host, -1, -1);
        t->ncur = 1;
    }
    else {
        start(&t->cur[t->ncur], kind, to_host, t->cur[0].p.device, t->cur[0].p.command);
        t->ncur++;
    }
    append(t, &t->cur[t->ncur - 1], b);
}

fuji_trace *fuji_trace_new(void)
{
    return calloc(1, sizeof(fuji_trace));
}

void fuji_trace_free(fuji_trace *t)
{
    free(t);
}

void fuji_trace_event(fuji_trace *t, int event, int byte, int stream)
{
    const uint8_t b = (uint8_t)byte;
    switch (event) {
    case FUJI_TRACE_IN_NONE:
        t->n_none++;
        return;
    case FUJI_TRACE_CMD_ON:
        if (stream)
            return;     /* netstream: the line is not passed on */
        commit(t);
        start(&t->frame, PK_CMD, 0, -1, -1);
        t->in_frame = 1;
        return;
    case FUJI_TRACE_CMD_OFF:
        if (stream || !t->in_frame)
            return;
        t->in_frame = 0;
        if (t->frame.p.nbytes == 0)
            return;
        commit(t);
        t->n_cmd++;
        t->cur[0] = t->frame;
        t->cur[0].p.serial = ++t->serial;
        t->ncur = 1;
        return;
    case FUJI_TRACE_OUT:
        t->n_out++;
        if (t->in_frame && !stream) {
            append(t, &t->frame, b);
            t->frame.p.device = t->frame.p.data[0];
            t->frame.p.command = t->frame.p.nbytes >= 2 ? t->frame.p.data[1] : -1;
            return;
        }
        data_byte(t, 0, b, stream);
        return;
    case FUJI_TRACE_IN:
        t->n_in++;
        data_byte(t, 1, b, stream);
        return;
    default:
        return;
    }
}

int fuji_trace_get(fuji_trace *t, a800_fuji_packet *out, int max)
{
    const int fold = repeats_prev(t);
    const int total = t->count + (fold ? 0 : t->ncur);
    const int n = total < max ? total : max;
    const uint64_t oldest = t->total - (uint64_t)t->count;
    int i, j = 0;

    for (i = total - n; i < total; i++) {
        if (i < t->count) {
            const uint64_t at = oldest + (uint64_t)i;
            pkt *k = ring_at(t, at);
            out[j] = k->p;
            /* the exchange going on, shown as one more of the last */
            if (fold && at >= t->prev_first) {
                pkt shown = *k;
                shown.p.repeat++;
                retext(&shown, at == t->prev_first ? shown.p.repeat : 1);
                out[j] = shown.p;
            }
        }
        else
            out[j] = t->cur[i - t->count].p;
        j++;
    }
    if (fold && j > 0)
        out[j - 1].serial = t->serial;
    return j;
}

void fuji_trace_clear(fuji_trace *t)
{
    t->total = 0;
    t->count = 0;
    t->ncur = 0;
    t->prev_n = 0;
    t->in_frame = 0;
}

void fuji_trace_counts(fuji_trace *t, uint64_t *out, uint64_t *in, uint64_t *none,
                       uint64_t *commands)
{
    if (out) *out = t->n_out;
    if (in) *in = t->n_in;
    if (none) *none = t->n_none;
    if (commands) *commands = t->n_cmd;
}
