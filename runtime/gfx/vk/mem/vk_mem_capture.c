#include <stdlib.h>
#include <string.h>

#include "nvn_api.h"
#include "vk_mem_internal.h"

// ---------------------------------------------------------------- the CPU's writes as at each submit (MKW_SHADOW_SUBMIT)
// On the console the GPU reads the memory a submission uses right after nvnQueueSubmitCommands; the asynchronous
// replay records ~3 submits behind the game (lf6, 06/11/2026), and its shadow sync copied guest memory as it was then:
// the game had already rewritten the next frame's indirect arguments and per-instance data, and a tree drew from
// another tree's (one dark frame; lf9: 37 such frames in 1,024 without waiting, 1 in 776 when every submit waited for
// its replay). So at each submit (vk_mem_shadow_capture, game thread) the written pages of the shadow pools the replay
// uses are queried and copied into a capture tagged with the submit number; the sync of that submission applies the
// captures up to it (cap_apply) and no longer reads guest memory for those pools.
// - Captured pools: those a sync touched, from the next submit on (cap_from); one the replay has not used for
//   CAP_IDLE submits leaves the list, and a sync reads it from guest memory again until it is back.
// - Between activation and the first capture covering it, a pool keeps what its activating sync copied (older bytes,
//   never newer ones that a capture would then take back).
// - Read-trapped pages hold the shadow's bytes already: left out. Pages of untracked memory (no write watch): the pool
//   leaves the list and is read as before.
// MKW_SHADOW_SUBMIT (a measurement, default 0; "ab"): 1 captures. fe2 (06/11/2026): no fewer dark trees (55 in 770
// frames with it, 46 in 1,030 without) and 2.3 ms a submit on the game thread (685 pools, 3.5 MB each). Switching in the A/B blocks drops
// every pool at the edge: a block's first syncs may see the bytes older captures hold.
#define CAP_IDLE 64

typedef struct { PoolRec *pool; uint64_t addr, n; uint8_t *data; } CapRun;
typedef struct { uint64_t sub; CapRun *runs; int nruns; } CapPack;

// The list and the captures have a lock of their own: under shadow_lock the game thread waited for every shadow sync
// of the replay, 0.19 ms per submit with nothing to capture (fe1, 06/11/2026).
uint64_t vk_replay_submit;
static int mode = -1, active;                  // active: the last submit captured
static PlatMutex cap_lock = PLAT_MUTEX_INIT;   // innermost: after shadow_lock and gen_lock
static PoolRec **list;                         // cap_lock: captured pools
static int nlist, clist;
static volatile int32_t nlist_v;               // nlist, read without the lock
static CapPack *packs;                         // cap_lock: captures not applied yet, in submit order
static int npacks, cpacks;
static PoolRec **snap;                         // capture: the list, copied (the front end's submit lock)
static uint8_t *tracked;
static int csnap;
static uint64_t c_subs, c_pools, c_bytes, c_ns, c_dropped;   // cap_lock
static uint64_t c_applied;                                  // shadow_lock

static void log_locked(void);

static void list_drop(int i) {   // cap_lock held
    list[i]->cap_on = 0;
    list[i] = list[--nlist];
    __atomic_store_n(&nlist_v, nlist, __ATOMIC_RELAXED);
}

void cap_forget(PoolRec *r) {
    plat_mutex_lock(&cap_lock);
    for (int i = 0; i < nlist; i++)
        if (list[i] == r) { list_drop(i); break; }
    for (int i = 0; i < npacks; i++)
        for (int k = 0; k < packs[i].nruns; k++)
            if (packs[i].runs[k].pool == r) packs[i].runs[k].pool = NULL;
    plat_mutex_unlock(&cap_lock);
}

// The captures leave the list under cap_lock and are applied after it: shadow_lock, held by the caller, keeps their
// pools alive (shadow_forget takes it).
void cap_apply(uint64_t sub) {
    if (!__atomic_load_n(&npacks, __ATOMIC_RELAXED)) return;
    plat_mutex_lock(&cap_lock);
    int done = 0;
    while (done < npacks && packs[done].sub <= sub) done++;
    CapPack *take = done ? malloc(sizeof *take * (size_t)done) : NULL;
    if (done && !take) rt_fatal("vk: out of memory (shadow capture)");
    if (done) memcpy(take, packs, sizeof *take * (size_t)done);
    if (done) memmove(packs, packs + done, sizeof *packs * (size_t)(npacks - done)), npacks -= done;
    plat_mutex_unlock(&cap_lock);
    for (int i = 0; i < done; i++) {
        for (int k = 0; k < take[i].nruns; k++) {
            CapRun *c = &take[i].runs[k];
            if (c->pool) shadow_apply_run(c->pool, c->addr, c->addr + c->n, c->data), c_applied += c->n;
            free(c->data);
        }
        free(take[i].runs);
    }
    free(take);
}

int cap_split(PoolRec **rs, int n) {
    if (!__atomic_load_n(&nlist_v, __ATOMIC_RELAXED)) return n;
    int k = 0;
    for (int i = 0; i < n; i++)
        if (!rs[i]->cap_on) {
            PoolRec *t = rs[k];
            rs[k++] = rs[i], rs[i] = t;
        }
    return k;
}

void cap_activate(PoolRec **rs, int n) {
    if (!__atomic_load_n(&active, __ATOMIC_RELAXED) || !n) return;
    plat_mutex_lock(&cap_lock);
    for (int i = 0; i < n; i++) {
        PoolRec *r = rs[i];
        if (nlist == clist) vk_mem_grow((void **)&list, &clist, sizeof *list);
        list[nlist++] = r;
        r->cap_on = 1, r->cap_from = UINT64_MAX, r->cgen = r->sgen;   // the sync just copied up to sgen
    }
    __atomic_store_n(&nlist_v, nlist, __ATOMIC_RELAXED);
    plat_mutex_unlock(&cap_lock);
}

static void capture_run(PoolRec *r, uint64_t a, uint64_t b, CapRun **runs, int *nruns, int *cruns, uint64_t *bytes) {
    if (*nruns == *cruns) vk_mem_grow((void **)runs, cruns, sizeof **runs);
    uint8_t *d = malloc(b - a);
    if (!d) rt_fatal("vk: out of memory (shadow capture)");
    mem_copy_out(d, a, b - a);   // MKW_EARLY_READ: on the game's thread, a watched page must not count as its read
    (*runs)[(*nruns)++] = (CapRun){r, a, b - a, d};
    *bytes += b - a;
}

// gen_lock held: the pages of r stamped after its last capture, appended to the runs (as shadow_copy_newer walks them).
static void capture_pool(PoolRec *r, CapRun **runs, int *nruns, int *cruns, uint64_t *bytes) {
    uint64_t lo = r->import_lo, hi = r->import_hi, run = 0;
    uint32_t since = r->cgen;
    r->cgen = gen_now;
    for (uint64_t k = lo >> REG_SHIFT; k <= (hi - 1) >> REG_SHIFT; k++) {
        uint64_t a = k << REG_SHIFT, b = a + (1ull << REG_SHIFT);
        if (a < lo) a = lo;
        if (b > hi) b = hi;
        const uint32_t *ch = gen_chunk[a >> GEN_CHUNK_SHIFT];   // a 2 MiB region lies in one chunk
        if (reg_max[k] <= since || !ch) {
            if (run) capture_run(r, run, a, runs, nruns, cruns, bytes), run = 0;
            continue;
        }
        for (uint64_t x = a; x < b; x += 1ull << GEN_PAGE_SHIFT) {
            int dirty = ch[(x >> GEN_PAGE_SHIFT) & (GEN_PER_CHUNK - 1)] > since && !mem_trapped(x);
            if (dirty && !run) run = x;
            else if (!dirty && run) capture_run(r, run, x, runs, nruns, cruns, bytes), run = 0;
        }
    }
    if (run) capture_run(r, run, hi, runs, nruns, cruns, bytes);
}

void vk_mem_shadow_capture(uint64_t sub) {
    if (!shadow_live) return;
    if (mode < 0) {
        mode = rt_knob("SHADOW_SUBMIT", "0");
        rt_log("mem: the CPU's writes captured at each submit %s (MKW_SHADOW_SUBMIT)",
               mode == 2 ? "in the B blocks" : mode ? "on" : "off");
    }
    uint64_t t0 = qpc();
    int on = RT_KNOB_ON(mode);
    __atomic_store_n(&active, on, __ATOMIC_RELAXED);
    if (!on && !__atomic_load_n(&nlist_v, __ATOMIC_RELAXED)) return;
    plat_mutex_lock_shared(&pool_lock);   // the records live while it is held (vk_pool_fini takes it exclusively)
    plat_mutex_lock(&cap_lock);
    int n = 0;
    for (int i = 0; i < nlist;)
        if (!on || (list[i]->touch_sub && sub > list[i]->touch_sub + CAP_IDLE)) list_drop(i), c_dropped++;
        else i++;
    if (nlist > csnap) {
        csnap = nlist * 2;
        free(snap), free(tracked);
        snap = malloc(sizeof *snap * (size_t)csnap), tracked = malloc((size_t)csnap);
        if (!snap || !tracked) rt_fatal("vk: out of memory (shadow capture)");
    }
    memcpy(snap, list, sizeof *snap * (size_t)nlist);
    n = nlist;
    plat_mutex_unlock(&cap_lock);
    CapRun *runs = NULL;
    int nruns = 0, cruns = 0;
    uint64_t bytes = 0;
    if (n) {
        gen_lock_x();
        shadow_query_pools(snap, tracked, n, RT_KNOB_ON(hot_mode));
        for (int i = 0; i < n; i++)
            if (tracked[i]) {
                capture_pool(snap[i], &runs, &nruns, &cruns, &bytes);
                if (snap[i]->cap_from == UINT64_MAX) snap[i]->cap_from = sub;
            }
        plat_mutex_unlock(&gen_lock);
    }
    plat_mutex_lock(&cap_lock);
    for (int i = 0; i < n; i++)
        if (!tracked[i] && snap[i]->cap_on)   // untracked: read at the sync as before
            for (int k = 0; k < nlist; k++)
                if (list[k] == snap[i]) { list_drop(k); break; }
    if (nruns) {
        if (npacks == cpacks) vk_mem_grow((void **)&packs, &cpacks, sizeof *packs);
        packs[npacks++] = (CapPack){sub, runs, nruns};
    }
    c_subs++, c_pools += (uint64_t)n, c_bytes += bytes, c_ns += qpc() - t0;
    if (c_subs >= 1500) log_locked();   // ~5 s of race; shadow_log only runs at a few frames
    plat_mutex_unlock(&cap_lock);
    plat_mutex_unlock_shared(&pool_lock);
}

static void log_locked(void) {   // cap_lock held; c_applied is the replay's, read as it is
    rt_log("mem: submit captures since the last line: %llu submits, %.1f pools each, %.1f MiB captured, %.1f MiB "
           "applied, %.2f ms on the game thread, %d pools listed, %llu dropped, %d captures pending",
           (unsigned long long)c_subs, c_subs ? (double)c_pools / (double)c_subs : 0.0, c_bytes / 1048576.0,
           c_applied / 1048576.0, qpc_ms(c_ns), nlist, (unsigned long long)c_dropped, npacks);
    c_subs = c_pools = c_bytes = c_ns = c_applied = c_dropped = 0;
}

void cap_log(void) {   // shadow_lock held
    if (mode <= 0) return;
    plat_mutex_lock(&cap_lock);
    log_locked();
    plat_mutex_unlock(&cap_lock);
}

int vk_mem_shadow_state(uint64_t addr) {
    plat_mutex_lock_shared(&pool_lock);
    const pidx_ent *e = pidx_find(&pool_index, addr, 1);
    const PoolRec *r = e ? e->buf : NULL;
    int st = !r ? 0 : !r->smap ? 1 : !r->cap_on ? 2 : r->cap_from > __atomic_load_n(&vk_replay_submit, __ATOMIC_RELAXED) ? 3 : 4;
    plat_mutex_unlock_shared(&pool_lock);
    return st;
}
