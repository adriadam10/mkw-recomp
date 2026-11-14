// One player's connection (srv_h2.h): TLS with Mbed TLS over a non-blocking socket, HTTP/2 with nghttp2 fed from
// memory (no socket of its own), and gRPC calls on its streams, handed to the services of srv_methods.c (srv_svc.h).
// Methods without a service of their own get the catch-all the game expects (the protocol description, "Dispatch,
// call kinds and the catch-all"): a unary call one empty reply, a server stream one empty message then silence until
// the player cancels it, a bidi stream nothing until the player half-closes it.
#include "srv_h2.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <mbedtls/net_sockets.h>
#include <nghttp2/nghttp2.h>

#include "srv.h"
#include "srv_auth.h"
#include "srv_svc.h"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0   // Windows has no SIGPIPE
#endif

enum { MSG_MAX = 1 << 20, BATCH = 32768 };   // inbound message cap (larger: RESOURCE_EXHAUSTED); TLS write batch
// Memory for the network: 4 MiB per connection, 128 MiB for the whole server; a buffer over KEEP_MAX is given back once
// empty. A player who sends faster than the server answers, opens many streams or never reads the replies meets these
// budgets, not the PC's memory (16 connections of 32 streams with 1 MiB messages would otherwise hold 512 MiB).
enum { CONN_MEM = 4 << 20, ALL_MEM = 128 << 20, KEEP_MAX = 64 << 10, STREAMS_MAX = 32, HANDSHAKE_S = 10 };

struct Call {
    struct SrvConn *conn;
    struct Call *next;
    int32_t id;
    const SrvMethod *m;   // NULL: a unary catch-all
    void *state;
    char path[192], auth[1024], tenant[96], uid[24];
    uint8_t *in;   // request bytes not yet cut into messages
    size_t in_len, in_cap;
    uint8_t *out;  // response DATA (gRPC-framed messages) nghttp2 has not taken yet
    size_t out_len, out_off, out_cap;
    int started, ending, deferred, done, half_closed, status, streaming, quiet;
    uint64_t last_sent;
    char message[128];
};
typedef struct Call Call;

struct SrvConn {
    Sock s;
    char peer[64], ip[48];
    mbedtls_ssl_context ssl;
    int handshaken;
    nghttp2_session *h2;
    Call *calls;
    uint8_t *out;   // TLS plaintext waiting for mbedtls_ssl_write, which must be offered it again as it was
    size_t out_len, out_off, out_cap;
    size_t mem;     // what this connection's buffers hold (grow, release)
    time_t born;
    int kicked;     // a reply was queued since the last flush (from another player's call, say): flush it soon
};

static size_t mem_all;   // all connections' buffers (one thread: a plain counter)

static int kind_of(const Call *c) { return c->m ? c->m->kind : SRV_UNARY; }

// A buffer grown to hold need bytes, within the connection's and the server's budgets; 0 when that would pass them.
static int grow(SrvConn *conn, uint8_t **buf, size_t *cap, size_t need) {
    if (need <= *cap) return 1;
    size_t n = *cap ? *cap * 2 : 256;
    if (n < need) n = need;
    if (conn->mem + (n - *cap) > CONN_MEM || mem_all + (n - *cap) > ALL_MEM) n = need;   // no doubling past a budget
    if (conn->mem + (n - *cap) > CONN_MEM || mem_all + (n - *cap) > ALL_MEM) return 0;
    uint8_t *b = realloc(*buf, n);
    if (!b) return 0;
    conn->mem += n - *cap;
    mem_all += n - *cap;
    *buf = b;
    *cap = n;
    return 1;
}

static void release(SrvConn *conn, uint8_t **buf, size_t *cap) {
    free(*buf);
    conn->mem -= *cap;
    mem_all -= *cap;
    *buf = NULL;
    *cap = 0;
}

// ---- TLS over the socket
static int bio_send(void *ctx, const unsigned char *buf, size_t len) {
    SrvConn *c = ctx;
    int n = (int)send(c->s, (const char *)buf, (int)(len > INT_MAX ? INT_MAX : len), MSG_NOSIGNAL);
    if (n >= 0) return n;
    return sock_would_block() ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_NET_SEND_FAILED;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len) {
    SrvConn *c = ctx;
    int n = (int)recv(c->s, (char *)buf, (int)(len > INT_MAX ? INT_MAX : len), 0);
    if (n >= 0) return n;   // 0: the player closed the connection
    return sock_would_block() ? MBEDTLS_ERR_SSL_WANT_READ : MBEDTLS_ERR_NET_RECV_FAILED;
}

// ---- gRPC replies
static void send_trailers(Call *c) {
    char st[12];
    snprintf(st, sizeof st, "%d", c->status);
    nghttp2_nv nv[2] = {
        {(uint8_t *)"grpc-status", (uint8_t *)st, 11, strlen(st), NGHTTP2_NV_FLAG_NONE},
        {(uint8_t *)"grpc-message", (uint8_t *)c->message, 12, strlen(c->message), NGHTTP2_NV_FLAG_NONE},
    };
    nghttp2_submit_trailer(c->conn->h2, c->id, nv, c->message[0] ? 2 : 1);
}

static nghttp2_ssize on_read(nghttp2_session *s, int32_t id, uint8_t *buf, size_t len, uint32_t *flags,
                             nghttp2_data_source *src, void *ud) {
    (void)s, (void)id, (void)ud;
    Call *c = src->ptr;
    size_t avail = c->out_len - c->out_off;
    if (avail) {
        size_t n = avail < len ? avail : len;
        memcpy(buf, c->out + c->out_off, n);
        c->out_off += n;
        if (c->out_off == c->out_len) {
            c->out_off = c->out_len = 0;
            if (c->out_cap > KEEP_MAX) release(c->conn, &c->out, &c->out_cap);
        }
        return (nghttp2_ssize)n;
    }
    if (c->ending) {   // the messages are out: the status goes in trailers, which end the stream
        *flags |= NGHTTP2_DATA_FLAG_EOF | NGHTTP2_DATA_FLAG_NO_END_STREAM;
        send_trailers(c);
        c->done = 1;
        return 0;
    }
    c->deferred = 1;   // nothing to send yet: the stream stays open until a reply resumes it
    return NGHTTP2_ERR_DEFERRED;
}

static void start(Call *c) {
    if (c->started) return;
    c->started = 1;
    nghttp2_nv nv[2] = {
        {(uint8_t *)":status", (uint8_t *)"200", 7, 3, NGHTTP2_NV_FLAG_NONE},
        {(uint8_t *)"content-type", (uint8_t *)"application/grpc", 12, 16, NGHTTP2_NV_FLAG_NONE},
    };
    nghttp2_data_provider2 prd = {.source.ptr = c, .read_callback = on_read};
    nghttp2_submit_response2(c->conn->h2, c->id, nv, 2, &prd);
}

static void wake(Call *c) {
    if (c->deferred) {
        c->deferred = 0;
        nghttp2_session_resume_data(c->conn->h2, c->id);
    }
}

// Past the budgets (a player who does not read its replies) the call ends.
void srv_call_reply(Call *c, const void *msg, size_t len) {
    if (c->ending || c->done) return;
    if (!grow(c->conn, &c->out, &c->out_cap, c->out_len + 5 + len)) {
        srv_call_end(c, ST_RESOURCE_EXHAUSTED, "server busy");
        return;
    }
    uint8_t *p = c->out + c->out_len;
    p[0] = 0;   // not compressed
    p[1] = (uint8_t)(len >> 24), p[2] = (uint8_t)(len >> 16), p[3] = (uint8_t)(len >> 8), p[4] = (uint8_t)len;
    if (len) memcpy(p + 5, msg, len);
    c->out_len += 5 + len;
    c->last_sent = srv_ms();
    c->conn->kicked = 1;
    start(c);
    wake(c);
}

// No message sent before the end: a trailers-only response (one HEADERS frame).
void srv_call_end(Call *c, int status, const char *message) {
    if (c->ending || c->done) return;
    c->ending = 1;
    c->status = status;
    c->conn->kicked = 1;
    snprintf(c->message, sizeof c->message, "%s", message ? message : "");
    if (status != ST_OK || !c->quiet)
        srv_log("%s %s -> %d%s%s", c->conn->peer, c->path, status, c->message[0] ? " " : "", c->message);
    if (c->started) {
        wake(c);
        return;
    }
    c->started = c->done = 1;
    char st[12];
    snprintf(st, sizeof st, "%d", status);
    nghttp2_nv nv[4] = {
        {(uint8_t *)":status", (uint8_t *)"200", 7, 3, NGHTTP2_NV_FLAG_NONE},
        {(uint8_t *)"content-type", (uint8_t *)"application/grpc", 12, 16, NGHTTP2_NV_FLAG_NONE},
        {(uint8_t *)"grpc-status", (uint8_t *)st, 11, strlen(st), NGHTTP2_NV_FLAG_NONE},
        {(uint8_t *)"grpc-message", (uint8_t *)c->message, 12, strlen(c->message), NGHTTP2_NV_FLAG_NONE},
    };
    nghttp2_submit_response2(c->conn->h2, c->id, nv, c->message[0] ? 4 : 3, NULL);
}

int srv_call_ended(const Call *c) { return c->ending || c->done; }
void srv_call_quiet(Call *c) { c->quiet = 1; }
const char *srv_call_uid(const Call *c) { return c->uid; }
const char *srv_call_tenant(const Call *c) { return c->tenant; }
const char *srv_call_ip(const Call *c) { return c->conn->ip; }
const char *srv_call_auth(const Call *c) { return c->auth; }
const char *srv_call_path(const Call *c) { return c->path; }
void **srv_call_state(Call *c) { return &c->state; }
uint64_t srv_call_last_sent(const Call *c) { return c->last_sent; }

// ---- requests
// The player has sent all of the request (END_STREAM): the service's handler, else the catch-all of the call's kind.
static void on_half_close(Call *c) {
    if (c->ending || c->done) return;
    if (c->m && c->m->on_half_close) {
        c->m->on_half_close(c);
        return;
    }
    switch (kind_of(c)) {
    case SRV_UNARY:
        srv_call_reply(c, NULL, 0);
        srv_call_end(c, ST_OK, NULL);
        break;
    case SRV_SSTREAM:
        if (!c->last_sent) srv_call_reply(c, NULL, 0);
        if (c->m && c->m->end_after_first) srv_call_end(c, ST_OK, NULL);
        else if (!c->m || !c->m->on_tick) srv_log("%s %s held open", c->conn->peer, c->path);
        break;
    default:
        srv_call_end(c, ST_OK, NULL);
    }
}

// Request bytes cut into gRPC messages (1 byte flag, 4 bytes big-endian length, the protobuf).
static void take(Call *c, const uint8_t *data, size_t len) {
    if (c->ending || c->done) return;
    if (c->in_len + len > MSG_MAX + 5) {
        srv_call_end(c, ST_RESOURCE_EXHAUSTED, "message too large");
        return;
    }
    if (!grow(c->conn, &c->in, &c->in_cap, c->in_len + len)) {
        srv_call_end(c, ST_RESOURCE_EXHAUSTED, "server busy");
        return;
    }
    memcpy(c->in + c->in_len, data, len);
    c->in_len += len;
    size_t off = 0;
    while (c->in_len - off >= 5 && !c->ending) {
        const uint8_t *p = c->in + off;
        size_t n = (size_t)p[1] << 24 | (size_t)p[2] << 16 | (size_t)p[3] << 8 | p[4];
        if (p[0]) {
            srv_call_end(c, ST_UNIMPLEMENTED, "compressed messages are not supported");
            return;
        }
        if (n > MSG_MAX) {
            srv_call_end(c, ST_RESOURCE_EXHAUSTED, "message too large");
            return;
        }
        if (c->in_len - off - 5 < n) break;
        if (c->m && c->m->on_message) c->m->on_message(c, p + 5, n);
        off += 5 + n;
    }
    if (c->ending) return;
    memmove(c->in, c->in + off, c->in_len - off);
    c->in_len -= off;
    if (!c->in_len && c->in_cap > KEEP_MAX) release(c->conn, &c->in, &c->in_cap);
}

static void call_free(Call *c) {
    if (c->m && c->m->on_close) c->m->on_close(c);
    if (c->streaming) srv_on_stream(c, 0);
    release(c->conn, &c->in, &c->in_cap);
    release(c->conn, &c->out, &c->out_cap);
    free(c);
}

static int on_begin_headers(nghttp2_session *s, const nghttp2_frame *f, void *ud) {
    SrvConn *conn = ud;
    if (f->hd.type != NGHTTP2_HEADERS || f->headers.cat != NGHTTP2_HCAT_REQUEST) return 0;
    Call *c = calloc(1, sizeof *c);
    if (!c) return NGHTTP2_ERR_CALLBACK_FAILURE;
    c->conn = conn;
    c->id = f->hd.stream_id;
    c->next = conn->calls;
    conn->calls = c;
    nghttp2_session_set_stream_user_data(s, c->id, c);
    return 0;
}

static void copy_value(char *dst, size_t cap, const uint8_t *v, size_t len) {
    size_t n = len < cap - 1 ? len : cap - 1;
    memcpy(dst, v, n);
    dst[n] = 0;
}

static int on_header(nghttp2_session *s, const nghttp2_frame *f, const uint8_t *name, size_t namelen,
                     const uint8_t *value, size_t valuelen, uint8_t flags, void *ud) {
    (void)flags, (void)ud;
    Call *c = nghttp2_session_get_stream_user_data(s, f->hd.stream_id);
    if (!c || f->hd.type != NGHTTP2_HEADERS || f->headers.cat != NGHTTP2_HCAT_REQUEST) return 0;
    if (namelen == 5 && !memcmp(name, ":path", 5)) copy_value(c->path, sizeof c->path, value, valuelen);
    else if (namelen == 13 && !memcmp(name, "authorization", 13)) copy_value(c->auth, sizeof c->auth, value, valuelen);
    else if (namelen == 14 && !memcmp(name, "npln-tenant-id", 14))
        copy_value(c->tenant, sizeof c->tenant, value, valuelen);
    return 0;
}

// The request's headers are in: which method, which player, and whether the player may call at all.
static void on_request(Call *c) {
    char service[96] = "", *method = strrchr(c->path, '/');
    if (method && method > c->path) {
        size_t n = (size_t)(method - c->path - 1);
        if (n >= sizeof service) n = sizeof service - 1;
        memcpy(service, c->path + 1, n);
        service[n] = 0;
        char *dot = strrchr(service, '.');
        if (dot) memmove(service, dot + 1, strlen(dot));
    }
    c->m = method ? srv_method(service, method + 1) : NULL;
    srv_auth_uid(c->auth, c->uid, sizeof c->uid);
    int st = srv_on_call(c);
    if (st != ST_OK) {
        srv_call_end(c, st, "banned");
        return;
    }
    if (kind_of(c) != SRV_UNARY) {
        c->streaming = 1;
        srv_on_stream(c, 1);
    }
}

static int on_frame_recv(nghttp2_session *s, const nghttp2_frame *f, void *ud) {
    (void)ud;
    Call *c = nghttp2_session_get_stream_user_data(s, f->hd.stream_id);
    if (!c) return 0;
    if (f->hd.type == NGHTTP2_HEADERS && f->headers.cat == NGHTTP2_HCAT_REQUEST) on_request(c);
    if ((f->hd.type == NGHTTP2_HEADERS || f->hd.type == NGHTTP2_DATA) && (f->hd.flags & NGHTTP2_FLAG_END_STREAM) &&
        !c->half_closed) {
        c->half_closed = 1;
        on_half_close(c);
    }
    return 0;
}

static int on_data(nghttp2_session *s, uint8_t flags, int32_t id, const uint8_t *data, size_t len, void *ud) {
    (void)flags, (void)ud;
    Call *c = nghttp2_session_get_stream_user_data(s, id);
    if (c) take(c, data, len);
    return 0;
}

static int on_stream_close(nghttp2_session *s, int32_t id, uint32_t error, void *ud) {
    SrvConn *conn = ud;
    Call *c = nghttp2_session_get_stream_user_data(s, id);
    if (!c) return 0;
    if (!c->done && c->path[0]) srv_log("%s %s closed by the player (%u)", conn->peer, c->path, error);
    for (Call **p = &conn->calls; *p; p = &(*p)->next)
        if (*p == c) {
            *p = c->next;
            break;
        }
    call_free(c);
    return 0;
}

// ---- the connection
SrvConn *srv_conn_new(Sock s, const SrvTls *tls, const char *peer, const char *ip) {
    SrvConn *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->s = s;
    c->born = time(NULL);
    snprintf(c->peer, sizeof c->peer, "%s", peer);
    snprintf(c->ip, sizeof c->ip, "%s", ip);
    mbedtls_ssl_init(&c->ssl);
    if (mbedtls_ssl_setup(&c->ssl, srv_tls_conf(tls, 1))) {
        mbedtls_ssl_free(&c->ssl);
        free(c);
        return NULL;
    }
    mbedtls_ssl_set_bio(&c->ssl, c, bio_send, bio_recv, NULL);
    return c;
}

Sock srv_conn_sock(const SrvConn *c) { return c->s; }

int srv_conn_stale(const SrvConn *c, time_t now) { return !c->handshaken && now - c->born > HANDSHAKE_S; }

size_t srv_mem_in_use(void) { return mem_all; }

int srv_conn_kicked(const SrvConn *c) { return c->kicked; }

void srv_conn_free(SrvConn *c) {
    // A call's close handler may reply to others (a player leaving a room tells the rest): none of those replies may
    // go into this connection while it is torn down.
    for (Call *k = c->calls; k; k = k->next) k->done = 1;
    if (c->handshaken) mbedtls_ssl_close_notify(&c->ssl);   // best effort: the socket does not wait
    if (c->h2) nghttp2_session_del(c->h2);                  // calls no stream-close callback: the calls go below
    while (c->calls) {
        Call *n = c->calls->next;
        call_free(c->calls);
        c->calls = n;
    }
    mbedtls_ssl_free(&c->ssl);
    sock_close(c->s);
    release(c, &c->out, &c->out_cap);
    free(c);
}

static int start_h2(SrvConn *c) {
    const char *alpn = mbedtls_ssl_get_alpn_protocol(&c->ssl);
    if (!alpn || strcmp(alpn, "h2")) {
        srv_log("%s: TLS without HTTP/2 (ALPN %s): closed", c->peer, alpn ? alpn : "none");
        return -1;
    }
    nghttp2_session_callbacks *cb;
    if (nghttp2_session_callbacks_new(&cb)) return -1;
    nghttp2_session_callbacks_set_on_begin_headers_callback(cb, on_begin_headers);
    nghttp2_session_callbacks_set_on_header_callback(cb, on_header);
    nghttp2_session_callbacks_set_on_frame_recv_callback(cb, on_frame_recv);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(cb, on_data);
    nghttp2_session_callbacks_set_on_stream_close_callback(cb, on_stream_close);
    int r = nghttp2_session_server_new(&c->h2, cb, c);
    nghttp2_session_callbacks_del(cb);
    if (r) return -1;
    // a player keeps 4 to 6 streams open; 32 leaves room without letting one connection open thousands
    nghttp2_settings_entry iv[] = {{NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, STREAMS_MAX}};
    return nghttp2_submit_settings(c->h2, NGHTTP2_FLAG_NONE, iv, 1) ? -1 : 0;
}

// Hands nghttp2's output to TLS, several frames per record. 1 while some still waits for the socket, 0 when all went,
// -1 on failure.
static int flush(SrvConn *c) {
    for (;;) {
        if (c->out_off == c->out_len) {
            c->out_off = c->out_len = 0;
            while (c->out_len < BATCH) {
                const uint8_t *d;
                nghttp2_ssize n = nghttp2_session_mem_send2(c->h2, &d);   // valid until the next call
                if (n < 0) return -1;
                if (n == 0) break;
                if (!grow(c, &c->out, &c->out_cap, c->out_len + (size_t)n)) return -1;   // over budget: closed
                memcpy(c->out + c->out_len, d, (size_t)n);
                c->out_len += (size_t)n;
            }
            if (!c->out_len) {
                if (c->out_cap > KEEP_MAX) release(c, &c->out, &c->out_cap);
                return 0;
            }
        }
        int r = mbedtls_ssl_write(&c->ssl, c->out + c->out_off, c->out_len - c->out_off);
        if (r > 0) {
            c->out_off += (size_t)r;
            continue;
        }
        if (r == MBEDTLS_ERR_SSL_WANT_WRITE || r == MBEDTLS_ERR_SSL_WANT_READ) return 1;
        return -1;
    }
}

int srv_conn_step(SrvConn *c, int revents) {
    (void)revents;
    if (!c->handshaken) {
        int r = mbedtls_ssl_handshake(&c->ssl);
        if (r == MBEDTLS_ERR_SSL_WANT_READ) return POLLIN;
        if (r == MBEDTLS_ERR_SSL_WANT_WRITE) return POLLIN | POLLOUT;
        if (r) {
            srv_log("%s: TLS handshake failed (-0x%04x)", c->peer, (unsigned)-r);
            srv_conn_free(c);
            return -1;
        }
        c->handshaken = 1;
        if (start_h2(c)) {
            srv_conn_free(c);
            return -1;
        }
        srv_log("%s: connected (%s)", c->peer, mbedtls_ssl_get_version(&c->ssl));
    }
    uint8_t buf[16384];
    for (;;) {
        int r = mbedtls_ssl_read(&c->ssl, buf, sizeof buf);
        if (r > 0) {
            if (nghttp2_session_mem_recv2(c->h2, buf, (size_t)r) < 0) {
                srv_log("%s: HTTP/2 error: closed", c->peer);
                srv_conn_free(c);
                return -1;
            }
            continue;
        }
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) break;
        srv_log("%s: disconnected", c->peer);   // 0, close_notify or a reset
        srv_conn_free(c);
        return -1;
    }
    // the services' timers: idle acknowledgements, lists sent again when they change
    uint64_t now = srv_ms();
    for (Call *k = c->calls; k; k = k->next)
        if (k->m && k->m->on_tick && !k->ending && !k->done && k->path[0]) k->m->on_tick(k, now);
    c->kicked = 0;   // before the flush: a stream it closes may queue replies (srv_conn_kicked)
    int f = flush(c);
    if (f < 0 || (f == 0 && !nghttp2_session_want_read(c->h2) && !nghttp2_session_want_write(c->h2))) {
        srv_log("%s: %s", c->peer, f < 0 ? "send failed: closed" : "session over");
        srv_conn_free(c);
        return -1;
    }
    return POLLIN | (f ? POLLOUT : 0);
}
