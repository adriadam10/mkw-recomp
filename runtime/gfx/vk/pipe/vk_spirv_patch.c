// SPIR-V rewrites of the translated shaders, split from vk_pipeline.c (see vk_spirv_patch.h).
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "vk.h"
#include "vk_spirv_patch.h"

// Binding decorations b >= 1 become b + off (the fragment stage's share of set 0).
uint32_t *spirv_rebind(const uint32_t *w, uint64_t n, uint32_t off) {
    uint32_t *o = malloc(n * 4);
    if (!o) return NULL;
    memcpy(o, w, n * 4);
    for (uint64_t i = 5; i < n;) {
        uint32_t op = o[i] & 0xffff, len = o[i] >> 16;
        if (!len || i + len > n) break;
        if (op == SPV_OP_DECORATE && len >= 4 && o[i + 2] == SPV_DEC_BINDING && o[i + 3] >= 1) o[i + 3] += off;
        i += len;
    }
    return o;
}

// DescriptorSet decorations become set (the fragment stage's resources move to set 1 with independent layouts).
uint32_t *spirv_set(const uint32_t *w, uint64_t n, uint32_t set) {
    uint32_t *o = malloc(n * 4);
    if (!o) return NULL;
    memcpy(o, w, n * 4);
    for (uint64_t i = 5; i < n;) {
        uint32_t op = o[i] & 0xffff, len = o[i] >> 16;
        if (!len || i + len > n) break;
        if (op == SPV_OP_DECORATE && len >= 4 && o[i + 2] == SPV_DEC_DESCRIPTOR_SET) o[i + 3] = set;
        i += len;
    }
    return o;
}

// Dual-source blending (docs/maxwell.md 5.6): SRC1 blend factors read the fragment shader's colour output 1,
// which Vulkan takes from Location 0, Index 1. The Output variable at Location 1 moves there, as Ryujinx declares it
// with DualSourceBlendEnable (CodeGen/Spirv/Declarations.cs); the pipeline is keyed by GKey.dsb, so only pipelines
// that blend with SRC1 get this module. NULL when the module has no Output at Location 1.
uint32_t *spirv_dual_source(const uint32_t *w, uint64_t n, uint64_t *out_n) {
    uint32_t outs[64];
    int nout = 0;
    uint64_t at = 0;
    for (uint64_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > n) return NULL;
        if (op == SPV_OP_VARIABLE && len >= 4 && w[i + 3] == SPV_SC_OUTPUT && nout < 64) outs[nout++] = w[i + 2];
        i += len;
    }
    for (uint64_t i = 5; i < n && !at;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (op == SPV_OP_DECORATE && len == 4 && w[i + 2] == SPV_DEC_LOCATION && w[i + 3] == 1)
            for (int k = 0; k < nout; k++)
                if (outs[k] == w[i + 1]) at = i;
        i += len;
    }
    if (!at) return NULL;
    uint32_t *o = malloc((n + 4) * 4);
    if (!o) return NULL;
    memcpy(o, w, (at + 4) * 4);
    o[at + 3] = 0;
    o[at + 4] = 4u << 16 | SPV_OP_DECORATE, o[at + 5] = w[at + 1], o[at + 6] = SPV_DEC_INDEX, o[at + 7] = 1;
    memcpy(o + at + 8, w + at + 4, (n - at - 4) * 4);
    *out_n = n + 4;
    return o;
}

// Adds an Output variable for each Location of `missing` (type and width from io) to a module, declared in the entry
// point interface and initialised as Maxwell reads an unwritten output: (0, 0, 0, 1), the w as the bits of 1.0f whatever
// the type (attribute memory is untyped); fewer than 4 components are all zero. Existing scalar, vector and pointer types
// are reused. NULL when the module does not have the expected layout.
uint32_t *spirv_add_outputs(const uint32_t *w, uint64_t n, uint32_t missing, const GfxShaderIo io[32],
                            uint64_t *out_n) {
    uint64_t ep_at = 0, ann_end = 0, fn_at = 0;
    uint32_t bound = w[3], scalar[3] = {0, 0, 0};          // GFX_TYPE_FLOAT, SINT, UINT
    struct { uint32_t id, comp, count; } vec[64];
    struct { uint32_t id, type; } ptr[64];
    int nvec = 0, nptr = 0;
    for (uint64_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > n) return NULL;
        const uint32_t *a = w + i + 1;
        if (op == SPV_OP_ENTRY_POINT && !ep_at) ep_at = i;
        else if (op == SPV_OP_DECORATE || op == 72 || op == 73 || op == 74 || op == 75 || op == 332 || op == 5632 || op == 5633)
            ann_end = i + len;
        else if (op == SPV_OP_TYPE_FLOAT && len >= 3 && a[1] == 32) scalar[GFX_TYPE_FLOAT] = a[0];
        else if (op == SPV_OP_TYPE_INT && len >= 4 && a[1] == 32) scalar[a[2] ? GFX_TYPE_SINT : GFX_TYPE_UINT] = a[0];
        else if (op == SPV_OP_TYPE_VECTOR && len >= 4 && nvec < 64) vec[nvec].id = a[0], vec[nvec].comp = a[1], vec[nvec++].count = a[2];
        else if (op == SPV_OP_TYPE_POINTER && len >= 4 && a[1] == SPV_SC_OUTPUT && nptr < 64) ptr[nptr].id = a[0], ptr[nptr++].type = a[2];
        else if (op == SPV_OP_FUNCTION) { fn_at = i; break; }
        i += len;
    }
    if (!ep_at || !ann_end || !fn_at || ann_end < ep_at || fn_at < ann_end) return NULL;
    uint32_t types[32 * 40], decs[32 * 4], vars[32], zero[3] = {0, 0, 0}, one[3] = {0, 0, 0};
    int nt = 0, nd = 0, nv = 0;
    for (int l = 0; l < 32; l++) {
        if (!(missing >> l & 1)) continue;
        int t = io[l].type <= GFX_TYPE_UINT ? io[l].type : GFX_TYPE_FLOAT;
        uint32_t comps = io[l].comps ? io[l].comps : 4;
        if (!scalar[t]) {
            scalar[t] = bound++;
            if (t == GFX_TYPE_FLOAT) types[nt++] = 3u << 16 | SPV_OP_TYPE_FLOAT, types[nt++] = scalar[t], types[nt++] = 32;
            else types[nt++] = 4u << 16 | SPV_OP_TYPE_INT, types[nt++] = scalar[t], types[nt++] = 32, types[nt++] = t == GFX_TYPE_SINT;
        }
        uint32_t type = scalar[t];
        if (comps > 1) {
            int k = 0;
            while (k < nvec && !(vec[k].comp == type && vec[k].count == comps)) k++;
            if (k == nvec) {
                if (nvec == 64) return NULL;
                vec[nvec].id = bound++, vec[nvec].comp = type, vec[nvec++].count = comps;
                types[nt++] = 4u << 16 | SPV_OP_TYPE_VECTOR, types[nt++] = vec[k].id, types[nt++] = type, types[nt++] = comps;
            }
            type = vec[k].id;
        }
        int k = 0;
        while (k < nptr && ptr[k].type != type) k++;
        if (k == nptr) {
            if (nptr == 64) return NULL;
            ptr[nptr].id = bound++, ptr[nptr++].type = type;
            types[nt++] = 4u << 16 | SPV_OP_TYPE_POINTER, types[nt++] = ptr[k].id, types[nt++] = SPV_SC_OUTPUT, types[nt++] = type;
        }
        uint32_t init = bound++, var = bound++;
        if (comps == 4) {
            if (!zero[t]) {
                zero[t] = bound++, one[t] = bound++;
                types[nt++] = 4u << 16 | SPV_OP_CONSTANT, types[nt++] = scalar[t], types[nt++] = zero[t], types[nt++] = 0;
                types[nt++] = 4u << 16 | SPV_OP_CONSTANT, types[nt++] = scalar[t], types[nt++] = one[t], types[nt++] = 0x3f800000;
            }
            types[nt++] = 7u << 16 | SPV_OP_CONSTANT_COMPOSITE, types[nt++] = type, types[nt++] = init;
            types[nt++] = zero[t], types[nt++] = zero[t], types[nt++] = zero[t], types[nt++] = one[t];
        } else {
            types[nt++] = 3u << 16 | SPV_OP_CONSTANT_NULL, types[nt++] = type, types[nt++] = init;
        }
        types[nt++] = 5u << 16 | SPV_OP_VARIABLE, types[nt++] = ptr[k].id, types[nt++] = var, types[nt++] = SPV_SC_OUTPUT,
        types[nt++] = init;
        decs[nd++] = 4u << 16 | SPV_OP_DECORATE, decs[nd++] = var, decs[nd++] = SPV_DEC_LOCATION, decs[nd++] = (uint32_t)l;
        vars[nv++] = var;
    }
    uint64_t total = n + (uint64_t)(nt + nd + nv);
    uint32_t *o = malloc(total * 4), *p = o;
    if (!o) return NULL;
    uint32_t ep_len = w[ep_at] >> 16;
    memcpy(p, w, 5 * 4), p[3] = bound, p += 5;
    memcpy(p, w + 5, (ep_at - 5) * 4), p += ep_at - 5;
    *p++ = (ep_len + (uint32_t)nv) << 16 | SPV_OP_ENTRY_POINT;
    memcpy(p, w + ep_at + 1, (ep_len - 1) * 4), p += ep_len - 1;
    memcpy(p, vars, (size_t)nv * 4), p += nv;
    memcpy(p, w + ep_at + ep_len, (ann_end - ep_at - ep_len) * 4), p += ann_end - ep_at - ep_len;
    memcpy(p, decs, (size_t)nd * 4), p += nd;
    memcpy(p, w + ann_end, (fn_at - ann_end) * 4), p += fn_at - ann_end;
    memcpy(p, types, (size_t)nt * 4), p += nt;
    memcpy(p, w + fn_at, (n - fn_at) * 4), p += n - fn_at;
    *out_n = (uint64_t)(p - o);
    return o;
}

// Two fixes for what the translator declares but a shader does not use (NULL: nothing to change):
//  - the fragment shader interlock capability and extension, which Ryujinx adds to every fragment shader when the host
//    reports interlock support (SpirvGenerator.cs) and vk_device.c does not enable; kept when the shader interlocks;
//  - the CullDistance[1] member of gl_PerVertex next to ClipDistance[8] (Declarations.cs CreatePerVertexStructType):
//    9 distances exceed maxCombinedClipAndCullDistances (8 on the RX 9070). Ryujinx never accesses CullDistance; the
//    member goes when it is the struct's last and no access chain selects it.
uint32_t *spirv_fixup(const uint32_t *w, uint64_t n, uint64_t *out_n) {
    enum { OP_EXTENSION = 10, OP_CAPABILITY = 17, OP_MEMBER_NAME = 6, OP_EXECUTION_MODE = 16, OP_TYPE_ARRAY = 28,
           OP_TYPE_STRUCT = 30, OP_CONSTANT = 43, OP_MEMBER_DECORATE = 72, OP_ACCESS_CHAIN = 65, OP_IN_BOUNDS_ACCESS_CHAIN = 66,
           OP_BEGIN_INTERLOCK = 5364, OP_END_INTERLOCK = 5365, CAP_SAMPLE_INTERLOCK = 5363, CAP_PIXEL_INTERLOCK = 5378,
           CAP_RATE_INTERLOCK = 5372, BUILTIN_CLIP = 3, BUILTIN_CULL = 4 };
    if (n < 5) return NULL;
    uint32_t bound = w[3] < (1u << 22) ? w[3] : (1u << 22);
    uint32_t *val = calloc(bound, sizeof *val), *elem = calloc(bound, sizeof *elem), *len = calloc(bound, sizeof *len),
             *pointee = calloc(bound, sizeof *pointee), *var_type = calloc(bound, sizeof *var_type);
    uint32_t *o = NULL;
    int interlock_used = 0, interlock_declared = 0, cull_ok = 0;
    uint32_t cull_struct = 0, cull_member = 0, clip_member = ~0u, members = 0, member_types[8] = {0};
    if (!val || !elem || !len || !pointee || !var_type) goto out;
    for (uint64_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, l = w[i] >> 16;
        if (!l || i + l > n) goto out;
        const uint32_t *a = w + i + 1;
        if (op == OP_BEGIN_INTERLOCK || op == OP_END_INTERLOCK) interlock_used = 1;
        else if (op == OP_EXECUTION_MODE && l >= 3 && a[1] >= 5366 && a[1] <= 5371) interlock_used = 1;
        else if (op == OP_CAPABILITY && l >= 2 && (a[0] == CAP_PIXEL_INTERLOCK || a[0] == CAP_SAMPLE_INTERLOCK || a[0] == CAP_RATE_INTERLOCK))
            interlock_declared = 1;
        else if (op == OP_CONSTANT && l == 4 && a[1] < bound) val[a[1]] = a[2];
        else if (op == OP_TYPE_ARRAY && l >= 4 && a[0] < bound) elem[a[0]] = a[1], len[a[0]] = a[2] < bound ? val[a[2]] : 0;
        else if (op == SPV_OP_TYPE_POINTER && l >= 4 && a[0] < bound) pointee[a[0]] = a[2];
        else if (op == SPV_OP_VARIABLE && l >= 4 && a[1] < bound) var_type[a[1]] = a[0] < bound ? pointee[a[0]] : 0;
        else if (op == OP_MEMBER_DECORATE && l >= 5 && a[2] == SPV_DEC_BUILTIN && a[3] == BUILTIN_CULL) cull_struct = a[0], cull_member = a[1];
        else if (op == OP_MEMBER_DECORATE && l >= 5 && a[2] == SPV_DEC_BUILTIN && a[3] == BUILTIN_CLIP) clip_member = a[1];
        else if (op == OP_TYPE_STRUCT && cull_struct && a[0] == cull_struct && l >= 2) {
            members = l - 2;
            for (uint32_t m = 0; m < members && m < 8; m++) member_types[m] = a[1 + m];
        } else if ((op == OP_ACCESS_CHAIN || op == OP_IN_BOUNDS_ACCESS_CHAIN) && l >= 5 && cull_struct && a[2] < bound) {
            uint32_t t = var_type[a[2]], k = 3;
            if (t < bound && t && elem[t] == cull_struct) k = 4;   // arrayed per-vertex block
            if ((t == cull_struct || k == 4) && k + 1 < l && a[k] < bound && val[a[k]] == cull_member) cull_struct = 0;
        }
        i += l;
    }
    VkCtx *c = vk_ctx();
    uint32_t limit = c ? c->props.limits.maxCombinedClipAndCullDistances : 8;
    if (cull_struct && members && members <= 8 && cull_member == members - 1 && clip_member < members) {
        uint32_t clip_t = member_types[clip_member], cull_t = member_types[cull_member];
        uint32_t clip_n = clip_t < bound ? len[clip_t] : 0, cull_n = cull_t < bound ? len[cull_t] : 0;
        cull_ok = clip_n + cull_n > limit && clip_n <= limit;
    }
    int strip = interlock_declared && !interlock_used;
    if (!strip && !cull_ok) goto out;
    o = malloc(n * 4);
    if (!o) goto out;
    uint64_t on = 5;
    memcpy(o, w, 5 * 4);
    for (uint64_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, l = w[i] >> 16;
        const uint32_t *a = w + i + 1;
        int drop = 0;
        if (strip && op == OP_CAPABILITY && (a[0] == CAP_PIXEL_INTERLOCK || a[0] == CAP_SAMPLE_INTERLOCK || a[0] == CAP_RATE_INTERLOCK)) drop = 1;
        if (strip && op == OP_EXTENSION && l >= 2 && !strncmp((const char *)a, "SPV_EXT_fragment_shader_interlock", (l - 1) * 4)) drop = 1;
        if (cull_ok && (op == OP_MEMBER_DECORATE || op == OP_MEMBER_NAME) && l >= 3 && a[0] == cull_struct && a[1] == cull_member) drop = 1;
        if (!drop) {
            memcpy(o + on, w + i, l * 4);
            if (cull_ok && op == OP_TYPE_STRUCT && a[0] == cull_struct) o[on] = (l - 1) << 16 | op;   // last member gone
            on += op == OP_TYPE_STRUCT && cull_ok && a[0] == cull_struct ? l - 1 : l;
        }
        i += l;
    }
    *out_n = on;
out:
    free(val), free(elem), free(len), free(pointee), free(var_type);
    return o;
}

// True when the module declares a GroupNonUniform* or SubgroupBallot-style capability (lane masks assume 32 lanes).
int uses_subgroup_ops(const uint32_t *w, uint64_t words) {
    for (uint64_t i = 5; i < words;) {
        uint32_t op = w[i] & 0xffff, n = w[i] >> 16;
        if (!n || i + n > words) return 0;
        if (op != 17) return 0;   // OpCapability; capabilities come first, stop at the first other instruction
        uint32_t cap = w[i + 1];
        if ((cap >= 61 && cap <= 68) || cap == 4423 || cap == 4431) return 1;   // NonUniform*, SubgroupBallotKHR, SubgroupVoteKHR
        i += n;
    }
    return 0;
}

// NVN's alpha test (nvnColorStateSetAlphaTest, nvnCommandBufferSetAlphaRef): a fragment whose colour output 0 alpha
// fails `alpha FUNC ref` is discarded. The countdown's numbers enter with blending off and the alpha test on; without
// it their transparent texels (white, alpha 0) drew a white square (sq5, 06/11/2026). As eden-nx2 does (idea only,
// GPL: shader_recompiler/backend/spirv/emit_spirv_special.cpp AlphaTest), the test goes before every OpReturn of the
// entry point, with func and ref baked in (GKey.alpha_func / alpha_ref). func: VkCompareOp. NULL when the module has no
// float vec4 Output at Location 0 or another layout.
uint32_t *spirv_alpha_test(const uint32_t *w, uint64_t n, int func, float ref, uint64_t *out_n) {
    enum { OP_TYPE_BOOL = 20, OP_CONSTANT_FALSE = 42, OP_FUNCTION_END = 56, OP_LOAD = 61, OP_COMPOSITE_EXTRACT = 81,
           OP_SELECTION_MERGE = 247, OP_LABEL = 248, OP_BRANCH_CONDITIONAL = 250, OP_KILL = 252, OP_RETURN = 253,
           OP_TERMINATE_INVOCATION = 4416 };
    static const uint16_t cmp_op[8] = {0, 184, 180, 188, 186, 182, 190, 0};   // VkCompareOp -> OpFOrd*
    if (func == VK_COMPARE_OP_ALWAYS || func < 0 || func > 7 || n < 5) return NULL;
    uint32_t bound = w[3], entry = 0, f32 = 0, boolt = 0, out0 = 0, v4 = 0, nret = 0;
    uint32_t vars[64][2], ptrs[64][2], vecs[64][3];   // (id, pointer type), (id, pointee), (id, comp, count)
    int nvars = 0, nptrs = 0, nvecs = 0;
    uint64_t fn_at = 0, fn_lo = 0, fn_hi = 0;
    uint32_t loc0[16];
    int nloc0 = 0;
    for (uint64_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > n) return NULL;
        const uint32_t *a = w + i + 1;
        if (op == SPV_OP_ENTRY_POINT && len >= 3 && !entry) entry = a[1];
        else if (op == SPV_OP_DECORATE && len == 4 && a[1] == SPV_DEC_LOCATION && a[2] == 0 && nloc0 < 16) loc0[nloc0++] = a[0];
        else if (op == SPV_OP_TYPE_FLOAT && len == 3 && a[1] == 32) f32 = a[0];
        else if (op == OP_TYPE_BOOL && len == 2) boolt = a[0];
        else if (op == SPV_OP_TYPE_VECTOR && len == 4 && nvecs < 64) vecs[nvecs][0] = a[0], vecs[nvecs][1] = a[1], vecs[nvecs++][2] = a[2];
        else if (op == SPV_OP_TYPE_POINTER && len == 4 && a[1] == SPV_SC_OUTPUT && nptrs < 64) ptrs[nptrs][0] = a[0], ptrs[nptrs++][1] = a[2];
        else if (op == SPV_OP_VARIABLE && len >= 4 && a[2] == SPV_SC_OUTPUT && nvars < 64) vars[nvars][0] = a[1], vars[nvars++][1] = a[0];
        else if (op == SPV_OP_FUNCTION && !fn_at) fn_at = i;
        if (op == SPV_OP_FUNCTION && len == 5 && a[1] == entry) fn_lo = i;
        if (op == OP_FUNCTION_END && fn_lo && !fn_hi) fn_hi = i;
        if (op == OP_RETURN && fn_lo && !fn_hi) nret++;
        i += len;
    }
    for (int k = 0; k < nvars && !out0; k++)
        for (int j = 0; j < nloc0; j++)
            if (vars[k][0] == loc0[j])
                for (int p = 0; p < nptrs; p++)
                    if (ptrs[p][0] == vars[k][1])
                        for (int v = 0; v < nvecs; v++)
                            if (vecs[v][0] == ptrs[p][1] && vecs[v][1] == f32 && vecs[v][2] == 4) out0 = vars[k][0], v4 = vecs[v][0];
    if (!out0 || !f32 || !fn_at || !fn_lo || !fn_hi || !nret) return NULL;
    uint32_t kill = w[1] >= 0x10600 ? OP_TERMINATE_INVOCATION : OP_KILL;
    // Types and constants go before the first function: bool if missing, the reference (or false for NEVER).
    uint32_t head[8], nh = 0, cref = bound++, cfalse = 0;
    if (!boolt) boolt = bound++, head[nh++] = 2u << 16 | OP_TYPE_BOOL, head[nh++] = boolt;
    uint32_t refbits;
    memcpy(&refbits, &ref, 4);
    head[nh++] = 4u << 16 | SPV_OP_CONSTANT, head[nh++] = f32, head[nh++] = cref, head[nh++] = refbits;
    if (func == VK_COMPARE_OP_NEVER) cfalse = bound++, head[nh++] = 3u << 16 | OP_CONSTANT_FALSE, head[nh++] = boolt, head[nh++] = cfalse;
    const uint32_t per = 4 + 5 + (cfalse ? 0 : 5) + 3 + 4 + 2 + 1 + 2;   // load, extract, compare, merge, branch, labels, kill
    uint32_t *o = malloc((n + nh + (uint64_t)nret * per) * 4);
    if (!o) return NULL;
    uint64_t at = 0;
    for (uint64_t i = 0; i < n;) {
        uint32_t op = i < 5 ? 0 : w[i] & 0xffff, len = i < 5 ? 5 : w[i] >> 16;
        if (i == fn_at) memcpy(o + at, head, nh * 4), at += nh;
        if (op == OP_RETURN && i > fn_lo && i < fn_hi) {
            uint32_t c = bound++, al = bound++, t = cfalse ? cfalse : bound++, ok = bound++, kl = bound++;
            o[at++] = 4u << 16 | OP_LOAD, o[at++] = v4, o[at++] = c, o[at++] = out0;
            o[at++] = 5u << 16 | OP_COMPOSITE_EXTRACT, o[at++] = f32, o[at++] = al, o[at++] = c, o[at++] = 3;
            if (!cfalse) o[at++] = 5u << 16 | cmp_op[func], o[at++] = boolt, o[at++] = t, o[at++] = al, o[at++] = cref;
            o[at++] = 3u << 16 | OP_SELECTION_MERGE, o[at++] = ok, o[at++] = 0;
            o[at++] = 4u << 16 | OP_BRANCH_CONDITIONAL, o[at++] = t, o[at++] = ok, o[at++] = kl;
            o[at++] = 2u << 16 | OP_LABEL, o[at++] = kl;
            o[at++] = 1u << 16 | kill;
            o[at++] = 2u << 16 | OP_LABEL, o[at++] = ok;
        }
        memcpy(o + at, w + i, len * 4), at += len;
        i += len;
    }
    o[3] = bound;
    *out_n = at;
    return o;
}

// Workgroup memory past the device's maxComputeSharedMemorySize: the largest workgroup uint array (the translator's
// smem, sized from the program's control section) shrinks so the module fits; NULL when it fits or the layout is not
// the translator's. MKW declares 48 KiB in 168 compute programs; Adreno 7xx has 32 KiB, and Turnip's compiler ends the
// process on such a shader (ir3.c ir3_get_reg_independent_max_waves: no workgroup fits, "impossible to have enough
// concurrent waves", exit(1)). The 168 index smem by the thread's global index (gl_WorkGroupID.x * 64 + local, times
// 48 bytes, up to 1024 items), so every workgroup past the 10th reaches past the cut end: undefined on Adreno, where it
// hits other workgroups' memory (flashing blue lines in races). The translator indexes such arrays modulo a sentinel
// constant (SMEM_WRAP_SENTINEL), set here to the words kept: each workgroup's 3 KiB window then wraps inside the array.
#define SMEM_WRAP_SENTINEL 0x7ffe5a3bu

uint32_t *spirv_clamp_shared(const uint32_t *w, uint64_t n, uint32_t limit, uint64_t *out_n, uint32_t *from,
                             uint32_t *to) {
    enum { OP_TYPE_BOOL = 20, OP_TYPE_ARRAY = 28, OP_TYPE_STRUCT = 30, SC_WORKGROUP = 4 };
    if (n < 5 || !limit) return NULL;
    uint32_t bound = w[3];
    if (bound >= (1u << 22)) return NULL;
    uint64_t *size = calloc(bound, sizeof *size);
    uint32_t *val = calloc(bound, sizeof *val), *arr_elem = calloc(bound, sizeof *arr_elem),
             *ptr_to = calloc(bound, sizeof *ptr_to);
    uint8_t *is_u32 = calloc(bound, 1), *other_ptr = calloc(bound, 1);
    uint32_t *o = NULL, wg_vars[16], nwg = 0;
    if (!size || !val || !arr_elem || !ptr_to || !is_u32 || !other_ptr) goto out;
    for (uint64_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, l = w[i] >> 16;
        if (!l || i + l > n) goto out;
        const uint32_t *a = w + i + 1;
        if (op == SPV_OP_FUNCTION) break;
        if ((op == SPV_OP_TYPE_INT || op == SPV_OP_TYPE_FLOAT) && l >= 3 && a[0] < bound) {
            size[a[0]] = a[1] / 8;
            is_u32[a[0]] = op == SPV_OP_TYPE_INT && a[1] == 32;
        } else if (op == OP_TYPE_BOOL && l >= 2 && a[0] < bound) size[a[0]] = 4;
        else if (op == SPV_OP_TYPE_VECTOR && l >= 4 && a[0] < bound && a[1] < bound) size[a[0]] = size[a[1]] * a[2];
        else if (op == SPV_OP_CONSTANT && l == 4 && a[1] < bound) val[a[1]] = a[2];
        else if (op == OP_TYPE_ARRAY && l >= 4 && a[0] < bound && a[1] < bound && a[2] < bound) {
            arr_elem[a[0]] = a[1];
            size[a[0]] = size[a[1]] * val[a[2]];
        } else if (op == OP_TYPE_STRUCT && l >= 2 && a[0] < bound) {
            for (uint32_t m = 1; m + 1 < l; m++) size[a[0]] += a[m] < bound ? size[a[m]] : 0;
        } else if (op == SPV_OP_TYPE_POINTER && l >= 4 && a[0] < bound && a[2] < bound) {
            ptr_to[a[0]] = a[2];
            if (a[1] != SC_WORKGROUP) other_ptr[a[2]] = 1;
        } else if (op == SPV_OP_VARIABLE && l >= 4 && a[2] == SC_WORKGROUP && a[0] < bound && nwg < 16) {
            wg_vars[nwg++] = ptr_to[a[0]];
        }
        i += l;
    }
    uint64_t total = 0;
    uint32_t big = 0;
    for (uint32_t k = 0; k < nwg; k++) {
        total += size[wg_vars[k]];
        uint32_t t = wg_vars[k];
        if (t && arr_elem[t] && is_u32[arr_elem[t]] && !other_ptr[t] && (!big || size[t] > size[big])) big = t;
    }
    if (total <= limit || !big || total - size[big] >= limit) goto out;
    uint32_t keep = (uint32_t)((limit - (total - size[big])) / 4);
    o = malloc((n + 4) * 4);
    if (!o) goto out;
    memcpy(o, w, 5 * 4);
    o[3] = bound + 1;
    uint64_t on = 5;
    for (uint64_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, l = w[i] >> 16;
        if (op == OP_TYPE_ARRAY && w[i + 1] == big) {
            o[on++] = 4u << 16 | SPV_OP_CONSTANT, o[on++] = arr_elem[big], o[on++] = bound, o[on++] = keep;
            memcpy(o + on, w + i, l * 4);
            o[on + 3] = bound;
        } else if (op == SPV_OP_CONSTANT && l == 4 && w[i + 1] < bound && is_u32[w[i + 1]] &&
                   w[i + 3] == SMEM_WRAP_SENTINEL) {
            // the translator indexes a shared array over 32 KiB modulo this constant (glsl/common.py
            // SMEM_WRAP_SENTINEL): make it the words kept, so a workgroup's window wraps inside the cut array
            memcpy(o + on, w + i, l * 4);
            o[on + 3] = keep;
        } else {
            memcpy(o + on, w + i, l * 4);
        }
        on += l;
        i += l;
    }
    *out_n = on;
    *from = (uint32_t)total, *to = (uint32_t)(total - size[big] + (uint64_t)keep * 4);
out:
    free(size), free(val), free(arr_elem), free(ptr_to), free(is_u32), free(other_ptr);
    return o;
}
