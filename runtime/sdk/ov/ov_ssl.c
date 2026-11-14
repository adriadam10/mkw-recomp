// nn::ssl for MKW's online ghosts (MKW_NPLN, ov_npln.c). Two clients use it:
// - main's OpenSSL (gRPC, the NPLN services) does its own TLS and only asks nn::ssl::BuiltInManager for the trusted CA
//   certificates: it gets the NPLN server's CA, so the server's certificate is verified as Nintendo's would be;
// - main's libcurl (the UGC uploads and downloads, main+0x14f02ac) does its TLS through the nnssl* connection calls. Those
//   run here with no TLS at all: what curl writes goes out on its socket as is and what arrives is handed back, so
//   curl speaks plain HTTP/1.1 to the server's HTTP port (ov_socket.c sends curl's names there). The server's URLs are
//   its own, so nothing leaves for anywhere else.
// Without MKW_NPLN the game never gets here (nifm reports no internet); the calls fail if it ever did.
//
// The connection calls are the C API (nnsslConnection*); each is a 0x30-byte sdk wrapper of the nn::ssl::Connection
// method of the same name with the object first (sdk+0x51dc30 nnsslConnectionRead -> Connection::Read(char *, int *,
// unsigned)). Results: ResultIoWouldBlock 0x1987b (module 123, description 204) is what curl's backend tests for
// (main+0x14f0ac8) on a nonblocking read or write with nothing to do.
#include <stdint.h>
#include <string.h>

#include "host.h"
#include "ov.h"
#include "platform/platform.h"

#define SSL_R_WOULD_BLOCK 0x1987bu
#define SSL_R_FAILED RT_RESULT(123, 2)   // any failure: curl reports the transfer as failed
#define SSL_R_OFF RT_RESULT(123, 1)
enum { IO_BLOCKING = 1, IO_NONBLOCKING = 2 };

typedef struct {
    uint64_t obj;   // the game's nnsslConnection
    int used, fd, io_mode;
} Conn;
#define NCONN 32
static Conn conns[NCONN];
static PlatMutex conns_lock = PLAT_MUTEX_INIT;
static volatile int32_t next_id;

static int conn_find(uint64_t obj, Conn *out) {
    int ok = 0;
    plat_mutex_lock(&conns_lock);
    for (int i = 0; i < NCONN && !ok; i++)
        if (conns[i].used && conns[i].obj == obj) *out = conns[i], ok = 1;
    plat_mutex_unlock(&conns_lock);
    return ok;
}

static void conn_set(uint64_t obj, int fd, int io_mode) {
    plat_mutex_lock(&conns_lock);
    for (int i = 0; i < NCONN; i++)
        if (conns[i].used && conns[i].obj == obj) {
            if (fd != -2) conns[i].fd = fd;
            if (io_mode) conns[i].io_mode = io_mode;
        }
    plat_mutex_unlock(&conns_lock);
}

static void ssl_ok(Cpu *c) { ret_x(c, ov_npln_on() ? 0 : SSL_R_OFF); }

// ---- library and contexts
// Result Context::Create(SslVersion): the context's id in its first 8 bytes (GetContextId reads it back)
static void ov_ctx_create(Cpu *c) {
    uint64_t *ctx = arg_p(c, 0);
    if (!ov_npln_on() || !ctx) {
        ret_x(c, SSL_R_OFF);
        return;
    }
    *ctx = (uint64_t)plat_inc32(&next_id);
    ret_x(c, 0);
}
// Result nnsslContextGetContextId(Context *, uint64_t *id)
static void ov_ctx_id(Cpu *c) {
    const uint64_t *ctx = arg_p(c, 0);
    uint64_t *out = arg_p(c, 1);
    if (out) *out = ctx ? *ctx : 0;
    ssl_ok(c);
}
// Result Context::Import{ServerPki,ClientPki,Crl}(uint64_t *id, ...): accepted, a new id
static void ov_ctx_import(Cpu *c) {
    uint64_t *out = arg_p(c, 1);
    if (out) *out = (uint64_t)plat_inc32(&next_id);
    ssl_ok(c);
}

// ---- the trusted CAs for main's OpenSSL: the NPLN server's only
// Result GetBuiltInCertificateBufSize(uint32_t *size, CaCertificateId *ids, uint32_t count)
// Result GetBuiltInCertificates(BuiltInCertificateInfo **out, uint8_t *buf, uint32_t size, CaCertificateId *, uint32_t)
// The buffer holds the entries, 0x18 bytes each {u32 id, u32 status, u64 size, u8 *data}, ending with id -1, then the
// certificates; status 1 is "enabled", the only ones main+0x115d1b4 counts.
// The ids asked for: the list (CaCertificateId, u32 each) and its count, or one entry of -1 for all. Each id asked gets
// an entry holding the server's CA, so whichever ids the caller keeps, it keeps that one; "all" gets one entry.
#define MAX_IDS 64
static uint32_t cert_ids(const Cpu *c, int ids_arg, uint32_t *ids) {
    const uint32_t *in = arg_p(c, ids_arg);
    uint32_t count = (uint32_t)arg_x(c, ids_arg + 1), k = 0;
    for (uint32_t i = 0; in && i < count && k < MAX_IDS; i++)
        if (in[i] != 0xffffffffu) ids[k++] = in[i];
    if (!k) ids[k++] = 0x100;   // all: an id of its own, outside the console's list
    return k;
}
static void ov_certs_size(Cpu *c) {
    uint32_t *out = arg_p(c, 0), n = 0, ids[MAX_IDS];
    uint32_t k = cert_ids(c, 1, ids);
    if (!ov_npln_ca(&n) || !out) {
        ret_x(c, SSL_R_OFF);
        return;
    }
    *out = (k + 1) * 0x18 + 8 + ((n + 15) & ~15u);
    ret_x(c, 0);
}
static void ov_certs(Cpu *c) {
    uint64_t *out = arg_p(c, 0);
    uint8_t *buf = arg_p(c, 1);
    uint32_t size = (uint32_t)arg_x(c, 2), n = 0, ids[MAX_IDS];
    uint32_t k = cert_ids(c, 3, ids), head = ((k + 1) * 0x18 + 15) & ~15u;
    const uint8_t *ca = ov_npln_ca(&n);
    if (!ca || !out || !buf || size < head + n) {
        ret_x(c, SSL_R_OFF);
        return;
    }
    mem_commit_range((uint64_t)(uintptr_t)buf, size);
    memset(buf, 0, head);
    for (uint32_t i = 0; i < k; i++) {
        uint8_t *e = buf + i * 0x18;
        *(uint32_t *)(e + 0) = ids[i];
        *(uint32_t *)(e + 4) = 1;   // enabled
        *(uint64_t *)(e + 8) = n;
        *(uint64_t *)(e + 0x10) = (uint64_t)(uintptr_t)(buf + head);
    }
    *(uint32_t *)(buf + k * 0x18) = 0xffffffffu;   // end
    memcpy(buf + head, ca, n);
    *out = (uint64_t)(uintptr_t)buf;
    static volatile int32_t logged;
    if (plat_inc32(&logged) <= 8)
        rt_log("npln: the game's TLS trusts the server's CA (%u bytes) for its %u built-in certificate ids (first %#x, "
               "asked from %#llx)", n, k, ids[0], (unsigned long long)c->x[30]);
    ret_x(c, 0);
}

// ---- connections (curl)
// Result nnsslConnectionCreate(Connection *, Context *)
static void ov_conn_create(Cpu *c) {
    uint64_t obj = arg_x(c, 0);
    int ok = 0;
    if (ov_npln_on() && obj) {
        plat_mutex_lock(&conns_lock);
        for (int i = 0; i < NCONN && !ok; i++)
            if (!conns[i].used || conns[i].obj == obj)
                conns[i] = (Conn){.obj = obj, .used = 1, .fd = -1, .io_mode = IO_BLOCKING}, ok = 1;
        plat_mutex_unlock(&conns_lock);
        if (!ok) rt_log("npln: no free ssl connection (%d in use)", NCONN);
    }
    ret_x(c, ok ? 0 : SSL_R_FAILED);
}
// Result nnsslConnectionDestroy(Connection *): the socket stays the game's, as curl closes it
static void ov_conn_destroy(Cpu *c) {
    uint64_t obj = arg_x(c, 0);
    plat_mutex_lock(&conns_lock);
    for (int i = 0; i < NCONN; i++)
        if (conns[i].used && conns[i].obj == obj) conns[i].used = 0;
    plat_mutex_unlock(&conns_lock);
    ret_x(c, 0);
}
// Result nnsslConnectionSetSocketDescriptor(Connection *, int fd)
static void ov_conn_fd(Cpu *c) {
    conn_set(arg_x(c, 0), (int)arg_x(c, 1), 0);
    ssl_ok(c);
}
// Result nnsslConnectionSetIoMode(Connection *, IoMode)
static void ov_conn_io(Cpu *c) {
    int m = (int)arg_x(c, 1);
    conn_set(arg_x(c, 0), -2, m == IO_NONBLOCKING ? IO_NONBLOCKING : IO_BLOCKING);
    ssl_ok(c);
}
// Result nnsslConnectionGetConnectionId(Connection *, uint64_t *)
static void ov_conn_id(Cpu *c) {
    uint64_t *out = arg_p(c, 1);
    if (out) *out = arg_x(c, 0) & 0xffffffff;
    ssl_ok(c);
}
// Result nnsslConnectionDoHandshakeWithCertBuffer(Connection *, uint32_t *cert_size, uint32_t *cert_count):
// nothing to negotiate in the clear; done as soon as the socket is connected
static void ov_conn_handshake(Cpu *c) {
    Conn k;
    uint32_t *size = arg_p(c, 1), *count = arg_p(c, 2);
    if (size) *size = 0;
    if (count) *count = 0;
    if (!ov_npln_on() || !conn_find(arg_x(c, 0), &k)) {
        ret_x(c, SSL_R_FAILED);
        return;
    }
    ret_x(c, 0);
}
// Result nnsslConnectionGetVerifyCertError(Connection *, Result *): none
static void ov_conn_verify_error(Cpu *c) {
    uint32_t *out = arg_p(c, 1);
    if (out) *out = 0;
    ssl_ok(c);
}
// Result nnsslConnectionGetNextAlpnProto(Connection *, AlpnProtoState *, uint8_t *out, uint32_t *len, uint32_t cap):
// state 2, no overlap, so curl keeps HTTP/1.1 (it asks for h2 too, main+0x14f096c); the server's HTTP port speaks 1.1
static void ov_conn_alpn(Cpu *c) {
    uint32_t *state = arg_p(c, 1), *len = arg_p(c, 3);
    if (state) *state = 2;
    if (len) *len = 0;
    ssl_ok(c);
}

// Result nnsslConnection{Read,Peek}(Connection *, char *buf, int *done, uint32_t n) and Write(..., const char *, ...)
static void conn_io(Cpu *c, int recv_side, int peek) {
    Conn k;
    int32_t *done = arg_p(c, 2);
    if (done) *done = 0;
    if (!ov_npln_on() || !conn_find(arg_x(c, 0), &k) || k.fd < 0) {
        ret_x(c, SSL_R_FAILED);
        return;
    }
    int64_t r = ov_sock_io(k.fd, arg_p(c, 1), (uint32_t)arg_x(c, 3), recv_side, peek, k.io_mode == IO_NONBLOCKING);
    if (r == OV_IO_AGAIN) {
        ret_x(c, SSL_R_WOULD_BLOCK);
        return;
    }
    if (r == OV_IO_ERROR) {
        ret_x(c, SSL_R_FAILED);
        return;
    }
    if (done) *done = (int32_t)r;   // 0 on a read: the server closed the connection
    ret_x(c, 0);
}
static void ov_conn_read(Cpu *c) { conn_io(c, 1, 0); }
static void ov_conn_peek(Cpu *c) { conn_io(c, 1, 1); }
static void ov_conn_write(Cpu *c) { conn_io(c, 0, 0); }
// Result nnsslConnectionPending(Connection *, int *): nothing is ever buffered here
static void ov_conn_pending(Cpu *c) {
    int32_t *out = arg_p(c, 1);
    if (out) *out = 0;
    ssl_ok(c);
}

HOST_OVERRIDE("nnsslInitialize", ssl_ok);
HOST_OVERRIDE("nnsslFinalize", ssl_ok);
HOST_OVERRIDE("_ZN2nn3ssl10InitializeEv", ssl_ok);
HOST_OVERRIDE("_ZN2nn3ssl8FinalizeEv", ssl_ok);
HOST_OVERRIDE("_ZN2nn3ssl7Context6CreateENS1_10SslVersionE", ov_ctx_create);
HOST_OVERRIDE("nnsslContextGetContextId", ov_ctx_id);
HOST_OVERRIDE("nnsslContextDestroy", ssl_ok);
HOST_OVERRIDE("_ZN2nn3ssl7Context15ImportServerPkiEPmPKcjNS0_17CertificateFormatE", ov_ctx_import);
HOST_OVERRIDE("_ZN2nn3ssl7Context15ImportClientPkiEPmPKcS4_jj", ov_ctx_import);
HOST_OVERRIDE("_ZN2nn3ssl7Context9ImportCrlEPmPKcj", ov_ctx_import);
HOST_OVERRIDE("_ZN2nn3ssl14BuiltInManager28GetBuiltInCertificateBufSizeEPjPNS0_15CaCertificateIdEj", ov_certs_size);
HOST_OVERRIDE("_ZN2nn3ssl14BuiltInManager22GetBuiltInCertificatesEPPNS0_26BuiltInCertificateInfoBaseEPhjPNS0_15CaCertificateIdEj",
              ov_certs);
HOST_OVERRIDE("nnsslConnectionCreate", ov_conn_create);
HOST_OVERRIDE("nnsslConnectionDestroy", ov_conn_destroy);
HOST_OVERRIDE("nnsslConnectionSetSocketDescriptor", ov_conn_fd);
HOST_OVERRIDE("nnsslConnectionSetHostName", ssl_ok);
HOST_OVERRIDE("nnsslConnectionSetVerifyOption", ssl_ok);
HOST_OVERRIDE("nnsslConnectionSetSessionCacheMode", ssl_ok);
HOST_OVERRIDE("nnsslConnectionSetOption", ssl_ok);
HOST_OVERRIDE("nnsslConnectionSetNextAlpnProto", ssl_ok);
HOST_OVERRIDE("nnsslConnectionSetIoMode", ov_conn_io);
HOST_OVERRIDE("nnsslConnectionGetNextAlpnProto", ov_conn_alpn);
HOST_OVERRIDE("nnsslConnectionGetConnectionId", ov_conn_id);
HOST_OVERRIDE("nnsslConnectionGetVerifyCertError", ov_conn_verify_error);
HOST_OVERRIDE("nnsslConnectionDoHandshakeWithCertBuffer", ov_conn_handshake);
HOST_OVERRIDE("nnsslConnectionRead", ov_conn_read);
HOST_OVERRIDE("nnsslConnectionPeek", ov_conn_peek);
HOST_OVERRIDE("nnsslConnectionWrite", ov_conn_write);
HOST_OVERRIDE("nnsslConnectionPending", ov_conn_pending);
