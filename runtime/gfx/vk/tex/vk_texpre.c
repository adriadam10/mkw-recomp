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
    int on, record;
    nvn_hmap have;          // hashes with a .bc7.zst
    nvn_hmap seen;          // hashes recorded (or found) in this run
    PlatMutex lock;         // seen, and pending
    uint64_t pending;       // bytes held by writer threads
    uint64_t n_hit, n_rec, hit_bytes;
} T = {.have = {.klen = 8}, .seen = {.klen = 8}};
static PlatOnce t_once = PLAT_ONCE_INIT;

static int list_cb(const char *name, void *ctx) {   // plat_file_list: 1 goes on
    (void)ctx;
    char *end;
    uint64_t h = strtoull(name, &end, 16);
    if (end == name + 16 && !strcmp(end, ".bc7.zst")) nvn_hm_put(&T.have, &h, (void *)1);
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
    const char *r = cfg_get("MKW_TEXPRE_RECORD");
    T.record = !(r && !strcmp(r, "0"));
    plat_file_list(T.dir, ".zst", list_cb, NULL);
    T.on = 1;
    rt_log("vk: precomputed BC7 textures in %s: %u (recording new ones: %s)", T.dir, T.have.n, T.record ? "on" : "off");
}

int vk_texpre_on(void) {
    plat_once(&t_once, t_init, NULL);
    return T.on;
}

uint64_t vk_texpre_hash(const void *p, uint64_t n) { return XXH3_64bits(p, (size_t)n); }

uint64_t vk_texpre_region_bytes(const VkTexpreRegion *r) {
    return (uint64_t)((r->w + 3) / 4) * ((r->h + 3) / 4) * 16 * r->slices;
}

int vk_texpre_has(uint64_t h) { return vk_texpre_on() && nvn_hm_get(&T.have, &h); }

// A .bc7.zst is zstd frames of at most CHUNK bytes each, one after the other: one thread decodes 1 GiB in ~2.4 s (zstd
// -b on a 121 MiB file, 505 MB/s), and the menus load 1.2 GiB in one frame (live19), so the frames go to LOAD_THREADS.
#define CHUNK (4u << 20)
#define LOAD_THREADS 8
typedef struct { const uint8_t *src; size_t sn; uint8_t *dst; size_t dn; } Frame;
typedef struct { Frame *f; int n; volatile int32_t next, bad; } FrameJob;

static void frame_worker(void *arg) {
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

int vk_texpre_load(uint64_t h, void *dst, uint64_t size) {
    if (!vk_texpre_has(h)) return 0;
    char path[PLAT_PATH_MAX + 32];
    snprintf(path, sizeof path, "%s/%016llx.bc7.zst", T.dir, (unsigned long long)h);
    uint64_t n = 0;
    uint8_t *z = plat_file_read(path, &n);
    int ok = z && decode_frames(z, (size_t)n, dst, size);
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
