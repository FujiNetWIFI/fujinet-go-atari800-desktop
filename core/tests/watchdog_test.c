/* FujiNet ending itself: the web UI's restart (exit code 75) brings it back
   by itself; restart?exit=1 (code 0) leaves it stopped. SKIPs (77) without
   the runtime. */
#include "test_util.h"
#if defined(_WIN32)
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

static void http_get(const char *path)
{
    char req[256], buf[512];
    struct sockaddr_in a;
    int fd = (int)socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd >= 0, "socket");
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(A800SESSION_WEBUI_PORT);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(connect(fd, (struct sockaddr *)&a, sizeof a) == 0, "connect web UI");
    snprintf(req, sizeof req, "GET %s HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n", path);
    CHECK(send(fd, req, (int)strlen(req), 0) > 0, "send");
    while (recv(fd, buf, sizeof buf, 0) > 0)
        ;
#if defined(_WIN32)
    closesocket(fd);
#else
    close(fd);
#endif
}

static int wait_event(a800session *s, unsigned gen, const char *text, int ms)
{
    char ev[200];
    int t;
    for (t = 0; t < ms; t += 50) {
        if (a800session_fujinet_generation(s) != gen) {
            a800session_fujinet_last_event(s, ev, sizeof ev);
            if (strstr(ev, text))
                return 1;
        }
        test_sleep_ms(50);
    }
    return 0;
}

int main(void)
{
    a800session *s = test_session_ex("watchdog", 1);
    unsigned gen;
    int t;

    if (!test_start_fujinet(s)) {
        fprintf(stderr, "SKIP: no FujiNet runtime\n");
        a800session_free(s);
        return 77;
    }
    gen = a800session_fujinet_generation(s);
    http_get("/restart");
    CHECK(wait_event(s, gen, "restarted", 8000), "restarted by itself");
    for (t = 0; t < 5000 && !a800session_fujinet_link(s); t += 50)
        test_sleep_ms(50);
    CHECK(a800session_fujinet_running(s) && a800session_fujinet_link(s), "back and linked");

    gen = a800session_fujinet_generation(s);
    http_get("/restart?exit=1");
    CHECK(wait_event(s, gen, "stopped", 8000), "stopped for good");
    CHECK(!a800session_fujinet_running(s), "not running");

    CHECK(a800session_fujinet_restart(s) == 0, "Restart FujiNet");
    CHECK(a800session_fujinet_link(s), "linked after Restart");

    a800session_stop(s);
    a800session_free(s);
    puts("watchdog: ok");
    return 0;
}
