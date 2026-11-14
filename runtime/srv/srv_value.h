// The game's attribute values (MapValue) as a tree the server can change: Gamesync documents (srv_docs.c), ticket and
// session attributes (srv_session.c), the referee's results (srv_referee.c). Decoding normalises the way the game reads
// values back (the protocol description, "MapValue"): int32, int64 and a Timestamp's seconds become one integer kind
// (sent as case 3), both string cases one string kind (case 7), fixed32 and fixed64 stay bit-exact (MKW's doubles
// travel in fixed64 and must not be touched), bool, bytes, lists and maps keep their kind. Field paths ("a.b",
// "`prp`.`user_name`") address values inside maps, for Update writes with a field mask.
// No platform code: tests/c/t_srv.c builds it alone.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "srv_pb.h"

enum { V_NONE, V_INT, V_BOOL, V_F32, V_F64, V_STR, V_BYTES, V_LIST, V_MAP };
// What a peer can make: values deeper than VAL_DEPTH are cut when decoded, a map or list of more than VAL_ITEMS entries
// is refused (keys are looked up one by one: a 1 MiB map of tiny entries would cost seconds), paths have at most
// FP_SEGS segments.
enum { VAL_DEPTH = 16, VAL_ITEMS = 4096, FP_SEGS = 16, FP_LEN = 512, KEY_MAX = 256 };

typedef struct SrvVal SrvVal;
typedef struct SrvEntry SrvEntry;

typedef struct {
    SrvEntry *e;   // insertion order (the order values go back out in)
    int n, cap;
} SrvMap;

typedef struct {
    SrvVal *v;
    int n, cap;
} SrvList;

struct SrvVal {
    int kind;   // V_*
    union {
        int64_t i;      // V_INT; V_BOOL 0 or 1
        uint32_t f32;   // V_F32, the bits as sent
        uint64_t f64;   // V_F64, the bits as sent
        struct {
            char *p;    // NUL after the n bytes (strings may still hold NULs of their own)
            size_t n;
        } s;            // V_STR, V_BYTES
        SrvList list;
        SrvMap map;
    } u;
};

struct SrvEntry {
    char *key;
    SrvVal v;
};

// Values the server makes. val_str and val_bytes copy; on a failed allocation the value is V_NONE.
static inline SrvVal val_int(int64_t i) { return (SrvVal){.kind = V_INT, .u.i = i}; }
static inline SrvVal val_bool(int b) { return (SrvVal){.kind = V_BOOL, .u.i = b ? 1 : 0}; }
SrvVal val_f64(double d);
SrvVal val_str(const char *s);
SrvVal val_bytes(const void *p, size_t n);

void val_free(SrvVal *v);
void map_free(SrvMap *m);
// Deep copies; 0 when memory ran out (dst then holds nothing to free).
int val_copy(SrvVal *dst, const SrvVal *src);
int map_copy(SrvMap *dst, const SrvMap *src);
// Same kind and value; maps compare as sets of keys (the order entries arrived in does not count), lists in order.
int val_equal(const SrvVal *a, const SrvVal *b);
int map_equal(const SrvMap *a, const SrvMap *b);

// A MapValue message, a Map message ({1: repeated {1: key, 2: MapValue}}). 0 when the bytes are not one (or memory ran
// out): the result then holds what was read before. A later entry with the same key replaces the earlier one.
int val_decode(SrvVal *v, const uint8_t *msg, size_t len);
int map_decode(SrvMap *m, const uint8_t *msg, size_t len);
// As field num: a MapValue (an empty message for V_NONE), a Map.
void val_encode(PbOut *o, uint32_t num, const SrvVal *v);
void map_encode(PbOut *o, uint32_t num, const SrvMap *m);

SrvVal *map_get(const SrvMap *m, const char *key);
// v under key: an old value there is freed, a new key goes at the end. Takes v (freed when memory ran out: 0).
int map_set(SrvMap *m, const char *key, SrvVal v);
void map_del(SrvMap *m, const char *key);
// An integer (V_INT) or a string (V_STR) under key; fallback / NULL otherwise.
int64_t map_int(const SrvMap *m, const char *key, int64_t fallback);
const char *map_str(const SrvMap *m, const char *key);
// A list's item i as an integer, fallback when it is not one.
int64_t list_int(const SrvVal *list, int i, int64_t fallback);
int list_add(SrvVal *list, SrvVal v);   // takes v; 0 when memory ran out

// A field path: segments split at '.', a segment quoted with backticks keeps its dots (`a.b`), \` and \\ inside the
// quotes are a backtick and a backslash. 0 when malformed (an empty segment, an unclosed quote, too long).
typedef struct {
    int n;
    const char *seg[FP_SEGS];
    char buf[FP_LEN];
} FieldPath;
int fpath_parse(FieldPath *fp, const char *s, size_t n);

const SrvVal *map_path_get(const SrvMap *m, const FieldPath *fp);
// A copy of v at the path, maps made on the way (a value that is not a map in the way is replaced by one); 0 when
// memory ran out.
int map_path_set(SrvMap *m, const FieldPath *fp, const SrvVal *v);
void map_path_del(SrvMap *m, const FieldPath *fp);
// An Update with a field mask (the protocol description, "Write kinds"): for each path, the value there in given is
// copied to the same path in doc, or doc's value there is deleted when given has none. Other fields stay. 0 when
// memory ran out.
int map_update(SrvMap *doc, const SrvMap *given, const FieldPath *paths, int npaths);
