// A phone as a controller (phonepad.h): what the phone's page (phonepad_page.html) and the server (phonepad_srv.c)
// say to each other, without sockets or SDL, so tests/runtime/t_phonepad.c checks it alone.
//
// Why HTTPS requests and not a WebSocket: a browser hands a page the phone's gyroscope and accelerometer only in a
// secure context (Chrome on Android since 76; Safari on iOS since 13, where DeviceMotionEvent.requestPermission also
// needs it), and a page on a LAN address is one only over HTTPS. The certificate is this PC's own (phonepad_cert.c),
// so the player accepts it once; Safari on iOS then lets the page's own requests through but refuses a wss:// socket
// to the same certificate. So the page POSTs its state and reads the rumble from the answer, one request at a time
// on a connection kept open.
//
//   POST /join?code=NNNN       -> 200, the session id (16 hex digits); 403 wrong code; 503 no controller left
//   POST /i?s=<session>        body: one packet (PPAD_PACKET_LEN bytes) -> 200, PPAD_REPLY_LEN bytes; 410 unknown session
//   POST /bye?s=<session>      -> 204, the controller goes at once (the page closed)
//   GET  /                     -> the page
//
// A packet, little-endian: version (PPAD_VERSION), flags (PPAD_FLAG_*), u16 buttons (bit PPAD_A..), i16 left stick x, y,
// right stick x, y (-32767..32767, right and up positive), i16 acceleration x, y, z (1 / PPAD_ACC_SCALE m/s^2),
// i16 rotation x, y, z (1 / PPAD_GYRO_SCALE rad/s). The motion is in the controller's axes with the phone held in
// landscape, its screen as the controller's face: x to the right, y toward the top edge, z out of the screen; the
// acceleration is the push against gravity (lying flat, screen up: z = +9.81), the rotation counter-clockwise positive.
// The page turns what each browser reports into that (Safari's acceleration has the opposite sign, and the screen's
// rotation swaps the axes). A reply: rumble low, rumble high (0..255), the player (1-8, 0 not known yet), 0.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { PPAD_VERSION = 1, PPAD_PACKET_LEN = 24, PPAD_REPLY_LEN = 4, PPAD_CODE_LEN = 4, PPAD_SID_LEN = 16 };
enum { PPAD_FLAG_MOTION = 1 };   // the packet carries the phone's sensors (the player allowed them)
enum { PPAD_A, PPAD_B, PPAD_X, PPAD_Y, PPAD_L, PPAD_R, PPAD_ZL, PPAD_ZR, PPAD_PLUS, PPAD_MINUS, PPAD_UP, PPAD_DOWN, PPAD_LEFT, PPAD_RIGHT,
       PPAD_LSTICK, PPAD_RSTICK, PPAD_BUTTON_COUNT };
#define PPAD_ACC_SCALE 500.0f     // +-65 m/s^2 in an i16: a shaken phone still fits
#define PPAD_GYRO_SCALE 1000.0f   // +-32 rad/s
#define PPAD_GRAVITY 9.80665f

typedef struct {
    uint16_t buttons;
    int16_t stick[4];   // lx, ly, rx, ry
    int motion;         // acc and gyro are the phone's
    float acc[3];       // m/s^2, the push against gravity, controller axes
    float gyro[3];      // rad/s, controller axes
} PpadState;

static inline int16_t ppad_i16(const uint8_t *p) { return (int16_t)(p[0] | p[1] << 8); }
static inline int16_t ppad_stick(const uint8_t *p) {
    int16_t v = ppad_i16(p);
    return v == INT16_MIN ? -INT16_MAX : v;   // -32768 has no positive twin: SDL's axis negated would overflow
}

// 1 when d is a packet of this version
static inline int ppad_packet_parse(const uint8_t *d, size_t n, PpadState *out) {
    if (n != PPAD_PACKET_LEN || d[0] != PPAD_VERSION) return 0;
    memset(out, 0, sizeof *out);
    out->buttons = (uint16_t)(d[2] | d[3] << 8);
    for (int i = 0; i < 4; i++) out->stick[i] = ppad_stick(d + 4 + i * 2);
    out->motion = d[1] & PPAD_FLAG_MOTION;
    for (int i = 0; i < 3; i++) {
        out->acc[i] = out->motion ? (float)ppad_i16(d + 12 + i * 2) / PPAD_ACC_SCALE : 0;
        out->gyro[i] = out->motion ? (float)ppad_i16(d + 18 + i * 2) / PPAD_GYRO_SCALE : 0;
    }
    return 1;
}

// The packet's motion in SDL's gamepad sensor axes (SDL_sensor.h: x to the right, y out of the face, z toward the
// player; the accelerometer reads the push against gravity), the inverse of input.c's input_motion_axes as
// switch2.c's push_motion is: SDL x = controller x, SDL y = controller z, SDL z = -controller y.
static inline void ppad_motion_sdl(const PpadState *s, float acc[3], float gyro[3]) {
    acc[0] = s->acc[0], acc[1] = s->acc[2], acc[2] = -s->acc[1];
    gyro[0] = s->gyro[0], gyro[1] = s->gyro[2], gyro[2] = -s->gyro[1];
}

// A controller lying still, face up, in SDL's axes: what a phone without sensors reads as
static inline void ppad_motion_rest(float acc[3], float gyro[3]) {
    acc[0] = 0, acc[1] = PPAD_GRAVITY, acc[2] = 0;
    gyro[0] = gyro[1] = gyro[2] = 0;
}

// An IPv4 address (host order) that never leaves a local network: 10/8, 172.16/12, 192.168/16, link-local
// 169.254/16 and loopback. The server answers no other peer.
static inline int ppad_local_ipv4(uint32_t h) {
    return (h >> 24) == 10 || (h >> 20) == 0xAC1 || (h >> 16) == 0xC0A8 || (h >> 16) == 0xA9FE || (h >> 24) == 127;
}

// ---- HTTP: one request head, as browsers send it

enum { PPAD_HEAD_MAX = 8192, PPAD_BODY_MAX = 256 };

typedef struct {
    char method[8], path[32], query[96], host[96];
    size_t head_len, body_len;   // bytes up to and including the empty line; Content-Length
    int close;                   // the client asked to close after this answer
} PpadRequest;

static inline int ppad_lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

// The value of header name (lower case) on the line [s, e), trimmed, into out; 0 when the line is another header
static inline int ppad_header(const char *s, const char *e, const char *name, char *out, size_t cap) {
    size_t n = strlen(name);
    if ((size_t)(e - s) <= n || s[n] != ':') return 0;
    for (size_t i = 0; i < n; i++)
        if (ppad_lower((unsigned char)s[i]) != name[i]) return 0;
    s += n + 1;
    while (s < e && (*s == ' ' || *s == '\t')) s++;
    while (e > s && (e[-1] == ' ' || e[-1] == '\t')) e--;
    size_t len = (size_t)(e - s);
    if (len >= cap) len = cap - 1;
    memcpy(out, s, len);
    out[len] = 0;
    return 1;
}

// 1: a whole head is in buf[0..n) and r describes it; 0: more bytes are needed; -1: not a request this server takes
// (malformed, a head over PPAD_HEAD_MAX, a body over PPAD_BODY_MAX).
static inline int ppad_http_parse(const char *buf, size_t n, PpadRequest *r) {
    size_t end = 0;
    for (size_t i = 3; i < n && !end; i++)
        if (buf[i] == '\n' && buf[i - 1] == '\r' && buf[i - 2] == '\n' && buf[i - 3] == '\r') end = i + 1;
    if (!end) return n >= PPAD_HEAD_MAX ? -1 : 0;
    memset(r, 0, sizeof *r);
    r->head_len = end;
    const char *p = buf, *eol = memchr(buf, '\r', end);
    const char *sp1 = memchr(p, ' ', (size_t)(eol - p));
    if (!sp1 || (size_t)(sp1 - p) >= sizeof r->method) return -1;
    memcpy(r->method, p, (size_t)(sp1 - p));
    const char *t = sp1 + 1, *sp2 = memchr(t, ' ', (size_t)(eol - t));
    if (!sp2 || t == sp2 || *t != '/' || strncmp(sp2 + 1, "HTTP/1.", 7)) return -1;
    const char *q = memchr(t, '?', (size_t)(sp2 - t)), *path_end = q ? q : sp2;
    if ((size_t)(path_end - t) >= sizeof r->path || (q && (size_t)(sp2 - q - 1) >= sizeof r->query)) return -1;
    memcpy(r->path, t, (size_t)(path_end - t));
    if (q) memcpy(r->query, q + 1, (size_t)(sp2 - q - 1));
    r->close = sp2[8] == '0';   // HTTP/1.0 closes unless told otherwise
    for (const char *line = eol + 2; line < buf + end - 2;) {
        const char *le = memchr(line, '\r', (size_t)(buf + end - line));
        char v[32];
        if (ppad_header(line, le, "content-length", v, sizeof v)) {
            char *stop;
            unsigned long len = strtoul(v, &stop, 10);
            if (stop == v || *stop || len > PPAD_BODY_MAX) return -1;
            r->body_len = len;
        } else if (ppad_header(line, le, "connection", v, sizeof v)) {
            for (char *c = v; *c; c++) *c = (char)ppad_lower((unsigned char)*c);
            r->close = strstr(v, "close") != NULL ? 1 : strstr(v, "keep-alive") != NULL ? 0 : r->close;
        } else {
            ppad_header(line, le, "host", r->host, sizeof r->host);
        }
        line = le + 2;
    }
    return 1;
}

// The value of key in a query string ("code=1234&x=1") into out; 0 when it is not there or does not fit
static inline int ppad_query(const char *query, const char *key, char *out, size_t cap) {
    size_t k = strlen(key);
    for (const char *p = query; *p;) {
        const char *amp = strchr(p, '&'), *e = amp ? amp : p + strlen(p);
        if ((size_t)(e - p) > k && !strncmp(p, key, k) && p[k] == '=') {
            size_t len = (size_t)(e - p) - k - 1;
            if (len >= cap) return 0;
            memcpy(out, p + k + 1, len);
            out[len] = 0;
            return 1;
        }
        p = amp ? amp + 1 : e;
    }
    return 0;
}

// A Host header fit to go back in a Location header: a name or an address with an optional port, nothing else
static inline int ppad_host_ok(const char *host) {
    if (!*host) return 0;
    for (const char *p = host; *p; p++) {
        int c = (unsigned char)*p;
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '-' ||
              c == ':' || c == '[' || c == ']'))
            return 0;
    }
    return 1;
}
