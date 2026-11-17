// --probe-dsu: the Cemuhook / DSU motion client (input/dsu.c) without the game or a controller. By default it is its
// own DSU server on the loopback: it waits for the client's pad data request, checks its header and CRC, then sends
// slot 0 a controller lying flat and turning right at 90 deg/s, 250 samples a second, and checks that the client hands
// the sample over in the Switch frame (acceleration z = -1 G, rate z = -pi/2 rad/s), that the fusion then reads a
// quarter turn a second, that a packet with a bad CRC is ignored, that the samples come back when the server quits and
// starts again, and that MKW_DSU=0 stops it. With MKW_DSU_SERVER set it asks that server instead (a real one:
// DS4Windows, BetterJoy...) and prints what slot MKW_DSU_SLOT sends for 5 s.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET HostSock;
#define HOST_BAD INVALID_SOCKET
#define host_close closesocket
typedef int SockLen;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int HostSock;
#define HOST_BAD (-1)
#define host_close close
typedef socklen_t SockLen;
#endif

#include "cfg.h"
#include "dsu.h"
#include "host.h"
#include "input.h"
#include "platform/platform.h"

static int failures;
#define CHECK(cond, ...)                                                                                               \
    do {                                                                                                               \
        int ok_ = (cond);                                                                                              \
        printf("%s dsu: ", ok_ ? "ok  " : "FAIL");                                                                     \
        printf(__VA_ARGS__);                                                                                           \
        printf("\n");                                                                                                  \
        if (!ok_) failures++;                                                                                          \
    } while (0)

static int close_to(double a, double b) { return fabs(a - b) < 1e-3; }

// A real server: what slot MKW_DSU_SLOT sends, every half second for 5 s.
static int real_server(void) {
    int slot = cfg_int("MKW_DSU_SLOT"), got = 0;
    printf("     dsu: asking %s, slot %d\n", cfg_value("MKW_DSU_SERVER"), slot);
    for (int i = 0; i < 10; i++) {
        plat_sleep_ms(500);
        double acc[3], gyro[3];
        if (!dsu_motion(slot, acc, gyro)) {
            printf("     dsu: no sample\n");
            continue;
        }
        got++;
        printf("     dsu: Switch frame acc %+.2f %+.2f %+.2f G, gyro %+.2f %+.2f %+.2f rad/s\n", acc[0], acc[1], acc[2],
               gyro[0], gyro[1], gyro[2]);
    }
    CHECK(got > 0, "%d of 10 reads had a sample", got);
    return failures ? 1 : 0;
}

typedef struct {
    HostSock s;
    volatile int request_ok, stop, bad_crc_sent;
} Server;

static void server_main(void *arg) {
    Server *sv = arg;
    uint8_t req[64];
    struct sockaddr_in from;
    SockLen len = sizeof from;
    int n = (int)recvfrom(sv->s, (char *)req, sizeof req, 0, (struct sockaddr *)&from, &len);
    uint8_t copy[64];
    if (n == 28) {
        memcpy(copy, req, 28);
        memset(copy + 8, 0, 4);
        uint32_t crc = req[8] | req[9] << 8 | req[10] << 16 | (uint32_t)req[11] << 24;
        sv->request_ok = !memcmp(req, "DSUC", 4) && req[4] == (1001 & 0xff) && req[6] == 12 && req[16] == 0x02 &&
                         req[18] == 0x10 && dsu_crc32(copy, 28) == crc;
    }
    const float flat[3] = {0, -1, 0}, yaw_right[3] = {0, 90, 0}, junk[3] = {5, 5, 5};
    uint8_t pkt[100];
    for (uint32_t i = 0; !sv->stop; i++) {
        dsu_pad_packet(pkt, 0, i, (uint64_t)i * 4000, flat, yaw_right);
        sendto(sv->s, (const char *)pkt, sizeof pkt, 0, (struct sockaddr *)&from, len);
        dsu_pad_packet(pkt, 1, i, (uint64_t)i * 4000, junk, junk);
        pkt[30] ^= 1;   // slot 1: the CRC no longer matches
        sendto(sv->s, (const char *)pkt, sizeof pkt, 0, (struct sockaddr *)&from, len);
        sv->bad_crc_sent = 1;
        plat_sleep_ms(4);
    }
}

int probe_dsu(void) {
    double acc[3], gyro[3];
    // The conversion alone: flat and turning right in DSU axes is z down and a clockwise turn about z for the Switch.
    const float flat[3] = {0, -1, 0}, yaw_right[3] = {0, 90, 0}, pitch_up[3] = {45, 0, 0}, roll_right[3] = {0, 0, 30};
    dsu_axes(flat, yaw_right, acc, gyro);
    CHECK(close_to(acc[0], 0) && close_to(acc[1], 0) && close_to(acc[2], -1) && close_to(gyro[2], -M_PI / 2) &&
              close_to(gyro[0], 0),
          "axes: flat -> acc (%.2f %.2f %.2f) G, yaw right 90 deg/s -> rate z %.3f rad/s", acc[0], acc[1], acc[2],
          gyro[2]);
    dsu_axes(flat, pitch_up, acc, gyro);
    CHECK(close_to(gyro[0], M_PI / 4), "axes: pitch up 45 deg/s -> rate x %.3f rad/s", gyro[0]);
    dsu_axes(flat, roll_right, acc, gyro);
    CHECK(close_to(gyro[1], M_PI / 6), "axes: roll right 30 deg/s -> rate y %.3f rad/s", gyro[1]);

    if (cfg_get("MKW_DSU_SERVER")) {
        cfg_set("MKW_DSU", "1");
        dsu_start();
        return real_server();
    }
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    Server sv = {.s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)};
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    SockLen alen = sizeof a;
    if (sv.s == HOST_BAD || bind(sv.s, (struct sockaddr *)&a, sizeof a) ||
        getsockname(sv.s, (struct sockaddr *)&a, &alen)) {
        CHECK(0, "cannot open the fake server's socket");
        return 1;
    }
    char server[32];
    snprintf(server, sizeof server, "127.0.0.1:%u", ntohs(a.sin_port));
    if (!plat_thread_spawn(server_main, &sv, "dsu-server", 0)) {
        CHECK(0, "cannot start the fake server");
        return 1;
    }
    cfg_set("MKW_DSU_SERVER", server);
    cfg_set("MKW_DSU_SLOT", "0");
    cfg_set("MKW_DSU", "1");
    dsu_start();
    uint64_t t0 = plat_now_ms();
    int got = 0;
    while (!(got = dsu_motion(0, acc, gyro)) && plat_now_ms() - t0 < 3000) plat_sleep_ms(10);
    CHECK(sv.request_ok, "the client's pad data request (DSUC, 1001, 12 bytes after the header, type 0x100002, CRC)");
    CHECK(got, "first sample from %s after %llu ms", server, (unsigned long long)(plat_now_ms() - t0));
    CHECK(got && close_to(acc[2], -1) && close_to(acc[0], 0) && close_to(gyro[2], -M_PI / 2),
          "sample in the Switch frame: acc (%.2f %.2f %.2f) G, rate (%.3f %.3f %.3f) rad/s", acc[0], acc[1], acc[2],
          gyro[0], gyro[1], gyro[2]);

    // Into a fusion as the motion thread feeds it: 250 Hz for 1 s, a quarter turn clockwise about z.
    MotionFusion *m = motion_new();
    uint64_t now = 1000000000ull;
    int fed = 0;
    for (int i = 0; i < 250; i++, now += 4000000) {
        if (dsu_motion(0, acc, gyro)) motion_feed(m, acc, gyro, now), fed++;
        plat_sleep_ms(4);
    }
    MotionState st;
    int n = motion_read(m, &st, 1, now);
    CHECK(fed == 250 && n == 1 && close_to(st.gyro[2], -0.25) && fabs(st.angle[2] + 0.25) < 0.01 &&
              close_to(st.acc[2], -1),
          "fusion: %d samples fed, rate z %.3f rev/s, angle z %.3f rev after 1 s, acc z %.2f G", fed, st.gyro[2],
          st.angle[2], st.acc[2]);

    CHECK(sv.bad_crc_sent && !dsu_motion(1, acc, gyro), "slot 1's packets with a bad CRC are ignored");

    // The server quits (the client's requests then meet a closed port: "port unreachable") and starts again on the
    // same port, as a player closing and reopening DS4Windows does: the samples stop, then come back on their own.
    sv.stop = 1;
    plat_sleep_ms(50);
    host_close(sv.s);
    plat_sleep_ms(1500);
    CHECK(!dsu_motion(0, acc, gyro), "server gone: no sample");
    static Server again;
    again.s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (again.s == HOST_BAD || bind(again.s, (struct sockaddr *)&a, sizeof a) ||
        !plat_thread_spawn(server_main, &again, "dsu-server", 0)) {
        CHECK(0, "cannot start the fake server again on %s", server);
        return 1;
    }
    t0 = plat_now_ms();
    while (!(got = dsu_motion(0, acc, gyro)) && plat_now_ms() - t0 < 3000) plat_sleep_ms(10);
    CHECK(got, "server back on the same port: samples again after %llu ms", (unsigned long long)(plat_now_ms() - t0));

    cfg_set("MKW_DSU", "0");
    plat_sleep_ms(600);
    CHECK(!dsu_motion(0, acc, gyro), "MKW_DSU=0 (live): no sample");
    again.stop = 1;
    plat_sleep_ms(50);
    printf("dsu: %s (%d failures)\n", failures ? "FAILED" : "passed", failures);
    return failures ? 1 : 0;
}
