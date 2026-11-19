// NVN's alpha test as a SPIR-V rewrite of the fragment shader, split from vk_spirv_patch.c (see vk_spirv_patch.h).
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "vk.h"
#include "vk_spirv_patch.h"

// NVN's alpha test (nvnColorStateSetAlphaTest, nvnCommandBufferSetAlphaRef): a fragment whose colour output 0 alpha
// fails `alpha FUNC ref` is discarded. The countdown's numbers enter with blending off and the alpha test on; without
// it their transparent texels (white, alpha 0) drew a white square (sq5, 06/11/2026). As eden-nx2 does (idea only,
// GPL: shader_recompiler/backend/spirv/emit_spirv_special.cpp AlphaTest), the test goes before every OpReturn of the
// entry point. func is baked in (GKey.alpha_func); ref is read from the first float of the support buffer (set 0
// binding 0, Ryujinx's SupportBuffer.FragmentAlphaTest slot, filled per draw by support_fill). With ref baked in too,
// every fade step (0.1 ... 0.9) was a pipeline of its own that no prewarm could know (17/11/2026: the 5 draws a
// qrace replay still found libraries missing for). func: VkCompareOp. NULL when the module has no float vec4 Output at
// Location 0 or another layout.
uint32_t *spirv_alpha_test(const uint32_t *w, uint64_t n, int func, uint64_t *out_n) {
    enum { OP_TYPE_BOOL = 20, OP_TYPE_STRUCT = 30, OP_CONSTANT_FALSE = 42, OP_FUNCTION_END = 56, OP_LOAD = 61,
           OP_ACCESS_CHAIN = 65, OP_MEMBER_DECORATE = 72, OP_COMPOSITE_EXTRACT = 81, OP_SELECTION_MERGE = 247,
           OP_LABEL = 248, OP_BRANCH_CONDITIONAL = 250, OP_KILL = 252, OP_RETURN = 253,
           OP_TERMINATE_INVOCATION = 4416, SC_UNIFORM = 2, DEC_BLOCK = 2, DEC_OFFSET = 35 };
    static const uint16_t cmp_op[8] = {0, 184, 180, 188, 186, 182, 190, 0};   // VkCompareOp -> OpFOrd*
    if (func == VK_COMPARE_OP_ALWAYS || func < 0 || func > 7 || n < 5) return NULL;
    uint32_t bound = w[3], entry = 0, f32 = 0, boolt = 0, i32 = 0, pf_uni = 0, out0 = 0, v4 = 0, nret = 0;
    uint32_t vars[64][2], ptrs[64][2], vecs[64][3];   // (id, pointer type), (id, pointee), (id, comp, count)
    int nvars = 0, nptrs = 0, nvecs = 0;
    uint64_t ep_at = 0, ty_at = 0, fn_at = 0, fn_lo = 0, fn_hi = 0;
    uint32_t loc0[16];
    int nloc0 = 0;
    for (uint64_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > n) return NULL;
        const uint32_t *a = w + i + 1;
        if (op == SPV_OP_ENTRY_POINT && len >= 3 && !entry) entry = a[1], ep_at = i;
        else if (op == SPV_OP_DECORATE && len == 4 && a[1] == SPV_DEC_LOCATION && a[2] == 0 && nloc0 < 16) loc0[nloc0++] = a[0];
        else if (op == SPV_OP_TYPE_FLOAT && len == 3 && a[1] == 32) f32 = a[0];
        else if (op == SPV_OP_TYPE_INT && len == 4 && a[1] == 32 && !i32) i32 = a[0];
        else if (op == OP_TYPE_BOOL && len == 2) boolt = a[0];
        else if (op == SPV_OP_TYPE_VECTOR && len == 4 && nvecs < 64) vecs[nvecs][0] = a[0], vecs[nvecs][1] = a[1], vecs[nvecs++][2] = a[2];
        else if (op == SPV_OP_TYPE_POINTER && len == 4 && a[1] == SPV_SC_OUTPUT && nptrs < 64) ptrs[nptrs][0] = a[0], ptrs[nptrs++][1] = a[2];
        else if (op == SPV_OP_TYPE_POINTER && len == 4 && a[1] == SC_UNIFORM && f32 && a[2] == f32) pf_uni = a[0];
        else if (op == SPV_OP_VARIABLE && len >= 4 && a[2] == SPV_SC_OUTPUT && nvars < 64) vars[nvars][0] = a[1], vars[nvars++][1] = a[0];
        // types, constants and global variables start after the annotations: the new decorations go before the first
        if (!ty_at && !fn_at && ((op >= 19 && op <= 52) || op == SPV_OP_VARIABLE)) ty_at = i;
        if (op == SPV_OP_FUNCTION && !fn_at) fn_at = i;
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
    if (!out0 || !f32 || !ep_at || !ty_at || !fn_at || !fn_lo || !fn_hi || !nret) return NULL;
    uint32_t kill = w[1] >= 0x10600 ? OP_TERMINATE_INVOCATION : OP_KILL;
    int never = func == VK_COMPARE_OP_NEVER, iface = w[1] >= 0x10400;   // from SPIR-V 1.4 the interface lists every global
    // The reference block { float ref; } at set 0 binding 0 (spirv_set and spirv_rebind move it with the stage's other
    // resources): its decorations before the types, its types and variable before the first function.
    uint32_t st = 0, var = 0, ann[16], na = 0, head[40], nh = 0, czero = 0, cfalse = 0;
    if (!never) {
        st = bound++, var = bound++;
        uint32_t pst = bound++;
        ann[na++] = 4u << 16 | SPV_OP_DECORATE, ann[na++] = var, ann[na++] = SPV_DEC_DESCRIPTOR_SET, ann[na++] = 0;
        ann[na++] = 4u << 16 | SPV_OP_DECORATE, ann[na++] = var, ann[na++] = SPV_DEC_BINDING, ann[na++] = 0;
        ann[na++] = 3u << 16 | SPV_OP_DECORATE, ann[na++] = st, ann[na++] = DEC_BLOCK;
        ann[na++] = 5u << 16 | OP_MEMBER_DECORATE, ann[na++] = st, ann[na++] = 0, ann[na++] = DEC_OFFSET, ann[na++] = 0;
        if (!i32) i32 = bound++, head[nh++] = 4u << 16 | SPV_OP_TYPE_INT, head[nh++] = i32, head[nh++] = 32, head[nh++] = 0;
        czero = bound++;
        head[nh++] = 4u << 16 | SPV_OP_CONSTANT, head[nh++] = i32, head[nh++] = czero, head[nh++] = 0;
        head[nh++] = 3u << 16 | OP_TYPE_STRUCT, head[nh++] = st, head[nh++] = f32;
        head[nh++] = 4u << 16 | SPV_OP_TYPE_POINTER, head[nh++] = pst, head[nh++] = SC_UNIFORM, head[nh++] = st;
        if (!pf_uni) {
            pf_uni = bound++;
            head[nh++] = 4u << 16 | SPV_OP_TYPE_POINTER, head[nh++] = pf_uni, head[nh++] = SC_UNIFORM, head[nh++] = f32;
        }
        head[nh++] = 4u << 16 | SPV_OP_VARIABLE, head[nh++] = pst, head[nh++] = var, head[nh++] = SC_UNIFORM;
    }
    if (!boolt) boolt = bound++, head[nh++] = 2u << 16 | OP_TYPE_BOOL, head[nh++] = boolt;
    if (never) cfalse = bound++, head[nh++] = 3u << 16 | OP_CONSTANT_FALSE, head[nh++] = boolt, head[nh++] = cfalse;
    // per return: access chain, load ref, load colour, extract, compare, merge, branch, labels, kill
    const uint32_t per = (never ? 0 : 5 + 4 + 5) + 4 + 5 + 3 + 4 + 2 + 1 + 2;
    uint32_t *o = malloc((n + 1 + na + nh + (uint64_t)nret * per) * 4);
    if (!o) return NULL;
    uint64_t at = 0;
    for (uint64_t i = 0; i < n;) {
        uint32_t op = i < 5 ? 0 : w[i] & 0xffff, len = i < 5 ? 5 : w[i] >> 16;
        if (i == ty_at) memcpy(o + at, ann, na * 4), at += na;
        if (i == fn_at) memcpy(o + at, head, nh * 4), at += nh;
        if (op == OP_RETURN && i > fn_lo && i < fn_hi) {
            uint32_t c = bound++, al = bound++, t = cfalse ? cfalse : bound++, ok = bound++, kl = bound++;
            if (!never) {
                uint32_t pr = bound++, ref = bound++;
                o[at++] = 5u << 16 | OP_ACCESS_CHAIN, o[at++] = pf_uni, o[at++] = pr, o[at++] = var, o[at++] = czero;
                o[at++] = 4u << 16 | OP_LOAD, o[at++] = f32, o[at++] = ref, o[at++] = pr;
                o[at++] = 4u << 16 | OP_LOAD, o[at++] = v4, o[at++] = c, o[at++] = out0;
                o[at++] = 5u << 16 | OP_COMPOSITE_EXTRACT, o[at++] = f32, o[at++] = al, o[at++] = c, o[at++] = 3;
                o[at++] = 5u << 16 | cmp_op[func], o[at++] = boolt, o[at++] = t, o[at++] = al, o[at++] = ref;
            } else {
                o[at++] = 4u << 16 | OP_LOAD, o[at++] = v4, o[at++] = c, o[at++] = out0;
                o[at++] = 5u << 16 | OP_COMPOSITE_EXTRACT, o[at++] = f32, o[at++] = al, o[at++] = c, o[at++] = 3;
            }
            o[at++] = 3u << 16 | OP_SELECTION_MERGE, o[at++] = ok, o[at++] = 0;
            o[at++] = 4u << 16 | OP_BRANCH_CONDITIONAL, o[at++] = t, o[at++] = ok, o[at++] = kl;
            o[at++] = 2u << 16 | OP_LABEL, o[at++] = kl;
            o[at++] = 1u << 16 | kill;
            o[at++] = 2u << 16 | OP_LABEL, o[at++] = ok;
        }
        memcpy(o + at, w + i, len * 4), at += len;
        if (i == ep_at && var && iface) o[at - len] += 1u << 16, o[at++] = var;
        i += len;
    }
    o[3] = bound;
    *out_n = at;
    return o;
}
