// The online server's services and the gRPC layer (srv_h2.c) between them and the network: a table of the methods the
// game calls (srv_methods.c), and what a service may do on a call. Everything runs on the server's single thread.
#pragma once
#include <stddef.h>
#include <stdint.h>

enum { SRV_UNARY, SRV_SSTREAM, SRV_BIDI };
// grpc-status codes the server uses (the protocol description, "Status codes")
enum {
    ST_OK = 0,
    ST_INVALID_ARGUMENT = 3,
    ST_NOT_FOUND = 5,
    ST_PERMISSION_DENIED = 7,
    ST_RESOURCE_EXHAUSTED = 8,
    ST_FAILED_PRECONDITION = 9,
    ST_UNIMPLEMENTED = 12,
    ST_INTERNAL = 13,
    ST_UNAVAILABLE = 14,
};

typedef struct Call SrvCall;

// A method the game calls. Handlers left NULL take the catch-all of their kind: unary, one empty reply; server stream,
// one empty message then held open (or ended at once with end_after_first); bidi, nothing until the player
// half-closes, then OK.
typedef struct {
    const char *service;   // the service name's last component ("Auth", "PresenceService"); NULL: any service
    const char *method;
    int kind, end_after_first;
    void (*on_message)(SrvCall *c, const uint8_t *msg, size_t len);   // each complete request message
    void (*on_half_close)(SrvCall *c);                                // the player sent all of its request
    void (*on_tick)(SrvCall *c, uint64_t now_ms);                     // open calls, several times a second
    void (*on_close)(SrvCall *c);                                     // the call is gone: free its state
} SrvMethod;

const SrvMethod *srv_method(const char *service, const char *method);   // NULL: not in the table (unary catch-all)

void srv_call_reply(SrvCall *c, const void *msg, size_t len);   // one reply message
void srv_call_end(SrvCall *c, int status, const char *message);
int srv_call_ended(const SrvCall *c);
// Its OK end goes unlogged (errors still are): Gamesync writes come several times a second per player, and
// the log is only rotated when the server starts.
void srv_call_quiet(SrvCall *c);
const char *srv_call_uid(const SrvCall *c);      // the caller from its bearer token, "" when anonymous
const char *srv_call_tenant(const SrvCall *c);   // the npln-tenant-id header, "" when absent
const char *srv_call_ip(const SrvCall *c);
const char *srv_call_auth(const SrvCall *c);     // the authorization header as sent (Gamesync reads its own tokens)
const char *srv_call_path(const SrvCall *c);
void **srv_call_state(SrvCall *c);               // the service's own data for this call (freed in on_close)
uint64_t srv_call_last_sent(const SrvCall *c);   // srv_ms() of the last reply message, 0 before any

// Hooks the services provide (srv_presence.c): every call, and every stream opening and closing, count for whether a
// player is online. srv_on_call returns a status to refuse the call with (a banned player), ST_OK otherwise.
int srv_on_call(SrvCall *c);
void srv_on_stream(SrvCall *c, int opened);
