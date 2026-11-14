// The methods the game calls that are not plain unary catch-alls (srv_svc.h): the services, and the stream kinds of the
// others (the wire does not say whether a method streams; the protocol description lists them).
#include <string.h>

#include "srv_session.h"
#include "srv_svc.h"

extern const SrvMethod SRV_SIGN_IN, SRV_KEEP_ALIVE, SRV_FRIEND_USERS, SRV_PRESENCES;

static const SrvMethod CATCH_ALL[] = {
    {NULL, "SubscribeMaintenanceSchedules", SRV_SSTREAM, 0, NULL, NULL, NULL, NULL},
    {NULL, "BulkGetDocuments", SRV_SSTREAM, 1, NULL, NULL, NULL, NULL},
};

// Gamesync's GetDocument names its service: Ugcstore has a GetDocument of its own (events, not served here).
static const SrvMethod *const SERVICES[] = {
    &SRV_SIGN_IN,       &SRV_KEEP_ALIVE,     &SRV_FRIEND_USERS, &SRV_PRESENCES,    &SRV_CREATE_ROOM, &SRV_TRACK_ROOM,
    &SRV_JOIN,          &SRV_GET_SESSION,    &SRV_CREATE_ALIAS, &SRV_GET_ALIAS,    &SRV_ICE,         &SRV_CREATE_TICKET,
    &SRV_TRACK_TICKET,  &SRV_CANCEL_TICKET,  &SRV_GS_ISSUE,     &SRV_GS_REFRESH,   &SRV_GS_GET,      &SRV_GS_WRITE,
    &SRV_GS_LISTEN,
};

static int is(const SrvMethod *m, const char *service, const char *method) {
    return !strcmp(m->method, method) && (!m->service || !strcmp(m->service, service));
}

const SrvMethod *srv_method(const char *service, const char *method) {
    for (size_t i = 0; i < sizeof SERVICES / sizeof *SERVICES; i++)
        if (is(SERVICES[i], service, method)) return SERVICES[i];
    for (size_t i = 0; i < sizeof CATCH_ALL / sizeof *CATCH_ALL; i++)
        if (is(&CATCH_ALL[i], service, method)) return &CATCH_ALL[i];
    return NULL;
}
