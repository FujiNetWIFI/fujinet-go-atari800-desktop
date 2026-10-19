/* The FujiNet tab's decoder: NetSIO's bytes, both ways, into SIO packets
 * (command frame, ACK/NAK, COMPLETE/ERROR, data and checksum). No emulator
 * in here: debug.cpp feeds it from atari800's SIO_byte_hook, the tests from
 * made-up byte streams. Not thread-safe (the caller locks). */
#ifndef A800_FUJI_TRACE_H
#define A800_FUJI_TRACE_H

#include "a800debug.h"

#ifdef __cplusplus
extern "C" {
#endif

/* events: atari800's SIO_BYTE_* */
enum { FUJI_TRACE_OUT, FUJI_TRACE_IN, FUJI_TRACE_IN_NONE, FUJI_TRACE_CMD_ON, FUJI_TRACE_CMD_OFF };

#define FUJI_TRACE_MAX 512

typedef struct fuji_trace fuji_trace;

fuji_trace *fuji_trace_new(void);
void fuji_trace_free(fuji_trace *t);
/* stream: netstream's raw bytes (no command frames) */
void fuji_trace_event(fuji_trace *t, int event, int byte, int stream);
/* The newest max packets, oldest first; the exchange still going is last. */
int  fuji_trace_get(fuji_trace *t, a800_fuji_packet *out, int max);
void fuji_trace_clear(fuji_trace *t);
/* Bytes each way, reads that found nothing, command frames: since start. */
void fuji_trace_counts(fuji_trace *t, uint64_t *out, uint64_t *in, uint64_t *none,
                       uint64_t *commands);

/* A disk drive's command ("READ SECTOR"), "FUJINET", "NETWORK" or "". */
const char *sio_command_name(int dev, int cmd);

#ifdef __cplusplus
}
#endif

#endif
