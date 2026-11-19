// Graphics pipeline library of the Vulkan replay (vk_caps.gpl), split from vk_pipeline.c: the four stage libraries of a
// pipeline, cached by the part of the key each depends on, and the fast link of a draw's pipeline from them. Shares the
// pipeline state and types of vk_pipe_internal.h; provenance as in vk_pipeline.c.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "vk_pipe_internal.h"
#include "vk_spirv_patch.h"
#include "gfx.h"
#include "host.h"
#include "platform/platform.h"

#define LOG_ONCE(...) do { static volatile int32_t once_; if (!plat_xchg32(&once_, 1)) rt_log(__VA_ARGS__); } while (0)

// MKW_PIPE_TRACE=1 (diagnosis): the shader a library is compiled from, logged before vkCreateGraphicsPipelines. The
// driver's compiler crashes in there now and then (libnvidia-gpucomp, SIGSEGV, twice in the pause menu, 10/11/2026) and
// the log names nothing; the last line before the crash is the shader it died on. stderr is flushed: the process dies
// inside the call, and a redirected stderr would keep the line in its buffer.
static int pipe_trace(void) {
    static int on = -1;
    if (on < 0) { const char *e = cfg_get("MKW_PIPE_TRACE"); on = e && *e && *e != '0'; }
    return on;
}

// ---- graphics pipeline library (vk_caps.gpl)
// A pipeline is linked from four libraries, each cached by the part of the pipeline key it depends on: vertex input
// (topology class, vertex attributes without dynamic vertex input), pre-rasterization (vertex shader, the outputs its
// union adds, static rasterization state), fragment shader (fragment shader, dual-source rewrite, samples) and fragment
// output (targets, blend, samples). With independent layouts each stage's set depends on its shader alone, so the
// pre-rasterization library uses {set 0, null} and the fragment shader library {null, set 1}. The fast link runs on the
// thread of the draw that needs it (no link-time optimisation); the optimised pipeline is the job pipe_miss queued
// (a monolithic build with the same modules), which replaces the fast one in the Pipe when done. Libraries and fast
// pipelines live until vk_pipeline_shutdown.
// Background build signalling, both with P.lock: work_cv wakes the pipeline workers, done_cv wakes threads waiting for a
// pipeline or a stage library to be published.
PlatCond work_cv = PLAT_COND_INIT, done_cv = PLAT_COND_INIT;
#define LIB_BUCKETS 1024
// building: a thread builds it or job holds it queued (pipe not set yet); entries live until libs_destroy
static Lib *libs[LIB_BUCKETS];


uint64_t pipe_qpc(void) { return plat_now_ns(); }
double ms_since(uint64_t t0) { return (double)(pipe_qpc() - t0) / 1e6; }

VkPipelineLayout half_layout(Layout *l, int h) {
    if (l->half[h]) return l->half[h];
    VkDescriptorSetLayout sets[2] = {h ? VK_NULL_HANDLE : l->set, h ? l->set1 : VK_NULL_HANDLE};
    VkPipelineLayoutCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                     .flags = VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT, .setLayoutCount = 2,
                                     .pSetLayouts = sets};
    if (vkCreatePipelineLayout(P.device, &pi, NULL, &l->half[h]) != VK_SUCCESS) l->half[h] = VK_NULL_HANDLE;
    return l->half[h];
}

// Builds one library from the pipeline's state; key is the library's key (PrKey: fin holds the added outputs).
static VkPipeline lib_create(int kind, const VkPipeState *s, const GKey *k, VkPipelineLayout lay, const void *key) {
    static const VkGraphicsPipelineLibraryFlagsEXT part[4] = {
        VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT, VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT,
        VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT, VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT};
    VkGraphicsPipelineLibraryCreateInfoEXT gl = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT, .flags = part[kind]};
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .pNext = &gl};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = P.n_dyn, .pDynamicStates = P.dyn};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri,
                                       .flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR, .pDynamicState = &dy};
    VkShaderModule m = VK_NULL_HANDLE;
    VkPipelineShaderStageCreateInfo st = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .pName = "main"};
    VkPipelineShaderStageRequiredSubgroupSizeCreateInfo rq;
    VkVertexInputAttributeDescription va[32];
    VkVertexInputBindingDescription vb[17];
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    static const VkPrimitiveTopology class_base[4] = {VK_PRIMITIVE_TOPOLOGY_POINT_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
                                                      VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_PRIMITIVE_TOPOLOGY_PATCH_LIST};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = class_base[k->topo_class]};
    VkPipelineViewportDepthClipControlCreateInfoEXT clip = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT,
                                                            .negativeOneToOne = k->minus_w};
    VkPipelineViewportStateCreateInfo vp = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .pNext = &clip};
    VkPipelineRasterizationProvokingVertexStateCreateInfoEXT pvs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_PROVOKING_VERTEX_STATE_CREATE_INFO_EXT,
        .provokingVertexMode = VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .pNext = P.pv_last ? &pvs : NULL, .depthClampEnable = k->clamp,
                                                 .polygonMode = (VkPolygonMode)k->poly_mode, .lineWidth = 1.0f};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = (VkSampleCountFlagBits)k->samples,
                                               .alphaToCoverageEnable = k->a2c};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
                                                .maxDepthBounds = 1.0f};
    VkPipelineColorBlendAttachmentState cba[8];
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    uint32_t *patched = NULL;
    VkPipeline pipe = VK_NULL_HANDLE;
    if (kind == LIB_VI) {
        if (!P.dyn_vi) {
            VtxInput v;
            vertex_input(s, &v);
            for (uint32_t i = 0; i < v.n_attr; i++)
                va[i] = (VkVertexInputAttributeDescription){v.a[i].location, v.a[i].binding, v.a[i].format, v.a[i].offset};
            for (uint32_t i = 0; i < v.n_bind; i++)
                vb[i] = (VkVertexInputBindingDescription){v.b[i].binding, v.b[i].stride, v.b[i].inputRate};
            vi.vertexAttributeDescriptionCount = v.n_attr, vi.pVertexAttributeDescriptions = va;
            vi.vertexBindingDescriptionCount = v.n_bind, vi.pVertexBindingDescriptions = vb;
        }
        pi.pVertexInputState = &vi, pi.pInputAssemblyState = &ia;
    } else if (kind == LIB_PR) {
        const PrKey *pk = key;
        pipe_test_stall(s->vs, NULL);
        uint64_t vw, sw, n2 = 0;
        const uint32_t *vcode = gfx_shader_spirv(s->vs, &vw), *scode = vcode;
        sw = vw;
        if (pk->missing && (patched = spirv_add_outputs(vcode, vw, pk->missing, pk->fin, &n2))) vcode = patched, vw = n2;
        else if (pk->missing) LOG_ONCE("vk_pipeline: cannot add missing vertex outputs (mask %#x)", pk->missing);
        if (!(m = make_module(vcode, vw))) goto out;
        st.stage = VK_SHADER_STAGE_VERTEX_BIT, st.module = m;
        pin_subgroup32(&st, &rq, scode, sw);
        pi.stageCount = 1, pi.pStages = &st, pi.pViewportState = &vp, pi.pRasterizationState = &rs;
        pi.layout = lay;
    } else if (kind == LIB_FS) {
        pipe_test_stall(NULL, s->fs);
        if (s->fs) {
            uint64_t fw, sw, n2 = 0;
            const uint32_t *fcode = gfx_shader_spirv(s->fs, &fw), *scode = fcode;
            sw = fw;
            uint32_t *d = NULL;
            if (k->dsb && fcode) {
                if ((d = spirv_dual_source(fcode, fw, &n2))) fcode = d, fw = n2;
                else LOG_ONCE("vk_pipeline: SRC1 blend factors but the fragment shader writes no colour output 1");
            }
            uint32_t *at = k->alpha_func && fcode ? spirv_alpha_test(fcode, fw, k->alpha_func - 1, &n2) : NULL;
            if (at) fcode = at, fw = n2;
            patched = fcode ? spirv_set(fcode, fw, 1) : NULL;
            free(d), free(at);
            if (!patched || !(m = make_module(patched, fw))) goto out;
            st.stage = VK_SHADER_STAGE_FRAGMENT_BIT, st.module = m;
            pin_subgroup32(&st, &rq, scode, sw);
            pi.stageCount = 1, pi.pStages = &st;
        }
        pi.pMultisampleState = &ms, pi.pDepthStencilState = &ds;
        pi.layout = lay;
    } else {
        memset(cba, 0, sizeof cba);
        for (int i = 0; i < k->n_color; i++) {
            const uint8_t *b = k->blend[i];
            cba[i].blendEnable = k->blend_en >> i & 1;
            cba[i].srcColorBlendFactor = (VkBlendFactor)b[0], cba[i].dstColorBlendFactor = (VkBlendFactor)b[1];
            cba[i].colorBlendOp = (VkBlendOp)b[2], cba[i].srcAlphaBlendFactor = (VkBlendFactor)b[3];
            cba[i].dstAlphaBlendFactor = (VkBlendFactor)b[4], cba[i].alphaBlendOp = (VkBlendOp)b[5];
            cba[i].colorWriteMask = k->wmask >> (4 * i) & 0xf;
        }
        cb.logicOpEnable = k->logic_op != 0, cb.logicOp = k->logic_op ? (VkLogicOp)(k->logic_op - 1) : VK_LOGIC_OP_COPY;
        cb.attachmentCount = k->n_color, cb.pAttachments = cba;
        ri.colorAttachmentCount = k->n_color, ri.pColorAttachmentFormats = (const VkFormat *)k->color;
        ri.depthAttachmentFormat = (VkFormat)k->depth, ri.stencilAttachmentFormat = (VkFormat)k->stencil;
        pi.pColorBlendState = &cb, pi.pMultisampleState = &ms;
    }
    if (pipe_trace() && (kind == LIB_PR || kind == LIB_FS)) {
        const GfxShader *sh = kind == LIB_PR ? s->vs : s->fs;
        const uint8_t *sk = sh ? gfx_shader_key(sh) : NULL;
        char hex[65] = "";
        for (int i = 0; sk && i < 32; i++) sprintf(hex + 2 * i, "%02x", sk[i]);
        rt_log("vk_pipeline: compiling %s %s thread %u", kind == LIB_PR ? "vs" : "fs", sk ? hex : "(none)", plat_thread_id());
        fflush(stderr);
    }
    if (vkCreateGraphicsPipelines(P.device, P.cache, 1, &pi, NULL, &pipe) != VK_SUCCESS) pipe = VK_NULL_HANDLE;
out:
    if (m) vkDestroyShaderModule(P.device, m, NULL);
    free(patched);
    return pipe;
}

// The cached library for key. Caller holds P.lock. build 0: lookup only (VK_NULL_HANDLE and *miss when absent);
// build 1: a missing one is built with P.lock released (a failed build is cached too).
// The library entry for key, NULL when absent. Caller holds P.lock.
Lib *lib_find(uint64_t h, const void *key, uint32_t size) {
    for (Lib *b = libs[h % LIB_BUCKETS]; b; b = b->next)
        if (b->hash == h && b->size == size && !memcmp(b->key, key, size)) return b;
    return NULL;
}

// Enters a library built with P.lock released; another thread's entry made meanwhile wins and pipe is destroyed.
// Returns the entered handle. Caller holds P.lock.
static VkPipeline lib_put(uint64_t h, const void *key, uint32_t size, VkPipeline pipe) {
    Lib *b = lib_find(h, key, size);
    if (b && b->building) {   // another thread's build publishes the entry
        if (pipe) vkDestroyPipeline(P.device, pipe, NULL);
        return VK_NULL_HANDLE;
    }
    if (b) {
        if (pipe) vkDestroyPipeline(P.device, pipe, NULL);
        return b->pipe;
    }
    if (!(b = calloc(1, sizeof *b + size))) return pipe;
    b->hash = h, b->size = size, b->pipe = pipe;
    memcpy(b->key, key, size);
    b->next = libs[h % LIB_BUCKETS];
    libs[h % LIB_BUCKETS] = b;
    return pipe;
}

// Shader libraries a waiting draw needs (lib_wait): queued here ahead of every pipeline job, so an idle worker builds
// each at once and the vertex and fragment libraries of one draw build in parallel. A job no worker took is built by
// the first thread that needs its library (lib_get, lib_wait). Under P.lock.
LibQueue U;

// A building entry for key, NULL when out of memory. Caller holds P.lock and found none.
Lib *lib_new(uint64_t h, const void *key, uint32_t size) {
    Lib *b = calloc(1, sizeof *b + size);
    if (!b) return NULL;
    b->hash = h, b->size = size, b->building = 1;
    memcpy(b->key, key, size);
    b->next = libs[h % LIB_BUCKETS];
    libs[h % LIB_BUCKETS] = b;
    return b;
}

// Builds b (building, not queued) with P.lock released and publishes it; a failed build is entered as VK_NULL_HANDLE.
VkPipeline lib_build(Lib *b, int kind, const VkPipeState *s, const GKey *k, Layout *l) {
    VkPipelineLayout lay = kind == LIB_PR ? half_layout(l, 0) : kind == LIB_FS ? half_layout(l, 1) : VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE;
    if (lay || kind == LIB_VI || kind == LIB_FO) {
        plat_mutex_unlock(&P.lock);
        uint64_t t0 = pipe_qpc();
        pipe = lib_create(kind, s, k, lay, b->key);
        double ms = ms_since(t0);
        plat_mutex_lock(&P.lock);
        P.g.lib_n[kind]++, P.g.lib_ms[kind] += ms;
        if (ms > P.g.lib_max[kind]) P.g.lib_max[kind] = ms;
    }
    if (!pipe && ++P.g.lib_fail <= 8) rt_log("vk_pipeline: gpl: stage library %d not built", kind);
    b->pipe = pipe, b->building = 0;
    plat_cond_broadcast(&done_cv);
    return pipe;
}

// Takes b's queued job out of the queue: b stays building, for whoever builds the job. Caller holds P.lock.
LibJob *lib_take(Lib *b) {
    LibJob *j = b->job, **pp = &U.head, *prev = NULL;
    while (*pp && *pp != j) prev = *pp, pp = &(*pp)->next;
    if (*pp) *pp = j->next;
    if (U.tail == j) U.tail = prev;
    b->job = NULL;
    return j;
}

// Takes b's queued job out of the queue and builds it on this thread. Caller holds P.lock.
void lib_steal(Lib *b) {
    LibJob *j = lib_take(b);
    lib_build(b, j->kind, &j->s, &j->k, j->l);
    free(j);
}

static VkPipeline lib_get(int kind, const VkPipeState *s, const GKey *k, Layout *l, const void *key, uint32_t size,
                          int build, int *miss) {
    uint64_t h = key_hash(key, size);
    Lib *have = lib_find(h, key, size);
    while (have && have->building) {   // being built (wait) or queued (build it here)
        if (!build) {
            *miss = 1;
            return VK_NULL_HANDLE;
        }
        if (have->job) U.stolen++, lib_steal(have);
        else plat_cond_wait(&done_cv, &P.lock);
    }
    if (have) return have->pipe;
    VkPipelineLayout lay = kind == LIB_PR ? half_layout(l, 0) : kind == LIB_FS ? half_layout(l, 1) : VK_NULL_HANDLE;
    if (!build || ((kind == LIB_PR || kind == LIB_FS) && !lay)) {
        *miss = 1;
        return VK_NULL_HANDLE;
    }
    Lib *b = lib_new(h, key, size);   // entered first, so other threads wait for this build instead of repeating it
    if (b) return lib_build(b, kind, s, k, l);
    plat_mutex_unlock(&P.lock);
    uint64_t t0 = pipe_qpc();
    VkPipeline pipe = lib_create(kind, s, k, lay, key);
    double ms = ms_since(t0);
    plat_mutex_lock(&P.lock);
    if ((have = lib_find(h, key, size)) && !have->building) {   // another thread built it meanwhile
        if (pipe) vkDestroyPipeline(P.device, pipe, NULL);
        return have->pipe;
    }
    P.g.lib_n[kind]++, P.g.lib_ms[kind] += ms;
    if (ms > P.g.lib_max[kind]) P.g.lib_max[kind] = ms;
    if (!pipe && ++P.g.lib_fail <= 8) rt_log("vk_pipeline: gpl: stage library %d not built", kind);
    return lib_put(h, key, size, pipe);
}

void libs_destroy(void) {
    for (int i = 0; i < LIB_BUCKETS; i++) {
        for (Lib *b = libs[i], *n; b; b = n) {
            n = b->next;
            if (b->pipe) vkDestroyPipeline(P.device, b->pipe, NULL);
            free(b);
        }
        libs[i] = NULL;
    }
}

// The four library keys of pipeline s (key k).
void lib_keys(const VkPipeState *s, const GKey *k, ViKey *v, PrKey *pk, FsKey *fk, FoKey *ok) {
    memset(v, 0, sizeof *v);
    v->kind = LIB_VI, v->topo_class = k->topo_class;
    memcpy(v->attr_fmt, k->attr_fmt, sizeof v->attr_fmt), memcpy(v->attr_off, k->attr_off, sizeof v->attr_off);
    memcpy(v->attr_bind, k->attr_bind, sizeof v->attr_bind), memcpy(v->rate, k->rate, sizeof v->rate);
    memset(pk, 0, sizeof *pk);
    GfxShaderIo fin[32];
    pk->kind = LIB_PR, pk->vs = s->vs, pk->clamp = k->clamp, pk->poly_mode = k->poly_mode, pk->minus_w = k->minus_w;
    pk->missing = vs_union_missing(s, fin);
    for (int i = 0; i < 32; i++)
        if (pk->missing >> i & 1) pk->fin[i] = fin[i];
    memset(fk, 0, sizeof *fk);
    fk->kind = LIB_FS, fk->fs = s->fs, fk->dsb = k->dsb, fk->samples = k->samples, fk->a2c = k->a2c;
    fk->alpha_func = k->alpha_func;
    memset(ok, 0, sizeof *ok);
    ok->kind = LIB_FO, ok->n_color = k->n_color, ok->samples = k->samples, ok->a2c = k->a2c, ok->logic_op = k->logic_op;
    ok->blend_en = k->blend_en, ok->depth = k->depth, ok->stencil = k->stencil, ok->wmask = k->wmask;
    memcpy(ok->color, k->color, sizeof ok->color), memcpy(ok->blend, k->blend, sizeof ok->blend);
}

// MKW_PIPE_MISS=1 (diagnosis): each draw that finds a shader library missing, with the key fields that tell why the
// boot prewarm (vk_pipe_pairs.c) did not build it: which library, the shader keys, whether the vertex shader is an
// integer-input variant (its key differs from its program key), the added outputs and the fragment state.
static void miss_trace(const VkPipeState *s, const PrKey *pk, const FsKey *fk) {
    static int on = -1;
    if (on < 0) { const char *e = cfg_get("MKW_PIPE_MISS"); on = e && *e && *e != '0'; }
    if (!on) return;
    int pr = !lib_find(key_hash(pk, sizeof *pk), pk, sizeof *pk), fs = !lib_find(key_hash(fk, sizeof *fk), fk, sizeof *fk);
    char v[65] = "", f[65] = "";
    for (int i = 0; i < 32; i++) {
        sprintf(v + 2 * i, "%02x", gfx_shader_input_id(s->vs)[i]);
        if (s->fs) sprintf(f + 2 * i, "%02x", gfx_shader_input_id(s->fs)[i]);
    }
    rt_log("vk_pipeline: miss %s%s vs %s%s fs %s%s missing %#x clamp %u poly %u minus_w %u dsb %u a2c %u alpha %u",
           pr ? "pr" : "", fs ? "fs" : "", v, memcmp(gfx_shader_key(s->vs), gfx_shader_input_id(s->vs), 32) ? " (variant)" : "",
           f, s->fs && memcmp(gfx_shader_key(s->fs), gfx_shader_input_id(s->fs), 32) ? " (variant)" : "", pk->missing,
           pk->clamp, pk->poly_mode, pk->minus_w, fk->dsb, fk->a2c, fk->alpha_func);
}

// A fast-linked pipeline for s from its stage libraries. Caller holds P.lock.
// build 0 (drawing thread): links only when the shader libraries exist (vertex input and fragment output libraries,
// which hold no shader, are built here); *final 0 = not yet, a worker builds them. build 1 (worker): builds the
// missing libraries and links with P.lock released. VK_NULL_HANDLE with *final 1 when GPL cannot express the pipeline
// (shared layout, feedback loop) or a build failed.
VkPipeline gpl_link(const VkPipeState *s, const GKey *k, const Layout *lc, int build, int *final) {
    Layout *l = (Layout *)lc;
    *final = 1;
    if (!l->set1 || k->fbl) return VK_NULL_HANDLE;
    uint64_t t0 = pipe_qpc();
    ViKey vk_;
    PrKey pk;
    FsKey fk;
    FoKey ok;
    lib_keys(s, k, &vk_, &pk, &fk, &ok);
    int miss = 0;
    VkPipeline lib[4];
    lib[1] = lib_get(LIB_PR, s, k, l, &pk, sizeof pk, build, &miss);   // shader libraries first: no work when absent
    lib[2] = miss ? VK_NULL_HANDLE : lib_get(LIB_FS, s, k, l, &fk, sizeof fk, build, &miss);
    if (miss) {
        if (!build) *final = 0, P.g.defer++, miss_trace(s, &pk, &fk);
        return VK_NULL_HANDLE;
    }
    lib[0] = lib_get(LIB_VI, s, k, l, &vk_, sizeof vk_, 1, &miss);
    lib[3] = lib_get(LIB_FO, s, k, l, &ok, sizeof ok, 1, &miss);
    VkPipeline pipe = VK_NULL_HANDLE;
    if (lib[0] && lib[1] && lib[2] && lib[3]) {
        VkPipelineLibraryCreateInfoKHR li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR, .libraryCount = 4,
                                             .pLibraries = lib};
        VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &li,
                                           .layout = l->layout};
        if (build) plat_mutex_unlock(&P.lock);
        uint64_t t1 = pipe_qpc();
        if (vkCreateGraphicsPipelines(P.device, VK_NULL_HANDLE, 1, &pi, NULL, &pipe) != VK_SUCCESS) pipe = VK_NULL_HANDLE;
        double ms = ms_since(t1);   // the link alone: library builds are counted apart
        if (build) plat_mutex_lock(&P.lock);
        P.g.link_ms += ms;
        if (ms > P.g.link_max) P.g.link_max = ms;
    }
    if (pipe) P.g.links++;
    else if (!P.g.link_fail++) rt_log("vk_pipeline: gpl: fast link failed; such draws wait or skip as without GPL");
    double total = ms_since(t0);
    if (!build && total > 5.0 && P.g.slow++ < 16)
        rt_log("vk_pipeline: gpl: fast link on the drawing thread took %.1f ms", total);
    return pipe;
}
