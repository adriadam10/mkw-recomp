// tex_par_for: one texture job's items spread over a persistent worker pool, the caller taking part (fork-join). The
// texture work the replay thread does at a course load is serial on the race's critical path: in the load's peak (1.5
// s, tl-fp1, 16/11/2026) it spent ~66 ms choosing BC1 or BC7 for ASTC images (astc_alpha_one) and ~44 ms deswizzling
// uploads, each of them independent per block range or per row band. The pool was tex_astc_mt.c's own (the CPU ASTC
// decode); it now serves every such job.
//
// One job at a time: a second caller while the pool is busy (another thread, or an item that calls tex_par_for itself)
// does its items on its own thread, so nothing waits on the pool and nesting cannot deadlock. Every worker joins each
// job once and the caller returns when all of them have, so items must be coarse (tens of microseconds or more).
//
// Placement: the workers are background threads (plat_thread_background), on every processor the process had, a hybrid
// CPU's efficiency cores included, whoever starts them. Left to inheritance they took the mask of their creator: the
// replay thread's one processor once it presents (vk_backend.c replay_core), or every processor when a background
// thread made the device (the pipeline prewarm did, in a Grand Prix run, texpar, 17/11/2026, frame's threads on 0-13).
// Items are claimed one by one, so a slower core takes fewer of them and holds the job up by one item at
// most, and during a load these stay off the fast cores the game's threads are busy on. The replays that measured the
// pool (vk_tex_upload.c, vk_tex_image.c) ran every thread on every processor, as these are.
#include "host.h"
#include "tex.h"
#include "platform/platform.h"

#define PAR_MAX_THREADS 8   // the caller included; as tex_astc_mt.c had it

static struct {
    PlatOnce once;
    PlatMutex busy;   // held by the caller for the whole job
    PlatMutex lock;
    PlatCond work, done;
    int threads;      // workers, the caller not counted
    uint64_t gen;     // bumped per job; a worker joins each generation once
    int active;       // workers that have not finished the current generation
    // the current job (written under lock before gen is bumped)
    TexParFn *fn;
    void *ctx;
    int32_t n;
    volatile int32_t next;   // first item not yet claimed
} P = {PLAT_ONCE_INIT, .lock = PLAT_MUTEX_INIT, .work = PLAT_COND_INIT, .done = PLAT_COND_INIT};

static void claim(void) {
    for (int32_t i; (i = plat_xadd32(&P.next, 1)) < P.n;) P.fn(P.ctx, (uint32_t)i);
}

static void worker(void *arg) {
    (void)arg;
    plat_thread_background();
    uint64_t seen = 0;
    plat_mutex_lock(&P.lock);
    for (;;) {
        while (P.gen == seen) plat_cond_wait(&P.work, &P.lock);
        seen = P.gen;
        plat_mutex_unlock(&P.lock);
        claim();
        plat_mutex_lock(&P.lock);
        if (!--P.active) plat_cond_broadcast(&P.done);
    }
}

// Hardware threads minus 2 (the replay and the main guest thread), 1 to PAR_MAX_THREADS; the caller is one of them.
// MKW_TEX_PAR=0 (a measurement) starts none: every job on its caller's thread, as before the pool served them.
static void pool_init(void *param) {
    (void)param;
    if (!rt_knob("TEX_PAR", "1")) {
        rt_log("tex_par: no texture workers, every job on its caller (MKW_TEX_PAR=0)");
        return;
    }
    int cpus = (int)plat_cpu_count() - 2;
    int n = (cpus < 1 ? 1 : cpus > PAR_MAX_THREADS ? PAR_MAX_THREADS : cpus) - 1;
    for (int i = 0; i < n; i++) {
        if (!plat_thread_spawn(worker, NULL, "tex par", 0)) break;
        P.threads++;
    }
    rt_log("tex_par: %d texture workers besides the caller", P.threads);
}

static void tex_par_start(void) { plat_once(&P.once, pool_init, NULL); }

int tex_par_threads(void) {
    tex_par_start();
    return P.threads + 1;
}

void tex_par_for(uint32_t n, TexParFn *fn, void *ctx) {
    if (n > 1) tex_par_start();
    if (n < 2 || !P.threads || !plat_mutex_trylock(&P.busy)) {
        for (uint32_t i = 0; i < n; i++) fn(ctx, i);
        return;
    }
    plat_mutex_lock(&P.lock);
    P.fn = fn, P.ctx = ctx, P.n = (int32_t)n, P.next = 0;
    P.active = P.threads;
    P.gen++;
    plat_cond_broadcast(&P.work);
    plat_mutex_unlock(&P.lock);
    claim();
    plat_mutex_lock(&P.lock);
    while (P.active) plat_cond_wait(&P.done, &P.lock);
    plat_mutex_unlock(&P.lock);
    plat_mutex_unlock(&P.busy);
}
