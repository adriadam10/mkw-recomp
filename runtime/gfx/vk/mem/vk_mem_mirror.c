#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "gfx.h"
#include "host.h"
#include "nvn_api.h"
#include "nvn_pool_index.h"
#include "vk.h"
#include "vk_exec.h"
#include "vk_mem_internal.h"
#include "platform/platform.h"

// ---------------------------------------------------------------- VRAM mirror of buffer-bound pools
// An imported pool keeps vertices, indices, constants
// and storage buffers in system memory, so a discrete GPU reads them over PCIe on every draw (GTX 1050 Ti and RTX
// 3090 Ti: about 110 ms of GPU time per frame in the field). In mirror mode (device class discrete, MKW_MEMORY) an
// imported pool gets a device-local buffer over its imported range the first time the replay uses it as a vertex,
// index, uniform, storage or indirect buffer (vk_mem_use). From then on those bindings, and the buffer copies, fills
// and uniform updates that touch the pool (vk_mem_pick), use the mirror at the same offsets. Guest memory stays what
// the CPU reads and writes; textures and every host path that reads or writes pool bytes keep using it.
// - CPU -> mirror: the first access of a submission to a mirror syncs it (mirror_sync): the 2 MiB write-watch
//   snapshots of its regions for this epoch (snap_regions, shared with the texture checks: the OS bits keep one
//   consumer, the shared page generations) and every page stamped after the mirror's mgen goes into a copy; so do the
//   nvnMemoryPoolFlushMappedRange ranges. The first use copies the whole range. The copies run in a prologue command
//   buffer that vk_sync.c submits right before the part that accessed the mirror first, between full barriers.
//   vk_mem_mirror_begin snaps the regions of the pools the previous submission accessed in a few merged queries.
// - GPU -> guest: what the replay writes into a mirror (storage buffers its shaders write, buffer copies, fills,
//   uniform updates, texture-to-buffer copies: vk_mem_gpu_wrote) goes back into the imported buffer at the end of the
//   submission, exact byte ranges, before the fence signals (vk_mem_mirror_end).
// - MKW_MEMORY=verify: every submission also copies up to VERIFY_BYTES of mirror pages, round robin, into host memory;
//   at its fence the pages the CPU has not written since their sync are compared with guest memory.
// A pool stays imported when its mirror would not fit the VRAM budget, its range is not write-watched, it overlaps
// another pool or the allocation fails.
#define VERIFY_BYTES (4u << 20)
#define VERIFY_SAMPLES 64
#define VERIFY_LOG 16
enum { MR_BUDGET, MR_UNTRACKED, MR_OVERLAP, MR_ALLOC, MR_N };

int vk_mirror_on;
int mirror_verify;
int mirror_gap = 2;             // pre-snap: hot pools at most this many 2 MiB regions apart share one query
int mirror_cpu_gen = 1;         // MKW_MIRROR_CPU_GEN (default 1; "ab"): sync by the CPU stamps only (vk_mem_gen.c)
static PoolRec **mlist;                // pool_lock: mirrored pools by import_lo
static int nm, mcap;
static uint32_t msub;                  // replay: number of the submission being recorded
static int mm_active, mm_inited;       // replay: this submission's write-backs merge by bytes (vk_mem_mmerge.c)
// Whose: a pool without a shadow (every pool on Windows, MKW_SHADOW off there). A shadowed pool's write-back reaches
// guest memory through the shadow's own merge (vk_mem_merge.c), which keeps the CPU's bytes: with the merge on every
// pool, a Linux race (shadows) counted none kept by it (MKW_MIRROR_MERGE_CENSUS, wm1 20/11/2026), and it cost +0.2-0.3
// ms of GPU a frame over the copy (MKW_MIRROR_MERGE_TIME, wab5/wab5i). MKW_MIRROR_MERGE_ALL=1 merges those too.
static int merged(const PoolRec *r) { return mm_active && (!r->smap || mmerge_all); }
static uint32_t mepoch;                // replay: its write-watch snapshot epoch (vk_texture_epoch)
typedef struct { PoolRec *r; uint64_t off, size; } MRange;
// The replay's chunks record on several threads (MKW_REPLAY_PAR): a pool's first access of the submission (creation,
// sync) and the lists go under use_lock; a pool already synced in this submission is read without it.
static PlatMutex use_lock = PLAT_MUTEX_INIT;
static MRange *pend, *wbl;             // use_lock: copies for the next prologue; GPU writes into mirrors this submission
static int npend, cpend, nwbl, cwbl;
// The list a prologue records, swapped with pend under use_lock: the parts of a submission are sent while its later
// chunks are still being recorded (vk_exec_par.c send_ready), and those threads add to pend meanwhile. Read in place
// and emptied at the end, the prologue dropped what they added (~260 ranges every 600 frames of a four-player race, so
// pages whose write never reached VRAM until they were written again) and sorted an array they could move: with the
// array moved at every add, 3 of 6 chunked replays of such a frame died of it in malloc or copy_ranges, and none of 14
// with the swap (22/11/2026).
static MRange *psend;
static int csend;
// MKW_MIRROR_REUP=1 (diagnosis, replay only): the GPU writes of the last submission, to count the pages the next sync
// uploads again although only the GPU wrote them since (NX2navi 6c971b7 saved 57 of 60 MiB a frame that way; here the
// write-back is a GPU copy that stamps no page, so the count should stay near zero; mir-reup, 14/11/2026).
static MRange *wbprev;
static int nwbprev, cwbprev, reup_diag = -1;
static VkBufferCopy *mregions;         // replay: vkCmdCopyBuffer regions
static int cregions;
static PlatMutex flush_lock = PLAT_MUTEX_INIT;

typedef struct {
    uint64_t subs, syncs, upload_bytes, upload_copies, first_n, first_bytes, wb_bytes, wb_copies, wb_ranges;
    uint64_t presnap_queries, sync_queries, presnap_t, sync_t, prologues, presnap_bytes, reup_bytes, gpu_kept_bytes;
} MirrorAcc;
static MirrorAcc mrep;                 // replay: this submission
static MirrorAcc macc;                 // mirror_stat_lock: since the last summary
static PlatMutex mirror_stat_lock = PLAT_MUTEX_INIT;
volatile int64_t m_live_n, m_live_bytes;
static volatile int64_t m_refused_n[MR_N], m_refused_bytes[MR_N];
static volatile int64_t m_flush_calls, m_flush_bytes, v_pages, v_skipped, v_mismatch;


static void range_add(MRange **a, int *n, int *cap, PoolRec *r, uint64_t off, uint64_t size) {
    if (*n && (*a)[*n - 1].r == r && (*a)[*n - 1].off + (*a)[*n - 1].size == off) {
        (*a)[*n - 1].size += size;
        return;
    }
    if (*n == *cap) vk_mem_grow((void **)a, cap, sizeof **a);
    (*a)[(*n)++] = (MRange){r, off, size};
}

static int range_cmp(const void *x, const void *y) {
    const MRange *a = x, *b = y;
    if (a->r != b->r) return (uintptr_t)a->r < (uintptr_t)b->r ? -1 : 1;
    return a->off < b->off ? -1 : a->off > b->off;
}

// Records the ranges as one vkCmdCopyBuffer per pool, overlapping and adjacent ranges merged; to_mirror: buf -> mbuf,
// else mbuf -> buf. Returns the commands recorded; adds what they move to *bytes and *copies.
static int copy_ranges(VkCommandBuffer cb, MRange *a, int n, int to_mirror, uint64_t *bytes, uint64_t *copies) {
    qsort(a, (size_t)n, sizeof *a, range_cmp);
    int rec = 0;
    for (int i = 0; i < n;) {
        PoolRec *r = a[i].r;
        uint64_t lim = r->import_hi - r->import_lo;
        int k = 0;
        for (; i < n && a[i].r == r; i++) {
            uint64_t lo = a[i].off, hi = a[i].off + a[i].size;
            if (hi > lim) hi = lim;
            if (lo >= hi) continue;
            if (k && lo <= mregions[k - 1].srcOffset + mregions[k - 1].size) {
                uint64_t end = mregions[k - 1].srcOffset + mregions[k - 1].size;
                if (hi > end) mregions[k - 1].size += hi - end;
                continue;
            }
            if (k == cregions) vk_mem_grow((void **)&mregions, &cregions, sizeof *mregions);
            mregions[k++] = (VkBufferCopy){lo, lo, hi - lo};
        }
        if (!k) continue;
        if (to_mirror) vkCmdCopyBuffer(cb, r->buf, r->mbuf, (uint32_t)k, mregions);
        else vkCmdCopyBuffer(cb, r->mbuf, r->buf, (uint32_t)k, mregions);
        rec++;
        for (int j = 0; j < k; j++) *bytes += mregions[j].size;
        *copies += (uint64_t)k;
    }
    return rec;
}

static void mbarrier(VkCommandBuffer cb, VkPipelineStageFlags2 ss, VkAccessFlags2 sa, VkPipelineStageFlags2 ds,
                     VkAccessFlags2 da) {
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = ss, .srcAccessMask = sa,
                           .dstStageMask = ds, .dstAccessMask = da};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
}

static void refuse(PoolRec *r, int why, const char *what) {
    r->mstate = MS_REFUSED;
    uint64_t n = r->import_hi - r->import_lo;
    if (plat_inc64(&m_refused_n[why]) <= 4) {
        char f[160];
        flag_names(r->flags, f, sizeof f);
        rt_log("mem: pool %#llx+%#llx (%s) stays imported: %s", (unsigned long long)r->lo, (unsigned long long)r->size,
               f, what);
    }
    plat_add64(&m_refused_bytes[why], (int64_t)n);
}

// Replay thread: the pool's mirror on its first buffer use.
static void mirror_create(PoolRec *r) {
    VkCtx *c = vk_ctx();
    uint64_t n = r->import_hi - r->import_lo;
    r->mstate = MS_REFUSED;
    __atomic_add_fetch(&mirror_gen, 1, __ATOMIC_RELEASE);
    if (r->kind != POOL_IMPORT || !r->buf || !n) return;
    plat_mutex_lock_shared(&pool_lock);
    int i = pidx_upper(&pool_index, r->lo) - 1;
    while (i >= 0 && pool_index.e[i].buf != r) i--;
    int alone = i >= 0 && (i == 0 || pool_index.maxhi[i - 1] <= pool_index.e[i].lo) &&
                (i + 1 == pool_index.n || pool_index.e[i + 1].lo >= pool_index.e[i].hi);
    plat_mutex_unlock_shared(&pool_lock);
    if (i < 0) return;   // finalized meanwhile
    if (!alone) return refuse(r, MR_OVERLAP, "it overlaps another pool");
    uint64_t r0 = r->import_lo >> SNAP_SHIFT, r1 = (r->import_hi - 1) >> SNAP_SHIFT;
    if (r1 >= SNAP_REGIONS) return refuse(r, MR_UNTRACKED, "outside the write-watched address space");
    uint64_t used, budget;
    int exact;
    if (vk_mem_vram(&used, &budget, &exact) && used + n + budget / 8 > budget) {
        char why[128];
        snprintf(why, sizeof why, "its %.1f MiB mirror does not fit the VRAM budget (%.0f of %.0f MiB in use)",
                 n / 1048576.0, used / 1048576.0, budget / 1048576.0);
        return refuse(r, MR_BUDGET, why);
    }
    gen_lock_x();
    mrep.sync_queries += snap_regions(r0, r1, mepoch);
    int untracked = 0;
    for (uint64_t k = r0; k <= r1; k++) untracked |= snap_untracked[k];
    r->mgen = gen_now;   // the whole-range copy below holds every write observed so far
    plat_mutex_unlock(&gen_lock);
    if (untracked) return refuse(r, MR_UNTRACKED, "its range is not write-watched");
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = n, .usage = pool_usage(c),
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
                                  .requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT};
    VkResult res = vmaCreateBuffer(c->vma, &bi, &ai, &r->mbuf, &r->malloc_, NULL);
    if (res != VK_SUCCESS) {
        r->mbuf = VK_NULL_HANDLE, r->malloc_ = NULL;
        char why[96];
        snprintf(why, sizeof why, "device-local allocation of %.1f MiB failed (%d)", n / 1048576.0, (int)res);
        return refuse(r, MR_ALLOC, why);
    }
    plat_mutex_lock(&pool_lock);
    if (nm == mcap) vk_mem_grow((void **)&mlist, &mcap, sizeof *mlist);
    int at = nm;
    while (at > 0 && mlist[at - 1]->import_lo > r->import_lo) at--;
    memmove(mlist + at + 1, mlist + at, (size_t)(nm - at) * sizeof *mlist);
    mlist[at] = r;
    nm++;
    plat_mutex_unlock(&pool_lock);
    range_add(&pend, &npend, &cpend, r, 0, n);
    __atomic_store_n(&r->acc_sub, msub, __ATOMIC_RELEASE);
    // After mbuf, the copy queued and acc_sub: vk_mem_use reads it without use_lock, and what it returns goes to the GPU
    // with the copies queued by then.
    __atomic_store_n(&r->mstate, MS_ON, __ATOMIC_RELEASE);
    mrep.first_n++, mrep.first_bytes += n;
    plat_inc64(&m_live_n);
    plat_add64(&m_live_bytes, (int64_t)n);
}

void mirror_forget(PoolRec *r) {
    __atomic_add_fetch(&mirror_gen, 1, __ATOMIC_RELEASE);
    for (int i = 0; i < nm; i++)
        if (mlist[i] == r) {
            memmove(mlist + i, mlist + i + 1, (size_t)(nm - i - 1) * sizeof *mlist);
            nm--;
            plat_dec64(&m_live_n);
            plat_add64(&m_live_bytes, -(int64_t)(r->import_hi - r->import_lo));
            break;
        }
}

// Finalized mirrored pools waiting for mirror_retire; vk_mem_shutdown destroys those whose callback never ran.
static PoolRec **retiring;
static int nretiring, cretiring;
static PlatMutex retire_lock = PLAT_MUTEX_INIT;

void mirror_retiring(PoolRec *r) {
    plat_mutex_lock(&retire_lock);
    if (nretiring == cretiring) vk_mem_grow((void **)&retiring, &cretiring, sizeof *retiring);
    retiring[nretiring++] = r;
    plat_mutex_unlock(&retire_lock);
}

int retiring_count(void) {
    plat_mutex_lock(&retire_lock);
    int n = nretiring;
    plat_mutex_unlock(&retire_lock);
    return n;
}

void mirror_retire(void *arg) {
    plat_mutex_lock(&retire_lock);
    for (int i = 0; i < nretiring; i++)
        if (retiring[i] == arg) { retiring[i] = retiring[--nretiring]; break; }
    plat_mutex_unlock(&retire_lock);
    destroy_rec(vk_ctx(), arg);
}

// vk_mem_shutdown, pool_lock held, after the indexed pools went: mirrors went with their pools; finalized pools whose
// callback never ran go now.
void mirror_reset(VkCtx *c) {
    nm = npend = nwbl = 0;
    mmerge_shutdown(c);
    mm_inited = mm_active = 0;
    for (int i = 0; i < nretiring; i++) destroy_rec(c, retiring[i]);
    nretiring = 0;
}

// Replay thread, the first access of this submission: queues the copies of the pages written since the last sync.
static void mirror_sync(PoolRec *r) {
    uint64_t t0 = qpc();
    uint64_t lo = r->import_lo, hi = r->import_hi;
    uint64_t r0 = lo >> SNAP_SHIFT, r1 = (hi - 1) >> SNAP_SHIFT;
    int excl = 0;
    gen_lock_s();
    for (uint64_t k = r0; k <= r1; k++)
        if (snap_at[k] != mepoch) { excl = 1; break; }
    if (excl) {
        plat_mutex_unlock_shared(&gen_lock);
        gen_lock_x();
        mrep.sync_queries += snap_regions(r0, r1, mepoch);
    }
    uint32_t since = r->mgen;
    for (uint64_t k = r0; k <= r1; k++) {
        if (reg_max[k] <= since) continue;
        uint64_t a = k << SNAP_SHIFT, b = a + (1ull << SNAP_SHIFT);
        if (a < lo) a = lo;
        if (b > hi) b = hi;
        const uint32_t *ch = gen_chunk[a >> GEN_CHUNK_SHIFT];   // a 2 MiB region lies in one chunk
        if (!ch) continue;
        // pages only the GPU wrote since the last sync (their fence stamped gen_chunk, not cpu_gen_chunk) are in the
        // mirror already; a chunk without a CPU stamp ever has none to copy
        const uint32_t *cc = RT_KNOB_ON(mirror_cpu_gen) ? cpu_gen_chunk[a >> GEN_CHUNK_SHIFT] : ch;
        for (uint64_t x = a; x < b; x += 1ull << GEN_PAGE_SHIFT)
            if (ch[(x >> GEN_PAGE_SHIFT) & (GEN_PER_CHUNK - 1)] > since) {
                if (!cc || cc[(x >> GEN_PAGE_SHIFT) & (GEN_PER_CHUNK - 1)] <= since) {
                    mrep.gpu_kept_bytes += 1ull << GEN_PAGE_SHIFT;
                    continue;
                }
                range_add(&pend, &npend, &cpend, r, x - lo, 1ull << GEN_PAGE_SHIFT);
                if (reup_diag > 0)
                    for (int i = 0; i < nwbprev; i++)
                        if (wbprev[i].r == r && wbprev[i].off < x - lo + (1ull << GEN_PAGE_SHIFT) &&
                            wbprev[i].off + wbprev[i].size > x - lo) {
                            mrep.reup_bytes += 1ull << GEN_PAGE_SHIFT;
                            break;
                        }
            }
    }
    r->mgen = gen_now;
    if (excl) plat_mutex_unlock(&gen_lock);
    else plat_mutex_unlock_shared(&gen_lock);
    if (__atomic_load_n(&r->fl_hi, __ATOMIC_ACQUIRE)) {
        plat_mutex_lock(&flush_lock);
        uint64_t a = r->fl_lo, b = r->fl_hi;
        r->fl_hi = 0;
        plat_mutex_unlock(&flush_lock);
        a = a > lo ? a & ~0xfffull : lo;
        b = b < hi ? (b + 0xfff) & ~0xfffull : hi;
        if (a < b) range_add(&pend, &npend, &cpend, r, a - lo, b - a);
    }
    mrep.syncs++;
    mrep.sync_t += qpc() - t0;
    // Last: a thread that finds it set uses the mirror without use_lock, and its chunk may be sent at once, so the copies
    // are in pend by then. Set first, a chunk used a mirror whose copies were not queued yet, ~900 times every 600
    // frames of a four-player race.
    __atomic_store_n(&r->acc_sub, msub, __ATOMIC_RELEASE);
}

void vk_mem_use(VkMemSpan *sp) {
    PoolRec *r = sp->pool;
    if (__atomic_load_n(&r->mstate, __ATOMIC_ACQUIRE) != MS_ON || __atomic_load_n(&r->acc_sub, __ATOMIC_ACQUIRE) != msub) {
        plat_mutex_lock(&use_lock);
        if (r->mstate == MS_NONE) mirror_create(r);
        if (r->mstate == MS_ON && r->acc_sub != msub) mirror_sync(r);
        plat_mutex_unlock(&use_lock);
        if (r->mstate != MS_ON) return;
    }
    sp->buffer = r->mbuf;
}

void vk_mem_pick(VkMemSpan *sp) {
    PoolRec *r = sp->pool;
    if (!r || __atomic_load_n(&r->mstate, __ATOMIC_ACQUIRE) != MS_ON) return;
    if (__atomic_load_n(&r->acc_sub, __ATOMIC_ACQUIRE) != msub) {
        plat_mutex_lock(&use_lock);
        if (r->acc_sub != msub) mirror_sync(r);
        plat_mutex_unlock(&use_lock);
    }
    sp->buffer = r->mbuf;
}

int vk_mem_mirror_probe(uint64_t addr) {
    VkMemSpan sp;
    if (!vk_mirror_on || !vk_mem_resolve(addr, 1, &sp) || !sp.pool) return 0;
    mepoch = vk_texture_epoch();
    vk_mem_use(&sp);
    return ((PoolRec *)sp.pool)->mstate == MS_ON;
}

void vk_mem_gpu_wrote(const VkMemSpan *sp, uint64_t size) {
    PoolRec *r = sp->pool;
    if (!r || r->mstate != MS_ON || sp->buffer != r->mbuf || !size) return;
    plat_mutex_lock(&use_lock);
    range_add(&wbl, &nwbl, &cwbl, r, sp->offset, size);
    if (merged(r)) mmerge_note(r, sp->offset, size);
    plat_mutex_unlock(&use_lock);
}

void nvn_pool_flushed(const pool_t *p, uint64_t off, uint64_t size) {
    if (!vk_mirror_on || !size || !p->mem) return;
    plat_inc64(&m_flush_calls);
    plat_add64(&m_flush_bytes, (int64_t)size);
    plat_mutex_lock_shared(&pool_lock);
    const pidx_ent *e = pidx_find(&pool_index, (uintptr_t)p->mem, 0);
    PoolRec *r = e && e->key == p ? e->buf : NULL;
    if (r && r->mstate == MS_ON) {
        uint64_t a = (uintptr_t)p->mem + off, b = a + size;
        plat_mutex_lock(&flush_lock);
        if (!r->fl_hi) r->fl_lo = a, r->fl_hi = b;
        else {
            if (a < r->fl_lo) r->fl_lo = a;
            if (b > r->fl_hi) r->fl_hi = b;
        }
        plat_mutex_unlock(&flush_lock);
    }
    plat_mutex_unlock_shared(&pool_lock);
}


void vk_mem_mirror_begin(void) {
    if (!vk_mirror_on) {   // import mode: the texture checks' snapshots still skip frozen regions
        if (!freeze_idle) return;
        gen_lock_x();
        ww_ep++;
        freeze_pass();
        plat_mutex_unlock(&gen_lock);
        return;
    }
    uint64_t t0 = qpc();
    if (!mm_inited) mm_inited = 1, mmerge_init(vk_ctx());
    mm_active = mmerge_ready();
    uint32_t prev = msub;
    msub = msub + 1 ? msub + 1 : 1;
    __atomic_add_fetch(&mirror_gen, 1, __ATOMIC_RELEASE);
    mepoch = vk_texture_epoch();
    uint64_t q = 0, run0 = 0, run1 = 0;
    int have = 0;
    plat_mutex_lock_shared(&pool_lock);
    gen_lock_x();
    ww_ep++;
    uint64_t b0 = ww_bytes;
    for (int i = 0; i < nm; i++) {
        const PoolRec *r = mlist[i];
        if (r->acc_sub != prev) continue;
        uint64_t a = r->import_lo >> SNAP_SHIFT, b = (r->import_hi - 1) >> SNAP_SHIFT;
        if (have && a <= run1 + 1 + (uint64_t)mirror_gap) {
            if (b > run1) run1 = b;
            continue;
        }
        if (have) q += snap_regions(run0, run1, mepoch);
        run0 = a, run1 = b, have = 1;
    }
    if (have) q += snap_regions(run0, run1, mepoch);
    mrep.presnap_bytes += ww_bytes - b0;
    if (freeze_idle) freeze_pass();
    plat_mutex_unlock(&gen_lock);
    plat_mutex_unlock_shared(&pool_lock);
    mrep.subs++;
    mrep.presnap_queries += q;
    mrep.presnap_t += qpc() - t0;
}

int vk_mem_mirror_pending(void) {
    plat_mutex_lock(&use_lock);
    int n = npend;
    plat_mutex_unlock(&use_lock);
    return n + mmerge_pending();
}

void vk_mem_mirror_prologue(VkCommandBuffer cb) {
    plat_mutex_lock(&use_lock);
    MRange *a = pend;
    int n = npend, cap = cpend;
    pend = psend, cpend = csend, npend = 0;
    psend = a, csend = cap;
    plat_mutex_unlock(&use_lock);
    if (!n) {   // only the bases of the merged write-backs (vk_mem_mmerge.c), which need no barrier before them
        mmerge_prologue(cb, 0);
        return;
    }
    // A shadowed pool's mirror is copied from its shadow, which holds the writes stamped up to its sgen only. A pool
    // already synced for this submission (by the early thread or an earlier part) is not synced again, so the shadow
    // keeps the bytes of that sync, but the mirror sync's query stamps the writes made since and listed them, and its
    // mgen claimed them: they were never copied again (#61, the menu labels drew another frame's glyphs). Measured
    // after the shadow sync: 1,242-2,101 of every ~200,000 mirror pages copied had a stamp above their pool's sgen; at
    // the mirror sync, ~3,400 listed pages per 20,000 syncs were of pools already synced for the submission and none of
    // a pool left untouched (g61d, g61h, 13/11/2026). Capped here, those pages go again at the next mirror sync, once
    // the shadow has them.
    gen_lock_x();
    for (int i = 0; i < n; i++) {
        PoolRec *r = a[i].r;
        if (r->smap && r->mgen > r->sgen) r->mgen = r->sgen;
    }
    plat_mutex_unlock(&gen_lock);
    // #43 (RX 6650 XT, a discrete card: mirror mode) hung after every marked command had finished
    uint32_t crumb = vk_crumb_op(cb, "mirror copies at the submission's start", (uint32_t)n, 0);
    mbarrier(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
             VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
    copy_ranges(cb, a, n, 1, &mrep.upload_bytes, &mrep.upload_copies);
    mmerge_prologue(cb, 1);   // the bases of the ranges this part writes, from the mirror just uploaded
    mbarrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
             VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    vk_crumb_end(cb, VK_CRUMB_OP, crumb);
    mrep.prologues++;
}

// ---- verify mode
typedef struct { PoolRec *r; uint64_t off, len, dst; uint32_t mgen; } VSample;
typedef struct { uint32_t sub; int n; VkBuffer buf; VmaAllocation alloc; const uint8_t *map; VSample s[VERIFY_SAMPLES]; } VJob;
static int ver_i;
static uint64_t ver_off;

// The CPU wrote the page since the mirror's sync that mgen records: stamped later, or its write-watch bit is set.
static int page_dirty(uint64_t a, uint32_t mgen) {
    uint32_t *s = gen_slot(a, 0);
    if (s && __atomic_load_n(s, __ATOMIC_RELAXED) > mgen) return 1;
    uint64_t p[1];
    return mem_written_peek(a, 1ull << GEN_PAGE_SHIFT, p, 1) != 0;   // UINT64_MAX (not watched) counts as written
}

static void verify_cb(void *arg) {   // completion thread
    VJob *j = arg;
    static volatile int32_t logged;
    uint64_t pages = 0, skipped = 0, bad = 0;
    for (int i = 0; i < j->n; i++) {
        const VSample *s = &j->s[i];
        PoolRec *r = s->r;
        uint32_t wb = __atomic_load_n(&r->wb_sub, __ATOMIC_RELAXED);
        if (wb != j->sub && wb - j->sub < 0x80000000u) {   // a later submission wrote back into it
            skipped += s->len >> GEN_PAGE_SHIFT;
            continue;
        }
        for (uint64_t o = 0; o < s->len; o += 1ull << GEN_PAGE_SHIFT) {
            uint64_t a = r->import_lo + s->off + o;
            const uint8_t *m = j->map + s->dst + o, *g = (const uint8_t *)(uintptr_t)a;
            pages++;
            if (!memcmp(m, g, 1ull << GEN_PAGE_SHIFT)) continue;
            if (page_dirty(a, s->mgen)) { skipped++; continue; }
            bad++;
            if (plat_inc32(&logged) <= VERIFY_LOG) {
                int k = 0, e = (1 << GEN_PAGE_SHIFT) - 1;
                while (k < e && m[k] == g[k]) k++;
                while (e > k && m[e] == g[e]) e--;
                char f[160];
                flag_names(r->flags, f, sizeof f);
                rt_log("mem: verify mismatch: pool %#llx+%#llx (%s) offset %#llx: bytes %#x..%#x of the page differ, "
                       "mirror %02x guest %02x at the first (submission %u)", (unsigned long long)r->lo,
                       (unsigned long long)r->size, f, (unsigned long long)(a - r->lo), k, e, m[k], g[k], j->sub);
            }
        }
    }
    plat_add64(&v_pages, (int64_t)pages);
    plat_add64(&v_skipped, (int64_t)skipped);
    plat_add64(&v_mismatch, (int64_t)bad);
    vmaDestroyBuffer(vk_ctx()->vma, j->buf, j->alloc);
    free(j);
}

static int verify_record(VkCommandBuffer cb, uint64_t fence) {
    VJob *j = calloc(1, sizeof *j);
    if (!j) rt_fatal("vk: out of memory");
    uint64_t left = VERIFY_BYTES, dst = 0;
    plat_mutex_lock_shared(&pool_lock);
    for (int tries = 0; nm && left && j->n < VERIFY_SAMPLES && tries <= nm; tries++) {
        if (ver_i >= nm) ver_i = 0, ver_off = 0;
        PoolRec *r = mlist[ver_i];
        uint64_t size = r->import_hi - r->import_lo;
        if (ver_off >= size) { ver_i++, ver_off = 0; continue; }
        uint64_t len = size - ver_off < left ? size - ver_off : left;
        j->s[j->n++] = (VSample){r, ver_off, len, dst, r->mgen};
        dst += len, left -= len, ver_off += len;
        if (ver_off >= size) ver_i++, ver_off = 0;
    }
    plat_mutex_unlock_shared(&pool_lock);
    if (!j->n) { free(j); return 0; }
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = dst,
                             .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                  .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
    VmaAllocationInfo info;
    if (vmaCreateBuffer(vk_ctx()->vma, &bi, &ai, &j->buf, &j->alloc, &info) != VK_SUCCESS) { free(j); return 0; }
    j->map = info.pMappedData;
    j->sub = msub;
    mbarrier(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
             VK_ACCESS_2_TRANSFER_READ_BIT);
    for (int i = 0; i < j->n; i++) {
        VkBufferCopy c = {j->s[i].off, j->s[i].dst, j->s[i].len};
        vkCmdCopyBuffer(cb, j->s[i].r->mbuf, j->buf, 1, &c);
    }
    mbarrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
             VK_ACCESS_2_HOST_READ_BIT);
    vk_completion_add(fence, verify_cb, j);
    return j->n + 2;
}

int vk_mem_mirror_end(VkCommandBuffer cb, uint64_t fence) {
    if (!vk_mirror_on) return 0;
    int rec = 0, tq = nwbl ? mmerge_time_begin(cb) : -1;
    if (mm_active) rec += mmerge_end(cb, fence, &mrep.wb_bytes);   // the ranges in wbl, by bytes
    if (nwbl) {
        for (int i = 0; i < nwbl; i++) wbl[i].r->wb_sub = msub;
        mrep.wb_ranges += (uint64_t)nwbl;
        int ncopy = 0;   // the ranges not merged first
        for (int i = 0; i < nwbl; i++)
            if (!merged(wbl[i].r)) {
                MRange t = wbl[ncopy];
                wbl[ncopy++] = wbl[i], wbl[i] = t;
            }
        if (ncopy) {
            mbarrier(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                     VK_ACCESS_2_TRANSFER_READ_BIT);
            rec += copy_ranges(cb, wbl, ncopy, 0, &mrep.wb_bytes, &mrep.wb_copies) + 2;
        }
        if (reup_diag < 0) {
            const char *e = cfg_get("MKW_MIRROR_REUP");
            reup_diag = e && *e && strcmp(e, "0") != 0;
        }
        if (reup_diag) {
            while (cwbprev < nwbl) vk_mem_grow((void **)&wbprev, &cwbprev, sizeof *wbprev);
            memcpy(wbprev, wbl, (size_t)nwbl * sizeof *wbl);
            nwbprev = nwbl;
        }
        if (ncopy)
            mbarrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_HOST_BIT | VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                     VK_ACCESS_2_HOST_READ_BIT | VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
        nwbl = 0;
    }
    mmerge_time_end(cb, tq, fence, mm_active);
    if (mirror_verify) rec += verify_record(cb, fence);
    plat_mutex_lock(&mirror_stat_lock);
    uint64_t *d = (uint64_t *)&macc, *s = (uint64_t *)&mrep;
    for (size_t i = 0; i < sizeof macc / sizeof(uint64_t); i++) d[i] += s[i];
    plat_mutex_unlock(&mirror_stat_lock);
    memset(&mrep, 0, sizeof mrep);
    return rec;
}

// One summary line: with MKW_TRACE=perf every 60 frames, else at frames 600 and 1800.
void mirror_log(uint64_t frame) {
    static uint64_t last;
    ww_log(frame);
    if (!vk_mirror_on) return;
    plat_mutex_lock(&mirror_stat_lock);
    MirrorAcc a = macc;
    memset(&macc, 0, sizeof macc);
    plat_mutex_unlock(&mirror_stat_lock);
    double f = frame > last ? (double)(frame - last) : 1.0, M = 1048576.0;
    last = frame;
    uint64_t fc = (uint64_t)plat_xchg64(&m_flush_calls, 0), fb = (uint64_t)plat_xchg64(&m_flush_bytes, 0);
    char ver[160] = "";
    if (mirror_verify)
        snprintf(ver, sizeof ver, "; verify: pages compared %lld, skipped %lld (written since), mismatches %lld",
                 (long long)plat_xchg64(&v_pages, 0), (long long)plat_xchg64(&v_skipped, 0),
                 (long long)plat_xchg64(&v_mismatch, 0));
    char mm[384];
    mmerge_log(mm, sizeof mm, f);
    rt_log("mem: mirror frame %llu: pools %lld, %.1f MiB device-local; stay imported: budget %lld (%.1f MiB), untracked "
           "%lld, overlapping %lld, failed %lld; per frame: submits %.2f, syncs %.1f, upload %.3f MiB in %.1f ranges, "
           "first use %.2f pools %.3f MiB, write-back %.3f MiB in %.1f ranges, flush calls %.1f (%.3f MiB), queries "
           "%.1f pre-snap (%.1f MiB) + %.1f sync, prologues %.2f, tracking cpu %.3f ms (pre-snap %.3f, syncs %.3f)%s%s"
           "; GPU-written kept %.3f MiB%s%.3f MiB uploaded again after only the GPU wrote them (MKW_MIRROR_REUP)",
           (unsigned long long)frame, (long long)m_live_n, m_live_bytes / M, (long long)m_refused_n[MR_BUDGET],
           m_refused_bytes[MR_BUDGET] / M, (long long)m_refused_n[MR_UNTRACKED], (long long)m_refused_n[MR_OVERLAP],
           (long long)m_refused_n[MR_ALLOC], a.subs / f, a.syncs / f, a.upload_bytes / f / M, a.upload_copies / f,
           a.first_n / f, a.first_bytes / f / M, a.wb_bytes / f / M, a.wb_copies / f, fc / f, fb / f / M,
           a.presnap_queries / f, a.presnap_bytes / f / M, a.sync_queries / f, a.prologues / f, qpc_ms(a.presnap_t + a.sync_t) / f,
           qpc_ms(a.presnap_t) / f, qpc_ms(a.sync_t) / f, ver, mm, a.gpu_kept_bytes / f / M, reup_diag > 0 ? "; " : "",
           a.reup_bytes / f / M);
}

