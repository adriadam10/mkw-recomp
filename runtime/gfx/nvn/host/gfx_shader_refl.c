// Reflection of a prepared shader: its .res.json (tools/shaders/translate.py) parsed into GfxShaderRes arrays, its
// specialization words and flags, and its SPIR-V scanned for interface Locations and the workgroup size.
//
// Provenance: the c2 handle ranges of handle_slot are those of MacNativeGames' mkw_xlate Service.cs (MIT).
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx_shader_internal.h"


static const char *const fmt_names[] = {   // img_format codes: Ryujinx TextureFormat enum order
    NULL, "R8Unorm", "R8Snorm", "R8Uint", "R8Sint", "R16Float", "R16Unorm", "R16Snorm", "R16Uint", "R16Sint", "R32Float",
    "R32Uint", "R32Sint", "R8G8Unorm", "R8G8Snorm", "R8G8Uint", "R8G8Sint", "R16G16Float", "R16G16Unorm", "R16G16Snorm",
    "R16G16Uint", "R16G16Sint", "R32G32Float", "R32G32Uint", "R32G32Sint", "R8G8B8A8Unorm", "R8G8B8A8Snorm",
    "R8G8B8A8Uint", "R8G8B8A8Sint", "R16G16B16A16Float", "R16G16B16A16Unorm", "R16G16B16A16Snorm", "R16G16B16A16Uint",
    "R16G16B16A16Sint", "R32G32B32A32Float", "R32G32B32A32Uint", "R32G32B32A32Sint", "R10G10B10A2Unorm",
    "R10G10B10A2Uint", "R11G11B10Float"};
#define COUNT(a) ((int)(sizeof a / sizeof *a))

// `"name":` inside [o, e): the first byte after the colon and its spaces, NULL when absent.
static const char *json_key(const char *o, const char *e, const char *name) {
    char pat[40];
    int n = snprintf(pat, sizeof pat, "\"%s\":", name);
    for (const char *p = o; p + n <= e; p++)
        if (*p == '"' && !memcmp(p, pat, (size_t)n)) {
            p += n;
            while (p < e && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++;
            return p;
        }
    return NULL;
}

static long json_int(const char *o, const char *e, const char *name, long def) {
    const char *p = json_key(o, e, name);
    return p ? strtol(p, NULL, 10) : def;
}

// Copies a string value into buf (empty when absent or not a string).
static void json_str(const char *o, const char *e, const char *name, char *buf, size_t n) {
    const char *p = json_key(o, e, name);
    size_t k = 0;
    if (p && *p == '"')
        for (p++; p < e && *p != '"' && k + 1 < n; p++) buf[k++] = *p;
    buf[k] = 0;
}

// The objects of the flat array `"name": [ {...}, ... ]`: calls fn for each object's [start, end). Returns the count.
static int json_objects(const char *js, const char *name, void (*fn)(GfxShader *, int, const char *, const char *),
                        GfxShader *s, int kind) {
    const char *end = js + strlen(js), *p = json_key(js, end, name);
    if (!p || *p != '[') return 0;
    int n = 0;
    for (p++; p < end && *p != ']'; p++) {
        if (*p != '{') continue;
        const char *q = memchr(p, '}', (size_t)(end - p));
        if (!q) break;
        if (fn) fn(s, kind, p, q);
        n++;
        p = q;
    }
    return n;
}

static int fmt_code(const char *name) {
    for (int i = 1; i < COUNT(fmt_names); i++)
        if (!strcmp(fmt_names[i], name)) return i;
    return 0;
}

// c2 slot of a texture or image handle (Service.cs of MNG's mkw_xlate: combined handles at 0x20, 32 slots; images at
// 0x120; separate textures at 0x168), -1 outside those ranges or for a handle in another constant buffer.
static int handle_slot(int kind, int cbuf_slot, int handle, int separate) {
    if (cbuf_slot >= 0 && cbuf_slot != 2) return -1;
    int bytes = (handle & 0x3fff) * 4;
    if (kind == GFX_RES_IMG) return bytes >= 0x120 && bytes < 0x168 ? (bytes - 0x120) / 8 : -1;
    if (separate) return bytes >= 0x168 && bytes < 0x568 ? (bytes - 0x168) / 8 : -1;
    return bytes >= 0x20 && bytes < 0x120 ? (bytes - 0x20) / 8 : -1;
}

// NVN2 compute programs read their resources from a table the driver packs into c0 for each program: from byte 0x10,
// A uniform buffers and then B storage buffers as {u64 address, u32 size} in 16 bytes, then C texture and D image
// handles in 8 bytes, each kind indexed by its NVN slot (the counts cover slots 0..n-1, unused ones included). The
// counts sit in the program's control section at +0xb1, +0xb2, +0xb3 and +0xb6, the table's end at +0xba (u16):
// measured on 14 of MKW's compute programs against their reflection, 31/10/2026 (A=13, B=10, C=19: textures at
// 0x180-0x210, end 0x218). NVN1 kept fixed tables (storage 0x310, uniform 0x210, handles in bank 2); slots here are
// given in those terms so the replay's lookups (ssbo_slot, handle_slot) stay as they are. Returns 0 when byte is not
// in the table of kind. The push constants come first, control +0xa9 words from 0x10: with them the table ends at +0xba
// in all 728 compute programs, without them in 698 (the other 30 read their tables 0x20 off). nxnative
// docs/reference/shaders.md «Ampere shader code» @ 2a6f9b3 (MIT); also MKW-THWOMP's nvn2_c0.h (GPL, the idea only).
static int cs_res(const GfxShader *s, int kind, int byte, int *slot) {
    int ub = 0x10 + 4 * s->cs_table[4], sb = ub + 16 * s->cs_table[0], tb = sb + 16 * s->cs_table[1], ib = tb + 8 * s->cs_table[2];
    int end = ib + 8 * s->cs_table[3];
    if (kind == GFX_RES_SSBO && byte >= ub && byte < sb) return *slot = (byte - ub) / 16, 2;   // a uniform buffer read as storage
    if (kind == GFX_RES_SSBO && byte >= sb && byte < tb) return *slot = (byte - sb) / 16, 1;
    if (kind == GFX_RES_TEX && byte >= tb && byte < ib) return *slot = (byte - tb) / 8, 1;
    if (kind == GFX_RES_IMG && byte >= ib && byte < end) return *slot = (byte - ib) / 8, 1;
    return 0;
}

static void add_res(GfxShader *s, int kind, const char *o, const char *e) {
    GfxShaderRes *r = &s->res[kind][s->nres[kind]++];
    r->set = 0;
    r->binding = (uint16_t)json_int(o, e, "binding", 0);
    if (kind == GFX_RES_UBO || kind == GFX_RES_SSBO) {
        r->slot = (int16_t)json_int(o, e, "slot", -1);
        r->count = 1;
        r->flags = (uint8_t)json_int(o, e, "flags", 0);   // UBO: 2, the bank's bounded word view (head.py cbNw)
        if (kind == GFX_RES_SSBO) {
            r->cbuf_slot = (int8_t)json_int(o, e, "cb_slot", -1);
            r->handle = (uint16_t)json_int(o, e, "cb_offset", 0);
            int slot, k;
            if (s->cs_known && r->cbuf_slot == 0 && (k = cs_res(s, kind, r->handle * 4, &slot)))
                r->handle = (uint16_t)((k == 2 ? 0x210 : 0x310) + 16 * slot) / 4;   // NVN1's tables (ssbo_slot)
        }
        return;
    }
    char buf[32];
    json_str(o, e, "format", buf, sizeof buf);
    const char *sep = json_key(o, e, "separate");
    r->type = (uint16_t)json_int(o, e, "type", 0);
    r->format = (uint8_t)fmt_code(buf);
    r->flags = (uint8_t)json_int(o, e, "flags", 0);
    r->cbuf_slot = (int8_t)json_int(o, e, "cbuf_slot", -1);
    int handle = (int)json_int(o, e, "handle", 0);
    r->handle = (uint16_t)handle;
    r->count = (uint16_t)json_int(o, e, "array", 1);
    r->separate = sep && *sep == 't';
    r->slot = (int16_t)handle_slot(kind, r->cbuf_slot, handle, r->separate);
    int slot;
    if (s->cs_known && r->cbuf_slot == 0 && cs_res(s, kind, handle * 4, &slot)) r->slot = (int16_t)slot;
}

// SPIR-V opcodes and enumerants used below (SPIR-V 1.6 specification, section 3).
enum { OP_EXECUTION_MODE = 16, OP_TYPE_INT = 21, OP_TYPE_FLOAT = 22, OP_TYPE_VECTOR = 23,
       OP_TYPE_ARRAY = 28, OP_TYPE_POINTER = 32, OP_VARIABLE = 59, OP_DECORATE = 71,
       DEC_LOCATION = 30, DEC_COMPONENT = 31, SC_INPUT = 1, SC_OUTPUT = 3, EM_LOCAL_SIZE = 17 };

// Interface Locations and the compute workgroup size from the module. Ids beyond 2^20 are ignored (Ryujinx modules stay
// far below).
static void reflect_spirv(GfxShader *s) {
    const uint32_t *w = s->spirv;
    uint64_t n = s->words;
    s->local[0] = s->local[1] = s->local[2] = 1;
    if (n < 5) return;
    uint32_t bound = w[3] < (1u << 20) ? w[3] : (1u << 20);
    // per id: location + 1 (0 = none), component flag, scalar/vector description of types, pointee of pointers
    int32_t *loc = calloc(bound, sizeof *loc);
    uint8_t *packed = calloc(bound, 1), *ttype = calloc(bound, 1), *tcomps = calloc(bound, 1);
    uint32_t *pointee = calloc(bound, sizeof *pointee);
    if (!loc || !packed || !ttype || !tcomps || !pointee) goto out;
    for (uint64_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > n) break;
        const uint32_t *a = w + i + 1;
        uint32_t id = len > 1 ? a[0] : 0;
        if (op == OP_EXECUTION_MODE && len >= 6 && a[1] == EM_LOCAL_SIZE) {
            s->local[0] = a[2], s->local[1] = a[3], s->local[2] = a[4];
        } else if (op == OP_DECORATE && len >= 4 && id < bound) {
            if (a[1] == DEC_LOCATION) loc[id] = (int32_t)a[2] + 1;
            else if (a[1] == DEC_COMPONENT) packed[id] = 1;
        } else if (op == OP_TYPE_INT && len >= 4 && id < bound) {
            ttype[id] = a[2] ? GFX_TYPE_SINT : GFX_TYPE_UINT, tcomps[id] = 1;
        } else if (op == OP_TYPE_FLOAT && len >= 3 && id < bound) {
            ttype[id] = GFX_TYPE_FLOAT, tcomps[id] = 1;
        } else if (op == OP_TYPE_VECTOR && len >= 4 && id < bound && a[1] < bound) {
            ttype[id] = ttype[a[1]], tcomps[id] = (uint8_t)a[2];
        } else if (op == OP_TYPE_ARRAY && len >= 4 && id < bound && a[1] < bound) {
            ttype[id] = ttype[a[1]], tcomps[id] = tcomps[a[1]];   // element type (arrayed interface blocks)
        } else if (op == OP_TYPE_POINTER && len >= 4 && id < bound) {
            pointee[id] = a[2];
        } else if (op == OP_VARIABLE && len >= 4) {
            uint32_t type = a[0], var = a[1], sc = a[2];
            if ((sc == SC_INPUT || sc == SC_OUTPUT) && var < bound && type < bound && loc[var] > 0 && loc[var] <= 32) {
                int l = loc[var] - 1;
                uint32_t t = pointee[type] < bound ? pointee[type] : 0;
                GfxShaderIo io = {ttype[t], tcomps[t] ? tcomps[t] : 4, packed[var]};
                if (sc == SC_INPUT) s->in_mask |= 1u << l, s->in[l] = io;
                else s->out_mask |= 1u << l, s->out[l] = io;
            }
        }
        i += len;
    }
out:
    free(loc), free(packed), free(ttype), free(tcomps), free(pointee);
}

// "spec": [[bank, byte], ...], the fixed-address constant words in specialization order (translator head.py). Two
// passes over the list: count the pairs, then fill.
static int spec_pairs(const char *p, const char *end, uint32_t *out) {
    int depth = 0, n = 0, nv = 0;
    long v[2] = {0, 0};
    for (p++; p < end; p++) {
        if (*p == '[') {
            depth++, nv = 0;
        } else if (*p == ']') {
            if (!depth--) break;
            if (nv == 2 && out) out[n] = (uint32_t)(v[0] << 16 | (v[1] & 0xffff));
            n += nv == 2;
        } else if (depth && *p >= '0' && *p <= '9') {
            char *q;
            long x = strtol(p, &q, 10);
            if (nv < 2) v[nv] = x;
            nv++;
            p = q - 1;
        }
    }
    return n;
}

static void reflect_spec(GfxShader *s) {
    const char *end = s->reflection + strlen(s->reflection), *p = json_key(s->reflection, end, "spec");
    if (!p || *p != '[') return;
    int n = spec_pairs(p, end, NULL);
    if (!n) return;
    s->spec = malloc(sizeof *s->spec * (size_t)n);
    if (!s->spec) rt_fatal("gfx_shader: out of memory");
    s->nspec = spec_pairs(p, end, s->spec);
}

void gfx_shader_reflect(GfxShader *s) {
    static const char *const kinds[GFX_RES_KINDS] = {"cbufs", "sbufs", "textures", "images"};
    const char *js = s->reflection;
    char stage[8];
    json_str(js, js + strlen(js), "stage", stage, sizeof stage);
    s->stage = !strcmp(stage, "cs") ? GFX_STAGE_CS : !strcmp(stage, "ps") ? GFX_STAGE_FS : GFX_STAGE_VS;
    s->mesh_kind = !strcmp(stage, "task") ? GFX_MESH_TASK : !strcmp(stage, "mesh") ? GFX_MESH_MESH : GFX_MESH_NONE;
    char t[32];
    unsigned c[5];
    json_str(js, js + strlen(js), "cs_table", t, sizeof t);   // translate.py: the control's counts, for a load by key
    if (s->stage == GFX_STAGE_CS && !s->cs_known && sscanf(t, "%u,%u,%u,%u,%u", &c[0], &c[1], &c[2], &c[3], &c[4]) == 5) {
        for (int i = 0; i < 5; i++) s->cs_table[i] = (uint8_t)c[i];
        s->cs_known = 1;
    }
    for (int k = 0; k < GFX_RES_KINDS; k++) {
        int count = json_objects(js, kinds[k], NULL, s, k);
        s->res[k] = calloc(count ? count : 1, sizeof *s->res[k]);
        json_objects(js, kinds[k], add_res, s, k);
    }
    reflect_spec(s);
    s->first_store = s->stage == GFX_STAGE_CS ? (int)json_int(js, js + strlen(js), "first_store", 0) : 0;
    s->int_inputs = (uint32_t)json_int(js, js + strlen(js), "int_inputs", 0);
    s->raw_outputs = (uint32_t)json_int(js, js + strlen(js), "raw_outputs", 0);
    reflect_spirv(s);
}

