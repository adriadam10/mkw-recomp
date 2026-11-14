// The online server's TLS (srv_tls.h). Certificates as the game's servers need them: an RSA-2048 CA and a server
// certificate it signs for every name the game connects to (they resolve to this server: ov_net.c), SHA-256.
#include "srv_tls.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef _WIN32
#include <sys/stat.h>
#endif

#include <mbedtls/oid.h>
#include <mbedtls/pk.h>
#include <mbedtls/platform_util.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>

#include "platform/platform.h"
#include "srv.h"

struct SrvTls {
    mbedtls_x509_crt chain;   // the server's certificate, then the CA's
    mbedtls_pk_context key;
    mbedtls_ssl_config h2, http1;
    unsigned char *ca;
    size_t ca_len;
};

static const char ORG[] = "MKW Recomp server";
// The names the game reaches its servers by (a wildcard covers one label), in the order the game's servers list them.
static const char *const NAMES[] = {"*.lp1.t.npln.srv.nintendo.net", "*.t.npln.srv.nintendo.net",
                                    "*.npln.srv.nintendo.net",       "npln.srv.nintendo.net",
                                    "*.npln.nintendo.net",           "npln.nintendo.net",
                                    "localhost"};
enum { MAX_IPS = 8, DER_MAX = 4096 };

static int fail(char *err, size_t cap, const char *what, int code) {
    snprintf(err, cap, "%s (%d)", what, code);
    return -1;
}

static int save(const char *dir, const char *name, const unsigned char *data, size_t len) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    if (!plat_file_write_atomic(path, data, len)) return 0;
#ifndef _WIN32
    chmod(path, 0600);   // keys and certificates are this user's alone
#endif
    return 1;
}

// A new RSA-2048 key in pk, its DER saved as <dir>/<name>.
static int make_key(const char *dir, const char *name, mbedtls_pk_context *pk, char *err, size_t cap) {
    psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&a, PSA_KEY_TYPE_RSA_KEY_PAIR);
    psa_set_key_bits(&a, 2048);
    psa_set_key_usage_flags(&a, PSA_KEY_USAGE_SIGN_HASH | PSA_KEY_USAGE_EXPORT);
    psa_set_key_algorithm(&a, PSA_ALG_RSA_PKCS1V15_SIGN(PSA_ALG_SHA_256));
    mbedtls_svc_key_id_t id;
    psa_status_t s = psa_generate_key(&a, &id);
    if (s != PSA_SUCCESS) return fail(err, cap, "RSA key generation failed", (int)s);
    int r = mbedtls_pk_copy_from_psa(id, pk);
    psa_destroy_key(id);
    if (r) return fail(err, cap, "RSA key copy failed", r);
    unsigned char buf[DER_MAX];
    int n = mbedtls_pk_write_key_der(pk, buf, sizeof buf);   // written at the end of buf
    if (n <= 0) return fail(err, cap, "RSA key encoding failed", n);
    int ok = save(dir, name, buf + sizeof buf - n, (size_t)n);
    mbedtls_platform_zeroize(buf, sizeof buf);
    return ok ? 0 : fail(err, cap, "could not write the key", 0);
}

static void stamp(char out[16], time_t t) {
    struct tm g;
#ifdef _WIN32
    gmtime_s(&g, &t);
#else
    gmtime_r(&t, &g);
#endif
    strftime(out, 16, "%Y%m%d%H%M%S", &g);
}

// A certificate signed by issuer_key, its DER saved as <dir>/<name>. ca: the CA's own; else the server's, for the game's
// names and the listening addresses.
static int make_cert(const char *dir, const char *name, int ca, mbedtls_pk_context *subject_key,
                     mbedtls_pk_context *issuer_key, const char *const *ips, int nips, char *err, size_t cap) {
    mbedtls_x509write_cert w;
    mbedtls_x509write_crt_init(&w);
    unsigned char serial[16], buf[DER_MAX];
    char subject[128], issuer[128], from[16], until[16];
    snprintf(issuer, sizeof issuer, "O=%s,CN=%s root CA", ORG, ORG);
    if (ca) snprintf(subject, sizeof subject, "%s", issuer);
    else snprintf(subject, sizeof subject, "O=%s,CN=npln.private.test", ORG);
    time_t now = time(NULL);
    stamp(from, now - 86400);   // a day back: a peer whose clock is a little behind still accepts it
    stamp(until, now + 86400 * (ca ? 3650 : 825));
    plat_random(serial, sizeof serial);
    serial[0] = (unsigned char)((serial[0] & 0x7f) | 0x01);   // positive and not zero, as RFC 5280 asks
    // the SAN list: the game's names, then the listening addresses (raw bytes), 127.0.0.1 last
    mbedtls_x509_san_list san[sizeof NAMES / sizeof *NAMES + MAX_IPS + 1];
    unsigned char raw[MAX_IPS + 1][16];
    int nsan = 0;
    for (size_t i = 0; i < sizeof NAMES / sizeof *NAMES; i++) {
        memset(&san[nsan], 0, sizeof san[nsan]);
        san[nsan].node.type = MBEDTLS_X509_SAN_DNS_NAME;
        san[nsan].node.san.unstructured_name.p = (unsigned char *)NAMES[i];
        san[nsan].node.san.unstructured_name.len = strlen(NAMES[i]);
        nsan++;
    }
    for (int i = 0; i <= nips && i <= MAX_IPS; i++) {
        const char *ip = i < nips ? ips[i] : "127.0.0.1";
        if (i < nips && !strcmp(ip, "127.0.0.1")) continue;
        int len = srv_ip_bytes(ip, raw[i]);
        if (len <= 0) continue;
        memset(&san[nsan], 0, sizeof san[nsan]);
        san[nsan].node.type = MBEDTLS_X509_SAN_IP_ADDRESS;
        san[nsan].node.san.unstructured_name.p = raw[i];
        san[nsan].node.san.unstructured_name.len = (size_t)len;
        nsan++;
    }
    // linked last to first: the writer builds the DER from its end, so the names come out in the order listed above
    for (int i = 0; i < nsan; i++) san[i].next = i > 0 ? &san[i - 1] : NULL;
    static const unsigned char server_auth[] = MBEDTLS_OID_SERVER_AUTH;
    mbedtls_asn1_sequence eku = {{MBEDTLS_ASN1_OID, sizeof server_auth - 1, (unsigned char *)server_auth}, NULL};
    int r;
    mbedtls_x509write_crt_set_version(&w, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&w, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&w, subject_key);
    mbedtls_x509write_crt_set_issuer_key(&w, issuer_key);
    if ((r = mbedtls_x509write_crt_set_serial_raw(&w, serial, sizeof serial)) ||
        (r = mbedtls_x509write_crt_set_validity(&w, from, until)) ||
        (r = mbedtls_x509write_crt_set_issuer_name(&w, issuer)) ||
        (r = mbedtls_x509write_crt_set_subject_name(&w, subject)) ||
        (r = mbedtls_x509write_crt_set_basic_constraints(&w, ca, ca ? 0 : -1)))
        goto out;
    if (ca) {
        if ((r = mbedtls_x509write_crt_set_key_usage(&w, MBEDTLS_X509_KU_KEY_CERT_SIGN | MBEDTLS_X509_KU_CRL_SIGN)) ||
            (r = mbedtls_x509write_crt_set_subject_key_identifier(&w)))
            goto out;
    } else if ((r = mbedtls_x509write_crt_set_key_usage(
                    &w, MBEDTLS_X509_KU_DIGITAL_SIGNATURE | MBEDTLS_X509_KU_KEY_ENCIPHERMENT)) ||
               (r = mbedtls_x509write_crt_set_ext_key_usage(&w, &eku)) ||
               (r = mbedtls_x509write_crt_set_authority_key_identifier(&w)) ||
               (r = mbedtls_x509write_crt_set_subject_alternative_name(&w, &san[nsan - 1]))) {
        goto out;
    }
    r = mbedtls_x509write_crt_der(&w, buf, sizeof buf);   // written at the end of buf
    if (r > 0) r = save(dir, name, buf + sizeof buf - r, (size_t)r) ? 0 : -1;
out:
    mbedtls_x509write_crt_free(&w);
    return r ? fail(err, cap, ca ? "could not make the CA certificate" : "could not make the server certificate", r)
             : 0;
}

static int load_key(const char *dir, const char *name, mbedtls_pk_context *pk) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    uint64_t n = 0;
    uint8_t *der = plat_file_read(path, &n);
    if (!der) return -1;
    int r = mbedtls_pk_parse_key(pk, der, (size_t)n, NULL, 0);
    mbedtls_platform_zeroize(der, (size_t)n);
    free(der);
    return r;
}

static int load_cert(const char *dir, const char *name, mbedtls_x509_crt *chain) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    uint64_t n = 0;
    uint8_t *der = plat_file_read(path, &n);
    if (!der) return -1;
    int r = mbedtls_x509_crt_parse_der(chain, der, (size_t)n);
    free(der);
    return r;
}

static int setup_conf(mbedtls_ssl_config *c, SrvTls *t, int h2) {
    static const char *ALPN_H2[] = {"h2", NULL}, *ALPN_HTTP1[] = {"http/1.1", NULL};
    mbedtls_ssl_config_init(c);
    int r = mbedtls_ssl_config_defaults(c, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM,
                                        MBEDTLS_SSL_PRESET_DEFAULT);
    if (!r) r = mbedtls_ssl_conf_own_cert(c, &t->chain, &t->key);
    if (!r) r = mbedtls_ssl_conf_alpn_protocols(c, h2 ? ALPN_H2 : ALPN_HTTP1);
    mbedtls_ssl_conf_min_tls_version(c, MBEDTLS_SSL_VERSION_TLS1_2);
    return r;
}

SrvTls *srv_tls_open(const char *dir, const char *const *ips, int nips, char *err, size_t cap) {
    *err = 0;
    psa_status_t ps = psa_crypto_init();
    if (ps != PSA_SUCCESS) {
        fail(err, cap, "the crypto library did not start", (int)ps);
        return NULL;
    }
    char pki[1024];
    snprintf(pki, sizeof pki, "%s/pki", dir);
    plat_file_mkdir_p(pki);
    SrvTls *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    mbedtls_x509_crt_init(&t->chain);
    mbedtls_pk_init(&t->key);
    mbedtls_pk_context ca_key, srv_key;
    mbedtls_pk_init(&ca_key);
    mbedtls_pk_init(&srv_key);
    mbedtls_x509_crt ca;
    mbedtls_x509_crt_init(&ca);
    int ok = 0;
    char path[1100];
    snprintf(path, sizeof path, "%s/ca.der", pki);
    if (!plat_file_exists(path)) {   // first start: a CA of this PC's own
        if (make_key(pki, "ca.key", &ca_key, err, cap) ||
            make_cert(pki, "ca.der", 1, &ca_key, &ca_key, NULL, 0, err, cap))
            goto out;
    } else if (load_key(pki, "ca.key", &ca_key)) {
        fail(err, cap, "pki/ca.key is missing or damaged; the CA cannot sign a new server certificate", 0);
        goto out;
    }
    if (load_cert(pki, "ca.der", &ca)) {
        fail(err, cap, "pki/ca.der is damaged", 0);
        goto out;
    }
    // the server's certificate: made again when it is missing or does not parse (the CA, which players keep, stays)
    if (load_key(pki, "server.key", &t->key) || load_cert(pki, "server.der", &t->chain)) {
        mbedtls_pk_free(&t->key);
        mbedtls_pk_init(&t->key);
        mbedtls_x509_crt_free(&t->chain);
        mbedtls_x509_crt_init(&t->chain);
        if (make_key(pki, "server.key", &srv_key, err, cap) ||
            make_cert(pki, "server.der", 0, &srv_key, &ca_key, ips, nips, err, cap))
            goto out;
        if (load_key(pki, "server.key", &t->key) || load_cert(pki, "server.der", &t->chain)) {
            fail(err, cap, "the new server certificate does not load", 0);
            goto out;
        }
    }
    if (mbedtls_x509_crt_parse_der(&t->chain, ca.raw.p, ca.raw.len)) {
        fail(err, cap, "the CA certificate does not chain", 0);
        goto out;
    }
    t->ca = malloc(ca.raw.len);
    if (!t->ca) goto out;
    memcpy(t->ca, ca.raw.p, ca.raw.len);
    t->ca_len = ca.raw.len;
    int r = setup_conf(&t->h2, t, 1);
    if (!r) r = setup_conf(&t->http1, t, 0);
    if (r) {
        fail(err, cap, "TLS setup failed", r);
        goto out;
    }
    ok = 1;
out:
    mbedtls_pk_free(&ca_key);
    mbedtls_pk_free(&srv_key);
    mbedtls_x509_crt_free(&ca);
    if (!ok) {
        mbedtls_x509_crt_free(&t->chain);
        mbedtls_pk_free(&t->key);
        free(t->ca);
        free(t);
        return NULL;
    }
    return t;
}

const mbedtls_ssl_config *srv_tls_conf(const SrvTls *t, int h2) { return h2 ? &t->h2 : &t->http1; }

const unsigned char *srv_tls_ca(const SrvTls *t, size_t *len) {
    *len = t->ca_len;
    return t->ca;
}
