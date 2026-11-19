// MKW_WB_PAR=N (default 0: off; "ab": 4 threads in the B blocks): the fence's write-backs on N threads besides the
// completion thread. Before a fence's waiters are released the completion thread writes back what the GPU wrote into guest memory
// (cb_done -> vk_gpu_written -> merge_back): in a 3-player race ~600 ranges and ~23 MiB a frame, 3.2 of the 3.7 ms
// between the GPU reaching the value and the game being released (MKW_WB_CENSUS, MKW_TRACE_TL; RTX 3090, Core Ultra 7
// 270K Plus, Linux, 19/11/2026, #641),
// nearly all of it comparing bytes the GPU did not change, on the frame's critical path while every other frame thread
// waits. vk_wb_par runs fn(ctx, i) for i in [0, n) on the caller and the pool and returns when all are done; inside a
// pool item it runs serially (no nesting). Used by cb_done (a callback's ranges) and merge_back (slices of one large
// range, when its callback has too few ranges to share out).
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "vk.h"
#include "platform/platform.h"

#define WP_MAX 16
#define WP_SPIN_NS 200000ull   // a worker polls this long for the next job before it sleeps: a fence's callbacks follow
                               // each other within a millisecond, and a futex wake costs 20-100 us each time

typedef struct {
    void (*fn)(void *, int);
    void *ctx;
    int n;
    volatile int32_t next;
    int active;   // wp_lock: workers inside this job
} WpJob;

static PlatMutex wp_lock = PLAT_MUTEX_INIT, wp_call = PLAT_MUTEX_INIT;
static PlatCond wp_work = PLAT_COND_INIT, wp_idle = PLAT_COND_INIT;
static WpJob *volatile wp_job;   // written under wp_lock; polled without it by spinning workers
static int wp_threads = -1, wp_ab;
static _Thread_local int wp_inside;
static volatile int64_t wp_jobs, wp_items, wp_ns;

static inline void relax(void) {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#endif
}

static void wp_drain(WpJob *j) {
    for (;;) {
        int i = __atomic_fetch_add(&j->next, 1, __ATOMIC_RELAXED);
        if (i >= j->n) return;
        j->fn(j->ctx, i);
    }
}

static void wp_main(void *arg) {
    (void)arg;
    wp_inside = 1;
    plat_mutex_lock(&wp_lock);
    for (;;) {
        WpJob *j = wp_job;
        if (!j || __atomic_load_n(&j->next, __ATOMIC_RELAXED) >= j->n) {
            plat_mutex_unlock(&wp_lock);
            uint64_t t0 = plat_now_ns();
            int seen = 0;
            while (!(seen = __atomic_load_n(&wp_job, __ATOMIC_RELAXED) != j) && plat_now_ns() - t0 < WP_SPIN_NS) relax();
            plat_mutex_lock(&wp_lock);
            if (!seen && wp_job == j) plat_cond_wait(&wp_work, &wp_lock);
            continue;
        }
        j->active++;
        plat_mutex_unlock(&wp_lock);
        wp_drain(j);
        plat_mutex_lock(&wp_lock);
        if (!--j->active) plat_cond_broadcast(&wp_idle);
    }
}

int vk_wb_par_threads(void) {
    if (wp_threads >= 0) return wp_ab && !rt_ab() ? 0 : wp_threads;
    plat_mutex_lock(&wp_lock);
    if (wp_threads < 0) {
        const char *e = cfg_get("MKW_WB_PAR");
        wp_ab = e && !strcmp(e, "ab");
        int n = wp_ab ? 4 : e ? atoi(e) : 0, ok = 0;
        if (n > WP_MAX) n = WP_MAX;
        for (int i = 0; i < n; i++) ok += plat_thread_spawn(wp_main, NULL, "vk wb par", 0);
        if (n > 0) rt_log("vk: fence write-backs on %d threads besides the completion thread%s (MKW_WB_PAR)", ok,
                          wp_ab ? " in the B blocks" : "");
        wp_threads = ok;
    }
    plat_mutex_unlock(&wp_lock);
    return wp_ab && !rt_ab() ? 0 : wp_threads;
}

int vk_wb_par_inside(void) { return wp_inside; }

void vk_wb_par(int n, void (*fn)(void *, int), void *ctx) {
    if (n < 2 || wp_inside || vk_wb_par_threads() <= 0 || !plat_mutex_trylock(&wp_call)) {
        for (int i = 0; i < n; i++) fn(ctx, i);
        return;
    }
    uint64_t t0 = plat_now_ns();
    WpJob j = {.fn = fn, .ctx = ctx, .n = n};
    plat_mutex_lock(&wp_lock);
    wp_job = &j;
    plat_cond_broadcast(&wp_work);
    plat_mutex_unlock(&wp_lock);
    wp_inside = 1;
    wp_drain(&j);
    wp_inside = 0;
    plat_mutex_lock(&wp_lock);
    while (j.active) plat_cond_wait(&wp_idle, &wp_lock);
    wp_job = NULL;   // after the last worker left: one arriving late finds next >= n while the job stands
    plat_mutex_unlock(&wp_lock);
    plat_mutex_unlock(&wp_call);
    int64_t jobs = __atomic_add_fetch(&wp_jobs, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&wp_items, n, __ATOMIC_RELAXED);
    __atomic_fetch_add(&wp_ns, (int64_t)(plat_now_ns() - t0), __ATOMIC_RELAXED);
    if (jobs % 20000 == 0)
        rt_log("vk: write-backs in parallel: 20000 jobs, %.1f items each, %.1f us each (MKW_WB_PAR)",
               (double)__atomic_exchange_n(&wp_items, 0, __ATOMIC_RELAXED) / 20000.0,
               (double)__atomic_exchange_n(&wp_ns, 0, __ATOMIC_RELAXED) / 20000.0 / 1e3);
}
