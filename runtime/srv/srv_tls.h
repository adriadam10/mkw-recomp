// The online server's TLS (Mbed TLS): its certificates, made on this PC the first time and kept, and the two
// configurations it serves with: HTTP/2 ("h2") for the game's gRPC port, HTTP/1.1 for the friends API next to it.
#pragma once
#include <stddef.h>

#include <mbedtls/ssl.h>

// <dir>/pki: ca.der + ca.key (the CA the game trusts, made once and never replaced: players keep it) and server.der +
// server.key (the certificate the server shows, signed by that CA, made again when missing). ips: the addresses the
// server listens on, added to the certificate's names. NULL with err set when nothing usable could be loaded or made.
typedef struct SrvTls SrvTls;
SrvTls *srv_tls_open(const char *dir, const char *const *ips, int nips, char *err, size_t cap);
const mbedtls_ssl_config *srv_tls_conf(const SrvTls *t, int h2);
// The CA certificate (DER) the runtime hands the game and the friends API serves at /v1/ca.der.
const unsigned char *srv_tls_ca(const SrvTls *t, size_t *len);
