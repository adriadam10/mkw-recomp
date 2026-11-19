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

// ---------------------------------------------------------------- shadow storage
// Where importing guest memory costs too much (Linux: re-protecting pages that amdgpu imported as a userptr
// invalidates the buffer, about 1.3 ms per 16 MiB at the next submit, and the import's write pinning clears the
// write watch), a CPU-visible pool gets a host-visible buffer of its own instead, at the offsets the import would have:
// - CPU -> shadow: right before each submit (vk_mem_shadow_sync), the pools the replay resolved since the last one are
//   queried for written pages; pages stamped after a pool's sgen are copied in by the CPU.
// - GPU -> guest: the GPU's writes stay pending from record time to their fence (vk_mem_wback.c); the CPU copies skip
//   them meanwhile, or merge them by bytes where the range has a base (vk_mem_merge.c).
// MKW_SHADOW=1/0 forces the mode on or off; the default is on for Linux.
int shadow_on;                                    // shadow mode: imported pools get shadow storage (vk_mem_class_log)
// The syncs, write-backs and touches run for shadowed pools: on with shadow_on, and set once a pool in import mode
// (Windows) takes a shadow because its import failed (import_pool). Read without a lock: a pool's first shadow is
// made under pool_lock before any address in it resolves, so the replay sees it set before recording a use.
int shadow_live;
PlatMutex shadow_lock = PLAT_MUTEX_INIT;          // after pool_lock; before gen_lock
// Pools resolved since the last sync, under a lock of their own: the recording threads add to it while the early
// syncs below hold shadow_lock. stouch: touched while recording a submission (the early thread takes them);
// slate: touched outside one (a probe, a fence callback), left for the next sync.
PlatMutex touch_lock = PLAT_MUTEX_INIT;
PlatCond touch_cv;
static PoolRec **stouch, **slate;                 // touch_lock
static int nstouch, cstouch, nslate, cslate;
static PoolRec **sbatch;                          // shadow_lock: the touched pools the sync takes from both lists
static int cbatch;
int64_t s_live_n, s_live_bytes;            // shadow_lock
static uint8_t *strack;                           // vk_mem_shadow_sync: tracked flag of each touched pool
static int cstrack;
int vk_dirty_census;                              // MKW_DIRTY_CENSUS with MKW_TRACE=perf (vk.h)
// Pages whose only stamp since the shadow's sync is a GPU write-back (vk_gpu_written_stamp) are not copied in: the
// write-back took those bytes from this shadow, so it holds them (sh-reup, 14/11/2026: 57 of 262 MiB copied in over
// build/cap/race). MKW_SHADOW_CPU_GEN (default 1; "ab") switches it; s_gpu_only counts them either way.
int shadow_cpu_gen = 1;
static uint64_t s_gpu_only;       // shadow_lock: bytes with only a GPU write-back since (skipped when the knob is on)
static uint64_t s_syncs, s_copied, s_queries, s_untracked, s_ticks;   // shadow_lock: since the last log
// The sync's ticks by step: captures applied, write-watch queries, pages copied in, merge bases (tl2: 6.9 ms a frame
// on the replay thread, the frame's critical path; 12.8 MiB copied in and 8.8 MiB of bases a frame, 0 KiB merged).
static uint64_t s_t_cap, s_t_query, s_t_copy, s_t_base;
static uint64_t s_t_lock, s_t_early, s_early;    // the sync's wait for shadow_lock; early syncs' ticks and pools
static uint64_t s_merged, s_nobase, s_based, s_base_bytes;   // shadow_lock: merged-in bytes, written ranges with and
                                                             // without a base, bytes of the bases
static int n_early_base;                          // shadow_lock: this part's merge ranges with an early base
static uint64_t s_early_bases;                    // shadow_lock: bases the early thread took
static uint64_t s_late[3];                        // shadow_lock: bytes of the sync's bases held back by another part's
                                                  // pending write over them, by a pool this sync synced (the early
                                                  // thread had not), and by none (vk_mem_shadow_sync)
// The bytes by none, split: the early thread left them for another part's write over them, gone by the sync (the same
// range's, another's), or never saw them, by their age at the sync (< 0.5 ms, < 2, more).
static uint64_t s_none[5];

int shadow_pool(VkCtx *c, PoolRec *r, uint64_t addr, uint64_t size) {
    r->import_lo = addr & ~0xfffull;
    r->import_hi = (addr + size + 0xfff) & ~0xfffull;
    uint64_t n = r->import_hi - r->import_lo;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = n, .usage = pool_usage(c),
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    // Cached: the fence copies read it back on the CPU (vk_mem_shadow_nc.c picks the type).
    VmaAllocationCreateInfo ai = {.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT};
    shadow_alloc_flags(c, &ai);
    VmaAllocationInfo info;
    VkResult res = vmaCreateBuffer(c->vma, &bi, &ai, &r->buf, &r->salloc, &info);
    if (res != VK_SUCCESS) {
        rt_log("vk: pool %#llx+%#llx: shadow allocation failed (%d)", (unsigned long long)addr, (unsigned long long)size,
               (int)res);
        static int reported;
        if (reported++ < 3) vram_log(0);   // what holds the memory
        r->buf = VK_NULL_HANDLE, r->salloc = NULL;
        return 0;
    }
    r->smap = info.pMappedData;
    shadow_alloc_note(c, r);
    gen_lock_x();
    r->sgen = gen_now;   // the copy below holds every write stamped so far; later stamps are copied again
    plat_mutex_unlock(&gen_lock);
    memcpy(r->smap, (const void *)(uintptr_t)r->import_lo, n);
    vk_pool_diag_note(r->flags, 0, n);
    shadow_flush(r, r->import_lo, r->import_hi);
    plat_mutex_lock(&shadow_lock);
    s_live_n++, s_live_bytes += (int64_t)n;
    plat_mutex_unlock(&shadow_lock);
    plat_inc32(&n_imports);
    return 1;
}

// A pool already synced for the submission being recorded is not synced again at its later parts: the CPU writes the
// submission must see came before it was submitted, and the first sync copied them. Writes made meanwhile by another
// thread are not ordered with it; the next submission's sync takes them. Syncing again at every part took 116-133 ms
// per s of wall clock in a race (rt-i2: 4.4 parts per submission).
// Early syncs (MKW_SHADOW_EARLY, default 1; "ab"; se-ab3: -0.5 ms a frame; off until 08/11/2026 for the cacti it
// brought back, pse1 6 in 1,800 race frames, which were a translator race fixed in 9129803: tse1 0 in 2,400): a pool first touched while a submission is recorded is synced
// right away on a thread of its own ("vk shadow"), while the replay goes on recording; the sync before the part's
// submit only waits for the one in progress and takes what is left. The game submitted that submission before the
// replay began it, so its writes for it are in guest memory already; later writes are the next frames' (taking them
// earlier is closer to the console, where the GPU reads right after the submit). The sync's queries and copies cost
// 4.7 ms a frame on the replay thread, the frame's critical path (sh1).
int early_mode = -1;
static int32_t sync_wants;   // syncs waiting for shadow_lock: the early thread lets them in before taking the next pool

void shadow_touch(PoolRec *r) {
    uint64_t f = vk_exec_recording_fence();
    if (f && __atomic_load_n(&r->ssub, __ATOMIC_RELAXED) == f) return;
    int early = f && early_mode > 0 && RT_KNOB_ON(early_mode);
    plat_mutex_lock(&touch_lock);
    if (!r->stouched) {
        if (early) {
            if (nstouch == cstouch) vk_mem_grow((void **)&stouch, &cstouch, sizeof *stouch);
            stouch[nstouch++] = r;
            r->sfence = f;
            plat_cond_signal(&touch_cv);
        } else {
            if (nslate == cslate) vk_mem_grow((void **)&slate, &cslate, sizeof *slate);
            slate[nslate++] = r;
        }
        r->stouched = 1;
    }
    plat_mutex_unlock(&touch_lock);
}

// vk_pool_fini, after the pool left the index: off the touched list before its record goes.
void shadow_forget(PoolRec *r) {
    plat_mutex_lock(&shadow_lock);
    plat_mutex_lock(&touch_lock);
    for (int i = 0; i < nstouch; i++)
        if (stouch[i] == r) { stouch[i] = stouch[--nstouch]; break; }
    for (int i = 0; i < nslate; i++)
        if (slate[i] == r) { slate[i] = slate[--nslate]; break; }
    plat_mutex_unlock(&touch_lock);
    spend_take();
    cap_forget(r);
    for (int i = 0; i < nspend; i++)   // the bases go with the shadow they copy; the ranges stay skipped
        if (spend[i].pool == r) {
            if (spend[i].mbase) n_mbase--;
            if (spend[i].lazy) lazy_free(spend[i].lazy);
            else free(spend[i].mbase);
            spend[i].mbase = NULL, spend[i].lazy = NULL, spend[i].pool = NULL;
        }
    s_live_n--, s_live_bytes -= (int64_t)(r->import_hi - r->import_lo);
    plat_mutex_unlock(&shadow_lock);
}

static uint64_t sync_fence;   // shadow_lock: the submission vk_mem_shadow_sync is syncing for

// 1 when a pending GPU write over [a, b) belongs to a submission before the one being synced (MKW_SHADOW_LOST).
static int spend_old(uint64_t a, uint64_t b) {
    for (int i = 0; i < nspend; i++)
        if (spend[i].lo < b && spend[i].hi > a && spend[i].fence != sync_fence) return 1;
    return 0;
}

// A pending write that keeps the CPU's bytes out of the shadow: one of a part already submitted, or any without merge.
static int spend_blocks(const SRange *e) { return !e->merge || e->part != s_part; }

// shadow_lock held: [lo, hi) merged in by bytes when every blocking pending write over it has a base; 0: skipped.
static int shadow_merge(PoolRec *r, uint64_t lo, uint64_t hi, const uint8_t *src) {
    enum { MAXB = 8 };
    uint8_t *bases[MAXB];
    uint64_t blo[MAXB];
    int n = 0;
    for (int j = 0; j < nspend; j++) {
        const SRange *e = &spend[j];
        if (e->lo >= hi || e->hi <= lo || !spend_blocks(e)) continue;
        if (!e->mbase || n >= MAXB - 1 || e->lo > lo || e->hi < hi) return 0;
        if (e->lazy) lazy_materialize(e->lazy, lo, hi);   // merge_in writes the base
        bases[n] = e->mbase, blo[n++] = e->lo;
        if (e->lazy && lazy_check(e->lazy)) bases[n] = lazy_check(e->lazy), blo[n++] = e->lo;   // kept alike
    }
    shadow_inval(r, lo, hi);   // merge_in compares the GPU's bytes with the bases
    uint8_t *old = race_on > 0 ? merge_base(r, lo, hi) : NULL;   // MKW_SHADOW_RACE: the shadow's bytes before
    uint64_t over = merge_over, m = merge_in(r, lo, hi, bases, blo, n, src);
    if (m) shadow_flush(r, lo, hi);
    if (watch_hi && merge_over != over) watch_event("merge-in over the GPU's", lo, hi, sync_fence, merge_over - over);
    if (old && m) race_check(r, lo, hi, sync_fence, old);
    free(old);
    s_merged += m;
    if (watch_hi && m) watch_event("merge-in", lo, hi, sync_fence, m);
    return 1;
}

// shadow_lock held: [a, b) of r's shadow just changed by a copy-in: the early bases of this part's ranges over it
// follow, as if they had been taken after it.
static void base_follow(PoolRec *r, uint64_t a, uint64_t b) {
    for (int i = 0; i < nspend; i++) {
        const SRange *e = &spend[i];
        if (e->part != s_part || !e->mbase || e->lo >= b || e->hi <= a) continue;
        uint64_t lo = e->lo > a ? e->lo : a, hi = e->hi < b ? e->hi : b;
        if (e->lazy) {   // the base's pages there into its buffer first; the check copy follows too
            lazy_materialize(e->lazy, lo, hi);
            if (lazy_check(e->lazy))
                memcpy(lazy_check(e->lazy) + (lo - e->lo), r->smap + (lo - r->import_lo), hi - lo);
        }
        memcpy(e->mbase + (lo - e->lo), r->smap + (lo - r->import_lo), hi - lo);
    }
}

// shadow_lock held: copies [a, b) into the shadow, leaving out the bytes of GPU writes not copied back yet (spend from
// index i on), or merging them in by bytes. The bytes come from src (a submit-time capture) or, NULL, guest memory.
static void shadow_copy(PoolRec *r, uint64_t a, uint64_t b, int i, const uint8_t *src) {
    for (; i < nspend; i++)
        if (spend[i].lo < b && spend[i].hi > a && spend_blocks(&spend[i])) {
            uint64_t lo = spend[i].lo > a ? spend[i].lo : a, hi = spend[i].hi < b ? spend[i].hi : b;
            int merged = spend[i].merge && shadow_merge(r, lo, hi, src ? src + (lo - a) : NULL);
            if (!merged && lost_on > 0 && !src) lost_skip(r, lo, hi, spend_old(lo, hi));
            if (!merged && watch_hi) watch_event("skip (pending)", lo, hi, spend[i].fence, hi - lo);
            if (spend[i].lo > a) shadow_copy(r, a, spend[i].lo, i + 1, src);
            if (spend[i].hi < b) shadow_copy(r, spend[i].hi, b, i + 1, src ? src + (spend[i].hi - a) : NULL);
            return;
        }
    // A read-trapped page still holds the shadow's bytes: copying it would fault and fill it for nothing. The GPU's
    // writes stamp their pages (vk_guest_written), so every sync after a fence would.
    if (src) {
        memcpy(r->smap + (a - r->import_lo), src, b - a);
        shadow_flush(r, a, b);
        s_copied += b - a;
        if (n_early_base) base_follow(r, a, b);
        return;
    }
    for (uint64_t x = a, y; x < b; x = y) {
        int t = mem_trapped(x);
        for (y = (x & ~0xfffull) + 0x1000; y < b && mem_trapped(y) == t; y += 0x1000) {}
        if (y > b) y = b;
        if (t) continue;
        if (race_on > 0) race_wait(race_check(r, x, y, sync_fence, NULL));
        if (watch_hi) watch_copy(r, x, y, sync_fence);
        mem_copy_out(r->smap + (x - r->import_lo), x, y - x);   // MKW_EARLY_READ's watched pages stay watched
        shadow_flush(r, x, y);
        s_copied += y - x;
        if (n_early_base) base_follow(r, x, y);
    }
}

// gen_lock held: stamps the written pages of [lo, hi) with g, one query per run of regions that are not frozen. 0: some
// part is not write-watched.
static int shadow_query(uint64_t lo, uint64_t hi, uint32_t g) {
    int tracked = 1;
    for (uint64_t a = lo; a < hi;) {
        uint64_t b = (a | ((1ull << REG_SHIFT) - 1)) + 1;
        if (b > hi) b = hi;
        if (mem_frozen(a)) { a = b; continue; }
        while (b < hi && !mem_frozen(b)) b = b + (1ull << REG_SHIFT) < hi ? b + (1ull << REG_SHIFT) : hi;
        s_queries++;
        if (!query_stamp(a, b, g)) tracked = 0;
        a = b;
    }
    return tracked;
}

// The runs a sync copies, found under gen_lock and copied after it: the recording reads the page stamps (texture
// freshness, the VRAM mirror) under gen_lock shared, and waited for every copy of the early thread (se-ab2: the
// replay 80-100 ms per s off the CPU in the early blocks, ~15 without).
typedef struct { PoolRec *r; uint64_t a, b; } SRun;
static SRun *sruns;                               // shadow_lock
static int nsruns, csruns;

static void run_add(PoolRec *r, uint64_t a, uint64_t b) {
    if (nsruns == csruns) vk_mem_grow((void **)&sruns, &csruns, sizeof *sruns);
    sruns[nsruns++] = (SRun){r, a, b};
}

// shadow_lock and gen_lock held, after the query: the runs of the pool's pages stamped after its shadow.
static void shadow_newer_runs(PoolRec *r, int tracked) {
    uint64_t lo = r->import_lo, hi = r->import_hi;
    uint32_t since = r->sgen;
    r->sgen = gen_now;
    if (!tracked) {   // nothing tells which pages changed: all of them
        s_untracked++;
        run_add(r, lo, hi);
        return;
    }
    uint64_t run = 0;
    for (uint64_t k = lo >> REG_SHIFT; k <= (hi - 1) >> REG_SHIFT; k++) {
        uint64_t a = k << REG_SHIFT, b = a + (1ull << REG_SHIFT);
        if (a < lo) a = lo;
        if (b > hi) b = hi;
        const uint32_t *ch = gen_chunk[a >> GEN_CHUNK_SHIFT];   // a 2 MiB region lies in one chunk
        if (reg_max[k] <= since || !ch) {
            if (run) run_add(r, run, a), run = 0;
            continue;
        }
        const uint32_t *cc = cpu_gen_chunk[a >> GEN_CHUNK_SHIFT];
        for (uint64_t x = a; x < b; x += 1ull << GEN_PAGE_SHIFT) {
            int dirty = ch[(x >> GEN_PAGE_SHIFT) & (GEN_PER_CHUNK - 1)] > since;
            if (dirty && (!cc || cc[(x >> GEN_PAGE_SHIFT) & (GEN_PER_CHUNK - 1)] <= since)) {
                s_gpu_only += 1ull << GEN_PAGE_SHIFT;
                if (RT_KNOB_ON(shadow_cpu_gen)) dirty = 0;
            }
            if (dirty && !run) run = x;
            else if (!dirty && run) run_add(r, run, x), run = 0;
        }
    }
    if (run) run_add(r, run, hi);
}

// shadow_lock held: a captured run of r into the shadow (vk_mem_capture.c).
void shadow_apply_run(PoolRec *r, uint64_t a, uint64_t b, const uint8_t *src) { shadow_copy(r, a, b, 0, src); }

// Hot pools (MKW_SHADOW_HOT, default 1; "ab" in the B blocks): a query protects the written pages again, so the CPU's
// next write faults and the next query finds and protects the page once more, ~2 us a page on the replay (rt-x4: 790
// written pages and 1.7 ms of queries a frame) plus the game's fault. A pool written in at least 4 of its last 8 syncs
// is queried without protecting for its next 32: its written pages are copied at every sync. Then one normal query
// protects them again and the history decides anew. Race: -3.6 % (rt-x6) and -1.3 % (rt-x7) per frame.
#define HOT_SYNCS 32
int hot_mode = -1;
static uint64_t s_hot_syncs, s_hot_pages;   // gen_lock

// gen_lock held: stamps the written pages of the pools, tracked[i] for each.
void shadow_query_pools(PoolRec **rs, uint8_t *tracked, int n, int hot_on) {
    for (int i = 0; i < n; i++) {
        PoolRec *r = rs[i];
        uint64_t p0 = ww_pages;
        ww_peek = hot_on && r->shot;
        tracked[i] = (uint8_t)shadow_query(r->import_lo, r->import_hi, ++gen_now);
        int found = ww_pages != p0;
        if (ww_peek) {
            r->shot--, s_hot_syncs++, s_hot_pages += ww_pages - p0;
            ww_peek = 0;
            continue;
        }
        r->shist = (uint8_t)(r->shist << 1 | found);
        if (hot_on && __builtin_popcount(r->shist) >= 4) r->shot = HOT_SYNCS, r->shist = 0;
    }
}

// shadow_lock held: syncs rs[0..n) for the submission fence (its pools skip it from then on).
static void sync_pools(PoolRec **rs, int n, uint64_t fence, uint64_t sub) {
    if (n > cstrack) {
        cstrack = n * 2;
        free(strack);
        if (!(strack = malloc((size_t)cstrack))) rt_fatal("vk: out of memory (shadow sync)");
    }
    uint64_t t1 = qpc();
    gen_lock_x();
    int k = cap_split(rs, n);   // pools the captures cover go last and are not read here
    shadow_query_pools(rs, strack, k, RT_KNOB_ON(hot_mode));
    uint64_t t2 = qpc();
    s_t_query += t2 - t1;
    nsruns = 0;
    for (int i = 0; i < k; i++) shadow_newer_runs(rs[i], strack[i]);
    cap_activate(rs, k);
    for (int i = 0; i < n; i++) rs[i]->touch_sub = sub, __atomic_store_n(&rs[i]->ssub, fence, __ATOMIC_RELAXED);
    plat_mutex_unlock(&gen_lock);
    for (int i = 0; i < nsruns; i++) shadow_copy(sruns[i].r, sruns[i].a, sruns[i].b, 0, NULL);
    s_t_copy += qpc() - t2;
    plat_mutex_lock(&touch_lock);   // after ssub: a touch meanwhile saw stouched and the sync took its writes
    for (int i = 0; i < n; i++) rs[i]->stouched = 0;
    plat_mutex_unlock(&touch_lock);
    s_syncs += (uint64_t)n;
}

// shadow_lock held: the base of spend[i], the shadow's bytes now (s_based and s_nobase count it). Once: a range over
// MERGE_MAX has none, and each early look retaking it brought n_want to 0 and ended early_bases before the ranges after
// it (ebn1: ~400 such takes and ~300 MiB of bases left to the sync before the submit per 60 frames).
static void take_base(int i) {
    SRange *e = &spend[i];
    e->btried = 1;
    // Lazy (MKW_LAZY_BASE) when no pending write of another part is over it: its GPU bytes would be in the shadow and
    // not in guest memory yet.
    int alone = e->pool && lazy_base_on();
    for (int j = 0; j < nspend && alone; j++)
        if (j != i && spend[j].lo < e->hi && spend[j].hi > e->lo) alone = 0;
    e->lazy = alone ? lazy_take(e->pool, e->lo, e->hi) : NULL;
    e->mbase = e->lazy ? lazy_buf(e->lazy) : merge_base(e->pool, e->lo, e->hi);
    *(e->mbase ? &s_based : &s_nobase) += 1;
    if (e->mbase) n_mbase++, s_base_bytes += e->hi - e->lo;
    n_want--;
}

// Early bases (with the early syncs): the base of a range this part writes is taken by the early thread once its pool
// was synced for the submission and no GPU write of another part is pending over it. Until the submit the shadow there
// changes only by copy-ins (base_follow keeps the base with them): the GPU runs no part that writes it, and the pool is
// not synced again for this submission. The sync before the submit took 1.7 ms a frame for them (sh1).
// MKW_EARLY_BASE_ANY (default 1; "ab"): the pool need not be synced for the submission first; base_follow keeps a base
// taken before the pool's copy-ins with them. A pool the submission never touches is never synced for it, and its
// ranges' bases all went to the sync before the submit: ~300 MiB per 60 frames, ~290 more of pools not reached yet (ebd3).
// Taken with shadow_lock, the replay's sync waits for them (ebn2: its wait 49-54 -> 66-70 ms per 60 frames while its
// bases went 39-43 -> 14-18). Not better (ebn3-6): letting the sync in took the bases back to it; copying without the
// lock let the sync in to take the rest itself; a thread of its own for them waited for the pool syncs. They are noted
// < 2 ms before their sync, and the copy is bound by memory: whoever copies, the replay waits.
static int any_mode = -1;

static void early_bases(void) {   // shadow_lock held
    if (any_mode < 0) {
        any_mode = rt_knob("EARLY_BASE_ANY", "1");
        rt_log("mem: early merge bases before the pool's sync %s (MKW_EARLY_BASE_ANY)",
               any_mode == 2 ? "in the B blocks" : any_mode ? "on" : "off");
    }
    int any = RT_KNOB_ON(any_mode);
    for (int i = 0; i < nspend && n_want; i++) {
        const SRange *e = &spend[i];
        if (e->part != s_part || !e->merge || e->btried || !e->pool ||
            (!any && (e->pool->ssub != e->fence || e->pool->stouched)))
            continue;
        int other = 0;
        for (int j = 0; j < nspend && !other; j++)
            if (spend[j].part != s_part && spend[j].lo < e->hi && spend[j].hi > e->lo)
                other = spend[j].lo == e->lo && spend[j].hi == e->hi ? 2 : 1;
        if (other) {
            spend[i].eother = (uint8_t)other;
            continue;
        }
        take_base(i);
        if (spend[i].mbase) n_early_base++, s_early_bases++;
    }
}

// The early thread: one pool at a time, so the sync before a submit waits for one pool at most.
static void early_main(void *arg) {
    (void)arg;
    plat_mutex_lock(&touch_lock);
    for (;;) {
        if ((!nstouch && !want_new) || !RT_KNOB_ON(early_mode)) {   // "ab" switches without a signal: the timeout looks
                                                                       // again
            plat_cond_wait_ms(&touch_cv, &touch_lock, 50);
            continue;
        }
        plat_mutex_unlock(&touch_lock);
        if (__atomic_load_n(&sync_wants, __ATOMIC_ACQUIRE)) {   // the replay's sync goes first (it takes the rest)
            plat_mutex_lock(&shadow_lock);
            plat_mutex_unlock(&shadow_lock);
            plat_mutex_lock(&touch_lock);
            continue;
        }
        plat_mutex_lock(&shadow_lock);
        plat_mutex_lock(&touch_lock);
        PoolRec *r = nstouch ? stouch[--nstouch] : NULL;
        plat_mutex_unlock(&touch_lock);
        if (r) {
            uint64_t t0 = qpc(), f = r->sfence;
            sdone_drain();
            sync_fence = f;
            sync_pools(&r, 1, f, __atomic_load_n(&vk_replay_submit, __ATOMIC_RELAXED));
            s_early++, s_t_early += qpc() - t0;
        }
        __atomic_store_n(&want_new, 0, __ATOMIC_RELAXED);
        // The writes noted since go into spend first: woken by a note with no pool to sync, the thread looked at a spend
        // without them, and every base went to the sync before the submit (ebd1: ~600 MiB per 60 frames, none of it
        // held back by another part's write).
        spend_take();
        if (n_want) {
            uint64_t t0 = qpc();
            early_bases();
            s_t_early += qpc() - t0;
        }
        plat_mutex_unlock(&shadow_lock);
        plat_mutex_lock(&touch_lock);
    }
}

void vk_mem_shadow_sync(uint64_t fence) {
    if (!__atomic_load_n(&shadow_live, __ATOMIC_ACQUIRE)) return;
    uint64_t t0 = qpc();
    __atomic_add_fetch(&sync_wants, 1, __ATOMIC_ACQ_REL);
    plat_mutex_lock(&shadow_lock);
    __atomic_sub_fetch(&sync_wants, 1, __ATOMIC_ACQ_REL);
    uint64_t tl = qpc();
    s_t_lock += tl - t0;
    sdone_drain();
    sync_fence = fence;
    if (hot_mode < 0) {
        race_level();
        watch_init();
        hot_mode = rt_knob("SHADOW_HOT", "1");
        rt_log("mem: hot shadow pools %s (MKW_SHADOW_HOT)", hot_mode == 2 ? "in the B blocks" : hot_mode ? "on" : "off");
    }
    if (early_mode < 0) {
        int m = rt_knob("SHADOW_EARLY", "1");
        if (m) {
            plat_cond_init(&touch_cv);
            if (!plat_thread_spawn(early_main, NULL, "vk shadow", 0)) rt_fatal("vk: cannot start the shadow thread");
        }
        rt_log("mem: early shadow syncs %s (MKW_SHADOW_EARLY)", m == 2 ? "in the B blocks" : m ? "on" : "off");
        __atomic_store_n(&early_mode, m, __ATOMIC_RELEASE);
    }
    uint64_t sub = __atomic_load_n(&vk_replay_submit, __ATOMIC_RELAXED);
    cap_apply(sub);   // the CPU's writes as they were at each submit up to this one (vk_mem_capture.c)
    uint64_t t1 = qpc();
    s_t_cap += t1 - tl;
    plat_mutex_lock(&touch_lock);
    int n = nstouch + nslate;
    if (n > cbatch) {
        cbatch = n * 2;
        free(sbatch);
        if (!(sbatch = malloc(sizeof *sbatch * (size_t)cbatch))) rt_fatal("vk: out of memory (shadow sync)");
    }
    memcpy(sbatch, stouch, sizeof *sbatch * (size_t)nstouch);
    memcpy(sbatch + nstouch, slate, sizeof *sbatch * (size_t)nslate);
    nstouch = nslate = 0;
    plat_mutex_unlock(&touch_lock);
    if (n) sync_pools(sbatch, n, fence, sub);
    t1 = qpc();
    // The bases of this part's ranges: the shadow as the part is submitted with it.
    // The part changes with the noted writes taken under touch_lock: a write noted later is the next part's.
    plat_mutex_lock(&touch_lock);
    spend_take_locked();
    uint64_t part = s_part++;
    plat_mutex_unlock(&touch_lock);
    for (int i = 0; i < nspend; i++)
        if (spend[i].part == part && spend[i].merge && !spend[i].btried) {
            int why = 2;
            for (int j = 0; j < nspend && why; j++)
                if (spend[j].part != part && spend[j].lo < spend[i].hi && spend[j].hi > spend[i].lo) why = 0;
            for (int j = 0; j < n && why == 2; j++)
                if (sbatch[j] == spend[i].pool) why = 1;
            uint64_t len = spend[i].hi - spend[i].lo;
            s_late[why] += len;
            if (why == 2) {
                uint64_t age = t1 - spend[i].tnote;
                s_none[spend[i].eother ? 2 - spend[i].eother : age < 500000 ? 2 : age < 2000000 ? 3 : 4] += len;
            }
            take_base(i);
        }
    s_t_base += qpc() - t1;
    n_want = n_early_base = 0;
    if (lost_on > 0) lost_check();
    s_ticks += qpc() - t0;
    plat_mutex_unlock(&shadow_lock);
}

int vk_mem_shadow_on(void) { return shadow_on; }

void shadow_log(uint64_t frame) {
    if (!shadow_live) return;
    uint64_t tn, tp, tf, to;
    mem_trap_stats(&tn, &tp, &tf, &to);
    plat_mutex_lock(&shadow_lock);
    sdone_drain();
    rt_log("mem: shadow at frame %llu: %lld pools %.1f MiB; since the last: %llu pool syncs, %llu queries, %.1f MiB "
           "copied in (%llu untracked), %.1f MiB written back, %.2f ms, %d GPU writes pending; read traps: %llu pages "
           "now, %.1f MiB trapped, filled %.1f MiB on access and %.1f MiB by a thaw; %.1f MiB listed for a copy in with "
           "only a GPU write-back since",
           (unsigned long long)frame, (long long)s_live_n, s_live_bytes / 1048576.0, (unsigned long long)s_syncs,
           (unsigned long long)s_queries, s_copied / 1048576.0, (unsigned long long)s_untracked, s_wb_bytes / 1048576.0,
           qpc_ms(s_ticks), nspend, (unsigned long long)tn, tp / 256.0, tf / 256.0, to / 256.0, s_gpu_only / 1048576.0);
    rt_log("mem: shadow sync by step since the last line, ms: captures applied %.2f, queries %.2f, copies in %.2f, "
           "merge bases %.2f; waiting for shadow_lock %.2f; early syncs %.2f (%llu pools, in the steps above; %llu early bases)",
           qpc_ms(s_t_cap), qpc_ms(s_t_query), qpc_ms(s_t_copy), qpc_ms(s_t_base), qpc_ms(s_t_lock), qpc_ms(s_t_early),
           (unsigned long long)s_early, (unsigned long long)s_early_bases);
    s_t_cap = s_t_query = s_t_copy = s_t_base = s_t_lock = s_t_early = s_early = s_early_bases = 0;
    if (merge_mode > 0)
        rt_log("mem: shadow merge since the last line: %llu written ranges with a base (%.1f MiB), %llu over %llu MiB "
               "without, %.1f KiB merged in; the sync's own bases %.1f MiB held back by another part's write, %.1f by "
               "a pool the early thread had not synced, %.1f by none", (unsigned long long)s_based,
               s_base_bytes / 1048576.0, (unsigned long long)s_nobase, (unsigned long long)(MERGE_MAX >> 20),
               s_merged / 1024.0, s_late[0] / 1048576.0, s_late[1] / 1048576.0, s_late[2] / 1048576.0);
    lazy_log();
    if (merge_mode > 0)
        rt_log("mem: shadow bases by none, MiB: left by the early thread for a write over them gone by the sync %.1f "
               "(the same range's) and %.1f (another's); never seen, noted < 0.5 ms before the sync %.1f, < 2 ms %.1f, "
               "earlier %.1f", s_none[0] / 1048576.0, s_none[1] / 1048576.0, s_none[2] / 1048576.0,
               s_none[3] / 1048576.0, s_none[4] / 1048576.0);
    memset(s_late, 0, sizeof s_late);
    memset(s_none, 0, sizeof s_none);
    cap_log();
    merge_census_log();
    if (race_on > 0) race_log(frame);
    s_syncs = s_queries = s_copied = s_untracked = s_wb_bytes = s_ticks = s_merged = s_based = s_nobase = s_gpu_only = 0;
    s_base_bytes = 0;
    if (hot_mode > 0) {
        gen_lock_x();
        rt_log("mem: hot shadow pools since the last line: %llu syncs without protecting, %llu written pages copied",
               (unsigned long long)s_hot_syncs, (unsigned long long)s_hot_pages);
        s_hot_syncs = s_hot_pages = 0;
        plat_mutex_unlock(&gen_lock);
    }
    plat_mutex_unlock(&shadow_lock);
}

// vk_mem_shutdown, pool_lock held, after every pool went: shadows went with their pools.
void shadow_reset(void) {
    plat_mutex_lock(&shadow_lock);
    plat_mutex_lock(&touch_lock);
    nstouch = nslate = 0;
    plat_mutex_unlock(&touch_lock);
    wback_reset();
    s_live_n = s_live_bytes = 0;
    plat_mutex_unlock(&shadow_lock);
}
