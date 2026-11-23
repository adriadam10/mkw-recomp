// Descriptor layouts of the Vulkan replay's pipelines, split from vk_pipeline.c (binding model in its header comment):
// the bindings of each stage, set layouts and their update templates, layouts shared by every pipeline with the same
// binding list, and the vertex outputs a fragment shader needs. Shares vk_pipe_internal.h; provenance as in vk_pipeline.c.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "vk_pipe_internal.h"
#include "vk_spirv_patch.h"
#include "gfx.h"
#include "host.h"
#include "nvn_hmap.h"
#include "platform/platform.h"

#define LOG_ONCE(...) do { static volatile int32_t once_; if (!plat_xchg32(&once_, 1)) rt_log(__VA_ARGS__); } while (0)

Layout *layouts[LAYOUT_BUCKETS];

// ---- descriptor layouts


static VkDescriptorType descriptor_type(int kind, const GfxShaderRes *r) {
    int buffer = (r->type & 0xff) == 2;
    switch (kind) {
    // flags 2: the bank's word view for dynamic addresses, a read-only storage buffer bound to the uniform range so the
    // shader bounds its reads by .length() (head.py cbNw; issue #22's NVIDIA device loss).
    case GFX_RES_UBO: return r->flags & 2 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    case GFX_RES_SSBO: return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    case GFX_RES_TEX:
        if (buffer) return VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
        if (r->separate) LOG_ONCE("vk_pipeline: a separate texture handle: bound as SAMPLED_IMAGE");
        return r->separate ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    default: return buffer ? VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    }
}

static VkShaderStageFlags stage_bit(int stage) {
    return stage == GFX_STAGE_VS ? VK_SHADER_STAGE_VERTEX_BIT : stage == GFX_STAGE_FS ? VK_SHADER_STAGE_FRAGMENT_BIT
                                                                                      : VK_SHADER_STAGE_COMPUTE_BIT;
}

// One past the highest binding a shader uses (1 with no resources: binding 0 is the support buffer).
uint32_t binding_span(const GfxShader *s) {
    uint32_t span = 1;
    for (int k = 0; k < GFX_RES_KINDS; k++) {
        const GfxShaderRes *r;
        int n = gfx_shader_resources(s, k, &r);
        for (int i = 0; i < n; i++)
            if (r[i].binding + (uint32_t)(r[i].count ? r[i].count : 1) > span) span = r[i].binding + (r[i].count ? r[i].count : 1);
    }
    return span;
}

// Adds a stage's resources with bindings b >= 1 moved by off; binding 0 (support buffer) is shared.
int add_stage(Bindings *B, const GfxShader *s, uint32_t off) {
    int stage = gfx_shader_stage(s), mk = gfx_shader_mesh_kind(s);
    VkShaderStageFlags bit = gfx_shader_cs_of_mesh(s) ? VK_SHADER_STAGE_COMPUTE_BIT   // its resources keep the task/mesh tag
                           : mk == GFX_MESH_TASK ? VK_SHADER_STAGE_TASK_BIT_EXT
                           : mk == GFX_MESH_MESH ? VK_SHADER_STAGE_MESH_BIT_EXT : stage_bit(stage);
    if (gfx_shader_cs_of_mesh(s)) B->pc_stages |= VK_SHADER_STAGE_COMPUTE_BIT;
    if (!B->n) {
        B->b[0] = (VkDescriptorSetLayoutBinding){0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, 0, NULL};
        B->pb[0] = (VkPipeBinding){0, (uint8_t)stage, VK_PIPE_SUPPORT, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, 0, NULL};
        B->n = 1;
    }
    B->b[0].stageFlags |= bit;
    B->pb[0].stages |= bit;
    for (int k = 0; k < GFX_RES_KINDS; k++) {
        const GfxShaderRes *r;
        int n = gfx_shader_resources(s, k, &r);
        for (int i = 0; i < n; i++) {
            if (B->n == MAX_BINDINGS || !r[i].binding) return 0;
            VkDescriptorType t = descriptor_type(k, &r[i]);
            uint32_t binding = r[i].binding + off, count = r[i].count ? r[i].count : 1;
            B->b[B->n] = (VkDescriptorSetLayoutBinding){binding, t, count, bit, NULL};
            uint8_t tag = mk == GFX_MESH_TASK ? VK_PIPE_STAGE_NVN | 6 : mk == GFX_MESH_MESH ? VK_PIPE_STAGE_NVN | 7 : (uint8_t)stage;
            B->pb[B->n] = (VkPipeBinding){(uint16_t)binding, tag, (uint8_t)k, t, (uint16_t)count, bit, &r[i]};
            B->n++;
        }
    }
    return 1;
}

// A binding a module's resource variables use that the binding list b[0..n) lacks (module bindings >= 1 moved by off),
// or -1. A reflection that misses a resource would otherwise hand the driver an invalid layout. From the bindings the
// reflection kept (gfx_shader_bindings): a pipeline miss checks it on the drawing thread, and the SPIR-V may be
// released; the module is read only when a binding is 64 or more.
static int module_binding_missing(const GfxShader *s, const VkDescriptorSetLayoutBinding *b, uint32_t n, uint32_t off) {
    uint64_t mask;
    if (gfx_shader_bindings(s, &mask)) {
        for (; mask; mask &= mask - 1) {
            uint32_t raw = (uint32_t)__builtin_ctzll(mask), want = raw + (raw >= 1 ? off : 0), j = 0;
            while (j < n && b[j].binding != want) j++;
            if (j == n) return (int)raw;
        }
        return -1;
    }
    uint64_t words = 0;
    const uint32_t *w = gfx_shader_spirv(s, &words);
    for (uint64_t i = 5; w && i < words;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > words) break;
        if (op == SPV_OP_DECORATE && len >= 4 && w[i + 2] == SPV_DEC_BINDING) {
            uint32_t want = w[i + 3] + (w[i + 3] >= 1 ? off : 0), j = 0;
            while (j < n && b[j].binding != want) j++;
            if (j == n) return (int)w[i + 3];
        }
        i += len;
    }
    return -1;
}

// Checks both stages of a graphics pipeline against its layout; 0 with why filled when one uses a missing binding.
int layout_covers(const VkPipeState *s, const Layout *l, uint32_t fs_off, char *why, size_t cap) {
    uint32_t nv = l->set1 ? l->n0 : l->n;
    int m = module_binding_missing(s->vs, l->b, nv, 0);
    const char *stage = "vertex";
    if (m < 0 && s->fs) {
        m = l->set1 ? module_binding_missing(s->fs, l->b + l->n0, l->n - l->n0, 0) : module_binding_missing(s->fs, l->b, l->n, fs_off);
        stage = "fragment";
    }
    if (m >= 0) snprintf(why, cap, "the %s shader uses binding %d, which its reflection and layout lack", stage, m);
    return m < 0;
}

// The update template of a set whose bindings are b[0..n): one entry per binding, in order, over the packed array the
// replay fills from the set's first descriptor, a VkDescSlot per descriptor. The bind point follows the stages (compute
// layouts hold the compute stage only). VK_NULL_HANDLE when templates are off, for an empty set, for a push set
// without vkCmdPushDescriptorSetWithTemplateKHR or when creation fails: the replay then writes the set with write
// structs.
static VkDescriptorUpdateTemplate make_tpl(const VkDescriptorSetLayoutBinding *b, uint32_t n, VkDescriptorSetLayout sl,
                                           int push, VkPipelineLayout pl, uint32_t set) {
    if (!P.tpl || !n || (push && !P.tpl_push)) return VK_NULL_HANDLE;
    VkDescriptorUpdateTemplateEntry e[MAX_BINDINGS];
    size_t off = 0;
    VkShaderStageFlags stages = 0;
    for (uint32_t i = 0; i < n; i++) {
        e[i] = (VkDescriptorUpdateTemplateEntry){b[i].binding, 0, b[i].descriptorCount, b[i].descriptorType, off, sizeof(VkDescSlot)};
        off += sizeof(VkDescSlot) * b[i].descriptorCount;
        stages |= b[i].stageFlags;
    }
    VkDescriptorUpdateTemplateCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO, .descriptorUpdateEntryCount = n,
        .pDescriptorUpdateEntries = e,
        .templateType = push ? VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_PUSH_DESCRIPTORS_KHR : VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET,
        .descriptorSetLayout = sl,
        .pipelineBindPoint = stages & VK_SHADER_STAGE_COMPUTE_BIT ? VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS,
        .pipelineLayout = pl, .set = set};
    VkDescriptorUpdateTemplate t;
    if (P.tpl_create(P.device, &ci, NULL, &t) != VK_SUCCESS) {
        LOG_ONCE("vk_pipeline: descriptor update template creation failed: that set is written with write structs");
        return VK_NULL_HANDLE;
    }
    return t;
}

// Finds or creates the layout of a binding list (sorted here); NULL on failure. Caller holds P.lock.
// Sorts a binding list by binding and hashes it into *h; the descriptor total, or -1 when a binding repeats.
static int64_t bindings_sort(Bindings *B, uint64_t *h) {
    for (uint32_t i = 1; i < B->n; i++)                     // insertion sort by binding; the lists are short
        for (uint32_t j = i; j > 0 && B->b[j - 1].binding > B->b[j].binding; j--) {
            VkDescriptorSetLayoutBinding t = B->b[j]; B->b[j] = B->b[j - 1]; B->b[j - 1] = t;
            VkPipeBinding u = B->pb[j]; B->pb[j] = B->pb[j - 1]; B->pb[j - 1] = u;
        }
    int64_t total = 0;
    for (uint32_t i = 0; i < B->n; i++) {
        if (i && B->b[i].binding == B->b[i - 1].binding) return -1;
        uint32_t k[4] = {B->b[i].binding, B->b[i].descriptorType, B->b[i].descriptorCount, B->b[i].stageFlags};
        *h = pipe_fnv(*h, k, sizeof k);
        total += B->b[i].descriptorCount;
    }
    return total;
}

// MKW_DRAW_CACHE: a layout's uniform buffers become dynamic when the pipeline layout holds no more than the device
// allows (n_other: those of the layout's other set); over it the layout keeps them static and its sets miss the cache.
static void dyn_ubos(Bindings *B, uint32_t n_other) {
    if (!P.draw_cache) return;
    uint32_t n = n_other;
    for (uint32_t i = 0; i < B->n; i++)
        if (B->b[i].descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) n += B->b[i].descriptorCount;
    if (n > P.max_dyn_ubo) {
        P.stats.dyn_over++;
        return;
    }
    for (uint32_t i = 0; i < B->n; i++)
        if (B->b[i].descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER)
            B->b[i].descriptorType = B->pb[i].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
}

static uint32_t ubo_count(const Bindings *B) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < B->n; i++)
        if (B->b[i].descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) n += B->b[i].descriptorCount;
    return n;
}

Layout *layout_get(Bindings *B) {
    dyn_ubos(B, 0);
    uint64_t h = FNV0;
    int64_t total = bindings_sort(B, &h);
    if (total < 0) return NULL;
    Layout **head = &layouts[h % LAYOUT_BUCKETS];
    for (Layout *l = *head; l; l = l->next)
        if (l->hash == h && l->n == B->n && l->pc_stages == B->pc_stages && !memcmp(l->b, B->b, B->n * sizeof *B->b))
            return l;
    Layout *l = calloc(1, sizeof *l + B->n * sizeof *B->b);
    if (!l) return NULL;
    l->hash = h, l->n = B->n, l->pc_stages = B->pc_stages;
    memcpy(l->b, B->b, B->n * sizeof *B->b);
    l->push = !P.draw_cache && total <= (int64_t)P.max_push;
    VkDescriptorSetLayoutCreateInfo si = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                          .flags = l->push ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR : 0,
                                          .bindingCount = l->n, .pBindings = l->b};
    if (vkCreateDescriptorSetLayout(P.device, &si, NULL, &l->set) != VK_SUCCESS) {
        free(l);
        return NULL;
    }
    VkPushConstantRange pc = {B->pc_stages, 0, MESH_PC_BYTES};
    VkPipelineLayoutCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                     .pSetLayouts = &l->set, .pushConstantRangeCount = B->pc_stages ? 1 : 0,
                                     .pPushConstantRanges = &pc};
    if (vkCreatePipelineLayout(P.device, &pi, NULL, &l->layout) != VK_SUCCESS) {
        vkDestroyDescriptorSetLayout(P.device, l->set, NULL);
        free(l);
        return NULL;
    }
    l->tpl[0] = make_tpl(l->b, l->n, l->set, l->push, l->layout, 0);
    l->next = *head;
    *head = l;
    P.stats.layouts++;
    P.stats.push_layouts += (uint64_t)l->push;
    return l;
}

// Independent layouts (P.indep): the vertex stage's bindings in set 0, the fragment stage's in set 1 (empty without a
// fragment shader), each with its own support buffer at binding 0, the pipeline layout created with
// VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT so stage libraries can use it. Only the P.push_stage set is a
// push-descriptor set (a layout holds one), when it fits maxPushDescriptors; the other is allocated per draw from the
// command buffer's pools. On success *B holds both lists, set 0 first, VkPipeBinding.set filled in. Caller holds P.lock.
Layout *layout_pair(Bindings *B, Bindings *F) {
    uint32_t nb = ubo_count(B), nf = ubo_count(F);
    dyn_ubos(B, nf), dyn_ubos(F, nb);
    uint64_t h = pipe_fnv(FNV0, "indep", 5);
    int64_t tv = bindings_sort(B, &h);
    h = pipe_fnv(h, "|", 1);
    int64_t tf = bindings_sort(F, &h);
    if (tv < 0 || tf < 0 || B->n + F->n > MAX_BINDINGS) return NULL;
    uint32_t n0 = B->n, n = B->n + F->n;
    memcpy(B->b + n0, F->b, F->n * sizeof *F->b);
    memcpy(B->pb + n0, F->pb, F->n * sizeof *F->pb);
    for (uint32_t i = n0; i < n; i++) B->pb[i].set = 1;
    B->n = n;
    Layout **head = &layouts[h % LAYOUT_BUCKETS];
    for (Layout *l = *head; l; l = l->next)
        if (l->hash == h && l->set1 && l->n == n && l->n0 == n0 && !memcmp(l->b, B->b, n * sizeof *B->b)) return l;
    Layout *l = calloc(1, sizeof *l + n * sizeof *B->b);
    if (!l) return NULL;
    l->hash = h, l->n = n, l->n0 = n0;
    memcpy(l->b, B->b, n * sizeof *B->b);
    l->push = !P.draw_cache && P.push_stage == GFX_STAGE_VS && tv <= (int64_t)P.max_push;
    l->push1 = !P.draw_cache && P.push_stage == GFX_STAGE_FS && F->n && tf <= (int64_t)P.max_push;
    VkDescriptorSetLayoutCreateInfo s0 = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                          .flags = l->push ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR : 0,
                                          .bindingCount = n0, .pBindings = l->b};
    VkDescriptorSetLayoutCreateInfo s1 = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                          .flags = l->push1 ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR : 0,
                                          .bindingCount = n - n0, .pBindings = l->b + n0};
    if (vkCreateDescriptorSetLayout(P.device, &s0, NULL, &l->set) != VK_SUCCESS) {
        free(l);
        return NULL;
    }
    if (vkCreateDescriptorSetLayout(P.device, &s1, NULL, &l->set1) != VK_SUCCESS) {
        vkDestroyDescriptorSetLayout(P.device, l->set, NULL);
        free(l);
        return NULL;
    }
    VkDescriptorSetLayout sets[2] = {l->set, l->set1};
    VkPipelineLayoutCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                     .flags = VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT, .setLayoutCount = 2,
                                     .pSetLayouts = sets};
    if (vkCreatePipelineLayout(P.device, &pi, NULL, &l->layout) != VK_SUCCESS) {
        vkDestroyDescriptorSetLayout(P.device, l->set, NULL);
        vkDestroyDescriptorSetLayout(P.device, l->set1, NULL);
        free(l);
        return NULL;
    }
    l->tpl[0] = make_tpl(l->b, n0, l->set, l->push, l->layout, 0);
    l->tpl[1] = make_tpl(l->b + n0, n - n0, l->set1, l->push1, l->layout, 1);
    l->next = *head;
    *head = l;
    P.stats.layouts++;
    P.stats.push_layouts += (uint64_t)(l->push | l->push1);
    return l;
}

// Vertex outputs with independent layouts: each vertex shader declares the union of the inputs of every fragment
// shader met with it (pipeline-keys.bin at the prewarm, then each pipeline built), so its module does not depend on the
// fragment shader of one pipeline. Unwritten ones read (0, 0, 0, 1) as before. MKW_VS_OUTS=all declares all 32.
VsOutsCache VO = {.lock = PLAT_MUTEX_INIT};

// Adds fs's inputs (packed ones excepted) to vs's union; returns the union, types in io (the first type met wins).
uint32_t vs_outs(const GfxShader *vs, const GfxShader *fs, GfxShaderIo io[32]) {
    GfxShaderIo fin[32];
    uint32_t need = fs ? gfx_shader_inputs(fs, fin) : 0, mask;
    for (int i = 0; i < 32; i++)
        if (need >> i & 1 && fin[i].packed) need &= ~(1u << i);
    plat_mutex_lock(&VO.lock);
    uint32_t j = (uint32_t)(((uintptr_t)vs >> 4) * 0x9e3779b1u) & (VS_OUTS_CAP - 1);
    while (VO.vs[j] && VO.vs[j] != vs) j = (j + 1) & (VS_OUTS_CAP - 1);
    if (!VO.vs[j] && VO.n + 1 >= VS_OUTS_CAP * 3 / 4) {   // full: this pair alone
        VO.full++;
        plat_mutex_unlock(&VO.lock);
        LOG_ONCE("vk_pipeline: vertex output union table full: outputs from the pair");
        memset(io, 0, 32 * sizeof *io);
        for (int i = 0; i < 32; i++)
            if (need >> i & 1) io[i] = fin[i];
        return need;
    }
    if (!VO.vs[j]) VO.vs[j] = vs, VO.n++;
    for (int i = 0; i < 32; i++)
        if (!(need >> i & 1)) continue;
        else if (!(VO.mask[j] >> i & 1)) VO.io[j][i] = fin[i], VO.mask[j] |= 1u << i;
        else if (VO.io[j][i].type == fin[i].type && VO.io[j][i].comps < fin[i].comps) VO.io[j][i].comps = fin[i].comps;
    mask = VO.mask[j];
    memcpy(io, VO.io[j], 32 * sizeof *io);
    plat_mutex_unlock(&VO.lock);
    return mask;
}

