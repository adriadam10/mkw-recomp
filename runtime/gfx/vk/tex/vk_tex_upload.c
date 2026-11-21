#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "gfx.h"
#include "host.h"
#include "nvn_api.h"
#include "nvn_hmap.h"
#include "nvn_tex.h"
#include "tex.h"
#include "vk.h"
#include "vk_exec.h"
#include "vk_tex_internal.h"
#include "ui/glyphs/glyphs.h"
#include "platform/platform.h"

// ---------------------------------------------------------------- uploads
static uint32_t texel_alpha_one(int format, uint32_t *elem) {
    switch (format) {
    case 0x19: *elem = 1; return 0xFF;
    case 0x1A: *elem = 1; return 0x7F;
    case 0x1B: case 0x1C: *elem = 1; return 1;
    case 0x1D: *elem = 2; return 0x3C00;
    case 0x1E: *elem = 2; return 0xFFFF;
    case 0x1F: *elem = 2; return 0x7FFF;
    case 0x20: case 0x21: *elem = 2; return 1;
    case 0x22: *elem = 4; return 0x3F800000;
    default: *elem = 4; return 1;   // 0x23, 0x24
    }
}

// 3-component texels to 4 (n texels).
void expand_rgb(int format, const uint8_t *src, uint8_t *dst, size_t n) {
    uint32_t e, one = texel_alpha_one(format, &e);
    for (size_t i = 0; i < n; i++, src += 3 * e, dst += 4 * e) {
        memcpy(dst, src, 3 * e);
        memcpy(dst + 3 * e, &one, e);
    }
}

void shrink_rgb(int format, const uint8_t *src, uint8_t *dst, size_t n) {
    uint32_t e;
    texel_alpha_one(format, &e);
    for (size_t i = 0; i < n; i++, src += 4 * e, dst += 3 * e) memcpy(dst, src, 3 * e);
}

// MKW_ASTC=check: the compute decoder's texels of n regions (gpu: the regions one after the other) against
// astc_decode_ldr of the same raw blocks (raw: the staging bytes the regions point into).
static void astc_check(const Tex *e, const uint8_t *raw, const VkAstcRegion *ar, int n, const uint8_t *gpu) {
    static volatile int32_t n_bad;
    static uint64_t n_tex, n_texels, n_diff_tex;
    uint32_t bw = e->f.astc_bw, bh = e->f.astc_bh;
    uint64_t diff = 0, first = UINT64_MAX;
    uint32_t want = 0, got = 0;
    int first_k = 0;
    for (int k = 0; k < n; k++) {
        const VkAstcRegion *r = &ar[k];
        size_t sb = (size_t)((r->w + bw - 1) / bw) * ((r->h + bh - 1) / bh) * 16, st = (size_t)r->w * r->h * 4;
        uint8_t *cpu = malloc(st);
        for (uint32_t z = 0; z < r->slices; z++, gpu += st) {
            astc_decode_ldr(raw + r->src + z * sb, r->w, r->h, bw, bh, e->f.astc_srgb, cpu);
            if (!memcmp(cpu, gpu, st)) continue;
            for (size_t i = 0; i < st / 4; i++)
                if (memcmp(cpu + 4 * i, gpu + 4 * i, 4)) {
                    if (first == UINT64_MAX) {
                        first = i, first_k = k;
                        memcpy(&want, cpu + 4 * i, 4), memcpy(&got, gpu + 4 * i, 4);
                    }
                    diff++;
                }
        }
        free(cpu);
        n_texels += (uint64_t)r->w * r->h * r->slices;
    }
    n_tex++;
    if (diff) {
        n_diff_tex++;
        log_limited(&n_bad, "ASTC check: texture %#llx fmt %#x %ux%u: %llu texels differ, first region %d texel %llu "
                    "cpu %08x gpu %08x", (unsigned long long)e->pub.addr, e->t.d.format, bw, bh,
                    (unsigned long long)diff, first_k, (unsigned long long)first, want, got);
    }
    if (n_tex == 1 || !(n_tex & 63) || diff)
        rt_log("vk: ASTC check: %llu textures, %llu texels compared, %llu textures differ", (unsigned long long)n_tex,
               (unsigned long long)n_texels, (unsigned long long)n_diff_tex);
}

// An ASTC texture re-encoded as BC7: the BC7 encoded offline for these storage bytes (vk_texpre.c) straight into the
// image. Without one, the storage is recorded for the offline encoder and 0 sends the upload on through the GPU encoder.
static int upload_texpre(Tex *e, uint32_t levels, uint32_t layers) {
    const tex_t *t = &e->t;
    const nvn_texdesc *d = &t->d;
    int is3d = d->target == NVN_TEXTURE_TARGET_3D, one_d = e->pub.type == VK_IMAGE_TYPE_1D;
    uint32_t n = levels * layers, k = 0;
    VkTexpreRegion *pr = calloc(n, sizeof *pr);
    VkBufferImageCopy *rg = calloc(n, sizeof *rg);
    VkDeviceSize total = 0;
    for (uint32_t lv = 0; lv < levels; lv++)
        for (uint32_t ly = 0; ly < layers; ly++, k++) {
            uint32_t w = mip(d->width, lv), h = one_d ? 1 : mip(d->height, lv), sl = is3d ? mip(d->depth, lv) : 1;
            pr[k] = (VkTexpreRegion){w, h, sl, lv, ly};
            rg[k] = (VkBufferImageCopy){.bufferOffset = total, .imageSubresource = {e->pub.aspect, lv, ly, 1},
                                        .imageExtent = {w, h, sl}};
            total += vk_texpre_region_bytes(&pr[k]);
        }
    uint64_t h = vk_texpre_hash(t->mem, t->L.size);
    int ok = 0;
    if (vk_texpre_has(h)) {
        VkBuffer sb;
        uint8_t *stg;
        VkCommandBuffer cb = vk_upload_begin_staging(total, &sb, (void **)&stg);
        if (stg && vk_texpre_load(h, stg, total)) {
            barrier(cb, ALL_RW, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
            vkCmdCopyBufferToImage(cb, sb, e->pub.image, VK_IMAGE_LAYOUT_GENERAL, n, rg);
            barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, ALL_RW);
            ok = 1;
        }
        vk_upload_end();
    }
    if (!ok) vk_texpre_record(h, d, pr, (int)n, t->mem, t->L.size);
    free(pr), free(rg);
    return ok;
}

// Copies the whole storage into the image (every level and layer the image has).
// MKW_TRACE_ADDR=<hex> (diagnosis): the texture whose storage starts there: its GPU writes (vk_texture_gpu_written),
// uploads and WriteTexels, the first 64 events.
uint64_t trace_addr = 1;
static volatile int32_t traced;
// MKW_TRACE_ADDR=all (diagnosis): every texture, up to 20 000 lines: two replays of a capture told apart by their
// uploads and GPU writes (tools/test: a recording order that changes the image).
int tracing(uint64_t addr) {
    if (trace_addr == 1) {
        const char *e = cfg_get("MKW_TRACE_ADDR");
        trace_addr = e && !strcmp(e, "all") ? UINT64_MAX : e && *e ? strtoull(e, NULL, 16) : 0;
    }
    if (trace_addr == UINT64_MAX) return plat_inc32(&traced) <= 20000;
    return trace_addr && addr == trace_addr && plat_inc32(&traced) <= 64;
}

// The button icon sheet (ui/glyphs/glyphs.c): its texture object and the icons it was drawn with. When they change it is
// uploaded again, redrawn, at the next present (vk_backend.c, the game thread, after the queued replays). Waiting for its next storage check was not enough: after the first
// upload the binding memos never check it again while its pages stay clean (gl4, gl5, 06/11/2026).
static uint64_t g_glyph_addr;
static int g_glyph_known;
static uint32_t g_glyph_gen;
static void upload(Tex *e);

int vk_glyphs_due(void) {
    if (!__atomic_load_n(&g_glyph_known, __ATOMIC_ACQUIRE)) return 0;
    static uint64_t last_ns;
    uint64_t now = plat_now_ns();
    if (now - last_ns < 100000000ull) return 0;   // ten times a second: a device switch shows within a few frames
    last_ns = now;
    return glyphs_generation() != g_glyph_gen;
}

// Every live entry at the sheet's address: the game samples it through more than one (another view of the same
// storage); uploading the one entry_of found again changed nothing on screen (gl6, 06/11/2026).
void vk_glyphs_redo(void) {
    lock();
    g_glyph_gen = glyphs_generation();
    for (Tex *e = entries_at(g_glyph_addr); e; e = e->next_at)
        if (e->uploaded && !e->released && e->pub.mirrored && !e->pub.gpu_owned) upload(e);
    unlock();
}

// The deswizzle of an upload, over the texture workers (tex_par.c): each level and layer in bands of block rows, so the
// first level (three quarters of the bytes) spreads too. It ran serially on the replay thread, ~44 ms of the course
// load's 1.5 s peak (tl-fp1, 16/11/2026), at memcpy speed (16-25 GB/s on an i7-14700F, tex_bl_copy); the bands write
// apart, so the staging bytes are the same. Storage that has no row split (linear, or no CPU layout) is one item.
// Measured on replays of four captures (RX 6700 XT, i7-14700F, 17/11/2026; MKW_TEX_PAR=0 against 1, same images):
// textures of 512 KiB or more 16.0-17.0 -> 7.9-8.4 ms per capture (title, tt8, photo: 2x with 8 threads), the
// smaller ones left on the caller (5-11 ms over 400-900 uploads of ~140 KiB: waking the workers for each would cost
// what it saves). gp397's whole deswizzle: 19.7 -> 15.4 ms with this threshold, 19.5 -> 16.0 with 1 MiB.
#define UP_BAND (128u << 10)        // bytes of a band
#define UP_PAR_MIN (512ull << 10)   // a smaller texture deswizzles on the caller
typedef struct {
    uint8_t *dst, *tmp;            // where its rows go (tmp: the CPU decodes' input, freed by upload)
    size_t pitch;
    uint32_t level, layer;
    uint32_t band, first;          // block rows per item (0: one nvn_deswizzle), its first item
    int bad;                       // no CPU layout: uploaded as zeros
} UpPart;
typedef struct {
    const nvn_texdesc *d;
    const void *mem;
    UpPart *pt;
    uint32_t n;
} UpJob;

static void up_deswizzle(void *ctx, uint32_t i) {
    const UpJob *J = ctx;
    uint32_t lo = 0, hi = J->n - 1;   // the last part whose first item is at most i
    while (lo < hi) {
        uint32_t mid = (lo + hi + 1) / 2;
        if (J->pt[mid].first <= i) lo = mid; else hi = mid - 1;
    }
    UpPart *q = &J->pt[lo];
    if (!q->band) {
        q->bad = nvn_deswizzle(J->d, q->level, q->layer, J->mem, q->dst, q->pitch) != 0;
        return;
    }
    uint32_t r0 = (i - q->first) * q->band;
    nvn_deswizzle_rows(J->d, q->level, q->layer, J->mem, q->dst, q->pitch, r0, r0 + q->band, NULL);
}

static void upload(Tex *e) {
    static volatile int32_t n_bad, n_undecoded;
    if (tracing(e->pub.addr))
        rt_log("vk: trace %#llx: upload %u of entry %p (%ux%u target %d), submit epoch %u", (unsigned long long)e->pub.addr,
               e->n_up + 1, (void *)e, e->t.d.width, e->t.d.height, e->t.d.target, submit_epoch);
    const tex_t *t = &e->t;
    const nvn_texdesc *d = &t->d;
    int bw, bh, bpb = nvn_fmt_bpb(d->format, &bw, &bh);
    if (!bpb || !t->mem) return;
    uint64_t p0 = perf_begin(), u0 = kern_now_ns();
    int linear = nvn_tex_is_linear(d), is3d = d->target == NVN_TEXTURE_TARGET_3D, one_d = e->pub.type == VK_IMAGE_TYPE_1D;
    uint32_t levels = linear ? 1 : e->pub.levels, layers = linear ? 1 : e->pub.layers;
    int astc = e->f.astc_bw != 0, rgb = e->f.expand_rgb;
    // MKW_ASTC=check stages a CPU decode beside the GPU one: textures over 16 MiB stay unchecked (a 357 MB staging
    // buffer failed at a course load, vis22; 64 MiB still failed with the mip levels, vis24).
    int agpu = astc && vk_astc_gpu(),
        acheck = agpu && vk_astc_mode() == VK_ASTC_CHECK && (uint64_t)e->pub.width * e->pub.height * e->pub.layers * 4 <= (16ull << 20);
    uint32_t n = levels * layers, k = 0;
    uint32_t drop = e->pub.drop;   // NVN level = image level + drop
    if (agpu && e->f.astc_bc7 == 1 && !acheck && !linear && !drop && vk_texpre_on() && upload_texpre(e, levels, layers))
        goto counted;
    VkBufferImageCopy *rg = calloc(n, sizeof *rg);
    VkAstcRegion *ar = agpu ? calloc(n, sizeof *ar) : NULL;
    VkDeviceSize total = 0, decoded = 0;
    for (uint32_t lv = 0; lv < levels; lv++)
        for (uint32_t ly = 0; ly < layers; ly++) {
            uint32_t w = mip(d->width, lv + drop), h = one_d ? 1 : mip(d->height, lv + drop),
                     sl = is3d ? mip(d->depth, lv + drop) : 1;
            uint64_t texels = (uint64_t)w * h * sl, raw = (uint64_t)(w + bw - 1) / bw * bpb * ((h + bh - 1) / bh) * sl;
            uint64_t out = agpu ? raw : astc ? texels * 4 : rgb ? texels * (uint64_t)bpb / 3 * 4 : raw;
            rg[k++] = (VkBufferImageCopy){.bufferOffset = total, .imageSubresource = {e->pub.aspect, lv, ly, 1},
                                          .imageExtent = {w, h, sl}};
            total += (out + 15) & ~15ull;
            decoded += texels * 4;
        }
    vk_upload_flush_over(64ull << 20);   // as vk_tex_texel.c: the load of a capture sent its textures in one batch
    VkBuffer sb, rbuf = VK_NULL_HANDLE;
    VmaAllocation ralloc = NULL;
    uint8_t *stg;
    VkCommandBuffer cb = vk_upload_begin_staging(total, &sb, (void **)&stg);
    uint8_t *rmap = acheck ? vk_staging(decoded, &rbuf, &ralloc) : NULL;
    UpPart *pt = calloc(n, sizeof *pt);
    uint32_t items = 0;
    uint64_t raw_all = 0;
    k = 0;
    for (uint32_t lv = 0; lv < levels; lv++)
        for (uint32_t ly = 0; ly < layers; ly++, k++) {
            uint32_t w = rg[k].imageExtent.width, h = rg[k].imageExtent.height, sl = rg[k].imageExtent.depth;
            uint32_t rb = (w + bw - 1) / bw * (uint32_t)bpb, rows = (h + bh - 1) / bh, all = 0;
            size_t raw = (size_t)rb * rows * sl;
            UpPart *q = &pt[k];
            q->level = lv + drop, q->layer = ly, q->pitch = rb, q->first = items;
            q->tmp = (astc && !agpu) || rgb ? malloc(raw) : NULL;
            q->dst = q->tmp ? q->tmp : stg + rg[k].bufferOffset;
            if (!nvn_deswizzle_rows(d, q->level, ly, t->mem, q->dst, rb, 0, 0, &all) && all)
                q->band = UP_BAND / rb ? UP_BAND / rb : 1;
            items += q->band ? (all + q->band - 1) / q->band : 1;
            raw_all += raw;
        }
    UpJob job = {d, t->mem, pt, n};
    uint64_t dz0 = plat_now_ns();
    if (raw_all >= UP_PAR_MIN) tex_par_for(items, up_deswizzle, &job);
    else for (uint32_t i = 0; i < items; i++) up_deswizzle(&job, i);
    if (vk_pool_diag_on()) {   // MKW_MEM_POOL_LABELS: read from a CPU_NO_ACCESS pool's game pages?
        const pool_t *p = nvn_pool_find(e->pub.addr, 1);
        if (p) vk_pool_diag_note(p->flags, 1, raw_all);
        if (p && astc) vk_pool_diag_note(p->flags, 3, raw_all);
    }
    if (rt_trace & RT_TRACE_PERF) {   // MKW_TRACE=perf: the deswizzle's wall time on this thread, split and not
        static volatile int64_t dz_ns[2], dz_bytes[2];
        static volatile int32_t dz_n;
        int split = raw_all >= UP_PAR_MIN;
        __atomic_add_fetch(&dz_ns[split], (int64_t)(plat_now_ns() - dz0), __ATOMIC_RELAXED);
        __atomic_add_fetch(&dz_bytes[split], (int64_t)raw_all, __ATOMIC_RELAXED);
        if (plat_inc32(&dz_n) % 256 == 0)
            rt_log("perf: upload deswizzle: %d uploads; over the texture workers %.1f MiB in %.1f ms, on the caller "
                   "(under %llu KiB) %.1f MiB in %.1f ms", dz_n, dz_bytes[1] / 1048576.0, dz_ns[1] / 1e6,
                   UP_PAR_MIN >> 10, dz_bytes[0] / 1048576.0, dz_ns[0] / 1e6);
    }
    k = 0;
    for (uint32_t lv = 0; lv < levels; lv++)
        for (uint32_t ly = 0; ly < layers; ly++, k++) {
            uint32_t w = rg[k].imageExtent.width, h = rg[k].imageExtent.height, sl = rg[k].imageExtent.depth;
            uint32_t rb = (w + bw - 1) / bw * (uint32_t)bpb, rows = (h + bh - 1) / bh;
            size_t raw = (size_t)rb * rows * sl;
            uint8_t *dst = stg + rg[k].bufferOffset, *tmp = pt[k].tmp;
            if (pt[k].bad) {
                log_limited(&n_bad, "texture %#llx fmt %#x target %d: level %u layer %u has no CPU layout; uploaded as zeros",
                            (unsigned long long)e->pub.addr, d->format, d->target, lv, ly);
                memset(tmp ? tmp : dst, 0, raw);
            }
            if (agpu) {
                ar[k] = (VkAstcRegion){.src = rg[k].bufferOffset, .w = w, .h = h, .slices = sl, .level = lv, .layer = ly};
            } else if (astc) {
                uint64_t a0 = perf_begin();
                for (uint32_t z = 0; z < sl; z++)
                    astc_decode_ldr_mt(tmp + (size_t)z * rb * rows, w, h, e->f.astc_bw, e->f.astc_bh, e->f.astc_srgb,
                                       dst + (size_t)z * w * h * 4);
                perf_end(PERF_ASTC, a0);
                rg[k].bufferRowLength = w, rg[k].bufferImageHeight = h;
            } else if (rgb) {
                expand_rgb(d->format, tmp, dst, (size_t)w * h * sl);
                rg[k].bufferRowLength = w, rg[k].bufferImageHeight = h;
            } else {
                rg[k].bufferRowLength = rb / (uint32_t)bpb * (uint32_t)bw, rg[k].bufferImageHeight = rows * (uint32_t)bh;
                if (lv == 0 && ly == 0 && sl == 1 && glyphs_apply(e->pub.addr, d->format, w, h, dst, rb)) {
                    g_glyph_addr = e->pub.addr;   // the button icon sheet (vk_glyphs_due)
                    g_glyph_gen = glyphs_generation();
                    __atomic_store_n(&g_glyph_known, 1, __ATOMIC_RELEASE);
                }
            }
            free(tmp);
        }
    free(pt);
    uint8_t *raw_copy = acheck ? malloc(total) : NULL;
    if (raw_copy) memcpy(raw_copy, stg, total);
    barrier(cb, ALL_RW, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    int checked = 0;
    if (agpu) {
        uint64_t a0 = perf_begin();
        checked = vk_astc_record(cb, sb, total, ar, (int)n, e->f.astc_bw, e->f.astc_bh, e->f.astc_srgb, e->pub.image,
                                 e->pub.aspect, e->f.astc_bc7, rbuf);
        perf_end(PERF_ASTC, a0);
        if (!checked)
            log_limited(&n_undecoded, "ASTC texture %#llx (fmt %#x %ux%u) not decoded on the GPU: left as it was",
                        (unsigned long long)e->pub.addr, d->format, d->width, d->height);
    } else {
        vkCmdCopyBufferToImage(cb, sb, e->pub.image, VK_IMAGE_LAYOUT_GENERAL, n, rg);
        if (e->dtwin.image) tex_twin_copy(e, cb, stg, total, rg, n);   // its depth twin (vk_tex_twin.c) from the same texels
    }
    barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, ALL_RW);
    vk_upload_end();
    if (acheck) {   // waits for the GPU
        vk_upload_flush(1);
        if (checked) {
            vmaInvalidateAllocation(vk_ctx()->vma, ralloc, 0, VK_WHOLE_SIZE);
            astc_check(e, raw_copy, ar, (int)n, rmap);
        }
        vmaDestroyBuffer(vk_ctx()->vma, rbuf, ralloc);
    }
    free(raw_copy);
    free(ar);
    free(rg);
counted:;
    uint64_t dt = kern_now_ns() - u0;
    if (e->uploaded) {
        n_reuploads++, perf_count(PERF_CNT_TEX_REUP);
        ru_n++, ru_bytes += t->L.size, ru_ns += dt;
        if (e->wt_n) {
            ru_wt_n++, ru_wt_bytes += t->L.size, ru_wt_ns += dt;
            if (rt_trace & RT_TRACE_PERF)
                rt_log("perf: frame %llu: re-upload after %u WriteTexels: texture %#llx fmt %#x %ux%u, %.1f KiB, %.3f ms",
                       (unsigned long long)__atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED), e->wt_n,
                       (unsigned long long)e->pub.addr, d->format, d->width, d->height, t->L.size / 1024.0, dt / 1e6);
        }
    } else {
        n_uploads++, perf_count(PERF_CNT_TEX_UP);
    }
    // MKW_TEX_UP_LOG=N (diagnosis): the first N uploads from frame MKW_TEX_UP_LOG_AT (2300) on, with how many submits
    // the game had made past the one being recorded: their storage is read now, from guest memory (Hookshot: MKW's
    // terrain height tiles, 68x68 RGBA16 at new addresses every frame, rewritten right after their submit).
    static int up_log = -1, up_at;
    if (up_log < 0) {
        up_log = cfg_get("MKW_TEX_UP_LOG") ? atoi(cfg_get("MKW_TEX_UP_LOG")) : 0;
        up_at = cfg_get("MKW_TEX_UP_LOG_AT") ? atoi(cfg_get("MKW_TEX_UP_LOG_AT")) : 2300;
    }
    uint64_t fr = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    if (up_log > 0 && fr >= (uint64_t)up_at) {
        up_log--;
        rt_log("vk: upload at frame %llu: %#llx %ux%ux%u fmt %#x levels %u, %.1f KiB%s, game %lld submits ahead",
               (unsigned long long)fr, (unsigned long long)e->pub.addr, d->width, d->height, d->depth, d->format,
               e->pub.levels, t->L.size / 1024.0, e->uploaded ? " (again)" : "",
               (long long)(nvn_submit_count() - __atomic_load_n(&vk_replay_submit, __ATOMIC_RELAXED)));
    }
    e->wt_n = 0;
    e->uploaded = 1;
    e->n_up++;
    perf_end(PERF_TEX, p0);
}

// A texture copy from an ASTC texture kept as RGBA8 (under 256x256) into one re-encoded as BC7: the source's own ASTC
// blocks, read from its storage, decoded and encoded straight into dst. MKW fills the small levels (5-11) of its sparse
// terrain arrays this way, from small ASTC textures; skipped, those levels read zero and the race's bushes and cacti,
// which take their colour from level 11, came out black (m16, 01/11/2026). 0 when it cannot (the caller skips).
int vk_texture_astc_copy(const tex_t *src, uint32_t level, uint32_t layer, uint32_t layers, uint32_t x, uint32_t y,
                         uint32_t w, uint32_t h, const VkTexImage *dst, VkImageAspectFlags aspect, uint32_t dlevel,
                         uint32_t dlayer, int32_t dx, int32_t dy, int bc7) {
    TexVkFormat f;
    if (!tex_vk_format(src->d.format, &f) || !f.astc_bw || !src->mem || x % f.astc_bw || y % f.astc_bh || dx % 4 || dy % 4)
        return 0;
    uint32_t rb = (w + f.astc_bw - 1) / f.astc_bw * 16, rows = (h + f.astc_bh - 1) / f.astc_bh;
    size_t per = (size_t)rb * rows;
    VkAstcRegion *ar = calloc(layers, sizeof *ar);
    if (!ar) return 0;
    vk_upload_flush_over(64ull << 20);
    VkBuffer sb;
    uint8_t *stg;
    VkCommandBuffer cb = vk_upload_begin_staging(per * layers, &sb, (void **)&stg);
    int ok = 1;
    for (uint32_t l = 0; l < layers && ok; l++) {
        ok = !nvn_texels_box(&src->d, level, layer + l, x / f.astc_bw * 16, y / f.astc_bh, 0, rb, rows, src->mem,
                             stg + l * per, rb, 1);
        ar[l] = (VkAstcRegion){.src = l * per, .w = w, .h = h, .slices = 1, .level = dlevel, .layer = dlayer + l,
                               .offset = {dx, dy, 0}, .lw = dst->width >> dlevel, .lh = dst->height >> dlevel};
    }
    // MKW_ASTC_PROBE_LEVEL=<n> (diagnosis): the first copies into level n read back the decoded texels and log them with
    // the source's first block (the bushes' level 11 of the terrain arrays sampled black, 01/11/2026).
    static int probe = -2;
    static volatile int32_t n_probed;
    if (probe == -2) probe = cfg_get("MKW_ASTC_PROBE_LEVEL") ? atoi(cfg_get("MKW_ASTC_PROBE_LEVEL")) : -1;
    VkBuffer rbuf = VK_NULL_HANDLE;
    VmaAllocation ralloc = NULL;
    uint8_t *rmap = NULL;
    if (ok && probe == (int)dlevel && layers == 1 && w * h <= 64 && plat_inc32(&n_probed) <= 6)
        rmap = vk_staging((VkDeviceSize)w * h * 4, &rbuf, &ralloc);
    if (ok) {
        barrier(cb, ALL_RW, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        ok = vk_astc_record(cb, sb, per * layers, ar, (int)layers, f.astc_bw, f.astc_bh, f.astc_srgb, dst->image, aspect, bc7,
                            rbuf);
        barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, ALL_RW);
    }
    uint8_t block[16];
    memcpy(block, stg, 16);
    vk_upload_end();
    if (rmap) {
        vk_upload_flush(1);
        vmaInvalidateAllocation(vk_ctx()->vma, ralloc, 0, VK_WHOLE_SIZE);
        uint32_t sum[4] = {0};
        for (uint32_t i = 0; i < w * h; i++)
            for (int k = 0; k < 4; k++) sum[k] += rmap[i * 4 + k];
        char hex[33];
        for (int k = 0; k < 16; k++) snprintf(hex + 2 * k, 3, "%02x", block[k]);
        rt_log("vk: ASTC probe: level %u -> %u layer %u %ux%u: block %s, mean RGBA %u %u %u %u", level, dlevel, dlayer,
               w, h, hex, sum[0] / (w * h), sum[1] / (w * h), sum[2] / (w * h), sum[3] / (w * h));
        vmaDestroyBuffer(vk_ctx()->vma, rbuf, ralloc);
    }
    free(ar);
    static volatile int32_t n_logged;
    if (plat_inc32(&n_logged) <= 8)
        rt_log("vk: ASTC %ux%u (fmt %#x) level %u layer %u x%u -> BC7 level %u layer %u at %d,%d: %s", w, h, src->d.format,
               level, layer, layers, dlevel, dlayer, dx, dy, ok ? "decoded" : "failed");
    return ok;
}

// vk_mem_changed over the pages wholly inside e's storage (all of its pages when it fills none): a neighbour that shares
// the first or last page does not count as a write to e. Storage that fills no page is judged by its bytes instead
// (rewritten_since_gpu).
int storage_rewritten(const Tex *e, uint32_t since, uint32_t *now) {
    uint64_t lo = (e->pub.addr + 0xfff) & ~0xfffull, hi = (e->pub.addr + e->t.L.size) & ~0xfffull;
    if (hi <= lo) lo = e->pub.addr, hi = e->pub.addr + e->t.L.size;
    return vk_mem_changed(lo, hi - lo, since, now);
}

// Storage that fills no whole page (1.8.0: a 16x1 R16F pitch target, 128 bytes, two of them rendered on alternate
// frames) shares its page with other data, and the page's write watch cannot tell a write to the texture from a write
// to its neighbours: any CPU write on that page counted as a rewrite of the storage, and the GPU's texels were never
// written back. In three Grand Prix races (drop2, RX 6700 XT, 21/11/2026) the page alone dropped 36 864 renders of two
// such textures, ~0.6 a frame, and in every one the texture's own bytes were still those of the GPU write. For such
// storage the bytes are kept at the GPU write and compared instead; under two pages, so at most 8 KiB copied on the
// replay thread (128 bytes for those two).
static int fills_no_page(const Tex *e) {
    uint64_t lo = (e->pub.addr + 0xfff) & ~0xfffull, hi = (e->pub.addr + e->t.L.size) & ~0xfffull;
    return hi <= lo && e->t.L.size <= 0x2000;
}

// MKW_TEX_WB_BYTES (default 1; 0: the page decides, as before 21/11/2026; --probe-vkexec's pitch page part fails then)
static int wb_bytes(void) {
    static int on = -1;
    if (on < 0) {
        const char *c = cfg_get("MKW_TEX_WB_BYTES");
        on = !c || !*c || atoi(c) != 0;
    }
    return on;
}

void gpu_base_note(Tex *e) {
    if (!fills_no_page(e)) return;
    if (!e->gpu_base && !(e->gpu_base = malloc(0x2000))) rt_fatal("vk: out of memory for a texture's storage copy");
    mem_copy_out(e->gpu_base, e->pub.addr, e->t.L.size);
}

int gpu_base_kept(const Tex *e) {
    if (!fills_no_page(e) || !e->gpu_base) return -1;
    uint8_t now[0x2000];
    mem_copy_out(now, e->pub.addr, e->t.L.size);
    return !memcmp(now, e->gpu_base, e->t.L.size);
}

int rewritten_since_gpu(const Tex *e) {
    uint32_t now;
    if (storage_rewritten(e, e->gpu_gen, &now) != 1) return 0;
    if (!wb_bytes() || gpu_base_kept(e) != 1) return 1;
    static volatile int32_t n_kept;
    log_limited(&n_kept, "texture %#llx %ux%u format %#x (%llu bytes, its page shared): a neighbour was written, its own "
                "bytes were not, so the GPU render goes back (MKW_TEX_WB_BYTES)", (unsigned long long)e->pub.addr,
                e->t.d.width, e->t.d.height, e->t.d.format, (unsigned long long)e->t.L.size);
    return 0;
}

// Without a write watch (vk_mem_changed -1: a kernel without userfaultfd, as the Odin 2's SteamOS ARM 7.2) every check
// uploaded the texture again: three 8192x8192 ASTC textures 627 times each at the first menus (run7, 10/11/2026), ~100
// ms of every frame. The storage's hash, once per submission, tells a rewrite apart: 1 when it is unchanged since the
// last upload. MKW_TEX_HASH_UNTRACKED=1 on any GPU, 0 never; on by default wherever the write watch is missing: also
// in a container whose seccomp profile refuses userfaultfd (#241: Docker's default, an RX 9070 at ~6 fps on the title,
// ~20 with this, 60 with userfaultfd allowed), not only on Qualcomm's kernels.
// The check runs once per submission: a rewrite of the storage later in the same submission is uploaded at the next
// one. That is the point (the 627 re-uploads were checks within a few submissions), and a texture the game rewrites
// mid-submission without a write watch reads the bytes of that submission's start.
static int hash_untracked(void) {
    static int on = -1;
    if (on < 0) {
        const char *c = cfg_get("MKW_TEX_HASH_UNTRACKED");
        uint64_t page;
        int no_watch = plat_mem_written(mem_layout()->heap.base, 0x1000, &page, 1, 0) == UINT64_MAX;
        on = c && *c ? atoi(c) != 0 : no_watch;
        if (no_watch)
            rt_log("vk: no write watch (on Linux, userfaultfd refused: in a container, allow it in its seccomp profile): "
                   "texture storage %s, much slower than with it (MKW_TEX_HASH_UNTRACKED)",
                   on ? "is hashed once per submission instead" : "is uploaded again at every check");
    }
    return on;
}

// Before a texture's first upload: its storage's hash, so the first check after it compares with what was uploaded
// instead of with 0 (an upload more for every texture). Taken before, a write while uploading only costs one upload
// more; taken after, it would hide that write.
static void note_storage(Tex *e) {
    if (!hash_untracked() || !e->t.mem) return;
    e->shash_epoch = submit_epoch;
    e->shash = vk_texpre_hash(e->t.mem, e->t.L.size);
}

static int same_storage(Tex *e) {
    if (!hash_untracked()) return 0;
    if (e->shash_epoch == submit_epoch) return 1;   // checked (and uploaded if needed) in this submission
    e->shash_epoch = submit_epoch;
    uint64_t h = vk_texpre_hash(e->t.mem, e->t.L.size);
    if (h == e->shash) return 1;
    e->shash = h;
    return 0;
}

// A mirrored texture is uploaded on first use and again when its storage pages carry a newer write generation.
void make_fresh(Tex *e) {
    if (!e->pub.mirrored || e->pub.gpu_owned) return;
    uint64_t size = e->t.L.size;
    if (!e->uploaded) {
        vk_mem_changed(e->pub.addr, size, 0, &e->wgen);   // resets the write bits before the storage is read
        note_storage(e);
        upload(e);
        return;
    }
    uint32_t now;
    int ch = vk_mem_changed(e->pub.addr, size, e->wgen, &now);
    e->wgen = now;
    e->last_ch = ch;
    if (ch < 0 && e->t.mem && same_storage(e)) return;   // not tracked: the hash decides
    if (ch) upload(e);   // 1 written, -1 not tracked and rewritten: both upload again
}

void upload_again(Tex *e) {
    if (e->pub.mirrored && !e->pub.gpu_owned) upload(e);
}

// make_fresh for draw bindings: the page check answers from this submission's write-watch snapshot (vk_memory.c
// vk_mem_changed_snap) instead of a system call per texture. A write the snapshot of the texture's region missed (made
// after it, during this replay) keeps its write-watch bit and is caught by the next submission's snapshot.
void make_fresh_snap(Tex *e) {
    if (!e->pub.mirrored || e->pub.gpu_owned) return;
    uint32_t now;
    n_checks++;
    int ch = vk_mem_changed_snap(e->pub.addr, e->t.L.size, e->uploaded ? e->wgen : 0, submit_epoch, &now);
    e->wgen = now;
    if (!e->uploaded) {
        note_storage(e);
        upload(e);
        return;
    }
    e->last_ch = ch;
    if (ch < 0 && e->t.mem && same_storage(e)) return;   // not tracked: the hash decides
    if (ch) upload(e);
}

