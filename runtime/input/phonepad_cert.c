// The phone controllers' certificate (phonepad_srv.h): a P-256 key and a self-signed X.509 certificate for the PC's
// LAN address, made here so no file ships with a key in it. Browsers do not trust it (nobody signs certificates for
// 192.168.x.x): the player accepts it once on the phone, and keeps it while the file phonepad.c saves stays.
// It follows what iOS asks of a server certificate before it offers to accept it (support.apple.com/103769): the
// address as a subjectAltName, serverAuth as an extended key usage, SHA-256, at most 825 days.
// The DER is written by hand: BearSSL (third_party/bearssl, MIT) reads and checks X.509 but does not write it.
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "bearssl.h"
#include "phonepad_srv.h"

typedef struct { uint8_t b[PPAD_CERT_MAX]; size_t n; int over; } Der;

static void raw(Der *d, const void *p, size_t n) {
    if (d->n + n > sizeof d->b) { d->over = 1; return; }
    memcpy(d->b + d->n, p, n);
    d->n += n;
}

// tag, length, content
static void tlv(Der *d, uint8_t tag, const void *content, size_t n) {
    uint8_t head[4] = {tag};
    size_t h = 2;
    if (n < 0x80) head[1] = (uint8_t)n;
    else if (n < 0x100) head[1] = 0x81, head[2] = (uint8_t)n, h = 3;
    else head[1] = 0x82, head[2] = (uint8_t)(n >> 8), head[3] = (uint8_t)n, h = 4;
    raw(d, head, h);
    raw(d, content, n);
}

static void wrap(Der *d, uint8_t tag, const Der *inner) {
    if (inner->over) d->over = 1;
    tlv(d, tag, inner->b, inner->n);
}

enum { SEQ = 0x30, SET = 0x31, INTEGER = 0x02, BITS = 0x03, OCTETS = 0x04, OID = 0x06, UTF8 = 0x0C, UTCTIME = 0x17 };

static const uint8_t OID_ECDSA_SHA256[] = {0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x04, 0x03, 0x02};
static const uint8_t OID_EC_PUBLIC_KEY[] = {0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x02, 0x01};
static const uint8_t OID_P256[] = {0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x03, 0x01, 0x07};
static const uint8_t OID_COMMON_NAME[] = {0x55, 0x04, 0x03};
static const uint8_t OID_KEY_USAGE[] = {0x55, 0x1D, 0x0F};
static const uint8_t OID_ALT_NAME[] = {0x55, 0x1D, 0x11};
static const uint8_t OID_EXT_KEY_USAGE[] = {0x55, 0x1D, 0x25};
static const uint8_t OID_SERVER_AUTH[] = {0x2B, 0x06, 0x01, 0x05, 0x05, 0x07, 0x03, 0x01};

static void name(Der *d) {   // issuer and subject: one common name
    static const char cn[] = "mkw-recomp phone controller";
    Der atv = {0}, set = {0}, rdn = {0};
    tlv(&atv, OID, OID_COMMON_NAME, sizeof OID_COMMON_NAME);
    tlv(&atv, UTF8, cn, sizeof cn - 1);
    wrap(&set, SEQ, &atv);
    wrap(&rdn, SET, &set);
    wrap(d, SEQ, &rdn);
}

static void utc_time(Der *d, int64_t t) {
    time_t tt = (time_t)t;
    struct tm g;
#ifdef _WIN32
    gmtime_s(&g, &tt);
#else
    gmtime_r(&tt, &g);
#endif
    char s[32];
    int n = snprintf(s, sizeof s, "%02d%02d%02d%02d%02d%02dZ", g.tm_year % 100, g.tm_mon + 1, g.tm_mday, g.tm_hour,
                     g.tm_min, g.tm_sec);
    tlv(d, UTCTIME, s, (size_t)n);
}

// One extension: its id and its value, DER inside an octet string
static void extension(Der *d, const uint8_t *oid, size_t oid_len, const Der *value) {
    Der e = {0};
    tlv(&e, OID, oid, oid_len);
    wrap(&e, OCTETS, value);
    wrap(d, SEQ, &e);
}

int phonecert_make(const uint8_t ip[4], int64_t now, PhoneCert *out) {
    memset(out, 0, sizeof *out);
    br_hmac_drbg_context rng;
    br_hmac_drbg_init(&rng, &br_sha256_vtable, NULL, 0);
    br_prng_seeder seed = br_prng_seeder_system(NULL);
    if (!seed || !seed(&rng.vtable)) return 0;

    const br_ec_impl *ec = br_ec_get_default();
    unsigned char kbuf[BR_EC_KBUF_PRIV_MAX_SIZE], pbuf[BR_EC_KBUF_PUB_MAX_SIZE];
    br_ec_private_key sk;
    br_ec_public_key pk;
    if (br_ec_keygen(&rng.vtable, ec, &sk, kbuf, BR_EC_secp256r1) != PPAD_KEY_LEN) return 0;
    size_t pub = br_ec_compute_pub(ec, &pk, pbuf, &sk);
    if (!pub) return 0;

    Der tbs = {0}, t = {0}, u = {0};
    static const uint8_t v3[] = {INTEGER, 1, 2};
    tlv(&tbs, 0xA0, v3, sizeof v3);                                   // version 3: it has extensions
    uint8_t serial[8];
    br_hmac_drbg_generate(&rng, serial, sizeof serial);
    serial[0] = (serial[0] & 0x7F) | 0x40;                            // positive, no leading zero byte
    tlv(&tbs, INTEGER, serial, sizeof serial);
    tlv(&t, OID, OID_ECDSA_SHA256, sizeof OID_ECDSA_SHA256);
    Der sig_alg = {0};
    wrap(&sig_alg, SEQ, &t);
    raw(&tbs, sig_alg.b, sig_alg.n);
    name(&tbs);
    t = (Der){0};
    utc_time(&t, now - 86400);                                        // a phone's clock a little behind still takes it
    utc_time(&t, now + (int64_t)PPAD_CERT_DAYS * 86400);
    wrap(&tbs, SEQ, &t);
    name(&tbs);
    t = (Der){0}, u = (Der){0};                                       // the public key: its curve, then the point
    tlv(&u, OID, OID_EC_PUBLIC_KEY, sizeof OID_EC_PUBLIC_KEY);
    tlv(&u, OID, OID_P256, sizeof OID_P256);
    wrap(&t, SEQ, &u);
    uint8_t point[1 + BR_EC_KBUF_PUB_MAX_SIZE] = {0};                 // a bit string: no unused bits
    memcpy(point + 1, pbuf, pub);
    tlv(&t, BITS, point, pub + 1);
    wrap(&tbs, SEQ, &t);

    Der exts = {0}, v = {0};
    u = (Der){0};
    tlv(&u, 0x87, ip, 4);                                             // subjectAltName: the IP address
    wrap(&v, SEQ, &u);
    extension(&exts, OID_ALT_NAME, sizeof OID_ALT_NAME, &v);
    static const uint8_t digital_signature[] = {7, 0x80};
    v = (Der){0};
    tlv(&v, BITS, digital_signature, sizeof digital_signature);
    extension(&exts, OID_KEY_USAGE, sizeof OID_KEY_USAGE, &v);
    u = (Der){0}, v = (Der){0};
    tlv(&u, OID, OID_SERVER_AUTH, sizeof OID_SERVER_AUTH);
    wrap(&v, SEQ, &u);
    extension(&exts, OID_EXT_KEY_USAGE, sizeof OID_EXT_KEY_USAGE, &v);
    t = (Der){0};
    wrap(&t, SEQ, &exts);
    wrap(&tbs, 0xA3, &t);

    Der body = {0}, cert = {0};
    wrap(&body, SEQ, &tbs);
    unsigned char hash[br_sha256_SIZE], sig[1 + 80] = {0};            // again a bit string around the signature
    br_sha256_context sha;
    br_sha256_init(&sha);
    br_sha256_update(&sha, body.b, body.n);
    br_sha256_out(&sha, hash);
    size_t sig_len = br_ecdsa_sign_asn1_get_default()(ec, &br_sha256_vtable, hash, &sk, sig + 1);
    if (!sig_len) return 0;
    raw(&body, sig_alg.b, sig_alg.n);
    tlv(&body, BITS, sig, sig_len + 1);
    wrap(&cert, SEQ, &body);
    if (cert.over) return 0;

    out->magic = PPAD_CERT_MAGIC;
    memcpy(out->key, kbuf, PPAD_KEY_LEN);
    memcpy(out->ip, ip, 4);
    out->made = now;
    out->der_len = (uint32_t)cert.n;
    memcpy(out->der, cert.b, cert.n);
    return 1;
}

int phonecert_usable(const PhoneCert *c, size_t n, const uint8_t ip[4], int64_t now) {
    return n == sizeof *c && c->magic == PPAD_CERT_MAGIC && c->der_len && c->der_len <= PPAD_CERT_MAX &&
           !memcmp(c->ip, ip, 4) && now >= c->made && now < c->made + (int64_t)(PPAD_CERT_DAYS - 30) * 86400;
}
