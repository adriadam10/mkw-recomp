// The friends API next to the game's port (gRPC port + 1, 8444): HTTPS/1.1 for our runtime's Online page, not for the
// game. GET /v1/ca.der (the CA, no check), GET /v1/friends, POST /v1/friends/<request|accept|decline|cancel|remove>.
#pragma once
#include <time.h>

#include "srv_net.h"
#include "srv_tls.h"

typedef struct SrvApi SrvApi;

SrvApi *srv_api_new(Sock s, const SrvTls *tls, const char *peer, const char *ip);
// As srv_conn_step: the events to wait for, or -1 when over (then freed, socket closed).
int srv_api_step(SrvApi *a, int revents);
void srv_api_free(SrvApi *a);
Sock srv_api_sock(const SrvApi *a);
int srv_api_stale(const SrvApi *a, time_t now);   // no TLS after 10 s, or 15 s without a request
