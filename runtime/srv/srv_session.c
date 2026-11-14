// Game sessions, rooms, room IDs, ICE and matchmaking (srv_session.h), as the protocol description's sections 6 and 7
// say. Ids are short counters never reused in a run (gs-0001, mt-0001, gct-0001), as the reference's are.
#include "srv_session.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "srv.h"
#include "srv_pb.h"
#include "srv_presence.h"
#include "srv_store.h"

enum {
    TICKETS_MAX = 512,
    MATCH_MS = 1000,         // the matcher's pass
    READY_MS = 6000,         // a ticket is "ready" after searching this long
    PLACING_MS = 1000,       // PLACING, then SUCCEEDED at the next pass
    ABANDON_MS = 30000,      // tickets of players gone are cancelled this often
    IDLE_MS = 900000,        // a session nobody listens to and nobody changed for this long is dropped
    FINAL_KEEP_MS = 600000,  // a final ticket without a session is forgotten this long after it was made
    ALIAS_S = 86400,
    ICE_S = 600,
};
enum { T_SEARCHING = 1, T_PLACING, T_SUCCEEDED, T_TIMED_OUT, T_FAILED, T_CANCELLED, T_ACCEPTING, T_DECLINED };
#define GAMESYNC_HOST "gamesync.npln.nintendo.net"   // under npln.nintendo.net: the runtime sends it to this server

typedef struct {
    int used, state;
    char id[24], tenant[64], config[64], uid[24], batching[64], build[65], session[24];
    uint8_t *body;   // the request's Ticket without its fields 1 and 5, echoed in every message about it
    size_t body_len;
    SrvMap attrs;    // the last ticket member's attributes
    uint64_t created_ms, placed_ms;
} Ticket;

static SrvSession *sessions[SESSIONS_MAX];   // in creation order (backfill takes the oldest)
static int nsessions;
static Ticket tickets[TICKETS_MAX];
static unsigned next_gs, next_mt, next_gct, next_ice;
static uint64_t member_seq;

static int final_state(int st) { return st >= T_SUCCEEDED && st != T_ACCEPTING; }

// A string field as a C string, cut to cap; "" when absent.
static void str_of(const uint8_t *msg, size_t len, uint32_t num, char *out, size_t cap) {
    PbField f;
    out[0] = 0;
    if (!msg || pb_find(msg, len, num, &f) != 1 || f.wt != PB_LEN) return;
    size_t n = f.len < cap - 1 ? f.len : cap - 1;
    memcpy(out, f.data, n);
    out[n] = 0;
}

static const char *last_seg(const char *s) {
    const char *p = strrchr(s, '/');
    return p ? p + 1 : s;
}

// The game the call is for: npln-tenant-id, else the game the player signed in to.
static void tenant_of(SrvCall *c, char out[64]) {
    const char *t = srv_call_tenant(c);
    if (!*t || strlen(t) >= 64 || strchr(t, '/')) t = srv_player_tenant(srv_call_uid(c));
    snprintf(out, 64, "%s", t);
}

static void scalar(const SrvVal *v, char out[64]) {
    out[0] = 0;
    if (v && v->kind == V_STR) snprintf(out, 64, "%s", v->u.s.p);
    else if (v && v->kind == V_INT) snprintf(out, 64, "%lld", (long long)v->u.i);
}

// The game version attributes carry: batching_key, else rom_id.
static void version_of(const SrvMap *attrs, char out[64]) {
    scalar(map_get(attrs, "batching_key"), out);
    if (!*out) scalar(map_get(attrs, "rom_id"), out);
}

static void put_ts(PbOut *o, uint32_t num, int64_t s) {
    size_t m = pb_begin(o, num);
    pb_int64(o, 1, s);
    pb_end(o, m);
}

static void reply(SrvCall *c, PbOut *o) {
    if (o->err) srv_call_end(c, ST_INTERNAL, "reply too large");
    else srv_call_reply(c, o->buf, o->len), srv_call_end(c, ST_OK, NULL);
    pb_free(o);
}

// ---- sessions
SrvSession *srv_session_find(const char *id) {
    for (int i = 0; i < nsessions; i++)
        if (!strcmp(sessions[i]->id, id)) return sessions[i];
    return NULL;
}

SrvMember *srv_member(SrvSession *s, const char *uid) {
    for (int i = 0; *uid && i < s->nmembers; i++)
        if (!strcmp(s->members[i].uid, uid)) return &s->members[i];
    return NULL;
}

SrvSession *srv_session_recent(const char *uid) {
    SrvSession *best = NULL;
    uint64_t seq = 0;
    for (int i = 0; i < nsessions; i++) {
        const SrvMember *m = srv_member(sessions[i], uid);
        if (m && m->seq > seq) best = sessions[i], seq = m->seq;
    }
    return best;
}

SrvSession *srv_session_by_credential(const char *uid, const char *credential) {
    for (int i = 0; i < nsessions; i++) {
        const SrvMember *m = srv_member(sessions[i], uid);
        if (m && srv_secret_equal(m->credential, credential)) return sessions[i];
    }
    return NULL;
}

static uint64_t fnv1a(const char *s) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 0x100000001b3ull;
    return h;
}

SrvSession *srv_session_by_token(const char *token, char *uid) {
    uint64_t h = fnv1a(token);
    for (int i = 0; *token && i < nsessions; i++)
        for (int k = 0; k < sessions[i]->ntokens; k++)
            if (sessions[i]->tokens[k].hash == h && srv_secret_equal(sessions[i]->tokens[k].token, token)) {
                memcpy(uid, sessions[i]->tokens[k].uid, sizeof sessions[i]->tokens[k].uid);
                return sessions[i];
            }
    return NULL;
}

// The player's oldest token makes room for a new one past GS_TOKENS_PER (so nobody can push the others out), the
// session's oldest past GS_TOKENS_MAX (players who left).
void srv_session_add_token(SrvSession *s, const char *token, const char *uid) {
    int mine = 0, first = -1;
    for (int k = 0; k < s->ntokens; k++)
        if (!strcmp(s->tokens[k].uid, uid) && mine++ == 0) first = k;
    int drop = mine >= GS_TOKENS_PER ? first : s->ntokens == GS_TOKENS_MAX ? 0 : -1;
    if (drop >= 0) {
        memmove(&s->tokens[drop], &s->tokens[drop + 1], (size_t)(s->ntokens - drop - 1) * sizeof s->tokens[0]);
        s->ntokens--;
    }
    SrvGsToken *t = &s->tokens[s->ntokens++];
    snprintf(t->token, TOKEN_MAX, "%s", token);
    snprintf(t->uid, sizeof t->uid, "%s", uid);
    t->hash = fnv1a(token);
}

int64_t srv_session_station(SrvSession *s, const char *uid) {
    SrvMember *m = srv_member(s, uid);
    if (!m) return 0;
    if (!s->room) return 20001 + (m - s->members);   // members never leave a matchmade session: indices hold
    if (!m->upcsid) m->upcsid = 20001 + s->next_upcsid++;
    return m->upcsid;
}

// GameSession (field 12 stays empty: filling it made the game refuse the session, error 0321-0384). num 0: the
// message itself.
static void put_session(PbOut *o, uint32_t num, const SrvSession *s) {
    char name[160];
    size_t m = num ? pb_begin(o, num) : 0;
    snprintf(name, sizeof name, "tenants/%s/gameSessions/%s", s->tenant, s->id);
    pb_str(o, 1, name);
    pb_varint(o, 2, 2);   // ACTIVE
    pb_str(o, 8, GAMESYNC_HOST);
    pb_varint(o, 9, 443);
    put_ts(o, 10, s->created);
    map_encode(o, 11, &s->attrs);
    if (num) pb_end(o, m);
}

// MemberEntry {1: TicketMember {1: user, 2: attributes}, 2: user session, 3: credential}, one per member in member
// order. A caller who is not a member gets no credentials: with one, anybody could take a member's Gamesync token.
static void put_members(PbOut *o, uint32_t num, SrvSession *s, const char *caller) {
    int with = srv_member(s, caller) != NULL;
    char name[192];
    for (int i = 0; i < s->nmembers; i++) {
        const SrvMember *m = &s->members[i];
        size_t e = pb_begin(o, num), t = pb_begin(o, 1);
        snprintf(name, sizeof name, "tenants/%s/users/%s", *m->tenant ? m->tenant : s->tenant, m->uid);
        pb_str(o, 1, name);
        map_encode(o, 2, &m->attrs);
        pb_end(o, t);
        snprintf(name, sizeof name, "tenants/%s/gameSessions/%s/userSessions/us-%s", s->tenant, s->id, m->uid + 2);
        pb_str(o, 2, name);
        pb_str(o, 3, with ? m->credential : "");
        pb_end(o, e);
    }
}

// A new session and its fixed documents (the protocol description, "Seeded documents"), in a batch the caller ends
// once its members are in (session_ready). NULL past SESSIONS_MAX. Takes attrs.
static SrvSession *session_new(const char *tenant, const char *config, int room, int max_players, SrvMap *attrs) {
    SrvSession *s = nsessions < SESSIONS_MAX ? calloc(1, sizeof *s) : NULL;
    if (!s) {
        map_free(attrs);
        srv_log("no room for another game session (%d)", nsessions);
        return NULL;
    }
    snprintf(s->id, sizeof s->id, "gs-%04u", ++next_gs);
    snprintf(s->tenant, sizeof s->tenant, "%s", tenant);
    snprintf(s->config, sizeof s->config, "%s", config);
    s->room = room;
    s->max_players = max_players;
    s->attrs = *attrs;
    *attrs = (SrvMap){0};
    s->created = (int64_t)time(NULL);
    s->changed_ms = srv_ms();
    docs_init(&s->docs, s->created);
    sessions[nsessions++] = s;
    uint8_t rs[16];
    if (!srv_random(rs, sizeof rs)) memset(rs, 0, sizeof rs);
    SrvMap f = {0}, m = {0};
    map_set(&f, "gsid", val_str(s->id));
    map_set(&f, "addr", val_str(""));
    map_set(&f, "p", val_int(0));
    map_set(&f, "mcn", val_str(config));
    map_set(&f, "maxu", val_int(max_players));
    map_set(&f, "rs", val_bytes(rs, sizeof rs));
    map_set(&m, "cp", val_bool(1));
    map_set(&m, "ip", val_bool(1));
    map_set(&m, "ebf", val_bool(0));
    docs_begin(&s->docs);
    docs_put(&s->docs, "__gs/f", &f);
    docs_put(&s->docs, "__gs/m", &m);
    return s;
}

static void session_ready(SrvSession *s) {
    docs_end(&s->docs, s->created);
    docs_changes_clear(&s->docs);   // nobody listens yet
}

// A member at the end, its __us document put in the session's batch (begun if it was not). Takes attrs.
static SrvMember *member_add(SrvSession *s, const char *uid, SrvMap *attrs, int64_t ussid) {
    if (s->nmembers == MEMBERS_MAX) {
        map_free(attrs);
        return NULL;
    }
    SrvMember *m = &s->members[s->nmembers];
    memset(m, 0, sizeof *m);
    if (!srv_auth_token(m->credential, TOKEN_MAX, uid, "c")) {
        map_free(attrs);
        return NULL;
    }
    s->nmembers++;
    snprintf(m->uid, sizeof m->uid, "%s", uid);
    snprintf(m->tenant, sizeof m->tenant, "%s", srv_player_tenant(uid));
    snprintf(m->build, sizeof m->build, "%s", srv_player_build(uid));
    m->ussid = ussid;
    m->attrs = *attrs;
    *attrs = (SrvMap){0};
    m->seq = ++member_seq;
    char path[48];
    snprintf(path, sizeof path, "__us/us-%s", uid + 2);
    SrvMap f = {0};
    map_set(&f, "uid", val_str(uid));
    map_set(&f, "st", val_int(2));
    map_set(&f, "att", (SrvVal){.kind = V_MAP});
    map_set(&f, "ltc", val_str(""));
    map_set(&f, "tn", val_str(s->tenant));
    map_set(&f, "ussid", val_int(ussid));
    map_set(&f, "ucsid", val_int(ussid));
    docs_begin(&s->docs);
    docs_put(&s->docs, path, &f);
    s->changed_ms = srv_ms();
    return m;
}

void srv_session_leave(SrvSession *s, const char *uid) {
    SrvMember *m = srv_member(s, uid);
    if (!m || !s->room) return;   // members of a matchmade session stay (their numbers must not collide)
    char path[48];
    snprintf(path, sizeof path, "__us/us-%s", uid + 2);
    int i = (int)(m - s->members);
    map_free(&m->attrs);
    memmove(&s->members[i], &s->members[i + 1], (size_t)(s->nmembers - i - 1) * sizeof *m);
    s->nmembers--;
    docs_begin(&s->docs);
    docs_remove(&s->docs, path);
    docs_end(&s->docs, (int64_t)time(NULL));
    s->changed_ms = srv_ms();
    srv_log("a player left room %s (%d left)", s->id, s->nmembers);
    srv_gs_changed(s, 0);
}

// The session a request names ("/gameSessions/<id>" anywhere in its bytes, as the reference looks for it).
static SrvSession *session_named(const uint8_t *msg, size_t len) {
    static const char key[] = "/gameSessions/";
    const size_t k = sizeof key - 1;
    for (size_t i = 0; msg && i + k <= len; i++) {
        if (memcmp(msg + i, key, k)) continue;
        char id[24];
        size_t n = 0;
        for (size_t j = i + k; j < len && n < sizeof id - 1 && (isalnum(msg[j]) || msg[j] == '-' || msg[j] == '_'); j++)
            id[n++] = (char)msg[j];
        id[n] = 0;
        SrvSession *s = n ? srv_session_find(id) : NULL;
        if (s) return s;
    }
    return NULL;
}

static SrvSession *session_of(SrvCall *c, const uint8_t *msg, size_t len) {
    SrvSession *s = session_named(msg, len);
    return s ? s : srv_session_recent(srv_call_uid(c));
}

// ---- rooms: CreateGameSessionCreationTicket {1: parent, 2: CreationTicket {2: config, 3: TicketMember (the host),
// 6: GameSessionTemplate {11: attributes}}}
static void create_room(SrvCall *c, const uint8_t *msg, size_t len) {
    const char *uid = srv_call_uid(c);
    PbField ct, cfg = {0}, host, tmpl, f;
    SrvMap host_attrs = {0}, attrs = {0};
    if (!*uid) {
        srv_call_end(c, ST_FAILED_PRECONDITION, "sign in first");
        return;
    }
    if (pb_find(msg, len, 2, &ct) != 1 || ct.wt != PB_LEN || pb_find(ct.data, ct.len, 2, &cfg) != 1 ||
        cfg.wt != PB_LEN) {
        srv_call_end(c, ST_INVALID_ARGUMENT, "no room config");
        return;
    }
    if (pb_find(ct.data, ct.len, 3, &host) == 1 && host.wt == PB_LEN && pb_find(host.data, host.len, 2, &f) == 1 &&
        f.wt == PB_LEN)
        map_decode(&host_attrs, f.data, f.len);
    if (pb_find(ct.data, ct.len, 6, &tmpl) == 1 && tmpl.wt == PB_LEN && pb_find(tmpl.data, tmpl.len, 11, &f) == 1 &&
        f.wt == PB_LEN)
        map_decode(&attrs, f.data, f.len);
    char tenant[64], config[160], version[64];
    tenant_of(c, tenant);
    snprintf(config, sizeof config, "%.*s", (int)cfg.len, (const char *)cfg.data);
    version_of(&host_attrs, version);   // the host's attributes, the room's, else what the player last showed
    if (!*version) version_of(&attrs, version);
    if (!*version) snprintf(version, sizeof version, "%s", srv_player_version(uid));
    SrvSession *s = session_new(tenant, last_seg(config), 1, srv_setting_int("room_size", 24, 2, MEMBERS_MAX), &attrs);
    if (!s) {
        map_free(&host_attrs);
        srv_call_end(c, ST_RESOURCE_EXHAUSTED, "too many game sessions");
        return;
    }
    snprintf(s->version, sizeof s->version, "%s", version);
    snprintf(s->creation, sizeof s->creation, "gct-%04u", ++next_gct);
    member_add(s, uid, &host_attrs, ++s->next_ussid);
    session_ready(s);
    srv_player_set_version(uid, version);
    PbOut o = {0};
    char name[160];
    snprintf(name, sizeof name, "tenants/%s/gameSessionCreationTickets/%s", tenant, s->creation);
    pb_str(&o, 1, name);
    pb_bytes(&o, 2, cfg.data, cfg.len);   // exactly as sent
    pb_varint(&o, 4, 1);
    srv_log("room %s made (%s)", s->id, s->config);
    reply(c, &o);
}

// TrackGameSessionCreationTicket {1: name}: one message, then OK (the game reads to the end). Only the room's members
// follow it: the reply carries their credentials.
static void track_room(SrvCall *c, const uint8_t *msg, size_t len) {
    if (srv_call_last_sent(c) || srv_call_ended(c)) return;
    char name[160], cfg[160];
    str_of(msg, len, 1, name, sizeof name);
    SrvSession *s = NULL;
    for (int i = 0; i < nsessions && !s; i++)
        if (sessions[i]->room && !strcmp(sessions[i]->creation, last_seg(name))) s = sessions[i];
    if (s && !srv_member(s, srv_call_uid(c))) s = NULL;
    PbOut o = {0};
    pb_str(&o, 1, name);
    if (s) {
        snprintf(cfg, sizeof cfg, "tenants/%s/matchmakingConfigs/%s", s->tenant, s->config);
        pb_str(&o, 2, cfg);
        pb_varint(&o, 4, 2);   // created
        put_members(&o, 5, s, srv_call_uid(c));
        put_session(&o, 6, s);
    } else {
        pb_varint(&o, 4, 5);   // failed
    }
    reply(c, &o);
}

static void track_room_end(SrvCall *c) { track_room(c, NULL, 0); }

// JoinGameSession {1: game session name, 3: TicketMember}: a room takes the player in (same version and build, room
// not full); a matchmade session's members do not change. Reply {1: MemberEntry..., 2: GameSession}.
static void join(SrvCall *c, const uint8_t *msg, size_t len) {
    const char *uid = srv_call_uid(c);
    SrvSession *s = session_of(c, msg, len);
    PbOut o = {0};
    if (!s) {
        reply(c, &o);
        return;
    }
    SrvMember *m = srv_member(s, uid);
    if (s->room && *uid && m) {
        m->seq = ++member_seq;   // joining a room one is in already is accepted
    } else if (s->room && *uid) {
        PbField tm, f;
        SrvMap attrs = {0};
        if (pb_find(msg, len, 3, &tm) == 1 && tm.wt == PB_LEN && pb_find(tm.data, tm.len, 2, &f) == 1 &&
            f.wt == PB_LEN)
            map_decode(&attrs, f.data, f.len);
        char v[64];
        version_of(&attrs, v);
        if (!*v) snprintf(v, sizeof v, "%s", srv_player_version(uid));
        const char *build = srv_player_build(uid);
        int other_build = 0;
        for (int i = 0; *build && i < s->nmembers; i++)
            other_build |= *s->members[i].build && strcmp(s->members[i].build, build);
        const char *why = *s->version && *v && strcmp(s->version, v) ? "another game version"
                          : other_build                              ? "another game build"
                                                                     : NULL;
        if (why || s->closed || s->nmembers >= s->max_players || s->nmembers >= MEMBERS_MAX) {
            map_free(&attrs);
            srv_call_end(c, why ? ST_FAILED_PRECONDITION : ST_RESOURCE_EXHAUSTED, why ? why : "room full or closed");
            return;
        }
        srv_player_set_version(uid, v);
        if (!member_add(s, uid, &attrs, ++s->next_ussid)) {
            srv_call_end(c, ST_RESOURCE_EXHAUSTED, "room full");
            return;
        }
        docs_end(&s->docs, (int64_t)time(NULL));
        srv_log("a player joined room %s (%d in it)", s->id, s->nmembers);
        srv_gs_changed(s, 1);   // their __us document, "added", to everyone listening
    }
    put_members(&o, 1, s, uid);
    put_session(&o, 2, s);
    reply(c, &o);
}

static void get_session(SrvCall *c, const uint8_t *msg, size_t len) {
    SrvSession *s = session_of(c, msg, len);
    PbOut o = {0};
    if (s) put_session(&o, 0, s);
    reply(c, &o);
}

// ---- room IDs: 6 random digits per session, unique among live sessions
static void alias_reply(SrvCall *c, const SrvSession *s) {
    PbOut o = {0};
    char name[160];
    snprintf(name, sizeof name, "tenants/%s/GameSessionShortAliases/%s", s->tenant, s->alias);   // capital G
    pb_str(&o, 1, name);
    snprintf(name, sizeof name, "tenants/%s/gameSessions/%s", s->tenant, s->id);
    pb_str(&o, 2, name);
    put_ts(&o, 3, (int64_t)time(NULL) + ALIAS_S);
    reply(c, &o);
}

static SrvSession *by_alias(const char *a) {
    for (int i = 0; i < nsessions; i++)
        if (!sessions[i]->closed && !strcmp(sessions[i]->alias, a)) return sessions[i];
    return NULL;
}

// CreateGameSessionShortAlias {1: "tenants/current", 2: {2: game session name}}
static void create_alias(SrvCall *c, const uint8_t *msg, size_t len) {
    SrvSession *s = session_of(c, msg, len);
    if (!s || s->closed) {
        srv_call_end(c, ST_NOT_FOUND, "no game session");
        return;
    }
    for (int tries = 0; !s->alias[0] && tries < 1000; tries++) {
        uint32_t r;
        char a[8];
        if (!srv_random(&r, sizeof r)) break;
        snprintf(a, sizeof a, "%06u", (unsigned)(r % 1000000));
        if (!by_alias(a)) memcpy(s->alias, a, sizeof a);
    }
    if (!s->alias[0]) {
        srv_call_end(c, ST_RESOURCE_EXHAUSTED, "no room ID free");
        return;
    }
    alias_reply(c, s);
}

// GetGameSessionShortAlias {1: "tenants/current/GameSessionShortAliases/<typed>"}: spaces dropped (shown "123 456").
static void get_alias(SrvCall *c, const uint8_t *msg, size_t len) {
    char name[160], typed[16];
    size_t n = 0;
    str_of(msg, len, 1, name, sizeof name);
    for (const char *p = last_seg(name); *p && n < sizeof typed - 1; p++)
        if (!isspace((unsigned char)*p)) typed[n++] = *p;
    typed[n] = 0;
    const SrvSession *s = n ? by_alias(typed) : NULL;
    if (!s) srv_call_end(c, ST_NOT_FOUND, "no room with that ID");
    else alias_reply(c, s);
}

// ---- AllocateIceServerSet: a STUN server and a relay (nothing answers TURN: races go straight between players), both
// under names the runtime sends to this server.
static void ice(SrvCall *c, const uint8_t *msg, size_t len) {
    (void)msg, (void)len;
    const char *uid = srv_call_uid(c);
    char tenant[64], name[160], cred[TOKEN_MAX];
    tenant_of(c, tenant);
    if (!srv_auth_token(cred, sizeof cred, *uid ? uid : "u-0000000000000000", "t")) cred[0] = 0;
    PbOut o = {0};
    snprintf(name, sizeof name, "tenants/%s/iceServerSets/ice-%04u", tenant, ++next_ice);
    pb_str(&o, 1, name);
    size_t m = pb_begin(&o, 2);
    pb_str(&o, 1, "stun.npln.nintendo.net");
    pb_varint(&o, 2, 3478);
    pb_varint(&o, 3, 1);   // UDP
    pb_end(&o, m);
    m = pb_begin(&o, 3);
    pb_str(&o, 1, "relay.npln.nintendo.net");
    pb_varint(&o, 2, 3479);
    pb_varint(&o, 3, 1);
    pb_str(&o, 4, uid);
    pb_str(&o, 5, cred);
    pb_end(&o, m);
    m = pb_begin(&o, 4);   // Duration
    pb_int64(&o, 1, ICE_S);
    pb_end(&o, m);
    put_ts(&o, 5, (int64_t)time(NULL) + ICE_S);
    reply(c, &o);
}

// ---- matchmaking tickets
static Ticket *ticket_find(const char *id) {
    for (int i = 0; i < TICKETS_MAX; i++)
        if (tickets[i].used && !strcmp(tickets[i].id, id)) return &tickets[i];
    return NULL;
}

static void ticket_free(Ticket *t) {
    free(t->body);
    map_free(&t->attrs);
    memset(t, 0, sizeof *t);
}

// Ticket {1: name, the request's fields as sent, 5: state}; once SUCCEEDED also {6: MemberEntry..., 7: GameSession}.
static void put_ticket(PbOut *o, const Ticket *t) {
    char name[160];
    snprintf(name, sizeof name, "tenants/%s/matchmakingTickets/%s", t->tenant, t->id);
    pb_str(o, 1, name);
    pb_raw(o, t->body, t->body_len);
    pb_varint(o, 5, (uint64_t)t->state);
    SrvSession *s = t->state == T_SUCCEEDED ? srv_session_find(t->session) : NULL;
    if (s) {
        put_members(o, 6, s, t->uid);
        put_session(o, 7, s);
    }
}

// CreateMatchmakingTicket {1: parent, 2: Ticket {2: config, 4: repeated TicketMember {1: user, 2: attributes}}}
static void create_ticket(SrvCall *c, const uint8_t *msg, size_t len) {
    const char *uid = srv_call_uid(c);
    PbField tk, f, a;
    PbOut o = {0};
    if (!*uid || pb_find(msg, len, 2, &tk) != 1 || tk.wt != PB_LEN) {   // no player: nothing to match
        reply(c, &o);
        return;
    }
    Ticket *t = NULL;
    for (int i = 0; i < TICKETS_MAX && !t; i++)
        if (!tickets[i].used) t = &tickets[i];
    if (!t) {
        srv_call_end(c, ST_RESOURCE_EXHAUSTED, "too many tickets");
        return;
    }
    PbOut body = {0};
    SrvMap attrs = {0};
    char config[160] = "";
    PbIn in = pb_in(tk.data, tk.len);
    const uint8_t *at = in.p;
    int r;
    while ((r = pb_next(&in, &f)) > 0) {
        // the client's own fields 1 and 5 (never seen) would be written twice: left out of the echo
        if (f.num != 1 && f.num != 5) pb_raw(&body, at, (size_t)(in.p - at));
        if (f.num == 2 && f.wt == PB_LEN) snprintf(config, sizeof config, "%.*s", (int)f.len, (const char *)f.data);
        if (f.num == 4 && f.wt == PB_LEN) {   // the last member's attributes are the ticket's
            map_free(&attrs);
            if (pb_find(f.data, f.len, 2, &a) == 1 && a.wt == PB_LEN) map_decode(&attrs, a.data, a.len);
        }
        at = in.p;
    }
    if (r < 0 || body.err) {
        pb_free(&body);
        map_free(&attrs);
        srv_call_end(c, ST_INVALID_ARGUMENT, "malformed ticket");
        return;
    }
    t->used = 1;
    t->state = T_SEARCHING;
    snprintf(t->id, sizeof t->id, "mt-%04u", ++next_mt);
    tenant_of(c, t->tenant);
    snprintf(t->config, sizeof t->config, "%s", last_seg(config));
    snprintf(t->uid, sizeof t->uid, "%s", uid);
    scalar(map_get(&attrs, "batching_key"), t->batching);
    snprintf(t->build, sizeof t->build, "%s", srv_player_build(uid));
    t->body = body.buf;   // taken over
    t->body_len = body.len;
    t->attrs = attrs;
    t->created_ms = srv_ms();
    char v[64];
    version_of(&attrs, v);
    srv_player_set_version(uid, v);
    put_ticket(&o, t);
    srv_log("matchmaking ticket %s (%s)", t->id, t->config);
    reply(c, &o);
}

typedef struct {
    char id[24];
    int known, state;
} Track;

static void track_send(SrvCall *c, Track *k, const Ticket *t) {
    PbOut o = {0};
    put_ticket(&o, t);
    if (o.err) {
        pb_free(&o);
        srv_call_end(c, ST_INTERNAL, "reply too large");
        return;
    }
    srv_call_reply(c, o.buf, o.len);
    pb_free(&o);
    k->state = t->state;
    if (final_state(t->state)) srv_call_end(c, ST_OK, NULL);   // the game stops reading after a final state
}

// TrackMatchmakingTicket {1: ticket name, 2: user}: the ticket at once, then each change of state; ends after a final
// one. Unknown (or another player's: the success carries credentials): {1: name as sent, 5: SEARCHING}, then silence.
static void track_ticket(SrvCall *c, const uint8_t *msg, size_t len) {
    Track **kp = (Track **)srv_call_state(c);
    if (*kp) return;
    if (!(*kp = calloc(1, sizeof **kp))) {
        srv_call_end(c, ST_INTERNAL, "no memory");
        return;
    }
    char name[160];
    str_of(msg, len, 1, name, sizeof name);
    snprintf((*kp)->id, sizeof (*kp)->id, "%s", last_seg(name));
    Ticket *t = ticket_find((*kp)->id);
    if (t && strcmp(t->uid, srv_call_uid(c))) t = NULL;
    if (t) {
        (*kp)->known = 1;
        track_send(c, *kp, t);
        return;
    }
    PbField raw;
    PbOut o = {0};
    if (pb_find(msg, len, 1, &raw) == 1 && raw.wt == PB_LEN) pb_bytes(&o, 1, raw.data, raw.len);
    pb_varint(&o, 5, T_SEARCHING);
    srv_call_reply(c, o.buf, o.len);
    pb_free(&o);
}

static void track_tick(SrvCall *c, uint64_t now) {
    (void)now;
    Track *k = *srv_call_state(c);
    if (!k || !k->known) return;
    const Ticket *t = ticket_find(k->id);
    if (!t) srv_call_end(c, ST_OK, NULL);   // forgotten with its session
    else if (t->state != k->state) track_send(c, k, t);
}

static void track_held(SrvCall *c) { (void)c; }   // the request is in: the stream stays for the states to come

static void free_state(SrvCall *c) {
    free(*srv_call_state(c));
    *srv_call_state(c) = NULL;
}

// CancelMatchmakingTicket {1: name}: a ticket not final becomes CANCELLED (its owner's alone). Reply: empty.
static void cancel_ticket(SrvCall *c, const uint8_t *msg, size_t len) {
    char name[160];
    str_of(msg, len, 1, name, sizeof name);
    Ticket *t = ticket_find(last_seg(name));
    if (t && !strcmp(t->uid, srv_call_uid(c)) && !final_state(t->state)) t->state = T_CANCELLED;
    PbOut o = {0};
    reply(c, &o);
}

// ---- the matcher
// The number the config's name ends with (Normal24: 24), 12 without one; at most MEMBERS_MAX.
static int session_size(const char *config) {
    size_t n = strlen(config), d = n;
    while (d > 0 && isdigit((unsigned char)config[d - 1])) d--;
    if (d == n) return 12;
    long v = 0;
    for (size_t i = d; i < n && v < 1000; i++) v = v * 10 + (config[i] - '0');
    return v < 2 ? 2 : v > MEMBERS_MAX ? MEMBERS_MAX : (int)v;
}

static int knockout(const char *config) { return !strcmp(config, "Survival24") || !strcmp(config, "ClutchSurvival24"); }

static int same_queue(const Ticket *a, const Ticket *b) {
    return !strcmp(a->tenant, b->tenant) && !strcmp(a->config, b->config) && !strcmp(a->batching, b->batching);
}

// The tickets in a state, oldest first.
static int by_age(Ticket **out, int state) {
    int n = 0;
    for (int i = 0; i < TICKETS_MAX; i++)
        if (tickets[i].used && tickets[i].state == state) {
            int k = n++;
            while (k > 0 && out[k - 1]->created_ms > tickets[i].created_ms) out[k] = out[k - 1], k--;
            out[k] = &tickets[i];
        }
    return n;
}

static void placed(Ticket *t, const SrvSession *s, uint64_t now) {
    t->state = T_PLACING;
    t->placed_ms = now;
    snprintf(t->session, sizeof t->session, "%s", s->id);
}

static int truthy(const SrvVal *v) { return v && (v->kind == V_BOOL || v->kind == V_INT) && v->u.i; }

// A lobby in progress the ticket may join (the protocol description, "Backfill"): matchmade, same queue, the host
// allows it (__gs/m: ebf true, cp not false), somebody really there (a participation document: a stale ebf stays when
// everyone leaves), room under min(maximum, bfmax), builds compatible.
static SrvSession *backfill_target(const Ticket *t) {
    for (int i = 0; i < nsessions; i++) {
        SrvSession *s = sessions[i];
        if (s->room || s->closed || !s->nmembers || strcmp(s->tenant, t->tenant) || strcmp(s->config, t->config) ||
            strcmp(s->batching, t->batching) || srv_member(s, t->uid))
            continue;
        const SrvDoc *m = docs_find(&s->docs, "__gs/m");
        const SrvVal *cp = m ? map_get(&m->fields, "cp") : NULL;
        if (!m || !truthy(map_get(&m->fields, "ebf")) || (cp && (cp->kind == V_BOOL || cp->kind == V_INT) && !cp->u.i))
            continue;
        int64_t limit = s->max_players, bf = map_int(&m->fields, "bfmax", 0);
        if (bf > 0 && bf < limit) limit = bf;
        int there = 0;
        for (int k = 0; k < s->docs.n && !there; k++)
            there = !strncmp(s->docs.doc[k].path, "__pgn/", 6) && strstr(s->docs.doc[k].path, "/__pus/");
        if (s->nmembers >= limit || s->nmembers >= MEMBERS_MAX || !there) continue;
        int ok = 1;
        for (int k = 0; ok && *t->build && k < s->nmembers; k++)
            ok = !*s->members[k].build || !strcmp(s->members[k].build, t->build);
        if (ok) return s;
    }
    return NULL;
}

// One queue (oldest first): a session once two tickets are ready, or one has waited alone long enough; Knockout
// lobbies take every ticket of the queue (they never take players later).
static void match_queue(Ticket **q, int n, uint64_t now) {
    if (!n) return;
    const char *config = q[0]->config;
    int size = session_size(config), ko = knockout(config);
    int solo = srv_setting_int("solo_after", 15, 0, 3600), kos = srv_setting_int("knockout_solo_after", 90, 0, 3600);
    uint64_t lone = (uint64_t)(solo && ko && kos > solo ? kos : solo) * 1000;
    int ready = 0;
    while (ready < n && now - q[ready]->created_ms >= READY_MS) ready++;   // the oldest: a prefix of the queue
    int alone = lone && now - q[0]->created_ms >= lone;
    if (ready < 2 && !alone) return;
    Ticket *pick[MEMBERS_MAX];
    int np = 0;
    for (int i = 0; i < (alone || ko ? n : ready) && np < size; i++) {
        int dup = 0;   // a player with two tickets is in a session once
        for (int k = 0; k < np && !dup; k++) dup = !strcmp(pick[k]->uid, q[i]->uid);
        if (!dup) pick[np++] = q[i];
    }
    if (np < 2 && !alone) return;
    SrvMap attrs;
    if (!map_copy(&attrs, &pick[0]->attrs)) return;
    map_del(&attrs, "npln_user_id");
    SrvSession *s = session_new(pick[0]->tenant, config, 0, size, &attrs);
    if (!s) return;
    snprintf(s->batching, sizeof s->batching, "%s", pick[0]->batching);
    for (int k = 0; k < np; k++) {   // the first is the host
        member_add(s, pick[k]->uid, &pick[k]->attrs, k + 1);
        placed(pick[k], s, now);
    }
    session_ready(s);
    srv_log("matched %d player(s) into %s (%s)", np, s->id, config);
}

static void matcher(uint64_t now) {
    static Ticket *list[TICKETS_MAX], *group[TICKETS_MAX], *queue[TICKETS_MAX];
    static uint8_t grouped[TICKETS_MAX];
    for (int i = 0; i < TICKETS_MAX; i++)   // 1. promote
        if (tickets[i].used && tickets[i].state == T_PLACING && now - tickets[i].placed_ms >= PLACING_MS)
            tickets[i].state = T_SUCCEEDED;
    int n = by_age(list, T_SEARCHING);
    for (int i = 0; i < n; i++) {   // 2. backfill, oldest ticket first
        SrvSession *s = backfill_target(list[i]);
        if (!s || !member_add(s, list[i]->uid, &list[i]->attrs, s->nmembers + 1)) continue;
        docs_end(&s->docs, (int64_t)time(NULL));
        placed(list[i], s, now);
        srv_log("a player joined %s in progress (%d in it)", s->id, s->nmembers);
        srv_gs_changed(s, 1);
    }
    n = by_age(list, T_SEARCHING);   // 3. new sessions, per (game, config, batching key), then per build
    memset(grouped, 0, sizeof grouped);
    for (int i = 0; i < n; i++) {
        if (grouped[i] || list[i]->state != T_SEARCHING) continue;
        int ng = 0, nb = 0;
        const char *builds[TICKETS_MAX];
        for (int j = i; j < n; j++)
            if (!grouped[j] && same_queue(list[i], list[j])) grouped[j] = 1, group[ng++] = list[j];
        for (int k = 0; k < ng; k++) {
            int known = !*group[k]->build;
            for (int b = 0; b < nb && !known; b++) known = !strcmp(builds[b], group[k]->build);
            if (!known) builds[nb++] = group[k]->build;
        }
        if (!nb) match_queue(group, ng, now);
        for (int b = 0; b < nb; b++) {   // a queue per build, each with the tickets of unknown build too
            int nq = 0;
            for (int k = 0; k < ng; k++)
                if (group[k]->state == T_SEARCHING && (!*group[k]->build || !strcmp(group[k]->build, builds[b])))
                    queue[nq++] = group[k];
            match_queue(queue, nq, now);
        }
    }
}

static void session_drop(int i) {
    SrvSession *s = sessions[i];
    for (int k = 0; k < TICKETS_MAX; k++)
        if (tickets[k].used && !strcmp(tickets[k].session, s->id)) ticket_free(&tickets[k]);
    for (int k = 0; k < s->nmembers; k++) map_free(&s->members[k].attrs);
    for (int k = 0; k < s->ndefs; k++) free(s->defs[k].writes);
    free(s->defs);
    map_free(&s->attrs);
    docs_free(&s->docs);
    srv_log("game session %s dropped (idle)", s->id);
    free(s);
    memmove(&sessions[i], &sessions[i + 1], (size_t)(nsessions - i - 1) * sizeof *sessions);
    nsessions--;
}

void srv_session_tick(uint64_t now) {
    static uint64_t last, last_abandon;
    if (last && now - last < MATCH_MS) return;
    last = now;
    matcher(now);
    if (now - last_abandon >= ABANDON_MS) {   // tickets whose player is gone (an unclean exit never cancels its own)
        last_abandon = now;
        for (int i = 0; i < TICKETS_MAX; i++)
            if (tickets[i].used && tickets[i].state == T_SEARCHING && !srv_uid_online(tickets[i].uid))
                tickets[i].state = T_CANCELLED;
    }
    for (int i = nsessions - 1; i >= 0; i--)
        if (!sessions[i]->listeners && now - sessions[i]->changed_ms >= IDLE_MS) session_drop(i);
    for (int i = 0; i < TICKETS_MAX; i++)
        if (tickets[i].used && final_state(tickets[i].state) && now - tickets[i].created_ms >= FINAL_KEEP_MS &&
            !srv_session_find(tickets[i].session))
            ticket_free(&tickets[i]);
    for (int i = 0; i < nsessions; i++) {   // races whose players all wait: nobody writes to close them
        referee_tick(&sessions[i]->referee, &sessions[i]->docs, now, (int64_t)time(NULL));
        if (sessions[i]->docs.nchanged) srv_gs_changed(sessions[i], 0);
    }
}

const SrvMethod SRV_CREATE_ROOM = {"GameSessionService", "CreateGameSessionCreationTicket", SRV_UNARY, 0,
                                   create_room, NULL, NULL, NULL};
const SrvMethod SRV_TRACK_ROOM = {NULL, "TrackGameSessionCreationTicket", SRV_SSTREAM, 0,
                                  track_room, track_room_end, NULL, NULL};
const SrvMethod SRV_JOIN = {NULL, "JoinGameSession", SRV_UNARY, 0, join, NULL, NULL, NULL};
const SrvMethod SRV_GET_SESSION = {NULL, "GetGameSession", SRV_UNARY, 0, get_session, NULL, NULL, NULL};
const SrvMethod SRV_CREATE_ALIAS = {NULL, "CreateGameSessionShortAlias", SRV_UNARY, 0, create_alias, NULL, NULL, NULL};
const SrvMethod SRV_GET_ALIAS = {NULL, "GetGameSessionShortAlias", SRV_UNARY, 0, get_alias, NULL, NULL, NULL};
const SrvMethod SRV_ICE = {"GameSessionService", "AllocateIceServerSet", SRV_UNARY, 0, ice, NULL, NULL, NULL};
const SrvMethod SRV_CREATE_TICKET = {NULL, "CreateMatchmakingTicket", SRV_UNARY, 0, create_ticket, NULL, NULL, NULL};
const SrvMethod SRV_TRACK_TICKET = {NULL, "TrackMatchmakingTicket", SRV_SSTREAM, 0,
                                    track_ticket, track_held, track_tick, free_state};
const SrvMethod SRV_CANCEL_TICKET = {NULL, "CancelMatchmakingTicket", SRV_UNARY, 0, cancel_ticket, NULL, NULL, NULL};
