/*
 * fujinet_api.c -- FujiNet's REST API on loopback (/api/v1/drives): which
 * image each drive slot holds, mounting one from the SD card, ejecting.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include "compat.h"
#include "fujinet_api.h"
#include "session_internal.h"

/* One request; the response body into body (NUL-terminated). Returns the
   HTTP status, or -1 if FujiNet did not answer. */
static int http(const char *method, const char *path, const char *json, char *body, int bodysz)
{
    struct sockaddr_in a;
    char req[2048], *buf;
    int fd, n, total = 0, status = -1, cap = 65536;
#if defined(_WIN32)
    static int wsa;
    if (!wsa) {
        WSADATA d;
        WSAStartup(MAKEWORD(2, 2), &d);
        wsa = 1;
    }
#endif
    fd = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    {
#if defined(_WIN32)
        DWORD tv = 5000;
#else
        struct timeval tv = {5, 0};
#endif
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof tv);
    }
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(A800SESSION_WEBUI_PORT);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) {
        a800_closesocket(fd);
        return -1;
    }
    snprintf(req, sizeof req,
             "%s %s HTTP/1.0\r\nHost: 127.0.0.1\r\nConnection: close\r\n"
             "Content-Type: application/json\r\nContent-Length: %u\r\n\r\n%s",
             method, path, json ? (unsigned)strlen(json) : 0u, json ? json : "");
    if (send(fd, req, (int)strlen(req), 0) <= 0) {
        a800_closesocket(fd);
        return -1;
    }
    buf = malloc((size_t)cap + 1);
    if (!buf) {
        a800_closesocket(fd);
        return -1;
    }
    /* to the end of the body: FujiNet's server keeps the connection open
       after it, so reading to the close would wait out the timeout */
    while (total < cap && (n = (int)recv(fd, buf + total, cap - total, 0)) > 0) {
        const char *end, *cl;
        total += n;
        buf[total] = 0;
        end = strstr(buf, "\r\n\r\n");
        if (!end)
            continue;
        cl = strstr(buf, "Content-Length:");
        if (!cl)
            cl = strstr(buf, "content-length:");
        if (cl && cl < end && (end - buf) + 4 + atoi(cl + 15) <= total)
            break;
    }
    a800_closesocket(fd);
    buf[total] = 0;
    if (sscanf(buf, "HTTP/%*s %d", &status) != 1)
        status = -1;
    if (body && bodysz > 0) {
        const char *b = strstr(buf, "\r\n\r\n");
        snprintf(body, (size_t)bodysz, "%s", b ? b + 4 : "");
    }
    free(buf);
    return status;
}

/* ---- just enough JSON for the drive list ----------------------------------- */

static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;
    return p;
}

static const char *read_string(const char *p, char *out, size_t outsz)
{
    size_t k = 0;
    if (*p != '"')
        return NULL;
    for (p++; *p && *p != '"'; p++) {
        char c = *p;
        if (c == '\\' && p[1]) {
            p++;
            c = *p == 'n' ? '\n' : *p == 't' ? '\t' : *p;
        }
        if (k + 1 < outsz)
            out[k++] = c;
    }
    if (outsz)
        out[k] = 0;
    return *p == '"' ? p + 1 : NULL;
}

/* The list, fetched at most twice a second: menus ask for each drive. */
static pthread_mutex_t s_cache_mtx = PTHREAD_MUTEX_INITIALIZER;
static fujinet_drive s_cache[16];
static int s_cache_n = -1;
static double s_cache_at;

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void cache_drop(void)
{
    pthread_mutex_lock(&s_cache_mtx);
    s_cache_n = -1;
    pthread_mutex_unlock(&s_cache_mtx);
}

static int fetch_drives(fujinet_drive *out, int max);

int fujinet_api_drives(fujinet_drive *out, int max)
{
    int n;
    pthread_mutex_lock(&s_cache_mtx);
    if (s_cache_n < 0 || now_s() - s_cache_at > 0.5) {
        s_cache_n = fetch_drives(s_cache, 16);
        s_cache_at = now_s();
    }
    n = s_cache_n < max ? s_cache_n : max;
    if (n > 0)
        memcpy(out, s_cache, (size_t)n * sizeof *out);
    pthread_mutex_unlock(&s_cache_mtx);
    return n;
}

static int fetch_drives(fujinet_drive *out, int max)
{
    static char body[65536];
    const char *p;
    int n = 0;
    if (http("GET", "/api/v1/drives", NULL, body, sizeof body) != 200)
        return -1;
    p = skip_ws(body);
    if (*p != '[')
        return -1;
    p++;
    while (n < max) {
        fujinet_drive d;
        memset(&d, 0, sizeof d);
        d.host_slot = -1;
        p = skip_ws(p);
        if (*p != '{')
            break;
        p++;
        for (;;) {
            char key[32], sval[512];
            p = skip_ws(p);
            if (*p == '}') {
                p++;
                break;
            }
            if (!(p = read_string(p, key, sizeof key)))
                return n;
            p = skip_ws(p);
            if (*p++ != ':')
                return n;
            p = skip_ws(p);
            if (*p == '"') {
                if (!(p = read_string(p, sval, sizeof sval)))
                    return n;
                if (!strcmp(key, "path"))
                    snprintf(d.path, sizeof d.path, "%s", sval);
                else if (!strcmp(key, "mode"))
                    d.writable = sval[0] == 'w';
                else if (!strcmp(key, "host"))
                    snprintf(d.host, sizeof d.host, "%.63s", sval);
            }
            else {
                long v = strtol(p, NULL, 10);
                if (!strncmp(p, "true", 4))
                    v = 1;
                else if (!strncmp(p, "false", 5))
                    v = 0;
                if (!strcmp(key, "slot"))
                    d.slot = (int)v;
                else if (!strcmp(key, "mounted"))
                    d.mounted = (int)v;
                else if (!strcmp(key, "host_slot"))
                    d.host_slot = (int)v;
                while (*p && *p != ',' && *p != '}')
                    p++;
            }
            p = skip_ws(p);
            if (*p == ',')
                p++;
        }
        out[n++] = d;
        p = skip_ws(p);
        if (*p == ',')
            p++;
    }
    return n;
}

static void json_escape(const char *in, char *out, size_t outsz)
{
    size_t k = 0;
    for (; *in && k + 3 < outsz; in++) {
        if (*in == '"' || *in == '\\')
            out[k++] = '\\';
        out[k++] = *in;
    }
    out[k] = 0;
}

int fujinet_api_mount(int slot, int host_slot, const char *path, int writable, char *msg, int msgsz)
{
    char url[64], json[1200], esc[1024], body[1024];
    int rc;
    json_escape(path, esc, sizeof esc);
    snprintf(url, sizeof url, "/api/v1/drives/%d/mount", slot);
    snprintf(json, sizeof json, "{\"path\":\"%s\",\"host_slot\":%d,\"mode\":\"%s\"}", esc, host_slot,
             writable ? "w" : "r");
    rc = http("POST", url, json, body, sizeof body);
    cache_drop();
    if (rc == 200)
        return 0;
    if (msg && msgsz > 0) {
        if (rc == 401)
            snprintf(msg, (size_t)msgsz, "FujiNet's web UI has a password; mount from CONFIG instead");
        else if (rc < 0)
            snprintf(msg, (size_t)msgsz, "FujiNet is not answering");
        else
            snprintf(msg, (size_t)msgsz, "FujiNet could not mount it (%d): %.200s", rc, body);
    }
    return -1;
}

int fujinet_api_eject(int slot)
{
    char url[64];
    int rc;
    snprintf(url, sizeof url, "/api/v1/drives/%d/eject", slot);
    rc = http("POST", url, "{}", NULL, 0);
    cache_drop();
    return rc == 200 ? 0 : -1;
}
