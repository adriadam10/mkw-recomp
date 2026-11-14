// The game's attribute values as a tree (srv_value.h).
#include "srv_value.h"

#include <stdlib.h>
#include <string.h>

SrvVal val_f64(double d) {
    SrvVal v = {.kind = V_F64};
    memcpy(&v.u.f64, &d, sizeof d);
    return v;
}

static SrvVal mk_bytes(int kind, const void *p, size_t n) {
    SrvVal v = {.kind = V_NONE};
    char *b = malloc(n + 1);
    if (!b) return v;
    if (n) memcpy(b, p, n);
    b[n] = 0;
    v.kind = kind;
    v.u.s.p = b;
    v.u.s.n = n;
    return v;
}

SrvVal val_str(const char *s) { return mk_bytes(V_STR, s, strlen(s)); }
SrvVal val_bytes(const void *p, size_t n) { return mk_bytes(V_BYTES, p, n); }

void val_free(SrvVal *v) {
    switch (v->kind) {
    case V_STR:
    case V_BYTES:
        free(v->u.s.p);
        break;
    case V_LIST:
        for (int i = 0; i < v->u.list.n; i++) val_free(&v->u.list.v[i]);
        free(v->u.list.v);
        break;
    case V_MAP:
        map_free(&v->u.map);
        break;
    }
    memset(v, 0, sizeof *v);   // V_NONE
}

void map_free(SrvMap *m) {
    for (int i = 0; i < m->n; i++) {
        free(m->e[i].key);
        val_free(&m->e[i].v);
    }
    free(m->e);
    memset(m, 0, sizeof *m);
}

int list_add(SrvVal *list, SrvVal v) {
    SrvList *l = &list->u.list;
    if (l->n == l->cap) {
        int cap = l->cap ? l->cap * 2 : 4;
        SrvVal *n = realloc(l->v, (size_t)cap * sizeof *n);
        if (!n) {
            val_free(&v);
            return 0;
        }
        l->v = n;
        l->cap = cap;
    }
    l->v[l->n++] = v;
    return 1;
}

// A new entry at the end, no check for the key being there already.
static int map_append(SrvMap *m, const char *key, SrvVal v) {
    if (m->n == m->cap) {
        int cap = m->cap ? m->cap * 2 : 8;
        SrvEntry *n = realloc(m->e, (size_t)cap * sizeof *n);
        if (!n) {
            val_free(&v);
            return 0;
        }
        m->e = n;
        m->cap = cap;
    }
    size_t klen = strlen(key);
    char *k = malloc(klen + 1);
    if (!k) {
        val_free(&v);
        return 0;
    }
    memcpy(k, key, klen + 1);
    m->e[m->n].key = k;
    m->e[m->n].v = v;
    m->n++;
    return 1;
}

static int map_index(const SrvMap *m, const char *key) {
    for (int i = 0; i < m->n; i++)
        if (!strcmp(m->e[i].key, key)) return i;
    return -1;
}

SrvVal *map_get(const SrvMap *m, const char *key) {
    int i = map_index(m, key);
    return i < 0 ? NULL : &m->e[i].v;
}

int map_set(SrvMap *m, const char *key, SrvVal v) {
    int i = map_index(m, key);
    if (i < 0) return map_append(m, key, v);
    val_free(&m->e[i].v);
    m->e[i].v = v;
    return 1;
}

void map_del(SrvMap *m, const char *key) {
    int i = map_index(m, key);
    if (i < 0) return;
    free(m->e[i].key);
    val_free(&m->e[i].v);
    memmove(&m->e[i], &m->e[i + 1], (size_t)(m->n - i - 1) * sizeof *m->e);   // the others keep their order
    m->n--;
}

int64_t map_int(const SrvMap *m, const char *key, int64_t fallback) {
    const SrvVal *v = map_get(m, key);
    return v && v->kind == V_INT ? v->u.i : fallback;
}

const char *map_str(const SrvMap *m, const char *key) {
    const SrvVal *v = map_get(m, key);
    return v && v->kind == V_STR ? v->u.s.p : NULL;
}

int64_t list_int(const SrvVal *list, int i, int64_t fallback) {
    if (list->kind != V_LIST || i < 0 || i >= list->u.list.n || list->u.list.v[i].kind != V_INT) return fallback;
    return list->u.list.v[i].u.i;
}

int val_copy(SrvVal *dst, const SrvVal *src) {
    *dst = (SrvVal){0};
    switch (src->kind) {
    case V_STR:
    case V_BYTES:
        *dst = mk_bytes(src->kind, src->u.s.p, src->u.s.n);
        return dst->kind == src->kind;
    case V_LIST: {
        SrvVal l = {.kind = V_LIST};
        for (int i = 0; i < src->u.list.n; i++) {
            SrvVal item;
            if (!val_copy(&item, &src->u.list.v[i]) || !list_add(&l, item)) {
                val_free(&l);
                return 0;
            }
        }
        *dst = l;
        return 1;
    }
    case V_MAP:
        dst->kind = V_MAP;
        if (map_copy(&dst->u.map, &src->u.map)) return 1;
        *dst = (SrvVal){0};
        return 0;
    default:
        *dst = *src;
        return 1;
    }
}

int map_copy(SrvMap *dst, const SrvMap *src) {
    *dst = (SrvMap){0};
    for (int i = 0; i < src->n; i++) {
        SrvVal v;
        if (!val_copy(&v, &src->e[i].v) || !map_append(dst, src->e[i].key, v)) {
            map_free(dst);
            return 0;
        }
    }
    return 1;
}

int val_equal(const SrvVal *a, const SrvVal *b) {
    if (a->kind != b->kind) return 0;
    switch (a->kind) {
    case V_INT:
    case V_BOOL:
        return a->u.i == b->u.i;
    case V_F32:
        return a->u.f32 == b->u.f32;
    case V_F64:
        return a->u.f64 == b->u.f64;   // the bits: a NaN equals itself, 0.0 and -0.0 differ
    case V_STR:
    case V_BYTES:
        return a->u.s.n == b->u.s.n && !memcmp(a->u.s.p, b->u.s.p, a->u.s.n);
    case V_LIST:
        if (a->u.list.n != b->u.list.n) return 0;
        for (int i = 0; i < a->u.list.n; i++)
            if (!val_equal(&a->u.list.v[i], &b->u.list.v[i])) return 0;
        return 1;
    case V_MAP:
        return map_equal(&a->u.map, &b->u.map);
    default:
        return 1;
    }
}

int map_equal(const SrvMap *a, const SrvMap *b) {
    if (a->n != b->n) return 0;   // keys are unique in a map: same count and every key of a in b is the same set
    for (int i = 0; i < a->n; i++) {
        const SrvVal *v = map_get(b, a->e[i].key);
        if (!v || !val_equal(&a->e[i].v, v)) return 0;
    }
    return 1;
}

// ---- decoding (depth as in pb_mapvalue: a list's items and a map's values one level down)
static int decode(SrvVal *v, const uint8_t *msg, size_t len, int depth);

static int decode_map(SrvMap *m, const uint8_t *msg, size_t len, int depth) {
    PbIn in = pb_in(msg, len);
    PbField e, k, val;
    int r;
    while ((r = pb_next(&in, &e)) > 0) {
        if (e.num != 1 || e.wt != PB_LEN) continue;
        if (m->n == VAL_ITEMS) return 0;
        int hk = pb_find(e.data, e.len, 1, &k), has = pb_find(e.data, e.len, 2, &val);
        if (hk < 0 || has < 0) return 0;
        if (hk != 1 || k.wt != PB_LEN || k.len > KEY_MAX || memchr(k.data, 0, k.len))
            continue;   // a key the game cannot have written
        char key[KEY_MAX + 1];
        memcpy(key, k.data, k.len);
        key[k.len] = 0;
        SrvVal x = {0};
        if (has == 1 && val.wt == PB_LEN && !decode(&x, val.data, val.len, depth + 1)) {
            val_free(&x);
            return 0;
        }
        if (!map_set(m, key, x)) return 0;
    }
    return r == 0;
}

static int decode(SrvVal *v, const uint8_t *msg, size_t len, int depth) {
    *v = (SrvVal){0};
    if (depth > VAL_DEPTH) return 1;   // nesting a peer controls is cut here (no value), as pb_mapvalue does
    PbIn in = pb_in(msg, len);
    PbField f, s;
    int r;
    while ((r = pb_next(&in, &f)) > 0) {
        SrvVal x = {0};
        int ok = 1;
        switch (f.num) {
        case 1:   // int32: its varint is the int64 of the same value (a negative one is sign-extended to 10 bytes)
        case 3:
            if ((ok = f.wt == PB_VARINT)) x = val_int((int64_t)f.u);
            break;
        case 2:
            if ((ok = f.wt == PB_VARINT)) x = val_bool(f.u != 0);
            break;
        case 4:
            if ((ok = f.wt == PB_I32)) x = (SrvVal){.kind = V_F32, .u.f32 = (uint32_t)f.u};
            break;
        case 5:
            if ((ok = f.wt == PB_I64)) x = (SrvVal){.kind = V_F64, .u.f64 = f.u};
            break;
        case 6:   // Timestamp {1: seconds}: its seconds, an integer
            if ((ok = f.wt == PB_LEN))
                x = val_int(pb_find(f.data, f.len, 1, &s) == 1 && s.wt == PB_VARINT ? (int64_t)s.u : 0);
            break;
        case 7:
        case 11:
        case 8:
            if (!(ok = f.wt == PB_LEN)) break;
            x = mk_bytes(f.num == 8 ? V_BYTES : V_STR, f.data, f.len);
            if (x.kind == V_NONE) return 0;
            break;
        case 9: {   // list {1: repeated MapValue}
            if (!(ok = f.wt == PB_LEN)) break;
            x.kind = V_LIST;
            PbIn li = pb_in(f.data, f.len);
            PbField item;
            int lr;
            while ((lr = pb_next(&li, &item)) > 0) {
                if (item.num != 1 || item.wt != PB_LEN) continue;
                SrvVal y;
                if (x.u.list.n == VAL_ITEMS) {
                    lr = -1;
                    break;
                }
                if (!decode(&y, item.data, item.len, depth + 1)) {
                    val_free(&y);
                    lr = -1;
                    break;
                }
                if (!list_add(&x, y)) {
                    lr = -1;
                    break;
                }
            }
            if (lr < 0) {
                val_free(&x);
                return 0;
            }
            break;
        }
        case 10:
            if (!(ok = f.wt == PB_LEN)) break;
            x.kind = V_MAP;
            if (!decode_map(&x.u.map, f.data, f.len, depth)) {
                val_free(&x);
                return 0;
            }
            break;
        default:
            ok = 0;
        }
        if (ok) {   // a oneof: the last case written wins
            val_free(v);
            *v = x;
        }
    }
    return r == 0;
}

int val_decode(SrvVal *v, const uint8_t *msg, size_t len) { return decode(v, msg, len, 0); }

int map_decode(SrvMap *m, const uint8_t *msg, size_t len) {
    *m = (SrvMap){0};
    return decode_map(m, msg, len, 0);
}

// ---- encoding
void val_encode(PbOut *o, uint32_t num, const SrvVal *v) {
    size_t m = pb_begin(o, num);
    switch (v->kind) {
    case V_INT:
        pb_int64(o, 3, v->u.i);
        break;
    case V_BOOL:
        pb_bool(o, 2, (int)v->u.i);   // written even when false: a oneof case that is set always is
        break;
    case V_F32:
        pb_fixed32(o, 4, v->u.f32);
        break;
    case V_F64:
        pb_fixed64(o, 5, v->u.f64);
        break;
    case V_STR:
        pb_bytes(o, 7, v->u.s.p, v->u.s.n);
        break;
    case V_BYTES:
        pb_bytes(o, 8, v->u.s.p, v->u.s.n);
        break;
    case V_LIST: {
        size_t l = pb_begin(o, 9);
        for (int i = 0; i < v->u.list.n; i++) val_encode(o, 1, &v->u.list.v[i]);
        pb_end(o, l);
        break;
    }
    case V_MAP:
        map_encode(o, 10, &v->u.map);
        break;
    }
    pb_end(o, m);
}

void map_encode(PbOut *o, uint32_t num, const SrvMap *m) {
    size_t mk = pb_begin(o, num);
    for (int i = 0; i < m->n; i++) {
        size_t e = pb_begin(o, 1);
        pb_str(o, 1, m->e[i].key);
        val_encode(o, 2, &m->e[i].v);
        pb_end(o, e);
    }
    pb_end(o, mk);
}

// ---- field paths
int fpath_parse(FieldPath *fp, const char *s, size_t n) {
    size_t i = 0, o = 0;
    fp->n = 0;
    if (!n) return 0;
    for (;;) {
        if (fp->n == FP_SEGS) return 0;
        fp->seg[fp->n++] = fp->buf + o;
        size_t start = o;
        if (s[i] == '`') {   // quoted: dots are part of the name, the quotes are not
            i++;
            for (;;) {
                if (i >= n) return 0;
                char c = s[i++];
                if (c == '`') break;
                if (c == '\\' && i < n) c = s[i++];
                if (!c || o + 1 >= FP_LEN) return 0;
                fp->buf[o++] = c;
            }
            if (i < n && s[i] != '.') return 0;
        } else {
            while (i < n && s[i] != '.') {
                if (!s[i] || o + 1 >= FP_LEN) return 0;
                fp->buf[o++] = s[i++];
            }
        }
        if (o == start) return 0;
        fp->buf[o++] = 0;
        if (i == n) return 1;
        if (++i == n) return 0;   // a dot at the end
    }
}

const SrvVal *map_path_get(const SrvMap *m, const FieldPath *fp) {
    const SrvVal *v = NULL;
    for (int i = 0; i < fp->n; i++) {
        if (!(v = map_get(m, fp->seg[i]))) return NULL;
        if (i + 1 < fp->n) {
            if (v->kind != V_MAP) return NULL;
            m = &v->u.map;
        }
    }
    return v;
}

int map_path_set(SrvMap *m, const FieldPath *fp, const SrvVal *v) {
    if (fp->n < 1) return 1;
    for (int i = 0; i + 1 < fp->n; i++) {
        SrvVal *x = map_get(m, fp->seg[i]);
        if (!x) {
            if (!map_set(m, fp->seg[i], (SrvVal){.kind = V_MAP})) return 0;
            x = map_get(m, fp->seg[i]);
        } else if (x->kind != V_MAP) {   // a value in the way becomes a map
            val_free(x);
            x->kind = V_MAP;
        }
        m = &x->u.map;
    }
    SrvVal c;
    return val_copy(&c, v) && map_set(m, fp->seg[fp->n - 1], c);
}

void map_path_del(SrvMap *m, const FieldPath *fp) {
    if (fp->n < 1) return;
    for (int i = 0; i + 1 < fp->n; i++) {
        SrvVal *x = map_get(m, fp->seg[i]);
        if (!x || x->kind != V_MAP) return;
        m = &x->u.map;
    }
    map_del(m, fp->seg[fp->n - 1]);
}

int map_update(SrvMap *doc, const SrvMap *given, const FieldPath *paths, int npaths) {
    for (int i = 0; i < npaths; i++) {
        const SrvVal *v = map_path_get(given, &paths[i]);
        if (v) {
            if (!map_path_set(doc, &paths[i], v)) return 0;
        } else {
            map_path_del(doc, &paths[i]);
        }
    }
    return 1;
}
