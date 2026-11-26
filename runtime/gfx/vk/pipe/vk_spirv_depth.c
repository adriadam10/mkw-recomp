// NVN's -1..1 clip-space depth as a SPIR-V rewrite of the last stage before rasterization, split from vk_spirv_patch.c
// (see vk_spirv_patch.h). For drivers without VK_EXT_depth_clip_control (Honeykrisp, Mesa 26.3, Apple M2, 26/11/2026).
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "vk_spirv_patch.h"

// With DeviceSetDepthMode(NEAR_IS_MINUS_W) the game's clip space has z in -w..w; Vulkan's is 0..w. Where the device
// has no VK_EXT_depth_clip_control (negativeOneToOne), the position's z becomes (z + w) / 2, which gives the same
// window depth: with negativeOneToOne the viewport maps z/w as (max - min) / 2 * z/w + (max + min) / 2, without it
// (max - min) * z'/w + min, equal for z' = (z + w) / 2. Clipping (-w <= z <= w becomes 0 <= z' <= w), depth clamp,
// depth bias (on the window depth) and gl_FragCoord.z see the same values. Nothing reads clip-space z back: the stages
// that write the position are the vertex shader (also the one of the mesh-as-compute draws, sm86 MESH_CS_VS) and the
// mesh shader; the translator has no geometry or tessellation.
// - vertex shader: the translator writes gl_Position one component at a time (sm86 glsl/mem.py), so the position is
//   read whole and rewritten before every OpReturn of the entry point. It is an Output variable decorated BuiltIn
//   Position (a vec4) or the member of a block decorated so (glslang's gl_PerVertex).
// - mesh shader: its outputs must not be read (VUID-StandaloneSpirv-MeshEXT-07107), so each store of a whole vertex's
//   position (gl_MeshVerticesEXT[v].gl_Position = ..., sm86 MESH_MAIN) stores the converted value instead. A store of
//   one component or of a whole vertex block cannot be converted alone: NULL.
// NULL also when the module has no position, is not a vertex or mesh shader, or has another layout.
uint32_t *spirv_depth_minus_w(const uint32_t *w, uint64_t n, uint64_t *out_n) {
    enum { OP_TYPE_ARRAY = 28, OP_FUNCTION_END = 56, OP_LOAD = 61, OP_STORE = 62, OP_COPY_MEMORY = 63,
           OP_ACCESS_CHAIN = 65, OP_IN_BOUNDS_ACCESS_CHAIN = 66, OP_MEMBER_DECORATE = 72, OP_COMPOSITE_EXTRACT = 81,
           OP_COMPOSITE_INSERT = 82, OP_FADD = 129, OP_FMUL = 133, OP_RETURN = 253, EM_VERTEX = 0, EM_MESH = 5365,
           BI_POSITION = 0 };
    if (n < 5 || w[0] != 0x07230203) return NULL;
    uint32_t bound = w[3], model = 0, entry = 0, f32 = 0, i32 = 0, v4 = 0, pv4_out = 0, nret = 0;
    uint32_t pos_var = 0, pos_struct = 0, pos_member = 0;
    uint32_t ptrs[128][3], vars[128][3], arrays[32][2], consts[256][2];   // (id, class, pointee), (id, type, class),
    int nptrs = 0, nvars = 0, narrays = 0, nconsts = 0;                    // (id, element), (id, value)
    uint64_t fn_at = 0, fn_lo = 0, fn_hi = 0;
    for (uint64_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > n) return NULL;
        const uint32_t *a = w + i + 1;
        if (op == SPV_OP_ENTRY_POINT && len >= 3 && !entry) model = a[0], entry = a[1];
        else if (op == SPV_OP_DECORATE && len == 4 && a[1] == SPV_DEC_BUILTIN && a[2] == BI_POSITION) pos_var = a[0];
        else if (op == OP_MEMBER_DECORATE && len == 5 && a[2] == SPV_DEC_BUILTIN && a[3] == BI_POSITION)
            pos_struct = a[0], pos_member = a[1];
        else if (op == SPV_OP_TYPE_FLOAT && len == 3 && a[1] == 32) f32 = a[0];
        else if (op == SPV_OP_TYPE_INT && len == 4 && a[1] == 32 && !i32) i32 = a[0];
        else if (op == SPV_OP_TYPE_VECTOR && len == 4 && f32 && a[1] == f32 && a[2] == 4) v4 = a[0];
        else if (op == OP_TYPE_ARRAY && len == 4 && narrays < 32) arrays[narrays][0] = a[0], arrays[narrays++][1] = a[1];
        else if (op == SPV_OP_CONSTANT && len == 4 && nconsts < 256) consts[nconsts][0] = a[1], consts[nconsts++][1] = a[2];
        else if (op == SPV_OP_TYPE_POINTER && len == 4 && nptrs < 128)
            ptrs[nptrs][0] = a[0], ptrs[nptrs][1] = a[1], ptrs[nptrs++][2] = a[2];
        else if (op == SPV_OP_VARIABLE && len >= 4 && !fn_at && nvars < 128)
            vars[nvars][0] = a[1], vars[nvars][1] = a[0], vars[nvars++][2] = a[2];
        if (op == SPV_OP_FUNCTION && !fn_at) fn_at = i;
        if (op == SPV_OP_FUNCTION && len == 5 && a[1] == entry) fn_lo = i;
        if (op == OP_FUNCTION_END && fn_lo && !fn_hi) fn_hi = i;
        if (op == OP_RETURN && fn_lo && !fn_hi) nret++;
        i += len;
    }
    if ((model != EM_VERTEX && model != EM_MESH) || !f32 || !v4 || !fn_at || !fn_lo || !fn_hi || !nret) return NULL;
    // The variable that holds the position: the vec4 itself, a block (vertex) or an array of blocks (mesh)
    uint32_t var = 0, block = 0;
    for (int k = 0; k < nvars && !var; k++) {
        if (vars[k][2] != SPV_SC_OUTPUT) continue;
        uint32_t pointee = 0;
        for (int p = 0; p < nptrs; p++)
            if (ptrs[p][0] == vars[k][1]) pointee = ptrs[p][2];
        if (model == EM_VERTEX && pos_var && vars[k][0] == pos_var && pointee == v4) var = pos_var;
        else if (model == EM_VERTEX && pos_struct && pointee == pos_struct) var = vars[k][0], block = 1;
        else if (model == EM_MESH && pos_struct)
            for (int r = 0; r < narrays; r++)
                if (arrays[r][0] == pointee && arrays[r][1] == pos_struct) var = vars[k][0];
    }
    if (!var) return NULL;
    // Mesh: the access chains to a vertex's position (gl_MeshVerticesEXT[v].gl_Position); any other way to write it
    // (one component, a whole vertex, a copy) cannot be converted
    uint32_t pos_ptrs[64], nstore = 0;
    int npos = 0;
    if (model == EM_MESH) {
        for (uint64_t i = fn_at; i < n;) {
            uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
            const uint32_t *a = w + i + 1;
            if ((op == OP_ACCESS_CHAIN || op == OP_IN_BOUNDS_ACCESS_CHAIN) && len >= 4 && a[2] == var) {
                uint32_t member = UINT32_MAX;
                for (int c = 0; len >= 6 && c < nconsts; c++)
                    if (consts[c][0] == a[4]) member = consts[c][1];
                if (len < 6 || member == UINT32_MAX) return NULL;   // a whole vertex, or a member not known
                if (member == pos_member && len > 6) return NULL;     // one component of the position
                if (member == pos_member) {
                    if (npos == 64) return NULL;
                    pos_ptrs[npos++] = a[1];
                }
            }
            if ((op == OP_STORE || op == OP_COPY_MEMORY) && len >= 3 && a[0] == var) return NULL;
            if (op == OP_STORE && len >= 3)
                for (int k = 0; k < npos; k++)
                    if (pos_ptrs[k] == a[0]) nstore++;
            i += len;
        }
        if (!nstore) return NULL;
    }
    for (int p = 0; p < nptrs && !pv4_out; p++)
        if (ptrs[p][1] == SPV_SC_OUTPUT && ptrs[p][2] == v4) pv4_out = ptrs[p][0];
    // Before the first function: what is missing of int, Output vec4 pointer, the member index, and 0.5
    uint32_t head[20], nh = 0, cmember = 0, chalf = bound++;
    if (block) {
        if (!i32) i32 = bound++, head[nh++] = 4u << 16 | SPV_OP_TYPE_INT, head[nh++] = i32, head[nh++] = 32, head[nh++] = 1;
        cmember = bound++;
        head[nh++] = 4u << 16 | SPV_OP_CONSTANT, head[nh++] = i32, head[nh++] = cmember, head[nh++] = pos_member;
        if (!pv4_out) {
            pv4_out = bound++;
            head[nh++] = 4u << 16 | SPV_OP_TYPE_POINTER, head[nh++] = pv4_out, head[nh++] = SPV_SC_OUTPUT, head[nh++] = v4;
        }
    }
    float half = 0.5f;
    uint32_t half_bits;
    memcpy(&half_bits, &half, 4);
    head[nh++] = 4u << 16 | SPV_OP_CONSTANT, head[nh++] = f32, head[nh++] = chalf, head[nh++] = half_bits;
    // z = (z + w) * 0.5 of the vec4 p: 2 extracts, add, multiply, insert; per vertex return also an access chain,
    // a load and a store
    const uint32_t conv = 5 + 5 + 5 + 5 + 6, per = model == EM_MESH ? conv : 5 + 4 + conv + 3;
    uint32_t *o = malloc((n + nh + (uint64_t)(model == EM_MESH ? nstore : nret) * per) * 4);
    if (!o) return NULL;
    uint64_t at = 0;
#define CONVERT(p, q)                                                                                                \
    do {                                                                                                             \
        uint32_t z = bound++, ww = bound++, s = bound++, h = bound++;                                                \
        o[at++] = 5u << 16 | OP_COMPOSITE_EXTRACT, o[at++] = f32, o[at++] = z, o[at++] = (p), o[at++] = 2;          \
        o[at++] = 5u << 16 | OP_COMPOSITE_EXTRACT, o[at++] = f32, o[at++] = ww, o[at++] = (p), o[at++] = 3;         \
        o[at++] = 5u << 16 | OP_FADD, o[at++] = f32, o[at++] = s, o[at++] = z, o[at++] = ww;                         \
        o[at++] = 5u << 16 | OP_FMUL, o[at++] = f32, o[at++] = h, o[at++] = s, o[at++] = chalf;                      \
        o[at++] = 6u << 16 | OP_COMPOSITE_INSERT, o[at++] = v4, o[at++] = (q), o[at++] = h, o[at++] = (p),         \
        o[at++] = 2;                                                                                                 \
    } while (0)
    for (uint64_t i = 0; i < n;) {
        uint32_t op = i < 5 ? 0 : w[i] & 0xffff, len = i < 5 ? 5 : w[i] >> 16;
        if (i == fn_at) memcpy(o + at, head, nh * 4), at += nh;
        int pos_store = 0;
        for (int k = 0; model == EM_MESH && op == OP_STORE && k < npos; k++) pos_store |= pos_ptrs[k] == w[i + 1];
        if (pos_store) {   // the converted value is stored instead
            uint32_t q = bound++;
            CONVERT(w[i + 2], q);
            memcpy(o + at, w + i, len * 4);
            o[at + 2] = q;
            at += len, i += len;
            continue;
        }
        if (model == EM_VERTEX && op == OP_RETURN && i > fn_lo && i < fn_hi) {
            uint32_t ptr = var, p = bound++, q = bound++;
            if (block) {
                ptr = bound++;
                o[at++] = 5u << 16 | OP_ACCESS_CHAIN, o[at++] = pv4_out, o[at++] = ptr, o[at++] = var, o[at++] = cmember;
            }
            o[at++] = 4u << 16 | OP_LOAD, o[at++] = v4, o[at++] = p, o[at++] = ptr;
            CONVERT(p, q);
            o[at++] = 3u << 16 | OP_STORE, o[at++] = ptr, o[at++] = q;
        }
        memcpy(o + at, w + i, len * 4), at += len;
        i += len;
    }
#undef CONVERT
    o[3] = bound;
    *out_n = at;
    return o;
}
