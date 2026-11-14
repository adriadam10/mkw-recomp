// The phone controllers' server (phonepad_srv.h): HTTPS on one port of the LAN, serving the controller's page
// (phonepad_page.html) and taking each phone's state (phonepad_proto.h). One thread, non-blocking sockets and
// BearSSL's engine (third_party/bearssl, MIT) fed by hand, so nothing waits on a slow phone and the runtime gains no
// thread per connection. TLS 1.2 with the PC's own certificate (phonepad_cert.c).
//
// A connection whose first byte is not a TLS handshake is a browser that was given the address without https://:
// it is sent to the same address with it. Who may join: an address of a local network that sends the code the menu
// shows; eight wrong codes close the door for ten seconds. A phone that says nothing for 0.7 s lets go of every
// button (its browser went to the background, the Wi-Fi dropped); after ten seconds its controller goes.
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET Sock;
#define SOCK_BAD INVALID_SOCKET
#define sock_close closesocket
#define sock_poll WSAPoll
#define SEND_FLAGS 0
static int sock_again(void) {
    int e = WSAGetLastError();
    return e == WSAEWOULDBLOCK || e == WSAEINTR;
}
static void sock_nonblock(Sock s) {
    u_long on = 1;
    ioctlsocket(s, FIONBIO, &on);
}
static uint64_t now_ms(void) { return GetTickCount64(); }
static void nap_ms(int ms) { Sleep((DWORD)ms); }
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
typedef int Sock;
#define SOCK_BAD (-1)
#define sock_close close
#define sock_poll poll
#define SEND_FLAGS MSG_NOSIGNAL   // a phone that left must not end the game with SIGPIPE
static int sock_again(void) { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR; }
static void sock_nonblock(Sock s) { fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK); }
static uint64_t now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}
static void nap_ms(int ms) { poll(NULL, 0, ms); }
#endif

#include "bearssl.h"
#include "phonepad_srv.h"

// The page. tests/test_phonepad.py builds with PHONEPAD_TEST_PAGE: a compiler without #embed (GCC before 15, Clang
// before 19) still builds the server, and the test knows what to expect.
#ifdef PHONEPAD_TEST_PAGE
static const char page[] = "<!doctype html><title>phone controller test page</title>";
#define PAGE_LEN (sizeof page - 1)
#else
#ifdef __clang__
#pragma clang diagnostic ignored "-Wc23-extensions"   // #embed before C23
#endif
static const char page[] = {
#embed "phonepad_page.html"
};
#define PAGE_LEN sizeof page
#endif

enum {
    MAX_CONNS = 24,                        // a browser opens a few at once; eight phones
    IN_MAX = PPAD_HEAD_MAX + PPAD_BODY_MAX,
    NEUTRAL_MS = 700, GONE_MS = 10000,     // a silent phone: buttons released, then its controller gone
    IDLE_MS = 30000,                       // a connection nobody uses
    JOIN_FAILS = 8, JOIN_LOCK_MS = 10000,  // wrong codes: 10^4 codes then take hours to try
};
enum { KIND_NEW, KIND_PLAIN, KIND_TLS };

typedef struct {
    int used, kind;
    Sock fd;
    uint64_t last_ms;
    char peer[20];
    char in[IN_MAX];              // request bytes not answered yet
    size_t in_len;
    uint8_t *out;                 // the answer being sent
    size_t out_len, out_off;
    int close_after;              // close once it is sent
    br_ssl_server_context *tls;   // with its record buffer behind it, one allocation
} Conn;

typedef struct {
    int used, neutral;
    char sid[PPAD_SID_LEN + 1], client[PPAD_SID_LEN + 1];   // client: the page's own id, the same after a reload
    void *pad;
    uint64_t last_ms;
} Session;

struct PhoneSrv {
    PhoneSrvConfig cfg;
    PhoneSink sink;
    Sock listener;
    Conn conns[MAX_CONNS];
    Session sessions[PPAD_MAX_PADS];
    int fails;
    uint64_t locked_until;
    br_hmac_drbg_context rng;     // session ids
    br_x509_certificate chain;
    br_ec_private_key key;
};

static void say(PhoneSrv *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void say(PhoneSrv *s, const char *fmt, ...) {
    if (!s->sink.log) return;
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    s->sink.log(s->sink.ctx, line);
}

int phonesrv_pads(const PhoneSrv *s) {
    int n = 0;
    for (int i = 0; i < PPAD_MAX_PADS; i++) n += s->sessions[i].used;
    return n;
}

// ---- connections

static void conn_close(PhoneSrv *s, Conn *c) {
    if (s->cfg.verbose && c->kind == KIND_TLS) {
        int err = br_ssl_engine_last_error(&c->tls->eng);
        if (err) say(s, "phone: %s: TLS closed with BearSSL error %d (a phone refusing the certificate ends so)", c->peer, err);
    }
    sock_close(c->fd);
    free(c->out);
    free(c->tls);
    memset(c, 0, sizeof *c);
}

static const char *status_text(int status) {
    switch (status) {
    case 200: return "OK";
    case 204: return "No Content";
    case 301: return "Moved Permanently";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 410: return "Gone";
    case 429: return "Too Many Requests";
    case 503: return "Service Unavailable";
    }
    return "Not Found";
}

// The answer to the request being handled; location: where a 301 sends the browser
static void reply(Conn *c, int status, const char *type, const void *body, size_t n, const char *location) {
    char head[400];
    int h = snprintf(head, sizeof head, "HTTP/1.1 %d %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\n", status,
                     status_text(status), n);
    if (type) h += snprintf(head + h, sizeof head - (size_t)h, "Content-Type: %s\r\n", type);
    if (location) h += snprintf(head + h, sizeof head - (size_t)h, "Location: %s\r\n", location);
    h += snprintf(head + h, sizeof head - (size_t)h, "Connection: %s\r\n\r\n", c->close_after ? "close" : "keep-alive");
    c->out = malloc((size_t)h + n);
    if (!c->out) {   // out of memory: the connection goes without an answer
        c->close_after = 1;
        return;
    }
    memcpy(c->out, head, (size_t)h);
    if (n) memcpy(c->out + h, body, n);
    c->out_len = (size_t)h + n, c->out_off = 0;
}

static void reply_text(Conn *c, int status, const char *text) { reply(c, status, "text/plain", text, strlen(text), NULL); }

// ---- sessions

static Session *session_by(PhoneSrv *s, const char *sid) {
    for (int i = 0; i < PPAD_MAX_PADS; i++)
        if (s->sessions[i].used && !strcmp(s->sessions[i].sid, sid)) return &s->sessions[i];
    return NULL;
}

static void session_end(PhoneSrv *s, Session *se, const char *why) {
    s->sink.detach(s->sink.ctx, se->pad);
    memset(se, 0, sizeof *se);
    say(s, "phone: a phone left (%s), %d connected", why, phonesrv_pads(s));
}

static int hex_ok(const char *v) {   // 1 to PPAD_SID_LEN hex digits
    size_t n = strlen(v);
    if (!n || n > PPAD_SID_LEN) return 0;
    for (size_t i = 0; i < n; i++)
        if (!((v[i] >= '0' && v[i] <= '9') || (v[i] >= 'a' && v[i] <= 'f'))) return 0;
    return 1;
}

static void join(PhoneSrv *s, Conn *c, const PpadRequest *r) {
    char code[PPAD_CODE_LEN + 1] = "", client[PPAD_SID_LEN + 1] = "";
    uint64_t now = now_ms();
    if (now < s->locked_until) {
        reply_text(c, 429, "wait\n");
        return;
    }
    int same = ppad_query(r->query, "code", code, sizeof code) && strlen(code) == PPAD_CODE_LEN;
    for (int i = 0; same && i < PPAD_CODE_LEN; i++) same = code[i] == s->cfg.code[i];
    if (!same) {
        if (++s->fails >= JOIN_FAILS) {
            s->fails = 0, s->locked_until = now + JOIN_LOCK_MS;
            say(s, "phone: %d wrong codes (the last from %s): no phone joins for %d s", JOIN_FAILS, c->peer, JOIN_LOCK_MS / 1000);
        }
        reply_text(c, 403, "code\n");
        return;
    }
    s->fails = 0;
    if (!ppad_query(r->query, "id", client, sizeof client) || !hex_ok(client)) client[0] = 0;
    Session *se = NULL;
    for (int i = 0; client[0] && i < PPAD_MAX_PADS; i++)   // the same page again (reloaded, back from sleep): its controller
        if (s->sessions[i].used && !strcmp(s->sessions[i].client, client)) se = &s->sessions[i];
    if (!se) {
        for (int i = 0; i < PPAD_MAX_PADS && !se; i++)
            if (!s->sessions[i].used) se = &s->sessions[i];
        void *pad = se ? s->sink.attach(s->sink.ctx, c->peer) : NULL;
        if (!pad) {
            reply_text(c, 503, "full\n");
            return;
        }
        uint8_t id[PPAD_SID_LEN / 2];
        br_hmac_drbg_generate(&s->rng, id, sizeof id);
        memset(se, 0, sizeof *se);
        for (size_t i = 0; i < sizeof id; i++) snprintf(se->sid + i * 2, 3, "%02x", id[i]);
        snprintf(se->client, sizeof se->client, "%s", client);
        se->used = 1, se->pad = pad;
        say(s, "phone: %s joined as a controller, %d connected", c->peer, phonesrv_pads(s));
    }
    se->last_ms = now;
    reply_text(c, 200, se->sid);
}

static void input(PhoneSrv *s, Conn *c, const PpadRequest *r, const uint8_t *body) {
    char sid[PPAD_SID_LEN + 1];
    Session *se = ppad_query(r->query, "s", sid, sizeof sid) ? session_by(s, sid) : NULL;
    PpadState st;
    if (!se) {   // the page joins again
        reply_text(c, 410, "join\n");
        return;
    }
    if (!ppad_packet_parse(body, r->body_len, &st)) {
        reply_text(c, 400, "packet\n");
        return;
    }
    s->sink.state(s->sink.ctx, se->pad, &st);
    se->last_ms = now_ms(), se->neutral = 0;
    uint8_t fb[PPAD_REPLY_LEN] = {0};
    s->sink.feedback(s->sink.ctx, se->pad, fb);
    reply(c, 200, "application/octet-stream", fb, sizeof fb, NULL);
}

static void handle(PhoneSrv *s, Conn *c, const PpadRequest *r, const uint8_t *body) {
    int get = !strcmp(r->method, "GET"), post = !strcmp(r->method, "POST");
    if (r->close) c->close_after = 1;
    if (c->kind == KIND_PLAIN && s->cfg.cert) {   // typed without https://: the same address with it
        char to[128];
        c->close_after = 1;
        snprintf(to, sizeof to, "https://%s/", r->host);
        if (ppad_host_ok(r->host)) reply(c, 301, NULL, NULL, 0, to);
        else reply_text(c, 400, "host\n");
    } else if (get && !strcmp(r->path, "/")) {
        reply(c, 200, "text/html; charset=utf-8", page, PAGE_LEN, NULL);
    } else if (post && !strcmp(r->path, "/join")) {
        join(s, c, r);
    } else if (post && !strcmp(r->path, "/i")) {
        input(s, c, r, body);
    } else if (post && !strcmp(r->path, "/bye")) {
        char sid[PPAD_SID_LEN + 1];
        Session *se = ppad_query(r->query, "s", sid, sizeof sid) ? session_by(s, sid) : NULL;
        if (se) session_end(s, se, "its page closed");
        reply(c, 204, NULL, NULL, 0, NULL);
    } else {
        reply_text(c, 404, "not found\n");
    }
}

// Answers the requests that are whole in c->in, one at a time: the next waits until this answer is sent
static void requests(PhoneSrv *s, Conn *c) {
    while (!c->out && !c->close_after && c->in_len) {
        PpadRequest r;
        int got = ppad_http_parse(c->in, c->in_len, &r);
        if (got < 0 || (!got && c->in_len == IN_MAX)) {
            c->close_after = 1;
            reply_text(c, 400, "bad request\n");
            return;
        }
        size_t total = r.head_len + r.body_len;
        if (!got || c->in_len < total) return;
        handle(s, c, &r, (const uint8_t *)c->in + r.head_len);
        memmove(c->in, c->in + total, c->in_len - total);
        c->in_len -= total;
    }
}

static void out_done(Conn *c) {
    free(c->out);
    c->out = NULL, c->out_len = c->out_off = 0;
}

// 0 when the connection was closed
static int pump_plain(PhoneSrv *s, Conn *c) {
    for (;;) {
        int n = c->in_len < IN_MAX ? (int)recv(c->fd, c->in + c->in_len, (int)(IN_MAX - c->in_len), 0) : -2;
        if (n == 0 || (n == -1 && !sock_again())) return conn_close(s, c), 0;
        if (n > 0) c->in_len += (size_t)n, c->last_ms = now_ms();
        requests(s, c);
        while (c->out && c->out_off < c->out_len) {
            int w = (int)send(c->fd, (const char *)c->out + c->out_off, (int)(c->out_len - c->out_off), SEND_FLAGS);
            if (w < 0 && sock_again()) return 1;   // the socket is full: poll says when it takes more
            if (w <= 0) return conn_close(s, c), 0;
            c->out_off += (size_t)w;
        }
        if (c->out) out_done(c);
        if (c->close_after) return conn_close(s, c), 0;
        if (n <= 0) return 1;   // nothing more arrived and nothing is left to answer
    }
}

// The engine says what it can take or give (its state changes with every step, so each step asks again); the loop
// ends when a pass moved nothing: the socket has no more to read or cannot take more, and no request is left.
static int pump_tls(PhoneSrv *s, Conn *c) {
    br_ssl_engine_context *e = &c->tls->eng;
    for (int progress = 1; progress;) {
        progress = 0;
        size_t len;
        unsigned char *buf;
        if (br_ssl_engine_current_state(e) & BR_SSL_CLOSED) return conn_close(s, c), 0;
        if (br_ssl_engine_current_state(e) & BR_SSL_SENDREC) {
            buf = br_ssl_engine_sendrec_buf(e, &len);
            int w = (int)send(c->fd, (const char *)buf, (int)len, SEND_FLAGS);
            if (w > 0) br_ssl_engine_sendrec_ack(e, (size_t)w), progress = 1;
            else if (w == 0 || !sock_again()) return conn_close(s, c), 0;
        }
        if (br_ssl_engine_current_state(e) & BR_SSL_RECVREC) {
            buf = br_ssl_engine_recvrec_buf(e, &len);
            int n = (int)recv(c->fd, (char *)buf, (int)len, 0);
            if (n > 0) br_ssl_engine_recvrec_ack(e, (size_t)n), c->last_ms = now_ms(), progress = 1;
            else if (n == 0 || !sock_again()) return conn_close(s, c), 0;
        }
        if ((br_ssl_engine_current_state(e) & BR_SSL_RECVAPP) && c->in_len < IN_MAX) {
            buf = br_ssl_engine_recvapp_buf(e, &len);
            if (len > IN_MAX - c->in_len) len = IN_MAX - c->in_len;
            memcpy(c->in + c->in_len, buf, len);
            c->in_len += len;
            br_ssl_engine_recvapp_ack(e, len);
            progress = 1;
        }
        requests(s, c);
        if (c->out && (br_ssl_engine_current_state(e) & BR_SSL_SENDAPP)) {
            buf = br_ssl_engine_sendapp_buf(e, &len);
            if (len > c->out_len - c->out_off) len = c->out_len - c->out_off;
            memcpy(buf, c->out + c->out_off, len);
            c->out_off += len;
            br_ssl_engine_sendapp_ack(e, len);
            if (c->out_off == c->out_len) {
                out_done(c);
                br_ssl_engine_flush(e, 0);
                if (c->close_after) br_ssl_engine_close(e);   // its close_notify goes out with the answer
            }
            progress = 1;
        }
        // closing, and all of it left: a browser answers a close_notify by dropping the connection, not with its own
        if (c->close_after && !c->out && !(br_ssl_engine_current_state(e) & BR_SSL_SENDREC)) return conn_close(s, c), 0;
    }
    return 1;
}

static int pump(PhoneSrv *s, Conn *c) {
    if (c->kind == KIND_NEW) {   // a TLS handshake record starts with 0x16; anything else is plain HTTP
        unsigned char first;
        int n = (int)recv(c->fd, (char *)&first, 1, MSG_PEEK);
        if (n == 0 || (n < 0 && !sock_again())) return conn_close(s, c), 0;
        if (n < 0) return 1;
        c->kind = KIND_PLAIN;
        if (first == 0x16 && s->cfg.cert) {
            c->tls = malloc(sizeof *c->tls + BR_SSL_BUFSIZE_BIDI);
            if (!c->tls) return conn_close(s, c), 0;
            br_ssl_server_init_full_ec(c->tls, &s->chain, 1, BR_KEYTYPE_EC, &s->key);
            br_ssl_engine_set_buffer(&c->tls->eng, c->tls + 1, BR_SSL_BUFSIZE_BIDI, 1);
            br_ssl_server_reset(c->tls);
            c->kind = KIND_TLS;
        }
    }
    return c->kind == KIND_TLS ? pump_tls(s, c) : pump_plain(s, c);
}

static void accept_one(PhoneSrv *s) {
    struct sockaddr_in a;
    socklen_t al = sizeof a;
    Sock fd = accept(s->listener, (struct sockaddr *)&a, &al);
    if (fd == SOCK_BAD) return;
    Conn *c = NULL;
    for (int i = 0; i < MAX_CONNS && !c; i++)
        if (!s->conns[i].used) c = &s->conns[i];
    char peer[sizeof c->peer] = "?";
    inet_ntop(AF_INET, &a.sin_addr, peer, sizeof peer);
    if (!c || (!s->cfg.any_peer && !ppad_local_ipv4(ntohl(a.sin_addr.s_addr)))) {
        if (s->cfg.verbose) say(s, "phone: %s refused (%s)", peer, c ? "not an address of a local network" : "too many connections");
        sock_close(fd);
        return;
    }
    int on = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&on, sizeof on);   // a state is one small packet: no waiting to fill one
    sock_nonblock(fd);
    memset(c, 0, sizeof *c);
    c->used = 1, c->fd = fd, c->last_ms = now_ms();
    memcpy(c->peer, peer, sizeof peer);
    if (s->cfg.verbose) say(s, "phone: %s connected", peer);
}

// ---- the server

PhoneSrv *phonesrv_open(const PhoneSrvConfig *cfg, const PhoneSink *sink) {
    PhoneSrv *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->cfg = *cfg, s->sink = *sink, s->listener = SOCK_BAD;
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa)) return free(s), NULL;
#endif
    br_hmac_drbg_init(&s->rng, &br_sha256_vtable, NULL, 0);
    br_prng_seeder seed = br_prng_seeder_system(NULL);
    int ok = seed && seed(&s->rng.vtable);
    if (!ok) say(s, "phone: the system gives no random numbers");
    if (cfg->cert) {
        s->chain = (br_x509_certificate){(unsigned char *)cfg->cert->der, cfg->cert->der_len};
        s->key = (br_ec_private_key){BR_EC_secp256r1, (unsigned char *)cfg->cert->key, PPAD_KEY_LEN};
    }
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(cfg->port)};
    memcpy(&a.sin_addr, cfg->bind, 4);
    if (ok) s->listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s->listener != SOCK_BAD) {
#ifndef _WIN32
        int on = 1;   // the port again right after a restart; on Windows the option would let another program share it
        setsockopt(s->listener, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
#endif
        if (bind(s->listener, (struct sockaddr *)&a, sizeof a) || listen(s->listener, 16)) {
            sock_close(s->listener);
            s->listener = SOCK_BAD;
        }
    }
    if (s->listener == SOCK_BAD) {
        if (ok) say(s, "phone: port %d did not open (another program has it?)", cfg->port);
        phonesrv_close(s);
        return NULL;
    }
    sock_nonblock(s->listener);
    return s;
}

void phonesrv_poll(PhoneSrv *s, int timeout_ms) {
    struct pollfd fds[MAX_CONNS + 1] = {{.fd = s->listener, .events = POLLRDNORM}};
    Conn *who[MAX_CONNS + 1] = {NULL};
    int n = 1;
    for (int i = 0; i < MAX_CONNS; i++) {
        Conn *c = &s->conns[i];
        if (!c->used) continue;
        int out = c->kind == KIND_TLS ? (br_ssl_engine_current_state(&c->tls->eng) & BR_SSL_SENDREC) != 0 : c->out != NULL;
        fds[n] = (struct pollfd){.fd = c->fd, .events = (short)(POLLRDNORM | (out ? POLLWRNORM : 0))};
        who[n++] = c;
    }
    int got = sock_poll(fds, (unsigned)n, timeout_ms);
    if (got < 0 && !sock_again()) nap_ms(timeout_ms);   // a poll that fails at once must not spin its thread
    for (int i = 1; got > 0 && i < n; i++)
        if (fds[i].revents) pump(s, who[i]);
    if (got > 0 && fds[0].revents) accept_one(s);
    uint64_t now = now_ms();
    for (int i = 0; i < PPAD_MAX_PADS; i++) {
        Session *se = &s->sessions[i];
        if (!se->used) continue;
        if (now - se->last_ms > (uint64_t)(s->cfg.gone_ms ? s->cfg.gone_ms : GONE_MS)) session_end(s, se, "silent too long");
        else if (!se->neutral && now - se->last_ms > NEUTRAL_MS) {
            PpadState rest = {0};
            s->sink.state(s->sink.ctx, se->pad, &rest);
            se->neutral = 1;
        }
    }
    for (int i = 0; i < MAX_CONNS; i++)
        if (s->conns[i].used && now - s->conns[i].last_ms > IDLE_MS) conn_close(s, &s->conns[i]);
}

void phonesrv_close(PhoneSrv *s) {
    for (int i = 0; i < PPAD_MAX_PADS; i++)
        if (s->sessions[i].used) s->sink.detach(s->sink.ctx, s->sessions[i].pad);
    for (int i = 0; i < MAX_CONNS; i++)
        if (s->conns[i].used) conn_close(s, &s->conns[i]);
    if (s->listener != SOCK_BAD) sock_close(s->listener);
#ifdef _WIN32
    WSACleanup();
#endif
    free(s);
}
