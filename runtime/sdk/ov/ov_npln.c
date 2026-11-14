// MKW_NPLN=<address>[:<port>] (Settings > Game > Ghost server): the game's online services (NPLN, Nintendo's gRPC
// platform) go to a self-hosted server, tools/npln/server.py, which answers the time trial's ranking, ghost uploads and
// downloads. Off by default: then nothing here runs and the game stays offline as before.
//
// What the console would supply and the runtime makes up here:
// - the network service account (NSA) of the local user, with an id derived from its Uid and an id token the server
//   accepts unchecked (the game sends it to nn.npln.auth.v1.Auth/IssueToken);
// - nifm's internet connection and nsd's names (ov_net.c), sockets over the host's (ov_socket.c), which reach only the
//   server and local networks;
// - the trusted CA certificates (ov_ssl.c): the game's own OpenSSL (main, external/openssl/nnsdk/
//   nnsdk_LoadBuiltinCerts.cpp at main+0x115cfc0) trusts what nn::ssl::BuiltInManager hands it, so it gets the
//   server's CA, which signed the server's certificate for *.npln.srv.nintendo.net. No game code is patched.
// The CA comes from the server itself (http://<address>:<port + 1>/ca.der) when the game first asks for it.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET HSock;
#define H_BAD INVALID_SOCKET
#define h_close closesocket
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int HSock;
#define H_BAD (-1)
#define h_close close
#endif

#include "cfg.h"
#include "host.h"
#include "ov.h"
#include "platform/platform.h"

#define NPLN_PORT 8443   // the server's default gRPC port; its HTTP port is the next one

static int parsed, usable;
static uint32_t server_ip;   // network order
static uint16_t server_port;

static void net_start(void) {
#ifdef _WIN32
    static int done;
    WSADATA wsa;
    if (!done) done = !WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
}

// MKW_NPLN: an IPv4 address or a host name (resolved once, by the host), and an optional port
static void parse(void) {
    static PlatMutex lock = PLAT_MUTEX_INIT;
    plat_mutex_lock(&lock);
    if (parsed) {
        plat_mutex_unlock(&lock);
        return;
    }
    const char *v = cfg_get("MKW_NPLN");
    if (v && *v) {
        char host[256];
        snprintf(host, sizeof host, "%s", v);
        char *colon = strrchr(host, ':');
        unsigned port = NPLN_PORT;
        if (colon) *colon = 0, port = (unsigned)strtoul(colon + 1, NULL, 10);
        net_start();
        struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM}, *res = NULL;
        if (port && port < 65535 && !getaddrinfo(host, NULL, &hints, &res) && res) {
            server_ip = ((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr;
            server_port = (uint16_t)port;
            usable = 1;
            char t[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &server_ip, t, sizeof t);
            rt_log("npln: online services go to the server at %s (%s), gRPC port %u, HTTP port %u", host, t, port,
                   port + 1);
        } else {
            rt_log("npln: MKW_NPLN=%s: cannot resolve the address or bad port; online stays off", v);
        }
        if (res) freeaddrinfo(res);
    }
    parsed = 1;
    plat_mutex_unlock(&lock);
}

int ov_npln_on(void) {
    if (!parsed) parse();
    return usable;
}

int ov_npln_server(uint32_t *ip, uint16_t *grpc_port, uint16_t *http_port) {
    if (!ov_npln_on()) return 0;
    if (ip) *ip = server_ip;
    if (grpc_port) *grpc_port = server_port;
    if (http_port) *http_port = (uint16_t)(server_port + 1);
    return 1;
}

// ---- the server's CA: one HTTP/1.0 GET of /ca.der on its HTTP port
const uint8_t *ov_npln_ca(uint32_t *size) {
    static PlatMutex lock = PLAT_MUTEX_INIT;
    static uint8_t *ca;
    static uint32_t ca_size;
    static int tried;
    if (!ov_npln_on()) return NULL;
    plat_mutex_lock(&lock);
    if (!tried) {
        tried = 1;
        HSock s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t)(server_port + 1))};
        a.sin_addr.s_addr = server_ip;
        size_t cap = 1 << 16, n = 0;
        uint8_t *buf = malloc(cap);
        if (s != H_BAD && buf && !connect(s, (struct sockaddr *)&a, sizeof a)) {
            static const char req[] = "GET /ca.der HTTP/1.0\r\nHost: npln\r\n\r\n";
            send(s, req, (int)sizeof req - 1, 0);
            int got;
            while (n < cap && (got = recv(s, (char *)buf + n, (int)(cap - n), 0)) > 0) n += (size_t)got;
        }
        if (s != H_BAD) h_close(s);
        uint8_t *body = NULL;
        for (size_t i = 0; i + 4 <= n; i++)
            if (!memcmp(buf + i, "\r\n\r\n", 4)) {
                body = buf + i + 4;
                break;
            }
        if (body && n > 12 && !memcmp(buf + 9, "200", 3) && buf + n > body) {
            ca_size = (uint32_t)(buf + n - body);
            ca = malloc(ca_size);
            memcpy(ca, body, ca_size);
            rt_log("npln: the server's CA certificate, %u bytes", ca_size);
        } else {
            rt_log("npln: cannot fetch the CA certificate from the server's HTTP port %u (is tools/npln/server.py "
                   "running?): the game will not trust it",
                   server_port + 1);
        }
        free(buf);
    }
    plat_mutex_unlock(&lock);
    if (size) *size = ca_size;
    return ca;
}

// ---- nn::account: the local user's network service account
// Its id: a 64-bit number from the Uid (stable across restarts, as account.uid is), never 0.
static uint64_t nsa_id(void) {
    uint64_t uid[2];
    account_uid(uid);
    uint64_t id = (uid[0] ^ (uid[1] * 0x9e3779b97f4a7c15ull)) & 0x7fffffffffffffffull;
    return id ? id : 1;
}

static void b64url(const uint8_t *in, size_t n, char *out) {
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        out[o++] = t[v >> 18 & 63], out[o++] = t[v >> 12 & 63];
        if (i + 1 < n) out[o++] = t[v >> 6 & 63];
        if (i + 2 < n) out[o++] = t[v & 63];
    }
    out[o] = 0;
}

// The id token: a JWT of the form the console's has (header.payload.signature), unsigned in substance: the server reads
// the NSA id from it and checks nothing else.
static int id_token(char *out, size_t cap) {
    char payload[512], head64[128], pay64[700];
    static const char head[] = "{\"alg\":\"RS256\",\"typ\":\"JWT\",\"kid\":\"mkw-recomp\"}";
    long long now = (long long)time(NULL);
    int pn = snprintf(payload, sizeof payload,
                      "{\"iss\":\"https://mkw-recomp.invalid\",\"sub\":\"%016llx\",\"aud\":\"npln\",\"iat\":%lld,"
                      "\"exp\":%lld,\"typ\":\"id_token\"}",
                      (unsigned long long)nsa_id(), now, now + 86400);
    b64url((const uint8_t *)head, sizeof head - 1, head64);
    b64url((const uint8_t *)payload, (size_t)pn, pay64);
    return snprintf(out, cap, "%s.%s.bWt3LXJlY29tcA", head64, pay64);
}

#define ACCOUNT_R_NSA_UNAVAILABLE (124u | (280u << 9))   // ResultNetworkServiceAccountUnavailable; description a hypothesis
static int nsa_ok(Cpu *c) {
    if (ov_npln_on()) return 1;
    ret_x(c, ACCOUNT_R_NSA_UNAVAILABLE);
    return 0;
}

// Result EnsureNetworkServiceAccountAvailable(const UserHandle &)
static void ov_nsa_ensure(Cpu *c) {
    if (nsa_ok(c)) ret_x(c, 0);
}
// Result GetNetworkServiceAccountId(NetworkServiceAccountId *out, const UserHandle &)
static void ov_nsa_id(Cpu *c) {
    if (!nsa_ok(c)) return;
    uint64_t *out = arg_p(c, 0);
    if (out) *out = nsa_id();
    ret_x(c, 0);
}
// Result LoadNetworkServiceAccountIdTokenCache(size_t *size, char *buf, size_t cap, const UserHandle &)
static void ov_nsa_token(Cpu *c) {
    if (!nsa_ok(c)) return;
    uint64_t *size = arg_p(c, 0);
    char *buf = arg_p(c, 1), tok[1024];
    uint64_t cap = arg_x(c, 2);
    int n = id_token(tok, sizeof tok);
    if (buf && cap) {
        uint64_t k = (uint64_t)n < cap ? (uint64_t)n : cap - 1;
        mem_commit_range((uint64_t)(uintptr_t)buf, k + 1);
        memcpy(buf, tok, k), buf[k] = 0;
    }
    if (size) *size = (uint64_t)n;
    static int logged;
    if (!logged++) rt_log("npln: network service account %016llx, id token of %d bytes", (unsigned long long)nsa_id(), n);
    ret_x(c, 0);
}
// Result EnsureNetworkServiceAccountIdTokenCacheAsync(AsyncContext *, const UserHandle &): done at once
static void ov_nsa_token_async(Cpu *c) {
    if (nsa_ok(c)) ret_x(c, 0);
}

// nn::account::AsyncContext: only the id token's (above) uses it in MKW, and that one is done as soon as it starts.
// The object is the game's; nothing here reads or writes it, so its size does not matter.
static void ov_async_nothing(Cpu *c) { ret_x(c, 0); }
// AsyncContext &operator=(AsyncContext &&): returns this
static void ov_async_move(Cpu *c) { ret_x(c, arg_x(c, 0)); }
// Result HasDone(bool *)
static void ov_async_done(Cpu *c) {
    uint8_t *out = arg_p(c, 1);
    if (out) *out = 1;
    ret_x(c, 0);
}
// Result GetSystemEvent(nn::os::SystemEvent *): a real event, already signalled (SystemEvent holds its
// SystemEventType first, as ov_chat.c's)
static void ov_async_event(Cpu *c) {
    static uint64_t create, signal;
    if (!create) {
        create = ldr_symbol("_ZN2nn2os17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb");
        signal = ldr_symbol("_ZN2nn2os17SignalSystemEventEPNS0_15SystemEventTypeE");
        if (!create || !signal) rt_fatal("npln: nn::os system event functions not found");
    }
    uint64_t ev = arg_x(c, 1);
    rt_call(c, create, 3, ev, 0, 0);   // manual clear, not interprocess
    rt_call(c, signal, 1, ev);
    ret_x(c, 0);
}

HOST_OVERRIDE("_ZN2nn7account36EnsureNetworkServiceAccountAvailableERKNS0_10UserHandleE", ov_nsa_ensure);
HOST_OVERRIDE("_ZN2nn7account26GetNetworkServiceAccountIdEPNS0_23NetworkServiceAccountIdERKNS0_10UserHandleE", ov_nsa_id);
HOST_OVERRIDE("_ZN2nn7account37LoadNetworkServiceAccountIdTokenCacheEPmPcmRKNS0_10UserHandleE", ov_nsa_token);
HOST_OVERRIDE("_ZN2nn7account44EnsureNetworkServiceAccountIdTokenCacheAsyncEPNS0_12AsyncContextERKNS0_10UserHandleE",
              ov_nsa_token_async);
HOST_OVERRIDE("_ZN2nn7account12AsyncContextC1Ev", ov_async_nothing);
HOST_OVERRIDE("_ZN2nn7account12AsyncContextD1Ev", ov_async_nothing);
HOST_OVERRIDE("_ZN2nn7account12AsyncContextaSEOS1_", ov_async_move);
HOST_OVERRIDE("_ZN2nn7account12AsyncContext7HasDoneEPb", ov_async_done);
HOST_OVERRIDE("_ZN2nn7account12AsyncContext9GetResultEv", ov_async_nothing);
HOST_OVERRIDE("_ZN2nn7account12AsyncContext6CancelEv", ov_async_nothing);
HOST_OVERRIDE("_ZN2nn7account12AsyncContext14GetSystemEventEPNS_2os11SystemEventE", ov_async_event);
