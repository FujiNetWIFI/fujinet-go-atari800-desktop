/* Shared helpers for the session tests. */
#ifndef A800_TEST_UTIL_H
#define A800_TEST_UTIL_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "a800session.h"
#include "test_tmpdir.h"

#define CHECK(cond, ...) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); exit(1); } } while (0)

static inline void test_sleep_ms(int ms)
{
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

/* A session in a fresh XDG tree. With FujiNet, its own fnconfig.ini: the
   SD card and no network hosts (tests never touch real servers). */
static inline a800session *test_session_ex(const char *tag, int fujinet)
{
    char base[512], cfg[600], data[600], path[700];
    a800session_paths p;
    FILE *fp;
    test_tmpdir(base, sizeof base, tag);
    snprintf(cfg, sizeof cfg, "%s/config", base);
    snprintf(data, sizeof data, "%s/data", base);
    memset(&p, 0, sizeof p);
    p.config_dir = cfg;
    p.data_dir = data;
    if (!fujinet)
        p.fujinet_lib = "";
    else {
        test_mkdir(data);
        snprintf(path, sizeof path, "%s/fujinet", data);
        test_mkdir(path);
        snprintf(path, sizeof path, "%s/fujinet/fnconfig.ini", data);
        if ((fp = fopen(path, "w")) != NULL) {
            fputs("[General]\ndevicename=fujinet-test\nconfig_enabled=1\nboot_mode=0\n"
                  "[Host1]\ntype=SD\nname=SD\n", fp);
            fclose(fp);
        }
    }
    return a800session_new(&p);
}

static inline a800session *test_session(const char *tag)
{
    return test_session_ex(tag, 0);
}

/* Starts with FujiNet and waits for its link; 0 if there is no runtime. */
static inline int test_start_fujinet(a800session *s)
{
    a800session_start_opts o = {1, 0, 0};
    int t;
    if (a800session_start(s, &o) != 0 || !a800session_fujinet_running(s))
        return 0;
    for (t = 0; t < 5000 && !a800session_fujinet_link(s); t += 50)
        test_sleep_ms(50);
    CHECK(a800session_fujinet_link(s), "NetSIO link");
    return 1;
}

static inline void test_start(a800session *s, int fujinet)
{
    a800session_start_opts o = {fujinet, 0, 0};
    int rc = a800session_start(s, &o);
    CHECK(rc == 0, "start: %s", a800session_last_error(s));
}

/* Waits up to ms for text on the GR.0 screen. */
static inline int test_wait_text(a800session *s, const char *text, int ms)
{
    char buf[2048];
    int t;
    for (t = 0; t < ms; t += 50) {
        a800session_copy_screen_text(s, buf, sizeof buf);
        if (strstr(buf, text))
            return 1;
        test_sleep_ms(50);
    }
    fprintf(stderr, "screen was:\n%s\n", buf);
    return 0;
}

/* Waits up to ms for text in FujiNet's console log. */
static inline int test_wait_fujinet_log(a800session *s, const char *text, int ms)
{
    static char buf[1 << 20];
    int t;
    for (t = 0; t < ms; t += 100) {
        a800session_fujinet_copy_log(s, buf, sizeof buf);
        if (strstr(buf, text))
            return 1;
        test_sleep_ms(100);
    }
    return 0;
}

/* The current frame as a PPM, for looking at by hand. */
static inline void test_save_ppm(a800session *s, const char *path)
{
    static uint32_t fb[A800SESSION_FB_MAX_WIDTH * A800SESSION_FB_MAX_HEIGHT];
    uint64_t serial = 0;
    int w, h, par, i;
    FILE *fp;
    a800session_copy_frame(s, fb, &w, &h, &par, &serial);
    fp = fopen(path, "wb");
    if (!fp)
        return;
    fprintf(fp, "P6\n%d %d\n255\n", w, h);
    for (i = 0; i < w * h; i++) {
        unsigned char rgb[3] = {(unsigned char)(fb[i] >> 16), (unsigned char)(fb[i] >> 8),
                                (unsigned char)fb[i]};
        fwrite(rgb, 1, 3, fp);
    }
    fclose(fp);
}

static inline void test_set_pref(a800session *s, const char *key, int value)
{
    int i = a800session_pref_find(key);
    CHECK(i >= 0, "no pref %s", key);
    if (a800session_pref(i)->type == A800_PREF_CHOICE)
        a800session_pref_set_choice(s, i, value);
    else
        a800session_set_int(s, key, value);
}

#endif
