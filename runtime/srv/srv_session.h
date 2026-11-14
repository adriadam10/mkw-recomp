// Game sessions (the protocol description, "Game sessions, rooms, room IDs, ICE" and "Matchmaking"): rooms made by
// a creation ticket and joined by JoinGameSession, sessions the matcher makes from matchmaking tickets, room IDs (short
// aliases), ICE server sets. Each session holds its Gamesync documents (srv_docs.h), served by srv_gamesync.c, and its
// race referee (srv_referee.h). Bounded: SESSIONS_MAX sessions, MEMBERS_MAX members each; past them a call ends with
// RESOURCE_EXHAUSTED.
#pragma once
#include <stdint.h>

#include "srv_auth.h"
#include "srv_docs.h"
#include "srv_referee.h"
#include "srv_svc.h"
#include "srv_value.h"

// Gamesync tokens: GS_TOKENS_PER per player and session (a game asks again after an hour), GS_TOKENS_MAX per session.
enum { SESSIONS_MAX = 256, MEMBERS_MAX = 32, GS_TOKENS_MAX = 128, GS_TOKENS_PER = 4, DEFERMENTS_MAX = 128,
       DEFERMENT_BYTES = 64 << 10, DEFERMENTS_BYTES = 1 << 20 };

typedef struct {
    char uid[24], tenant[64], build[65];
    int64_t ussid;    // the user session number (__us document's ussid and ucsid)
    int64_t upcsid;   // rooms: the station id fixed at the member's first allocation, 0 before
    SrvMap attrs;     // the ticket attributes of the member entry
    char credential[TOKEN_MAX];   // the member entry's credential, asked back by Gamesync IssueToken
    uint64_t seq;     // when they were matched into, created or joined it: a player's most recent session
    int streams;      // their Gamesync listen streams open on it
} SrvMember;

typedef struct {
    char token[TOKEN_MAX], uid[24];
    uint64_t hash;   // looked at before the constant-time comparison: a call does not compare every token in full
} SrvGsToken;

typedef struct {
    char uid[24], name[160];
    uint8_t *writes;   // the Write messages, each as field 1 (a WriteDocuments request without deferments)
    size_t len;
} SrvDeferment;

typedef struct {
    char id[24], tenant[64], config[64], batching[64], version[64], alias[8], creation[24];
    int room, closed, max_players, nmembers, listeners;
    SrvMember members[MEMBERS_MAX];   // in member order (the first is the host)
    int64_t next_ussid, next_upcsid;  // rooms: numbers handed out so far, never given twice
    SrvMap attrs;                     // the session attributes (GameSession field 11)
    int64_t created;                  // Unix seconds
    uint64_t changed_ms;              // last document or membership change (idle sessions are dropped)
    SrvDocs docs;
    SrvReferee referee;
    SrvGsToken tokens[GS_TOKENS_MAX];
    int ntokens;
    SrvDeferment *defs;   // the deferred writes of its players, in the order first set
    int ndefs, cdefs;
    size_t def_bytes;
} SrvSession;

SrvSession *srv_session_find(const char *id);
SrvMember *srv_member(SrvSession *s, const char *uid);
// The session a player most recently created, joined or was matched into, among those still listing them; NULL none.
SrvSession *srv_session_recent(const char *uid);
// The session whose member entry for uid carries this credential, NULL none.
SrvSession *srv_session_by_credential(const char *uid, const char *credential);
// The session and player a Gamesync token is bound to (uid: 24 bytes), NULL none.
SrvSession *srv_session_by_token(const char *token, char *uid);
void srv_session_add_token(SrvSession *s, const char *token, const char *uid);
// A member's station id for an upcsid allocation (the protocol description, "upcsid allocation"): matchmade, 20001 +
// their member index; room, fixed at their first allocation (20001, 20002... in allocation order). 0: not a member.
int64_t srv_session_station(SrvSession *s, const char *uid);
// A room member's last listen stream ended: they leave, their __us document goes (listeners get "removed").
void srv_session_leave(SrvSession *s, const char *uid);
// The matcher (each second), abandoned tickets (each 30 s), idle sessions and old tickets dropped, races closed late.
void srv_session_tick(uint64_t now_ms);

// CancelGameSessionCreationTicket has no effect: the unary catch-all's empty reply is all it needs.
extern const SrvMethod SRV_CREATE_ROOM, SRV_TRACK_ROOM, SRV_JOIN, SRV_GET_SESSION, SRV_CREATE_ALIAS, SRV_GET_ALIAS,
    SRV_ICE, SRV_CREATE_TICKET, SRV_TRACK_TICKET, SRV_CANCEL_TICKET;

// srv_gamesync.c: a session's documents changed (s->docs.changed): sent to the streams listening (added: a member
// joined, "added" rather than "modified"), then the participants queries waiting for everyone are checked again.
void srv_gs_changed(SrvSession *s, int added);
extern const SrvMethod SRV_GS_ISSUE, SRV_GS_REFRESH, SRV_GS_GET, SRV_GS_WRITE, SRV_GS_LISTEN;
