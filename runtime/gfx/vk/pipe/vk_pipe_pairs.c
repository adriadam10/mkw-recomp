// Shader libraries of every vertex + fragment program pair of the player's game copy, built at boot (vk_pipeline_prewarm).
//
// pipeline-keys.bin only holds what earlier runs drew: a new place or menu drew with pipelines whose libraries were
// still compiling, and those draws were skipped (#500: 17 616 on a B580 starting with 1 159 keys). The expensive part
// of a pipeline under the graphics pipeline library (vk_pipe_gpl.c) is its two shader libraries, and they depend on
// little besides the shader: pre-rasterization on the vertex shader and the union of its pairs' fragment inputs
// (vs_outs), fragment on the fragment shader, its sample count, alpha to coverage, alpha test function (its reference
// is read from the support buffer) and dual-source rewrite.
// A census of 6 165 recorded pipelines (17/11/2026, tools/shaders/pipeline_keys.py) had 1 sample in all,
// the alpha test in 29 and about one fragment library per fragment shader. So the libraries of every pair the copy's
// programs can form (pipeline-pairs.bin, tools/shaders/pipeline_pairs.py at setup) are built here with the common
// state: draws then only link (vertex input and fragment output libraries hold no shader and are made on the drawing
// thread). Every pair's fragment inputs enter the vertex output unions before any library key is taken, so the
// pre-rasterization key a draw computes later is the one built here.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "gfx.h"
#include "host.h"
#include "platform/platform.h"
#include "vk_pipe_internal.h"

#define PAIRS_VERSION 3
#define PAIR_ALPHA 1u           // PairRec.flags: the fragment stage may draw with NVN's alpha test (pipeline_pairs.py)
#define PAIR_CS 2u              // PairRec.flags: vs holds a compute program, fs is none (pipeline_pairs.py)
#define JOB_CS (-1)             // PairJob.kind of a compute pipeline (vk_pipeline_compute_warm)
#define PAIRS_STALL_MS 20000u   // as PREWARM_STALL_MS: this long without a library finished and the boot goes on

typedef struct { char magic[8]; uint32_t version, count; } PairsHeader;
typedef struct { uint8_t vs[32], fs[32]; uint32_t flags; } PairRec;
typedef struct { int kind; Lib *b; Layout *l; VkPipeState s; GKey k; const GfxShader *cs; uint32_t first; } PairJob;

// finished: every library built, no thread left reading SPIR-V (vk_pipeline_prewarm releases it then)
static struct { PairJob *j; size_t n; volatile int64_t next, done, cs_failed; int no_stall, finished; } PJ;

// The alpha test functions the game draws with (pipeline_pairs.py: GREATER and GEQUAL in every recorded pipeline with
// the test on), as GKey.alpha_func: VkCompareOp + 1. The reference is no part of a pipeline (spirv_alpha_test).
static const uint8_t alpha_funcs[] = {VK_COMPARE_OP_GREATER + 1, VK_COMPARE_OP_GREATER_OR_EQUAL + 1};

static int id_cmp(const void *x, const void *y) { return memcmp(x, y, 32); }

static void pairs_worker(void *arg) {
    plat_thread_background();   // off the frame's fast cores (MKW_FAST_CORES)
    (void)arg;
    gfx_shader_spirv_quiet(1);   // not a draw's use: the SPIR-V may be released after the prewarm
    plat_mutex_lock(&P.lock);
    for (int64_t i; (i = plat_inc64(&PJ.next) - 1) < (int64_t)PJ.n;) {
        PairJob *j = &PJ.j[i];
        if (j->kind == JOB_CS) {
            if (!vk_pipeline_compute_warm(j->cs, j->first)) plat_inc64(&PJ.cs_failed);   // releases P.lock
        } else if (j->b->building && !j->b->job) {
            lib_build(j->b, j->kind, &j->s, &j->k, j->l);   // releases P.lock
        }
        plat_inc64(&PJ.done);
    }
    plat_mutex_unlock(&P.lock);
}

// The state common to the recorded pipelines (census above) with the pair's shaders.
static void pair_state(VkPipeState *s, const GfxShader *vs, const GfxShader *fs) {
    memset(s, 0, sizeof *s);
    s->vs = vs, s->fs = fs;
    s->samples = 1, s->prim = NVN_DRAW_PRIMITIVE_TRIANGLES, s->depth_minus_w = 1;
    s->poly.mode = NVN_POLYGON_MODE_FILL;
}

// Queues the library of kind for key unless one exists. Caller holds P.lock.
static void queue_lib(PairJob *jobs, size_t *n, size_t cap, int kind, const void *key, uint32_t size, Layout *l,
                      const VkPipeState *s, const GKey *k) {
    uint64_t h = key_hash(key, size);
    if (*n == cap || lib_find(h, key, size)) return;
    Lib *b = lib_new(h, key, size);
    if (b) jobs[(*n)++] = (PairJob){.kind = kind, .b = b, .l = l, .s = *s, .k = *k};
}

// The pairs file next to pipeline-keys.bin, NULL when absent or of another format.
static PairRec *pairs_read(size_t *n, char *path, size_t cap) {
    snprintf(path, cap, "%s", K.path);
    char *slash = strrchr(path, '\\'), *fslash = strrchr(path, '/');
    if (fslash > slash) slash = fslash;
    snprintf(slash ? slash + 1 : path, cap - (size_t)(slash ? slash + 1 - path : 0), "pipeline-pairs.bin");
    uint64_t len = 0;
    uint8_t *b = (uint8_t *)plat_file_read(path, &len);
    PairsHeader h;
    if (!b || len < sizeof h) return free(b), NULL;
    memcpy(&h, b, sizeof h);
    if (memcmp(h.magic, "MKW_PAIR", 8) || h.version != PAIRS_VERSION || len != sizeof h + (uint64_t)h.count * sizeof(PairRec)) {
        rt_log("vk_pipeline: %s has another format or version; shader libraries are built as the game draws", path);
        free(b);
        return NULL;
    }
    memmove(b, b + sizeof h, len - sizeof h);
    *n = h.count;
    return (PairRec *)b;
}

// The variant a draw makes of vs when its integer attributes have integer formats (gfx_shader_int_variant), NULL when
// it reads none. In the census (6 165 recorded pipelines, 17/11/2026) every integer attribute format but 9 was an
// unsigned one at 6 or 7, the skinning indices (87 of the 96 draws a race replay still found libraries missing for,
// with every pair built, drew such a variant); the 9 were RGBA16I at 0 in the two programs the translator names in
// int_inputs (sm86/translate/glsl/int_hints.py), taken as signed here.
static const GfxShader *vs_int_variant(const GfxShader *vs) {
    uint32_t read = gfx_shader_inputs(vs, NULL), u = read & 0xc0, si = read & gfx_shader_int_inputs(vs) & ~0xc0u;
    if (!u && !si) return NULL;
    GfxShaderAnswers a = {0};
    for (int l = 0; l < 16; l++)
        a.attr_type[l] = u >> l & 1 ? GFX_TYPE_UINT : si >> l & 1 ? GFX_TYPE_SINT : GFX_TYPE_FLOAT;
    const GfxShader *v = gfx_shader_int_variant(vs, &a);
    return v != vs ? v : NULL;
}

// The variant of fs with integer outputs a draw into integer targets makes, for the outputs the translator found only
// moving flat inputs' bits (raw_outputs); NULL for any other. In the census one fragment program drew into an integer
// target (R8UI), DrivePassShaderDefault's 664, which int_hints names.
static const GfxShader *fs_int_variant(const GfxShader *fs) {
    uint32_t m = gfx_shader_raw_outputs(fs);
    if (!m) return NULL;
    GfxShaderAnswers a = {0};
    for (int l = 0; l < 8; l++)
        if (m >> l & 1) a.out_type[l] = GFX_TYPE_UINT;
    const GfxShader *v = gfx_shader_int_variant(fs, &a);
    return v != fs ? v : NULL;
}

// The fragment library key lib_keys makes, for a record without a vertex stage.
static void fs_key(const GKey *k, const GfxShader *fs, FsKey *fk) {
    memset(fk, 0, sizeof *fk);
    fk->kind = LIB_FS, fk->fs = fs, fk->dsb = k->dsb, fk->samples = k->samples, fk->a2c = k->a2c;
    fk->alpha_func = k->alpha_func;
}

// A record's loaded shaders (an all-zero key: none) and the integer variants of each (var: by index in ids); 0 when a
// named one is missing or of the wrong stage.
static int pair_shaders(const PairRec *r, const uint8_t (*ids)[32], size_t u, const GfxShader **sh,
                        const GfxShader **var, const GfxShader **vs, const GfxShader **fs, const GfxShader **vv,
                        const GfxShader **fv) {
    static const uint8_t zero[32];
    *vs = *fs = *vv = *fv = NULL;
    if (memcmp(r->vs, zero, 32)) {
        const uint8_t (*v)[32] = bsearch(r->vs, ids, u, 32, id_cmp);
        if (!v || !sh[v - ids] || gfx_shader_stage(sh[v - ids]) != GFX_STAGE_VS) return 0;
        // A task or mesh program in the vertex slot draws through a monolithic pipeline (mesh_pipe), never a
        // pre-rasterization library (RADV crashed building one from DrivePassShaderDefault's task program 98); drawn
        // as compute (vk_exec_draw.c), its fragment stage goes with our vertex shader of the vertices its passes wrote.
        if (gfx_shader_mesh_kind(sh[v - ids]) == GFX_MESH_NONE) *vs = sh[v - ids], *vv = var[v - ids];
        else if (vk_mesh_cs_on()) *vs = gfx_shader_mesh_cs_vs();
    }
    if (memcmp(r->fs, zero, 32)) {
        const uint8_t (*f)[32] = bsearch(r->fs, ids, u, 32, id_cmp);
        if (!f || !sh[f - ids] || gfx_shader_stage(sh[f - ids]) != GFX_STAGE_FS) return 0;
        *fs = sh[f - ids], *fv = var[f - ids];
    }
    return *vs || *fs;
}

void pairs_prewarm(void) {
    // without the pipeline library only the compute pipelines: the graphics records' prewarm is all there is for draws
    int libs = P.gpl && P.indep;
    char path[sizeof K.path + 32];
    size_t n = 0;
    PairRec *r = pairs_read(&n, path, sizeof path);
    PJ.finished = 1;   // until threads start: what the shader preload loaded is released after the prewarm
    if (!r || !n) {
        if (!r) rt_log("vk_pipeline: no %s: shader libraries are built as the game draws", path);
        free(r);
        return;
    }
    double t0 = (double)plat_now_ns() / 1e6;
    uint8_t (*ids)[32] = malloc(2 * n * 32);
    const GfxShader **sh = calloc(2 * n, sizeof *sh);
    if (!ids || !sh) {
        free(r), free(ids), free(sh);
        return;
    }
    for (size_t i = 0; i < n; i++) memcpy(ids[2 * i], r[i].vs, 32), memcpy(ids[2 * i + 1], r[i].fs, 32);
    qsort(ids, 2 * n, 32, id_cmp);
    size_t u = 0;
    for (size_t i = 0; i < 2 * n; i++)
        if (!u || memcmp(ids[u - 1], ids[i], 32)) memcpy(ids[u++], ids[i], 32);
    gfx_shader_prewarm((const uint8_t(*)[32])ids, u, sh);
    double t1 = (double)plat_now_ns() / 1e6;

    // Each shader's integer variant, then the unions: a library key taken before the last pair of its
    // vertex shader entered would not be a draw's.
    // At most one library per shader and per variant, and per alpha-tested record a fragment one per function for its
    // shader and its variant: lib_find takes the repeats.
    size_t nalpha = 0, ncs = 0;
    for (size_t i = 0; i < n; i++) nalpha += (r[i].flags & PAIR_ALPHA) != 0, ncs += (r[i].flags & PAIR_CS) != 0;
    // A task or mesh program runs as compute (its .cs.spv, gfx_shader_cs_variant): its pipeline too. cs-warm1 on the
    // Windows PC (18/11/2026): with the compute records alone, the 18 the race drew were still built at their dispatch.
    const GfxShader **mesh_cs = calloc(u, sizeof *mesh_cs);
    size_t nmesh = 0;
    for (size_t i = 0; mesh_cs && i < u; i++) nmesh += (mesh_cs[i] = sh[i] ? gfx_shader_cs_variant(sh[i]) : NULL) != NULL;
    size_t cap = 2 * u + 2 * sizeof alpha_funcs * nalpha + 3 * ncs + nmesh;   // a compute program: its pipeline and two splits
    const GfxShader **var = calloc(u, sizeof *var);
    PairJob *jobs = malloc(cap * sizeof *jobs);
    if (!var || !jobs || !mesh_cs) {
        free(r), free(ids), free(sh), free(var), free(jobs), free(mesh_cs);
        return;
    }
    gfx_shader_spirv_quiet(1);   // the variants, unions and layouts below read SPIR-V as the workers do
    uint32_t used = 0, dropped = 0, variants = 0;
    for (size_t i = 0; i < u; i++) {
        int st = sh[i] ? gfx_shader_stage(sh[i]) : -1;
        var[i] = st == GFX_STAGE_VS ? vs_int_variant(sh[i]) : st == GFX_STAGE_FS ? fs_int_variant(sh[i]) : NULL;
        variants += var[i] != NULL;
    }
    for (size_t i = 0; i < n; i++) {
        const GfxShader *vs, *fs, *vv, *fv;
        if (r[i].flags & PAIR_CS || !libs) continue;
        if (!pair_shaders(&r[i], (const uint8_t(*)[32])ids, u, sh, var, &vs, &fs, &vv, &fv)) {
            dropped++;
            continue;
        }
        GfxShaderIo io[32];
        if (vs) vs_outs(vs, fs, io);
        if (vv) vs_outs(vv, fs, io);
        used++;
    }
    size_t nj = 0;
    plat_mutex_lock(&P.lock);
    uint32_t cs_queued = 0;
    for (size_t i = 0; i < n; i++) {
        const GfxShader *vs, *fs, *vv, *fv;
        if (r[i].flags & PAIR_CS) {   // as a dispatch builds it (vk_exec_ops.c), with first_store_pre's two splits
            const uint8_t (*c)[32] = bsearch(r[i].vs, ids, u, 32, id_cmp);
            const GfxShader *cs = c ? sh[c - ids] : NULL;
            if (c && mesh_cs[c - ids]) {   // a mesh program (pipeline_pairs.py): its compute form, queued below
                used++;
                continue;
            }
            if (!cs || gfx_shader_stage(cs) != GFX_STAGE_CS) {
                dropped++;
                continue;
            }
            for (uint32_t first = 0; first < (gfx_shader_first_store(cs) ? 3u : 1u) && nj < cap; first++)
                jobs[nj++] = (PairJob){.kind = JOB_CS, .cs = cs, .first = first}, cs_queued++;
            used++;
            continue;
        }
        if (!libs) continue;
        if (!pair_shaders(&r[i], (const uint8_t(*)[32])ids, u, sh, var, &vs, &fs, &vv, &fv)) continue;
        // the shaders and their variants; a pre-rasterization key holds no fragment shader, one fragment key no vertex
        // shader: their repeats are found built
        const GfxShader *vlist[2] = {vs, vv}, *flist[2] = {fs, fv};
        for (int xy = 0; xy < 2 * (1 + (fv != NULL)); xy++) {
            int x = xy & 1;
            if (x && !vv) continue;
            VkPipeState s;
            pair_state(&s, vlist[x], flist[xy >> 1]);
            Bindings B = {0}, F = {0};
            Layout *l = (!s.vs || add_stage(&B, s.vs, 0)) && (!s.fs || add_stage(&F, s.fs, 0)) ? layout_pair(&B, &F) : NULL;
            if (!l) {
                dropped++;
                continue;
            }
            GKey k;
            if (s.vs) gkey_fill(&s, &k);
            else memset(&k, 0, sizeof k), k.fs = s.fs, k.samples = 1, k.indep = (uint8_t)P.indep;   // vertex_input needs a vs
            FsKey fk;
            if (s.vs) {
                ViKey vk_;
                PrKey pk;
                FoKey ok;
                lib_keys(&s, &k, &vk_, &pk, &fk, &ok);
                queue_lib(jobs, &nj, cap, LIB_PR, &pk, sizeof pk, l, &s, &k);
            } else {
                fs_key(&k, s.fs, &fk);
            }
            if (x) continue;
            queue_lib(jobs, &nj, cap, LIB_FS, &fk, sizeof fk, l, &s, &k);
            for (size_t a = 0; s.fs && r[i].flags & PAIR_ALPHA && a < sizeof alpha_funcs; a++) {
                GKey ka = k;
                FsKey fa = fk;
                ka.alpha_func = fa.alpha_func = alpha_funcs[a];
                queue_lib(jobs, &nj, cap, LIB_FS, &fa, sizeof fa, l, &s, &ka);
            }
        }
    }
    for (size_t i = 0; i < u; i++)   // a task/mesh draw dispatches its compute form whole (vk_exec_ops.c)
        if (mesh_cs[i] && nj < cap) jobs[nj++] = (PairJob){.kind = JOB_CS, .cs = mesh_cs[i]}, cs_queued++;
    plat_mutex_unlock(&P.lock);
    free(ids), free(sh), free(r), free(var), free(mesh_cs);

    PJ.j = jobs, PJ.n = nj, PJ.next = PJ.done = PJ.cs_failed = 0;
    PlatThread *t[64];
    int nt = 0, cpus = (int)plat_cpu_count();
    const char *pt = cfg_get("MKW_PAIRS_THREADS");   // diagnosis: 1 makes the last MKW_PIPE_TRACE line a crash's library
    if (pt && atoi(pt) > 0) cpus = atoi(pt);
    for (int i = 0; i < cpus && i < 64 && nj; i++)
        if ((t[nt] = plat_thread_new(pairs_worker, NULL, "pairs prewarm"))) nt++;
    if (!nt) pairs_worker(NULL);
    // A driver that takes minutes for one library (issue #36, AMD on Windows) must not hold the start: after
    // PAIRS_STALL_MS without one finished the rest is left to these threads and the game goes on.
    int64_t last = 0;
    double last_t = (double)plat_now_ns() / 1e6;
    int stalled = 0, i = 0;
    for (; i < nt && !stalled; i++)
        while (!plat_thread_join_ns(t[i], 2000000000u)) {
            double now = (double)plat_now_ns() / 1e6;
            if (PJ.done != last) last = PJ.done, last_t = now;
            rt_log("vk_pipeline: pairs prewarm: %lld of %zu shader libraries, %.0f ms", (long long)PJ.done, nj, now - t1);
            if ((stalled = !PJ.no_stall && now - last_t >= PAIRS_STALL_MS)) break;
        }
    gfx_shader_spirv_quiet(0);
    PJ.finished = !stalled;
    if (stalled) {
        for (i--; i < nt; i++) plat_thread_detach(t[i]);
        rt_log("vk_pipeline: pairs prewarm: no library finished in %u ms; the game starts, %lld of %zu still building",
               PAIRS_STALL_MS, (long long)(nj - (size_t)PJ.done), nj);
    } else {
        free(jobs);
    }
    rt_log("vk_pipeline: pairs prewarm: %zu records in %s (%u used, %u dropped), %zu shaders and %u integer "
           "variants; %zu shader libraries and compute pipelines (%u compute, %lld failed) on %d threads; shaders %.0f ms, "
           "libraries %.0f ms",
           n, path, used, dropped, u, variants, nj, cs_queued, (long long)PJ.cs_failed, nt, t1 - t0,
           (double)plat_now_ns() / 1e6 - t1);
}

// After the whole prewarm: the SPIR-V only the pairs prewarm read is freed (gfx_shader_spirv_release), unless its
// threads were left building.
void pairs_release(void) {
    if (PJ.finished) gfx_shader_spirv_release();
    PJ.finished = 0;
}

// mkw --prewarm: the boot's prewarm without the game, for the setup's warm step: every library and recorded pipeline
// built into the driver's caches and pipeline-cache.bin, so the player's first start only loads them. No stall rule:
// nothing waits here but the setup. 0 when the device came up.
int vk_pipeline_prewarm_main(void) {
    PJ.no_stall = 1;
    vk_pipeline_async_enable();
    vk_pipeline_prewarm();
    int ok = P.ready;
    vk_pipeline_shutdown();
    rt_log("vk_pipeline: --prewarm %s", ok ? "done" : "failed: no Vulkan device");
    return ok ? 0 : 1;
}
