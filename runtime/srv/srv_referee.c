// MKW's race referee (srv_referee.h).
#include "srv_referee.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "srv.h"

#define RESULT "@@RefereeResult/"

// "<a>/<b>/<c>/<d>" with a and c as given: b and d copied out (d may not hold a '/'). 0 when it is another path.
static int split4(const char *path, const char *a, const char *c, char *b, size_t bcap, char *dd, size_t dcap) {
    size_t na = strlen(a), nc = strlen(c);
    if (strncmp(path, a, na) || path[na] != '/') return 0;
    const char *p = path + na + 1, *s = strchr(p, '/');
    if (!s || s == p || (size_t)(s - p) >= bcap || strncmp(s + 1, c, nc) || s[1 + nc] != '/') return 0;
    const char *q = s + 2 + nc;
    if (!*q || strchr(q, '/') || strlen(q) >= dcap) return 0;
    memcpy(b, p, (size_t)(s - p));
    b[s - p] = 0;
    memcpy(dd, q, strlen(q) + 1);
    return 1;
}

static int digits(const char *s, size_t n, int64_t *out) {
    if (!n || n > 9) return 0;
    int64_t v = 0;
    for (size_t i = 0; i < n; i++) {
        if (!isdigit((unsigned char)s[i])) return 0;
        v = v * 10 + (s[i] - '0');
    }
    *out = v;
    return 1;
}

// A Knockout Tour section's request id, Surv_<a>_<b>_<c>_<section>_<suffix>: the tour is all but the section.
static int knockout_id(const char *id, char *tour, size_t cap, int *section) {
    if (strncmp(id, "Surv_", 5)) return 0;
    const char *p = id + 5, *num[4];
    size_t len[4];
    for (int i = 0; i < 4; i++) {
        size_t n = 0;
        while (isdigit((unsigned char)p[n])) n++;
        if (!n || p[n] != '_') return 0;
        num[i] = p, len[i] = n;
        p += n + 1;
    }
    if (!*p) return 0;
    for (const char *s = p; *s; s++)
        if (!isalnum((unsigned char)*s) && *s != '_') return 0;
    int64_t sec;
    if (!digits(num[3], len[3], &sec)) return 0;
    *section = (int)sec;
    snprintf(tour, cap, "Surv_%.*s_%.*s_%.*s_%s", (int)len[0], num[0], (int)len[1], num[1], (int)len[2], num[2], p);
    return 1;
}

static SrvRace *by_uuid(SrvReferee *r, const char *uuid) {
    for (int i = 0; i < RACES_MAX; i++)
        if (r->races[i].used && !strcmp(r->races[i].uuid, uuid)) return &r->races[i];
    return NULL;
}

static int seen_request(const SrvReferee *r, const char *id) {
    for (int i = 0; i < RACES_MAX; i++)
        if (r->races[i].used && !strcmp(r->races[i].request_id, id)) return 1;
    return 0;
}

// A free slot, else the oldest closed race's, else the oldest race's.
static SrvRace *slot(SrvReferee *r) {
    SrvRace *best = NULL;
    for (int i = 0; i < RACES_MAX; i++) {
        SrvRace *x = &r->races[i];
        if (!x->used) return x;
        if (!best || (best->open && !x->open) || (best->open == x->open && x->seq < best->seq)) best = x;
    }
    return best;
}

static void uuid4(char out[40]) {
    uint8_t b[16];
    if (!srv_random(b, sizeof b)) memset(b, 0, sizeof b);
    b[6] = (uint8_t)((b[6] & 0x0f) | 0x40);   // version 4 (random), RFC 9562
    b[8] = (uint8_t)((b[8] & 0x3f) | 0x80);
    snprintf(out, 40, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[0], b[1], b[2], b[3],
             b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

static SrvVal index_list(const SrvRace *x, const int *ks, int n) {
    SrvVal l = {.kind = V_LIST};
    for (int i = 0; i < n; i++) list_add(&l, val_int(x->idx[ks[i]]));
    return l;
}

// A request document seen for the first time: the race, and its result document with status 0 at once.
static void accept(SrvReferee *r, SrvDocs *d, const SrvDoc *req, const char *request_id) {
    SrvRace *x = slot(r);
    memset(x, 0, sizeof *x);
    x->used = x->open = 1;
    x->seq = ++r->seq;
    x->section = -1;
    snprintf(x->request_id, sizeof x->request_id, "%s", request_id);
    uuid4(x->uuid);
    const SrvVal *users = map_get(&req->fields, "users"), *map = map_get(&req->fields, "ussid_map"),
                 *option = map_get(&req->fields, "option");
    int nusers = users && users->kind == V_LIST ? users->u.list.n : 0;
    // ussid_map {"<index>": ussid}; entries with another key or value are left out; none: index i is users[i]
    for (int i = 0; map && map->kind == V_MAP && i < map->u.map.n && x->n < RACE_PLAYERS; i++) {
        const SrvEntry *e = &map->u.map.e[i];
        int64_t idx;
        if (e->v.kind == V_INT && digits(e->key, strlen(e->key), &idx)) {
            x->idx[x->n] = idx;
            x->ussid[x->n++] = e->v.u.i;
        }
    }
    if (!x->n)
        for (int i = 0; i < nusers && i < RACE_PLAYERS; i++) {
            x->idx[i] = i;
            x->ussid[i] = list_int(users, i, -1);
            x->n = i + 1;
        }
    if (option && option->kind == V_MAP) x->dropout_rank = map_int(&option->u.map, "dropout_rank", 0);
    if (!knockout_id(request_id, x->tour, sizeof x->tour, &x->section)) x->section = -1;
    SrvMap f = {0};
    map_set(&f, "request_id", val_str(request_id));
    map_set(&f, "status", val_int(0));
    map_set(&f, "result", (SrvVal){.kind = V_LIST});
    map_set(&f, "user_session_count", val_int(nusers));
    char path[DOC_PATH_MAX];
    snprintf(path, sizeof path, RESULT "%s", x->uuid);
    docs_put(d, path, &f);
}

static int pos_of(const SrvRace *x, int64_t idx) {
    for (int k = 0; k < x->n; k++)
        if (x->idx[k] == idx) return k;
    return -1;
}

static int truthy(const SrvVal *v) { return v && (v->kind == V_BOOL || v->kind == V_INT) && v->u.i != 0; }

// A report {result: [index...], result_order: [rank...], retired} from the player ussid: the indices it lists join
// the official order, their ranks take the reported values, the reporter's own index is retired or not.
static void report(SrvRace *x, const SrvDoc *rep, int64_t ussid, uint64_t now_ms) {
    if (!x->first_report_ms) x->first_report_ms = now_ms ? now_ms : 1;
    const SrvVal *res = map_get(&rep->fields, "result"), *ord = map_get(&rep->fields, "result_order");
    for (int i = 0; res && res->kind == V_LIST && i < res->u.list.n; i++) {
        int k = pos_of(x, list_int(res, i, -1));
        if (k < 0) continue;
        int listed = 0;
        for (int j = 0; j < x->norder && !listed; j++) listed = x->order[j] == k;
        if (!listed) x->order[x->norder++] = k;
        if (ord && ord->kind == V_LIST && i < ord->u.list.n && ord->u.list.v[i].kind == V_INT) {
            x->rank[k] = ord->u.list.v[i].u.i;
            x->ranked[k] = 1;
        }
    }
    for (int k = 0; k < x->n; k++)
        if (x->ussid[k] == ussid) x->retired[k] = (uint8_t)truthy(map_get(&rep->fields, "retired"));
}

// Still in the session: its __us document (field ussid) and a participation document __pgn/<group>/__pus/<us>.
static int present(const SrvDocs *d, int64_t ussid) {
    for (int i = 0; i < d->n; i++) {
        const SrvDoc *u = &d->doc[i];
        if (strncmp(u->path, "__us/", 5) || map_int(&u->fields, "ussid", -1) != ussid) continue;
        char tail[DOC_PATH_MAX + 8];
        int tn = snprintf(tail, sizeof tail, "/__pus/%s", u->path + 5);
        for (int j = 0; j < d->n; j++) {
            const char *p = d->doc[j].path;
            size_t n = strlen(p);
            if (!strncmp(p, "__pgn/", 6) && n > 6 + (size_t)tn && !strcmp(p + n - (size_t)tn, tail)) return 1;
        }
    }
    return 0;
}

// Knocked out by an earlier closed section of the same Knockout Tour.
static int knocked_out(const SrvReferee *r, const SrvRace *x, int64_t ussid) {
    for (int i = 0; x->section >= 0 && i < RACES_MAX; i++) {
        const SrvRace *y = &r->races[i];
        if (!y->used || y->open || y->section < 0 || y->section >= x->section || y->dropout_rank <= 0 ||
            strcmp(y->tour, x->tour))
            continue;
        for (int p = 0; p < y->nfinal; p++)
            if (p + 1 >= y->dropout_rank && y->ussid[y->final[p]] == ussid) return 1;
    }
    return 0;
}

static int is_present(const SrvReferee *r, const SrvDocs *d, const SrvRace *x, int k) {
    return present(d, x->ussid[k]) && !knocked_out(r, x, x->ussid[k]);
}

void referee_elo(const double *rating, const double *rank, int n, double *out) {
    double div = n - 1 > 1 ? n - 1 : 1;
    for (int i = 0; i < n; i++) out[i] = rating[i];
    for (int a = 0; a < n; a++)
        for (int b = a + 1; b < n; b++) {
            double e = 1.0 / (1.0 + pow(10.0, (rating[b] - rating[a]) / 400.0));
            double s = rank[a] < rank[b] ? 1.0 : rank[a] == rank[b] ? 0.5 : 0.0;
            double step = 24.0 * (s - e) / div;
            out[a] += step;
            out[b] -= step;
        }
}

// A float32 held in an integer's low 32 bits (the game's PlayerStatic ratings), also taken as fixed32 or a double.
static double stat(const SrvDocs *d, int64_t ussid, const char *key, double fallback) {
    char path[48];
    snprintf(path, sizeof path, "PlayerStatic/%lld", (long long)ussid);
    const SrvDoc *doc = docs_find(d, path);
    const SrvVal *v = doc ? map_get(&doc->fields, key) : NULL;
    double r = fallback;
    if (v && (v->kind == V_INT || v->kind == V_F32)) {
        uint32_t bits = v->kind == V_INT ? (uint32_t)v->u.i : v->u.f32;
        float f;
        memcpy(&f, &bits, sizeof f);
        r = f;
    } else if (v && v->kind == V_F64) {
        memcpy(&r, &v->u.f64, sizeof r);
    }
    return isfinite(r) ? r : fallback;
}

static void close_race(SrvReferee *r, SrvDocs *d, SrvRace *x) {
    uint8_t in[RACE_PLAYERS] = {0};
    x->nfinal = 0;
    for (int i = 0; i < x->norder; i++) x->final[x->nfinal++] = x->order[i], in[x->order[i]] = 1;
    for (int k = 0; k < x->n; k++)   // then the retired, then everyone else
        if (x->retired[k] && !in[k]) x->final[x->nfinal++] = k, in[k] = 1;
    for (int k = 0; k < x->n; k++)
        if (!in[k]) x->final[x->nfinal++] = k, in[k] = 1;
    int count = 0;
    for (int k = 0; k < x->n; k++) count += is_present(r, d, x, k);
    // Ranks: the reported ones; the others after every reported one, in final order (the reference counted an
    // unreported player's position from 0 against 1-based reports, so it could tie with the winner).
    double rating[RACE_PLAYERS], coef[RACE_PLAYERS], rank[RACE_PLAYERS], out[RACE_PLAYERS], top = -1;
    for (int k = 0; k < x->n; k++)
        if (x->ranked[k] && x->rank[k] > top) top = (double)x->rank[k];
    for (int p = 0; p < x->nfinal; p++) {
        int k = x->final[p];
        rating[p] = stat(d, x->ussid[k], "rating_point_raw_value", 1500.0);
        coef[p] = stat(d, x->ussid[k], "rating_point_coefficient", 0.0);
        rank[p] = x->ranked[k] ? (double)x->rank[k] : ++top;
    }
    referee_elo(rating, rank, x->nfinal, out);
    SrvMap f = {0}, ratings = {0}, coefs = {0};
    for (int p = 0; p < x->nfinal; p++) {
        char key[24];
        snprintf(key, sizeof key, "%lld", (long long)x->ussid[x->final[p]]);
        map_set(&ratings, key, val_f64(out[p]));
        map_set(&coefs, key, val_f64(coef[p]));
    }
    map_set(&f, "request_id", val_str(x->request_id));
    map_set(&f, "status", val_int(1));
    map_set(&f, "result", index_list(x, x->final, x->nfinal));
    map_set(&f, "user_session_count", val_int(count));
    map_set(&f, "ratings", (SrvVal){.kind = V_MAP, .u.map = ratings});
    map_set(&f, "coefficients", (SrvVal){.kind = V_MAP, .u.map = coefs});
    char path[DOC_PATH_MAX];
    snprintf(path, sizeof path, RESULT "%s", x->uuid);
    docs_put(d, path, &f);
    x->open = 0;
}

// The result document's order so far (sent only if it changed: an unchanged document is no change).
static void progress(SrvDocs *d, const SrvRace *x) {
    char path[DOC_PATH_MAX];
    snprintf(path, sizeof path, RESULT "%s", x->uuid);
    const SrvDoc *doc = docs_find(d, path);
    SrvMap f = {0};
    if (doc && !map_copy(&f, &doc->fields)) return;
    if (!doc) {
        map_set(&f, "request_id", val_str(x->request_id));
        map_set(&f, "status", val_int(0));
    }
    map_set(&f, "result", index_list(x, x->order, x->norder));
    docs_put(d, path, &f);
}

static int placed_all(const SrvReferee *r, const SrvDocs *d, const SrvRace *x) {
    for (int k = 0; k < x->n; k++) {
        int placed = x->retired[k];
        for (int i = 0; i < x->norder && !placed; i++) placed = x->order[i] == k;
        if (!placed && is_present(r, d, x, k)) return 0;
    }
    return 1;
}

static void closes(SrvReferee *r, SrvDocs *d, uint64_t now_ms) {
    for (int i = 0; i < RACES_MAX; i++) {
        SrvRace *x = &r->races[i];
        // a race closes once it has a report (before that it has only begun) and everyone there is placed, or when
        // its first report is old enough
        if (x->used && x->open && x->first_report_ms &&
            (placed_all(r, d, x) || now_ms - x->first_report_ms >= RACE_GRACE_MS))
            close_race(r, d, x);
    }
}

void referee_batch(SrvReferee *r, SrvDocs *d, int from, uint64_t now_ms, int64_t now_s) {
    int end = d->nchanged;
    docs_begin(d);
    for (int i = from; i < end; i++) {
        char a[DOC_PATH_MAX], b[DOC_PATH_MAX];
        const char *path = d->changed[i];
        const SrvDoc *doc = docs_find(d, path);
        if (!doc) continue;
        if (split4(path, "@@RefereeRequest", "Request", a, sizeof a, b, sizeof b)) {
            if (strlen(b) < sizeof r->races[0].request_id && !seen_request(r, b)) accept(r, d, doc, b);
        } else if (split4(path, "@@RefereeReport", "User", a, sizeof a, b, sizeof b)) {
            SrvRace *x = by_uuid(r, a);
            int64_t ussid;
            if (x && x->open && digits(b, strlen(b), &ussid)) {
                report(x, docs_find(d, path), ussid, now_ms);
                progress(d, x);
            }
        }
    }
    closes(r, d, now_ms);
    docs_end(d, now_s);
}

void referee_tick(SrvReferee *r, SrvDocs *d, uint64_t now_ms, int64_t now_s) {
    for (int i = 0; i < RACES_MAX; i++) {
        const SrvRace *x = &r->races[i];
        if (x->used && x->open && x->first_report_ms && now_ms - x->first_report_ms >= RACE_GRACE_MS) {
            referee_batch(r, d, d->nchanged, now_ms, now_s);
            return;
        }
    }
}
