// The online server's UDP answers (srv_udp.h).
#include "srv_udp.h"

#include <string.h>

#define STUN_COOKIE 0x2112A442u
enum { STUN_BINDING = 0x0001, STUN_BINDING_OK = 0x0101, ATTR_MAPPED = 0x0001, ATTR_XOR_MAPPED = 0x0020 };

static void be16(uint8_t *p, unsigned v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

// MAPPED-ADDRESS or XOR-MAPPED-ADDRESS (RFC 5389 15.1, 15.2); `key` is the cookie and transaction id the address is
// XORed with, NULL for the plain one. Returns the attribute's size.
static size_t put_addr(uint8_t *p, unsigned type, const SrvPeer *from, const uint8_t *key) {
    size_t alen = from->v6 ? 16 : 4;
    be16(p, type);
    be16(p + 2, (unsigned)(4 + alen));
    p[4] = 0;
    p[5] = from->v6 ? 2 : 1;
    unsigned port = from->port;
    if (key) port ^= (unsigned)(key[0] << 8 | key[1]);
    be16(p + 6, port);
    for (size_t i = 0; i < alen; i++) p[8 + i] = (uint8_t)(from->addr[i] ^ (key ? key[i] : 0));
    return 8 + alen;
}

size_t stun_reply(const uint8_t *req, size_t n, const SrvPeer *from, uint8_t *out, size_t cap) {
    // Header: type, length of the attributes, cookie (a 3489 request has its transaction id there), transaction id.
    if (n < 20 || (req[0] & 0xc0) || (unsigned)(req[0] << 8 | req[1]) != STUN_BINDING) return 0;
    size_t alen = (size_t)(req[2] << 8 | req[3]);
    if (alen != n - 20 || (alen & 3)) return 0;
    uint32_t cookie = (uint32_t)req[4] << 24 | (uint32_t)req[5] << 16 | (uint32_t)req[6] << 8 | req[7];
    if (cap < 20 + 2 * 24) return 0;
    be16(out, STUN_BINDING_OK);
    memcpy(out + 4, req + 4, 16);  // cookie (or the 3489 id's first bytes) and transaction id, as they came
    size_t len = 20;
    if (cookie == STUN_COOKIE) len += put_addr(out + len, ATTR_XOR_MAPPED, from, req + 4);
    len += put_addr(out + len, ATTR_MAPPED, from, NULL);
    be16(out + 2, (unsigned)(len - 20));
    return len;
}

static void be32(uint8_t *p, uint32_t v) {
    be16(p, v >> 16);
    be16(p + 2, v & 0xffff);
}

size_t nat_reply(const uint8_t *req, size_t n, const SrvPeer *from, const uint8_t self[4], uint8_t *out, size_t cap) {
    static const uint8_t mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};   // ::ffff:a.b.c.d
    if (n != 16 || cap < 16) return 0;
    uint32_t type = (uint32_t)req[0] << 24 | (uint32_t)req[1] << 16 | (uint32_t)req[2] << 8 | req[3];
    if (type < 101 || type > 103) return 0;
    const uint8_t *ip = from->addr;
    if (from->v6) {
        if (memcmp(from->addr, mapped, sizeof mapped)) return 0;   // the check is IPv4 only
        ip = from->addr + 12;
    }
    if (ip[0] == 127 && self && (self[0] | self[1] | self[2] | self[3])) ip = self;
    be32(out, type);
    be32(out + 4, from->port);
    memcpy(out + 8, ip, 4);
    be32(out + 12, 0);
    return 16;
}
