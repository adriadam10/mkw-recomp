// Precomputed BC7 of ASTC textures. Without native ASTC, vk_astc.c decodes each ASTC texture of 256x256 or more on the
// GPU and encodes it again as BC7 with mode 6 only; here the same texture comes ready, encoded offline with every mode
// bc7enc.cpp has (runtime/third_party/bc7enc, MIT).
//
// Only what the game loads: the whole romfs in BC7 would take 34.5 GiB, 17 GiB with zstd (23 545 textures; sample of 40).
// - The upload hashes the texture's storage (XXH3 of every byte, swizzled as NVN keeps it). When <dir>/<hash>.bc7.zst
//   exists it holds the BC7 blocks of every region of the upload, in its order (levels, then layers; slices of a 3D level
//   one after the other), zstd-compressed, and goes to the image as is.
// - Otherwise the upload goes on through the GPU encoder, and a writer thread stores the storage with its descriptor and
//   regions in <dir>/src/<hash>.astc.zst (MKW_TEXPRE_RECORD=0: not).
// - build/mkw --texpre encodes every src file into its .bc7.zst on all cores, with the runtime's own deswizzle and ASTC
//   decoder (bit-exact with the GPU decoder: --probe-astc), and deletes the src file; a .bc7.zst of one frame (written
//   before CHUNK) is split into frames.
// MKW_TEXPRE=1 (directory <exe dir>/texpre) or =<dir>; off by default: in the menus up to frame 1400 the uploads took
// the render thread 3 088 ms with it against 1 543 ms without (live21 / live22, 31/10/2026; tools/run/texms.sh), as the
// BC7 is 4 times the bytes of the ASTC and the GPU encoder costs the CPU little. What it gives is quality (every BC7
// mode). The directory listing is read once at the first upload.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zstd.h>
#define XXH_INLINE_ALL
#include <xxhash.h>

#include "cfg.h"
#include "host.h"
#include "nvn_hmap.h"
#include "tex.h"
#include "vk.h"
#include "platform/platform.h"

void bc7enc_c_init(void);
void bc7enc_c_block(void *out16, const void *rgba64);

#define SRC_MAGIC 0x3150585457544b4dull   // "MKTWXTP1"
typedef struct {
    uint64_t magic, size;   // size: bytes of storage after the regions
    nvn_texdesc d;
    uint32_t n, pad;
} SrcHead;

static struct {
    char dir[PLAT_PATH_MAX];
    int on, record, writeback;
    nvn_hmap have;          // hashes with a .bc7.zst
    nvn_hmap raw;           // keys with an uncompressed .bc7.raw / .bc1.raw (write-back)
    nvn_hmap seen;          // hashes recorded (or found) in this run
    nvn_hmap wb_seen;       // keys claimed by the write-back in this run (apart: a recorded source must not block it)
    nvn_hmap bad;           // keys whose raw file failed a read in this run: uploaded through the GPU encoder again
    nvn_hmap failed;        // tickets of queued reads that failed (vk_texpre_failed)
    PlatMutex lock;         // seen, and pending
    uint64_t pending;       // bytes held by writer threads
    uint64_t n_hit, n_rec, hit_bytes, wb_pending, n_wb, wb_bytes;
    int closing, wb_active;    // vk_texpre_shutdown: no new writer; writers still running
    PlatCond wb_cv;            // wb_active fell to 0
    volatile int32_t nbad, nfailed;   // entries in bad and in failed (read without the lock while 0)
} T = {.have = {.klen = 8}, .raw = {.klen = 8}, .seen = {.klen = 8}, .wb_seen = {.klen = 8}, .bad = {.klen = 8}, .failed = {.klen = 8}};
static PlatOnce t_once = PLAT_ONCE_INIT;

#define BC1_SALT 0x9e3779b97f4a7c15ull   // T.have and T.seen key of a hash's BC1 file

static int raw_cb(const char *name, void *ctx) {   // plat_file_list of the write-back's .raw files
    (void)ctx;
    char *end;
    uint64_t h = strtoull(name, &end, 16);
    if (end == name + 16 && !strcmp(end, ".bc1.raw")) h ^= BC1_SALT;
    if (end == name + 16 && (!strcmp(end, ".bc7.raw") || !strcmp(end, ".bc1.raw"))) nvn_hm_put(&T.raw, &h, (void *)1);
    return 1;
}

static int list_cb(const char *name, void *ctx) {   // plat_file_list: 1 goes on
    (void)ctx;
    char *end;
    uint64_t h = strtoull(name, &end, 16);
    if (end == name + 16 && !strcmp(end, ".bc1.zst")) h ^= BC1_SALT;   // BC1 blocks of the same storage (write-back)
    if (end == name + 16 && (!strcmp(end, ".bc7.zst") || !strcmp(end, ".bc1.zst"))) nvn_hm_put(&T.have, &h, (void *)1);
    return 1;
}

static void t_init(void *arg) {
    (void)arg;
    const char *e = cfg_get("MKW_TEXPRE");
    if (!e || !*e || !strcmp(e, "0")) return;
    if (strcmp(e, "1") && strlen(e) < sizeof T.dir) {
        strcpy(T.dir, e);
    } else {
        snprintf(T.dir, sizeof T.dir, "%s/texpre", cfg_game_dir());
    }
    const char *wb = cfg_get("MKW_TEXPRE_WRITEBACK"), *r = cfg_get("MKW_TEXPRE_RECORD");
    T.writeback = !(wb && !strcmp(wb, "0"));
    T.record = r && *r ? strcmp(r, "0") != 0 : !T.writeback;   // the source files only feed --texpre
    plat_file_list(T.dir, ".zst", list_cb, NULL);
    plat_file_list(T.dir, ".raw", raw_cb, NULL);
    if (T.writeback) plat_file_mkdir_p(T.dir);   // the source recorder made it (<dir>/src); the write-back writes at its top
    plat_cond_init(&T.wb_cv);
    T.on = 1;
    rt_log("vk: precomputed BC7/BC1 textures in %s: %u (recording new ones: %s, GPU write-back: %s)", T.dir, T.have.n + T.raw.n,
           T.record ? "on" : "off", T.writeback ? "on" : "off");
}

int vk_texpre_on(void) {
    plat_once(&t_once, t_init, NULL);
    return T.on;
}

uint64_t vk_texpre_hash(const void *p, uint64_t n) { return XXH3_64bits(p, (size_t)n); }

uint64_t vk_texpre_region_bytes(const VkTexpreRegion *r) {
    return (uint64_t)((r->w + 3) / 4) * ((r->h + 3) / 4) * 16 * r->slices;
}

static uint64_t bc_key(uint64_t h, int bc) { return bc == 2 ? h ^ BC1_SALT : h; }
static int is_bad(uint64_t k) {
    if (!__atomic_load_n(&T.nbad, __ATOMIC_ACQUIRE)) return 0;
    plat_mutex_lock(&T.lock);
    int b = nvn_hm_get(&T.bad, &k) != NULL;
    plat_mutex_unlock(&T.lock);
    return b;
}
int vk_texpre_has_bc(uint64_t h, int bc) {
    uint64_t k = bc_key(h, bc);
    return vk_texpre_on() && (nvn_hm_get(&T.raw, &k) || nvn_hm_get(&T.have, &k)) && !is_bad(k);
}
int vk_texpre_has(uint64_t h) { return vk_texpre_has_bc(h, 1); }

// A .bc7.zst is zstd frames of at most CHUNK bytes each, one after the other: one thread decodes 1 GiB in ~2.4 s (zstd
// -b on a 121 MiB file, 505 MB/s), and the menus load 1.2 GiB in one frame (live19), so the frames go to LOAD_THREADS.
#define CHUNK (4u << 20)
#define LOAD_THREADS 8
typedef struct { const uint8_t *src; size_t sn; uint8_t *dst; size_t dn; } Frame;
typedef struct { Frame *f; int n; volatile int32_t next, bad; } FrameJob;

static void frame_worker(void *arg) {
    plat_thread_background();   // off the frame's fast cores (MKW_FAST_CORES)
    FrameJob *j = arg;
    for (int i; (i = plat_inc32(&j->next) - 1) < j->n;)
        if (ZSTD_decompress(j->f[i].dst, j->f[i].dn, j->f[i].src, j->f[i].sn) != j->f[i].dn) plat_inc32(&j->bad);
}

// The frames of z[0, n) decoded into dst, which they must fill exactly; 1 on success.
static int decode_frames(const uint8_t *z, size_t n, uint8_t *dst, uint64_t size) {
    int cap = (int)(size / CHUNK) + 2, nf = 0;
    Frame *f = malloc(sizeof *f * (size_t)cap);
    uint64_t out = 0;
    for (size_t off = 0; f && off < n; off += f[nf++].sn) {
        size_t fs = ZSTD_findFrameCompressedSize(z + off, n - off);
        unsigned long long cs = ZSTD_isError(fs) ? ZSTD_CONTENTSIZE_ERROR : ZSTD_getFrameContentSize(z + off, fs);
        if (nf == cap || cs == ZSTD_CONTENTSIZE_ERROR || cs == ZSTD_CONTENTSIZE_UNKNOWN || out + cs > size) { nf = -1; break; }
        f[nf] = (Frame){z + off, fs, dst + out, (size_t)cs};
        out += cs;
    }
    FrameJob j = {f, nf};
    int ok = f && nf > 0 && out == size;
    if (ok) {
        PlatThread *th[LOAD_THREADS - 1];
        int nt = nf - 1 < LOAD_THREADS - 1 ? nf - 1 : LOAD_THREADS - 1;
        for (int i = 0; i < nt; i++) th[i] = plat_thread_new(frame_worker, &j, "texpre-load");
        frame_worker(&j);
        for (int i = 0; i < nt; i++) if (th[i]) plat_thread_join(th[i]);
        ok = !j.bad;
    }
    free(f);
    return ok;
}

// buf[0, n) as frames of CHUNK bytes at zstd level: the compressed bytes (malloc) and their size in *zn; NULL on failure.
static uint8_t *encode_frames(const uint8_t *buf, size_t n, int level, size_t *zn) {
    size_t cap = 0;
    for (size_t at = 0; at < n; at += CHUNK) cap += ZSTD_compressBound(n - at < CHUNK ? n - at : CHUNK);
    uint8_t *z = malloc(cap ? cap : 1);
    *zn = 0;
    for (size_t at = 0; z && at < n; at += CHUNK) {
        size_t r = ZSTD_compress(z + *zn, cap - *zn, buf + at, n - at < CHUNK ? n - at : CHUNK, level);
        if (ZSTD_isError(r)) { free(z); return NULL; }
        *zn += r;
    }
    return z;
}

// An uncompressed write-back file read straight into dst: the zstd frames cost the replay thread 30-96 ms in the frames
// that loaded large textures (q31-use, 24/11/2026: 1.6 GiB in a race start), where the GPU encoder had cost it ~2.
// A file over RAW_PAR_MIN is read in RAW_CHUNK pieces by up to RAW_THREADS threads: one 1 GiB pread of a 121 MiB BC7
// file took 78 ms on the Deck's NVMe (q37-cs, 24/11/2026: 1.5 GB/s, the file mostly out of the page cache), where dd
// reads 3.2 GB/s with 8 MiB requests; from the page cache the copy spreads over the threads too.
#define RAW_PAR_MIN (8ull << 20)
#define RAW_CHUNK (4u << 20)
#define RAW_THREADS 4
typedef struct { PlatFile *f; uint8_t *dst; uint64_t size, base; volatile int32_t next, bad; } RawJob;

static void raw_worker(void *arg) {
    RawJob *j = arg;
    for (int32_t i; (uint64_t)(i = plat_inc32(&j->next) - 1) * RAW_CHUNK < j->size;) {
        uint64_t off = (uint64_t)i * RAW_CHUNK, n = j->size - off < RAW_CHUNK ? j->size - off : RAW_CHUNK;
        for (uint64_t done = 0; done < n;) {
            int64_t r = plat_file_pread(j->f, j->dst + off + done, (uint32_t)(n - done), j->base + off + done);
            if (r <= 0) { plat_inc32(&j->bad); return; }
            done += (uint64_t)r;
        }
    }
}

// Bytes [off, off + n) of h's raw file, which must hold fsize bytes, into dst.
static int read_raw_at(uint64_t h, int bc, void *dst, uint64_t off, uint64_t n, uint64_t fsize) {
    char path[PLAT_PATH_MAX + 32];
    snprintf(path, sizeof path, "%s/%016llx.%s.raw", T.dir, (unsigned long long)h, bc == 2 ? "bc1" : "bc7");
    if (plat_file_size(path) != (int64_t)fsize || off + n > fsize) return 0;
    PlatFile *f = plat_file_open(path);
    if (!f) return 0;
    RawJob j = {f, dst, n, off};
    PlatThread *th[RAW_THREADS - 1];
    int nt = n < RAW_PAR_MIN ? 0 : RAW_THREADS - 1;
    for (int i = 0; i < nt; i++) th[i] = plat_thread_new(raw_worker, &j, "texpre-read");
    raw_worker(&j);
    for (int i = 0; i < nt; i++) if (th[i]) plat_thread_join(th[i]);
    plat_file_close(f);
    return !j.bad;
}

static int load_raw(uint64_t h, int bc, void *dst, uint64_t size) { return read_raw_at(h, bc, dst, 0, size, size); }

// ---- cache reads off the replay thread (MKW_TEXPRE_ASYNC, default on, 0 off): a hit records its copy at once and a
// reader thread fills the staging; vk_upload_flush waits for the batch's reads before it submits it
// (vk_texpre_reads_wait). The race's remaining 50-110 ms frames held 14-73 ms of texture uploads on the replay thread:
// many small cache reads in the frame where the course opens up, the same place in every run (q45, 24/11/2026), with
// the thread mostly off the CPU, waiting for them (perf, user samples).
#define RD_THREADS 3
#define RD_QMAX 1024
typedef struct { uint64_t h; void *dst; uint64_t off, n, fsize, ticket; int bc; } RdJob;
static struct {
    PlatMutex lock;
    PlatCond work, idle;
    RdJob q[RD_QMAX];
    int head, n, busy;
    uint64_t failed;
} R = {.lock = PLAT_MUTEX_INIT};
static PlatOnce rd_once = PLAT_ONCE_INIT;
static int rd_on;

// A raw file that failed a read: not used again in this run (vk_texpre_has_bc). A queued read's ticket is kept for
// make_fresh, which uploads the texture again through the GPU encoder (and the write-back rewrites the file): stamping
// its storage as written was not enough, as without a write watch the storage hash, unchanged, kept the zeros.
#define FAILED_MAX 65536   // tickets: 16 bytes each in the table
static void read_failed(uint64_t h, int bc, uint64_t ticket) {
    uint64_t k = bc_key(h, bc);
    plat_mutex_lock(&T.lock);
    if (!nvn_hm_get(&T.bad, &k)) nvn_hm_put(&T.bad, &k, (void *)1), __atomic_add_fetch(&T.nbad, 1, __ATOMIC_RELEASE);
    if (ticket && !nvn_hm_get(&T.failed, &ticket)) {
        // A texture re-uploaded or gone before its read failed never asks for its ticket, so the table stops at
        // FAILED_MAX: past it a failed read's texture keeps its zeros until its storage changes (a run that loses that
        // many cache reads has lost its disk; every failure also marks its key bad, so nothing more is read from it).
        static volatile int32_t full_logged;
        if (T.failed.n < FAILED_MAX)
            nvn_hm_put(&T.failed, &ticket, (void *)1), __atomic_add_fetch(&T.nfailed, 1, __ATOMIC_RELEASE);
        else if (plat_inc32(&full_logged) == 1)
            rt_log("vk: texture cache: %d failed reads kept, later ones are not repaired", FAILED_MAX);
    }
    plat_mutex_unlock(&T.lock);
}

static volatile uint64_t next_ticket;
uint64_t vk_texpre_ticket(void) { return __atomic_add_fetch(&next_ticket, 1, __ATOMIC_RELAXED); }

// make_fresh: 1 once if the queued read of that ticket failed (the texture holds zeros).
int vk_texpre_failed(uint64_t ticket) {
    if (!__atomic_load_n(&T.nfailed, __ATOMIC_ACQUIRE)) return 0;
    plat_mutex_lock(&T.lock);
    int f = nvn_hm_del(&T.failed, &ticket) != NULL;
    if (f) __atomic_sub_fetch(&T.nfailed, 1, __ATOMIC_RELEASE);
    plat_mutex_unlock(&T.lock);
    return f;
}

static void rd_worker(void *arg) {
    (void)arg;
    plat_mutex_lock(&R.lock);
    for (;;) {
        while (!R.n) plat_cond_wait(&R.work, &R.lock);
        RdJob j = R.q[R.head];
        R.head = (R.head + 1) % RD_QMAX, R.n--, R.busy++;
        plat_mutex_unlock(&R.lock);
        int ok = read_raw_at(j.h, j.bc, j.dst, j.off, j.n, j.fsize);
        if (!ok) {   // its copy is recorded already: zeros go up now, and the texture again at the next check
            memset(j.dst, 0, (size_t)j.n);
            read_failed(j.h, j.bc, j.ticket);
        }
        plat_mutex_lock(&R.lock);
        R.busy--;
        if (!ok && ++R.failed <= 16)
            rt_log("vk: texture cache read of %016llx failed: the texture goes up again through the GPU encoder",
                   (unsigned long long)j.h);
        if (!R.n && !R.busy) plat_cond_broadcast(&R.idle);
    }
}

static void rd_init(void *arg) {
    (void)arg;
    const char *e = cfg_get("MKW_TEXPRE_ASYNC");
    if (e && !strcmp(e, "0")) return;
    plat_cond_init(&R.work), plat_cond_init(&R.idle);
    int n = 0;
    for (int i = 0; i < RD_THREADS; i++) n += plat_thread_spawn(rd_worker, NULL, "texpre-reader", 0) != 0;
    rd_on = n > 0;
    rt_log("vk: texture cache reads on %d threads of their own (MKW_TEXPRE_ASYNC)", n);
}

// 1: h has a raw write-back file of size bytes (read with vk_texpre_read_at); counted as a hit.
int vk_texpre_raw(uint64_t h, int bc, uint64_t size) {
    plat_once(&rd_once, rd_init, NULL);
    uint64_t key = bc_key(h, bc);
    if (!vk_texpre_on() || !nvn_hm_get(&T.raw, &key) || is_bad(key)) return 0;
    char path[PLAT_PATH_MAX + 32];
    snprintf(path, sizeof path, "%s/%016llx.%s.raw", T.dir, (unsigned long long)h, bc == 2 ? "bc1" : "bc7");
    int ok = plat_file_size(path) == (int64_t)size;
    if (ok) {
        plat_mutex_lock(&T.lock);
        T.n_hit++, T.hit_bytes += size;
        if (!(T.n_hit & (T.n_hit - 1)))
            rt_log("vk: %llu precomputed BC7 textures loaded (%.0f MiB)", (unsigned long long)T.n_hit, T.hit_bytes / 1048576.0);
        plat_mutex_unlock(&T.lock);
    }
    return ok;
}

// Bytes [off, off + n) of h's raw file (fsize bytes, vk_texpre_raw) into dst: on a reader thread before the open upload
// batch is sent when async and one has room, else now. 0 when a read now failed (the caller takes the GPU encoder).
// ticket (vk_texpre_ticket, 0: read now): the texture's, reported by vk_texpre_failed when a queued read fails.
int vk_texpre_read_at(uint64_t h, int bc, void *dst, uint64_t off, uint64_t n, uint64_t fsize, uint64_t ticket) {
    int queued = 0;
    if (ticket && rd_on) {
        plat_mutex_lock(&R.lock);
        if ((queued = R.n < RD_QMAX))
            R.q[(R.head + R.n) % RD_QMAX] = (RdJob){h, dst, off, n, fsize, ticket, bc}, R.n++, plat_cond_signal(&R.work);
        plat_mutex_unlock(&R.lock);
    }
    if (queued) return 1;
    if (read_raw_at(h, bc, dst, off, n, fsize)) return 1;
    read_failed(h, bc, 0);
    return 0;
}

// vk_upload_flush, before it submits the open batch: every queued read is in its staging.
void vk_texpre_reads_wait(void) {
    if (!__atomic_load_n(&rd_on, __ATOMIC_ACQUIRE)) return;
    plat_mutex_lock(&R.lock);
    while (R.n || R.busy) plat_cond_wait(&R.idle, &R.lock);
    plat_mutex_unlock(&R.lock);
}

int vk_texpre_load_bc(uint64_t h, int bc, void *dst, uint64_t size) {
    if (!vk_texpre_has_bc(h, bc)) return 0;
    uint64_t key = bc_key(h, bc);
    int raw = nvn_hm_get(&T.raw, &key) && load_raw(h, bc, dst, size);
    if (!raw && nvn_hm_get(&T.raw, &key) && !nvn_hm_get(&T.have, &key)) read_failed(h, bc, 0);
    char path[PLAT_PATH_MAX + 32];
    snprintf(path, sizeof path, "%s/%016llx.%s.zst", T.dir, (unsigned long long)h, bc == 2 ? "bc1" : "bc7");
    uint64_t n = 0;
    uint8_t *z = raw || !nvn_hm_get(&T.have, &key) ? NULL : plat_file_read(path, &n);
    int ok = raw || (z && decode_frames(z, (size_t)n, dst, size));
    free(z);
    static volatile int32_t n_bad;
    if (!ok && plat_inc32(&n_bad) <= 16) rt_log("vk: %s does not hold %llu bytes of BC7; not used", path, (unsigned long long)size);
    if (ok) {
        plat_mutex_lock(&T.lock);
        T.n_hit++, T.hit_bytes += size;
        if (!(T.n_hit & (T.n_hit - 1)))
            rt_log("vk: %llu precomputed BC7 textures loaded (%.0f MiB)", (unsigned long long)T.n_hit, T.hit_bytes / 1048576.0);
        plat_mutex_unlock(&T.lock);
    }
    return ok;
}

int vk_texpre_load(uint64_t h, void *dst, uint64_t size) { return vk_texpre_load_bc(h, 1, dst, size); }

typedef struct { uint64_t h; size_t n; uint8_t *buf; } WriteJob;

static void writer(void *arg) {
    WriteJob *j = arg;
    size_t cap = ZSTD_compressBound(j->n);
    uint8_t *z = malloc(cap);
    size_t zn = z ? ZSTD_compress(z, cap, j->buf, j->n, 3) : 0;
    char dir[PLAT_PATH_MAX + 8], path[PLAT_PATH_MAX + 48];
    snprintf(dir, sizeof dir, "%s/src", T.dir);
    snprintf(path, sizeof path, "%s/%016llx.astc.zst", dir, (unsigned long long)j->h);
    plat_file_mkdir_p(dir);
    if (z && !ZSTD_isError(zn) && !plat_file_write_atomic(path, z, zn)) rt_log("vk: cannot write %s", path);
    free(z);
    plat_mutex_lock(&T.lock);
    T.pending -= j->n;
    plat_mutex_unlock(&T.lock);
    free(j->buf);
    free(j);
}

// At most this many bytes wait for writer threads; a texture past it is recorded in a later run.
#define PENDING_MAX (1ull << 30)

void vk_texpre_record(uint64_t h, const nvn_texdesc *d, const VkTexpreRegion *rg, int n, const void *mem, uint64_t size) {
    if (!vk_texpre_on() || !T.record) return;
    size_t total = sizeof(SrcHead) + sizeof *rg * (size_t)n + (size_t)size;
    plat_mutex_lock(&T.lock);
    int seen = nvn_hm_get(&T.seen, &h) != NULL, full = !seen && T.pending + total > PENDING_MAX;
    if (!seen && !full) nvn_hm_put(&T.seen, &h, (void *)1), T.pending += total, T.n_rec++;
    static uint64_t n_full;
    uint64_t nf = full ? ++n_full : 0;
    if (nf && !(nf & (nf - 1)))
        rt_log("vk: %llu textures not recorded: %.0f MiB already wait for the disk (next run records them)",
               (unsigned long long)n_full, T.pending / 1048576.0);
    plat_mutex_unlock(&T.lock);
    if (seen || full) return;
    WriteJob *j = malloc(sizeof *j);
    uint8_t *b = malloc(total);
    if (!j || !b) rt_fatal("vk: out of memory recording a texture");
    SrcHead hd = {.magic = SRC_MAGIC, .size = size, .d = *d, .n = (uint32_t)n};
    memcpy(b, &hd, sizeof hd);
    memcpy(b + sizeof hd, rg, sizeof *rg * (size_t)n);
    memcpy(b + sizeof hd + sizeof *rg * (size_t)n, mem, (size_t)size);
    *j = (WriteJob){h, total, b};
    if (!plat_thread_spawn(writer, j, "texpre", 0)) writer(j);
}

// ---- MKW_TEXPRE_WRITEBACK (default on with MKW_TEXPRE): the BC7 or BC1 blocks the GPU encoder made of an ASTC texture
// are read back (vk_astc_bc_readback) and stored as <hash>.bc7.raw / <hash>.bc1.raw, so the next launch uploads them as
// they are instead of decoding and encoding again on the GPU. On the Steam Deck (no native ASTC) that work came in upload
// batches of up to 121 ms on the single queue during the first lap of every race after a launch (q29-tl, 24/11/2026:
// 2.4 G texels decoded in the race's first part, none after): the stutter the player saw. The blocks stored are the GPU
// encoder's own, so the picture does not change. Files of earlier runs are listed once, at the first upload.
#define WB_PENDING_MAX (256ull << 20)   // staging held by write-backs not yet on disk; past it a texture waits for a later run
// One part per upload group (vk_tex_upload.c up_group): no staging buffer the size of a whole texture (#821).
#define WB_PARTS 64
struct VkTexpreWb {
    uint64_t h, size, got;
    int bc, n, bad;
    struct { VkBuffer buf; VmaAllocation alloc; const void *map; uint64_t size; } p[WB_PARTS];
};

static int wb_claim(uint64_t h, int bc, uint64_t size) {
    if (!vk_texpre_on() || !T.writeback || !size) return 0;
    uint64_t k = bc_key(h, bc);
    plat_mutex_lock(&T.lock);
    int ok = !nvn_hm_get(&T.wb_seen, &k) && T.wb_pending + size <= WB_PENDING_MAX;
    if (ok) nvn_hm_put(&T.wb_seen, &k, (void *)1), T.wb_pending += size;
    plat_mutex_unlock(&T.lock);
    return ok;
}

// writer: it ran on a writer thread counted in wb_active (wb_ready).
static void wb_done(VkTexpreWb *j, int written, int writer) {
    for (int i = 0; i < j->n; i++) vmaDestroyBuffer(vk_ctx()->vma, j->p[i].buf, j->p[i].alloc);
    plat_mutex_lock(&T.lock);
    T.wb_pending -= j->size;
    if (writer && !--T.wb_active) plat_cond_broadcast(&T.wb_cv);
    if (written) {
        T.n_wb++, T.wb_bytes += j->size;
        if (!(T.n_wb & (T.n_wb - 1)))
            rt_log("vk: %llu GPU-encoded textures stored for the next launch (%.0f MiB)", (unsigned long long)T.n_wb,
                   T.wb_bytes / 1048576.0);
    }
    plat_mutex_unlock(&T.lock);
    free(j);
}

static void wb_writer(void *arg) {
    plat_thread_background();   // off the frame's fast cores (MKW_FAST_CORES)
    VkTexpreWb *j = arg;
    char path[PLAT_PATH_MAX + 48], tmp[PLAT_PATH_MAX + 80];   // uncompressed: read back without decoding (load_raw)
    snprintf(path, sizeof path, "%s/%016llx.%s.raw", T.dir, (unsigned long long)j->h, j->bc == 2 ? "bc1" : "bc7");
    snprintf(tmp, sizeof tmp, "%s.%u.tmp", path, plat_thread_id());
    // On the disk before it takes its name (plat_file_write_atomic's way, in parts): without the sync a crash or a flat
    // battery can leave a file of the right size holding zeros, which the size check would take.
    FILE *fp = fopen(tmp, "wb");
    int ok = fp != NULL;
    for (int i = 0; ok && i < j->n; i++) {
        vmaInvalidateAllocation(vk_ctx()->vma, j->p[i].alloc, 0, VK_WHOLE_SIZE);
        ok = fwrite(j->p[i].map, 1, (size_t)j->p[i].size, fp) == (size_t)j->p[i].size;
    }
    ok = ok && plat_file_sync(fp);
    ok = fp && fclose(fp) == 0 && ok;
    if (ok) ok = plat_file_replace(tmp, path);
    if (!ok) remove(tmp), rt_log("vk: cannot write %s", path);
    wb_done(j, ok, 1);
}

// vk_defer_call: the batch that copied the blocks is done. Past vk_texpre_shutdown (the upload batches' teardown runs
// the deferred calls left, then the staging pool and VMA go) the blocks are dropped.
static void wb_ready(void *arg) {
    plat_mutex_lock(&T.lock);
    int run = !T.closing;
    if (run) T.wb_active++;
    plat_mutex_unlock(&T.lock);
    if (!run) wb_done(arg, 0, 0);
    else if (!plat_thread_spawn(wb_writer, arg, "texpre-wb", 0)) wb_writer(arg);
}

// upload_shutdown, before its last deferred calls, the staging pool and the allocator go: the queued reads are done, the
// writers have finished with their buffers, and no new writer starts.
void vk_texpre_shutdown(void) {
    if (!__atomic_load_n(&T.on, __ATOMIC_ACQUIRE)) return;
    vk_texpre_reads_wait();
    plat_mutex_lock(&T.lock);
    T.closing = 1;
    while (T.wb_active) plat_cond_wait(&T.wb_cv, &T.lock);
    plat_mutex_unlock(&T.lock);
}

// The write-back of an upload that misses the cache: NULL when it is not stored (off, already claimed, too much pending).
VkTexpreWb *vk_texpre_wb_begin(uint64_t h, int bc, uint64_t size) {
    if (!wb_claim(h, bc, size)) return NULL;
    VkTexpreWb *w = calloc(1, sizeof *w);
    if (!w) rt_fatal("vk: out of memory (texture write-back)");
    w->h = h, w->bc = bc, w->size = size;
    return w;
}

// A host-visible buffer for the next group's size bytes of blocks (vk_astc_bc_readback); VK_NULL_HANDLE when none could be had.
VkBuffer vk_texpre_wb_part(VkTexpreWb *w, uint64_t size) {
    VkBuffer b = VK_NULL_HANDLE;
    VmaAllocation a = NULL;
    void *m = w->n < WB_PARTS && !w->bad ? vk_staging_try(size, &b, &a) : NULL;
    if (!m) { w->bad = 1; return VK_NULL_HANDLE; }
    w->p[w->n].buf = b, w->p[w->n].alloc = a, w->p[w->n].map = m, w->p[w->n].size = size, w->n++;
    return b;
}

// The bytes the GPU copied into the last part (vk_astc_bc_readback_take) and whether its decode was recorded.
void vk_texpre_wb_got(VkTexpreWb *w, uint64_t bytes, int recorded) {
    if (!recorded || !w->n || bytes != w->p[w->n - 1].size) w->bad = 1;
    w->got += bytes;
}

// After the upload's last group: written to disk once its batches are done, or its parts freed then.
void vk_texpre_wb_end(VkTexpreWb *w) {
    if (!w->bad && w->got == w->size) {
        vk_defer_call(wb_ready, w);
        return;
    }
    plat_mutex_lock(&T.lock);
    T.wb_pending -= w->size;
    plat_mutex_unlock(&T.lock);
    for (int i = 0; i < w->n; i++) vk_defer_buffer(w->p[i].buf, w->p[i].alloc);
    free(w);
}

// ---- build/mkw --texpre
typedef struct { char (*names)[64]; int n, cap; volatile int32_t next; uint64_t in_bytes, out_bytes, texels; int failed; } EncList;
static PlatMutex enc_lock;

static int src_cb(const char *name, void *ctx) {   // also the .bc7.zst of the directory (rechunk_one)
    EncList *L = ctx;
    if (strlen(name) >= 64 || !(strstr(name, ".astc.zst") || strstr(name, ".bc7.zst"))) return 1;
    if (L->n == L->cap && !(L->names = realloc(L->names, sizeof *L->names * (size_t)(L->cap = L->cap ? 2 * L->cap : 256))))
        rt_fatal("texpre: out of memory");
    strcpy(L->names[L->n++], name);
    return 1;
}

// One src file into its .bc7.zst; 1 on success.
static int encode_one(const char *name, uint64_t *in_b, uint64_t *out_b, uint64_t *texels) {
    char path[PLAT_PATH_MAX + 96], out[PLAT_PATH_MAX + 96];
    snprintf(path, sizeof path, "%s/src/%s", T.dir, name);
    snprintf(out, sizeof out, "%s/%.16s.bc7.zst", T.dir, name);
    uint64_t zn = 0;
    uint8_t *z = plat_file_read(path, &zn), *b = NULL, *bc = NULL;
    unsigned long long raw = z ? ZSTD_getFrameContentSize(z, zn) : 0;
    int ok = 0;
    if (!z || raw == ZSTD_CONTENTSIZE_ERROR || raw == ZSTD_CONTENTSIZE_UNKNOWN || raw < sizeof(SrcHead)) goto done;
    if (!(b = malloc(raw)) || ZSTD_decompress(b, raw, z, zn) != raw) goto done;
    SrcHead hd;
    memcpy(&hd, b, sizeof hd);
    const VkTexpreRegion *rg = (const VkTexpreRegion *)(b + sizeof hd);
    const uint8_t *mem = b + sizeof hd + sizeof *rg * hd.n;
    TexVkFormat f;
    if (hd.magic != SRC_MAGIC || sizeof hd + sizeof *rg * hd.n + hd.size != raw || !tex_vk_format(hd.d.format, &f) ||
        !f.astc_bw)
        goto done;
    uint64_t total = 0;
    for (uint32_t k = 0; k < hd.n; k++) total += vk_texpre_region_bytes(&rg[k]);
    if (!(bc = malloc(total))) goto done;
    uint8_t *o = bc;
    for (uint32_t k = 0; k < hd.n; k++) {
        const VkTexpreRegion *r = &rg[k];
        uint32_t rb = (r->w + f.astc_bw - 1) / f.astc_bw * 16, rows = (r->h + f.astc_bh - 1) / f.astc_bh;
        uint8_t *lin = malloc((size_t)rb * rows * r->slices), *px = malloc((size_t)r->w * r->h * 4);
        int bad = !lin || !px || nvn_deswizzle(&hd.d, r->level, r->layer, mem, lin, rb);
        for (uint32_t s = 0; !bad && s < r->slices; s++) {
            astc_decode_ldr(lin + (size_t)s * rb * rows, r->w, r->h, f.astc_bw, f.astc_bh, f.astc_srgb, px);
            for (uint32_t by = 0; by < r->h; by += 4)
                for (uint32_t bx = 0; bx < r->w; bx += 4, o += 16) {
                    uint8_t blk[64];
                    for (uint32_t y = 0; y < 4; y++)   // edge blocks repeat the last row and column
                        for (uint32_t x = 0; x < 4; x++) {
                            uint32_t sx = bx + x < r->w ? bx + x : r->w - 1, sy = by + y < r->h ? by + y : r->h - 1;
                            memcpy(blk + 4 * (4 * y + x), px + 4 * ((size_t)sy * r->w + sx), 4);
                        }
                    bc7enc_c_block(o, blk);
                }
        }
        free(lin), free(px);
        if (bad) goto done;
        *texels += (uint64_t)r->w * r->h * r->slices;
    }
    size_t zon;
    uint8_t *zo = encode_frames(bc, total, 19, &zon);
    ok = zo && plat_file_write_atomic(out, zo, zon);
    free(zo);
    if (ok) {
        plat_file_delete(path);
        *in_b += zn, *out_b += zon;
    }
done:
    if (!ok) printf("texpre: %s could not be encoded\n", name);
    free(z), free(b), free(bc);
    return ok;
}

// A .bc7.zst written as one frame (before CHUNK) and larger than CHUNK, written again as frames; 1 unless it fails.
static int rechunk_one(const char *name) {
    char path[PLAT_PATH_MAX + 96];
    snprintf(path, sizeof path, "%s/%s", T.dir, name);
    uint64_t zn = 0;
    uint8_t *z = plat_file_read(path, &zn), *b = NULL, *zo = NULL;
    unsigned long long raw = z ? ZSTD_getFrameContentSize(z, zn) : ZSTD_CONTENTSIZE_ERROR;
    int ok = z && raw != ZSTD_CONTENTSIZE_ERROR && raw != ZSTD_CONTENTSIZE_UNKNOWN;
    if (ok && raw > CHUNK && ZSTD_findFrameCompressedSize(z, zn) == zn) {
        size_t on;
        ok = (b = malloc(raw)) && ZSTD_decompress(b, raw, z, zn) == raw && (zo = encode_frames(b, raw, 19, &on)) &&
             plat_file_write_atomic(path, zo, on);
    }
    if (!ok) printf("texpre: %s could not be split into frames\n", name);
    free(z), free(b), free(zo);
    return ok;
}

static void enc_worker(void *arg) {
    plat_thread_background();   // off the frame's fast cores (MKW_FAST_CORES)
    EncList *L = arg;
    uint64_t in_b = 0, out_b = 0, tx = 0;
    int failed = 0;
    for (int i; (i = plat_inc32(&L->next) - 1) < L->n;) {
        failed += strstr(L->names[i], ".bc7.zst") ? !rechunk_one(L->names[i]) : !encode_one(L->names[i], &in_b, &out_b, &tx);
        if ((i + 1) % 100 == 0) printf("texpre: %d of %d\n", i + 1, L->n), fflush(stdout);
    }
    plat_mutex_lock(&enc_lock);
    L->in_bytes += in_b, L->out_bytes += out_b, L->texels += tx, L->failed += failed;
    plat_mutex_unlock(&enc_lock);
}

int vk_texpre_encode(void) {
    if (!vk_texpre_on()) { printf("texpre: off (set MKW_TEXPRE=1 or MKW_TEXPRE=<dir>)\n"); return 1; }
    bc7enc_c_init();
    char dir[PLAT_PATH_MAX + 8];
    snprintf(dir, sizeof dir, "%s/src", T.dir);
    EncList L = {0};
    plat_file_list(dir, ".zst", src_cb, &L);
    plat_file_list(T.dir, ".bc7.zst", src_cb, &L);
    const char *j = cfg_get("MKW_TEXPRE_JOBS");
    int jobs = j && atoi(j) > 0 ? atoi(j) : plat_cpu_count();
    printf("texpre: %d files in %s (to encode) and %s (to check for frames) with %d threads\n", L.n, dir, T.dir, jobs), fflush(stdout);
    uint64_t t0 = kern_now_ns();
    PlatThread **th = calloc((size_t)jobs, sizeof *th);
    for (int i = 0; i < jobs; i++) th[i] = plat_thread_new(enc_worker, &L, "texpre");
    for (int i = 0; i < jobs; i++) if (th[i]) plat_thread_join(th[i]);
    double s = (kern_now_ns() - t0) / 1e9;
    printf("texpre: %d encoded, %d failed, %.1f M texels in %.1f s; %.1f MiB of ASTC (zstd) -> %.1f MiB of BC7 (zstd)\n",
           L.n - L.failed, L.failed, L.texels / 1e6, s, L.in_bytes / 1048576.0, L.out_bytes / 1048576.0);
    free(th), free(L.names);
    return L.failed != 0;
}
