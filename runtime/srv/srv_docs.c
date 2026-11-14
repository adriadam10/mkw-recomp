// A game session's Gamesync documents (srv_docs.h).
#include "srv_docs.h"

#include <stdlib.h>
#include <string.h>

#include "srv_svc.h"

enum { MASK_MAX = 256, TRANSFORM_KEYS = 64 };

struct DocSnap {
    char *path;
    int existed;
    SrvMap fields;
    int64_t updated;
    size_t size;
};

static size_t all_bytes;   // every session's documents (one thread: a plain counter)

static char *dup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

void docs_init(SrvDocs *d, int64_t created) {
    memset(d, 0, sizeof *d);
    d->created = created;
}

SrvDoc *docs_find(const SrvDocs *d, const char *path) {
    for (int i = 0; i < d->n; i++)
        if (!strcmp(d->doc[i].path, path)) return &d->doc[i];
    return NULL;
}

static SrvDoc *doc_make(SrvDocs *d, const char *path) {
    SrvDoc *x = docs_find(d, path);
    if (x) return x;
    if (d->n == d->cap) {
        int cap = d->cap ? d->cap * 2 : 16;
        SrvDoc *n = realloc(d->doc, (size_t)cap * sizeof *n);
        if (!n) return NULL;
        d->doc = n;
        d->cap = cap;
    }
    char *p = dup(path);
    if (!p) return NULL;
    d->doc[d->n] = (SrvDoc){.path = p, .updated = d->created};
    return &d->doc[d->n++];
}

static void doc_drop(SrvDocs *d, SrvDoc *x) {
    int i = (int)(x - d->doc);
    free(x->path);
    map_free(&x->fields);
    memmove(&d->doc[i], &d->doc[i + 1], (size_t)(d->n - i - 1) * sizeof *d->doc);   // queries keep their order
    d->n--;
}

static size_t doc_size(const SrvDoc *x) {
    PbOut o = {0};
    map_encode(&o, 2, &x->fields);
    size_t n = o.err ? (size_t)DOCS_BYTES + 1 : o.len;
    pb_free(&o);
    return n + strlen(x->path) + 64;   // 64: what the document's own record costs
}

void docs_changes_clear(SrvDocs *d) {
    for (int i = 0; i < d->nchanged; i++) free(d->changed[i]);
    d->nchanged = 0;
}

static void snaps_free(SrvDocs *d) {
    for (int i = 0; i < d->nsnap; i++) {
        free(d->snap[i].path);
        map_free(&d->snap[i].fields);
    }
    d->nsnap = 0;
    d->batch = 0;
}

void docs_free(SrvDocs *d) {
    if (d->batch) docs_rollback(d);
    for (int i = 0; i < d->n; i++) {
        free(d->doc[i].path);
        map_free(&d->doc[i].fields);
    }
    all_bytes -= d->bytes;
    docs_changes_clear(d);
    free(d->doc);
    free(d->snap);
    free(d->changed);
    memset(d, 0, sizeof *d);
}

void docs_begin(SrvDocs *d) {
    if (d->batch) return;
    d->batch = 1;
    memcpy(d->saved, d->counters, sizeof d->saved);
    d->nsaved = d->ncounters;
}

// The path as it was before the batch, kept the first time the batch touches it.
static int touch(SrvDocs *d, const char *path) {
    if (!d->batch) docs_begin(d);
    for (int i = 0; i < d->nsnap; i++)
        if (!strcmp(d->snap[i].path, path)) return 1;
    if (d->nsnap == d->csnap) {
        int cap = d->csnap ? d->csnap * 2 : 8;
        DocSnap *n = realloc(d->snap, (size_t)cap * sizeof *n);
        if (!n) return 0;
        d->snap = n;
        d->csnap = cap;
    }
    DocSnap *s = &d->snap[d->nsnap];
    SrvDoc *x = docs_find(d, path);
    *s = (DocSnap){.path = dup(path), .existed = x != NULL, .updated = x ? x->updated : 0, .size = x ? x->size : 0};
    if (!s->path) return 0;
    if (x && !map_copy(&s->fields, &x->fields)) {
        free(s->path);
        return 0;
    }
    d->nsnap++;
    return 1;
}

void docs_rollback(SrvDocs *d) {
    for (int i = d->nsnap - 1; i >= 0; i--) {
        DocSnap *s = &d->snap[i];
        SrvDoc *x = docs_find(d, s->path);
        if (!s->existed) {
            if (x) doc_drop(d, x);
            continue;
        }
        if (!x && !(x = doc_make(d, s->path))) {   // no memory to bring it back: it is gone, and so is its share
            d->bytes -= s->size;
            all_bytes -= s->size;
            continue;
        }
        map_free(&x->fields);
        x->fields = s->fields;   // moved back
        s->fields = (SrvMap){0};
        x->updated = s->updated;
        x->size = s->size;
    }
    memcpy(d->counters, d->saved, sizeof d->counters);
    d->ncounters = d->nsaved;
    snaps_free(d);
}

static void add_changed(SrvDocs *d, const char *path) {
    for (int i = 0; i < d->nchanged; i++)
        if (!strcmp(d->changed[i], path)) return;
    if (d->nchanged == d->cchanged) {
        int cap = d->cchanged ? d->cchanged * 2 : 16;
        char **n = realloc(d->changed, (size_t)cap * sizeof *n);
        if (!n) return;   // no memory: this change is not sent (the document itself is right)
        d->changed = n;
        d->cchanged = cap;
    }
    char *p = dup(path);
    if (p) d->changed[d->nchanged++] = p;
}

int docs_end(SrvDocs *d, int64_t now) {
    if (!d->batch) return ST_OK;
    size_t total = d->bytes;
    for (int i = 0; i < d->nsnap; i++) {
        const SrvDoc *x = docs_find(d, d->snap[i].path);
        total = total - d->snap[i].size + (x ? doc_size(x) : 0);   // the snapshot's size is part of the total
    }
    if (d->n > DOCS_MAX || total > DOCS_BYTES || all_bytes - d->bytes + total > DOCS_ALL) {
        docs_rollback(d);
        return ST_RESOURCE_EXHAUSTED;
    }
    for (int i = 0; i < d->nsnap; i++) {
        const DocSnap *s = &d->snap[i];
        SrvDoc *x = docs_find(d, s->path);
        if (x) x->size = doc_size(x);
        if ((x != NULL) != s->existed || (x && !map_equal(&x->fields, &s->fields))) {
            if (x) x->updated = now;
            add_changed(d, s->path);
        }
    }
    all_bytes = all_bytes - d->bytes + total;
    d->bytes = total;
    snaps_free(d);
    return ST_OK;
}

int docs_put(SrvDocs *d, const char *path, SrvMap *fields) {
    SrvDoc *x;
    if (!touch(d, path) || !(x = doc_make(d, path))) {
        map_free(fields);
        return 0;
    }
    map_free(&x->fields);
    x->fields = *fields;
    *fields = (SrvMap){0};
    return 1;
}

int docs_remove(SrvDocs *d, const char *path) {
    if (!touch(d, path)) return 0;
    SrvDoc *x = docs_find(d, path);
    if (x) doc_drop(d, x);
    return 1;
}

int docs_path(const uint8_t *p, size_t n, char out[DOC_PATH_MAX]) {
    if (n >= 5 && !memcmp(p, "docs/", 5)) p += 5, n -= 5;
    if (!n || n >= DOC_PATH_MAX || memchr(p, 0, n)) return 0;
    memcpy(out, p, n);
    out[n] = 0;
    return 1;
}

void docs_encode(PbOut *o, uint32_t num, const SrvDocs *d, const SrvDoc *doc) {
    size_t m = num ? pb_begin(o, num) : 0;
    size_t pm = pb_begin(o, 1);   // "docs/" + path, without a copy
    pb_raw(o, "docs/", 5);
    pb_raw(o, doc->path, strlen(doc->path));
    pb_end(o, pm);
    map_encode(o, 2, &doc->fields);
    size_t t = pb_begin(o, 3);
    pb_int64(o, 1, d->created);
    pb_end(o, t);
    t = pb_begin(o, 4);
    pb_int64(o, 1, doc->updated);
    pb_end(o, t);
    if (num) pb_end(o, m);
}

// ---- the writes (the protocol description, "WriteDocuments")
static void written(PbOut *res) {
    size_t m = pb_begin(res, 1);   // WriteResult {1: {}}
    pb_end(res, pb_begin(res, 1));
    pb_end(res, m);
}

// Document {1: name, 2: Map}: its path and fields.
static int read_doc(const PbField *docf, char path[DOC_PATH_MAX], SrvMap *fields) {
    PbField name, f;
    *fields = (SrvMap){0};
    if (docf->wt != PB_LEN || pb_find(docf->data, docf->len, 1, &name) != 1 || name.wt != PB_LEN ||
        !docs_path(name.data, name.len, path))
        return 0;
    int has = pb_find(docf->data, docf->len, 2, &f);
    if (has < 0 || (has && (f.wt != PB_LEN || !map_decode(fields, f.data, f.len)))) {
        map_free(fields);
        return 0;
    }
    return 1;
}

// Update {1: Document, 2: FieldMask {1: repeated path}}: no mask, "*" or no document yet: the document becomes the given
// fields; otherwise each masked path is copied (or deleted when absent from the given fields).
static int w_update(SrvDocs *d, const PbField *op, PbOut *res) {
    PbField docf, mask, f;
    char path[DOC_PATH_MAX];
    SrvMap given;
    if (pb_find(op->data, op->len, 1, &docf) != 1 || !read_doc(&docf, path, &given)) return ST_INVALID_ARGUMENT;
    FieldPath *paths = NULL;
    int npaths = 0, full = 0, st = ST_OK;
    if (pb_find(op->data, op->len, 2, &mask) == 1 && mask.wt == PB_LEN) {
        PbIn in = pb_in(mask.data, mask.len);
        while (!full && pb_next(&in, &f) > 0) {
            if (f.num != 1 || f.wt != PB_LEN) continue;
            if (f.len == 1 && f.data[0] == '*') full = 1;
            if (full || npaths == MASK_MAX) continue;
            if (!paths && !(paths = malloc(MASK_MAX * sizeof *paths))) {
                map_free(&given);
                return ST_RESOURCE_EXHAUSTED;
            }
            if (fpath_parse(&paths[npaths], (const char *)f.data, f.len)) npaths++;
        }
    }
    SrvDoc *x = NULL;
    if (!touch(d, path)) st = ST_RESOURCE_EXHAUSTED;
    else if (full || !npaths || !(x = docs_find(d, path))) st = docs_put(d, path, &given) ? ST_OK : ST_RESOURCE_EXHAUSTED;
    else if (!map_update(&x->fields, &given, paths, npaths)) st = ST_RESOURCE_EXHAUSTED;
    map_free(&given);
    free(paths);
    if (st == ST_OK) written(res);
    return st;
}

static int w_set(SrvDocs *d, const PbField *op, PbOut *res) {
    PbField docf;
    char path[DOC_PATH_MAX];
    SrvMap given;
    if (pb_find(op->data, op->len, 1, &docf) != 1 || !read_doc(&docf, path, &given)) return ST_INVALID_ARGUMENT;
    if (!docs_put(d, path, &given)) return ST_RESOURCE_EXHAUSTED;
    written(res);
    return ST_OK;
}

static int w_delete(SrvDocs *d, const PbField *op, PbOut *res) {
    PbField name;
    char path[DOC_PATH_MAX];
    if (pb_find(op->data, op->len, 1, &name) != 1 || name.wt != PB_LEN || !docs_path(name.data, name.len, path))
        return ST_INVALID_ARGUMENT;
    if (!docs_remove(d, path)) return ST_RESOURCE_EXHAUSTED;
    written(res);
    return ST_OK;
}

static SrvCounter *counter(SrvDocs *d, const char *name, int make) {
    for (int i = 0; i < d->ncounters; i++)
        if (!strcmp(d->counters[i].name, name)) return &d->counters[i];
    if (!make || d->ncounters == COUNTERS_MAX || strlen(name) >= sizeof d->counters[0].name) return NULL;
    SrvCounter *c = &d->counters[d->ncounters++];
    memset(c, 0, sizeof *c);
    memcpy(c->name, name, strlen(name));
    return c;
}

static int64_t add64(int64_t a, int64_t b) { return (int64_t)((uint64_t)a + (uint64_t)b); }   // wraps, never UB

// One FieldTransform on a top-level field of fields (the protocol description, "FieldTransform"): 2 server time, 3
// increment, 4 maximum, 5 minimum (integers only; a missing or non-integer field counts as 0), 6 load a session counter,
// 7 store one (the upcsid override first).
static int transform(SrvDocs *d, SrvMap *fields, const char *key, int kind, const SrvVal *op, const SrvWriteCtx *ctx) {
    const SrvVal *cur = map_get(fields, key);
    int64_t cv = cur && cur->kind == V_INT ? cur->u.i : 0, r;
    SrvCounter *c;
    switch (kind) {
    case 2:
        return map_set(fields, key, val_int(ctx->now));
    case 3:
    case 4:
    case 5:
        if (op->kind != V_INT) return 1;   // integer operands only
        r = kind == 3 ? add64(cv, op->u.i) : kind == 4 ? (cv > op->u.i ? cv : op->u.i) : (cv < op->u.i ? cv : op->u.i);
        return map_set(fields, key, val_int(r));
    case 6:
        if (op->kind != V_STR) return 1;
        c = counter(d, op->u.s.p, 0);
        return map_set(fields, key, val_int(c ? c->v : 0));
    case 7:
        if (op->kind != V_STR) return 1;
        if (!strcmp(key, "upcsid") && ctx->upcsid) {
            int64_t station = ctx->upcsid(ctx->ctx);
            if (station > 0 && !map_set(fields, key, val_int(station))) return 0;
        }
        cur = map_get(fields, key);
        cv = cur && cur->kind == V_INT ? cur->u.i : 0;
        if ((c = counter(d, op->u.s.p, 1))) c->v = c->v > cv ? c->v : cv;   // a new counter is 0
        return 1;
    }
    return 1;
}

// Transform {1: "docs/<path>", 2: repeated FieldTransform {1: field, 2..7: one operation}}: the document made empty if
// missing, then each operation in order; the result has one value per distinct field, in the order first named (the
// game pairs them by index), each the field's final value (an empty MapValue when it has none).
static int w_transform(SrvDocs *d, const PbField *op, const SrvWriteCtx *ctx, PbOut *res) {
    PbField name, ft, f;
    char path[DOC_PATH_MAX];
    if (pb_find(op->data, op->len, 1, &name) != 1 || name.wt != PB_LEN || !docs_path(name.data, name.len, path))
        return ST_INVALID_ARGUMENT;
    if (!touch(d, path)) return ST_RESOURCE_EXHAUSTED;
    SrvDoc *x = doc_make(d, path);
    if (!x) return ST_RESOURCE_EXHAUSTED;
    char *keys[TRANSFORM_KEYS];
    int nkeys = 0, st = ST_OK;
    PbIn in = pb_in(op->data, op->len);
    while (st == ST_OK && pb_next(&in, &ft) > 0) {
        if (ft.num != 2 || ft.wt != PB_LEN) continue;
        PbIn fi = pb_in(ft.data, ft.len);
        PbField key = {0}, operand = {0};
        int kind = 0, has_key = 0, r;
        while ((r = pb_next(&fi, &f)) > 0) {
            if (f.num == 1 && f.wt == PB_LEN) key = f, has_key = 1;
            else if (f.num >= 2 && f.num <= 7) kind = (int)f.num, operand = f;   // a oneof: the last one wins
        }
        const uint8_t *k = key.data;
        size_t klen = key.len;
        if (klen && k[0] == '`') k++, klen--;   // a top-level field, its quotes stripped
        if (klen && k[klen - 1] == '`') klen--;
        if (r < 0 || !has_key || !klen || klen > KEY_MAX || memchr(k, 0, klen)) {
            st = ST_INVALID_ARGUMENT;
            break;
        }
        char kz[KEY_MAX + 1];
        memcpy(kz, k, klen);
        kz[klen] = 0;
        int seen = 0;
        for (int i = 0; i < nkeys && !seen; i++) seen = !strcmp(keys[i], kz);
        if (!seen) {
            if (nkeys == TRANSFORM_KEYS) {
                st = ST_INVALID_ARGUMENT;
                break;
            }
            if (!(keys[nkeys] = dup(kz))) {
                st = ST_RESOURCE_EXHAUSTED;
                break;
            }
            nkeys++;
        }
        SrvVal v = {0};
        if (kind >= 3 && operand.wt == PB_LEN && !val_decode(&v, operand.data, operand.len)) st = ST_INVALID_ARGUMENT;
        else if (kind && !transform(d, &x->fields, kz, kind, &v, ctx)) st = ST_RESOURCE_EXHAUSTED;
        val_free(&v);
    }
    if (st == ST_OK) {
        size_t m = pb_begin(res, 1);   // WriteResult {2: TransformResults {1: repeated MapValue}}
        size_t t = pb_begin(res, 2);
        static const SrvVal none = {0};
        for (int i = 0; i < nkeys; i++) {
            const SrvVal *v = map_get(&x->fields, keys[i]);
            val_encode(res, 1, v ? v : &none);
        }
        pb_end(res, t);
        pb_end(res, m);
    }
    for (int i = 0; i < nkeys; i++) free(keys[i]);
    return st;
}

int docs_write(SrvDocs *d, const uint8_t *w, size_t len, const SrvWriteCtx *ctx, PbOut *results) {
    PbIn in = pb_in(w, len);
    PbField f, op = {0};
    int kind = 0, r;
    while ((r = pb_next(&in, &f)) > 0)
        if (f.num >= 1 && f.num <= 4 && f.wt == PB_LEN) kind = (int)f.num, op = f;   // a oneof: the last one wins
    if (r < 0 || !kind) return ST_INVALID_ARGUMENT;
    if (!d->batch) docs_begin(d);
    switch (kind) {
    case 1:
        return w_update(d, &op, results);
    case 2:
        return w_set(d, &op, results);
    case 3:
        return w_transform(d, &op, ctx, results);
    default:
        return w_delete(d, &op, results);
    }
}
