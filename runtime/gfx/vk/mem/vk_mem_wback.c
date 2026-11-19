#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "nvn_pool_index.h"
#include "vk.h"
#include "vk_exec.h"
#include "vk_mem_internal.h"
#include "platform/platform.h"

// ---------------------------------------------------------------- GPU writes into shadow pools (vk_mem_shadow.c)
// The replay notes each GPU write into CPU-visible memory at record time (vk_mem_gpu_pending) and reports it at the
// fence (vk_gpu_written), which copies those bytes back into guest memory. Until then the shadow syncs skip them: a
// page the CPU and the GPU both write keeps the GPU's bytes. Merged by bytes instead where the range has a base
// (vk_mem_merge.c).
SRange *spend;
int nspend;
static int cspend;
static SRange *snew;                              // touch_lock: GPU writes noted while recording, not in spend yet
static int nsnew, csnew;
uint64_t s_part = 1;                              // syncs so far, +1: the part being recorded
uint64_t s_wb_bytes;
int32_t n_mbase;
int n_want;
volatile int32_t want_new;
int merge_mode = -1;                              // MKW_SHADOW_MERGE (default 1; "ab")
int read_traps;                                   // MKW_READ_TRAPS (default 1): GPU writes reach guest memory on access
int wb_defer;                                     // MKW_SHADOW_WB_DEFER (default 1; "ab"): vk_gpu_written below

// Fences whose write-back found shadow_lock taken (MKW_SHADOW_WB_DEFER): their ranges leave spend at the next taker
// that reads it. The completion thread waited there for a shadow sync's write-watch queries, ~25 waits of ~1 ms per s
// of race (aq-on2, 06/11/2026), so the fence was done that much later. Until a range leaves, syncs skip it: the same
// as when the fence waited for the sync to end.
typedef struct { uint64_t lo, hi, wb; } SDone;
static SDone *sdone;
static int nsdone, csdone;
static volatile int32_t sdone_n;                  // nsdone, read without the lock
static PlatMutex sdone_lock = PLAT_MUTEX_INIT;    // after shadow_lock

// The oldest pending write of [lo, hi): fences complete in order, so it is the one a write-back reports. -1: none.
static int spend_oldest(uint64_t lo, uint64_t hi) {   // shadow_lock held
    int k = -1;
    for (int i = 0; i < nspend; i++)
        if (spend[i].lo == lo && spend[i].hi == hi && (k < 0 || spend[i].fence < spend[k].fence)) k = i;
    return k;
}

static void spend_remove(uint64_t lo, uint64_t hi) {   // shadow_lock held
    int i = spend_oldest(lo, hi);
    if (i < 0) return;
    if (spend[i].mbase) n_mbase--;
    free(spend[i].base);
    if (spend[i].lazy) lazy_free(spend[i].lazy);
    else free(spend[i].mbase);
    spend[i] = spend[--nspend];
}

// The recording threads note their writes under touch_lock only: under shadow_lock they waited for the early thread's
// copies.
void spend_take_locked(void) {
    for (int i = 0; i < nsnew; i++) {
        if (nspend == cspend) vk_mem_grow((void **)&spend, &cspend, sizeof *spend);
        spend[nspend++] = snew[i];
        if (snew[i].merge) n_want++;
    }
    __atomic_store_n(&nsnew, 0, __ATOMIC_RELEASE);
}

void spend_take(void) {
    if (!__atomic_load_n(&nsnew, __ATOMIC_ACQUIRE)) return;
    plat_mutex_lock(&touch_lock);
    spend_take_locked();
    plat_mutex_unlock(&touch_lock);
}

void sdone_drain(void) {
    spend_take();   // a fence's ranges are in spend before they leave it
    if (!__atomic_load_n(&sdone_n, __ATOMIC_ACQUIRE)) return;
    plat_mutex_lock(&sdone_lock);
    for (int i = 0; i < nsdone; i++) spend_remove(sdone[i].lo, sdone[i].hi), s_wb_bytes += sdone[i].wb;
    nsdone = 0;
    __atomic_store_n(&sdone_n, 0, __ATOMIC_RELEASE);
    plat_mutex_unlock(&sdone_lock);
}

void wback_reset(void) {
    plat_mutex_lock(&sdone_lock);
    plat_mutex_lock(&touch_lock);
    nsnew = 0;
    plat_mutex_unlock(&touch_lock);
    nspend = nsdone = 0;
    __atomic_store_n(&sdone_n, 0, __ATOMIC_RELEASE);
    plat_mutex_unlock(&sdone_lock);
}

int vk_mem_gpu_may_write(uint64_t addr, uint64_t size) {
    if (!shadow_live) return 1;   // imported pools: nothing tells
    plat_mutex_lock(&shadow_lock);
    sdone_drain();
    int hit = 0;
    for (int i = 0; i < nspend && !hit; i++) hit = spend[i].lo < addr + size && spend[i].hi > addr;
    plat_mutex_unlock(&shadow_lock);
    return hit;
}

// MKW_WB_ORDER (default 1; "ab"): a write-back copies the GPU's bytes from the live shadow once its fence is reached,
// on the completion thread, and the GPU is by then running the next submission. One that writes the same bytes hands
// guest memory its half-done state: a dispatch clears counters another one adds to (34331 and 34334, the clouds), and
// guest memory gets the zeros (tdry1: ~9.5 write-backs a frame with the next writer of their pages queued, over a dozen
// pages; tnone1, the clouds gone for a frame). On the console the bytes are read where the GPU left them. Here a part
// writing a page whose earlier writer's write-back is still to come waits on the GPU (vk_sync.c, wb_sem) until the
// completion thread has done that write-back; no CPU thread waits.
static uint64_t *wb_last;    // per guest page, the newest submission writing it into a shadow; mapped lazily
static uint64_t wb_need;     // the newest earlier writer the part being recorded must wait for (atomic max)
static int wb_order = -1;

uint64_t vk_mem_wb_need_take(uint64_t fence) {
    uint64_t n = __atomic_exchange_n(&wb_need, 0, __ATOMIC_ACQ_REL);
    return n < fence ? n : 0;
}

static void wb_order_note(uint64_t addr, uint64_t size, uint64_t fence) {
    if (wb_order < 0) {
        wb_order = rt_knob("WB_ORDER", "1");
        if (wb_order && !(wb_last = plat_mem_alloc(RT_ADDRESS_LIMIT / 0x1000 * sizeof *wb_last))) wb_order = 0;
        rt_log("mem: a submission writing bytes of an earlier one waits on the GPU for its write-back %s (MKW_WB_ORDER)",
               wb_order == 2 ? "in the B blocks" : wb_order ? "on" : "off");
    }
    if (!wb_order || !fence || addr + size > RT_ADDRESS_LIMIT) return;
    uint64_t need = 0;
    for (uint64_t p = addr >> 12; p <= (addr + size - 1) >> 12; p++) {
        uint64_t old = __atomic_exchange_n(&wb_last[p], fence, __ATOMIC_RELAXED);
        if (old && old < fence && old > need) need = old;
    }
    if (!need || !RT_KNOB_ON(wb_order)) return;
    for (uint64_t cur = __atomic_load_n(&wb_need, __ATOMIC_RELAXED); cur < need;)
        if (__atomic_compare_exchange_n(&wb_need, &cur, need, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) break;
}

void vk_mem_gpu_pending(uint64_t addr, uint64_t size) {
    mem_early_gpu_wrote(addr, size, vk_exec_recording_fence());   // MKW_EARLY_READ
    if (!shadow_live || !size) return;
    uint8_t *base = lost_base(addr, size);
    if (merge_mode < 0) {
        merge_mode = rt_knob("SHADOW_MERGE", "1");
        rt_log("mem: shadow merged by bytes %s (MKW_SHADOW_MERGE)", merge_mode == 2 ? "in the B blocks" : merge_mode ? "on" : "off");
    }
    PoolRec *r = NULL;
    if (RT_KNOB_ON(merge_mode)) {   // the record lives while its shadow does; shadow_forget drops it from spend
        plat_mutex_lock_shared(&pool_lock);
        const pidx_ent *e = pidx_find(&pool_index, addr, size);
        r = e ? e->buf : NULL;
        plat_mutex_unlock_shared(&pool_lock);
    }
    if (r && r->smap) wb_order_note(addr, size, vk_exec_recording_fence());
    plat_mutex_lock(&touch_lock);
    if (nsnew == csnew) vk_mem_grow((void **)&snew, &csnew, sizeof *snew);
    snew[nsnew] = (SRange){.lo = addr, .hi = addr + size, .fence = vk_exec_recording_fence(), .part = s_part, .pool = r,
                           .base = base, .merge = r && r->smap, .tnote = qpc()};
    __atomic_store_n(&nsnew, nsnew + 1, __ATOMIC_RELEASE);
    plat_mutex_unlock(&touch_lock);
    if (r && r->smap && early_mode > 0 && !__atomic_load_n(&want_new, __ATOMIC_RELAXED)) {
        __atomic_store_n(&want_new, 1, __ATOMIC_RELAXED);
        plat_mutex_lock(&touch_lock);
        plat_cond_signal(&touch_cv);
        plat_mutex_unlock(&touch_lock);
    }
}

void vk_gpu_written(uint64_t addr, uint64_t size) {
    if (shadow_live && size) {
        plat_mutex_lock_shared(&pool_lock);
        const pidx_ent *e = pidx_find(&pool_index, addr, size);
        PoolRec *r = e ? e->buf : NULL;
        // The copy runs without shadow_lock: the range stays in spend (GPU writes not copied back) until it is done, so
        // shadow syncs skip it meanwhile. Under the lock, a 17 MB write-back held the replay's address resolution
        // (shadow_touch) and this fence's waiters for its whole length (stacks of live35, 01/11/2026).
        // With read traps the pages take the bytes when the game touches them (memory.c), not all of them here.
        uint64_t wb = 0;   // trapped ranges count when filled (read traps below)
        if (r && r->smap && addr >= r->import_lo && addr + size <= r->import_hi) {
            shadow_inval(r, addr, addr + size);   // the GPU's bytes, read below or by a read trap later
            // The bases leave their entry here, before the copy changes guest memory. Without any, no lock: the
            // completion thread waited for syncs on it (MKW_SHADOW_WB_DEFER).
            uint8_t *base = NULL, *mbase = NULL;
            LazyBase *lazy = NULL;
            uint64_t wf = 0;   // the fence written back
            if (lost_on > 1 || __atomic_load_n(&n_mbase, __ATOMIC_ACQUIRE)) {
                plat_mutex_lock(&shadow_lock);
                spend_take();
                int i = spend_oldest(addr, addr + size);
                if (i >= 0) base = spend[i].base, mbase = spend[i].mbase, lazy = spend[i].lazy, wf = spend[i].fence,
                            spend[i].base = spend[i].mbase = NULL, spend[i].lazy = NULL;
                if (mbase) n_mbase--;
                plat_mutex_unlock(&shadow_lock);
            }
            if (base && mbase) free(base);   // merged: the write-back reverts nothing
            else if (base) lost_writeback(addr, size, r->smap + (addr - r->import_lo), base);
            int copied = 0;
            if (mbase) {   // merged: the GPU's bytes now, the CPU's stay
                mem_early_wb_check(addr, size, wf);   // MKW_EARLY_READ_DRY: a later writer running meanwhile
                wb = merge_back(addr, size, r->smap + (addr - r->import_lo), mbase, lazy);
                if (lazy) lazy_free(lazy);
                else free(mbase);
                if (watch_hi) watch_event("write-back merged", addr, addr + size, 0, wb);
            } else if ((copied = !read_traps || !mem_trap(addr, size, r->smap + (addr - r->import_lo)))) {
                mem_thaw(addr, size);
                memcpy((void *)(uintptr_t)addr, r->smap + (addr - r->import_lo), size);
                wb = size;
                if (watch_hi) watch_event("write-back copied", addr, addr + size, 0, wb);
            } else if (watch_hi) watch_event("write-back trapped", addr, addr + size, 0, size);
        }
        if (r) vk_pool_diag_note(r->flags, 2, wb);
        int defer = RT_KNOB_ON(wb_defer);
        if (defer && !plat_mutex_trylock(&shadow_lock)) {
            plat_mutex_lock(&sdone_lock);
            if (nsdone == csdone) vk_mem_grow((void **)&sdone, &csdone, sizeof *sdone);
            sdone[nsdone++] = (SDone){addr, addr + size, wb};
            __atomic_store_n(&sdone_n, nsdone, __ATOMIC_RELEASE);
            plat_mutex_unlock(&sdone_lock);
        } else {
            if (!defer) plat_mutex_lock(&shadow_lock);
            sdone_drain();
            spend_remove(addr, addr + size);
            s_wb_bytes += wb;
            plat_mutex_unlock(&shadow_lock);
        }
        plat_mutex_unlock_shared(&pool_lock);
    }
    vk_gpu_written_stamp(addr, size);
}
