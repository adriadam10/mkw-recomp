// The pipeline-key file and the boot prewarm, split from vk_pipeline.c (see vk_pipe_internal.h).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "solid_spv.h"
#include "vk_spirv_patch.h"
#include "gfx.h"
#include "host.h"
#include "nvn_hmap.h"
#include "platform/platform.h"
#include "tex.h"
#include "vk_pipe_internal.h"

// ---- pipeline keys (pipeline-keys.bin next to the pipeline cache)
// Every graphics pipeline built in a game run appends a KeyRec: the VkPipeState it was built from with the shader
// pointers zeroed and the shaders named by their program keys instead (gfx_shader_input_id), so the record rebuilds the
// pipeline without guest memory. At the next boot vk_pipeline_prewarm (rt_boot, before the guest runs;
// MKW_PIPE_PREBUILD=0 skips it) loads each record's shaders (gfx_shader_prewarm; a record whose shader is missing is
// dropped; an integer-input vertex shader is made again from the record's attribute formats), enters each Pipe as building under the key a draw would compute, queues it to the low queue and builds the queue on
// every core. Records are deduplicated by a hash of their bytes; they are collected under P.lock by the worker that
// built the pipeline and appended by the present thread (vk_pipeline_autosave) or at exit (vk_pipeline_save). A file
// with another magic, version or record size is ignored and rewritten. Bump KEYS_VERSION when VkPipeState or the
// record changes meaning without changing size.
#define KEYS_VERSION 5                 // 3: shaders named by input id (gfx_shader.c); 4: prepared shaders by program key; 5: VkPipeState.mesh
KeyFile K = {.io = PLAT_MUTEX_INIT};

static uint64_t rec_hash(const KeyRec *r) {
    uint64_t h = pipe_fnv(FNV0, r, sizeof *r);
    return h ? h : 1;
}

// 1 if h was new. Caller holds P.lock.
static int seen_add(uint64_t h) {
    if ((K.seen_n + 1) * 2 > K.seen_cap) {
        uint32_t cap = K.seen_cap ? K.seen_cap * 2 : 4096;
        uint64_t *t = calloc(cap, sizeof *t);
        if (!t) return 1;
        for (uint32_t i = 0; i < K.seen_cap; i++) {
            if (!K.seen[i]) continue;
            uint32_t j = (uint32_t)K.seen[i] & (cap - 1);
            while (t[j]) j = (j + 1) & (cap - 1);
            t[j] = K.seen[i];
        }
        free(K.seen);
        K.seen = t, K.seen_cap = cap;
    }
    uint32_t j = (uint32_t)h & (K.seen_cap - 1);
    while (K.seen[j]) {
        if (K.seen[j] == h) return 0;
        j = (j + 1) & (K.seen_cap - 1);
    }
    K.seen[j] = h;
    K.seen_n++;
    return 1;
}

static void rec_fill(KeyRec *r, const VkPipeState *s) {
    memset(r, 0, sizeof *r);
    memcpy(r->vs, gfx_shader_input_id(s->vs), 32);
    if (s->fs) memcpy(r->fs, gfx_shader_input_id(s->fs), 32), r->has_fs = 1;
    r->s = *s;
    r->s.vs = r->s.fs = r->s.mesh = NULL;
    r->s.prim_restart = 0;   // dynamic state: the same pipeline either way
}

// A pipeline was built for a draw: queue its record for the file. Caller holds P.lock.
static void keys_note(const VkPipeState *s) {
    // Variants (vk_spec.c) have no prepared file: a record of one could not be loaded at the next boot.
    if (!A.on || !K.path[0] || mesh_pipe(s) || gfx_shader_variant_of(s->vs) || (s->fs && gfx_shader_variant_of(s->fs))) return;
    KeyRec r;
    rec_fill(&r, s);
    if (!seen_add(rec_hash(&r))) return;
    if (K.pend_n == K.pend_cap) {
        uint32_t cap = K.pend_cap ? K.pend_cap * 2 : 64;
        KeyRec *t = realloc(K.pend, cap * sizeof *t);
        if (!t) return;
        K.pend = t, K.pend_cap = cap;
    }
    K.pend[K.pend_n++] = r;
}

// Appends the pending records (present thread or exit; never the render thread).
void keys_flush(void) {
    if (!K.path[0]) return;
    plat_mutex_lock(&K.io);
    plat_mutex_lock(&P.lock);
    KeyRec *r = K.pend;
    uint32_t n = K.pend_n;
    K.pend = NULL, K.pend_n = K.pend_cap = 0;
    plat_mutex_unlock(&P.lock);
    if (n) {
        FILE *f = fopen(K.path, K.header_ok ? "ab" : "wb");
        int ok = f != NULL;
        if (ok && !K.header_ok) {
            KeysHeader h = {{'M', 'K', 'W', '_', 'P', 'K', 'E', 'Y'}, KEYS_VERSION, sizeof(KeyRec)};   // keys_open checks it
            ok = fwrite(&h, sizeof h, 1, f) == 1;
        }
        if (ok) ok = fwrite(r, sizeof *r, n, f) == n;
        if (ok) ok = plat_file_sync(f);   // appended records on the disk before the next append
        if (f) ok = !fclose(f) && ok;
        if (ok) K.header_ok = 1;
        else rt_log("vk_pipeline: cannot write %s", K.path);
    }
    free(r);
    plat_mutex_unlock(&K.io);
}

// Reads the key file into K.file and its record hashes into the set. Caller holds P.lock (ensure_init).
void keys_open(void) {
    if (!A.on || !P.cache_path[0]) return;
    snprintf(K.path, sizeof K.path, "%s", P.cache_path);
    char *slash = strrchr(K.path, '\\'), *fslash = strrchr(K.path, '/');
    if (fslash > slash) slash = fslash;
    snprintf(slash ? slash + 1 : K.path, sizeof K.path - (size_t)(slash ? slash + 1 - K.path : 0), "pipeline-keys.bin");
    FILE *f = fopen(K.path, "rb");
    if (!f) return;
    KeysHeader h;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len >= (long)sizeof h && fread(&h, sizeof h, 1, f) == 1 && !memcmp(h.magic, "MKW_PKEY", 8) &&
        h.version == KEYS_VERSION && h.rec_size == sizeof(KeyRec)) {
        K.header_ok = 1;
        size_t n = ((size_t)len - sizeof h) / sizeof(KeyRec);
        if (n && (K.file = malloc(n * sizeof(KeyRec))) && fread(K.file, sizeof(KeyRec), n, f) == n) K.file_n = n;
        else free(K.file), K.file = NULL;
        for (size_t i = 0; i < K.file_n; i++) seen_add(rec_hash((const KeyRec *)K.file + i));
        if (len != (long)(sizeof h + n * sizeof(KeyRec)))   // a torn append: rewrite the file from what was read
            K.header_ok = 0;
    } else {
        rt_log("vk_pipeline: %s has another format or version; it is rewritten", K.path);
    }
    fclose(f);
    if (!K.header_ok && K.file_n) {   // keep the whole records of a torn file
        KeyRec *t = malloc(K.file_n * sizeof *t);
        if (t) memcpy(t, K.file, K.file_n * sizeof *t), K.pend = t, K.pend_n = K.pend_cap = (uint32_t)K.file_n;
    }
}

static double now_ms(void) { return (double)plat_now_ns() / 1e6; }

// Caller holds P.lock.
static void prebuild_report(void) {
    rt_log("vk_pipeline: prebuild done: %u built, %u failed, %u asked for by a draw first; %.0f ms after the device was "
           "ready", K.built, K.failed, K.promoted, now_ms() - K.t0);
}

static void pipe_worker(void *arg) {
    int me = (int)(intptr_t)arg;
    plat_mutex_lock(&P.lock);
    for (;;) {
        // With 3 or more workers one never takes low-queue work, so a waiting draw's library starts at once
        // (not with MKW_PIPE_WAIT=draw).
        while (!U.head && !A.head && !(A.low && (A.threads < 3 || !U.on || A.low_running < A.threads - 1))) {
            U.idle++;
            plat_cond_wait(&work_cv, &P.lock);
            U.idle--;
        }
        if (U.head) {   // a library a draw waits for
            LibJob *lj = U.head;
            if (!(U.head = lj->next)) U.tail = NULL;
            lj->b->job = NULL;
            if (A.prio_low[me]) A.prio_low[me] = 0, plat_thread_priority(PLAT_PRIO_NORMAL);
            U.by_worker++, U.running++;
            lib_build(lj->b, lj->kind, &lj->s, &lj->k, lj->l);   // releases P.lock while building
            U.running--;
            free(lj);
            continue;
        }
        Job *j = A.head;
        int lowrun = !j;
        if (j) {
            if (!(A.head = j->next)) A.tail = NULL;
        } else {
            j = A.low;
            if ((A.low = j->next)) A.low->prev = NULL;
            else A.low_tail = NULL;
            A.low_running++;
        }
        j->next = j->prev = NULL;
        j->worker = me;
        if (j->low != A.prio_low[me]) {   // under P.lock, so a draw raising this thread (promote) is not undone
            A.prio_low[me] = j->low;
            plat_thread_priority(j->low ? PLAT_PRIO_BELOW_NORMAL : PLAT_PRIO_NORMAL);
        }
        // Step 1 for a job a draw waits for: its stage libraries and the fast link. Step 2, the optimised build, goes to
        // the head of the low queue, so a burst of new pipelines gets every fast link before any optimised build.
        if (P.gpl && (!j->prebuild || j->demanded) && !j->p->info.pipeline && !j->p->fast_tried && j->p->lay) {
            int final;
            VkPipeline f = gpl_link(&j->s, &j->k, j->p->lay, 1, &final);   // releases P.lock while building
            j->p->fast_tried = 1;
            if (f && !j->p->info.pipeline) {
                j->p->fast = j->p->info.pipeline = f, P.g.wlinks++;
                plat_cond_broadcast(&done_cv);
                j->low = 1, j->worker = -1, P.g.requeued++;
                j->prev = NULL, j->next = A.low;
                if (A.low) A.low->prev = j;
                else A.low_tail = j;
                A.low = j;
                A.low_running -= lowrun;
                continue;
            } else if (f) {
                vkDestroyPipeline(P.device, f, NULL);
            }
        }
        plat_mutex_unlock(&P.lock);
        VkPipeline pipe = create_graphics(&j->s, &j->k, j->l, j->fs_off);
        plat_mutex_lock(&P.lock);
        if (pipe || !j->p->fast) j->p->info.pipeline = pipe;   // a failed optimised build keeps the fast-linked one
        if (pipe && j->p->fast) P.g.swaps++;
        j->p->building = 0;
        j->p->job = NULL;
        if (pipe) {
            P.stats.graphics++, P.stats.async_built++;
            if (!j->prebuild) keys_note(&j->s);
        } else {
            P.stats.failures++;
            rt_log("vk_pipeline: graphics pipeline not built: pipeline creation failed");
        }
        if (j->prebuild) {
            if (pipe) K.built++;
            else K.failed++;
            K.promoted += j->demanded;
            if (!--K.left && !K.loading) prebuild_report();
        }
        A.low_running -= lowrun;
        if (!--A.inflight) plat_cond_broadcast(&A.idle);
        plat_cond_broadcast(&done_cv);
        if (pipe && (!j->prebuild || j->demanded)) perf_count(PERF_CNT_PIPE_ASYNC);   // a draw waited for it
        free(j);
    }
    return;
}

// Starts the pool on first use: a quarter of the logical processors, 2 to 4; one on a machine under 16 GB, because
// RADV compiling one of MKW's large pipelines holds gigabytes (heaptrack, run68: 5.2 GB of the process's peak inside
// vkCreateGraphicsPipelines on three workers, and systemd-oomd ended runs 54-67). MKW_PIPE_WORKERS=<n> sets it. Caller
// holds P.lock.
static int workers_start(void) {
    if (A.threads) return 1;
    uint32_t cpus = plat_cpu_count();
    int n = cpus / 4 < 2 ? 2 : cpus / 4 > MAX_WORKERS ? MAX_WORKERS : (int)(cpus / 4);
    float used, total = 0, proc;
    plat_mem_info(&used, &total, &proc);
    if (total > 0 && total < 16 * 1024) n = 1;
    const char *w = cfg_get("MKW_PIPE_WORKERS");
    if (w && atoi(w) > 0) n = atoi(w) > MAX_WORKERS ? MAX_WORKERS : atoi(w);
    for (int i = 0; i < n; i++) {
        PlatThread *t = plat_thread_new(pipe_worker, (void *)(intptr_t)A.threads, "pipe worker");
        if (!t) break;
        A.thread[A.threads++] = t;   // kept: promote() raises a worker running a prebuild job
    }
    if (A.threads) rt_log("vk_pipeline: %d background graphics pipeline workers", A.threads);
    else rt_log("vk_pipeline: cannot start pipeline workers; building synchronously");
    return A.threads != 0;
}

void workers_drain(void) {
    plat_mutex_lock(&P.lock);
    K.loading = 0;
    for (Job *j = A.low, *n; j; j = n) {   // queued prebuild jobs are dropped, not built
        n = j->next;
        j->p->building = 0, j->p->job = NULL;
        A.inflight--, K.left--;
        free(j);
    }
    A.low = A.low_tail = NULL;
    for (LibJob *j = U.head, *n; j; j = n) {   // queued library jobs too: their entries read as failed builds
        n = j->next;
        j->b->job = NULL, j->b->building = 0;
        free(j);
    }
    U.head = U.tail = NULL;
    while (A.inflight) plat_cond_wait(&A.idle, &P.lock);
    while (U.running) plat_cond_wait(&done_cv, &P.lock);
    plat_mutex_unlock(&P.lock);
}

void vk_pipeline_async_enable(void) {
    U.on = strcmp(cfg_value("MKW_PIPE_WAIT"), "draw") != 0;
    const char *e = cfg_get("MKW_PIPE_SYNC");
    if (e && *e && strcmp(e, "0")) return;
    A.on = 1;
}

// A draw needs a pipeline a prebuild job owns: the job moves to the draw queue, or its worker to normal priority.
// Caller holds P.lock.
static void promote(Pipe *p) {
    Job *j = p->job;
    if (!j || !j->low) return;
    j->low = 0, j->demanded = 1;
    if (j->worker < 0) {
        if (j->prev) j->prev->next = j->next;
        else A.low = j->next;
        if (j->next) j->next->prev = j->prev;
        else A.low_tail = j->prev;
        j->next = j->prev = NULL;
        if (A.tail) A.tail->next = j;
        else A.head = j;
        A.tail = j;
        plat_cond_signal(&work_cv);
    } else if (A.prio_low[j->worker]) {
        A.prio_low[j->worker] = 0;
        plat_thread_set_priority(A.thread[j->worker], PLAT_PRIO_NORMAL);
    }
}

// Enters a missing pipeline: built here (mode 0), or queued for a draw (1) or for the prebuild (2). Failures are
// entered too. *queued: 1 when a job owns it. Caller holds P.lock.
static Pipe *pipe_miss(const VkPipeState *s, const GKey *k, uint64_t h, int mode, int *queued) {
    *queued = 0;
    const char *why = NULL;
    Bindings B = {0};
    Layout *l = NULL;
    uint32_t span = binding_span(s->vs);
    if (gfx_shader_stage(s->vs) != GFX_STAGE_VS || (s->fs && gfx_shader_stage(s->fs) != GFX_STAGE_FS)) why = "stage mismatch";
    else if (k->topo_class == 3) why = "patches need tessellation stages, which are not translated";
    else if (k->meshp) {   // one set: task, mesh, fragment (mesh_offsets)
        uint32_t mo, fo;
        mesh_offsets(s, &mo, &fo);
        if (!vk_ctx()->has_mesh) why = "no VK_EXT_mesh_shader";
        else if (vk_mesh_cs_on()) why = "task/mesh drawn as compute (MKW_MESH_COMPUTE)";
        else if (!add_stage(&B, s->vs, 0) || (s->mesh && !add_stage(&B, s->mesh, mo)) || (s->fs && !add_stage(&B, s->fs, fo)))
            why = "too many bindings";
        else if (!(l = layout_get(&B))) why = "descriptor layout creation failed";
    } else if (P.indep) {
        Bindings F = {0};
        if (!add_stage(&B, s->vs, 0) || (s->fs && !add_stage(&F, s->fs, 0))) why = "too many bindings";
        else if (!(l = layout_pair(&B, &F))) why = "descriptor layout creation failed";
        span = 1;
    } else if (!add_stage(&B, s->vs, 0) || (s->fs && !add_stage(&B, s->fs, span - 1))) why = "too many bindings";
    else if (!(l = layout_get(&B))) why = "descriptor layout creation failed";
    char lack[128];
    if (!why && !k->meshp && !layout_covers(s, l, span - 1, lack, sizeof lack)) why = lack, l = NULL;
    VkPipeline pipe = VK_NULL_HANDLE;
    Job *j = NULL;
    if (!why && mode && workers_start() && (j = calloc(1, sizeof *j))) {
        *j = (Job){.l = l, .fs_off = span - 1, .low = mode == 2, .prebuild = mode == 2, .worker = -1, .s = *s, .k = *k};
    } else if (!why && !(pipe = create_graphics(s, k, l, span - 1))) {
        why = "pipeline creation failed";
    }
    if (why) {
        P.stats.failures++;
        rt_log("vk_pipeline: graphics pipeline not built: %s", why);
    } else if (!j) {
        P.stats.graphics++;
        if (mode != 2) keys_note(s);
    }
    Pipe *p = pipe_add(k, sizeof *k, h, pipe, l, l ? &B : NULL, VK_PIPELINE_BIND_POINT_GRAPHICS, gfx_shader_inputs(s->vs, NULL));
    if (!j || !p) {
        free(j);
        return p;
    }
    j->p = p;
    p->building = 1, p->job = j;
    if (mode == 2) {
        j->prev = A.low_tail;
        if (A.low_tail) A.low_tail->next = j;
        else A.low = j;
        A.low_tail = j;
        K.left++;
    } else {
        if (A.tail) A.tail->next = j;
        else A.head = j;
        A.tail = j;
    }
    A.inflight++;
    plat_cond_signal(&work_cv);
    *queued = 1;
    return p;
}

// A job still in the draw queue goes to the head of the low queue: its pipeline has (or is about to have) a fast-linked
// one, so the optimised build waits behind other draws' misses. Caller holds P.lock.
static void job_lower(Job *j) {
    if (!j || j->worker >= 0 || j->low) return;
    if (A.head == j) {
        if (!(A.head = j->next)) A.tail = NULL;
    } else {
        Job *q = A.head;
        while (q && q->next != j) q = q->next;
        if (!q) return;
        if (!(q->next = j->next)) A.tail = q;
    }
    j->low = 1, P.g.requeued++;
    j->prev = NULL, j->next = A.low;
    if (A.low) A.low->prev = j;
    else A.low_tail = j;
    A.low = j;
}

#define LIB_HELPERS 4   // lib_wait's helper threads at once; beyond them a draw waits for a worker

static void lib_helper(void *arg) {
    LibJob *j = arg;
    plat_mutex_lock(&P.lock);
    lib_build(j->b, j->kind, &j->s, &j->k, j->l);
    U.helpers--;
    plat_mutex_unlock(&P.lock);
    free(j);
}

// A draw waits for pipeline s: its missing pre-rasterization and fragment shader libraries go to the head of the
// worker queues (U), so they build in parallel on idle workers, and this thread sleeps until both are published or
// end (now_ms) passes. A job no worker took within 1 ms (all busy) is built on a helper thread, never here: a driver
// build may not return, and this thread must be free to give up at end (issue #36: AMD's Windows driver 26.10.1 held
// the first frame in one library build; the window never opened). Caller holds P.lock.
static void lib_wait(const VkPipeState *s, const GKey *k, Layout *l, double end) {
    ViKey vk_;
    PrKey pk;
    FsKey fk;
    FoKey ok;
    lib_keys(s, k, &vk_, &pk, &fk, &ok);
    U.waits++;
    const void *key[2] = {&pk, &fk};
    uint32_t size[2] = {sizeof pk, sizeof fk};
    Lib *b[2];
    int queued = 0;
    for (int i = 0; i < 2; i++) {
        uint64_t h = key_hash(key[i], size[i]);
        if ((b[i] = lib_find(h, key[i], size[i]))) continue;
        if (!half_layout(l, i)) return;   // gpl_link reports it
        LibJob *j = calloc(1, sizeof *j);
        if (!j || !(b[i] = lib_new(h, key[i], size[i]))) {
            free(j);
            return;
        }
        *j = (LibJob){.b = b[i], .kind = i ? LIB_FS : LIB_PR, .l = l, .s = *s, .k = *k};
        b[i]->job = j;
        if (U.tail) U.tail->next = j;
        else U.head = j;
        U.tail = j;
        U.queued++, queued++;
    }
    if (queued) plat_cond_broadcast(&work_cv);
    double steal_at = now_ms() + 1.0;
    for (;;) {
        if (!b[0]->building && !b[1]->building) return;
        double now = now_ms();
        if (now >= end) {
            U.timeouts++;
            return;
        }
        Lib *q = b[0]->job ? b[0] : b[1]->job ? b[1] : NULL;
        if (q && (now >= steal_at || !U.idle) && U.helpers < LIB_HELPERS) {
            LibJob *j = lib_take(q);
            if (plat_thread_spawn(lib_helper, j, "pipe lib", 0)) {
                U.stolen++, U.helpers++;
            } else {   // no thread: back to the head of the queue for a worker
                j->next = U.head, U.head = j, q->job = j;
                if (!U.tail) U.tail = j;
                steal_at = end;
            }
            continue;
        }
        if (q && now >= steal_at) steal_at = end;   // helpers all busy: a worker takes it
        double until = q && steal_at < end ? steal_at : end;
        plat_cond_wait_ms(&done_cv, &P.lock, (uint32_t)(until - now) + 1);
    }
}

#define GIVE_UP_MS 5000u   // a wait this long (the one-shot draws' 30 s) that ends without the pipeline gives it up

static const char *lib_state(const void *key, uint32_t size) {
    Lib *b = lib_find(key_hash(key, size), key, size);
    return !b ? "absent" : b->job ? "queued" : b->building ? "building" : b->pipe ? "built" : "failed";
}

static void shader_hex(const GfxShader *sh, char out[65]) {
    const uint8_t *id = sh ? gfx_shader_key(sh) : NULL;
    if (!id) {
        snprintf(out, 65, "none");
        return;
    }
    for (int i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", id[i]);
}

// A draw waited its whole wait_ms and the pipeline is still building: the driver may never return it (issue #36: AMD's
// Windows driver 26.10.1, the first frame waited 30 s per frame for one full-screen pipeline and the window never
// opened). The shader keys name the prepared SPIR-V (spirv/<key>.*); the library states say which build is stuck.
// Caller holds P.lock.
static void gave_up_report(Pipe *p, const VkPipeState *s, const GKey *k, uint32_t wait_ms) {
    static int n;
    if (n++ >= 8) return;
    char vs[65], fs[65], libs[96] = "";
    shader_hex(s->vs, vs), shader_hex(s->fs, fs);
    if (P.gpl && !k->meshp) {
        ViKey v;
        PrKey pk;
        FsKey fk;
        FoKey ok;
        lib_keys(s, k, &v, &pk, &fk, &ok);
        snprintf(libs, sizeof libs, "; libraries: pre-rasterization %s, fragment %s, fast link %s",
                 lib_state(&pk, sizeof pk), lib_state(&fk, sizeof fk), p->fast_tried ? "tried" : "not tried");
    }
    rt_log("vk_pipeline: a draw gave up on its pipeline after %u ms; later draws skip it until it is built. vs %s fs %s%s; "
           "workers %d (%d idle, %d on libraries), %u jobs in flight",
           wait_ms, vs, fs, libs, A.threads, U.idle, U.running, (unsigned)A.inflight);
}

VkPipeline pipe_graphics(const VkPipeState *s, const VkPipeInfo **info, int async, int *pending, uint32_t wait_ms,
                           int low) {
    if (info) *info = NULL;
    if (pending) *pending = 0;
    if (!s->vs || !ensure_init()) return VK_NULL_HANDLE;
    GKey k;
    gkey_fill(s, &k);
    uint64_t h = key_hash(&k, sizeof k);
    plat_mutex_lock(&P.lock);
    Pipe *p = pipe_find(&k, sizeof k, h);
    int queued;
    if (p) {
        if (!p->building) P.stats.hits++;
    } else {
        p = pipe_miss(s, &k, h, async, &queued);
        if (low && p && p->building) job_lower(p->job);   // behind the draws' own misses
    }
    // A draw that would wait (wait_ms) gets its missing shader libraries built at once, the vertex and fragment ones in
    // parallel on idle workers (lib_wait; about 0.12 ms per library from the driver's disk cache, 5-180 ms cold), and
    // links them here: its own job may queue behind optimised builds of earlier pipelines. The job then only does the
    // optimised build, from the low queue. Other draws leave the libraries to the job and are skipped meanwhile.
    // MKW_PIPE_WAIT=draw builds them on this thread one after the other instead, as before; MKW_GPL_DRAW_BUILD=0
    // leaves them to the job.
    static int draw_build = -1;
    if (draw_build < 0) {
        const char *e = cfg_get("MKW_GPL_DRAW_BUILD");
        draw_build = !(e && !strcmp(e, "0"));
        rt_log("vk_pipeline: a draw that waits for its pipeline %s", !draw_build ? "leaves its libraries to its job"
               : U.on ? "has its libraries built in parallel on the workers, ahead of other jobs"
                      : "builds its libraries itself (MKW_PIPE_WAIT=draw)");
    }
    if (p && p->gave_up && p->building && !p->info.pipeline) wait_ms = 0;   // one full wait per pipeline, not one per frame
    double end = wait_ms ? now_ms() + wait_ms : 0;
    if (p && p->building && P.gpl && !k.meshp && !p->info.pipeline && !p->fast_tried && p->lay) {
        int final = 0, build = wait_ms != 0 && draw_build;
        uint64_t p0 = perf_begin(), t0 = pipe_qpc();
        if (build && U.on) {
            job_lower(p->job);   // its worker would only wait for the same libraries; it links them if this draw cannot
            lib_wait(s, &k, (Layout *)p->lay, end);   // releases P.lock while waiting
            build = 0;                                // links here when both libraries exist (gpl_link build 0)
        } else if (build) {
            p->fast_tried = 1;   // the job, if a worker takes it meanwhile, goes straight to the optimised build
        }
        VkPipeline f = p->info.pipeline ? VK_NULL_HANDLE : gpl_link(s, &k, p->lay, build, &final);   // build 1 releases P.lock
        perf_end(PERF_PIPE, p0);
        if (wait_ms && draw_build) {
            double ms = ms_since(t0);
            P.g.dlinks++, P.g.dlink_ms += ms;
            if (ms > P.g.dlink_max) P.g.dlink_max = ms;
            if (ms > 5.0 && P.g.slow++ < 16)
                rt_log("vk_pipeline: gpl: a waiting draw %s its libraries in %.1f ms", U.on ? "got" : "built", ms);
        }
        if (final) p->fast_tried = 1;
        if (f && !p->info.pipeline) {
            p->fast = p->info.pipeline = f;
            job_lower(p->job);
            plat_cond_broadcast(&done_cv);
        } else if (f) {
            vkDestroyPipeline(P.device, f, NULL);   // a worker published one while the lock was free
        }
    }
    if (p && p->building && !p->info.pipeline) {
        promote(p);
        uint64_t p0 = wait_ms ? perf_begin() : 0;   // the wait counts as the render thread's pipeline time
        double now;
        while (p->building && !p->info.pipeline && wait_ms && (now = now_ms()) < end)
            plat_cond_wait_ms(&done_cv, &P.lock, (uint32_t)(end - now) + 1);
        if (wait_ms) perf_end(PERF_PIPE, p0);
        if (wait_ms && !p->building) perf_count(PERF_CNT_PIPE_WAIT);
    }
    int wait = p && p->building && !p->info.pipeline;
    if (wait) P.stats.pending_skips++;
    if (wait && wait_ms >= GIVE_UP_MS && !p->gave_up) p->gave_up = 1, gave_up_report(p, s, &k, wait_ms);
    VkPipeline pipe = p ? p->info.pipeline : VK_NULL_HANDLE;
    plat_mutex_unlock(&P.lock);
    if (wait) {
        perf_count(PERF_CNT_PIPE_SKIP);
        if (pending) *pending = 1;
        return VK_NULL_HANDLE;
    }
    if (!p) return VK_NULL_HANDLE;
    if (info) *info = &p->info;
    return pipe;
}

VkPipeline vk_pipeline_graphics_async(const VkPipeState *s, const VkPipeInfo **info, int *pending, uint32_t wait_ms) {
    return pipe_graphics(s, info, A.on, pending, wait_ms, 0);
}

// As vk_pipeline_graphics_async without waiting, a miss built from the low queue: pipelines no draw needs to be drawn
// (MKW_SPEC's variants, vk_spec.c) do not hold back the ones that skip draws until they exist.
VkPipeline vk_pipeline_graphics_async_low(const VkPipeState *s, const VkPipeInfo **info, int *pending) {
    return pipe_graphics(s, info, A.on, pending, 0, 1);
}

// ---- boot prewarm from the key file (comment at "pipeline keys")

#define PREWARM_STALL_MS 20000u   // vk_pipeline_prewarm: this long without a pipeline finished and the boot goes on

static int id_cmp(const void *x, const void *y) { return memcmp(x, y, 32); }

// Builds queued prebuild jobs until the low queue is empty, beside the pipe_worker pool (same bookkeeping).
static void warm_builder(void *arg) {
    (void)arg;
    plat_mutex_lock(&P.lock);
    while (A.low) {
        Job *j = A.low;
        if ((A.low = j->next)) A.low->prev = NULL;
        else A.low_tail = NULL;
        j->next = j->prev = NULL;
        j->low = 0;                    // not on a pool thread: promote() has nothing to raise
        plat_mutex_unlock(&P.lock);
        VkPipeline pipe = create_graphics(&j->s, &j->k, j->l, j->fs_off);
        plat_mutex_lock(&P.lock);
        if (pipe || !j->p->fast) j->p->info.pipeline = pipe;   // a failed optimised build keeps the fast-linked one
        if (pipe && j->p->fast) P.g.swaps++;
        j->p->building = 0;
        j->p->job = NULL;
        if (pipe) {
            P.stats.graphics++, P.stats.async_built++, K.built++;
        } else {
            P.stats.failures++, K.failed++;
            rt_log("vk_pipeline: graphics pipeline not built: pipeline creation failed");
        }
        K.promoted += j->demanded;
        K.left--;
        if (!--A.inflight) plat_cond_broadcast(&A.idle);
        plat_cond_broadcast(&done_cv);
        free(j);
    }
    plat_mutex_unlock(&P.lock);
    return;
}

void vk_pipeline_prewarm(void) {
    const char *gpu = rt_config()->gpu;
    if (!A.on || (gpu && !strcmp(gpu, "null"))) return;
    if (!cfg_bool("MKW_PIPE_PREBUILD")) {   // prebuild_start logs the off case
        ensure_init();
        return;
    }
    double t0 = now_ms();
    K.warm = 1;
    if (!ensure_init() || !K.file_n) {
        rt_log("vk_pipeline: prewarm: no pipeline keys (%s)", K.path[0] ? K.path : "<none>");
        return;
    }
    const KeyRec *r = (const KeyRec *)K.file;
    size_t n = K.file_n, m = 0;
    uint8_t (*ids)[32] = malloc(2 * n * 32);
    const GfxShader **sh = calloc(2 * n, sizeof *sh);
    if (!ids || !sh) {
        free(ids), free(sh);
        return;
    }
    for (size_t i = 0; i < n; i++) {
        memcpy(ids[m++], r[i].vs, 32);
        if (r[i].has_fs) memcpy(ids[m++], r[i].fs, 32);
    }
    qsort(ids, m, 32, id_cmp);
    size_t u = 0;
    for (size_t i = 0; i < m; i++)
        if (!u || memcmp(ids[u - 1], ids[i], 32)) memcpy(ids[u++], ids[i], 32);
    int cpus = (int)plat_cpu_count();
    gfx_shader_prewarm((const uint8_t(*)[32])ids, u, sh);
    double t1 = now_ms();

    if (P.indep) {   // vertex output unions first: workers may take a job before the queue is complete
        uint64_t pair = 0, uni = 0, pairs = 0;
        for (size_t i = 0; i < n; i++) {
            const uint8_t (*v)[32] = bsearch(r[i].vs, ids, u, 32, id_cmp);
            const uint8_t (*f)[32] = r[i].has_fs ? bsearch(r[i].fs, ids, u, 32, id_cmp) : NULL;
            if (!v || !f || !sh[v - ids] || !sh[f - ids]) continue;
            GfxShaderIo io[32];
            vs_outs(sh[v - ids], sh[f - ids], io);
            pair += (uint64_t)__builtin_popcount(gfx_shader_inputs(sh[f - ids], NULL)), pairs++;
        }
        for (uint32_t j = 0; j < VS_OUTS_CAP; j++) uni += VO.vs[j] ? (uint64_t)__builtin_popcount(VO.mask[j]) : 0;
        rt_log("vk_pipeline: vertex output unions: %u vertex shaders, %.2f locations each; %llu pairs read %.2f each",
               VO.n, VO.n ? (double)uni / VO.n : 0.0, (unsigned long long)pairs, pairs ? (double)pair / pairs : 0.0);
    }
    uint32_t queued = 0, dropped = 0, known = 0;
    plat_mutex_lock(&P.lock);
    K.t0 = t1;
    K.loading = 1;
    for (size_t i = 0; i < n; i++) {
        const uint8_t (*v)[32] = bsearch(r[i].vs, ids, u, 32, id_cmp);
        const uint8_t (*f)[32] = r[i].has_fs ? bsearch(r[i].fs, ids, u, 32, id_cmp) : NULL;
        const GfxShader *vs = v ? sh[v - ids] : NULL, *fs = f ? sh[f - ids] : NULL;
        if (!vs || (r[i].has_fs && !fs)) {
            dropped++;
            continue;
        }
        VkPipeState s = r[i].s;
        // A vertex shader fed integer attributes was drawn as its integer-input variant, which the record names by
        // the base program (gfx_shader.c int_input_variant keeps the program key as its id): built from the base, the
        // pipeline paired float inputs with integer formats, the likely cause of RADV's crash in run49.
        GfxShaderAnswers a = {0};
        GfxShaderAnswers af = {0};
        vk_pipeline_answers(&s, GFX_STAGE_VS, &a);
        vk_pipeline_answers(&s, GFX_STAGE_FS, &af);
        s.vs = gfx_shader_int_variant(vs, &a), s.fs = fs ? gfx_shader_int_variant(fs, &af) : NULL;
        GKey k;
        gkey_fill(&s, &k);
        uint64_t h = key_hash(&k, sizeof k);
        int q = 0;
        if (pipe_find(&k, sizeof k, h)) known++;
        else pipe_miss(&s, &k, h, 2, &q), queued += (uint32_t)q;
    }
    K.loading = 0;
    plat_mutex_unlock(&P.lock);
    free(ids), free(sh);

    PlatThread *t[64];
    int nt = 0;
    for (int i = 0; i < cpus && i < 64 && queued; i++)
        if ((t[nt] = plat_thread_new(warm_builder, NULL, "pipe prewarm"))) nt++;
    if (!nt) warm_builder(NULL);
    // The boot waits while pipelines keep finishing; PREWARM_STALL_MS without one and the game starts, the rest built
    // on in the background (issue #36: AMD's Windows drivers 25.2.1 and 26.10.1 took 264-442 s for one pipeline of
    // 11-19, and the window opened that much later).
    uint32_t last = 0;
    double last_t = now_ms();
    int i = 0, stalled = 0;
    for (; i < nt && !stalled; i++) {
        while (!plat_thread_join_ns(t[i], 2000000000u)) {
            uint32_t done = K.built + K.failed;
            if (done != last) last = done, last_t = now_ms();
            rt_log("vk_pipeline: prewarm: %u of %u pipelines, %.0f ms", done, queued, now_ms() - t1);
            if ((stalled = now_ms() - last_t >= PREWARM_STALL_MS)) break;
        }
    }
    if (stalled)
        for (i--; i < nt; i++) plat_thread_detach(t[i]);
    plat_mutex_lock(&P.lock);
    if (stalled) {
        rt_log("vk_pipeline: prewarm: no pipeline finished in %u ms; the game starts, %u pipelines still building in the "
               "background", PREWARM_STALL_MS, K.left);
    } else {
        while (K.left) plat_cond_wait(&done_cv, &P.lock);   // jobs a pool worker took
    }
    rt_log("vk_pipeline: prewarm: %zu records in %s: %u pipelines built, %u failed, %u already built, %u dropped "
           "(shader not loaded); shaders %.0f ms, pipelines %.0f ms on %d threads, total %.0f ms",
           n, K.path, K.built, K.failed, known, dropped, t1 - t0, now_ms() - t1, nt, now_ms() - t0);
    free(K.file);
    K.file = NULL;
    plat_mutex_unlock(&P.lock);
}

// ensure_init's hook: without a prewarm (MKW_PIPE_PREBUILD=0, or the device came up first elsewhere) the records
// are not built. Caller holds no lock.
void prebuild_start(void) {
    plat_mutex_lock(&P.lock);
    if (K.file && !K.warm) {
        rt_log("vk_pipeline: prebuild off%s; %zu records not built",
               !cfg_bool("MKW_PIPE_PREBUILD") ? " (MKW_PIPE_PREBUILD=0)" : " (no boot prewarm)", K.file_n);
        free(K.file);
        K.file = NULL;
    }
    plat_mutex_unlock(&P.lock);
}

// A compute pipeline for cs in layout l; VK_NULL_HANDLE on failure. No lock needed (P.cache is driver-synchronised).
static VkPipeline compute_create(const GfxShader *cs, const Layout *l) {
    uint64_t words;
    const uint32_t *code = gfx_shader_spirv(cs, &words);
    VkShaderModule m = make_module(code, words);
    if (!m) return VK_NULL_HANDLE;
    VkPipelineShaderStageRequiredSubgroupSizeCreateInfo rqc;
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"},
                                      .layout = l->layout};
    pin_subgroup32(&ci.stage, &rqc, code, words);
    if (!ci.stage.pNext && vk_caps.cs_wide) {   // vk_caps.c cs_wide: Adreno's wave limit for barriers
        rqc = (VkPipelineShaderStageRequiredSubgroupSizeCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO,
            .requiredSubgroupSize = vk_caps.cs_wide};
        ci.stage.pNext = &rqc;
    }
    if (vk_ctx()->has_exec_stats) ci.flags |= VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;
    if (vk_ctx()->has_exec_stats && cfg_get("MKW_SPEC_ISA")) ci.flags |= VK_PIPELINE_CREATE_CAPTURE_INTERNAL_REPRESENTATIONS_BIT_KHR;
    VkPipeline pipe = VK_NULL_HANDLE;
    if (vkCreateComputePipelines(P.device, P.cache, 1, &ci, NULL, &pipe) != VK_SUCCESS) pipe = VK_NULL_HANDLE;
    else if (vk_ctx()->has_exec_stats) log_stats(pipe, &(VkPipeState){.vs = cs});
    if (pipe && vk_ctx()->set_name) {   // validation messages name the program (build/spirv/<key>.glsl)
        const uint8_t *k = gfx_shader_key(cs);
        char name[24];
        snprintf(name, sizeof name, "cs %02x%02x%02x%02x%02x%02x%02x%02x", k[0], k[1], k[2], k[3], k[4], k[5], k[6], k[7]);
        vk_ctx()->set_name(P.device, &(VkDebugUtilsObjectNameInfoEXT){.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
                           .objectType = VK_OBJECT_TYPE_PIPELINE, .objectHandle = (uint64_t)pipe, .pObjectName = name});
    }
    vkDestroyShaderModule(P.device, m, NULL);
    return pipe;
}

// The layout of a compute shader in *B; NULL with *why on failure. Caller holds P.lock.
static Layout *compute_layout(const GfxShader *cs, Bindings *B, const char **why) {
    Layout *l = NULL;
    if (gfx_shader_stage(cs) != GFX_STAGE_CS) *why = "not a compute shader";
    else if (!add_stage(B, cs, 0)) *why = "too many bindings";
    else if (!(l = layout_get(B))) *why = "descriptor layout creation failed";
    return l;
}

VkPipeline vk_pipeline_compute(const GfxShader *cs, const VkPipeInfo **info) {
    if (info) *info = NULL;
    if (!cs || !ensure_init()) return VK_NULL_HANDLE;
    struct { const GfxShader *cs; uint32_t compute; } k = {cs, 1};
    uint64_t h = key_hash(&k, sizeof k);
    plat_mutex_lock(&P.lock);
    Pipe *p = pipe_find(&k, sizeof k, h);
    if (p) {
        P.stats.hits++;
    } else {
        const char *why = NULL;
        Bindings B = {0};
        Layout *l = compute_layout(cs, &B, &why);
        VkPipeline pipe = VK_NULL_HANDLE;
        if (!why) {
            uint64_t p0 = perf_begin();
            pipe = compute_create(cs, l);
            perf_end(PERF_PIPE, p0);
            if (!pipe) why = "pipeline creation failed";
        }
        if (why) {
            P.stats.failures++;
            rt_log("vk_pipeline: compute pipeline not built: %s", why);
        } else {
            P.stats.compute++;
        }
        p = pipe_add(&k, sizeof k, h, pipe, l, l ? &B : NULL, VK_PIPELINE_BIND_POINT_COMPUTE, 0);
    }
    plat_mutex_unlock(&P.lock);
    if (!p) return VK_NULL_HANDLE;
    if (info) *info = &p->info;
    return p->info.pipeline;
}

