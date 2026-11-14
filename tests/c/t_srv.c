// The online server's building blocks that need no network: the protobuf reader and writer (runtime/srv/srv_pb.c)
// against the encoding guide's examples and malformed input, the STUN reply (runtime/srv/srv_udp.c) against RFC 5769's
// sample addresses and Pia's NAT check against the protocol description's example, the value tree and field paths
// (srv_value.c), Gamesync writes (srv_docs.c: masks, transforms, counters, no change for unchanged documents, the
// budget) and the race referee (srv_referee.c: a race accepted, reported, closed with Elo ratings). Built and run by
// tests/test_srv.py.
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "srv.h"
#include "srv_docs.h"
#include "srv_pb.h"
#include "srv_referee.h"
#include "srv_svc.h"
#include "srv_udp.h"
#include "srv_value.h"

// srv_main.c's random bytes, made predictable here (the referee's race ids)
int srv_random(void *buf, size_t n) {
    static uint8_t next;
    for (size_t i = 0; i < n; i++) ((uint8_t *)buf)[i] = next++;
    return 1;
}

static int failed, passed;

static void check(int ok, const char *what) {
    if (ok) {
        passed++;
    } else {
        failed++;
        printf("FAIL %s\n", what);
    }
}

static int same(const PbOut *o, const uint8_t *want, size_t n) {
    return !o->err && o->len == n && memcmp(o->buf, want, n) == 0;
}

static int bad(const uint8_t *msg, size_t n) {
    PbIn in = pb_in(msg, n);
    PbField f;
    int r;
    while ((r = pb_next(&in, &f)) > 0) {}
    return r < 0;
}

static void test_pb(void) {
    PbOut o = {0};
    pb_varint(&o, 1, 150);
    check(same(&o, (const uint8_t[]){0x08, 0x96, 0x01}, 3), "varint 150");
    pb_free(&o);

    pb_str(&o, 2, "testing");
    check(same(&o, (const uint8_t[]){0x12, 7, 't', 'e', 's', 't', 'i', 'n', 'g'}, 9), "string");
    pb_free(&o);

    size_t m = pb_begin(&o, 3);
    pb_varint(&o, 1, 150);
    pb_end(&o, m);
    check(same(&o, (const uint8_t[]){0x1a, 3, 0x08, 0x96, 0x01}, 5), "nested");
    pb_free(&o);

    pb_int32(&o, 1, -1);
    check(same(&o, (const uint8_t[]){0x08, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01}, 11),
          "negative int32 takes 10 bytes");
    pb_free(&o);

    pb_fixed32(&o, 5, 0x12345678);
    check(same(&o, (const uint8_t[]){0x2d, 0x78, 0x56, 0x34, 0x12}, 5), "fixed32 little-endian");
    pb_free(&o);

    // Three levels, the middle one over 127 bytes: its length takes 2 bytes, the outer one's grows with it.
    uint8_t blob[200];
    memset(blob, 0xab, sizeof blob);
    size_t a = pb_begin(&o, 1);
    size_t b = pb_begin(&o, 2);
    pb_bytes(&o, 3, blob, sizeof blob);
    pb_double(&o, 4, 1.5);
    pb_end(&o, b);
    pb_bool(&o, 5, 1);
    pb_end(&o, a);
    PbField f, g;
    check(!o.err && pb_find(o.buf, o.len, 1, &f) == 1 && f.wt == PB_LEN, "outer field");
    PbIn in = pb_sub(&f);
    check(pb_next(&in, &g) == 1 && g.num == 2 && g.len == 3 + sizeof blob + 9, "middle length");
    PbField h;
    check(pb_find(g.data, g.len, 3, &h) == 1 && h.len == sizeof blob && h.data[199] == 0xab, "inner bytes");
    double d = 0;
    check(pb_find(g.data, g.len, 4, &h) == 1 && h.wt == PB_I64 && (memcpy(&d, &h.u, 8), d == 1.5), "double");
    check(pb_next(&in, &g) == 1 && g.num == 5 && g.u == 1 && pb_next(&in, &g) == 0, "after the middle");
    pb_free(&o);

    const uint8_t twice[] = {0x08, 1, 0x08, 2};
    check(pb_find(twice, sizeof twice, 1, &f) == 1 && f.u == 2, "last value wins");
    check(pb_find(twice, sizeof twice, 9, &f) == 0, "absent field");

    check(bad((const uint8_t[]){0x08}, 1), "varint cut");
    check(bad((const uint8_t[]){0x08, 0x80}, 2), "varint continued past the end");
    check(bad((const uint8_t[]){0x12, 5, 'a'}, 3), "length past the end");
    check(bad((const uint8_t[]){0x0b}, 1), "group start refused");
    check(bad((const uint8_t[]){0x0c}, 1), "group end refused");
    check(bad((const uint8_t[]){0x00, 1}, 2), "field 0 refused");
    check(bad((const uint8_t[]){0x0d, 1, 2, 3}, 4), "fixed32 cut");
    check(bad((const uint8_t[]){0x08, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x02}, 11),
          "varint over 64 bits");
    check(bad((const uint8_t[]){0x08, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x01}, 12),
          "varint of 11 bytes");
    check(bad((const uint8_t[]){0x12, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01}, 11),
          "length of 2^64-1");
    check(!bad((const uint8_t[]){0x12, 0}, 2), "empty bytes are fine");
}

// pb_mapvalue(in) written as field 2 equals want written as field 2.
static int normalises_to(const PbOut *in, const PbOut *want) {
    PbOut got = {0}, ref = {0};
    pb_mapvalue(&got, 2, in->buf, in->len);
    pb_bytes(&ref, 2, want->buf, want->len);
    int ok = !got.err && got.len == ref.len && !memcmp(got.buf, ref.buf, got.len);
    pb_free(&got);
    pb_free(&ref);
    return ok;
}

static void test_mapvalue(void) {
    PbOut in = {0}, want = {0};
    pb_int32(&in, 1, -5);
    pb_int64(&want, 3, -5);
    check(normalises_to(&in, &want), "int32 becomes int64");
    pb_free(&in), pb_free(&want);

    pb_str(&in, 11, "Mario");
    pb_str(&want, 7, "Mario");
    check(normalises_to(&in, &want), "second string case becomes 7");
    pb_free(&in), pb_free(&want);

    size_t m = pb_begin(&in, 6);
    pb_int64(&in, 1, 1700000000);
    pb_int32(&in, 2, 500);
    pb_end(&in, m);
    pb_int64(&want, 3, 1700000000);
    check(normalises_to(&in, &want), "Timestamp becomes its seconds");
    pb_free(&in), pb_free(&want);

    pb_double(&in, 5, 1500.25);
    pb_double(&want, 5, 1500.25);
    pb_fixed32(&in, 4, 1153138688);
    pb_fixed32(&want, 4, 1153138688);
    check(normalises_to(&in, &want), "fixed64 and fixed32 kept bit-exact");
    pb_free(&in), pb_free(&want);

    // list {1: {1: 7}} and map {k: {11: "x"}}
    m = pb_begin(&in, 9);
    size_t e = pb_begin(&in, 1);
    pb_int32(&in, 1, 7);
    pb_end(&in, e);
    pb_end(&in, m);
    m = pb_begin(&want, 9);
    e = pb_begin(&want, 1);
    pb_int64(&want, 3, 7);
    pb_end(&want, e);
    pb_end(&want, m);
    check(normalises_to(&in, &want), "list items normalised");
    pb_free(&in), pb_free(&want);

    m = pb_begin(&in, 10);
    e = pb_begin(&in, 1);
    pb_str(&in, 1, "k");
    size_t v = pb_begin(&in, 2);
    pb_str(&in, 11, "x");
    pb_end(&in, v);
    pb_end(&in, e);
    pb_end(&in, m);
    m = pb_begin(&want, 10);
    e = pb_begin(&want, 1);
    pb_str(&want, 1, "k");
    v = pb_begin(&want, 2);
    pb_str(&want, 7, "x");
    pb_end(&want, v);
    pb_end(&want, e);
    pb_end(&want, m);
    check(normalises_to(&in, &want), "map values normalised");
    pb_free(&in), pb_free(&want);

    // a value 40 lists deep (each list {1: value}): cut at 16 levels, no crash
    PbOut deep = {0};
    pb_int64(&deep, 3, 1);
    for (int i = 0; i < 40; i++) {
        PbOut list = {0}, wrap = {0};
        pb_bytes(&list, 1, deep.buf, deep.len);
        pb_bytes(&wrap, 9, list.buf, list.len);
        pb_free(&list);
        pb_free(&deep);
        deep = wrap;
    }
    PbOut got = {0};
    pb_mapvalue(&got, 2, deep.buf, deep.len);
    check(!got.err && got.len < deep.len, "deep nesting cut");
    pb_free(&got), pb_free(&deep);
    check(!pb_mapvalue(&got, 2, (const uint8_t[]){0x0b}, 1), "malformed value refused");
    pb_free(&got);
}

static const uint8_t TXID[12] = {0xb7, 0xe7, 0xa7, 0x01, 0xbc, 0x34, 0xd6, 0x86, 0xfa, 0x87, 0xdf, 0xae};

static size_t request(uint8_t *req, int modern) {
    memset(req, 0, 20);
    req[1] = 0x01;
    if (modern) memcpy(req + 4, (const uint8_t[]){0x21, 0x12, 0xa4, 0x42}, 4);
    else memcpy(req + 4, (const uint8_t[]){1, 2, 3, 4}, 4);
    memcpy(req + 8, TXID, 12);
    return 20;
}

static void test_stun(void) {
    uint8_t req[64], out[128];
    SrvPeer v4 = {0, {192, 0, 2, 1}, 32853};
    size_t n = stun_reply(req, request(req, 1), &v4, out, sizeof out);
    // RFC 5769 2.2: 192.0.2.1 port 32853 with that transaction id.
    const uint8_t want4[] = {0x01, 0x01, 0x00, 0x18, 0x21, 0x12, 0xa4, 0x42, 0xb7, 0xe7, 0xa7, 0x01, 0xbc, 0x34,
                             0xd6, 0x86, 0xfa, 0x87, 0xdf, 0xae, 0x00, 0x20, 0x00, 0x08, 0x00, 0x01, 0xa1, 0x47,
                             0xe1, 0x12, 0xa6, 0x43, 0x00, 0x01, 0x00, 0x08, 0x00, 0x01, 0x80, 0x55, 0xc0, 0x00,
                             0x02, 0x01};
    check(n == sizeof want4 && memcmp(out, want4, n) == 0, "STUN IPv4 reply");

    // RFC 5769 2.3: 2001:db8:1234:5678:11:2233:4455:6677 port 32853.
    SrvPeer v6 = {1, {0x20, 0x01, 0x0d, 0xb8, 0x12, 0x34, 0x56, 0x78, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77},
                  32853};
    n = stun_reply(req, request(req, 1), &v6, out, sizeof out);
    const uint8_t xor6[] = {0x00, 0x20, 0x00, 0x14, 0x00, 0x02, 0xa1, 0x47, 0x01, 0x13, 0xa9, 0xfa, 0xa5, 0xd3,
                            0xf1, 0x79, 0xbc, 0x25, 0xf4, 0xb5, 0xbe, 0xd2, 0xb9, 0xd9};
    check(n == 20 + 24 + 24 && out[3] == 48 && memcmp(out + 20, xor6, sizeof xor6) == 0, "STUN IPv6 reply");

    n = stun_reply(req, request(req, 0), &v4, out, sizeof out);
    check(n == 32 && out[3] == 12 && out[21] == 0x01 && memcmp(out + 4, req + 4, 16) == 0, "RFC 3489 request");

    size_t len = request(req, 1);
    req[0] = 0x01, req[1] = 0x01;
    check(stun_reply(req, len, &v4, out, sizeof out) == 0, "a response is not answered");
    request(req, 1);
    check(stun_reply(req, 19, &v4, out, sizeof out) == 0, "short datagram");
    req[3] = 4;
    check(stun_reply(req, 20, &v4, out, sizeof out) == 0, "length past the datagram");
    request(req, 1);
    req[0] = 0x40;
    check(stun_reply(req, 20, &v4, out, sizeof out) == 0, "not STUN (first bits)");
    request(req, 1);
    check(stun_reply(req, 20, &v4, out, 40) == 0, "reply buffer too small");
    // Attributes in the request (here a 4-byte one) are read past, not answered.
    request(req, 1);
    req[3] = 8;
    memcpy(req + 20, (const uint8_t[]){0x80, 0x22, 0x00, 0x04, 'm', 'k', 'w', '!'}, 8);
    check(stun_reply(req, 28, &v4, out, sizeof out) == sizeof want4, "request with an attribute");
}

static void test_nat(void) {
    // The protocol description's example: type 101, port 50000, 192.0.2.1 from 192.0.2.7:61000.
    const uint8_t req[16] = {0, 0, 0, 0x65, 0, 0, 0xc3, 0x50, 0xc0, 0, 2, 1, 0, 0, 0, 0};
    const uint8_t want[16] = {0, 0, 0, 0x65, 0, 0, 0xee, 0x48, 0xc0, 0, 2, 7, 0, 0, 0, 0};
    const uint8_t self[4] = {192, 0, 2, 9};
    uint8_t out[32];
    SrvPeer from = {0, {192, 0, 2, 7}, 61000};
    check(nat_reply(req, 16, &from, self, out, sizeof out) == 16 && !memcmp(out, want, 16), "NAT check reply");
    SrvPeer local = {0, {127, 0, 0, 1}, 61000};
    check(nat_reply(req, 16, &local, self, out, sizeof out) == 16 && !memcmp(out + 8, self, 4),
          "NAT check from loopback reports the server's address");
    check(nat_reply(req, 16, &local, NULL, out, sizeof out) == 16 && out[8] == 127, "loopback kept without one");
    SrvPeer mapped = {1, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 192, 0, 2, 7}, 61000};
    check(nat_reply(req, 16, &mapped, self, out, sizeof out) == 16 && !memcmp(out, want, 16), "IPv4-mapped sender");
    check(nat_reply(req, 15, &from, self, out, sizeof out) == 0, "NAT check: other length ignored");
    uint8_t other[16];
    memcpy(other, req, 16);
    other[3] = 104;
    check(nat_reply(other, 16, &from, self, out, sizeof out) == 0, "NAT check: other type ignored");
    other[3] = 103;
    check(nat_reply(other, 16, &from, self, out, sizeof out) == 16 && out[3] == 103, "NAT check type 103");
}

// ---- the value tree
static SrvVal map_val(SrvMap m) { return (SrvVal){.kind = V_MAP, .u.map = m}; }

static int path_is(const char *s, int n, const char *a, const char *b) {
    FieldPath fp;
    return fpath_parse(&fp, s, strlen(s)) && fp.n == n && !strcmp(fp.seg[0], a) && (n < 2 || !strcmp(fp.seg[1], b));
}

static int bad_path(const char *s) {
    FieldPath fp;
    return !fpath_parse(&fp, s, strlen(s));
}

static void test_values(void) {
    PbOut o = {0};
    pb_int32(&o, 1, -7);   // int32 in, int64 (case 3) out
    SrvVal v;
    check(val_decode(&v, o.buf, o.len) && v.kind == V_INT && v.u.i == -7, "int32 decoded");
    PbOut back = {0}, want = {0};
    val_encode(&back, 2, &v);
    size_t m = pb_begin(&want, 2);
    pb_int64(&want, 3, -7);
    pb_end(&want, m);
    check(back.len == want.len && !memcmp(back.buf, want.buf, back.len), "int32 written back as int64");
    pb_free(&o), pb_free(&back), pb_free(&want), val_free(&v);

    SrvVal f = val_bool(0);   // a false bool is still written (its case is set)
    val_encode(&o, 2, &f);
    check(o.len == 4 && o.buf[2] == 0x10 && o.buf[3] == 0, "false written");
    pb_free(&o);

    SrvMap a = {0}, b = {0}, c;
    map_set(&a, "x", val_int(1));
    map_set(&a, "y", val_str("s"));
    map_set(&b, "y", val_str("s"));
    map_set(&b, "x", val_int(1));
    check(map_equal(&a, &b), "maps equal whatever their order");
    map_set(&b, "x", val_f64(1.0));
    check(!map_equal(&a, &b), "an int is not a double");
    check(map_copy(&c, &a) && map_equal(&a, &c), "copy");
    map_encode(&o, 2, &a);
    SrvMap d;
    PbField fld;
    check(pb_find(o.buf, o.len, 2, &fld) == 1 && map_decode(&d, fld.data, fld.len) && map_equal(&a, &d),
          "map round trip");
    map_free(&a), map_free(&b), map_free(&c), map_free(&d), pb_free(&o);

    for (int i = 0; i <= VAL_ITEMS; i++) {   // one entry more than a map may hold
        char key[16];
        snprintf(key, sizeof key, "k%d", i);
        size_t e = pb_begin(&o, 1);
        pb_str(&o, 1, key);
        pb_end(&o, e);
    }
    check(!map_decode(&d, o.buf, o.len) && d.n == VAL_ITEMS, "a map past VAL_ITEMS entries is refused");
    map_free(&d), pb_free(&o);

    check(path_is("a.b", 2, "a", "b"), "path a.b");
    check(path_is("`prp`.`user_name`", 2, "prp", "user_name"), "backtick-quoted path");
    check(path_is("`a.b`.c", 2, "a.b", "c"), "a quoted dot is part of the name");
    check(path_is("`a\\`b`", 1, "a`b", NULL), "escaped backtick");
    check(bad_path("") && bad_path("a..b") && bad_path("a.") && bad_path(".a") && bad_path("`a") && bad_path("`a`b"),
          "malformed paths");

    // Update with a mask: prp.user_name replaced, cp deleted (absent from the given fields), other fields kept; a value
    // in the way of a path becomes a map.
    SrvMap doc = {0}, prp = {0}, given = {0}, gprp = {0};
    map_set(&prp, "user_name", val_str("old"));
    map_set(&prp, "other", val_int(1));
    map_set(&doc, "prp", map_val(prp));
    map_set(&doc, "cp", val_bool(1));
    map_set(&doc, "keep", val_int(9));
    map_set(&doc, "n", val_int(5));
    map_set(&gprp, "user_name", val_str("new"));
    map_set(&given, "prp", map_val(gprp));
    map_set(&given, "n", map_val((SrvMap){0}));
    SrvMap inner = {0};
    map_set(&inner, "deep", val_int(3));
    map_set(&map_get(&given, "n")->u.map, "m", map_val(inner));
    FieldPath paths[3];
    fpath_parse(&paths[0], "`prp`.`user_name`", 17);
    fpath_parse(&paths[1], "cp", 2);
    fpath_parse(&paths[2], "n.m", 3);
    check(map_update(&doc, &given, paths, 3), "update");
    const SrvVal *un = map_path_get(&doc, &paths[0]);
    check(un && un->kind == V_STR && !strcmp(un->u.s.p, "new"), "masked path replaced");
    check(map_int(&map_get(&doc, "prp")->u.map, "other", 0) == 1, "sibling kept");
    check(!map_get(&doc, "cp") && map_int(&doc, "keep", 0) == 9, "absent path deleted, unnamed field kept");
    const SrvVal *nm = map_path_get(&doc, &paths[2]);
    check(map_get(&doc, "n")->kind == V_MAP && nm && nm->kind == V_MAP && map_int(&nm->u.map, "deep", 0) == 3,
          "a value in the way becomes a map");
    map_free(&doc), map_free(&given);
}

// ---- Gamesync writes
static void doc_msg(PbOut *o, uint32_t num, const char *path, const SrvMap *fields) {
    size_t d = pb_begin(o, num);
    pb_str(o, 1, path);
    map_encode(o, 2, fields);
    pb_end(o, d);
}

static int run(SrvDocs *d, const PbOut *w, const SrvWriteCtx *ctx, PbOut *res) {
    pb_free(res);
    return docs_write(d, w->buf, w->len, ctx, res);
}

// A FieldTransform: kind 2 (server time) has no operand.
static void ft(PbOut *o, const char *field, int kind, SrvVal operand) {
    size_t m = pb_begin(o, 2);
    pb_str(o, 1, field);
    if (kind == 2) pb_varint(o, 2, 1);
    else val_encode(o, (uint32_t)kind, &operand);
    pb_end(o, m);
    val_free(&operand);
}

// TransformResults' values from a WriteResult list holding one result.
static int transform_results(const PbOut *res, SrvVal *out, int max) {
    PbField wr, tr, f;
    if (pb_find(res->buf, res->len, 1, &wr) != 1 || pb_find(wr.data, wr.len, 2, &tr) != 1) return -1;
    PbIn in = pb_in(tr.data, tr.len);
    int n = 0;
    while (pb_next(&in, &f) > 0 && n < max)
        if (f.num == 1) val_decode(&out[n++], f.data, f.len);
    return n;
}

static int64_t station;
static int64_t next_station(void *ctx) {
    (void)ctx;
    return station;
}

static void test_docs(void) {
    SrvDocs d;
    docs_init(&d, 1000);
    SrvWriteCtx ctx = {2000, next_station, NULL};
    PbOut w = {0}, res = {0};
    SrvMap f = {0};
    map_set(&f, "k", val_int(1));
    size_t s = pb_begin(&w, 2);   // Set
    doc_msg(&w, 1, "docs/a/b", &f);
    pb_end(&w, s);
    docs_begin(&d);
    check(run(&d, &w, &ctx, &res) == ST_OK && res.len == 4 && docs_end(&d, 2000) == ST_OK, "set written");
    check(d.nchanged == 1 && !strcmp(d.changed[0], "a/b") && docs_find(&d, "a/b")->updated == 2000, "set changed");
    docs_changes_clear(&d);
    docs_begin(&d);
    run(&d, &w, &ctx, &res);
    docs_end(&d, 3000);
    check(d.nchanged == 0 && docs_find(&d, "a/b")->updated == 2000, "the same write again is no change");
    PbOut enc = {0};
    docs_encode(&enc, 1, &d, docs_find(&d, "a/b"));
    PbField de, name, ts, sec;
    check(pb_find(enc.buf, enc.len, 1, &de) == 1 && pb_find(de.data, de.len, 1, &name) == 1 && name.len == 8 &&
              !memcmp(name.data, "docs/a/b", 8) && pb_find(de.data, de.len, 3, &ts) == 1 &&
              pb_find(ts.data, ts.len, 1, &sec) == 1 && sec.u == 1000,
          "document: name with docs/, created = the session's time");
    pb_free(&enc), pb_free(&w), map_free(&f);

    // Update with a mask on a missing document: it becomes the given fields; then a masked update keeps the rest.
    map_set(&f, "x", val_int(1));
    map_set(&f, "y", val_int(2));
    size_t u = pb_begin(&w, 1);
    doc_msg(&w, 1, "docs/u", &f);
    size_t mk = pb_begin(&w, 2);
    pb_str(&w, 1, "x");
    pb_end(&w, mk);
    pb_end(&w, u);
    docs_begin(&d);
    run(&d, &w, &ctx, &res);
    docs_end(&d, 2000);
    check(map_int(&docs_find(&d, "u")->fields, "y", 0) == 2, "masked update of a missing document: all fields");
    pb_free(&w), map_free(&f);
    map_set(&f, "x", val_int(5));
    u = pb_begin(&w, 1);
    doc_msg(&w, 1, "docs/u", &f);
    mk = pb_begin(&w, 2);
    pb_str(&w, 1, "x");
    pb_end(&w, mk);
    pb_end(&w, u);
    docs_begin(&d);
    run(&d, &w, &ctx, &res);
    docs_end(&d, 2000);
    check(map_int(&docs_find(&d, "u")->fields, "x", 0) == 5 && map_int(&docs_find(&d, "u")->fields, "y", 0) == 2,
          "masked update keeps the other fields");
    pb_free(&w), map_free(&f);
    docs_changes_clear(&d);

    // upcsid: load the counter, increment, store it (the override gives the member's station id first): one value.
    size_t t = pb_begin(&w, 3);
    pb_str(&w, 1, "docs/st");
    ft(&w, "upcsid", 6, val_str("__upcsidn"));
    ft(&w, "upcsid", 3, val_int(1));
    ft(&w, "`upcsid`", 7, val_str("__upcsidn"));
    ft(&w, "t", 2, (SrvVal){0});
    ft(&w, "lo", 5, val_int(5));
    ft(&w, "hi", 4, val_int(7));
    ft(&w, "hi", 3, val_str("ignored"));
    pb_end(&w, t);
    SrvVal got[8];
    station = 20001;
    docs_begin(&d);
    check(run(&d, &w, &ctx, &res) == ST_OK && docs_end(&d, 2000) == ST_OK, "transform written");
    int n = transform_results(&res, got, 8);
    check(n == 4, "one result per distinct field");
    check(n == 4 && got[0].kind == V_INT && got[0].u.i == 20001, "upcsid override");
    check(n == 4 && got[1].kind == V_INT && got[1].u.i == 2000, "server time as an integer");
    check(n == 4 && got[2].kind == V_INT && got[2].u.i == 0, "minimum of a new field never rises above 0");
    check(n == 4 && got[3].kind == V_INT && got[3].u.i == 7, "maximum; a string increment is ignored");
    for (int i = 0; i < n; i++) val_free(&got[i]);
    station = 20002;   // the next member: the counter follows
    docs_begin(&d);
    run(&d, &w, &ctx, &res);
    docs_end(&d, 2000);
    n = transform_results(&res, got, 8);
    check(n == 4 && got[0].u.i == 20002 && d.ncounters == 1 && d.counters[0].v == 20002, "counter stored");
    for (int i = 0; i < n; i++) val_free(&got[i]);
    station = 0;   // not a member: the plain counter (load 20002, + 1, store)
    docs_begin(&d);
    run(&d, &w, &ctx, &res);
    docs_end(&d, 2000);
    n = transform_results(&res, got, 8);
    check(n == 4 && got[0].u.i == 20003, "no override without a station id");
    for (int i = 0; i < n; i++) val_free(&got[i]);
    pb_free(&w);
    docs_changes_clear(&d);

    // Delete, then a batch past the budget is undone whole.
    size_t del = pb_begin(&w, 4);
    pb_str(&w, 1, "docs/a/b");
    pb_end(&w, del);
    docs_begin(&d);
    run(&d, &w, &ctx, &res);
    docs_end(&d, 2000);
    check(!docs_find(&d, "a/b") && d.nchanged == 1, "deleted, a change");
    pb_free(&w);
    docs_changes_clear(&d);
    static uint8_t big[DOCS_BYTES];
    memset(big, 'z', sizeof big);
    map_set(&f, "big", val_bytes(big, sizeof big));
    docs_begin(&d);
    SrvMap small = {0};
    map_set(&small, "x", val_int(99));
    docs_put(&d, "u", &small);
    docs_put(&d, "huge", &f);
    check(docs_end(&d, 2000) == ST_RESOURCE_EXHAUSTED && !docs_find(&d, "huge") &&
              map_int(&docs_find(&d, "u")->fields, "x", 0) == 5 && !d.nchanged,
          "past the budget: undone");
    check(docs_write(&d, (const uint8_t *)"\x2a\x00", 2, &ctx, &res) == ST_INVALID_ARGUMENT, "a write of no kind");
    pb_free(&res);
    docs_free(&d);
}

// ---- the referee
static void put(SrvDocs *d, const char *path, SrvMap f) { docs_put(d, path, &f); }

static SrvVal ints(int n, const int64_t *v) {
    SrvVal l = {.kind = V_LIST};
    for (int i = 0; i < n; i++) list_add(&l, val_int(v[i]));
    return l;
}

static double rating_of(const SrvDoc *res, const char *ussid) {
    const SrvVal *r = map_get(&res->fields, "ratings"), *v = r ? map_get(&r->u.map, ussid) : NULL;
    double x = -1;
    if (v && v->kind == V_F64) memcpy(&x, &v->u.f64, sizeof x);
    return x;
}

// Two players (ussid 1 and 2) in the session, both with a participation document and a rating of 1500.
static void seed_players(SrvDocs *d) {
    docs_init(d, 1000);
    docs_begin(d);
    for (int i = 1; i <= 2; i++) {
        char p[64];
        SrvMap us = {0}, pus = {0}, st = {0};
        map_set(&us, "ussid", val_int(i));
        snprintf(p, sizeof p, "__us/us-%016x", i);
        put(d, p, us);
        snprintf(p, sizeof p, "__pgn/g1/__pus/us-%016x", i);
        put(d, p, pus);
        map_set(&st, "rating_point_raw_value", val_int(1153138688));   // 1500.0f's bits
        snprintf(p, sizeof p, "PlayerStatic/%d", i);
        put(d, p, st);
    }
    docs_end(d, 1000);
    docs_changes_clear(d);
}

static void test_referee(void) {
    double r[2] = {1500, 1500}, rank[2] = {0, 1}, out[2];
    referee_elo(r, rank, 2, out);
    check(out[0] == 1512.0 && out[1] == 1488.0, "Elo: two players at 1500 give 1512 and 1488");

    SrvDocs d;
    SrvReferee ref;
    memset(&ref, 0, sizeof ref);
    seed_players(&d);
    SrvMap req = {0}, map = {0};
    const int64_t users[2] = {1, 2};
    map_set(&req, "users", ints(2, users));
    map_set(&map, "0", val_int(1));
    map_set(&map, "1", val_int(2));
    map_set(&map, "x", val_int(3));   // not a digit key: left out
    map_set(&req, "ussid_map", map_val(map));
    SrvMap req2;
    map_copy(&req2, &req);
    docs_begin(&d);
    put(&d, "@@RefereeRequest/1/Request/R1", req);
    docs_end(&d, 2000);
    referee_batch(&ref, &d, 0, 5000, 2000);
    const SrvDoc *res = NULL;
    for (int i = 0; i < d.n && !res; i++)
        if (!strncmp(d.doc[i].path, "@@RefereeResult/", 16)) res = &d.doc[i];
    check(res && !strcmp(map_str(&res->fields, "request_id"), "R1") && map_int(&res->fields, "status", -1) == 0 &&
              map_int(&res->fields, "user_session_count", -1) == 2 && strlen(res->path) == 16 + 36,
          "request accepted in the same batch: result with status 0 and a race id");
    check(d.nchanged == 2 && !strcmp(d.changed[1], res ? res->path : ""), "the result is a change of the batch");
    char uuid[40];
    snprintf(uuid, sizeof uuid, "%s", res ? res->path + 16 : "");
    docs_changes_clear(&d);
    docs_begin(&d);
    put(&d, "@@RefereeRequest/2/Request/R1", req2);   // the other player's request for the same race
    docs_end(&d, 2000);
    referee_batch(&ref, &d, 0, 5000, 2000);
    int results = 0;
    for (int i = 0; i < d.n; i++) results += !strncmp(d.doc[i].path, "@@RefereeResult/", 16);
    check(results == 1, "a request id seen already starts no race");
    docs_changes_clear(&d);

    // Player 2 (index 1) reports: index 1 first, index 0 second. Both are placed: the race closes.
    SrvMap rep = {0};
    const int64_t order[2] = {1, 0}, ranks[2] = {1, 2};
    map_set(&rep, "result", ints(2, order));
    map_set(&rep, "result_order", ints(2, ranks));
    char path[128];
    snprintf(path, sizeof path, "@@RefereeReport/%s/User/2", uuid);
    docs_begin(&d);
    put(&d, path, rep);
    docs_end(&d, 2100);
    referee_batch(&ref, &d, 0, 6000, 2100);
    snprintf(path, sizeof path, "@@RefereeResult/%s", uuid);
    res = docs_find(&d, path);
    const SrvVal *final = res ? map_get(&res->fields, "result") : NULL;
    check(res && map_int(&res->fields, "status", -1) == 1 && list_int(final, 0, -1) == 1 &&
              list_int(final, 1, -1) == 0 && map_int(&res->fields, "user_session_count", -1) == 2,
          "closed: status 1, final order, players present");
    check(res && rating_of(res, "2") == 1512.0 && rating_of(res, "1") == 1488.0, "ratings as doubles");
    docs_free(&d);

    // Player 2 never reports: closed 120 s after the first report, the unreported player after the reported one.
    memset(&ref, 0, sizeof ref);
    seed_players(&d);
    SrvMap req3 = {0}, rep3 = {0};
    map_set(&req3, "users", ints(2, users));
    docs_begin(&d);
    put(&d, "@@RefereeRequest/1/Request/R2", req3);
    docs_end(&d, 2000);
    referee_batch(&ref, &d, 0, 5000, 2000);
    snprintf(uuid, sizeof uuid, "%s", ref.races[0].uuid);
    const int64_t first[1] = {0}, one[1] = {1};
    map_set(&rep3, "result", ints(1, first));
    map_set(&rep3, "result_order", ints(1, one));
    snprintf(path, sizeof path, "@@RefereeReport/%s/User/1", uuid);
    docs_begin(&d);
    put(&d, path, rep3);
    docs_end(&d, 2000);
    referee_batch(&ref, &d, 0, 10000, 2000);
    check(ref.races[0].open, "a present player not placed holds the race open");
    referee_tick(&ref, &d, 10000 + RACE_GRACE_MS - 1, 2120);
    check(ref.races[0].open, "still open before 120 s");
    referee_tick(&ref, &d, 10000 + RACE_GRACE_MS, 2120);
    snprintf(path, sizeof path, "@@RefereeResult/%s", uuid);
    res = docs_find(&d, path);
    check(!ref.races[0].open && res && map_int(&res->fields, "status", -1) == 1 &&
              rating_of(res, "1") == 1512.0 && rating_of(res, "2") == 1488.0,
          "forced close after 120 s");
    docs_free(&d);
}

int main(void) {
    test_pb();
    test_mapvalue();
    test_stun();
    test_nat();
    test_values();
    test_docs();
    test_referee();
    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
