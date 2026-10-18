/* FujiNet's REST API on loopback (fujinet_api.c). */
#ifndef A800_FUJINET_API_H
#define A800_FUJINET_API_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int slot;               /* 1-based */
    char path[512];         /* on the host, "" if none */
    char host[64];
    int host_slot;          /* 0-based, -1 if none */
    int mounted;
    int writable;
} fujinet_drive;

/* The drive slots; returns how many, or -1 if FujiNet did not answer. */
int fujinet_api_drives(fujinet_drive *out, int max);
/* Mounts path (on host host_slot, 0 = the SD card) in drive slot (1-based). */
int fujinet_api_mount(int slot, int host_slot, const char *path, int writable,
                      char *msg, int msgsz);
int fujinet_api_eject(int slot);

#ifdef __cplusplus
}
#endif

#endif
