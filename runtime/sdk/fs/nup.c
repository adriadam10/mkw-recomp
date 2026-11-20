// Switch 2 .nup package reader (nup.h). Layout, all little-endian (reverse-engineered from dumps; no public spec):
//   0x00  "NUP\x01"; u32 @0x08 = offset of the table of contents (TOC)
//   TOC   "TOC\x01", u32 title count, u64 dir count, u64 file count, u64 name-pool offset (from the TOC),
//         u64 name-pool size; the whole table runs from the TOC to the end of the name pool
//   TOC+0x30: titles, 0x40 bytes each: u64 title id, u32 root dir, u32 exefs dir, u32 romfs dir, u32 version
//   then dirs  (24 bytes): u32 name offset, u32 name length, u32 first child, u32 child count, 8 unused
//   then files (80 bytes): u32 name offset, u32 name length, u64 size, u64 absolute offset, u64 stored size,
//                          32-byte hash, u8 codec @0x40 (0 raw, 1 one zstd frame), 15 unused
//   then a u32 child list: an index below the dir count is a dir, else a file (index - dir count)
// Of Mario Kart World 1.0.0's 205 665 files, 35 081 are zstd frames (2.4 GB once decoded, the largest 78 MB); the rest
// are stored raw, and the RomFS reads them in place at their offset.
// Titles are layered (nup.h): a package can hold the base game and an update as two titles, the update's RomFS only
// what it adds or changes. Eglantine's --pack-nup writes MKW 1.0.0 + 1.8.0 so: 205 660 base files, 8 224 in the update.
#include <stdlib.h>
#include <string.h>

#include <zstd.h>

#include "host.h"
#include "nup.h"

#define TOC_MAX (256u << 20)   // the table of contents read whole; MKW's is 24 MiB
#define CHUNK (8u << 20)

struct Nup {
    char *path;
    PlatFile *f;
    uint64_t file_size;
    PackEntry *part[2];
    size_t count[2];
    Nup *next;
};

static PlatMutex lock = PLAT_MUTEX_INIT;
static Nup *opened;

static uint32_t u32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t u64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }

static int read_all(PlatFile *f, void *buf, uint64_t n, uint64_t off) {
    for (uint64_t done = 0; done < n;) {
        uint32_t want = n - done > CHUNK ? CHUNK : (uint32_t)(n - done);
        int64_t got = plat_file_pread(f, (uint8_t *)buf + done, want, off + done);
        if (got <= 0) return 0;
        done += (uint64_t)got;
    }
    return 1;
}

int nup_is(const char *path) {
    if (!path || !plat_file_exists(path)) return 0;
    PlatFile *f = plat_file_open(path);
    uint8_t m[4] = {0};
    int ok = f && plat_file_pread(f, m, 4, 0) == 4 && !memcmp(m, "NUP\x01", 4);
    if (f) plat_file_close(f);
    return ok;
}

// The parse: one walk of a part's directory tree into a flat list of its files.
typedef struct {
    const uint8_t *t;                  // the TOC
    uint64_t size, nd, nf, dirs, files, children, pool, pool_size, file_size;
    PackEntry *out;
    size_t n, cap;
    int depth;
    const char *err;
} Walk;

static int name_at(Walk *w, uint32_t off, uint32_t len, const char **s) {
    if ((uint64_t)off + len > w->pool_size || !len || memchr(w->t + w->pool + off, '/', len) ||
        memchr(w->t + w->pool + off, 0, len))
        return 0;
    *s = (const char *)w->t + w->pool + off;
    return 1;
}

static void walk(Walk *w, uint64_t dir, const char *prefix) {
    if (w->err) return;
    if (dir >= w->nd || ++w->depth > 64) { w->err = "bad directory"; return; }
    const uint8_t *d = w->t + w->dirs + dir * 24;
    uint64_t first = u32(d + 8), count = u32(d + 12);
    if (first + count > (w->size - w->children) / 4) { w->err = "child list out of range"; return; }
    for (uint64_t k = first; k < first + count && !w->err; k++) {
        uint64_t c = u32(w->t + w->children + k * 4);
        int is_dir = c < w->nd;
        const uint8_t *r = is_dir ? w->t + w->dirs + c * 24 : w->t + w->files + (c - w->nd) * 80;
        if (!is_dir && c - w->nd >= w->nf) { w->err = "file index out of range"; return; }
        const char *nm;
        if (!name_at(w, u32(r), u32(r + 4), &nm)) { w->err = "bad name"; return; }
        size_t pl = strlen(prefix), nl = u32(r + 4);
        char *path = malloc(pl + nl + 2);
        if (!path) { w->err = "memory"; return; }
        memcpy(path, prefix, pl);
        if (pl) path[pl++] = '/';
        memcpy(path + pl, nm, nl);
        path[pl + nl] = 0;
        if (is_dir) {
            walk(w, c, path);
            free(path);
            continue;
        }
        PackEntry e = {path, u64(r + 16), u64(r + 8), u64(r + 24), r[0x40] == 1 ? PACK_WHOLE : PACK_RAW, 0};
        if (r[0x40] > 1) w->err = "unknown codec";
        else if (e.kind == PACK_RAW && e.stored != e.size) w->err = "raw file with a stored size unlike its size";
        else if (e.offset > w->file_size || e.stored > w->file_size - e.offset) w->err = "file past the end";
        if (w->err) { free(path); return; }
        if (w->n == w->cap) {
            w->cap = w->cap ? w->cap * 2 : 4096;
            PackEntry *g = realloc(w->out, w->cap * sizeof *g);
            if (!g) { free(path); w->err = "memory"; return; }
            w->out = g;
        }
        w->out[w->n++] = e;
    }
    w->depth--;
}

static int entry_order(const void *a, const void *b) {
    return strcmp(((const PackEntry *)a)->name, ((const PackEntry *)b)->name);
}

// A base game's and its update's title ids differ only in the low 12 bits (0x800 on the console), a DLC's is higher
// (base + 0x1000 and up): the lowest id, so masked, names the game.
#define GAME_ID(tid) ((tid) & ~0xFFFull)

typedef struct {
    const uint8_t *rec;   // the title's 0x40-byte record
    uint32_t version;
    uint32_t index;       // in the table, to break a tie of versions
} Title;

static int title_order(const void *a, const void *b) {
    const Title *x = a, *y = b;
    if (x->version != y->version) return x->version < y->version ? -1 : 1;
    return x->index < y->index ? -1 : x->index > y->index;
}

// Sorts by name and keeps one file per name: the newest title's. w->out holds the titles' files oldest title first, so
// the one pushed last wins; qsort is not stable, so each entry's position goes in its id (unused by .nup rows) first.
static int layer_order(const void *a, const void *b) {
    int c = entry_order(a, b);
    if (c) return c;
    uint32_t x = ((const PackEntry *)a)->id, y = ((const PackEntry *)b)->id;
    return x > y ? -1 : x < y;
}

static void layer(Walk *w) {
    for (size_t i = 0; i < w->n; i++) w->out[i].id = (uint32_t)i;
    qsort(w->out, w->n, sizeof *w->out, layer_order);
    size_t k = 0;
    for (size_t i = 0; i < w->n; i++) {
        if (k && !strcmp(w->out[k - 1].name, w->out[i].name)) {
            free(w->out[i].name);
            continue;
        }
        w->out[k] = w->out[i];
        w->out[k++].id = 0;
    }
    w->n = k;
}

static Nup *parse(const char *path) {
    PlatFile *f = plat_file_open(path);
    PlatFileId id;
    uint8_t h[0x30];
    uint8_t *t = NULL;
    const char *err = NULL;
    Nup *n = NULL;
    if (!f || !plat_file_id(f, &id)) { rt_log("NUP %s: cannot open", path); goto out; }
    if (plat_file_pread(f, h, 12, 0) != 12 || memcmp(h, "NUP\x01", 4)) { err = "no NUP magic"; goto bad; }
    uint64_t toc = u32(h + 8);
    if (plat_file_pread(f, h, 0x30, toc) != 0x30 || memcmp(h, "TOC\x01", 4)) { err = "no TOC"; goto bad; }
    uint64_t nt = u32(h + 4), nd = u64(h + 8), nf = u64(h + 16), pool = u64(h + 24), pool_size = u64(h + 32);
    uint64_t size = pool + pool_size, dirs = 0x30 + nt * 0x40, files = dirs + nd * 24, children = files + nf * 80;
    if (!nt || nt > 4096 || nd > TOC_MAX || nf > TOC_MAX || pool > TOC_MAX || pool_size > TOC_MAX || size > TOC_MAX ||
        children > pool || toc + size > id.size) {
        err = "table of contents out of range";
        goto bad;
    }
    t = malloc(size);
    if (!t || !read_all(f, t, size, toc)) { err = "table of contents unreadable"; goto bad; }
    // The game's titles (DLC, a higher id, left out), oldest first.
    uint64_t game = UINT64_MAX;
    for (uint32_t i = 0; i < nt; i++) {
        uint64_t g = GAME_ID(u64(t + 0x30 + (uint64_t)i * 0x40));
        if (g < game) game = g;
    }
    Title *ts = malloc(nt * sizeof *ts);
    size_t used = 0;
    if (!ts) { err = "memory"; goto bad; }
    for (uint32_t i = 0; i < nt; i++) {
        const uint8_t *r = t + 0x30 + (uint64_t)i * 0x40;
        if (GAME_ID(u64(r)) == game) ts[used++] = (Title){r, u32(r + 20), i};
    }
    qsort(ts, used, sizeof *ts, title_order);
    const uint8_t *title = ts[used - 1].rec;
    n = calloc(1, sizeof *n);
    if (!n || !(n->path = strdup(path))) { free(ts); err = "memory"; goto bad; }
    for (int p = 0; p < 2; p++) {
        Walk w = {t, size, nd, nf, dirs, files, children, pool, pool_size, id.size, NULL, 0, 0, 0, NULL};
        // ExeFS: the newest title's whole (an update replaces every module; 1.8.0's subsdk0 is another module than
        // 1.0.0's). RomFS: every title's, the newest's file kept where they share a path. A title without the part
        // (its dir index past the table) adds nothing.
        if (p == NUP_EXEFS) {
            for (size_t k = used; k-- > 0 && !w.n && !w.err;)
                if (u32(ts[k].rec + 12) < nd) walk(&w, u32(ts[k].rec + 12), "");
        } else {
            for (size_t k = 0; k < used && !w.err; k++)
                if (u32(ts[k].rec + 16) < nd) walk(&w, u32(ts[k].rec + 16), "");
        }
        if (w.err) {
            for (size_t i = 0; i < w.n; i++) free(w.out[i].name);
            free(w.out);
            free(ts);
            err = w.err;
            goto bad;
        }
        layer(&w);
        n->part[p] = w.out;
        n->count[p] = w.n;
    }
    n->f = f;
    n->file_size = id.size;
    rt_log("NUP %s: title %016llx v%u (%zu of %u titles layered), %zu exefs and %zu romfs files", path,
           (unsigned long long)u64(title), u32(title + 20), used, (unsigned)nt, n->count[0], n->count[1]);
    free(ts);
    free(t);
    return n;
bad:
    rt_log("NUP %s: %s", path, err);
    if (n) {
        for (int p = 0; p < 2; p++) {
            for (size_t i = 0; i < n->count[p]; i++) free(n->part[p][i].name);
            free(n->part[p]);
        }
        free(n->path);
        free(n);
    }
out:
    free(t);
    if (f) plat_file_close(f);
    return NULL;
}

Nup *nup_open(const char *path) {
    plat_mutex_lock(&lock);
    Nup *n = opened;
    while (n && strcmp(n->path, path)) n = n->next;
    if (!n && (n = parse(path))) {
        n->next = opened;
        opened = n;
    }
    plat_mutex_unlock(&lock);
    return n;
}

size_t nup_entries(Nup *n, int part, const PackEntry **out) {
    *out = n->part[part];
    return n->count[part];
}

PlatFile *nup_file(Nup *n) { return n->f; }

uint8_t *nup_read(Nup *n, const PackEntry *e) {
    uint8_t *dst = malloc(e->size ? e->size : 1);
    if (!dst) return NULL;
    if (e->kind == PACK_RAW) {
        if (read_all(n->f, dst, e->size, e->offset)) return dst;
        free(dst);
        return NULL;
    }
    // Streamed through a small buffer: the frames are written with zstd --long (windows past the 128 MiB default
    // limit), so the window limit is raised to the format's 2 GiB.
    ZSTD_DCtx *z = ZSTD_createDCtx();
    uint8_t *src = malloc(CHUNK);
    int ok = z && src && !ZSTD_isError(ZSTD_DCtx_setParameter(z, ZSTD_d_windowLogMax, 31));
    ZSTD_outBuffer o = {dst, e->size, 0};
    size_t r = 1;
    for (uint64_t done = 0; ok && done < e->stored;) {
        uint32_t want = e->stored - done > CHUNK ? CHUNK : (uint32_t)(e->stored - done);
        ok = read_all(n->f, src, want, e->offset + done);
        ZSTD_inBuffer in = {src, want, 0};
        while (ok && r && in.pos < in.size) {
            r = ZSTD_decompressStream(z, &o, &in);
            ok = !ZSTD_isError(r) && (o.pos < o.size || r == 0 || in.pos == in.size);
        }
        done += want;
        if (ok && r == 0 && (in.pos < in.size || done < e->stored)) ok = 0;   // bytes after the frame
    }
    ok = ok && r == 0 && o.pos == e->size;
    if (!ok) rt_log("NUP %s: cannot decompress %s", n->path, e->name);
    ZSTD_freeDCtx(z);
    free(src);
    if (ok) return dst;
    free(dst);
    return NULL;
}
