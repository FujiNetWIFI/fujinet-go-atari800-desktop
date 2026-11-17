/* The real in-process FujiNet over NetSIO: it connects to the hub, the Atari
   boots FujiNet's CONFIG from it, a power cycle restarts FujiNet and boots
   CONFIG again. Host listings (the SD card and, when tnfsd is installed, a
   TNFS server) must stay right across repeated restarts, with an image
   mounted. SKIPs (77) when the runtime was not built. */
#include "test_util.h"
#include "a800debug.h"

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <poll.h>

/* One request to FujiNet's web server; reads until the page ends or goes
   quiet (the server keeps the connection open and may chunk the body). */
static int web(const char *method, const char *path, const char *json, char *out, int outsz)
{
    struct sockaddr_in a;
    char req[1024];
    int fd = socket(AF_INET, SOCK_STREAM, 0), total = 0;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(A800SESSION_WEBUI_PORT);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    out[0] = 0;
    if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof a) != 0) {
        if (fd >= 0)
            close(fd);
        return -1;
    }
    snprintf(req, sizeof req,
             "%s %s HTTP/1.0\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\n"
             "Content-Length: %u\r\n\r\n%s",
             method, path, json ? (unsigned)strlen(json) : 0u, json ? json : "");
    if (send(fd, req, strlen(req), 0) <= 0) {
        close(fd);
        return -1;
    }
    for (;;) {
        struct pollfd pf = {fd, POLLIN, 0};
        int n;
        if (poll(&pf, 1, total ? 1500 : 6000) <= 0)
            break;
        n = (int)recv(fd, out + total, (size_t)(outsz - 1 - total), 0);
        if (n <= 0)
            break;
        total += n;
        out[total] = 0;
        if (strstr(out, "</html>") || strstr(out, "\r\n\r\n}\n"))
            break;
        if (total >= outsz - 1)
            break;
    }
    close(fd);
    out[total] = 0;
    return total;
}

static int count_of(const char *hay, const char *needle)
{
    int n = 0;
    for (; (hay = strstr(hay, needle)) != NULL; hay += strlen(needle))
        n++;
    return n;
}

/* Host slot slot (0-based): the root listing has exactly the files made. */
static void check_listing(int slot, const char *what, const char *const *names, int nnames)
{
    static char page[1 << 17];
    char url[96];
    int i;
    snprintf(url, sizeof url, "/hsdir?hostslot=%d&path=%%2F", slot);
    web("GET", url, NULL, page, sizeof page);
    for (i = 0; i < nnames; i++)
        CHECK(strstr(page, names[i]), "%s: %s missing from the listing", what, names[i]);
    /* garbage names show up as extra entries (32 of them); the SD placeholder and the Parent link are the allowed extras */
    i = count_of(page, "<li>");
    CHECK(i >= nnames && i <= nnames + 2, "%s: %d entries, expected %d", what, i, nnames);
    CHECK(!strstr(page, "\\x"), "%s: unprintable bytes in a file name", what);
}

static void mount_drive1(int host_slot, const char *path, const char *what)
{
    static char page[4096];
    char json[256];
    snprintf(json, sizeof json, "{\"path\":\"%s\",\"host_slot\":%d,\"mode\":\"r\"}", path,
             host_slot);
    web("POST", "/api/v1/drives/1/mount", json, page, sizeof page);
    CHECK(strstr(page, " 200 "), "%s: mount %s failed: %.200s", what, path, page);
    web("GET", "/api/v1/drives/1", NULL, page, sizeof page);
    CHECK(strstr(page, path + 1), "%s: drive 1 does not show %s: %.200s", what, path, page);
}

/* FujiNet's host names carry no port: the server must be on TNFS's own. */
static pid_t start_tnfsd(const char *root)
{
    pid_t pid = fork();
    if (pid == 0) {
        freopen("/dev/null", "w", stdout);
        freopen("/dev/null", "w", stderr);
        execlp("tnfsd", "tnfsd", root, (char *)NULL);
        _exit(127);
    }
    test_sleep_ms(500);
    if (pid > 0 && waitpid(pid, NULL, WNOHANG) == pid)
        return -1;   /* not installed (or could not bind) */
    return pid;
}

static void make_files(const char *dir, const char *const *names, int n)
{
    char path[700];
    int i;
    for (i = 0; i < n; i++) {
        snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        CHECK(a800session_new_blank_disk(path, 0) == 0, "make %s", path);
    }
}

/* list, mount an image, restart FujiNet three times, list again each time */
static void cycle(a800session *s, int slot, const char *what, const char *const *names, int n)
{
    char image[64];
    int i;
    check_listing(slot, what, names, n);
    snprintf(image, sizeof image, "/%s", names[0]);
    mount_drive1(slot, image, what);
    for (i = 1; i <= 3; i++) {
        char label[96];
        a800session_power_cycle(s);
        CHECK(a800session_fujinet_running(s) && a800session_fujinet_link(s), "%s: FujiNet back", what);
        CHECK(test_wait_fujinet_log(s, "READ DEVICE SLOTS", 60000), "%s: CONFIG after cycle %d",
              what, i);
        snprintf(label, sizeof label, "%s after power cycle %d", what, i);
        check_listing(slot, label, names, n);
    }
}
#endif

/* The debugger's FujiNet trace saw CONFIG's commands and FujiNet's replies. */
static void check_trace(a800session *s)
{
    static a800_fuji_packet p[512];
    int n = a800debug_fujinet_trace(a800session_debugger(s), p, 512), i, req = 0, rep = 0;
    for (i = 0; i < n; i++) {
        if (!p[i].to_host && strstr(p[i].text, "FujiNet "))
            req++;
        if (p[i].to_host && strstr(p[i].text, "ACK COMPLETE"))
            rep++;
    }
    CHECK(req > 0 && rep > 0, "FujiNet trace: %d packets, %d to FujiNet, %d replies", n, req, rep);
}

/* Reads a cursor dry into acc (appending); returns 1 if any read said reset. */
static int drain_log(a800session *s, a800_log_cursor *cur, char *acc, size_t accsz)
{
    static char chunk[1 << 16];
    int n, reset, any = 0;
    do {
        n = a800session_fujinet_read_log(s, cur, chunk, sizeof chunk, &reset);
        if (reset) {
            acc[0] = 0;   /* a new log: what was shown is gone */
            any = 1;
        }
        CHECK(strlen(acc) + (size_t)n < accsz, "log accumulator full");
        strcat(acc, chunk);
    } while (n > 0);
    return any;
}

/* The incremental log: every byte once, in order, and a reset when FujiNet restarts. */
static void check_incremental_log(a800session *s)
{
    static char a[3 << 20], b[3 << 20];
    a800_log_cursor c1 = {0, 0}, c2 = {0, 0};
    CHECK(drain_log(s, &c1, a, sizeof a), "first read starts a log");
    CHECK(strstr(a, "READ DEVICE SLOTS"), "incremental log has CONFIG's commands");
    test_sleep_ms(1500);
    CHECK(!drain_log(s, &c1, a, sizeof a), "no reset while the same run goes on");
    b[0] = 0;
    drain_log(s, &c2, b, sizeof b);
    CHECK(strlen(b) >= strlen(a) && strncmp(a, b, strlen(a)) == 0,
          "appended text is the log read whole (%zu vs %zu bytes)", strlen(a), strlen(b));
    a800session_power_cycle(s);
    CHECK(test_wait_fujinet_log(s, "READ DEVICE SLOTS", 60000), "CONFIG after power cycle");
    CHECK(drain_log(s, &c1, a, sizeof a), "a restart resets the reader");
    CHECK(strstr(a, "READ DEVICE SLOTS"), "the new run's log");
}

int main(void)
{
    char data[600], ini[256];
    a800session *s;
#ifndef _WIN32
    static const char *const sdfiles[] = {"sdone.atr", "sdtwo.atr", "sdthree.atr"};
    static const char *const tnfsfiles[] = {"tnfsone.atr", "tnfstwo.atr", "tnfsthree.atr"};
    char dir[700], tdir[700];
    pid_t tnfsd;
    snprintf(ini, sizeof ini, "[Host2]\ntype=TNFS\nname=127.0.0.1\n");
#else
    ini[0] = 0;
#endif
    s = test_session_cfg("netboot", 1, ini, data, sizeof data);
    CHECK(s, "new");
    if (!test_start_fujinet(s)) {
        fprintf(stderr, "SKIP: no FujiNet runtime (%s)\n", a800session_last_error(s));
        a800session_free(s);
        return 77;
    }
    /* CONFIG running: it asks FujiNet for the host and drive slots */
    CHECK(test_wait_fujinet_log(s, "READ DEVICE SLOTS", 60000), "CONFIG booted");
    test_sleep_ms(1500);
    check_trace(s);
    if (getenv("A800_TEST_SHOT"))
        test_save_ppm(s, getenv("A800_TEST_SHOT"));

    /* power cycle: FujiNet restarts (a fresh log) and boots CONFIG again */
    a800session_power_cycle(s);
    CHECK(a800session_fujinet_running(s) && a800session_fujinet_link(s), "FujiNet back");
    CHECK(test_wait_fujinet_log(s, "READ DEVICE SLOTS", 60000), "CONFIG after power cycle");

#ifndef _WIN32
    /* the SD card, then a TNFS server: the same listing before and after
       each restart, with an image mounted from the host */
    snprintf(dir, sizeof dir, "%s/fujinet/SD", data);
    make_files(dir, sdfiles, 3);
    cycle(s, 0, "SD", sdfiles, 3);

    snprintf(tdir, sizeof tdir, "%s/tnfsroot", data);
    test_mkdir(tdir);
    make_files(tdir, tnfsfiles, 3);
    tnfsd = start_tnfsd(tdir);
    if (tnfsd > 0) {
        cycle(s, 1, "TNFS", tnfsfiles, 3);
        kill(tnfsd, SIGTERM);
        waitpid(tnfsd, NULL, 0);
    }
    else {
        fprintf(stderr, "netboot: no tnfsd (or TNFS's port is taken), TNFS listing not tested\n");
    }
#endif

    check_incremental_log(s);

    a800session_stop(s);
    a800session_free(s);
    puts("netboot: ok");
    return 0;
}
