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

static inline a800session *test_session(const char *tag)
{
    char base[512], cfg[600], data[600];
    a800session_paths p;
    test_tmpdir(base, sizeof base, tag);
    snprintf(cfg, sizeof cfg, "%s/config", base);
    snprintf(data, sizeof data, "%s/data", base);
    memset(&p, 0, sizeof p);
    p.config_dir = cfg;
    p.data_dir = data;
    p.fujinet_lib = "";
    return a800session_new(&p);
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
