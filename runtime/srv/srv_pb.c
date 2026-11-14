// Protocol Buffers wire format (srv_pb.h).
#include "srv_pb.h"

#include <stdlib.h>
#include <string.h>

// A varint is at most 10 bytes; the 10th carries bit 63 only.
static int get_varint(PbIn *in, uint64_t *v) {
    uint64_t r = 0;
    for (int i = 0; i < 10; i++) {
        if (in->p >= in->end) return -1;
        uint8_t b = *in->p++;
        if (i == 9 && b > 1) return -1;
        r |= (uint64_t)(b & 0x7f) << (7 * i);
        if (!(b & 0x80)) {
            *v = r;
            return 0;
        }
    }
    return -1;
}

static uint64_t get_le(const uint8_t *p, int n) {
    uint64_t v = 0;
    for (int i = n - 1; i >= 0; i--) v = v << 8 | p[i];
    return v;
}

int pb_next(PbIn *in, PbField *f) {
    if (in->p >= in->end) return 0;
    uint64_t key;
    if (get_varint(in, &key) < 0) return -1;
    uint64_t num = key >> 3;
    if (num == 0 || num > 0x1fffffff) return -1;
    memset(f, 0, sizeof *f);
    f->num = (uint32_t)num;
    f->wt = (uint8_t)(key & 7);
    switch (f->wt) {
    case PB_VARINT:
        return get_varint(in, &f->u) < 0 ? -1 : 1;
    case PB_I64:
    case PB_I32: {
        int n = f->wt == PB_I64 ? 8 : 4;
        if (in->end - in->p < n) return -1;
        f->u = get_le(in->p, n);
        in->p += n;
        return 1;
    }
    case PB_LEN:
        if (get_varint(in, &f->u) < 0 || f->u > (uint64_t)(in->end - in->p)) return -1;
        f->data = in->p;
        f->len = (size_t)f->u;
        in->p += f->len;
        return 1;
    default:
        return -1;
    }
}

int pb_find(const void *msg, size_t len, uint32_t num, PbField *f) {
    PbIn in = pb_in(msg, len);
    PbField cur;
    int r, found = 0;
    while ((r = pb_next(&in, &cur)) > 0)
        if (cur.num == num) {
            *f = cur;
            found = 1;
        }
    return r < 0 ? -1 : found;
}

void pb_free(PbOut *o) {
    free(o->buf);
    memset(o, 0, sizeof *o);
}

static int room(PbOut *o, size_t n) {
    if (o->err) return 0;
    if (n > PB_OUT_MAX - o->len) {
        o->err = 1;
        return 0;
    }
    if (o->len + n <= o->cap) return 1;
    size_t cap = o->cap ? o->cap : 64;
    while (cap < o->len + n) cap *= 2;
    uint8_t *b = realloc(o->buf, cap);
    if (!b) {
        o->err = 1;
        return 0;
    }
    o->buf = b;
    o->cap = cap;
    return 1;
}

static int enc_varint(uint8_t out[10], uint64_t v) {
    int n = 0;
    do {
        out[n++] = (uint8_t)((v & 0x7f) | (v > 0x7f ? 0x80 : 0));
        v >>= 7;
    } while (v);
    return n;
}

static void put(PbOut *o, const void *data, size_t n) {
    if (!room(o, n)) return;
    if (n) memcpy(o->buf + o->len, data, n);
    o->len += n;
}

static void put_varint(PbOut *o, uint64_t v) {
    uint8_t b[10];
    put(o, b, (size_t)enc_varint(b, v));
}

static void put_key(PbOut *o, uint32_t num, int wt) { put_varint(o, (uint64_t)num << 3 | (unsigned)wt); }

void pb_varint(PbOut *o, uint32_t num, uint64_t v) {
    put_key(o, num, PB_VARINT);
    put_varint(o, v);
}

static void put_le(PbOut *o, uint32_t num, int wt, uint64_t v, int n) {
    uint8_t b[8];
    for (int i = 0; i < n; i++) b[i] = (uint8_t)(v >> (8 * i));
    put_key(o, num, wt);
    put(o, b, (size_t)n);
}

void pb_fixed32(PbOut *o, uint32_t num, uint32_t v) { put_le(o, num, PB_I32, v, 4); }
void pb_fixed64(PbOut *o, uint32_t num, uint64_t v) { put_le(o, num, PB_I64, v, 8); }

void pb_double(PbOut *o, uint32_t num, double v) {
    uint64_t u;
    memcpy(&u, &v, sizeof u);
    pb_fixed64(o, num, u);
}

void pb_bytes(PbOut *o, uint32_t num, const void *data, size_t len) {
    put_key(o, num, PB_LEN);
    put_varint(o, len);
    put(o, data, len);
}

void pb_str(PbOut *o, uint32_t num, const char *s) { pb_bytes(o, num, s, strlen(s)); }

void pb_raw(PbOut *o, const void *data, size_t len) { put(o, data, len); }

size_t pb_begin(PbOut *o, uint32_t num) {
    put_key(o, num, PB_LEN);
    return o->len;
}

static int mapvalue(PbOut *o, uint32_t num, const uint8_t *v, size_t len, int depth);

// A Map message {1: repeated {1: key, 2: MapValue}} written with each value normalised.
static int map(PbOut *o, uint32_t num, const uint8_t *v, size_t len, int depth) {
    PbIn in = pb_in(v, len);
    PbField e, k = {0}, val = {0};
    int r;
    size_t m = pb_begin(o, num);
    while ((r = pb_next(&in, &e)) > 0) {
        if (e.num != 1 || e.wt != PB_LEN) continue;
        if (pb_find(e.data, e.len, 1, &k) != 1 || k.wt != PB_LEN) continue;
        size_t em = pb_begin(o, 1);
        pb_bytes(o, 1, k.data, k.len);
        if (pb_find(e.data, e.len, 2, &val) == 1 && val.wt == PB_LEN) mapvalue(o, 2, val.data, val.len, depth + 1);
        pb_end(o, em);
    }
    pb_end(o, m);
    return r == 0;
}

static int mapvalue(PbOut *o, uint32_t num, const uint8_t *v, size_t len, int depth) {
    if (depth > 16) return 0;   // nesting a peer controls stops here
    PbIn in = pb_in(v, len);
    PbField f, s;
    int r;
    size_t m = pb_begin(o, num);
    while ((r = pb_next(&in, &f)) > 0) {
        int ok = f.wt == PB_LEN;
        switch (f.num) {
        case 1:   // int32: the same varint is the int64 of the same value (negative ones are 10 bytes either way)
        case 3:
            if (f.wt == PB_VARINT) pb_varint(o, 3, f.u);
            break;
        case 2:
            if (f.wt == PB_VARINT) pb_varint(o, 2, f.u);
            break;
        case 4:
            if (f.wt == PB_I32) pb_fixed32(o, 4, (uint32_t)f.u);
            break;
        case 5:
            if (f.wt == PB_I64) pb_fixed64(o, 5, f.u);
            break;
        case 6:   // Timestamp {1: seconds}: its seconds
            if (ok) pb_varint(o, 3, pb_find(f.data, f.len, 1, &s) == 1 && s.wt == PB_VARINT ? s.u : 0);
            break;
        case 7:
        case 11:
            if (ok) pb_bytes(o, 7, f.data, f.len);
            break;
        case 8:
            if (ok) pb_bytes(o, 8, f.data, f.len);
            break;
        case 9: {   // list {1: repeated MapValue}
            if (!ok) break;
            PbIn li = pb_in(f.data, f.len);
            PbField item;
            size_t lm = pb_begin(o, 9);
            while (pb_next(&li, &item) > 0)
                if (item.num == 1 && item.wt == PB_LEN) mapvalue(o, 1, item.data, item.len, depth + 1);
            pb_end(o, lm);
            break;
        }
        case 10:
            if (ok) map(o, 10, f.data, f.len, depth);
            break;
        }
    }
    pb_end(o, m);
    return r == 0;
}

int pb_mapvalue(PbOut *o, uint32_t num, const uint8_t *v, size_t len) { return mapvalue(o, num, v, len, 0); }

// The content written since pb_begin moves up to make room for its length in front of it.
void pb_end(PbOut *o, size_t mark) {
    if (o->err || mark > o->len) return;
    size_t clen = o->len - mark;
    uint8_t b[10];
    size_t n = (size_t)enc_varint(b, clen);
    if (!room(o, n)) return;
    memmove(o->buf + mark + n, o->buf + mark, clen);
    memcpy(o->buf + mark, b, n);
    o->len += n;
}
