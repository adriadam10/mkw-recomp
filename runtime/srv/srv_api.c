// The friends API (srv_api.h), as the protocol description's "Friends HTTPS API" says: TLS 1.2+, HTTP/1.1 with
// keep-alive, one request at a time, JSON replies whose arrays hold flat objects with the player's own "name" before
// them (our runtime's JSON reader is minimal), {"error": "..."} on failure. 60 requests per address and minute.
#include "srv_api.h"

#include <ctype.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mbedtls/net_sockets.h>

#include "srv.h"
#include "srv_auth.h"
#include "srv_presence.h"
#include "srv_store.h"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

enum { HEAD_MAX = 8192, BODY_MAX = 4096, LIST_MAX = 100, IDLE_S = 15, HANDSHAKE_S = 10, PER_MINUTE = 60 };

struct SrvApi {
    Sock s;
    char peer[64], ip[48];
    mbedtls_ssl_context ssl;
    int handshaken, closing;
    char in[HEAD_MAX + BODY_MAX + 1];
    size_t in_len;
    char *out;   // the reply being written
    size_t out_len, out_off;
    time_t born, last;
    const SrvTls *tls;
};

typedef struct {
    char *p;
    size_t n, cap;
} Text;

static void tput(Text *t, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int need = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (need < 0) return;
    if (t->n + (size_t)need + 1 > t->cap) {
        size_t cap = t->cap ? t->cap * 2 : 1024;
        while (cap < t->n + (size_t)need + 1) cap *= 2;
        char *p = realloc(t->p, cap);
        if (!p) return;
        t->p = p;
        t->cap = cap;
    }
    va_start(ap, fmt);
    vsnprintf(t->p + t->n, t->cap - t->n, fmt, ap);
    va_end(ap);
    t->n += (size_t)need;
}

// A JSON string's content: quote, backslash and control characters escaped, UTF-8 kept as it is (the runtime shows it).
static void json_str(Text *t, const char *s) {
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') tput(t, "\\%c", c);
        else if (c < 0x20) tput(t, "\\u%04x", c);
        else tput(t, "%c", c);
    }
}

static int bio_send(void *ctx, const unsigned char *buf, size_t len) {
    SrvApi *a = ctx;
    int n = (int)send(a->s, (const char *)buf, (int)(len > INT_MAX ? INT_MAX : len), MSG_NOSIGNAL);
    if (n >= 0) return n;
    return sock_would_block() ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_NET_SEND_FAILED;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len) {
    SrvApi *a = ctx;
    int n = (int)recv(a->s, (char *)buf, (int)(len > INT_MAX ? INT_MAX : len), 0);
    if (n >= 0) return n;
    return sock_would_block() ? MBEDTLS_ERR_SSL_WANT_READ : MBEDTLS_ERR_NET_RECV_FAILED;
}

// ---- requests per address and minute
static struct {
    char ip[48];
    time_t at[PER_MINUTE];
    int n;
} seen[32];

static int too_many(const char *ip, time_t now) {
    int slot = -1, oldest = 0;
    for (int i = 0; i < 32; i++) {
        if (!strcmp(seen[i].ip, ip)) {
            slot = i;
            break;
        }
        if (!seen[i].n || (seen[oldest].n && seen[i].at[seen[i].n - 1] < seen[oldest].at[seen[oldest].n - 1]))
            oldest = i;
    }
    if (slot < 0) {
        slot = oldest;
        memset(&seen[slot], 0, sizeof seen[slot]);
        snprintf(seen[slot].ip, sizeof seen[slot].ip, "%s", ip);
    }
    int k = 0;
    for (int i = 0; i < seen[slot].n; i++)
        if (now - seen[slot].at[i] < 60) seen[slot].at[k++] = seen[slot].at[i];
    seen[slot].n = k;
    if (k >= PER_MINUTE) return 1;
    seen[slot].at[seen[slot].n++] = now;
    return 0;
}

// ---- replies
static void reply(SrvApi *a, int status, const char *type, const void *body, size_t len) {
    static const struct {
        int code;
        const char *text;
    } REASONS[] = {{200, "OK"}, {400, "Bad Request"}, {403, "Forbidden"}, {404, "Not Found"}, {405, "Method Not Allowed"},
                   {409, "Conflict"}, {429, "Too Many Requests"}, {500, "Internal Server Error"}};
    const char *reason = "Error";
    for (size_t i = 0; i < sizeof REASONS / sizeof *REASONS; i++)
        if (REASONS[i].code == status) reason = REASONS[i].text;
    Text t = {0};
    tput(&t, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n%sConnection: %s\r\n\r\n", status, reason,
         type, len, strcmp(type, "application/json") ? "" : "Cache-Control: no-store\r\n",
         a->closing ? "close" : "keep-alive");
    if (!t.p) return;
    char *all = realloc(t.p, t.n + len);
    if (!all) {
        free(t.p);
        return;
    }
    if (len) memcpy(all + t.n, body, len);
    a->out = all;
    a->out_len = t.n + len;
    a->out_off = 0;
}

static void reply_json(SrvApi *a, int status, Text *t) {
    reply(a, status, "application/json", t->p ? t->p : "", t->n);
    free(t->p);
}

static void reply_error(SrvApi *a, int status, const char *why) {
    Text t = {0};
    tput(&t, "{\"error\":\"");
    json_str(&t, why);
    tput(&t, "\"}");
    reply_json(a, status, &t);
}

// {"code", "name", "friends": [{code, name, online}], "received": [{code, name, since}], "sent": [...]}
static void friends_json(SrvApi *a, const SrvAccount *me) {
    Text t = {0};
    uint64_t l[LIST_MAX];
    tput(&t, "{\"code\":\"%012llu\",\"name\":\"", (unsigned long long)me->code);
    json_str(&t, me->name);
    tput(&t, "\",\"friends\":[");
    int n = srv_friends(me->code, l, LIST_MAX);
    for (int i = 0; i < n; i++) {
        SrvAccount *f = srv_account(l[i]);
        tput(&t, "%s{\"code\":\"%012llu\",\"name\":\"", i ? "," : "", (unsigned long long)l[i]);
        json_str(&t, f ? f->name : "");
        tput(&t, "\",\"online\":%s}", srv_online(l[i]) ? "true" : "false");
    }
    for (int pass = 0; pass < 2; pass++) {   // received, then sent
        tput(&t, pass ? "],\"sent\":[" : "],\"received\":[");
        n = pass ? srv_sent(me->code, l, LIST_MAX) : srv_received(me->code, l, LIST_MAX);
        for (int i = 0; i < n; i++) {
            SrvAccount *f = srv_account(l[i]);
            double since = pass ? srv_added_since(me->code, l[i]) : srv_added_since(l[i], me->code);
            tput(&t, "%s{\"code\":\"%012llu\",\"name\":\"", i ? "," : "", (unsigned long long)l[i]);
            json_str(&t, f ? f->name : "");
            tput(&t, "\",\"since\":%.0f}", since);
        }
    }
    tput(&t, "]}");
    reply_json(a, 200, &t);
}

// ---- requests
typedef struct {
    char method[8], path[128], code[32], secret[160];
    const char *body;
    size_t body_len;
} Req;

// The header's value (case-insensitive name) copied into out, "" when absent.
static void header(const char *head, const char *name, char *out, size_t cap) {
    size_t n = strlen(name);
    out[0] = 0;
    for (const char *line = strstr(head, "\r\n"); line && line[2]; line = strstr(line + 2, "\r\n")) {
        const char *h = line + 2;
        size_t i = 0;
        while (i < n && h[i] && tolower((unsigned char)h[i]) == tolower((unsigned char)name[i])) i++;
        if (i < n || h[n] != ':') continue;
        const char *v = h + n + 1;
        while (*v == ' ' || *v == '\t') v++;
        size_t len = strcspn(v, "\r\n");
        while (len && (v[len - 1] == ' ' || v[len - 1] == '\t')) len--;
        snprintf(out, cap, "%.*s", (int)len, v);
        return;
    }
}

// The player making the request, from X-Droplet-Code and X-Droplet-Secret; NULL with a reply already made.
static SrvAccount *player(SrvApi *a, const Req *r) {
    char digits[16];
    size_t n = 0;
    for (const char *p = r->code; *p && n < 13; p++)
        if (isdigit((unsigned char)*p)) digits[n++] = *p;
    digits[n] = 0;
    SrvAccount *me = n == 12 && *r->secret ? srv_account(strtoull(digits, NULL, 10)) : NULL;
    char hash[65] = "";
    if (me) srv_secret_hash(r->secret, hash);
    if (!me || !srv_secret_equal(me->secret, hash)) {
        reply_error(a, 403, "play online once first: this code is not known with this secret");
        return NULL;
    }
    if (me->banned || !me->approved) {
        reply_error(a, 403, me->banned ? "banned" : "waiting for approval");
        return NULL;
    }
    return me;
}

// The friend code a request names: spaces and dashes out, a leading SW out; 12 digits, not zero. 0 when it is not one.
static uint64_t target_code(const char *body, size_t len) {
    char buf[BODY_MAX + 1], digits[16];
    memcpy(buf, body, len);
    buf[len] = 0;
    const char *k = strstr(buf, "\"code\"");
    if (!k) return 0;
    k = strchr(k + 6, ':');
    k = k ? strchr(k, '"') : NULL;
    if (!k) return 0;
    k++;
    size_t n = 0;
    if (toupper((unsigned char)k[0]) == 'S' && toupper((unsigned char)k[1]) == 'W') k += 2;
    for (; *k && *k != '"'; k++) {
        if (*k == ' ' || *k == '-') continue;
        if (!isdigit((unsigned char)*k) || n == 12) return 0;
        digits[n++] = *k;
    }
    digits[n] = 0;
    return n == 12 ? strtoull(digits, NULL, 10) : 0;
}

// The error of a friend action, or NULL once done.
static const char *apply_op(const char *op, uint64_t m, uint64_t t) {
    int max = srv_setting_int("max_friends", 200, 1, 1000);
    int theirs = srv_added(t, m), mine = srv_added(m, t);
    if (!strcmp(op, "request") || !strcmp(op, "accept")) {
        if (!strcmp(op, "request") && !srv_account(t)) return "no player has this code here";
        if (!strcmp(op, "accept") && !(theirs && !mine)) return "no request from this player";
        if (!mine && srv_adds_of(m) >= max) return "your friend list is full";
        srv_undecline(m, t);
        srv_undecline(t, m);
        srv_add(m, t);
    } else if (!strcmp(op, "decline")) {
        if (!(theirs && !mine)) return "no request from this player";
        srv_unadd(t, m);
        srv_unadd(m, t);
        srv_decline(m, t);
    } else if (!strcmp(op, "cancel")) {
        if (!(mine && !theirs)) return "no request to this player";
        srv_unadd(m, t);
    } else {   // remove
        if (!(mine && theirs)) return "this player is not a friend";
        srv_unadd(m, t);
        srv_unadd(t, m);
        srv_decline(m, t);
    }
    return NULL;
}

static void friends_op(SrvApi *a, const Req *r, SrvAccount *me, const char *op) {
    if (strcmp(op, "request") && strcmp(op, "accept") && strcmp(op, "decline") && strcmp(op, "cancel") &&
        strcmp(op, "remove")) {
        reply_error(a, 404, "unknown action");
        return;
    }
    size_t i = 0;
    while (i < r->body_len && (r->body[i] == ' ' || r->body[i] == '\r' || r->body[i] == '\n' || r->body[i] == '\t')) i++;
    if (i == r->body_len || r->body[i] != '{') {
        reply_error(a, 400, "the body is not a JSON object");
        return;
    }
    uint64_t t = target_code(r->body, r->body_len);
    if (!t || t == me->code) {
        reply_error(a, 409, "not a friend code");
        return;
    }
    const char *err = apply_op(op, me->code, t);
    if (err) reply_error(a, 409, err);
    else friends_json(a, me);
}

static void handle(SrvApi *a, const Req *r) {
    if (too_many(a->ip, time(NULL))) {
        reply_error(a, 429, "too many requests: wait a minute");
    } else if (!strcmp(r->path, "/v1/ca.der")) {
        size_t n;
        const unsigned char *ca = srv_tls_ca(a->tls, &n);
        if (strcmp(r->method, "GET")) reply_error(a, 405, "GET only");
        else reply(a, 200, "application/x-x509-ca-cert", ca, n);
    } else if (!strcmp(r->path, "/v1/friends")) {
        SrvAccount *me = NULL;
        if (strcmp(r->method, "GET")) reply_error(a, 405, "GET only");
        else if ((me = player(a, r))) friends_json(a, me);
    } else if (!strncmp(r->path, "/v1/friends/", 12)) {
        SrvAccount *me = NULL;
        if (strcmp(r->method, "POST")) reply_error(a, 405, "POST only");
        else if ((me = player(a, r))) friends_op(a, r, me, r->path + 12);
    } else {
        reply_error(a, 404, "not found");
    }
}

// One whole request in a->in: handled and taken out of the buffer (1). 0 while more is to come, -1 to close.
static int take_request(SrvApi *a) {
    a->in[a->in_len] = 0;
    char *end = strstr(a->in, "\r\n\r\n");
    if (!end) return a->in_len >= HEAD_MAX ? -1 : 0;
    size_t head = (size_t)(end - a->in) + 4;
    Req r;
    memset(&r, 0, sizeof r);
    char clen[16], conn[16];
    *end = 0;   // the head as a string while it is read
    int ok = sscanf(a->in, "%7s %127s", r.method, r.path) == 2;
    header(a->in, "content-length", clen, sizeof clen);
    header(a->in, "connection", conn, sizeof conn);
    header(a->in, "x-droplet-code", r.code, sizeof r.code);
    header(a->in, "x-droplet-secret", r.secret, sizeof r.secret);
    *end = '\r';
    if (!ok) return -1;
    long body = *clen ? strtol(clen, NULL, 10) : 0;
    if (body < 0 || body > BODY_MAX) {
        a->closing = 1;
        reply_error(a, 400, "body too large");
        a->in_len = 0;
        return 1;
    }
    if (a->in_len < head + (size_t)body) return 0;
    r.body = a->in + head;
    r.body_len = (size_t)body;
    for (char *c = conn; *c; c++) *c = (char)tolower((unsigned char)*c);
    a->closing = !strcmp(conn, "close");
    handle(a, &r);
    memmove(a->in, a->in + head + body, a->in_len - head - (size_t)body);
    a->in_len -= head + (size_t)body;
    return 1;
}

// ---- the connection
SrvApi *srv_api_new(Sock s, const SrvTls *tls, const char *peer, const char *ip) {
    SrvApi *a = calloc(1, sizeof *a);
    if (!a) return NULL;
    a->s = s;
    a->tls = tls;
    a->born = a->last = time(NULL);
    snprintf(a->peer, sizeof a->peer, "%s", peer);
    snprintf(a->ip, sizeof a->ip, "%s", ip);
    mbedtls_ssl_init(&a->ssl);
    if (mbedtls_ssl_setup(&a->ssl, srv_tls_conf(tls, 0))) {
        mbedtls_ssl_free(&a->ssl);
        free(a);
        return NULL;
    }
    mbedtls_ssl_set_bio(&a->ssl, a, bio_send, bio_recv, NULL);
    return a;
}

Sock srv_api_sock(const SrvApi *a) { return a->s; }

int srv_api_stale(const SrvApi *a, time_t now) {
    return now - (a->handshaken ? a->last : a->born) > (a->handshaken ? IDLE_S : HANDSHAKE_S);
}

void srv_api_free(SrvApi *a) {
    if (a->handshaken) mbedtls_ssl_close_notify(&a->ssl);
    mbedtls_ssl_free(&a->ssl);
    sock_close(a->s);
    free(a->out);
    free(a);
}

static int over(SrvApi *a) {
    srv_api_free(a);
    return -1;
}

int srv_api_step(SrvApi *a, int revents) {
    (void)revents;
    if (!a->handshaken) {
        int r = mbedtls_ssl_handshake(&a->ssl);
        if (r == MBEDTLS_ERR_SSL_WANT_READ) return POLLIN;
        if (r == MBEDTLS_ERR_SSL_WANT_WRITE) return POLLIN | POLLOUT;
        if (r) return over(a);   // a first contact that does not trust the CA yet is normal: no log
        a->handshaken = 1;
        a->last = time(NULL);
    }
    for (;;) {
        while (a->out) {   // the reply first: one request at a time
            int r = mbedtls_ssl_write(&a->ssl, (const unsigned char *)a->out + a->out_off, a->out_len - a->out_off);
            if (r == MBEDTLS_ERR_SSL_WANT_WRITE || r == MBEDTLS_ERR_SSL_WANT_READ) return POLLIN | POLLOUT;
            if (r <= 0) return over(a);
            a->out_off += (size_t)r;
            if (a->out_off == a->out_len) {
                free(a->out);
                a->out = NULL;
                if (a->closing) return over(a);
            }
        }
        int t = take_request(a);
        if (t < 0) return over(a);
        if (t > 0) {
            a->last = time(NULL);
            continue;
        }
        if (a->in_len >= sizeof a->in - 1) return over(a);
        int r = mbedtls_ssl_read(&a->ssl, (unsigned char *)a->in + a->in_len, sizeof a->in - 1 - a->in_len);
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) return POLLIN;
        if (r <= 0) return over(a);
        a->in_len += (size_t)r;
    }
}
