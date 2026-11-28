// CPU reads of GPU-written storage: mirrored textures the GPU rendered into, written back to their storage when the
// CPU reads it (an invalidate, a memcpy, ReadTexels) or waits for their fence (the small ones). From vk_tex_texel.c.
#include <stdlib.h>

#include "cfg.h"
#include "gfx.h"
#include "host.h"
#include "nvn_api.h"
#include "nvn_tex.h"
#include "tex.h"
#include "vk.h"
#include "vk_tex_internal.h"
#include "platform/platform.h"

// ---------------------------------------------------------------- CPU reads of GPU-written storage
// A mirrored texture the GPU rendered into holds its texels in its image only; its storage keeps what the CPU last put
// there. The game may still read that storage with the CPU: TotK moves its generated grass maps between pools with
// memcpy once the GPU is done, and the copy then carried zeros. Such a read writes the image back first.
//
// The images are read without the texture lock: every region's copy goes into the upload batch under it, the GPU is
// waited for once without it, and the rows go to storage under it again. Holding it through the wait (one per level
// and layer) stopped the replay's chunk threads, which look textures up under it: with the game a frame ahead
// (MKW_FENCE_AHEAD), the write-back of the small targets at each fence wait (44 a second, 2-player race) held it while
// they slept 228 ms a second on it, and they spent 24 ms a frame off the CPU in their recording (sim2-lock-ahead,
// sim2-ctr2). A hit destroyed meanwhile (no longer listed) or rewritten by the CPU is left alone. One the replay
// recorded a render into meanwhile (its gpu_seq moved) still gets the bytes read, which the CPU waited for, and stays
// dirty for that newer render, as when it was recorded after the write-back. All are checked before any is written, as each write
// is itself a CPU write to storage another hit may share. A hit stays GPU-dirty until its rows are in storage, so a
// CPU read meanwhile (vk_texture_cpu_stale, vk_texture_write_back) still sees it; wb_lock (before the texture lock)
// keeps write-backs one at a time, as the texture lock did, and the recording never takes it.
static PlatMutex wb_lock = PLAT_MUTEX_INIT;
typedef struct {
    Tex *e;
    uint64_t addr, seq;   // to know e is still listed, and whether a render was recorded into it since
    Region g;
    NVNcopyRegion r;
    TexRead rd;
    int ok;
} WbRead;

// ---------------------------------------------------------------- eager write-backs
// MKW_EAGER_WB (default 1; "ab"): the readback of a pitch or small target (wb_at_fence) the CPU has had written back
// before is recorded at the end of the submission that renders it, into staging memory, so the write-back after the
// CPU's fence wait copies rows from there without a GPU round trip. That round trip (the copy in an upload batch, sent
// and waited for) went behind every submission already queued: with the game running ahead of the GPU (MKW_FENCE_EARLY,
// MKW_FENCE_AHEAD) the game's main thread waited 27 % of its wall time there, in vk_upload_flush under
// vk_texture_write_back_small, for the animation textures of MKW's instanced characters (ahead-chains1001, 24-kart
// Grand Prix, 27/11/2026).
typedef struct { Region g; NVNcopyRegion r; TexRead rd; } EwbItem;
struct TexEwb { uint64_t fence, seq; int n; EwbItem it[]; };
static Tex **ew_pend;   // tex lock: rendered in the submission being recorded, to read back at its end
static int new_pend, cew_pend;
static int ewb_mode = -1;
static uint64_t ewb_made, ewb_used, ewb_stale;

static void ewb_free(void *arg) {   // completion thread, or the consumer: the staging buffers and the record
    TexEwb *w = arg;
    VkCtx *c = vk_ctx();
    for (int i = 0; i < w->n; i++)
        if (w->it[i].rd.sa) vmaDestroyBuffer(c->vma, w->it[i].rd.sb, w->it[i].rd.sa);
    free(w);
}

void ewb_note_locked(Tex *e) {
    if (ewb_mode < 0) {
        ewb_mode = rt_knob("EAGER_WB", "1");
        rt_log("vk: pitch and small render targets the CPU reads are read back at their submission's end%s (MKW_EAGER_WB)",
               !ewb_mode ? ": off" : ewb_mode == 2 ? " in the B blocks" : "");
    }
    if (!e->wb_seen || e->ewb_noted || !RT_KNOB_ON(ewb_mode)) return;
    if (new_pend == cew_pend && !(ew_pend = realloc(ew_pend, sizeof *ew_pend * (size_t)(cew_pend = cew_pend ? 2 * cew_pend : 64))))
        rt_fatal("vk: out of memory for eager write-backs");
    ew_pend[new_pend++] = e;
    e->ewb_noted = 1;
}

void ewb_drop_locked(Tex *e) {
    if (e->ewb_noted)
        for (int i = 0; i < new_pend; i++)
            if (ew_pend[i] == e) {
                ew_pend[i] = ew_pend[--new_pend];
                break;
            }
    e->ewb_noted = 0;
    if (e->ewb) vk_completion_add(e->ewb->fence, ewb_free, e->ewb), e->ewb = NULL;   // its copy may still run
}

// The replay thread at the end of the submission of value fence (vk_exec.c), its last command buffer outside a pass:
// the readbacks of the noted targets this submission rendered last. Returns the commands recorded.
int vk_texture_eager_wb(VkCommandBuffer cb, uint64_t fence) {
    if (!__atomic_load_n(&new_pend, __ATOMIC_RELAXED)) return 0;
    int rec = 0;
    lock();
    for (int i = 0; i < new_pend; i++) {
        Tex *e = ew_pend[i];
        e->ewb_noted = 0;
        if (!e->gpu_dirty || e->gpu_fence != fence || !e->pub.image) continue;
        const nvn_texdesc *d = &e->t.d;
        int linear = nvn_tex_is_linear(d), is3d = d->target == NVN_TEXTURE_TARGET_3D;
        uint32_t levels = linear ? 1 : e->pub.levels, layers = linear || is3d ? 1 : e->pub.layers;
        TexEwb *w = malloc(sizeof *w + sizeof *w->it * (size_t)levels * layers);
        if (!w) rt_fatal("vk: out of memory for an eager write-back");
        w->fence = fence, w->seq = e->gpu_seq, w->n = 0;
        for (uint32_t lv = 0; lv < levels; lv++)
            for (uint32_t ly = 0; ly < layers; ly++) {
                uint32_t nl = lv + e->pub.drop;
                view_t v = {.set_levels = 1, .set_layers = 1, .base_level = (uint8_t)nl, .min_layer = (uint16_t)ly};
                EwbItem *it = &w->it[w->n];
                it->r = (NVNcopyRegion){.width = (int)mip(d->width, nl),
                                        .height = d->target == 0 || d->target == 3 ? 1 : (int)mip(d->height, nl),
                                        .depth = is3d ? (int)mip(d->depth, nl) : 1};
                if (!region_of(&e->t, &v, &it->r, 0, 0, &it->g) || !image_read_check(e, &it->g, &it->r)) continue;
                image_read_stage(e, &it->g, &it->r, &it->rd);
                if (!rec++) barrier(cb, ALL_RW, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
                image_read_into(e, &it->g, &it->r, &it->rd, cb);
                w->n++;
            }
        if (!w->n) {
            free(w);
            continue;
        }
        if (e->ewb) vk_completion_add(e->ewb->fence, ewb_free, e->ewb);   // an older one never used
        e->ewb = w;
        ewb_made++;
    }
    new_pend = 0;
    if (rec) barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
                     VK_ACCESS_2_HOST_READ_BIT), rec++;
    if ((ewb_made & 4095) == 4095)
        rt_log("vk: eager write-backs: %llu recorded, %llu used, %llu stale", (unsigned long long)ewb_made,
               (unsigned long long)ewb_used, (unsigned long long)ewb_stale);
    unlock();
    return rec;
}

static int still_listed(const Tex *e, uint64_t addr) {
    for (int i = owned_lower(addr); i < nowned && owned[i]->pub.addr == addr; i++)
        if (owned[i] == e) return 1;
    return 0;
}

// hit: oldest render first. Called holding the texture lock once (lock()), which it leaves and takes again.
static void write_back_hits(Tex **hit, int k) {
    WbRead *rs = NULL;
    int n = 0, cap = 0, flush = 0;
    TexEwb **took = NULL;   // eager readbacks used here, freed at the end
    int ntook = 0;
    uint64_t done = vk_sync_fence_done();
    for (int i = 0; i < k; i++) {
        Tex *e = hit[i];
        int n0 = n;
        e->wb_seen = 1;   // its next renders are read back at their submission's end (ewb_note_locked)
        TexEwb *ew = e->ewb;
        if (ew && (ew->seq != e->gpu_seq || ew->fence > done)) ewb_stale++, ew = NULL;   // an older render, or not done
        if (ew) {   // the rows are in staging memory already: no copy, no GPU round trip
            e->ewb = NULL;
            if (!(took = realloc(took, sizeof *took * (size_t)(ntook + 1)))) rt_fatal("vk: out of memory for a texture write-back");
            took[ntook++] = ew;
            for (int j = 0; j < ew->n; j++) {
                if (n == cap && !(rs = realloc(rs, sizeof *rs * (size_t)(cap = cap ? 2 * cap : 16))))
                    rt_fatal("vk: out of memory for a texture write-back");
                rs[n++] = (WbRead){.e = e, .addr = e->pub.addr, .seq = e->gpu_seq, .g = ew->it[j].g, .r = ew->it[j].r,
                                   .rd = ew->it[j].rd};
                ew->it[j].rd.sa = NULL;   // image_read_rows frees it
            }
            ewb_used++;
            continue;
        }
        const nvn_texdesc *d = &e->t.d;
        int linear = nvn_tex_is_linear(d), is3d = d->target == NVN_TEXTURE_TARGET_3D;
        uint32_t levels = linear ? 1 : e->pub.levels, layers = linear || is3d ? 1 : e->pub.layers;
        for (uint32_t lv = 0; lv < levels; lv++)
            for (uint32_t ly = 0; ly < layers; ly++) {
                uint32_t nl = lv + e->pub.drop;   // the NVN level of image level lv
                view_t v = {.set_levels = 1, .set_layers = 1, .base_level = (uint8_t)nl, .min_layer = (uint16_t)ly};
                if (n == cap && !(rs = realloc(rs, sizeof *rs * (size_t)(cap = cap ? 2 * cap : 16))))
                    rt_fatal("vk: out of memory for a texture write-back");
                WbRead *w = &rs[n];
                *w = (WbRead){.e = e, .addr = e->pub.addr, .seq = e->gpu_seq};
                w->r = (NVNcopyRegion){.width = (int)mip(d->width, nl),
                                       .height = d->target == 0 || d->target == 3 ? 1 : (int)mip(d->height, nl),
                                       .depth = is3d ? (int)mip(d->depth, nl) : 1};
                if (region_of(&e->t, &v, &w->r, 0, 0, &w->g) && image_read_record(e, &w->g, &w->r, &w->rd)) n++, flush = 1;
            }
        if (n == n0) e->gpu_dirty = 0;   // nothing of it can be read back (ASTC, packed depth-stencil)
    }
    if (!n) {
        free(rs);
        for (int i = 0; i < ntook; i++) ewb_free(took[i]);
        free(took);
        return;
    }
    if (flush) {
        unlock();
        vk_upload_flush(1);
        lock();
    }
    for (int i = 0; i < n; i++) {   // all checked first: the writes below are CPU writes to the storage
        WbRead *w = &rs[i];
        if (!still_listed(w->e, w->addr)) continue;   // destroyed meanwhile
        w->ok = !rewritten_since_gpu(w->e);
        if (w->e->gpu_seq == w->seq) w->e->gpu_dirty = 0;   // else a newer render, recorded meanwhile, goes back later
    }
    for (int i = 0; i < n; i++) {
        WbRead *w = &rs[i];
        uint8_t *buf = w->ok ? malloc(w->g.img * (size_t)w->g.nz) : NULL;
        if (w->ok && !buf) rt_fatal("vk: out of memory for a texture write-back");
        image_read_rows(&w->g, &w->rd, buf);
        if (buf) storage_texels(&w->e->t, &w->g, &w->r, buf, 1), free(buf);
    }
    free(rs);
    for (int i = 0; i < ntook; i++) ewb_free(took[i]);   // (their staging went with image_read_rows)
    free(took);
}

// Mirrored textures overlapping [addr, addr+size) that the GPU wrote since their last write-back.
static int owned_dirty(uint64_t addr, uint64_t size, Tex **hit) {
    int n = 0;
    for (int i = owned_lower(addr > max_owned ? addr - max_owned : 0); i < nowned && owned[i]->pub.addr < addr + size; i++) {
        Tex *e = owned[i];
        if (__atomic_load_n(&e->gpu_dirty, __ATOMIC_RELAXED) && e->pub.addr + e->t.L.size > addr) {
            if (!hit) return 1;
            hit[n++] = e;
        }
    }
    return n;
}

size_t vk_texture_texel_span(const tex_t *t, const view_t *v, const NVNcopyRegion *r, uint64_t row, uint64_t img) {
    Region g;
    if (!region_of(t, v, r, row, img, &g)) return 0;
    return (size_t)(g.nz - 1) * g.img + (g.hb - 1) * g.row + g.wb;
}

int vk_texture_cpu_stale(uint64_t addr, uint64_t size) {
    if (!size || !__atomic_load_n(&nowned, __ATOMIC_ACQUIRE)) return 0;
    plat_mutex_lock_shared(&owned_lock);
    int dirty = owned_dirty(addr, size, NULL);
    plat_mutex_unlock_shared(&owned_lock);
    return dirty;
}

// The newest submission recorded to write a GPU-written mirrored texture overlapping [addr, addr+size); 0: none.
uint64_t vk_texture_dirty_fence(uint64_t addr, uint64_t size) {
    if (!size || !__atomic_load_n(&nowned, __ATOMIC_ACQUIRE)) return 0;
    lock();
    uint64_t f = 0;
    for (int i = owned_lower(addr > max_owned ? addr - max_owned : 0); i < nowned && owned[i]->pub.addr < addr + size; i++) {
        const Tex *e = owned[i];
        if (!e->gpu_dirty || e->pub.addr + e->t.L.size <= addr) continue;
        if (e->gpu_fence > f) f = e->gpu_fence;
        if (e->wr_fence > f) f = e->wr_fence;
    }
    unlock();
    return f;
}

// The image is the newest content only while nothing wrote the storage since the GPU render: TotK finalizes grass maps,
// reuses their storage for new data and memcpys that data on, and writing the old image back first handed the copy
// a stale map or stale resource bytes (wrong textures in the distance). Every hit is checked before any is written
// back, since a write-back is itself a CPU write to storage another hit may share; the hits then go back oldest render
// first, so where two descriptors share storage the newer render wins.
void vk_texture_write_back(uint64_t addr, uint64_t size) {
    plat_mutex_lock(&wb_lock);
    lock();
    Tex **hit = malloc(sizeof *hit * (size_t)(nowned + 1));
    if (!hit) rt_fatal("vk: out of memory for a texture write-back");
    int n = owned_dirty(addr, size, hit), k = 0;   // owned[] cannot change while tex_lock is held
    for (int i = 0; i < n; i++) {
        if (rewritten_since_gpu(hit[i])) hit[i]->gpu_dirty = 0;
        else hit[k++] = hit[i];
    }
    for (int i = 1; i < k; i++)
        for (int j = i; j > 0 && hit[j - 1]->gpu_seq > hit[j]->gpu_seq; j--) {
            Tex *x = hit[j];
            hit[j] = hit[j - 1], hit[j - 1] = x;
        }
    write_back_hits(hit, k);
    for (int i = 0; i < k; i++) perf_count(PERF_CNT_WB);
    for (int i = k; i < n; i++) perf_count(PERF_CNT_WB_SKIP);
    free(hit);
    unlock();
    plat_mutex_unlock(&wb_lock);
}

// TotK's grass system renders a 128x128 RGBA8 map of cut and trampled grass into a CPU-cached pool every frame and its
// CPU code reads the pool bytes without nvnMemoryPoolInvalidateMappedRange (cut reactions: blades, insects, the cut
// itself). On the Switch the bytes are there once the frame's fence has passed. So when the
// CPU waits for a fence, mirrored targets up to WB_SMALL bytes rendered by work up to that fence go back to their
// storage, unless the CPU rewrote it since. Larger targets still go back on an invalidate, memcpy or ReadTexels.
// Pitch textures go back at the fence whatever their size (wb_at_fence). MKW fills, on the GPU, three pitch RGBA16F
// textures for each animation of its instanced characters (scale, rotation, translation: 8x1218 texels, 0x80 bytes a
// row, 152 KiB each) and its CPU code reads the pool bytes with plain loads, every frame, for each bone (1.8.0:
// main+0x42bea0). Left on the GPU, the storage stayed as the heap had it: zeros gave the bones scale 0 and stale bytes
// gave huge matrices, the giant triangles over Shy Guy Bazaar (#72; free roam there, the same drive: 17 of 600 frames
// wrong, then 0).
int vk_texture_small_dirty(void) { return __atomic_load_n(&small_pending, __ATOMIC_ACQUIRE); }

void vk_texture_write_back_small(uint64_t fence) {
    plat_mutex_lock(&wb_lock);
    lock();
    __atomic_store_n(&small_pending, 0, __ATOMIC_RELAXED);
    plat_mutex_lock_shared(&owned_lock);
    int n = 0, later = 0;
    for (int i = 0; i < nowned; i++) n += owned[i]->gpu_dirty && wb_at_fence(&owned[i]->t.d, owned[i]->t.L.size);
    Tex **hit = n ? malloc(sizeof *hit * (size_t)n) : NULL;
    if (n && !hit) rt_fatal("vk: out of memory for a texture write-back");
    n = 0;
    for (int i = 0; i < nowned; i++) {
        Tex *e = owned[i];
        if (!e->gpu_dirty || !wb_at_fence(&e->t.d, e->t.L.size)) continue;
        if (e->gpu_fence > fence) later = 1;
        else hit[n++] = e;
    }
    plat_mutex_unlock_shared(&owned_lock);
    if (later) __atomic_store_n(&small_pending, 1, __ATOMIC_RELEASE);
    int k = 0;
    for (int i = 0; i < n; i++) {
        if (rewritten_since_gpu(hit[i])) hit[i]->gpu_dirty = 0;
        else hit[k++] = hit[i];
    }
    for (int i = 1; i < k; i++)
        for (int j = i; j > 0 && hit[j - 1]->gpu_seq > hit[j]->gpu_seq; j--) {
            Tex *x = hit[j];
            hit[j] = hit[j - 1], hit[j - 1] = x;
        }
    static volatile int32_t n_pitch;   // (before the write-back, which leaves the lock: a hit may be gone after it)
    for (int i = 0; i < k; i++)
        if (hit[i]->t.L.size > WB_SMALL)
            log_limited(&n_pitch, "pitch texture %#llx %ux%u format %#x (%llu bytes) written back to its storage at fence %llu",
                        (unsigned long long)hit[i]->pub.addr, hit[i]->t.d.width, hit[i]->t.d.height, hit[i]->t.d.format,
                        (unsigned long long)hit[i]->t.L.size, (unsigned long long)fence);
    write_back_hits(hit, k);
    for (int i = 0; i < k; i++) perf_count(PERF_CNT_WB);
    for (int i = k; i < n; i++) perf_count(PERF_CNT_WB_SKIP);
    free(hit);
    unlock();
    plat_mutex_unlock(&wb_lock);
}

