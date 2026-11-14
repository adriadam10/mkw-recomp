// A game session's Gamesync documents (the protocol description, "Gamesync"): paths with fields (srv_value.h), the
// writes the game sends (Update with a field mask, Set, Delete, Transform and its session counters) and the results it
// reads back. Writes run in batches: a batch keeps each path as it was before its first write, so at the end it knows
// which documents really changed (a write that leaves a document as it was is no change: MKW rewrites its lobby flags
// whenever that document changes, and sending unchanged writes back made a loop of ~40 writes a second) and can be
// undone whole when it would pass the session's budget.
// No platform code: tests/c/t_srv.c builds it alone. srv_gamesync.c sends the changes to the players listening.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "srv_pb.h"
#include "srv_value.h"

// A session's budget: documents and bytes (paths and encoded fields); all sessions together hold at most DOCS_ALL (in
// memory the tree takes a few times its encoded size: values of 2 bytes each make the worst case).
enum { DOCS_MAX = 4096, DOCS_BYTES = 4 << 20, DOCS_ALL = 64 << 20, DOC_PATH_MAX = 512, COUNTERS_MAX = 64 };

typedef struct {
    char *path;        // without "docs/"
    SrvMap fields;
    int64_t updated;   // Unix seconds of its last change
    size_t size;       // its part of the budget
} SrvDoc;

typedef struct {
    char name[64];
    int64_t v;
} SrvCounter;

typedef struct DocSnap DocSnap;

typedef struct {
    SrvDoc *doc;   // in the order they were made
    int n, cap;
    size_t bytes;
    int64_t created;   // the session's creation time: every document's "created", "updated" before any change
    SrvCounter counters[COUNTERS_MAX];   // the session counters of transforms 6 and 7
    int ncounters;
    int batch;   // a batch in progress: what each path was before it, the counters before it
    DocSnap *snap;
    int nsnap, csnap;
    SrvCounter saved[COUNTERS_MAX];
    int nsaved;
    char **changed;   // paths changed by the batches since docs_changes_clear, each once, in the order first changed
    int nchanged, cchanged;
} SrvDocs;

typedef struct {
    int64_t now;   // Unix seconds (transform 2)
    // The writer's station id for a "store counter" transform on the field upcsid (the protocol description, "upcsid
    // allocation"), 0 for none (the writer is not a member). NULL: none.
    int64_t (*upcsid)(void *ctx);
    void *ctx;
} SrvWriteCtx;

void docs_init(SrvDocs *d, int64_t created);
void docs_free(SrvDocs *d);
SrvDoc *docs_find(const SrvDocs *d, const char *path);

void docs_begin(SrvDocs *d);
// One Write message (Update, Set, Transform or Delete) in the batch (begun if it was not), its WriteResult added to
// results as field 1. ST_OK, ST_INVALID_ARGUMENT (malformed: nothing written), ST_RESOURCE_EXHAUSTED (no memory).
int docs_write(SrvDocs *d, const uint8_t *w, size_t len, const SrvWriteCtx *ctx, PbOut *results);
// The server's own writes in the batch: a document set to exactly these fields (taken), a document removed. 0 when
// memory ran out.
int docs_put(SrvDocs *d, const char *path, SrvMap *fields);
int docs_remove(SrvDocs *d, const char *path);
// Ends the batch: the documents that changed get `now` as their update time and join the changed list. Past the
// budget the whole batch is undone: ST_RESOURCE_EXHAUSTED.
int docs_end(SrvDocs *d, int64_t now);
void docs_rollback(SrvDocs *d);
void docs_changes_clear(SrvDocs *d);

// A Document message as field num (0: the message itself): {1: "docs/<path>", 2: fields, 3: created, 4: updated}.
void docs_encode(PbOut *o, uint32_t num, const SrvDocs *d, const SrvDoc *doc);
// A path in a message ("docs/<path>" or "<path>") as a C string; 0 when empty, too long or holding a NUL.
int docs_path(const uint8_t *p, size_t n, char out[DOC_PATH_MAX]);
