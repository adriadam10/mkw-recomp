// MKW_MEM_SPILL (on by default on Linux, 0 turns it off): which game memory leaves RAM for a file (core/memory/memory_spill.c does the moving). A pool the
// game made without CPU access (NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS) and the GPU has never resolved (still lazy, no GPU
// storage made from its pages) holds bytes only the runtime reads: texture uploads cut from it, which take the bytes
// back on their first touch. A thread looks every MKW_MEM_SPILL_SWEEP_MS (5000) for such pools older than
// MKW_MEM_SPILL_AGE_MS (10000: the game's first frames with them upload their textures; a capture's replay lasts ~12 s)
// and spills their read-write pages, pages filled back since included. Each 2 MiB
// goes under pool_lock shared, so lazy_back (pool_lock exclusive, which thaws the pool before importing it) never
// imports a pool half spilled; the order pool_lock then frz_lock is lazy_back's own. Each step first hands the write
// watch's marks of its pages to the generation table (vk_mem_changed), outside pool_lock: a page still marked written
// is not spilled (memory_spill.c), and dropping it would lose a write the texture checks have not seen.
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "nvn_api.h"
#include "nvn_pool_index.h"
#include "vk.h"
#include "vk_mem_internal.h"
#include "platform/platform.h"

#define STEP (2ull << 20)

static uint32_t knob_ms(const char *name, uint32_t def) {
    const char *e = cfg_get(name);
    return e && *e ? (uint32_t)strtoul(e, NULL, 10) : def;
}

// MKW_MEM_SPILL_ANY=1 (diagnosis): every lazy pool, CPU-accessible ones too, to measure what the game reads back.
static int spill_any;

static int eligible(const PoolRec *r) {
    return r->kind == POOL_IMPORT && r->lazy && (spill_any || (r->flags & NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS));
}

// One step of the pool keyed key at [lo, hi): spilled only if the pool is still there and still eligible.
static uint64_t spill_step(const pool_t *key, uint64_t lo, uint64_t hi) {
    uint64_t done = 0;
    uint32_t now;
    vk_mem_changed(lo, hi - lo, 0, &now);
    plat_mutex_lock_shared(&pool_lock);
    const pidx_ent *e = pidx_find(&pool_index, (uintptr_t)lo, 0);
    if (e && e->key == key && eligible(e->buf) && !zc_in_use(lo, hi)) done = mem_spill(lo, hi);
    plat_mutex_unlock_shared(&pool_lock);
    return done;
}

static void sweep_main(void *arg) {
    (void)arg;
    typedef struct { const pool_t *key; uint64_t lo, hi; } Cand;
    Cand *c = NULL;
    int cap = 0;
    int64_t last[5] = {0};
    uint32_t sweep_ms = knob_ms("MKW_MEM_SPILL_SWEEP_MS", 5000), age_ms = knob_ms("MKW_MEM_SPILL_AGE_MS", 10000);
    __atomic_store_n(&spill_any, (int)knob_ms("MKW_MEM_SPILL_ANY", 0) != 0, __ATOMIC_RELAXED);
    for (;;) {
        plat_sleep_ms(sweep_ms);
        uint32_t now = plat_now_ms();
        int n = 0;
        plat_mutex_lock_shared(&pool_lock);
        for (int i = 0; i < pool_index.n; i++) {
            const PoolRec *r = pool_index.e[i].buf;
            if (!eligible(r) || now - r->born_ms < age_ms) continue;
            if (n == cap && !(c = realloc(c, (size_t)(cap = cap ? cap * 2 : 64) * sizeof *c)))
                rt_fatal("vk: out of memory for the spill sweep");
            c[n++] = (Cand){r->key, r->lo, r->lo + r->size};
        }
        plat_mutex_unlock_shared(&pool_lock);
        uint64_t done = 0;
        for (int i = 0; i < n; i++)
            for (uint64_t a = c[i].lo; a < c[i].hi; a += STEP)
                done += spill_step(c[i].key, a, c[i].hi - a > STEP ? a + STEP : c[i].hi);
        int64_t s[5];
        mem_spill_stats(s);
        if (memcmp(s, last, sizeof s)) {
            rt_log("mem: spill: %.1f MiB out now (+%.1f this sweep, %d pools); so far spilled %.1f MiB (all-zero %.1f), "
                   "filled back by an access %.1f MiB, by a thaw %.1f MiB",
                   s[0] / 256.0, done / 1048576.0, n, s[1] / 256.0, s[2] / 256.0, s[3] / 256.0, s[4] / 256.0);
            memcpy(last, s, sizeof s);
        }
    }
}

void vk_mem_spill_start(void) {
    static volatile int32_t started;
    if (__atomic_exchange_n(&started, 1, __ATOMIC_ACQ_REL) || !mem_spill_on()) return;
    if (!plat_thread_spawn(sweep_main, NULL, "mem spill", 0)) rt_log("mem: MKW_MEM_SPILL: no sweep thread, nothing spilled");
}
