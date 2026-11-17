// Phones as controllers (runtime/input/phonepad_proto.h, phonepad_srv.c, phonepad_cert.c) and the QR code of their
// address (runtime/core/base/qr.c), without SDL, the game or a phone. tests/test_phonepad.py builds and runs it.
//
//   t_phonepad                                  the checks that need no socket: packets, the HTTP head, the motion's
//                                               axes, the QR code against python-qrcode's rows, the certificate
//   t_phonepad serve PORT CODE TLS [GONE_MS]    the server on 127.0.0.1:PORT with a table for controllers, each event
//                                               a line on stdout (ready, attach, state, detach, log), until killed
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "phonepad_srv.h"
#include "qr.h"

static int checks, fails;
#define CHECK(cond, ...)                                                                                              \
    do {                                                                                                              \
        checks++;                                                                                                     \
        if (!(cond)) {                                                                                                \
            fails++;                                                                                                  \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                               \
            printf(__VA_ARGS__);                                                                                      \
            printf("\n");                                                                                             \
        }                                                                                                             \
    } while (0)

static int near(double a, double b) { return fabs(a - b) < 1e-3; }

// ---- packets and the motion's axes

static void put16(uint8_t *p, int v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8); }

// input.c's input_motion_axes: SDL's sensor values to the Switch controller frame (acceleration in G as gravity reads)
static void switch_frame(const float a[3], const float g[3], double acc[3], double gyro[3]) {
    acc[0] = -a[0] / PPAD_GRAVITY, acc[1] = a[2] / PPAD_GRAVITY, acc[2] = -a[1] / PPAD_GRAVITY;
    gyro[0] = g[0], gyro[1] = -g[2], gyro[2] = g[1];
}

static void test_packet(void) {
    uint8_t d[PPAD_PACKET_LEN] = {PPAD_VERSION, PPAD_FLAG_MOTION};
    put16(d + 2, 1 << PPAD_A | 1 << PPAD_RSTICK);
    put16(d + 4, 100), put16(d + 6, -200), put16(d + 8, 32767), put16(d + 10, -32768);
    put16(d + 12, -500), put16(d + 14, 1000), put16(d + 16, 4903);
    put16(d + 18, 1000), put16(d + 20, -2000), put16(d + 22, 0);
    PpadState s;
    CHECK(ppad_packet_parse(d, sizeof d, &s), "a packet of this version");
    CHECK(s.buttons == (1 << PPAD_A | 1 << PPAD_RSTICK), "buttons %#x", s.buttons);
    CHECK(s.stick[0] == 100 && s.stick[1] == -200 && s.stick[2] == 32767, "sticks");
    CHECK(s.stick[3] == -32767, "-32768 has no positive twin: %d", s.stick[3]);
    CHECK(s.motion && near(s.acc[0], -1) && near(s.acc[1], 2) && near(s.acc[2], 9.806), "acceleration in m/s^2");
    CHECK(near(s.gyro[0], 1) && near(s.gyro[1], -2) && near(s.gyro[2], 0), "rotation in rad/s");
    CHECK(!ppad_packet_parse(d, sizeof d - 1, &s) && !ppad_packet_parse(d, sizeof d + 1, &s), "another length");
    d[0] = PPAD_VERSION + 1;
    CHECK(!ppad_packet_parse(d, sizeof d, &s), "another version");
    d[0] = PPAD_VERSION, d[1] = 0;
    CHECK(ppad_packet_parse(d, sizeof d, &s) && !s.motion && s.acc[2] == 0 && s.gyro[0] == 0, "no sensors: none read");
}

static void test_motion(void) {
    float a[3], g[3];
    double acc[3], gyro[3];
    // lying on a table, screen up: SDL's accelerometer reads +9.81 on y, the Switch's -1 G on z
    PpadState flat = {.motion = 1, .acc = {0, 0, PPAD_GRAVITY}};
    ppad_motion_sdl(&flat, a, g);
    CHECK(near(a[0], 0) && near(a[1], PPAD_GRAVITY) && near(a[2], 0), "flat in SDL's axes: %g %g %g", a[0], a[1], a[2]);
    switch_frame(a, g, acc, gyro);
    CHECK(near(acc[0], 0) && near(acc[1], 0) && near(acc[2], -1), "flat as the Switch reads it");
    ppad_motion_rest(a, g);
    CHECK(near(a[1], PPAD_GRAVITY) && a[0] == 0 && a[2] == 0 && g[0] == 0 && g[1] == 0 && g[2] == 0, "rest is lying flat");
    // held as a wheel (screen to the player) and turned 30 degrees clockwise: the push leans toward the left edge;
    // gravity then reads +0.5 G on the controller's x, as on a Joy-Con turned the same way
    PpadState wheel = {.motion = 1, .acc = {-PPAD_GRAVITY * 0.5f, PPAD_GRAVITY * 0.8660254f, 0}, .gyro = {0.1f, 0.2f, 0.3f}};
    ppad_motion_sdl(&wheel, a, g);
    switch_frame(a, g, acc, gyro);
    CHECK(near(acc[0], 0.5) && near(acc[1], -0.8660254) && near(acc[2], 0), "wheel: %g %g %g", acc[0], acc[1], acc[2]);
    CHECK(near(gyro[0], 0.1) && near(gyro[1], 0.2) && near(gyro[2], 0.3), "rotation keeps the controller's axes");
}

// ---- the HTTP head

static int parse(const char *text, PpadRequest *r) { return ppad_http_parse(text, strlen(text), r); }

static void test_http(void) {
    PpadRequest r;
    CHECK(parse("GET / HTTP/1.1\r\nHost: 192.168.1.5:6590\r\nUser-Agent: x\r\n\r\n", &r) == 1, "a GET");
    CHECK(!strcmp(r.method, "GET") && !strcmp(r.path, "/") && !r.query[0] && !r.body_len && !r.close, "its fields");
    CHECK(!strcmp(r.host, "192.168.1.5:6590") && r.head_len == 57, "host %s, head %zu", r.host, r.head_len);
    CHECK(parse("POST /i?s=0123456789abcdef HTTP/1.1\r\ncontent-LENGTH:  24 \r\n\r\nBODY", &r) == 1, "a POST");
    CHECK(!strcmp(r.method, "POST") && !strcmp(r.path, "/i") && !strcmp(r.query, "s=0123456789abcdef"), "path and query");
    CHECK(r.body_len == 24 && r.head_len == 61, "length %zu after a head of %zu", r.body_len, r.head_len);
    CHECK(parse("GET / HTTP/1.1\r\nHost: a\r\n", &r) == 0, "a head still arriving");
    CHECK(parse("GET / HTTP/1.1\r\nConnection: close\r\n\r\n", &r) == 1 && r.close, "Connection: close");
    CHECK(parse("GET / HTTP/1.0\r\n\r\n", &r) == 1 && r.close, "HTTP/1.0 closes");
    CHECK(parse("GET / HTTP/1.0\r\nConnection: Keep-Alive\r\n\r\n", &r) == 1 && !r.close, "unless it asks not to");
    CHECK(parse("POST /i HTTP/1.1\r\nContent-Length: 257\r\n\r\n", &r) == -1, "a body over the limit");
    CHECK(parse("POST /i HTTP/1.1\r\nContent-Length: 2x\r\n\r\n", &r) == -1, "a length that is no number");
    CHECK(parse("GET  HTTP/1.1\r\n\r\n", &r) == -1 && parse("GET x HTTP/1.1\r\n\r\n", &r) == -1, "no path");
    CHECK(parse("GET / SPDY/3\r\n\r\n", &r) == -1 && parse("\r\n\r\n", &r) == -1, "not HTTP/1.x");
    CHECK(parse("GET /0123456789012345678901234567890123456789 HTTP/1.1\r\n\r\n", &r) == -1, "a long path");
    static char big[PPAD_HEAD_MAX + 1];
    memset(big, 'a', PPAD_HEAD_MAX);
    CHECK(ppad_http_parse(big, PPAD_HEAD_MAX - 1, &r) == 0 && ppad_http_parse(big, PPAD_HEAD_MAX, &r) == -1, "a head with no end");

    char v[8];
    CHECK(ppad_query("code=1234&id=ab", "code", v, sizeof v) && !strcmp(v, "1234"), "the first key");
    CHECK(ppad_query("code=1234&id=ab", "id", v, sizeof v) && !strcmp(v, "ab"), "the last key");
    CHECK(!ppad_query("sid=1", "s", v, sizeof v) && !ppad_query("", "s", v, sizeof v), "a key that is not there");
    CHECK(!ppad_query("s=12345678", "s", v, sizeof v) && ppad_query("s=1234567", "s", v, sizeof v), "a value that fits");

    CHECK(ppad_local_ipv4(0xC0A80105) && ppad_local_ipv4(0x0A000001) && ppad_local_ipv4(0xAC100001) && ppad_local_ipv4(0xAC1FFFFE),
          "home networks");
    CHECK(ppad_local_ipv4(0xA9FE0101) && ppad_local_ipv4(0x7F000001), "link-local and loopback");
    CHECK(!ppad_local_ipv4(0x08080808) && !ppad_local_ipv4(0xAC200001) && !ppad_local_ipv4(0xC0A90001) && !ppad_local_ipv4(0x0B000001),
          "the internet");
    CHECK(ppad_host_ok("192.168.1.5:6590") && ppad_host_ok("pc.local") && ppad_host_ok("[::1]:80"), "hosts");
    CHECK(!ppad_host_ok("") && !ppad_host_ok("a/b") && !ppad_host_ok("a\r\nSet-Cookie: x") && !ppad_host_ok("a b"), "not hosts");
}

// ---- the QR code

static const char QR_TEXT[] = "https://192.168.100.20:6590/#4821";
// python-qrcode 8.2 for QR_TEXT in byte mode, version 3, level M, masks 2 and 5 (qrcode.QRCode(version=3,
// error_correction=ERROR_CORRECT_M, mask_pattern=N), QRData(mode=MODE_8BIT_BYTE), make(fit=False))
static const char *const QR_MASK2[29] = {
    "11111110001110011101001111111",
    "10000010011100000110001000001",
    "10111010101000101010001011101",
    "10111010101100001110101011101",
    "10111010101011110001001011101",
    "10000010100011111100001000001",
    "11111110101010101010101111111",
    "00000000100101010011000000000",
    "10111110011000110110101111100",
    "11101101000100011001001010001",
    "00110010110010000000010000000",
    "10001001000010101001100101010",
    "00100111110010000111010001100",
    "10100101110001110101001010001",
    "01101110100001111010000001100",
    "11101101101111010000011010010",
    "10011110111010111110110101100",
    "11001000101100000001001010101",
    "10100110010100010010011110100",
    "10011100010010100010010000010",
    "10100110000100011110111110111",
    "00000000110011100111100011111",
    "11111110011011110101101011100",
    "10000010110011010000100010000",
    "10111010100100101110111110110",
    "10111010101001000011010101111",
    "10111010101000010110011011110",
    "10000010010101011001101110010",
    "11111110111000011110011011100",
};
static const char *const QR_MASK5[29] = {
    "11111110000011110000101111111",
    "10000010101100010110001000001",
    "10111010101000101010001011101",
    "10111010110100110110001011101",
    "10111010001011110001001011101",
    "10000010010011101100001000001",
    "11111110101010101010101111111",
    "00000000110101000011000000000",
    "10000010111000110110111001110",
    "11010101111100100001110010110",
    "00110010110010000000010000000",
    "10011001010010111001110101000",
    "01001010011111101010111100001",
    "10110101100001100101011010011",
    "01101110100001111010000001100",
    "11010101010111101000100010101",
    "10011110111010111110110101100",
    "11011000111100010001011010111",
    "11001011111001111111110011001",
    "10001100000010110010000000000",
    "10100110000100011110111110111",
    "00000000101011011111100011000",
    "11111110011011110101101011100",
    "10000010000011000000100010010",
    "10111010001001000011111111011",
    "10111010011001010011000101101",
    "10111010001000010110011011110",
    "10000010001101100001010110101",
    "11111110111000011110011011100",
};

static int qr_same(const uint8_t *m, int size, const char *const *rows) {
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++)
            if (m[y * size + x] != rows[y][x] - '0') return 0;
    return 1;
}

static void test_qr(void) {
    static uint8_t m[QR_MAX_SIZE * QR_MAX_SIZE], forced[QR_MAX_SIZE * QR_MAX_SIZE];
    CHECK(qr_encode(QR_TEXT, 2, m) == 29 && qr_same(m, 29, QR_MASK2), "mask 2 as python-qrcode draws it");
    CHECK(qr_encode(QR_TEXT, 5, m) == 29 && qr_same(m, 29, QR_MASK5), "mask 5 as python-qrcode draws it");
    int size = qr_encode(QR_TEXT, -1, m), same = 0;
    for (int k = 0; k < 8; k++) same += qr_encode(QR_TEXT, k, forced) == size && !memcmp(m, forced, (size_t)(size * size));
    CHECK(size == 29 && same == 1, "the picked mask is one of the eight: %d match", same);
    static const struct { int len, size; } fit[] = {{1, 21}, {14, 21}, {15, 25}, {26, 25}, {27, 29}, {42, 29},
                                                   {43, 33}, {78, 33}, {79, 37}, {106, 37}, {107, 0}};
    for (size_t i = 0; i < sizeof fit / sizeof *fit; i++) {
        char text[128] = {0};
        memset(text, 'k', (size_t)fit[i].len);
        CHECK(qr_encode(text, -1, m) == fit[i].size, "%d bytes: %d modules a side", fit[i].len, fit[i].size);
    }
}

// ---- the certificate

static void test_cert(void) {
    static PhoneCert c;
    const uint8_t ip[4] = {192, 168, 1, 5}, other[4] = {192, 168, 1, 6};
    int64_t now = (int64_t)time(NULL);
    CHECK(phonecert_make(ip, now, &c), "a key and a certificate");
    CHECK(c.der_len > 300 && c.der[0] == 0x30 && c.der[1] == 0x82 && (size_t)(c.der[2] << 8 | c.der[3]) + 4 == c.der_len,
          "one DER sequence of %u bytes", c.der_len);
    CHECK(phonecert_usable(&c, sizeof c, ip, now) && phonecert_usable(&c, sizeof c, ip, now + 700 * 86400ll), "usable");
    CHECK(!phonecert_usable(&c, sizeof c, other, now), "made for another address");
    CHECK(!phonecert_usable(&c, sizeof c, ip, now + 771 * 86400ll), "a month from its end");
    CHECK(!phonecert_usable(&c, sizeof c - 1, ip, now), "a file cut short");
    static PhoneCert d;
    CHECK(phonecert_make(ip, now, &d) && memcmp(c.key, d.key, PPAD_KEY_LEN), "another key each time");
}

// ---- serve: the server with a table for controllers

static void *table[PPAD_MAX_PADS];
static uint16_t table_buttons[PPAD_MAX_PADS];

static int slot(void *pad) { return (int)((void **)pad - table); }

static void *t_attach(void *ctx, const char *peer) {
    (void)ctx;
    for (int i = 0; i < PPAD_MAX_PADS; i++)
        if (!table[i]) {
            table[i] = &table[i];
            printf("attach %d %s\n", i, peer);
            return &table[i];
        }
    return NULL;
}

static void t_detach(void *ctx, void *pad) {
    (void)ctx;
    printf("detach %d\n", slot(pad));
    table[slot(pad)] = NULL;
}

static void t_state(void *ctx, void *pad, const PpadState *s) {
    (void)ctx;
    table_buttons[slot(pad)] = s->buttons;
    printf("state %d %04x %d %d %d %d %d %.3f %.3f %.3f %.3f %.3f %.3f\n", slot(pad), s->buttons, s->stick[0], s->stick[1],
           s->stick[2], s->stick[3], s->motion != 0, s->acc[0], s->acc[1], s->acc[2], s->gyro[0], s->gyro[1], s->gyro[2]);
}

static void t_feedback(void *ctx, void *pad, uint8_t out[PPAD_REPLY_LEN]) {
    (void)ctx;
    int a = table_buttons[slot(pad)] >> PPAD_A & 1;   // the motors run while A is held
    out[0] = (uint8_t)(a ? 0x10 + slot(pad) : 0), out[1] = (uint8_t)(a ? 0x80 : 0), out[2] = (uint8_t)(slot(pad) + 1);
}

static void t_log(void *ctx, const char *line) {
    (void)ctx;
    printf("log %s\n", line);
}

static int serve(int port, const char *code, int tls, int gone_ms) {
    static PhoneCert cert;
    PhoneSrvConfig cfg = {.bind = {127, 0, 0, 1}, .port = (uint16_t)port, .gone_ms = gone_ms, .verbose = 1};
    snprintf(cfg.code, sizeof cfg.code, "%s", code);
    if (tls) {
        if (!phonecert_make(cfg.bind, (int64_t)time(NULL), &cert)) return puts("no certificate"), 1;
        cfg.cert = &cert;
    }
    PhoneSink sink = {NULL, t_attach, t_detach, t_state, t_feedback, t_log};
    setvbuf(stdout, NULL, _IOLBF, 0);
    PhoneSrv *s = phonesrv_open(&cfg, &sink);
    if (!s) return puts("no server"), 1;
    printf("ready %d ", port);
    for (uint32_t i = 0; tls && i < cert.der_len; i++) printf("%02x", cert.der[i]);
    printf("\n");
    fflush(stdout);
    for (;;) {
        phonesrv_poll(s, 20);
        fflush(stdout);
    }
}

int main(int argc, char **argv) {
    if (argc >= 5 && !strcmp(argv[1], "serve"))
        return serve(atoi(argv[2]), argv[3], atoi(argv[4]), argc > 5 ? atoi(argv[5]) : 0);
    test_packet();
    test_motion();
    test_http();
    test_qr();
    test_cert();
    printf("t_phonepad: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
