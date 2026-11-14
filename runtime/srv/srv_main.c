// The online server's process (srv.h): one thread, one poll loop over every socket, so no lock anywhere. UDP: the STUN
// answers Pia asks for on 3478 and 3479, its NAT check on 10025, 10125 and 33334. TCP: the game's gRPC port (8443 unless
// --port), TLS + HTTP/2 (srv_h2.c).
#include "srv_net.h"
#ifdef __linux__
#include <signal.h>
#include <sys/prctl.h>
#endif
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "platform/platform.h"
#include "srv.h"
#include "srv_api.h"
#include "srv_auth.h"
#include "srv_h2.h"
#include "srv_session.h"
#include "srv_store.h"
#include "srv_tls.h"
#include "srv_udp.h"

enum { MAX_HOSTS = 8, MAX_LISTEN = 64, MAX_CONNS = 256, MAX_PER_IP = 16, LOG_MAX = 2 << 20 };
enum { K_STUN, K_NAT, K_GRPC, K_API };
static const struct {
    int port, kind;
} UDP_PORTS[] = {{3478, K_STUN}, {3479, K_STUN}, {10025, K_NAT}, {10125, K_NAT}, {33334, K_NAT}};

typedef struct {
    Sock s;
    int kind, udp;
} Listener;

typedef struct {
    SrvConn *c;   // the game's gRPC port
    SrvApi *a;    // or the friends API next to it
    int events;
    char ip[48];
} Conn;

static Listener ls[MAX_LISTEN];
static int nls;
// The address the NAT check tells a game on 127.x: the first --host that is IPv4, not loopback and not 0.0.0.0.
static uint8_t self_ip[4];
static Conn conns[MAX_CONNS];
static int nconns;
static FILE *log_file;

void srv_log(const char *fmt, ...) {
    char line[1024];
    time_t t = time(NULL);
    struct tm tm;
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    size_t n = strftime(line, sizeof line, "%Y-%m-%d %H:%M:%S ", &tm);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line + n, sizeof line - n, fmt, ap);
    va_end(ap);
    fprintf(stderr, "%s\n", line);
    if (log_file) {
        fprintf(log_file, "%s\n", line);
        fflush(log_file);
    }
}

// <data>/logs/server.log, the previous one kept as server.log.1 once it passes 2 MiB.
static void open_log(const char *data) {
    char dir[1024], path[1100], old[1110];
    snprintf(dir, sizeof dir, "%s/logs", data);
    plat_file_mkdir_p(dir);
    snprintf(path, sizeof path, "%s/server.log", dir);
    snprintf(old, sizeof old, "%s.1", path);
    uint64_t size = 0;
    uint8_t *prev = plat_file_read(path, &size);
    free(prev);
    if (size > LOG_MAX) plat_file_replace(path, old);
    log_file = fopen(path, "a");
}

static const char *data_dir = "online-server";
const char *srv_data_dir(void) { return data_dir; }

int srv_random(void *buf, size_t n) { return plat_random(buf, n); }

uint64_t srv_ms(void) {
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
#endif
}

int srv_ip_bytes(const char *ip, unsigned char out[16]) {
    if (inet_pton(AF_INET, ip, out) == 1) return 4;
    if (inet_pton(AF_INET6, ip, out) == 1) return 16;
    return 0;
}

static int numeric_host(const char *host, int port, int udp, struct addrinfo **ai) {
    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = udp ? SOCK_DGRAM : SOCK_STREAM;
    hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;
    char ps[8];
    snprintf(ps, sizeof ps, "%d", port);
    return getaddrinfo(host, ps, &hints, ai) == 0 && *ai;
}

// A port taken by another program (another server still running, say) is reported and skipped: the other ports
// still work.
static int listen_on(const char *host, int port, int udp, int kind) {
    struct addrinfo *ai = NULL;
    if (nls >= MAX_LISTEN || !numeric_host(host, port, udp, &ai)) return 0;
    Sock s = socket(ai->ai_family, udp ? SOCK_DGRAM : SOCK_STREAM, udp ? IPPROTO_UDP : IPPROTO_TCP);
    int on = 1;
#ifdef _WIN32
    // Windows' SO_REUSEADDR lets another program take the port too; this keeps it ours alone
    if (s != SOCK_BAD) setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&on, sizeof on);
#else
    if (s != SOCK_BAD && !udp) setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);   // restart past TIME_WAIT
#endif
    int ok = s != SOCK_BAD && bind(s, ai->ai_addr, (int)ai->ai_addrlen) == 0 && (udp || listen(s, 64) == 0) &&
             sock_nonblocking(s);
    freeaddrinfo(ai);
    if (!ok) {
        if (s != SOCK_BAD) sock_close(s);
        srv_log("%s %s port %d is taken or not on this PC", udp ? "UDP" : "TCP", host, port);
        return 0;
    }
#ifdef _WIN32
    if (udp) {   // a peer's ICMP "port unreachable" would otherwise fail the next receive on this socket
        BOOL off = FALSE;
        DWORD got = 0;
        WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof off, NULL, 0, &got, NULL, NULL);
    }
#endif
    ls[nls++] = (Listener){s, kind, udp};
    return 1;
}

static SrvPeer peer_of(const struct sockaddr_storage *ss) {
    SrvPeer p;
    memset(&p, 0, sizeof p);
    if (ss->ss_family == AF_INET6) {
        const struct sockaddr_in6 *a = (const struct sockaddr_in6 *)ss;
        p.v6 = 1;
        memcpy(p.addr, &a->sin6_addr, 16);
        p.port = ntohs(a->sin6_port);
    } else {
        const struct sockaddr_in *a = (const struct sockaddr_in *)ss;
        memcpy(p.addr, &a->sin_addr, 4);
        p.port = ntohs(a->sin_port);
    }
    return p;
}

static void on_udp(const Listener *l) {
    uint8_t in[1500], out[256];
    struct sockaddr_storage from;
    socklen_t fl = sizeof from;
    int n = (int)recvfrom(l->s, (char *)in, sizeof in, 0, (struct sockaddr *)&from, &fl);
    if (n <= 0) return;
    SrvPeer p = peer_of(&from);
    size_t r = 0;
    if (l->kind == K_STUN) r = stun_reply(in, (size_t)n, &p, out, sizeof out);
    else if (l->kind == K_NAT) r = nat_reply(in, (size_t)n, &p, self_ip, out, sizeof out);
    if (r) sendto(l->s, (const char *)out, (int)r, 0, (struct sockaddr *)&from, fl);
}

static void on_accept(const Listener *l, const SrvTls *tls) {
    for (int k = 0; k < 16; k++) {   // a few per pass: the other sockets get their turn
        struct sockaddr_storage from;
        socklen_t fl = sizeof from;
        Sock s = accept(l->s, (struct sockaddr *)&from, &fl);
        if (s == SOCK_BAD) return;
        SrvPeer p = peer_of(&from);
        char ip[48], peer[64];
        inet_ntop(p.v6 ? AF_INET6 : AF_INET, p.addr, ip, sizeof ip);
        snprintf(peer, sizeof peer, "%s:%u", ip, p.port);
        int same = 0;
        for (int i = 0; i < nconns; i++) same += (conns[i].c || conns[i].a) && !strcmp(conns[i].ip, ip);
        if (nconns >= MAX_CONNS || same >= MAX_PER_IP) {
            srv_log("%s: refused (%s)", peer, nconns >= MAX_CONNS ? "server full" : "too many connections");
            sock_close(s);
            continue;
        }
        int on = 1;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&on, sizeof on);
        SrvConn *c = NULL;
        SrvApi *a = NULL;
        if (sock_nonblocking(s)) {
            if (l->kind == K_API) a = srv_api_new(s, tls, peer, ip);
            else c = srv_conn_new(s, tls, peer, ip);
        }
        if (!c && !a) {
            sock_close(s);
            continue;
        }
        conns[nconns] = (Conn){c, a, POLLIN, ""};
        snprintf(conns[nconns].ip, sizeof conns[nconns].ip, "%s", ip);
        nconns++;
    }
}

int srv_main(int argc, char **argv) {
#ifdef _WIN32
    WSADATA w;   // first: Windows' getaddrinfo, which checks --host, fails before it
    if (WSAStartup(MAKEWORD(2, 2), &w)) return 1;
#endif
    const char *hosts[MAX_HOSTS];
    int nhosts = 0, seconds = 0, port = 8443;
    for (int i = 0; i < argc; i++) {
        struct addrinfo *ai = NULL;
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(argv[i], "--host") && v && nhosts < MAX_HOSTS && numeric_host(v, 0, 1, &ai)) {
            freeaddrinfo(ai);
            hosts[nhosts++] = argv[++i];
        } else if (!strcmp(argv[i], "--seconds") && v && atoi(v) > 0) {
            seconds = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--port") && v && atoi(v) > 0 && atoi(v) < 65534) {
            port = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--data") && v && *v) {
            data_dir = argv[++i];
        } else {
            fprintf(stderr, "usage: mkw --server [--host ADDR]... [--port P] [--data DIR] [--seconds S]  (at most %d "
                            "numeric addresses)\n", MAX_HOSTS);
            return 2;
        }
    }
    if (!nhosts) hosts[nhosts++] = "127.0.0.1";
    for (int h = 0; h < nhosts && !self_ip[0]; h++) {
        unsigned char b[16];
        if (srv_ip_bytes(hosts[h], b) == 4 && b[0] != 127 && (b[0] | b[1] | b[2] | b[3])) memcpy(self_ip, b, 4);
    }
#ifdef __linux__
    prctl(PR_SET_PDEATHSIG, SIGTERM);   // ends with the game; on Windows the job net_host.c puts it in does it
#endif
    open_log(data_dir);
    char err[256];
    SrvTls *tls = srv_tls_open(data_dir, hosts, nhosts, err, sizeof err);
    if (!tls) {
        srv_log("cannot start: %s", err);
        return 1;
    }
    if (!srv_auth_start()) {
        srv_log("cannot start: the token key could not be made or read");
        return 1;
    }
    srv_store_load();
    for (int h = 0; h < nhosts; h++) {
        listen_on(hosts[h], port, 0, K_GRPC);
        listen_on(hosts[h], port + 1, 0, K_API);
        for (size_t k = 0; k < sizeof UDP_PORTS / sizeof *UDP_PORTS; k++)
            listen_on(hosts[h], UDP_PORTS[k].port, 1, UDP_PORTS[k].kind);
    }
    if (!nls) return 1;
    srv_log("started: port %d (friends API %d) on %d address(es)", port, port + 1, nhosts);
    time_t end = seconds ? time(NULL) + seconds : 0;
    static struct pollfd pf[MAX_LISTEN + MAX_CONNS];
    int code = 0;
    while (!end || time(NULL) < end) {
        int n = 0, polled = nconns;
        for (int i = 0; i < nls; i++) pf[n++] = (struct pollfd){.fd = ls[i].s, .events = POLLIN};
        for (int i = 0; i < nconns; i++)
            pf[n++] = (struct pollfd){.fd = conns[i].c ? srv_conn_sock(conns[i].c) : srv_api_sock(conns[i].a),
                                      .events = (short)conns[i].events};
        int r = poll(pf, (unsigned)n, 250);
        if (r < 0 && poll_interrupted()) continue;
        if (r < 0) {
            srv_log("poll failed: stopping");
            code = 1;   // the sockets themselves failed: stop rather than spin
            break;
        }
        for (int i = 0; i < nls; i++) {
            if (!(pf[i].revents & POLLIN)) continue;
            if (ls[i].udp) on_udp(&ls[i]);
            else on_accept(&ls[i], tls);
        }
        time_t now = time(NULL);
        for (int i = 0; i < polled; i++) {   // every connection, events or not: the services' timers run in there
            Conn *k = &conns[i];
            int ev = k->c ? srv_conn_step(k->c, pf[nls + i].revents) : srv_api_step(k->a, pf[nls + i].revents);
            if (ev < 0) k->c = NULL, k->a = NULL;   // freed by the step
            else k->events = ev;
            if (k->c && srv_conn_stale(k->c, now)) {
                srv_log("%s: no TLS after 10 s: closed", k->ip);
                srv_conn_free(k->c);
                k->c = NULL;
            } else if (k->a && srv_api_stale(k->a, now)) {
                srv_api_free(k->a);   // idle 15 s, or no TLS after 10 s
                k->a = NULL;
            }
        }
        srv_session_tick(srv_ms());
        // Replies one player's call queued for another's connection (a write sent to every listener) go out now, not
        // after the next poll: a few rounds, since stepping a connection may queue more.
        for (int round = 0, again = 1; again && round < 4; round++) {
            again = 0;
            for (int i = 0; i < nconns; i++) {
                Conn *k = &conns[i];
                if (!k->c || !srv_conn_kicked(k->c)) continue;
                again = 1;
                int ev = srv_conn_step(k->c, 0);
                if (ev < 0) k->c = NULL;
                else k->events = ev;
            }
        }
        int k = 0;
        for (int i = 0; i < nconns; i++)
            if (conns[i].c || conns[i].a) conns[k++] = conns[i];
        nconns = k;
        srv_store_tick(srv_ms());
    }
    for (int i = 0; i < nconns; i++) {
        if (conns[i].c) srv_conn_free(conns[i].c);
        else srv_api_free(conns[i].a);
    }
    for (int i = 0; i < nls; i++) sock_close(ls[i].s);
    srv_store_tick(UINT64_MAX);   // what waited for its second
    srv_log("stopped");
    return code;
}
