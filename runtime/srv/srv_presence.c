// Players online and their presence (srv_presence.h), as the protocol description's "Presence and friends" says.
// Online: a stream open, or a call less than 120 s ago before any stream, or less than 15 s since the last stream
// closed. KeepAlive: each presence change is stored (a field mask names the attributes that changed; the others stay)
// and acknowledged, and an acknowledgement goes out after 5 s of silence; heartbeats get no reply (replying to them
// starts a storm of messages). The friend list and the friends' presences go out whole at once, then again whenever
// they would differ from what was sent (checked twice a second).
#include "srv_presence.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "srv.h"
#include "srv_auth.h"
#include "srv_pb.h"
#include "srv_store.h"
#include "srv_svc.h"

enum { ATTRS_MAX = 64, FRIENDS_MAX = 200, RESEND_MS = 500, IDLE_ACK_MS = 5000 };

typedef struct {
    char key[64];
    uint8_t *val;   // field 2 of an attribute entry, encoded: the normalised MapValue (pb_mapvalue)
    size_t len;
} Attr;

typedef struct {
    char uid[24], tenant[64], build[65], version[64];
    uint64_t code;
    int streams, streamed;   // open now; any since the sign-in
    uint64_t last_call, last_close;
    Attr attrs[ATTRS_MAX];
    int nattrs;
} Player;

static Player **players;
static int nplayers, cplayers;

static Player *find_player(const char *uid) {
    for (int i = 0; i < nplayers; i++)
        if (!strcmp(players[i]->uid, uid)) return players[i];
    return NULL;
}

// The player of a uid, made when first seen (a token still valid after the server started again finds its account).
static Player *player(const char *uid) {
    Player *p = find_player(uid);
    if (p || !*uid) return p;
    if (nplayers == cplayers) {
        int cap = cplayers ? cplayers * 2 : 32;
        Player **n = realloc(players, (size_t)cap * sizeof *n);
        if (!n) return NULL;
        players = n;
        cplayers = cap;
    }
    p = calloc(1, sizeof *p);
    if (!p) return NULL;
    snprintf(p->uid, sizeof p->uid, "%s", uid);
    p->code = srv_uid_code(uid);
    SrvAccount *a = p->code ? srv_account(p->code) : NULL;
    if (a) snprintf(p->tenant, sizeof p->tenant, "%s", a->tenant);
    players[nplayers++] = p;
    return p;
}

static int online(const Player *p, uint64_t now) {
    if (p->streams > 0) return 1;
    if (!p->streamed) return p->last_call && now - p->last_call < 120000;
    uint64_t last = p->last_close > p->last_call ? p->last_close : p->last_call;
    return now - last < 15000;
}

int srv_on_call(SrvCall *c) {
    Player *p = player(srv_call_uid(c));
    if (!p) return ST_OK;
    p->last_call = srv_ms();
    SrvAccount *a = p->code ? srv_account(p->code) : NULL;
    return a && a->banned ? ST_PERMISSION_DENIED : ST_OK;
}

void srv_on_stream(SrvCall *c, int opened) {
    Player *p = find_player(srv_call_uid(c));
    if (!p) return;
    if (opened) {
        p->streams++;
        p->streamed = 1;
    } else {
        if (p->streams > 0) p->streams--;
        p->last_close = srv_ms();
    }
}

void srv_presence_signin(const char *uid, uint64_t code, const char *tenant, const char *build) {
    Player *p = player(uid);
    if (!p) return;
    p->code = code;
    snprintf(p->tenant, sizeof p->tenant, "%s", tenant);
    snprintf(p->build, sizeof p->build, "%s", build);
    p->last_call = srv_ms();
    p->streamed = p->streams > 0;
}

int srv_online(uint64_t code) {
    char uid[24];
    snprintf(uid, sizeof uid, "u-%016llx", (unsigned long long)code);
    return srv_uid_online(uid);
}

int srv_uid_online(const char *uid) {
    Player *p = find_player(uid);
    return p && online(p, srv_ms());
}

const char *srv_player_tenant(const char *uid) {
    Player *p = find_player(uid);
    return p ? p->tenant : "";
}

const char *srv_player_build(const char *uid) {
    Player *p = find_player(uid);
    return p ? p->build : "";
}

const char *srv_player_version(const char *uid) {
    Player *p = find_player(uid);
    return p ? p->version : "";
}

void srv_player_set_version(const char *uid, const char *version) {
    Player *p = *version ? player(uid) : NULL;
    if (p) snprintf(p->version, sizeof p->version, "%s", version);
}

// ---- attributes
static void attr_drop(Player *p, int i) {
    free(p->attrs[i].val);
    p->attrs[i] = p->attrs[--p->nattrs];
}

static void attr_set(Player *p, const uint8_t *key, size_t klen, const uint8_t *val, size_t vlen) {
    if (!klen || klen >= sizeof p->attrs[0].key) return;
    PbOut o = {0};
    if (!pb_mapvalue(&o, 2, val, vlen) || o.err) {
        pb_free(&o);
        return;
    }
    int i = 0;
    while (i < p->nattrs && (strlen(p->attrs[i].key) != klen || memcmp(p->attrs[i].key, key, klen))) i++;
    if (i == p->nattrs) {
        if (p->nattrs == ATTRS_MAX) {
            pb_free(&o);
            return;
        }
        memcpy(p->attrs[i].key, key, klen);
        p->attrs[i].key[klen] = 0;
        p->nattrs++;
    } else {
        free(p->attrs[i].val);
    }
    p->attrs[i].val = o.buf;   // taken over: not freed here
    p->attrs[i].len = o.len;
}

// The attribute entry named key in a Presence's repeated field 3, or 0.
static int entry_of(const uint8_t *presence, size_t plen, const char *key, size_t klen, PbField *val) {
    PbIn in = pb_in(presence, plen);
    PbField e, k;
    while (pb_next(&in, &e) > 0)
        if (e.num == 3 && e.wt == PB_LEN && pb_find(e.data, e.len, 1, &k) == 1 && k.len == klen &&
            !memcmp(k.data, key, klen) && pb_find(e.data, e.len, 2, val) == 1 && val->wt == PB_LEN)
            return 1;
    return 0;
}

// The in-game name (the PlayerName attribute, a string) becomes the account's name, cut on a whole UTF-8 character.
static void keep_name(Player *p) {
    SrvAccount *a = p->code ? srv_account(p->code) : NULL;
    for (int i = 0; a && i < p->nattrs; i++) {
        PbField v, s;
        if (strcmp(p->attrs[i].key, "PlayerName") || pb_find(p->attrs[i].val, p->attrs[i].len, 2, &v) != 1 ||
            pb_find(v.data, v.len, 7, &s) != 1 || s.wt != PB_LEN)
            continue;
        size_t n = s.len < sizeof a->name - 1 ? s.len : sizeof a->name - 1;
        while (n < s.len && n && (s.data[n] & 0xc0) == 0x80) n--;
        if (strlen(a->name) != n || memcmp(a->name, s.data, n)) {
            memcpy(a->name, s.data, n);
            a->name[n] = 0;
            srv_store_save_soon();
        }
    }
}

// PresenceChange {1: Presence {3: repeated {1: key, 2: MapValue}}, 2: FieldMask {1: repeated "attributes.<key>"}}
static void apply_change(Player *p, const uint8_t *change, size_t len) {
    PbField presence = {0}, mask = {0}, f;
    if (pb_find(change, len, 1, &presence) != 1 || presence.wt != PB_LEN) presence.len = 0;
    int masked = 0;
    if (pb_find(change, len, 2, &mask) == 1 && mask.wt == PB_LEN) {
        PbIn in = pb_in(mask.data, mask.len);
        while (pb_next(&in, &f) > 0) {
            if (f.num != 1 || f.wt != PB_LEN || f.len <= 11 || memcmp(f.data, "attributes.", 11)) continue;
            masked = 1;
            const char *key = (const char *)f.data + 11;
            size_t klen = f.len - 11;
            PbField v;
            if (presence.len && entry_of(presence.data, presence.len, key, klen, &v)) {
                attr_set(p, f.data + 11, klen, v.data, v.len);
            } else {
                for (int i = 0; i < p->nattrs; i++)
                    if (strlen(p->attrs[i].key) == klen && !memcmp(p->attrs[i].key, key, klen)) {
                        attr_drop(p, i);
                        break;
                    }
            }
        }
    }
    if (!masked) {   // the whole set, replaced
        while (p->nattrs) attr_drop(p, 0);
        PbIn in = pb_in(presence.data, presence.len);
        while (presence.len && pb_next(&in, &f) > 0) {
            PbField k, v;
            if (f.num == 3 && f.wt == PB_LEN && pb_find(f.data, f.len, 1, &k) == 1 && k.wt == PB_LEN &&
                pb_find(f.data, f.len, 2, &v) == 1 && v.wt == PB_LEN)
                attr_set(p, k.data, k.len, v.data, v.len);
        }
    }
    keep_name(p);
}

// ---- PresenceService/KeepAlive (bidi)
static const uint8_t ACK[] = {0x0a, 0x08, 0x0a, 0x02, 0x08, 0x1e, 0x12, 0x02, 0x08, 0x1e};   // Ack {30 s, 30 s}

static void keepalive_msg(SrvCall *c, const uint8_t *msg, size_t len) {
    PbField change;
    if (pb_find(msg, len, 1, &change) != 1 || change.wt != PB_LEN) return;   // a heartbeat: no reply
    Player *p = player(srv_call_uid(c));
    if (p) apply_change(p, change.data, change.len);
    srv_call_reply(c, ACK, sizeof ACK);
}

static void keepalive_tick(SrvCall *c, uint64_t now) {
    uint64_t **opened = (uint64_t **)srv_call_state(c);
    if (!*opened && (*opened = malloc(sizeof **opened))) **opened = now;   // the stream's start: the first silence
    uint64_t last = srv_call_last_sent(c);
    if (*opened && last < **opened) last = **opened;
    if (now - last >= IDLE_ACK_MS) srv_call_reply(c, ACK, sizeof ACK);
}

static void keepalive_end(SrvCall *c) { srv_call_end(c, ST_OK, NULL); }

static void free_state(SrvCall *c) {
    free(*srv_call_state(c));
    *srv_call_state(c) = NULL;
}

// ---- the friend list and the friends' presences (server streams)
typedef struct {
    uint8_t *last;   // what was sent last
    size_t len;
    uint64_t at;     // when it was last checked
    int sent;
} Sub;

static const char *tenant_of(SrvCall *c, const Player *me) {
    return me && *me->tenant ? me->tenant : srv_call_tenant(c);
}

// FriendUsers {1: repeated Friend {1: id (the code in hex), 2: FriendUser {2: "tenants/<t>/users/<uid>"}}}
static void build_friends(SrvCall *c, PbOut *o) {
    Player *me = player(srv_call_uid(c));
    if (!me || !me->code) return;   // no friend code: an empty message
    uint64_t f[FRIENDS_MAX];
    int n = srv_friends(me->code, f, FRIENDS_MAX);
    char s[160];
    for (int i = 0; i < n; i++) {
        size_t m = pb_begin(o, 1);
        snprintf(s, sizeof s, "%llx", (unsigned long long)f[i]);
        pb_str(o, 1, s);
        size_t u = pb_begin(o, 2);
        snprintf(s, sizeof s, "tenants/%s/users/u-%016llx", tenant_of(c, me), (unsigned long long)f[i]);
        pb_str(o, 2, s);
        pb_end(o, u);
        pb_end(o, m);
    }
}

// Presences {1: PresenceList {1: repeated Presence {1: name, 2: state, 3: attributes}, 2: "p"}}. A friend is online here
// only when online and last signed in to this game: each game reads its own attributes.
static void build_presences(SrvCall *c, PbOut *o) {
    Player *me = player(srv_call_uid(c));
    if (!me || !me->code) return;
    uint64_t f[FRIENDS_MAX], now = srv_ms();
    int n = srv_friends(me->code, f, FRIENDS_MAX);
    const char *tenant = tenant_of(c, me);
    char s[160];
    size_t list = pb_begin(o, 1);
    for (int i = 0; i < n; i++) {
        snprintf(s, sizeof s, "u-%016llx", (unsigned long long)f[i]);
        Player *fp = find_player(s);
        int on = fp && online(fp, now) && !strcmp(fp->tenant, tenant);
        size_t m = pb_begin(o, 1);
        snprintf(s, sizeof s, "tenants/%s/users/u-%016llx/presence", tenant, (unsigned long long)f[i]);
        pb_str(o, 1, s);
        pb_varint(o, 2, on ? 1 : 2);
        for (int k = 0; on && k < fp->nattrs; k++) {
            size_t e = pb_begin(o, 3);
            pb_str(o, 1, fp->attrs[k].key);
            pb_raw(o, fp->attrs[k].val, fp->attrs[k].len);
            pb_end(o, e);
        }
        pb_end(o, m);
    }
    pb_str(o, 2, "p");
    pb_end(o, list);
}

static void sub_send(SrvCall *c, uint64_t now, void (*build)(SrvCall *, PbOut *)) {
    Sub **sp = (Sub **)srv_call_state(c);
    if (!*sp && !(*sp = calloc(1, sizeof **sp))) return;
    Sub *s = *sp;
    if (s->sent && now - s->at < RESEND_MS) return;
    s->at = now;
    PbOut o = {0};
    build(c, &o);
    if (o.err) {
        pb_free(&o);
        return;
    }
    if (!s->sent || o.len != s->len || (o.len && memcmp(o.buf, s->last, o.len))) {
        srv_call_reply(c, o.buf, o.len);
        free(s->last);
        s->last = o.buf;   // taken over
        s->len = o.len;
        s->sent = 1;
        return;
    }
    pb_free(&o);
}

static void friends_tick(SrvCall *c, uint64_t now) { sub_send(c, now, build_friends); }
static void friends_first(SrvCall *c) { sub_send(c, srv_ms(), build_friends); }
static void presences_tick(SrvCall *c, uint64_t now) { sub_send(c, now, build_presences); }
static void presences_first(SrvCall *c) { sub_send(c, srv_ms(), build_presences); }

static void sub_close(SrvCall *c) {
    Sub *s = *srv_call_state(c);
    if (s) free(s->last);
    free(s);
    *srv_call_state(c) = NULL;
}

const SrvMethod SRV_KEEP_ALIVE = {"PresenceService", "KeepAlive", SRV_BIDI, 0,
                                  keepalive_msg, keepalive_end, keepalive_tick, free_state};
const SrvMethod SRV_FRIEND_USERS = {NULL, "SubscribeFriendUsers", SRV_SSTREAM, 0,
                                    NULL, friends_first, friends_tick, sub_close};
const SrvMethod SRV_PRESENCES = {NULL, "SubscribePresences", SRV_SSTREAM, 0,
                                 NULL, presences_first, presences_tick, sub_close};
