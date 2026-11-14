// Gamesync (the protocol description, "Gamesync"): the per-session document store the game's peer-to-peer layer (Pia)
// reads the session from. IssueToken / RefreshToken bind a token to (game session, player); GetDocument; WriteDocuments
// applies a request's writes as one batch (srv_docs.c), keeps its deferred writes, lets the referee answer
// (srv_referee.c) and sends what changed; KeepUserSession is the listen stream: targets (documents, or a collection
// queried by prefix), their documents, "current" once all are sent (held for a participants query until every member is
// in: Pia elects its host among the participants it knows then), and at its end the player's deferred writes and, in a
// room, their leaving.
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "srv.h"
#include "srv_pb.h"
#include "srv_session.h"

// A stream's targets: TARGETS_MAX, holding at most LISTEN_BYTES of paths together. A request's writes: WRITES_MAX (the
// game sends a handful; each looks its path up among the session's documents).
enum { TARGETS_MAX = 32, LISTEN_BYTES = 16 << 10, WRITES_MAX = 1024, GS_TOKEN_S = 3600 };
enum { DC_ADDED = 1, DC_MODIFIED = 2, DC_REMOVED = 3 };                  // DocumentChange kinds
enum { TC_ADDED = 1, TC_CURRENT = 2, TC_REMOVED = 3, TC_ERROR = 4 };    // TargetChange states

typedef struct {
    char id[24];   // the last segment of the target's name, sent back as it came
    int query, held;
    char **paths;  // a documents target's paths
    int npaths;
    char *coll;    // a query target's collection
} Target;

typedef struct Listener {
    struct Listener *next;
    SrvCall *call;
    char session[24], uid[24];
    int bound;     // its token names a session: it gets that session's documents
    Target t[TARGETS_MAX];
    int nt;
    size_t bytes;  // the targets' paths
} Listener;

static Listener *listeners;

static char *dup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

// The session and player of the call's Gamesync token ("authorization: bearer <token>", any case).
static SrvSession *session_of(SrvCall *c, char uid[24]) {
    static const char prefix[] = "bearer ";
    const char *a = srv_call_auth(c);
    char tok[TOKEN_MAX];
    uid[0] = 0;
    while (*a == ' ') a++;
    for (int i = 0; prefix[i]; i++)
        if (tolower((unsigned char)a[i]) != prefix[i]) return NULL;
    a += 7;
    while (*a == ' ') a++;
    size_t n = strcspn(a, " \t\r\n");
    if (!n || n >= sizeof tok) return NULL;
    memcpy(tok, a, n);
    tok[n] = 0;
    return srv_session_by_token(tok, uid);
}

static void reply(SrvCall *c, PbOut *o) {
    if (o->err) srv_call_end(c, ST_INTERNAL, "reply too large");
    else srv_call_reply(c, o->buf, o->len), srv_call_end(c, ST_OK, NULL);
    pb_free(o);
}

// ---- IssueToken / RefreshToken {1: "userSessions/us-<hex16>", 2: credential}
static void issue(SrvCall *c, const uint8_t *msg, size_t len) {
    PbField us = {0}, cr;
    char uid[24] = "", cred[TOKEN_MAX] = "", token[TOKEN_MAX];
    if (pb_find(msg, len, 1, &us) != 1 || us.wt != PB_LEN) us.len = 0;
    // the player is the user session's 16 hex digits: us-<hex16> is u-<hex16>'s
    if (us.len == 32 && !memcmp(us.data, "userSessions/us-", 16)) {
        int hex = 1;
        for (int i = 16; i < 32; i++) hex &= isdigit(us.data[i]) || (us.data[i] >= 'a' && us.data[i] <= 'f');
        if (hex) snprintf(uid, sizeof uid, "u-%.16s", (const char *)us.data + 16);
    }
    if (pb_find(msg, len, 2, &cr) == 1 && cr.wt == PB_LEN && cr.len) {
        if (cr.len >= sizeof cred) {
            srv_call_end(c, ST_PERMISSION_DENIED, "not this player's credential");
            return;
        }
        memcpy(cred, cr.data, cr.len);
        cred[cr.len] = 0;
    }
    // Bound to the session whose member entry carried this credential (checked: without it anybody could listen as a
    // member); without one, the player's most recent session. None: a token bound to nothing (writes then fail).
    SrvSession *s = NULL;
    if (*uid && *cred && !(s = srv_session_by_credential(uid, cred))) {
        srv_call_end(c, ST_PERMISSION_DENIED, "not this player's credential");
        return;
    }
    if (*uid && !*cred) s = srv_session_recent(uid);
    if (!srv_auth_token(token, sizeof token, *uid ? uid : "u-0000000000000000", "g")) {
        srv_call_end(c, ST_INTERNAL, "could not sign a token");
        return;
    }
    if (s) srv_session_add_token(s, token, uid);
    PbOut o = {0};
    size_t m = pb_begin(&o, 1);   // GamesyncToken
    pb_bytes(&o, 1, us.data, us.len);   // the user session as sent: anything else aborts the game
    pb_str(&o, 2, token);
    pb_str(&o, 3, token);
    size_t d = pb_begin(&o, 4);
    pb_int64(&o, 1, GS_TOKEN_S);
    pb_end(&o, d);
    pb_end(&o, m);
    reply(c, &o);
}

// ---- the listen streams
static void send_target(Listener *L, const char *id, int state) {
    PbOut o = {0};
    size_t m = pb_begin(&o, 3);   // ListenResponse {3: TargetChange {1: id, 2: state}}
    pb_str(&o, 1, id);
    pb_varint(&o, 2, (uint64_t)state);
    pb_end(&o, m);
    if (!o.err) srv_call_reply(L->call, o.buf, o.len);
    pb_free(&o);
}

// The Document as field 3: whole, or only its name once it is gone.
static void doc_field(PbOut *o, const SrvSession *s, const char *path) {
    const SrvDoc *d = docs_find(&s->docs, path);
    if (d) {
        docs_encode(o, 3, &s->docs, d);
        return;
    }
    size_t m = pb_begin(o, 3), n = pb_begin(o, 1);
    pb_raw(o, "docs/", 5);
    pb_raw(o, path, strlen(path));
    pb_end(o, n);
    pb_end(o, m);
}

// ListenResponse {2: DocumentChange {1: target id, 2: kind, 3: Document}}
static void send_doc(Listener *L, const char *id, int kind, const PbOut *doc) {
    PbOut o = {0};
    size_t m = pb_begin(&o, 2);
    pb_str(&o, 1, id);
    pb_varint(&o, 2, (uint64_t)kind);
    pb_raw(&o, doc->buf, doc->len);
    pb_end(&o, m);
    if (!o.err && !doc->err) srv_call_reply(L->call, o.buf, o.len);
    pb_free(&o);
}

// A target covers its listed paths, or (a query) every path under its collection, at any depth: the game queries
// direct children (__pgn/<group>/__pus, @@RefereeResult); matching by prefix is a superset that works.
static int covers(const Target *t, const char *path) {
    if (t->query) {
        size_t n = strlen(t->coll);
        return n && !strncmp(path, t->coll, n) && path[n] == '/';
    }
    for (int i = 0; i < t->npaths; i++)
        if (!strcmp(t->paths[i], path)) return 1;
    return 0;
}

// Every member has its participation document in a participants query's collection (one ending /__pus); other
// targets are always complete. Without the hold, the game that joins first sees only itself and hosts too.
static int complete(SrvSession *s, const Target *t) {
    size_t n = t->query ? strlen(t->coll) : 0;
    if (n < 6 || strcmp(t->coll + n - 6, "/__pus")) return 1;
    char path[DOC_PATH_MAX + 32];
    for (int i = 0; i < s->nmembers; i++) {
        snprintf(path, sizeof path, "%s/us-%s", t->coll, s->members[i].uid + 2);
        if (!docs_find(&s->docs, path)) return 0;
    }
    return 1;
}

static int listening(const Listener *L, const SrvSession *s) {
    return L->bound && !strcmp(L->session, s->id) && !srv_call_ended(L->call);
}

void srv_gs_changed(SrvSession *s, int added) {
    SrvDocs *d = &s->docs;
    if (d->nchanged) s->changed_ms = srv_ms();
    for (int i = 0; i < d->nchanged; i++) {   // each changed path once, to every stream with a target covering it
        const char *path = d->changed[i];
        int kind = !docs_find(d, path) ? DC_REMOVED : added ? DC_ADDED : DC_MODIFIED;
        PbOut doc = {0};
        doc_field(&doc, s, path);
        for (Listener *L = listeners; L; L = L->next)
            for (int k = 0; listening(L, s) && k < L->nt; k++)
                if (covers(&L->t[k], path)) send_doc(L, L->t[k].id, kind, &doc);
        pb_free(&doc);
    }
    docs_changes_clear(d);
    for (Listener *L = listeners; L; L = L->next)   // then the participants queries that were waiting
        for (int k = 0; listening(L, s) && k < L->nt; k++)
            if (L->t[k].held && complete(s, &L->t[k])) {
                L->t[k].held = 0;
                send_target(L, L->t[k].id, TC_CURRENT);
            }
}

static size_t target_bytes(const Target *t) {
    size_t n = t->coll ? strlen(t->coll) + 1 : 0;
    for (int i = 0; i < t->npaths; i++) n += strlen(t->paths[i]) + 1;
    return n;
}

static void target_free(Target *t) {
    for (int i = 0; i < t->npaths; i++) free(t->paths[i]);
    free(t->paths);
    free(t->coll);
    memset(t, 0, sizeof *t);
}

static void target_drop(Listener *L, int k) {
    L->bytes -= target_bytes(&L->t[k]);
    target_free(&L->t[k]);
    memmove(&L->t[k], &L->t[k + 1], (size_t)(L->nt - k - 1) * sizeof L->t[0]);
    L->nt--;
}

static int target_of(const Listener *L, const char *id) {
    for (int k = 0; k < L->nt; k++)
        if (!strcmp(L->t[k].id, id)) return k;
    return -1;
}

// A target's id: the last segment of its name.
static void target_id(const uint8_t *msg, size_t len, char id[24]) {
    PbField n;
    id[0] = 0;
    if (pb_find(msg, len, 1, &n) != 1 || n.wt != PB_LEN) return;
    size_t i = n.len;
    while (i > 0 && n.data[i - 1] != '/') i--;
    snprintf(id, 24, "%.*s", (int)(n.len - i), (const char *)n.data + i);
}

static void add_path(Listener *L, Target *t, const uint8_t *p, size_t n) {
    char path[DOC_PATH_MAX];
    char **grown;
    if (!docs_path(p, n, path) || L->bytes + strlen(path) + 1 > LISTEN_BYTES ||
        !(grown = realloc(t->paths, (size_t)(t->npaths + 1) * sizeof *grown)))
        return;   // past the budget the path is left out
    t->paths = grown;
    if ((t->paths[t->npaths] = dup(path))) {
        L->bytes += strlen(path) + 1;
        t->npaths++;
    }
}

// A spec that is a message of field-1 strings, not a bare string (the reference also takes a bare one: a path
// starting "docs/" never parses as a message, its first byte being a group end).
static int is_list(const PbField *f) {
    PbIn in = pb_in(f->data, f->len);
    PbField x;
    int r;
    while ((r = pb_next(&in, &x)) > 0)
        if (x.num != 1 || x.wt != PB_LEN) return 0;
    return r == 0;
}

// AddTarget {1: Target {1: name, 2: DocumentsSpec {1: repeated "docs/<path>"}, 3: QuerySpec {1: "docs/<collection>"}}}
static void add_target(Listener *L, SrvSession *s, const uint8_t *msg, size_t len) {
    PbField tf, spec, q;
    char id[24];
    if (pb_find(msg, len, 1, &tf) != 1 || tf.wt != PB_LEN) return;
    target_id(tf.data, tf.len, id);
    int old = target_of(L, id);
    if (old >= 0) target_drop(L, old);
    if (L->nt == TARGETS_MAX) {
        send_target(L, id, TC_ERROR);
        return;
    }
    Target *t = &L->t[L->nt++];
    memset(t, 0, sizeof *t);
    memcpy(t->id, id, sizeof id);
    if (pb_find(tf.data, tf.len, 3, &spec) == 1 && spec.wt == PB_LEN) {
        char path[DOC_PATH_MAX] = "";
        t->query = 1;
        if (is_list(&spec) && pb_find(spec.data, spec.len, 1, &q) == 1) docs_path(q.data, q.len, path);
        else docs_path(spec.data, spec.len, path);
        if (L->bytes + strlen(path) + 1 > LISTEN_BYTES || !(t->coll = dup(path))) {
            L->nt--;
            send_target(L, id, TC_ERROR);
            return;
        }
        L->bytes += strlen(path) + 1;
    } else if (pb_find(tf.data, tf.len, 2, &spec) == 1 && spec.wt == PB_LEN) {
        PbIn in = pb_in(spec.data, spec.len);
        if (!is_list(&spec)) add_path(L, t, spec.data, spec.len);
        else
            while (pb_next(&in, &q) > 0) add_path(L, t, q.data, q.len);
    }
    send_target(L, id, TC_ADDED);
    if (s) {   // what the target covers now: listed paths in their order (gone: "removed"), a query's documents
        PbOut doc = {0};
        for (int i = 0; i < t->npaths; i++) {
            doc_field(&doc, s, t->paths[i]);
            send_doc(L, id, docs_find(&s->docs, t->paths[i]) ? DC_ADDED : DC_REMOVED, &doc);
            pb_free(&doc);
        }
        for (int i = 0; t->query && i < s->docs.n; i++)
            if (covers(t, s->docs.doc[i].path)) {
                docs_encode(&doc, 3, &s->docs, &s->docs.doc[i]);
                send_doc(L, id, DC_ADDED, &doc);
                pb_free(&doc);
            }
        t->held = !complete(s, t);
    }
    if (!t->held) send_target(L, id, TC_CURRENT);
}

// ListenRequest {3: repeated AddTarget, 4: repeated RemoveTarget {1: name}}. The stream listens to the session of its
// token from its first request on.
static void listen_msg(SrvCall *c, const uint8_t *msg, size_t len) {
    Listener **lp = (Listener **)srv_call_state(c), *L = *lp;
    if (!L) {
        if (!(L = calloc(1, sizeof *L))) {
            srv_call_end(c, ST_INTERNAL, "no memory");
            return;
        }
        L->call = c;
        L->next = listeners;
        listeners = L;
        *lp = L;
        SrvSession *s = session_of(c, L->uid);
        if (s) {   // no session: target acknowledgements, no documents
            L->bound = 1;
            snprintf(L->session, sizeof L->session, "%s", s->id);
            s->listeners++;
            SrvMember *m = srv_member(s, L->uid);
            if (m) m->streams++;
        }
    }
    SrvSession *s = L->bound ? srv_session_find(L->session) : NULL;
    PbIn in = pb_in(msg, len);
    PbField f;
    while (pb_next(&in, &f) > 0) {
        if (f.wt != PB_LEN) continue;
        if (f.num == 3) {
            add_target(L, s, f.data, f.len);
        } else if (f.num == 4) {
            char id[24];
            target_id(f.data, f.len, id);
            int k = target_of(L, id);
            if (k >= 0) target_drop(L, k);
            send_target(L, id, TC_REMOVED);
        }
    }
}

// A player leaving half-closes and waits: the stream ends at once (after what is pending), or some flows give up.
static void listen_half(SrvCall *c) { srv_call_end(c, ST_OK, NULL); }

// ---- writes
typedef struct {
    SrvSession *s;
    const char *uid;
} Writer;

static int64_t station(void *ctx) {
    const Writer *w = ctx;
    return srv_session_station(w->s, w->uid);
}

// The writes of a WriteRequest (its field 1s) from uid as one batch, then the referee; their results go in res. The
// caller sends the changes (srv_gs_changed).
static int run_writes(SrvSession *s, const char *uid, const uint8_t *msg, size_t len, PbOut *res) {
    Writer w = {s, uid};
    int64_t now = (int64_t)time(NULL);
    SrvWriteCtx ctx = {now, station, &w};
    int from = s->docs.nchanged, st = ST_OK, r = 0, n = 0;
    PbIn in = pb_in(msg, len);
    PbField f;
    docs_begin(&s->docs);
    while (st == ST_OK && (r = pb_next(&in, &f)) > 0)
        if (f.num == 1 && f.wt == PB_LEN)
            st = ++n > WRITES_MAX ? ST_INVALID_ARGUMENT : docs_write(&s->docs, f.data, f.len, &ctx, res);
    if (st == ST_OK && r < 0) st = ST_INVALID_ARGUMENT;
    if (st != ST_OK) {
        docs_rollback(&s->docs);
        return st;
    }
    if ((st = docs_end(&s->docs, now)) != ST_OK) return st;
    referee_batch(&s->referee, &s->docs, from, srv_ms(), now);
    return ST_OK;
}

static int def_find(const SrvSession *s, const char *uid, const char *name) {
    for (int i = 0; i < s->ndefs; i++)
        if (!strcmp(s->defs[i].uid, uid) && (!name || !strcmp(s->defs[i].name, name))) return i;
    return -1;
}

static void def_drop(SrvSession *s, int i, int keep_writes) {
    if (!keep_writes) free(s->defs[i].writes);
    s->def_bytes -= s->defs[i].len;
    memmove(&s->defs[i], &s->defs[i + 1], (size_t)(s->ndefs - i - 1) * sizeof *s->defs);
    s->ndefs--;
}

// A named list of writes kept for when the player's listen stream ends; a name set again keeps its place.
static void def_set(SrvSession *s, const char *uid, const char *name, PbOut *w) {
    int i = def_find(s, uid, name);
    size_t had = i >= 0 ? s->defs[i].len : 0;
    if (w->err || w->len > DEFERMENT_BYTES || s->def_bytes - had + w->len > DEFERMENTS_BYTES ||
        (i < 0 && s->ndefs == DEFERMENTS_MAX)) {
        srv_log("deferred writes refused in %s (too many)", s->id);
        return;
    }
    if (i < 0) {
        if (s->ndefs == s->cdefs) {
            int cap = s->cdefs ? s->cdefs * 2 : 8;
            SrvDeferment *n = realloc(s->defs, (size_t)cap * sizeof *n);
            if (!n) return;
            s->defs = n;
            s->cdefs = cap;
        }
        i = s->ndefs++;
        memset(&s->defs[i], 0, sizeof s->defs[i]);
        snprintf(s->defs[i].uid, sizeof s->defs[i].uid, "%s", uid);
        snprintf(s->defs[i].name, sizeof s->defs[i].name, "%s", name);
    }
    free(s->defs[i].writes);
    s->defs[i].writes = w->buf;   // taken over
    s->defs[i].len = w->len;
    s->def_bytes = s->def_bytes - had + w->len;
    *w = (PbOut){0};
}

// WriteRequest field 2: DefermentChange {1: SetDeferments {1: repeated Deferment {1: name, 2: repeated Write}},
// 2: DeleteDeferment {1: name}}
static void deferments(SrvSession *s, const char *uid, const uint8_t *msg, size_t len) {
    PbIn in = pb_in(msg, len);
    PbField ch, op, dfm, f, n;
    char name[160];
    while (pb_next(&in, &ch) > 0) {
        if (ch.num != 2 || ch.wt != PB_LEN) continue;
        PbIn ci = pb_in(ch.data, ch.len);
        while (pb_next(&ci, &op) > 0) {
            if (op.wt != PB_LEN) continue;
            if (op.num == 2) {
                if (pb_find(op.data, op.len, 1, &n) == 1 && n.wt == PB_LEN && n.len < sizeof name) {
                    snprintf(name, sizeof name, "%.*s", (int)n.len, (const char *)n.data);
                    int i = def_find(s, uid, name);
                    if (i >= 0) def_drop(s, i, 0);
                }
                continue;
            }
            if (op.num != 1) continue;
            PbIn di = pb_in(op.data, op.len);
            while (pb_next(&di, &dfm) > 0) {
                if (dfm.num != 1 || dfm.wt != PB_LEN || pb_find(dfm.data, dfm.len, 1, &n) != 1 || n.wt != PB_LEN ||
                    n.len >= sizeof name)
                    continue;
                snprintf(name, sizeof name, "%.*s", (int)n.len, (const char *)n.data);
                PbOut w = {0};   // its writes as a WriteRequest's field 1s, run as one when the time comes
                PbIn wi = pb_in(dfm.data, dfm.len);
                while (pb_next(&wi, &f) > 0)
                    if (f.num == 2 && f.wt == PB_LEN) pb_bytes(&w, 1, f.data, f.len);
                def_set(s, uid, name, &w);
                pb_free(&w);
            }
        }
    }
}

// The player's listen stream ended: their deferred writes, in the order first set, each like a WriteDocuments of
// theirs (same changes sent, same referee), then forgotten. The game deletes its participation record this way: the
// others learn that someone left only from that.
static void run_deferments(SrvSession *s, const char *uid) {
    int i;
    while ((i = def_find(s, uid, NULL)) >= 0) {
        SrvDeferment d = s->defs[i];
        def_drop(s, i, 1);
        PbOut res = {0};
        if (run_writes(s, uid, d.writes, d.len, &res) == ST_OK) srv_gs_changed(s, 0);
        pb_free(&res);
        free(d.writes);
    }
}

static void listen_close(SrvCall *c) {
    Listener **lp = (Listener **)srv_call_state(c), *L = *lp;
    if (!L) return;
    *lp = NULL;
    for (Listener **p = &listeners; *p; p = &(*p)->next)   // first: nothing more goes to it
        if (*p == L) {
            *p = L->next;
            break;
        }
    SrvSession *s = L->bound ? srv_session_find(L->session) : NULL;
    if (s) {
        if (s->listeners > 0) s->listeners--;
        SrvMember *m = srv_member(s, L->uid);
        if (m && m->streams > 0) m->streams--;
        s->changed_ms = srv_ms();   // the idle clock starts again when a listener goes
        run_deferments(s, L->uid);
        m = srv_member(s, L->uid);
        if (m && !m->streams) srv_session_leave(s, L->uid);   // rooms only: matchmade members stay
    }
    for (int k = 0; k < L->nt; k++) target_free(&L->t[k]);
    free(L);
}

// ---- GetDocument {1: "docs/<path>"} and WriteDocuments {1: repeated Write, 2: repeated DefermentChange}
static void get_doc(SrvCall *c, const uint8_t *msg, size_t len) {
    char uid[24], path[DOC_PATH_MAX];
    SrvSession *s = session_of(c, uid);
    PbField f;
    const SrvDoc *d = NULL;
    srv_call_quiet(c);
    if (s && pb_find(msg, len, 1, &f) == 1 && f.wt == PB_LEN && docs_path(f.data, f.len, path))
        d = docs_find(&s->docs, path);
    if (!d) {
        srv_call_end(c, ST_NOT_FOUND, s ? "no such document" : "no game session for this token");
        return;
    }
    PbOut o = {0};
    docs_encode(&o, 0, &s->docs, d);
    reply(c, &o);
}

static void write_docs(SrvCall *c, const uint8_t *msg, size_t len) {
    char uid[24];
    SrvSession *s = session_of(c, uid);
    srv_call_quiet(c);
    if (!s) {
        srv_call_end(c, ST_FAILED_PRECONDITION, "no game session for this token");
        return;
    }
    PbOut res = {0};   // WriteReply {1: repeated WriteResult, 2: commit time}
    int st = run_writes(s, uid, msg, len, &res);
    if (st != ST_OK) {
        pb_free(&res);
        srv_call_end(c, st, st == ST_RESOURCE_EXHAUSTED ? "game session full" : "malformed write");
        return;
    }
    deferments(s, uid, msg, len);
    srv_gs_changed(s, 0);
    size_t m = pb_begin(&res, 2);
    pb_int64(&res, 1, (int64_t)time(NULL));
    pb_end(&res, m);
    reply(c, &res);
}

const SrvMethod SRV_GS_ISSUE = {"Gamesync", "IssueToken", SRV_UNARY, 0, issue, NULL, NULL, NULL};
const SrvMethod SRV_GS_REFRESH = {"Gamesync", "RefreshToken", SRV_UNARY, 0, issue, NULL, NULL, NULL};
const SrvMethod SRV_GS_GET = {"Gamesync", "GetDocument", SRV_UNARY, 0, get_doc, NULL, NULL, NULL};
const SrvMethod SRV_GS_WRITE = {"Gamesync", "WriteDocuments", SRV_UNARY, 0, write_docs, NULL, NULL, NULL};
const SrvMethod SRV_GS_LISTEN = {NULL, "KeepUserSession", SRV_BIDI, 0, listen_msg, listen_half, NULL, listen_close};
