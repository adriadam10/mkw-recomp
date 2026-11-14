// The online server's UDP answers, as pure functions of the datagram and the address it came from: the socket loop
// (srv_main.c) only receives and sends. Pia (the game's peer-to-peer library) asks them before a race to learn the
// address its peers will see.
#pragma once
#include <stddef.h>
#include <stdint.h>

// Where a datagram came from, as the server saw it. IPv6 is kept for STUN; Pia's NAT check is IPv4 only.
typedef struct {
    int v6;
    uint8_t addr[16];  // network order; IPv4 in the first 4 bytes
    uint16_t port;     // host order
} SrvPeer;

// STUN binding (RFC 5389, and RFC 3489 requests without the magic cookie): the reply to `req`, its length, or 0 when
// `req` is not a binding request (nothing is sent back). A 5389 reply carries XOR-MAPPED-ADDRESS and MAPPED-ADDRESS,
// a 3489 reply MAPPED-ADDRESS. There is no relay (TURN): race traffic goes straight between players.
size_t stun_reply(const uint8_t *req, size_t n, const SrvPeer *from, uint8_t *out, size_t cap);

// Pia's NAT check (UDP 10025, 10125, 33334): a 16-byte request {type 101, 102 or 103, a port, an IPv4 address, 0}, four
// big-endian 32-bit words, gets the same type back with the port and IPv4 address the server saw (16 bytes), or 0 for
// anything else (no reply). A sender on 127.x is told `self` (the server's own address the others reach it by, NULL
// or 0.0.0.0 when it has none): a game on the server's PC would otherwise give its peers 127.0.0.1.
size_t nat_reply(const uint8_t *req, size_t n, const SrvPeer *from, const uint8_t self[4], uint8_t *out, size_t cap);
