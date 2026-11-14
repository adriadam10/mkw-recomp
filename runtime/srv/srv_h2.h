// One player's connection to the online server's gRPC port: TLS (srv_tls.h), HTTP/2 (nghttp2) and gRPC calls on it.
// Run from the server's single poll loop: srv_conn_step does what the socket allows and never waits.
#pragma once
#include <stddef.h>
#include <time.h>

#include "srv_net.h"
#include "srv_tls.h"

typedef struct SrvConn SrvConn;

SrvConn *srv_conn_new(Sock s, const SrvTls *tls, const char *peer, const char *ip);
// After every poll, revents for this socket (0: a timer pass, for the services' timers). Returns the events to wait
// for next (POLLIN, POLLOUT), or -1 when the connection is over: it is then freed and its socket closed.
int srv_conn_step(SrvConn *c, int revents);
void srv_conn_free(SrvConn *c);
Sock srv_conn_sock(const SrvConn *c);
// 1 for a connection still without TLS after 10 s: the loop closes it (a socket held open costs a slot).
int srv_conn_stale(const SrvConn *c, time_t now);
// 1 when replies were queued on it outside its own step (one player's write sent to another's stream): the loop steps
// it again at once instead of after the next poll (up to 250 ms later).
int srv_conn_kicked(const SrvConn *c);
// What all connections' buffers hold now (within 4 MiB per connection, 128 MiB in all).
size_t srv_mem_in_use(void);
