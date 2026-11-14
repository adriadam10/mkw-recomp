// Online play on a private server: settings, friend code, certificate and friends API (net_online.h).
// Windows does the https of the friends API with WinHTTP; the server's own certificate is pinned (its CA is the only
// root the chain may end in), checked before the request carries the friend code's secret.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wincrypt.h>
#include <winhttp.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "net_online.h"
#include "platform/platform.h"

void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// Pia's NAT check servers (nncs1/nncs2) are under n.n.srv, its monitoring under p.srv; the game's NPLN services
// under npln.srv. The server answers all of them.
static const char *const kHosts[] = {"npln.srv.nintendo.net", "npln.nintendo.net", "n.n.srv.nintendo.net",
                                     "p.srv.nintendo.net"};

static void wsa_init(void) {
#ifdef _WIN32
    static volatile int done;
    if (!done) {
        WSADATA w;
        WSAStartup(MAKEWORD(2, 2), &w);   // reference counted: a second start by another caller is harmless
        done = 1;
    }
#endif
}

int net_enabled(void) {
    const char *v = cfg_get("MKW_ONLINE");
    return v && *v && strcmp(v, "0") != 0;
}

// "a.b.c.d[:port]" or "name[:port]"; never a Nintendo name, which the redirect would send back to Nintendo.
static void parse_server(const char *text, NetServer *s) {
    memset(s, 0, sizeof *s);
    if (!text || !*text) return;
    snprintf(s->text, sizeof s->text, "%s", text);
    char host[160];
    snprintf(host, sizeof host, "%s", text);
    unsigned port = 8443;   // the server's online port when the setting has none (an address alone reached 443)
    char *colon = strrchr(host, ':');
    if (colon) {
        port = (unsigned)strtoul(colon + 1, NULL, 10);
        *colon = 0;
    }
    for (char *p = host; *p; p++) *p = (char)tolower((unsigned char)*p);
    if (!*host || port == 0 || port > 65535 || strstr(host, "nintendo")) {
        rt_log("net: online server \"%s\" is not <ipv4 or name>[:<port>]; online play is off", text);
        return;
    }
    wsa_init();
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_INET;
    if (getaddrinfo(host, NULL, &hints, &res) || !res) {
        rt_log("net: online server %s has no IPv4 address; online play is off", host);
        return;
    }
    memcpy(s->ip, &((struct sockaddr_in *)res->ai_addr)->sin_addr, 4);
    freeaddrinfo(res);
    s->port = (uint16_t)port;
    s->on = 1;
}

const NetServer *net_server(void) {
    static NetServer s[2];
    static volatile unsigned seen = ~0u;
    static volatile int cur;
    static PlatMutex lock = PLAT_MUTEX_INIT;
    unsigned v = cfg_version();
    if (v != seen) {
        plat_mutex_lock(&lock);
        if (v != seen) {
            int next = !cur;
            parse_server(net_enabled() ? cfg_get("MKW_ONLINE_SERVER") : NULL, &s[next]);
            if (s[next].on)
                rt_log("net: online server %s = %u.%u.%u.%u, its port 443 is %u", s[next].text, s[next].ip[0],
                       s[next].ip[1], s[next].ip[2], s[next].ip[3], s[next].port);
            cur = next;   // the other copy stays readable for a caller that took it before
            seen = v;
        }
        plat_mutex_unlock(&lock);
    }
    return &s[cur];
}

int net_redirected_host(const char *host) {
    if (!host || !net_server()->on) return 0;
    size_t n = strlen(host);
    if (n && host[n - 1] == '.') n--;   // an absolute name
    for (size_t i = 0; i < sizeof kHosts / sizeof *kHosts; i++) {
        size_t m = strlen(kHosts[i]);
        if (n == m && !strncmp(host, kHosts[i], m)) return 1;
        if (n > m && host[n - m - 1] == '.' && !strncmp(host + n - m, kHosts[i], m)) return 1;
    }
    return 0;
}

// The local address of the route toward the server (or toward a public address without one): a UDP connect sends
// nothing, it only picks the route. Through Tailscale this is the 100.x address the other players reach.
uint32_t net_local_ip(void) {
    wsa_init();
    const NetServer *s = net_server();
    struct sockaddr_in to = {0}, me = {0};
    to.sin_family = AF_INET;
    to.sin_port = htons(s->on ? s->port : 53);
    if (s->on) memcpy(&to.sin_addr, s->ip, 4);
    else to.sin_addr.s_addr = htonl(0x08080808);
    uint32_t ip = 0;
#ifdef _WIN32
    SOCKET fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd == INVALID_SOCKET) return 0;
    int len = sizeof me;
#else
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) return 0;
    socklen_t len = sizeof me;
#endif
    if (!connect(fd, (struct sockaddr *)&to, sizeof to) && !getsockname(fd, (struct sockaddr *)&me, &len))
        ip = me.sin_addr.s_addr;
#ifdef _WIN32
    closesocket(fd);
#else
    close(fd);
#endif
    return ip;
}

// ---- the profile: <exe dir>/profile/online/<key>.txt, Droplet's format
static int online_dir(char *out, size_t cap) {
    char exe[1024];
    if (!plat_exe_path(exe, sizeof exe)) return 0;
    char *slash = strrchr(exe, '/'), *bs = strrchr(exe, '\\');
    if (bs > slash) slash = bs;
    if (!slash) return 0;
    *slash = 0;
    snprintf(out, cap, "%s/profile/online", exe);
    return 1;
}

// This PC's own server written as one of its addresses ("192.168.1.10:8443", "100.x.y.z:8443", "127.0.0.1"):
// its port, else 0. The address alone is enough: no name is resolved here.
static unsigned this_pc_port(const char *server) {
    char host[64];
    snprintf(host, sizeof host, "%s", server ? server : "");
    unsigned port = 8443;
    char *colon = strrchr(host, ':');
    if (colon) port = (unsigned)strtoul(colon + 1, NULL, 10), *colon = 0;
    struct in_addr a;
    wsa_init();
    if (!port || port > 65535 || inet_pton(AF_INET, host, &a) != 1 || !net_is_this_pc(a.s_addr)) return 0;
    return port;
}

static void profile_key(const char *server, char *out, size_t cap) {
    // this PC's own server keeps one friend code whatever address it is played at (home network, Tailscale)
    unsigned own = this_pc_port(server);
    if (own) {
        snprintf(out, cap, "ce-pc_%u", own);
        return;
    }
    size_t n = 0;
    for (const char *p = server; *p && n + 1 < cap; p++) {
        char ch = (char)tolower((unsigned char)*p);
        out[n++] = isalnum((unsigned char)ch) || ch == '.' || ch == '-' ? ch : '_';
    }
    out[n] = 0;
    if (!n) snprintf(out, cap, "default");
}

static int profile_path(const char *server, const char *ext, char *out, size_t cap) {
    char dir[1100], key[200];
    if (!online_dir(dir, sizeof dir)) return 0;
    profile_key(server, key, sizeof key);
    snprintf(out, cap, "%s/%s%s", dir, key, ext);
    return 1;
}

void net_format_code(const char *code, char out[18]) {
    snprintf(out, 18, "SW-%.4s-%.4s-%.4s", code, code + 4, code + 8);
}

int net_parse_code(const char *text, char out[13]) {
    int n = 0;
    for (const char *p = text; p && *p; p++) {
        if (isdigit((unsigned char)*p)) {
            if (n == 12) return 0;
            out[n++] = *p;
        } else if (!(*p == '-' || *p == ' ' || ((*p == 'S' || *p == 's' || *p == 'W' || *p == 'w') && n == 0))) {
            return 0;
        }
    }
    out[n] = 0;
    return n == 12 && strcmp(out, "000000000000") != 0;
}

static void parse_codes(const char *v, char (*codes)[13], int *n) {
    *n = 0;
    char buf[13];
    while (v && *v && *n < NET_MAX_CODES) {
        const char *comma = strchr(v, ',');
        size_t len = comma ? (size_t)(comma - v) : strlen(v);
        if (len == 12) {
            memcpy(buf, v, 12);
            buf[12] = 0;
            if (net_parse_code(buf, codes[*n])) (*n)++;
        }
        v = comma ? comma + 1 : NULL;
    }
}

static int new_profile(NetProfile *p) {
    uint8_t r[24];
    if (!plat_random(r, sizeof r)) return 0;
    uint64_t v = 0;
    memcpy(&v, r, 8);
    snprintf(p->code, sizeof p->code, "%012llu", (unsigned long long)(v % 999999999999ull + 1));
    for (int i = 0; i < 16; i++) snprintf(p->secret + 2 * i, 3, "%02x", r[8 + i]);
    return 1;
}

// One profile file into *p: 1 when it holds a code and a secret
static int profile_read(const char *path, NetProfile *p) {
    memset(p, 0, sizeof *p);
    uint64_t size = 0;
    char *text = (char *)plat_file_read(path, &size);
    if (!text) return 0;
    char *line = text;
    while (line && line < text + size) {
        char *end = memchr(line, '\n', (size_t)(text + size - line));
        if (end) *end = 0;
        size_t len = strlen(line);
        if (len && line[len - 1] == '\r') line[len - 1] = 0;
        char *eq = strchr(line, '=');
        if (eq) {
            *eq = 0;
            const char *v = eq + 1;
            if (!strcmp(line, "code")) net_parse_code(v, p->code);
            else if (!strcmp(line, "secret")) snprintf(p->secret, sizeof p->secret, "%s", v);
            else if (!strcmp(line, "friends")) parse_codes(v, p->friends, &p->nfriends);
            else if (!strcmp(line, "removed")) parse_codes(v, p->removed, &p->nremoved);
            else if (!strcmp(line, "machine")) snprintf(p->machine, sizeof p->machine, "%s", v);
        }
        line = end ? end + 1 : NULL;
    }
    free(text);
    if (p->code[0] && strlen(p->secret) >= 16) return 1;
    rt_log("net: %s is damaged", path);
    memset(p, 0, sizeof *p);
    return 0;
}

int net_profile_load(const char *server, NetProfile *p, int create) {
    memset(p, 0, sizeof *p);
    char path[1400], dir[1100];
    if (!server || !*server || !profile_path(server, ".txt", path, sizeof path)) return 0;
    if (profile_read(path, p)) {
        if (!p->machine[0]) return net_profile_save(server, p), 1;   // from before the mark: this PC's from now on
        if (!strcmp(p->machine, net_machine_id())) return 1;
        // A copied folder: this friend code is another PC's. Kept aside; this PC gets its own.
        char aside[1450];
        snprintf(aside, sizeof aside, "%s.autre-pc", path);
        plat_file_replace(path, aside);
        rt_log("net: %s was made on another PC (kept as %s): this PC gets its own friend code", path, aside);
        memset(p, 0, sizeof *p);
    }
    // This PC's own server, first time under its own key: the friend code it had under one of its addresses, the
    // old key ("a.b.c.d_<port>", or "a.b.c.d" for one written without its port, 8443 now). Tailscale's first (the one
    // its friends know), then the home network's and 127.0.0.1's.
    unsigned own = this_pc_port(server);
    if (own && online_dir(dir, sizeof dir)) {
        NetAddrs a;
        net_addrs(&a);
        const char *ips[3] = {a.ts, a.home, "127.0.0.1"};
        for (int i = 0; i < 6; i++) {
            const char *ip = ips[i / 2];
            if (!ip[0] || ((i & 1) && own != 8443)) continue;
            char old[1300];
            if (i & 1) snprintf(old, sizeof old, "%s/%s.txt", dir, ip);
            else snprintf(old, sizeof old, "%s/%s_%u.txt", dir, ip, own);
            if (!plat_file_exists(old) || !profile_read(old, p)) continue;
            if (p->machine[0] && strcmp(p->machine, net_machine_id())) continue;   // another PC's
            if (!net_profile_save(server, p)) return 0;
            char shown[18];
            net_format_code(p->code, shown);
            rt_log("net: this PC's server keeps friend code %s (from %s)", shown, old);
            return 1;
        }
    }
    if (!create || !new_profile(p)) return 0;
    if (!net_profile_save(server, p)) return 0;
    char shown[18];
    net_format_code(p->code, shown);
    rt_log("net: new friend code %s for %s (%s)", shown, server, path);
    return 1;
}

static void join_codes(char *out, size_t cap, const char (*codes)[13], int n) {
    size_t used = 0;
    out[0] = 0;
    for (int i = 0; i < n && used + 14 < cap; i++)
        used += (size_t)snprintf(out + used, cap - used, "%s%s", i ? "," : "", codes[i]);
}

int net_profile_save(const char *server, const NetProfile *p) {
    char path[1400], dir[1100];
    if (!profile_path(server, ".txt", path, sizeof path) || !online_dir(dir, sizeof dir)) return 0;
    plat_file_mkdir_p(dir);
    static char friends[NET_MAX_CODES * 13 + 1], removed[NET_MAX_CODES * 13 + 1], text[4096];
    static PlatMutex lock = PLAT_MUTEX_INIT;
    plat_mutex_lock(&lock);
    join_codes(friends, sizeof friends, p->friends, p->nfriends);
    join_codes(removed, sizeof removed, p->removed, p->nremoved);
    int n = snprintf(text, sizeof text, "server=%s\ncode=%s\nsecret=%s\nfriends=%s\nremoved=%s\nmachine=%s\n", server,
                     p->code, p->secret, friends, removed, net_machine_id());
    int ok = n > 0 && (size_t)n < sizeof text && plat_file_write_atomic(path, text, (size_t)n);
    plat_mutex_unlock(&lock);
    if (!ok) rt_log("net: could not save %s", path);
    return ok;
}

int net_identity_token(const char *build, char *out, size_t cap) {
    const NetServer *s = net_server();
    NetProfile p;
    if (!s->on || !net_profile_load(s->text, &p, 1)) return 0;
    char friends[NET_MAX_CODES * 13 + 1], removed[NET_MAX_CODES * 13 + 1];
    join_codes(friends, sizeof friends, (const char (*)[13])p.friends, p.nfriends);
    join_codes(removed, sizeof removed, (const char (*)[13])p.removed, p.nremoved);
    int n = snprintf(out, cap, "droplet-online-v1;code=%s;secret=%s%s%s;friends=%s%s%s", p.code, p.secret,
                     build && *build ? ";build=" : "", build ? build : "", friends, p.nremoved ? ";removed=" : "",
                     removed);
    return n > 0 && (size_t)n < cap;
}

// ---- the certificate
static uint8_t *pem_to_der(const char *pem, size_t n, size_t *len) {
    const char *b = strstr(pem, "-----BEGIN CERTIFICATE-----");
    if (!b) return NULL;
    b += 27;
    const char *e = strstr(b, "-----END");
    if (!e || e > pem + n) return NULL;
    uint8_t *out = malloc((size_t)(e - b));
    if (!out) return NULL;
    size_t o = 0;
    unsigned acc = 0;
    int bits = 0;
    for (const char *p = b; p < e; p++) {
        int v;
        if (*p >= 'A' && *p <= 'Z') v = *p - 'A';
        else if (*p >= 'a' && *p <= 'z') v = *p - 'a' + 26;
        else if (*p >= '0' && *p <= '9') v = *p - '0' + 52;
        else if (*p == '+') v = 62;
        else if (*p == '/') v = 63;
        else continue;
        acc = acc << 6 | (unsigned)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[o++] = (uint8_t)(acc >> bits);
        }
    }
    *len = o;
    return out;
}

static uint8_t *read_cert(const char *path, size_t *len) {
    uint64_t size = 0;
    uint8_t *data = plat_file_read(path, &size);
    if (!data) return NULL;
    if (size > 64 && data[0] == 0x30) {   // DER: an X.509 SEQUENCE
        *len = (size_t)size;
        return data;
    }
    char *pem = realloc(data, (size_t)size + 1);
    if (!pem) {
        free(data);
        return NULL;
    }
    pem[size] = 0;
    uint8_t *der = pem_to_der(pem, (size_t)size, len);
    free(pem);
    return der;
}

uint8_t *net_server_ca(const char *server, size_t *len) {
    char path[1400];
    const char *set = cfg_get("MKW_ONLINE_CA");
    if (set && *set) return read_cert(set, len);
    if (!server || !*server) return NULL;
    // this PC's own server (net_host.c): the certificate of its pki, beside mkw, before any saved copy (a copy saved
    // from a server it ran before would no longer match)
    uint8_t *der;
    char exe[900];
    char *cut = this_pc_port(server) && plat_exe_path(exe, sizeof exe) ? plat_last_sep(exe) : NULL;
    if (cut) {
        *cut = 0;
        snprintf(path, sizeof path, "%s/server/pki/ca.der", exe);
        if ((der = read_cert(path, len))) return der;
    }
    der = profile_path(server, ".ca.der", path, sizeof path) ? read_cert(path, len) : NULL;
    if (der) return der;
    // Droplet's copy for the same server, when this PC also runs Droplet
    char base[900], key[200];
    profile_key(server, key, sizeof key);
#ifdef _WIN32
    if (!plat_getenv("APPDATA", base, sizeof base)) return NULL;
    snprintf(path, sizeof path, "%s/Droplet-online/profiles/%s.ca.der", base, key);
#else
    if (!plat_getenv("HOME", base, sizeof base)) return NULL;
    snprintf(path, sizeof path, "%s/.local/share/Droplet-online/profiles/%s.ca.der", base, key);
#endif
    return read_cert(path, len);
}

// ---- https to the server's API port (its online port + 1)
static int api_address(const char *server, char *host, size_t cap, int *port) {
    snprintf(host, cap, "%s", server);
    unsigned online = 8443;   // as parse_server
    char *colon = strrchr(host, ':');
    if (colon) {
        online = (unsigned)strtoul(colon + 1, NULL, 10);
        *colon = 0;
    }
    *port = (int)online + 1;
    return *host && online && online < 65535;
}

typedef struct {
    char *body;
    size_t len;
    int status;
} Reply;

#ifdef _WIN32
typedef struct { const uint8_t *ca; size_t ca_len; volatile int bad, checked; } Pin;

// The server certificate's chain must end in the pinned CA alone (an engine whose only root is that CA).
static int chain_ok(PCCERT_CONTEXT leaf, const uint8_t *ca, size_t ca_len) {
    HCERTSTORE roots = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, 0, NULL);
    HCERTCHAINENGINE engine = NULL;
    PCCERT_CHAIN_CONTEXT chain = NULL;
    int ok = 0;
    if (roots && CertAddEncodedCertificateToStore(roots, X509_ASN_ENCODING, ca, (DWORD)ca_len,
                                                  CERT_STORE_ADD_ALWAYS, NULL)) {
        CERT_CHAIN_ENGINE_CONFIG cfg;
        memset(&cfg, 0, sizeof cfg);
        cfg.cbSize = sizeof cfg;
        cfg.hExclusiveRoot = roots;
        CERT_CHAIN_PARA para;
        memset(&para, 0, sizeof para);
        para.cbSize = sizeof para;
        if (CertCreateCertificateChainEngine(&cfg, &engine) &&
            CertGetCertificateChain(engine, leaf, NULL, leaf->hCertStore, &para, 0, NULL, &chain))
            ok = chain->TrustStatus.dwErrorStatus == CERT_TRUST_NO_ERROR;
    }
    if (chain) CertFreeCertificateChain(chain);
    if (engine) CertFreeCertificateChainEngine(engine);
    if (roots) CertCloseStore(roots, 0);
    return ok;
}

// Called once the TLS handshake is done and before the request goes out (SENDING_REQUEST): a server whose
// certificate fails the pin never gets the headers, the request is closed here.
static void CALLBACK on_status(HINTERNET h, DWORD_PTR ctx, DWORD status, LPVOID info, DWORD len) {
    (void)info, (void)len;
    Pin *pin = (Pin *)ctx;
    if (status != WINHTTP_CALLBACK_STATUS_SENDING_REQUEST || !pin || !pin->ca || pin->checked) return;
    pin->checked = 1;
    PCCERT_CONTEXT cert = NULL;
    DWORD n = sizeof cert;
    if (!WinHttpQueryOption(h, WINHTTP_OPTION_SERVER_CERT_CONTEXT, &cert, &n) || !cert ||
        !chain_ok(cert, pin->ca, pin->ca_len))
        pin->bad = 1;
    if (cert) CertFreeCertificateContext(cert);
    if (pin->bad) WinHttpCloseHandle(h);
}

static void wide(const char *s, wchar_t *out, int cap) {
    if (!MultiByteToWideChar(CP_UTF8, 0, s, -1, out, cap)) out[0] = 0;
}

// method GET or POST; the headers carry the friend code and secret when p; ca NULL: no pin (fetching the CA itself).
static const char *https(const char *server, const char *method, const char *path, const char *body,
                         const NetProfile *p, const uint8_t *ca, size_t ca_len, Reply *r, char *err, size_t cap) {
    memset(r, 0, sizeof *r);
    char host[160];
    int port;
    if (!api_address(server, host, sizeof host, &port)) {
        snprintf(err, cap, "the server address is not name:port");
        return err;
    }
    if (p && !ca) {
        snprintf(err, cap, "no certificate to check the server with");
        return err;
    }
    wchar_t whost[160], wpath[200], wmethod[8];
    wide(host, whost, 160), wide(path, wpath, 200), wide(method, wmethod, 8);
    HINTERNET ses = WinHttpOpen(L"mkw-recomp", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET con = ses ? WinHttpConnect(ses, whost, (INTERNET_PORT)port, 0) : NULL;
    HINTERNET req = con ? WinHttpOpenRequest(con, wmethod, wpath, NULL, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                        : NULL;
    Pin pin = {ca, ca_len, 0, 0};
    const char *res = err;
    if (!req) {
        snprintf(err, cap, "cannot open a connection to %s:%d", host, port);
        goto done;
    }
    WinHttpSetTimeouts(req, 4000, 4000, 8000, 8000);
    // The server's certificate names Nintendo's hosts, not this address, and comes from its own CA: Windows' checks
    // are off and the pin is the check.
    DWORD flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                  SECURITY_FLAG_IGNORE_CERT_DATE_INVALID | SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
    WinHttpSetOption(req, WINHTTP_OPTION_SECURITY_FLAGS, &flags, sizeof flags);
    if (ca) {
        DWORD_PTR ctx = (DWORD_PTR)&pin;
        WinHttpSetOption(req, WINHTTP_OPTION_CONTEXT_VALUE, &ctx, sizeof ctx);
        WinHttpSetStatusCallback(req, on_status, WINHTTP_CALLBACK_FLAG_SEND_REQUEST, 0);
    }
    wchar_t headers[400] = L"";
    if (p) {
        char h[300];
        snprintf(h, sizeof h, "X-Droplet-Code: %s\r\nX-Droplet-Secret: %s\r\nX-Droplet-Version: mkw-recomp\r\n%s",
                 p->code, p->secret, body ? "Content-Type: application/json\r\n" : "");
        wide(h, headers, 400);
    }
    DWORD blen = body ? (DWORD)strlen(body) : 0;
    BOOL sent = WinHttpSendRequest(req, headers[0] ? headers : WINHTTP_NO_ADDITIONAL_HEADERS, (DWORD)-1L,
                                   (LPVOID)body, blen, blen, 0);
    if (pin.bad) {
        req = NULL;   // closed by the callback
        snprintf(err, cap, "the server's certificate is not the one saved for %s (another server?)", server);
        goto done;
    }
    // a server that does not answer never shows its certificate: said as such, not as a certificate problem
    if (!sent) {
        snprintf(err, cap, "%s:%d does not answer (error %lu)", host, port, (unsigned long)GetLastError());
        goto done;
    }
    if (ca && !pin.checked) {
        snprintf(err, cap, "could not check the server's certificate");
        goto done;
    }
    if (!WinHttpReceiveResponse(req, NULL)) {
        snprintf(err, cap, "%s:%d does not answer (error %lu)", host, port, (unsigned long)GetLastError());
        goto done;
    }
    DWORD status = 0, sl = sizeof status;
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &status, &sl, NULL);
    r->status = (int)status;
    for (;;) {
        DWORD avail = 0, got = 0;
        if (!WinHttpQueryDataAvailable(req, &avail) || !avail || r->len + avail > (1u << 20)) break;
        char *nb = realloc(r->body, r->len + avail + 1);
        if (!nb) break;
        r->body = nb;
        if (!WinHttpReadData(req, r->body + r->len, avail, &got) || !got) break;
        r->len += got;
    }
    if (r->body) r->body[r->len] = 0;
    res = "";
done:
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    if (ses) WinHttpCloseHandle(ses);
    return res;
}
#else
static const char *https(const char *server, const char *method, const char *path, const char *body,
                         const NetProfile *p, const uint8_t *ca, size_t ca_len, Reply *r, char *err, size_t cap) {
    (void)server, (void)method, (void)path, (void)body, (void)p, (void)ca, (void)ca_len, (void)api_address;
    memset(r, 0, sizeof *r);
    snprintf(err, cap, "the friends page needs the Windows build for now; put the server's ca.der in profile/online");
    return err;
}
#endif

const char *net_fetch_ca(const char *server, char *err, size_t cap) {
    Reply r;
    // Trust on first use, as Droplet does: nothing is pinned yet and nothing secret is sent.
    const char *e = https(server, "GET", "/v1/ca.der", NULL, NULL, NULL, 0, &r, err, cap);
    if (*e) return e;
    if (r.status != 200 || r.len < 64 || r.len > 16384 || (uint8_t)r.body[0] != 0x30) {
        snprintf(err, cap, "%s (HTTP %d)", r.status == 404 ? "this server does not hand out its certificate"
                                                           : "the server's answer was not a certificate", r.status);
        free(r.body);
        return err;
    }
    char path[1400], dir[1100];
    int ok = profile_path(server, ".ca.der", path, sizeof path) && online_dir(dir, sizeof dir);
    if (ok) plat_file_mkdir_p(dir);
    ok = ok && plat_file_write_atomic(path, r.body, r.len);
    free(r.body);
    if (!ok) {
        snprintf(err, cap, "could not save the certificate");
        return err;
    }
    rt_log("net: saved the certificate of %s (%s)", server, path);
    return "";
}

void net_forget_ca(const char *server) {
    char path[1400];
    if (server && *server && profile_path(server, ".ca.der", path, sizeof path) && plat_file_exists(path)) {
        plat_file_delete(path);
        rt_log("net: forgot the saved certificate of %s: the next call fetches it again", server);
    }
}

// ---- the friends API's JSON: {"name": "...", "friends": [{"code": "...", "name": "...", "online": true}], ...}
static const char *json_string(const char *p, char *out, size_t cap) {
    size_t n = 0;
    if (*p != '"') return NULL;
    for (p++; *p && *p != '"'; p++) {
        char ch = *p;
        if (ch == '\\' && p[1]) {
            p++;
            ch = *p == 'n' ? '\n' : *p == 't' ? '\t' : *p == 'u' ? '?' : *p;
            if (*p == 'u') {
                for (int i = 0; i < 4 && p[1]; i++) p++;
            }
        }
        if (n + 1 < cap) out[n++] = ch;
    }
    out[n] = 0;
    return *p == '"' ? p + 1 : NULL;
}

static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
    return p;
}

// The value after "key": between p and end (this format's keys are distinct, nested ones included).
static const char *json_key(const char *p, const char *end, const char *key) {
    char want[48];
    snprintf(want, sizeof want, "\"%s\"", key);
    size_t n = strlen(want);
    for (const char *q = p; q && q + n <= end; q += n) {
        q = strstr(q, want);
        if (!q || q + n > end) return NULL;
        const char *v = skip_ws(q + n);
        if (*v == ':') return skip_ws(v + 1);
    }
    return NULL;
}

static void json_entries(const char *body, const char *key, NetFriend *out, int *n) {
    *n = 0;
    const char *a = json_key(body, body + strlen(body), key);
    if (!a || *a != '[') return;
    const char *p = a + 1;
    while (*n < NET_MAX_CODES) {
        p = skip_ws(p);
        if (*p != '{') break;
        const char *end = strchr(p, '}');
        if (!end) break;
        NetFriend *f = &out[*n];
        memset(f, 0, sizeof *f);
        char code[40] = "";
        const char *v = json_key(p, end, "code");
        if (v) json_string(v, code, sizeof code);
        v = json_key(p, end, "name");
        if (v) json_string(v, f->name, sizeof f->name);
        v = json_key(p, end, "online");
        f->online = v && !strncmp(v, "true", 4);
        if (net_parse_code(code, f->code)) (*n)++;
        p = skip_ws(end + 1);
        if (*p == ',') p++;
    }
}

static int load_for_api(const char *server, NetProfile *p, uint8_t **ca, size_t *ca_len, char *err, size_t cap) {
    if (!net_profile_load(server, p, 1)) {
        snprintf(err, cap, "no friend code for this server (cannot write profile/online)");
        return 0;
    }
    *ca = net_server_ca(server, ca_len);
    if (!*ca && !*net_fetch_ca(server, err, cap)) *ca = net_server_ca(server, ca_len);
    return *ca != NULL;
}

void net_friends_get(const char *server, NetFriendLists *out) {
    memset(out, 0, sizeof *out);
    NetProfile p;
    uint8_t *ca = NULL;
    size_t ca_len = 0;
    if (!load_for_api(server, &p, &ca, &ca_len, out->error, sizeof out->error)) return;
    Reply r;
    const char *e = https(server, "GET", "/v1/friends", NULL, &p, ca, ca_len, &r, out->error, sizeof out->error);
    free(ca);
    if (*e) return;
    out->reached = 1;
    if (r.status != 200 || !r.body) {
        char why[160] = "";
        const char *v = r.body ? json_key(r.body, r.body + r.len, "error") : NULL;
        if (v) json_string(v, why, sizeof why);
        snprintf(out->error, sizeof out->error, "%s",
                 *why ? why : r.status == 404 ? "this server has no friend requests (older server)" : "the server refused");
    } else {
        const char *v = json_key(r.body, r.body + r.len, "name");
        if (v && *v == '"') json_string(v, out->name, sizeof out->name);
        json_entries(r.body, "friends", out->friends, &out->nfriends);
        json_entries(r.body, "received", out->received, &out->nreceived);
        json_entries(r.body, "sent", out->sent, &out->nsent);
    }
    free(r.body);
}

static void list_drop(char (*codes)[13], int *n, const char *code) {
    for (int i = 0; i < *n; i++)
        if (!strcmp(codes[i], code)) {
            memmove(codes[i], codes[i + 1], (size_t)(*n - i - 1) * 13);
            (*n)--;
            return;
        }
}

static void list_add(char (*codes)[13], int *n, const char *code) {
    for (int i = 0; i < *n; i++)
        if (!strcmp(codes[i], code)) return;
    if (*n < NET_MAX_CODES) snprintf(codes[(*n)++], 13, "%s", code);
}

const char *net_friends_do(const char *server, const char *op, const char *text, char *err, size_t cap) {
    char code[13];
    if (!net_parse_code(text, code)) {
        snprintf(err, cap, "a friend code has 12 digits, like SW-1234-5678-9012");
        return err;
    }
    NetProfile p;
    uint8_t *ca = NULL;
    size_t ca_len = 0;
    if (!load_for_api(server, &p, &ca, &ca_len, err, cap)) return err;
    if (!strcmp(code, p.code)) {
        free(ca);
        snprintf(err, cap, "that is your own friend code");
        return err;
    }
    char path[64], body[64];
    snprintf(path, sizeof path, "/v1/friends/%s", op);
    snprintf(body, sizeof body, "{\"code\": \"%s\"}", code);
    Reply r;
    const char *e = https(server, "POST", path, body, &p, ca, ca_len, &r, err, cap);
    free(ca);
    if (*e) return e;
    if (r.status != 200) {
        char why[160] = "";
        const char *v = r.body ? json_key(r.body, r.body + r.len, "error") : NULL;
        if (v) json_string(v, why, sizeof why);
        snprintf(err, cap, "%s", *why ? why : "the server refused");
        free(r.body);
        return err;
    }
    free(r.body);
    // The identity token carries the same list (the server reads it at each sign-in): keep both in step.
    if (!strcmp(op, "request") || !strcmp(op, "accept")) {
        list_add(p.friends, &p.nfriends, code);
        list_drop(p.removed, &p.nremoved, code);
    } else {
        list_drop(p.friends, &p.nfriends, code);
        if (!strcmp(op, "remove")) list_add(p.removed, &p.nremoved, code);
    }
    net_profile_save(server, &p);
    return "";
}

const char *net_probe(const char *server, char *err, size_t cap) {
    NetServer s;
    parse_server(server, &s);
    if (!s.on) {
        snprintf(err, cap, "write the server as address:port, like 100.x.y.z:8443");
        return err;
    }
    struct sockaddr_in to = {0};
    to.sin_family = AF_INET;
    to.sin_port = htons(s.port);
    memcpy(&to.sin_addr, s.ip, 4);
    int ok = 0;
#ifdef _WIN32
    SOCKET fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd != INVALID_SOCKET) {
        u_long nb = 1;
        ioctlsocket(fd, FIONBIO, &nb);
        connect(fd, (struct sockaddr *)&to, sizeof to);
        WSAPOLLFD pf = {fd, POLLWRNORM, 0};
        if (WSAPoll(&pf, 1, 3000) == 1 && (pf.revents & POLLWRNORM) && !(pf.revents & (POLLERR | POLLHUP))) ok = 1;
        closesocket(fd);
    }
#else
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd >= 0) {
        fcntl(fd, F_SETFL, O_NONBLOCK);
        connect(fd, (struct sockaddr *)&to, sizeof to);
        struct pollfd pf = {fd, POLLOUT, 0};
        int e = 0;
        socklen_t el = sizeof e;
        if (poll(&pf, 1, 3000) == 1 && !getsockopt(fd, SOL_SOCKET, SO_ERROR, &e, &el) && !e) ok = 1;
        close(fd);
    }
#endif
    if (ok) return "";
    snprintf(err, cap, "%u.%u.%u.%u:%u does not answer: is the server started, and Tailscale on?", s.ip[0], s.ip[1],
             s.ip[2], s.ip[3], s.port);
    return err;
}
