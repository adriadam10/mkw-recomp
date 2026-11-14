// The phone controllers' server (phonepad_srv.c) and its certificate (phonepad_cert.c), without SDL or the rest of
// the runtime: phonepad.c gives each phone a virtual gamepad through PhoneSink, tests/runtime/t_phonepad.c a table it
// prints. One thread calls everything here.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "phonepad_proto.h"

// ---- the certificate: this PC's own, for the address the phones open

enum { PPAD_KEY_LEN = 32, PPAD_CERT_MAX = 640, PPAD_CERT_MAGIC = 0x31435050 };   // "PPC1"
typedef struct {
    uint32_t magic;
    uint8_t key[PPAD_KEY_LEN];    // the P-256 private key
    uint8_t ip[4];              // the address it names
    int64_t made;               // Unix time
    uint32_t der_len;
    uint8_t der[PPAD_CERT_MAX];   // the X.509 certificate
} PhoneCert;

// A new key and a certificate for ip, valid from the day before now for PPAD_CERT_DAYS; 0 when the system gives no
// random numbers.
int phonecert_make(const uint8_t ip[4], int64_t now, PhoneCert *out);
// c (as read from a file) is whole, names ip and has at least a month left: the phones that accepted it keep it
int phonecert_usable(const PhoneCert *c, size_t n, const uint8_t ip[4], int64_t now);
enum { PPAD_CERT_DAYS = 800 };   // Apple refuses server certificates valid for more than 825 days

// ---- the server

enum { PPAD_MAX_PADS = 8 };

typedef struct {
    void *ctx;
    // A phone joined: its controller, NULL when there is none to give. peer: its address, for the log.
    void *(*attach)(void *ctx, const char *peer);
    void (*detach)(void *ctx, void *pad);
    void (*state)(void *ctx, void *pad, const PpadState *s);
    // What goes back with the next answer: rumble low and high (0..255), the player (1-8, 0 not known), 0.
    void (*feedback)(void *ctx, void *pad, uint8_t out[PPAD_REPLY_LEN]);
    void (*log)(void *ctx, const char *line);
} PhoneSink;

typedef struct {
    uint8_t bind[4];              // 0.0.0.0: every interface
    uint16_t port;
    char code[PPAD_CODE_LEN + 1];   // what a phone must send to join
    const PhoneCert *cert;        // NULL: plain HTTP (the tests; a browser gives such a page no sensors)
    int any_peer;                 // 0: only addresses of a local network are answered
    int verbose;                  // every connection and TLS failure in the log
    int gone_ms;                  // a phone silent for this long loses its controller; 0: ten seconds
} PhoneSrvConfig;

typedef struct PhoneSrv PhoneSrv;
PhoneSrv *phonesrv_open(const PhoneSrvConfig *cfg, const PhoneSink *sink);   // NULL: the port did not open (logged)
void phonesrv_poll(PhoneSrv *s, int timeout_ms);   // waits that long at most, answers what arrived, drops the silent
int phonesrv_pads(const PhoneSrv *s);              // phones joined now
void phonesrv_close(PhoneSrv *s);                  // every controller goes
