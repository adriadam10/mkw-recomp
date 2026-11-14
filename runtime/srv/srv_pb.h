// Protocol Buffers wire format, read and written by hand: the online server (srv_*.c) has no .proto files for the
// game's messages, only their field numbers, so it walks fields and writes them one by one. Encoding as in
// https://protobuf.dev/programming-guides/encoding/ : varint, 64-bit, length-delimited and 32-bit fields. Groups
// (wire types 3 and 4) are refused: proto3 has none and the game's services are proto3.
// Nothing here reaches outside the buffers it is given: the reader is the first code a remote peer's bytes meet.
#pragma once
#include <stddef.h>
#include <stdint.h>

enum { PB_VARINT = 0, PB_I64 = 1, PB_LEN = 2, PB_I32 = 5 };

typedef struct {
    const uint8_t *p, *end;
} PbIn;

typedef struct {
    uint32_t num;          // field number, 1 .. 2^29-1
    uint8_t wt;            // PB_*
    uint64_t u;            // PB_VARINT, PB_I64, PB_I32
    const uint8_t *data;   // PB_LEN
    size_t len;
} PbField;

static inline PbIn pb_in(const void *data, size_t len) {
    return (PbIn){(const uint8_t *)data, (const uint8_t *)data + len};
}
static inline PbIn pb_sub(const PbField *f) { return pb_in(f->data, f->len); }

// The next field: 1, 0 at the end of the message, -1 when the bytes are not a message (the rest is left unread).
int pb_next(PbIn *in, PbField *f);
// The last field `num` of a message (proto3: the last one wins), 1 found, 0 absent, -1 malformed.
int pb_find(const void *msg, size_t len, uint32_t num, PbField *f);

typedef struct {
    uint8_t *buf;
    size_t len, cap;
    int err;               // set once, by an allocation failure or past PB_OUT_MAX; later writes do nothing
} PbOut;

// What one reply may grow to: the largest message the server builds (a Gamesync listen batch) is far below.
#define PB_OUT_MAX (8u << 20)

void pb_free(PbOut *o);
void pb_varint(PbOut *o, uint32_t num, uint64_t v);
static inline void pb_int32(PbOut *o, uint32_t num, int32_t v) { pb_varint(o, num, (uint64_t)(int64_t)v); }
static inline void pb_int64(PbOut *o, uint32_t num, int64_t v) { pb_varint(o, num, (uint64_t)v); }
static inline void pb_bool(PbOut *o, uint32_t num, int v) { pb_varint(o, num, v ? 1 : 0); }
void pb_fixed32(PbOut *o, uint32_t num, uint32_t v);
void pb_fixed64(PbOut *o, uint32_t num, uint64_t v);
void pb_double(PbOut *o, uint32_t num, double v);
void pb_bytes(PbOut *o, uint32_t num, const void *data, size_t len);
void pb_str(PbOut *o, uint32_t num, const char *s);
void pb_raw(PbOut *o, const void *data, size_t len);   // bytes already encoded (fields kept as they were written)
// A nested message: pb_end(o, pb_begin(o, num)) around its fields. Nests to any depth; the length is written in its
// shortest form once the content is known.
size_t pb_begin(PbOut *o, uint32_t num);
void pb_end(PbOut *o, size_t mark);

// The game's attribute value (MapValue, a oneof) written back as field num the way the game reads it: int32 and int64 as
// int64 (3), a Timestamp as its seconds (3), both string cases as 7; bool, fixed32, fixed64 (doubles: bit-exact),
// bytes, lists and maps kept, lists and maps recursively. 0 when v is not a message (nothing usable written).
int pb_mapvalue(PbOut *o, uint32_t num, const uint8_t *v, size_t len);
