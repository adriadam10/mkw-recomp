// Cemuhook / DSU (DualShock UDP) motion client. A DSU server (DS4Windows, BetterJoy, SteamGyroDSU, a phone app...)
// serves a controller's gyro and accelerometer over UDP, by default at 127.0.0.1:26760; this reads it for a controller
// whose own driver gives none, such as a pad set to XInput mode (discussion #435). Only motion is read: buttons and
// sticks still come through SDL.
//
// The protocol (v1993.github.io/cemuhook-protocol, version 1001; see docs/research.md): every packet starts with a 16
// byte header, little endian: magic "DSUC" (client) or "DSUS" (server), u16 version 1001, u16 length of what follows
// the header, u32 CRC32 (zlib's) of the whole packet with that field zeroed, u32 sender id; then a u32 message type.
// The client asks for pad data (type 0x100002, flags 0 = every slot) and the server sends a 100 byte pad data packet
// per slot per sample, for a few seconds after each request, so the request goes out again every second. Its motion:
// a u64 timestamp in microseconds, the accelerometer x, y, z in G, the gyro pitch, yaw, roll in deg/s (floats).
//
// One thread of its own: it opens the socket, sends the requests and keeps each slot's newest sample; input.c's motion
// thread takes it with dsu_motion at its own 250 Hz. Never the game's thread. MKW_DSU and MKW_DSU_SERVER are live: the
// socket closes when it is turned off and reopens at another address when the server changes.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
typedef SOCKET HostSock;
#define HOST_BAD INVALID_SOCKET
#define host_close closesocket
#define host_poll WSAPoll
#else
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int HostSock;
#define HOST_BAD (-1)
#define host_close close
#define host_poll poll
#endif

#include "cfg.h"
#include "dsu.h"
#include "host.h"
#include "input.h"
#include "platform/platform.h"

#define DSU_VERSION 1001
#define DSU_PAD_DATA 0x100002u
#define DSU_PAD_SIZE 100
#define FRESH_NS 200000000ull   // a sample older than this is no motion (motion_fusion.c's STALE_NS)

typedef struct {
    float acc[3], gyro[3];   // DSU axes: G, deg/s
    uint64_t at_ns;          // plat_now_ns when it arrived, 0 never
    uint64_t count;          // samples since the slot (re)connected, for the rate the log gives
    uint64_t since_ns;
    int logged;
} Slot;

static PlatMutex lock = PLAT_MUTEX_INIT;
static Slot slots[DSU_SLOTS];
static volatile int32_t enabled;   // MKW_DSU as the thread last read it

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v), put16(p + 2, v >> 16); }
static uint32_t get16(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8; }
static uint32_t get32(const uint8_t *p) { return get16(p) | get16(p + 2) << 16; }
static float getf(const uint8_t *p) {
    uint32_t u = get32(p);
    float f;
    memcpy(&f, &u, 4);
    return f;
}
static void putf(uint8_t *p, float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    put32(p, u);
}

// zlib's CRC32 (reflected polynomial 0xEDB88320), bit by bit: a 100 byte packet a few hundred times a second.
uint32_t dsu_crc32(const uint8_t *p, int n) {
    uint32_t c = 0xffffffffu;
    for (int i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
    }
    return ~c;
}

// Header for a packet of n bytes in all; the CRC goes in last (dsu_seal).
static void header(uint8_t *p, const char *magic, int n, uint32_t id, uint32_t type) {
    memcpy(p, magic, 4);
    put16(p + 4, DSU_VERSION);
    put16(p + 6, (uint32_t)(n - 16));
    put32(p + 8, 0);
    put32(p + 12, id);
    put32(p + 16, type);
}
static void seal(uint8_t *p, int n) { put32(p + 8, dsu_crc32(p, n)); }

int dsu_pad_packet(uint8_t out[DSU_PAD_SIZE], int slot, uint32_t number, uint64_t stamp_us, const float acc_g[3],
                   const float gyro_deg_s[3]) {
    memset(out, 0, DSU_PAD_SIZE);
    header(out, "DSUS", DSU_PAD_SIZE, 0x4d4b5744u, DSU_PAD_DATA);
    uint8_t *d = out + 20;
    d[0] = (uint8_t)slot, d[1] = 2, d[2] = 2, d[3] = 1, d[10] = 5;   // connected, full gyro, USB, battery full
    d[11] = 1;
    put32(d + 12, number);
    for (int i = 20; i < 24; i++) d[i] = 128;   // sticks centred
    put32(d + 48, (uint32_t)stamp_us), put32(d + 52, (uint32_t)(stamp_us >> 32));
    for (int i = 0; i < 3; i++) putf(d + 56 + 4 * i, acc_g[i]), putf(d + 68 + 4 * i, gyro_deg_s[i]);
    seal(out, DSU_PAD_SIZE);
    return DSU_PAD_SIZE;
}

// DSU axes (the DualShock 4's as DS4Windows serves them; the protocol page names no directions, Dolphin's DSU client
// labels them: accelerometer "Up" = -y, "Right" = -x, "Forward" = +z, gyro "Pitch Up" = +pitch, "Roll Right" = +roll,
// "Yaw Right" = +yaw): x to the right, y up out of the face, z away from the player, the acceleration as gravity reads
// (lying flat, face up: y = -1 G); pitch about x (front edge up positive), yaw about y (turning right positive), roll
// about z (right side down positive). So against SDL's sensor axes (input_motion_axes) the acceleration is SDL's
// negated and in G, pitch = SDL x, yaw = -SDL y, roll = -SDL z; through input_motion_axes' SDL -> Switch step:
// Switch acc = (x, -z, y), Switch gyro = (pitch, roll, -yaw), deg/s to rad/s. The PadForge server sends the same from
// SDL's axes (its accelerometer, yaw and roll negated).
void dsu_axes(const float a[3], const float g[3], double acc[3], double gyro[3]) {
    const double rad = M_PI / 180;
    acc[0] = a[0];
    acc[1] = -a[2];
    acc[2] = a[1];
    gyro[0] = g[0] * rad;
    gyro[1] = g[2] * rad;
    gyro[2] = -g[1] * rad;
}

int dsu_motion(int slot, double acc[3], double gyro[3]) {
    if (!enabled || slot < 0 || slot >= DSU_SLOTS) return 0;
    float a[3], g[3];
    uint64_t at;
    plat_mutex_lock(&lock);
    memcpy(a, slots[slot].acc, sizeof a);
    memcpy(g, slots[slot].gyro, sizeof g);
    at = slots[slot].at_ns;
    plat_mutex_unlock(&lock);
    if (!at || plat_now_ns() - at > FRESH_NS) return 0;
    dsu_axes(a, g, acc, gyro);
    return 1;
}

// One received datagram: a checked pad data packet updates its slot.
static void receive(const uint8_t *p, int n) {
    if (n < DSU_PAD_SIZE || memcmp(p, "DSUS", 4) || get16(p + 4) != DSU_VERSION || get32(p + 16) != DSU_PAD_DATA)
        return;
    int len = (int)get16(p + 6) + 16;
    if (len < DSU_PAD_SIZE || len > n) return;   // short: dropped; long: the rest is ignored (the page's rule)
    uint8_t copy[512];
    if (len > (int)sizeof copy) return;
    memcpy(copy, p, (size_t)len);
    put32(copy + 8, 0);
    if (dsu_crc32(copy, len) != get32(p + 8)) return;
    const uint8_t *d = p + 20;
    int slot = d[0];
    if (slot >= DSU_SLOTS || d[1] != 2 || !d[11]) return;   // not connected
    uint64_t now = plat_now_ns();
    plat_mutex_lock(&lock);
    Slot *s = &slots[slot];
    if (!s->at_ns || now - s->at_ns > 2000000000ull) s->count = 0, s->since_ns = now, s->logged = 0;
    for (int i = 0; i < 3; i++) s->acc[i] = getf(d + 56 + 4 * i), s->gyro[i] = getf(d + 68 + 4 * i);
    s->at_ns = now;
    s->count++;
    int report = !s->logged && now - s->since_ns >= 2000000000ull;
    uint64_t count = s->count, since = s->since_ns;
    if (report) s->logged = 1;
    float a[3] = {s->acc[0], s->acc[1], s->acc[2]};
    plat_mutex_unlock(&lock);
    if (report) {
        static const char *const models[] = {"no model", "partial gyro", "full gyro", "model 3"};
        rt_log("dsu: slot %d sends %.0f samples/s (%s, accelerometer %.2f %.2f %.2f G)", slot,
               (double)count * 1e9 / (double)(now - since), models[d[2] & 3], a[0], a[1], a[2]);
    }
}

// MKW_DSU_SERVER to an address: "host:port", "host" (port 26760), "[v6]:port" or a bare IPv6 address; 0 when it does
// not resolve. IPv4 first when a name has both: DSU servers listen on 127.0.0.1, and "localhost" can resolve to ::1
// first.
static int resolve(const char *server, struct sockaddr_storage *out, int *len) {
    char host[128], port[16] = "26760";
    snprintf(host, sizeof host, "%s", server);
    char *colon = strchr(host, ':');
    if (host[0] == '[') {   // [::1]:26760
        char *end = strchr(host, ']');
        if (end && end[1] == ':') snprintf(port, sizeof port, "%s", end + 2);
        if (end) *end = 0;
        memmove(host, host + 1, strlen(host));
    } else if (colon && colon == strrchr(host, ':')) {   // one colon: host:port (more is a bare IPv6 address)
        snprintf(port, sizeof port, "%s", colon + 1);
        *colon = 0;
    }
    struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_DGRAM}, *res = NULL;
    if (getaddrinfo(host, port, &hints, &res) || !res) return 0;
    const struct addrinfo *pick = res;
    for (const struct addrinfo *r = res; r; r = r->ai_next)
        if (r->ai_family == AF_INET) {
            pick = r;
            break;
        }
    memcpy(out, pick->ai_addr, pick->ai_addrlen);
    *len = (int)pick->ai_addrlen;
    freeaddrinfo(res);
    return 1;
}

static HostSock open_socket(const char *server) {
    struct sockaddr_storage addr;
    int len;
    if (!resolve(server, &addr, &len)) {
        rt_log("dsu: cannot resolve the server \"%s\" (MKW_DSU_SERVER, host:port)", server);
        return HOST_BAD;
    }
    HostSock s = socket(addr.ss_family, SOCK_DGRAM, IPPROTO_UDP);
    if (s == HOST_BAD) return HOST_BAD;
#ifdef _WIN32
    BOOL off = FALSE;   // no server yet: a port-unreachable reply would fail every later recv with WSAECONNRESET
    DWORD got = 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof off, NULL, 0, &got, NULL, NULL);
#endif
    // Non-blocking, so draining after a poll never waits; on Linux it also reads (and clears) the error a "port
    // unreachable" leaves on the socket while no server listens, which would otherwise make every poll return at once
    // with POLLERR and the thread spin.
#ifdef _WIN32
    u_long nb = 1;
    int nb_ok = !ioctlsocket(s, FIONBIO, &nb);
#else
    int fl = fcntl(s, F_GETFL, 0), nb_ok = fl >= 0 && !fcntl(s, F_SETFL, fl | O_NONBLOCK);
#endif
    if (!nb_ok || connect(s, (struct sockaddr *)&addr, len)) {   // connected: only the server's datagrams arrive
        host_close(s);
        return HOST_BAD;
    }
    rt_log("dsu: asking %s for motion (MKW_DSU_SLOT %d is player 1's)", server, cfg_int("MKW_DSU_SLOT"));
    return s;
}

static void dsu_main(void *arg) {
    (void)arg;
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa)) {
        rt_log("dsu: WSAStartup failed: no DSU motion");
        return;
    }
#endif
    HostSock s = HOST_BAD;
    char server[160] = "";
    unsigned seen = ~0u;
    int on = 0;
    uint32_t id = (uint32_t)plat_now_ns() ^ 0x4d4b5755u;
    uint64_t next_request = 0, retry = 0;
    uint8_t buf[512];
    for (;;) {
        unsigned v = cfg_version();
        if (v != seen) {
            seen = v;
            on = cfg_bool("MKW_DSU");
            const char *want = cfg_value("MKW_DSU_SERVER");
            if (strcmp(want, server) && s != HOST_BAD) host_close(s), s = HOST_BAD;
            snprintf(server, sizeof server, "%s", want);
            enabled = on;
            retry = 0;
        }
        if (!on) {
            if (s != HOST_BAD) host_close(s), s = HOST_BAD, rt_log("dsu: off");
            plat_sleep_ms(250);
            continue;
        }
        uint64_t now = plat_now_ns();
        if (s == HOST_BAD) {
            if (now < retry) {
                plat_sleep_ms(250);
                continue;
            }
            s = open_socket(server);
            if (s == HOST_BAD) {
                retry = now + 5000000000ull;
                continue;
            }
            next_request = 0;
        }
        if (now >= next_request) {   // pad data for every slot; the server sends for a few seconds after each one
            uint8_t req[28] = {0};
            header(req, "DSUC", sizeof req, id, DSU_PAD_DATA);
            seal(req, sizeof req);
            send(s, (const char *)req, sizeof req, 0);
            next_request = now + 1000000000ull;
        }
        struct pollfd pfd = {.fd = s, .events = POLLIN};
        if (host_poll(&pfd, 1, 100) <= 0) continue;
        // Drain on any event (POLLERR included): each slot keeps only its newest sample. An error (no server yet, a
        // truncated datagram) ends the drain; the next request a second later tries again.
        for (int i = 0; i < 256; i++) {
            int n = (int)recv(s, (char *)buf, sizeof buf, 0);
            if (n <= 0) break;
            receive(buf, n);
        }
    }
}

static void spawn(void *arg) {
    (void)arg;
    if (!plat_thread_spawn(dsu_main, NULL, "dsu", 0)) rt_log("dsu: cannot start its thread: no DSU motion");
}

void dsu_start(void) {
    static PlatOnce once = PLAT_ONCE_INIT;
    plat_once(&once, spawn, NULL);
}
