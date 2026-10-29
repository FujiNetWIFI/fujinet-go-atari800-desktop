/* "Use external FujiNet": no runtime of ours is started and the NetSIO hub
   listens on the chosen address:port. A UDP client plays the external
   FujiNet: it pings the hub, connects, and the link light follows. A second
   start on another port proves the hub is rebound and the old port is
   released. A bad address refuses to start. */
#include "test_util.h"
#if defined(_WIN32)
int main(void)
{
    return 77; /* POSIX sockets; Windows has the address unit test only */
}
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#define NETSIO_DEVICE_CONNECTED 0xC1
#define NETSIO_PING_REQUEST     0xC2
#define NETSIO_PING_RESPONSE    0xC3

/* A UDP port nobody holds right now. */
static int free_port(void)
{
    struct sockaddr_in a;
    socklen_t len = sizeof a;
    int fd = socket(AF_INET, SOCK_DGRAM, 0), port;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(fd >= 0 && bind(fd, (struct sockaddr *)&a, sizeof a) == 0, "probe socket");
    getsockname(fd, (struct sockaddr *)&a, &len);
    port = ntohs(a.sin_port);
    close(fd);
    return port;
}

/* Sends one byte to the hub; 1 if a PONG came back within ms. */
static int ping(int fd, int port, int ms)
{
    struct sockaddr_in a;
    struct timeval tv = {ms / 1000, (ms % 1000) * 1000};
    unsigned char b = NETSIO_PING_REQUEST, r = 0;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    sendto(fd, &b, 1, 0, (struct sockaddr *)&a, sizeof a);
    return recv(fd, &r, 1, 0) == 1 && r == NETSIO_PING_RESPONSE;
}

int main(void)
{
    a800session *s = test_session("external");
    a800session_start_opts o;
    char addr[64];
    int port = free_port(), port2, fd, t;
    unsigned char hello = NETSIO_DEVICE_CONNECTED;
    struct sockaddr_in a;

    CHECK(s, "new");
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(fd >= 0, "client socket");

    /* the address pref: greyed until the switch is on, a bad text is not stored */
    {
        int ext = a800session_pref_find("fujinet_external"), ai = a800session_pref_find("fujinet_external_addr");
        char err[200];
        CHECK(ext >= 0 && ai >= 0 && a800session_pref(ai)->type == A800_PREF_TEXT, "prefs");
        CHECK(!a800session_pref_enabled(s, ai), "address greyed while the switch is off");
        a800session_set_int(s, "fujinet_external", 1);
        CHECK(a800session_pref_enabled(s, ai), "address live with the switch on");
        CHECK(a800session_pref_set_text(s, ai, "127.0.0.1:11600", err, sizeof err) == 0, "valid text");
        CHECK(a800session_pref_set_text(s, ai, "nope", err, sizeof err) != 0 && err[0], "bad text");
        CHECK(!strcmp(a800session_pref_get_text(s, ai), "127.0.0.1:11600"), "bad text not stored");
        a800session_set_int(s, "fujinet_external", 0);
    }

    /* a bad address is refused, not started */
    a800session_set_int(s, "fujinet_external", 1);
    a800session_set_str(s, "fujinet_external_addr", "not an address");
    a800session_default_opts(s, &o);
    o.enable_audio = o.enable_gamepad = 0;
    CHECK(a800session_start(s, &o) != 0 && !a800session_is_running(s), "bad address starts");

    snprintf(addr, sizeof addr, "127.0.0.1:%d", port);
    a800session_set_str(s, "fujinet_external_addr", addr);
    a800session_default_opts(s, &o);
    o.enable_audio = o.enable_gamepad = 0;
    CHECK(o.external_fujinet == 1, "opts external");
    CHECK(a800session_start(s, &o) == 0, "start: %s", a800session_last_error(s));
    CHECK(a800session_fujinet_external(s) && !a800session_fujinet_running(s),
          "external, no runtime of ours");
    CHECK(!strcmp(a800session_fujinet_endpoint(s), addr), "endpoint %s",
          a800session_fujinet_endpoint(s));
    CHECK(!a800session_fujinet_link(s), "no link before FujiNet connects");
    CHECK(ping(fd, port, 2000), "hub answers on %s", addr);

    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sendto(fd, &hello, 1, 0, (struct sockaddr *)&a, sizeof a);
    for (t = 0; t < 2000 && !a800session_fujinet_link(s); t += 20)
        test_sleep_ms(20);
    CHECK(a800session_fujinet_link(s), "link after DEVICE_CONNECTED");
    a800session_stop(s);
    CHECK(!a800session_fujinet_external(s), "not external when stopped");

    /* another port: the hub is rebound, the old port let go */
    port2 = free_port();
    snprintf(addr, sizeof addr, "127.0.0.1:%d", port2);
    a800session_set_str(s, "fujinet_external_addr", addr);
    a800session_default_opts(s, &o);
    o.enable_audio = o.enable_gamepad = 0;
    CHECK(a800session_start(s, &o) == 0, "restart: %s", a800session_last_error(s));
    CHECK(ping(fd, port2, 2000), "hub answers on the new %s", addr);
    CHECK(!ping(fd, port, 600), "the old port is released");
    a800session_stop(s);
    a800session_free(s);
    close(fd);
    puts("external: ok");
    return 0;
}
#endif
