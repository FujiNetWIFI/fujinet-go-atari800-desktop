/* The FujiNet tab's decoder on made-up NetSIO traffic: reads, writes, NAK,
   no response, bad checksums, replies arriving a byte at a time, repeats
   folding into xN, netstream, clearing and the ring's limit. No ROMs, no
   network. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fuji_trace.h"

#define CHECK(cond, ...) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); exit(1); } } while (0)

static a800_fuji_packet pk[FUJI_TRACE_MAX + 8];

static uint8_t ck(const uint8_t *b, int n)
{
    unsigned s = 0;
    int i;
    for (i = 0; i < n; i++) {
        s += b[i];
        if (s > 255) s = (s & 0xff) + 1;
    }
    return (uint8_t)s;
}

static void bytes(fuji_trace *t, int event, const uint8_t *b, int n, int stream)
{
    int i;
    for (i = 0; i < n; i++)
        fuji_trace_event(t, event, b[i], stream);
}

/* The command line asserted, the frame (checksum added), released. */
static void command(fuji_trace *t, int dev, int cmd, int aux1, int aux2)
{
    uint8_t f[5] = { (uint8_t)dev, (uint8_t)cmd, (uint8_t)aux1, (uint8_t)aux2, 0 };
    f[4] = ck(f, 4);
    fuji_trace_event(t, FUJI_TRACE_CMD_ON, 0, 0);
    bytes(t, FUJI_TRACE_OUT, f, 5, 0);
    fuji_trace_event(t, FUJI_TRACE_CMD_OFF, 0, 0);
}

/* ACK, COMPLETE, n data bytes from seed, their checksum. */
static void read_reply(fuji_trace *t, int n, int seed)
{
    uint8_t buf[300];
    int i;
    buf[0] = 'A';
    buf[1] = 'C';
    for (i = 0; i < n; i++)
        buf[2 + i] = (uint8_t)(seed + i * 7);
    buf[2 + n] = ck(buf + 2, n);
    bytes(t, FUJI_TRACE_IN, buf, n + 3, 0);
}

static int get(fuji_trace *t)
{
    return fuji_trace_get(t, pk, FUJI_TRACE_MAX + 8);
}

static int has(int i, const char *text)
{
    return strstr(pk[i].text, text) != NULL;
}

int main(void)
{
    fuji_trace *t = fuji_trace_new();
    uint8_t buf[300];
    uint64_t out, in, none, cmds, serial;
    int n, i;

    CHECK(t, "new");
    CHECK(get(t) == 0, "empty trace");

    /* a read: GET ADAPTERCONFIG, 140 bytes back */
    command(t, 0x70, 0xE8, 0, 0);
    n = get(t);
    CHECK(n == 1 && pk[0].to_host == 0 && pk[0].nbytes == 5, "frame: %d", n);
    CHECK(has(0, "CMD 70 E8 00 00 [") && has(0, " ok] FujiNet GET ADAPTERCONFIG"), "frame: %s", pk[0].text);
    CHECK(pk[0].device == 0x70 && pk[0].command == 0xE8 && pk[0].repeat == 1, "frame fields");

    /* the reply a byte at a time: the open packet grows */
    fuji_trace_event(t, FUJI_TRACE_IN, 'A', 0);
    n = get(t);
    CHECK(n == 2 && pk[1].to_host == 1 && !strcmp(pk[1].text, "\xe2\x86\x90 ACK"), "ack: %s", pk[1].text);
    serial = pk[1].serial;
    fuji_trace_event(t, FUJI_TRACE_IN, 'C', 0);
    n = get(t);
    CHECK(n == 2 && has(1, "ACK COMPLETE") && pk[1].serial != serial, "complete: %s", pk[1].text);
    buf[0] = 0;
    for (i = 0; i < 140; i++)
        buf[i] = (uint8_t)(i * 3);
    bytes(t, FUJI_TRACE_IN, buf, 70, 0);
    n = get(t);
    CHECK(n == 2 && has(1, "ACK COMPLETE 69 B ck "), "half the data: %s", pk[1].text);
    bytes(t, FUJI_TRACE_IN, buf + 70, 70, 0);
    fuji_trace_event(t, FUJI_TRACE_IN, ck(buf, 140), 0);
    n = get(t);
    CHECK(n == 2 && has(1, "ACK COMPLETE 140 B ck ok"), "read: %s", pk[1].text);
    CHECK(pk[1].nbytes == 143 && pk[1].length == 140 && pk[1].data[0] == 'A' && pk[1].data[2] == 0,
          "read bytes: %d %d", pk[1].nbytes, pk[1].length);
    CHECK(pk[1].device == 0x70 && pk[1].command == 0xE8, "reply carries the command");

    /* a write: D1 WRITE SECTOR 1, ACK, 128 bytes, ACK COMPLETE */
    command(t, 0x31, 0x57, 1, 0);
    fuji_trace_event(t, FUJI_TRACE_IN, 'A', 0);
    for (i = 0; i < 128; i++)
        buf[i] = (uint8_t)i;
    buf[128] = ck(buf, 128);
    bytes(t, FUJI_TRACE_OUT, buf, 129, 0);
    fuji_trace_event(t, FUJI_TRACE_IN, 'A', 0);
    fuji_trace_event(t, FUJI_TRACE_IN, 'C', 0);
    n = get(t);
    CHECK(n == 6, "write: %d packets", n);
    CHECK(has(2, "D1: WRITE SECTOR 1"), "write frame: %s", pk[2].text);
    CHECK(pk[3].to_host == 1 && !strcmp(pk[3].text, "\xe2\x86\x90 ACK"), "write ack: %s", pk[3].text);
    CHECK(pk[4].to_host == 0 && has(4, "DATA 128 B ck ok") && pk[4].length == 128, "write data: %s",
          pk[4].text);
    CHECK(pk[5].to_host == 1 && !strcmp(pk[5].text, "\xe2\x86\x90 ACK COMPLETE"), "write done: %s",
          pk[5].text);

    /* a NAK, then a command nothing answers, then a bad frame checksum */
    command(t, 0x71, 0x4F, 0x0C, 0);
    fuji_trace_event(t, FUJI_TRACE_IN, 'N', 0);
    command(t, 0x35, 0x53, 0, 0);
    fuji_trace_event(t, FUJI_TRACE_CMD_ON, 0, 0);
    buf[0] = 0x70; buf[1] = 0xF2; buf[2] = 0; buf[3] = 0; buf[4] = 0x55;
    bytes(t, FUJI_TRACE_OUT, buf, 5, 0);
    fuji_trace_event(t, FUJI_TRACE_CMD_OFF, 0, 0);
    n = get(t);
    CHECK(n == 10, "nak etc: %d", n);
    CHECK(has(6, "N1: OPEN") && !strcmp(pk[7].text, "\xe2\x86\x90 NAK"), "nak: %s / %s", pk[6].text,
          pk[7].text);
    CHECK(has(8, "D5: STATUS (no response)"), "no response: %s", pk[8].text);
    CHECK(has(9, "[55 bad] FujiNet READ DEVICE SLOTS") && !has(9, "no response"), "bad frame: %s",
          pk[9].text);

    /* status polls: identical exchanges fold into one, xN */
    fuji_trace_clear(t);
    CHECK(get(t) == 0, "cleared");
    for (i = 0; i < 3; i++) {
        command(t, 0x70, 0xFA, 0, 0);
        read_reply(t, 1, 3);
    }
    n = get(t);
    CHECK(n == 2 && has(0, "GET WIFISTATUS \xc3\x97" "3") && pk[0].repeat == 3 && pk[1].repeat == 3,
          "polls: %d %s", n, pk[0].text);
    CHECK(has(1, "ACK COMPLETE 1 B ck ok") && !has(1, "\xc3\x97"), "poll reply: %s", pk[1].text);
    /* half of a fourth shows as itself until it is the same again */
    serial = pk[1].serial;
    command(t, 0x70, 0xFA, 0, 0);
    fuji_trace_event(t, FUJI_TRACE_IN, 'A', 0);
    n = get(t);
    CHECK(n == 4 && has(0, "\xc3\x97" "3"), "half a poll: %d", n);
    fuji_trace_event(t, FUJI_TRACE_IN, 'C', 0);
    fuji_trace_event(t, FUJI_TRACE_IN, 3, 0);
    fuji_trace_event(t, FUJI_TRACE_IN, 3, 0);
    n = get(t);
    CHECK(n == 2 && has(0, "\xc3\x97" "4") && pk[1].serial != serial, "fourth poll: %d %s", n, pk[0].text);
    /* a different reply breaks the run */
    command(t, 0x70, 0xFA, 0, 0);
    read_reply(t, 1, 4);
    command(t, 0x70, 0xFA, 0, 0);
    read_reply(t, 1, 4);
    n = get(t);
    CHECK(n == 4 && has(0, "\xc3\x97" "4") && has(2, "\xc3\x97" "2"), "new run: %d %s", n, pk[2].text);

    /* netstream: raw runs both ways; its command line is not FujiNet's */
    fuji_trace_clear(t);
    fuji_trace_event(t, FUJI_TRACE_CMD_ON, 0, 1);
    bytes(t, FUJI_TRACE_OUT, (const uint8_t *)"ATZ", 3, 1);
    fuji_trace_event(t, FUJI_TRACE_CMD_OFF, 0, 1);
    bytes(t, FUJI_TRACE_IN, (const uint8_t *)"OK", 2, 1);
    fuji_trace_event(t, FUJI_TRACE_IN_NONE, 0xFF, 1);
    n = get(t);
    CHECK(n == 2 && has(0, "\xe2\x86\x92 STREAM 3 B") && has(1, "\xe2\x86\x90 STREAM 2 B"), "stream: %d %s",
          n, n ? pk[0].text : "");
    CHECK(pk[0].data[0] == 'A' && pk[1].data[1] == 'K', "stream bytes");
    /* bytes after a finished exchange are a stream too */
    command(t, 0x50, 0x58, 0, 0);
    bytes(t, FUJI_TRACE_IN, (const uint8_t *)"AC", 2, 0);
    bytes(t, FUJI_TRACE_OUT, (const uint8_t *)"hi", 2, 0);
    n = get(t);
    CHECK(n == 5 && has(2, "R1:") && has(3, "ACK COMPLETE") && has(4, "\xe2\x86\x92 STREAM 2 B"),
          "concurrent: %d %s", n, pk[4].text);
    /* a short frame is not sent */
    fuji_trace_event(t, FUJI_TRACE_CMD_ON, 0, 0);
    bytes(t, FUJI_TRACE_OUT, (const uint8_t *)"\x31\x52", 2, 0);
    fuji_trace_event(t, FUJI_TRACE_CMD_OFF, 0, 0);
    n = get(t);
    CHECK(has(n - 1, "CMD short, not sent (2 B)"), "short: %s", pk[n - 1].text);

    fuji_trace_counts(t, &out, &in, &none, &cmds);
    CHECK(out > 0 && in > 0 && none == 1 && cmds > 0, "counts %llu %llu %llu %llu",
          (unsigned long long)out, (unsigned long long)in, (unsigned long long)none,
          (unsigned long long)cmds);

    /* the ring keeps the newest */
    fuji_trace_clear(t);
    for (i = 0; i < 400; i++) {
        command(t, 0x31, 0x52, i & 0xff, i >> 8);
        read_reply(t, 4, i);
    }
    n = fuji_trace_get(t, pk, FUJI_TRACE_MAX);
    CHECK(n == FUJI_TRACE_MAX, "ring: %d", n);
    CHECK(has(n - 2, "D1: READ SECTOR 399") && has(n - 1, "4 B ck ok"), "newest: %s", pk[n - 2].text);
    CHECK(pk[0].serial < pk[n - 1].serial, "oldest first");
    CHECK(fuji_trace_get(t, pk, 3) == 3 && has(1, "READ SECTOR 399"), "newest 3: %s", pk[1].text);

    fuji_trace_free(t);
    puts("fujitrace: ok");
    return 0;
}
