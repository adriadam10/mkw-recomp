// The integer variants of a prepared shader (gfx_shader.c): a vertex shader whose attributes have integer formats,
// a fragment shader that writes integer colour targets (gfx_spirv_io.c) and a compute shader whose bound views are
// integer images (gfx_spirv_img.c). A variant enters the memory cache under the program key with the masks folded
// into its last bytes, and keeps the program key as its input id: pipeline key records name the program.
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "gfx_shader_internal.h"
#include "host.h"
#include "platform/platform.h"


// The variant of s with the Input (out 0) or Output (out 1) Locations in um / sm as uint / int vectors, or s.
static const GfxShader *int_io_variant(GfxShader *s, uint32_t um, uint32_t sm, int out) {
    if (!um && !sm) return s;
    uint8_t key[32];
    memcpy(key, s->key, 32);
    // variants: the program key with the masks folded into its last bytes, other marks for outputs
    key[30] ^= out ? 0x69 : 0x5a, key[31] ^= out ? 0x96 : 0xa5;
    for (int i = 0; i < 4; i++) key[22 + i] ^= (uint8_t)(um >> 8 * i), key[26 + i] ^= (uint8_t)(sm >> 8 * i);
    GfxShader *v = gfx_shader_find(key);
    if (v) return v;
    uint64_t nw = 0;
    uint32_t *code = spirv_int_io(gfx_shader_spirv(s, NULL), s->words, um, sm, out ? 3 : 1, &nw);
    if (!code) {
        static volatile int32_t n_fail;
        if (plat_inc32(&n_fail) <= 8)
            rt_log("gfx_shader: integer %s %#x/%#x not patched in", out ? "colour outputs" : "vertex inputs", um, sm);
        return s;
    }
    if (!(v = calloc(1, sizeof *v)) || !(v->reflection = malloc(s->reflection_size + 1))) rt_fatal("gfx_shader: out of memory");
    memcpy(v->key, key, 32), memcpy(v->iid, s->iid, 32);
    v->spirv = code, v->words = nw;
    memcpy(v->reflection, s->reflection, s->reflection_size), v->reflection[s->reflection_size] = 0;
    v->reflection_size = s->reflection_size;
    gfx_shader_reflect(v);
    for (int l = 0; l < 32; l++)
        if ((um | sm) >> l & 1) (out ? v->out : v->in)[l].type = um >> l & 1 ? GFX_TYPE_UINT : GFX_TYPE_SINT;
    const char *dd = cfg_get("MKW_SPIRV_INT_DUMP");   // the variants for spirv-val
    if (dd && *dd) {
        char path[PATH_CAP + 96], hex[65];
        gfx_shader_key_hex(key, hex);
        plat_file_mkdir_p(dd);
        snprintf(path, sizeof path, "%s/%s.spv", dd, hex);
        plat_file_write(path, code, nw * 4);
    }
    static volatile int32_t n_var[2];   // apart: the vertex ones would use up the fragment ones' lines
    if (plat_inc32(&n_var[out]) <= 8)
        rt_log("gfx_shader: %s uint %#x sint %#x", out ? "fragment shader variant with integer outputs"
                                                       : "vertex shader variant with integer inputs", um, sm);
    return gfx_shader_insert(v);
}

// The variant of a prepared vertex shader for the integer attribute formats in a (GfxShaderAnswers.attr_type), or s.
static const GfxShader *int_input_variant(GfxShader *s, const GfxShaderAnswers *a) {
    if (!s || !a || s->stage != GFX_STAGE_VS || s->mesh_kind != GFX_MESH_NONE) return s;
    uint32_t um = 0, sm = 0;
    for (int l = 0; l < 16; l++)
        if (s->in_mask >> l & 1) {
            if (a->attr_type[l] == GFX_TYPE_UINT) um |= 1u << l;
            else if (a->attr_type[l] == GFX_TYPE_SINT) sm |= 1u << l;
        }
    return int_io_variant(s, um, sm, 0);
}

// The variant of a prepared fragment shader for the integer colour targets in a (GfxShaderAnswers.out_type), or s:
// only the outputs the shader writes (the others are masked off, vk_pipeline.c target_wmask).
static const GfxShader *int_output_variant(GfxShader *s, const GfxShaderAnswers *a) {
    if (!s || !a || s->stage != GFX_STAGE_FS) return s;
    uint32_t um = 0, sm = 0;
    for (int l = 0; l < 8; l++)
        if (s->out_mask >> l & 1) {
            if (a->out_type[l] == GFX_TYPE_UINT) um |= 1u << l;
            else if (a->out_type[l] == GFX_TYPE_SINT) sm |= 1u << l;
        }
    return int_io_variant(s, um, sm, 1);
}

static const GfxShader *int_image_variant(GfxShader *s, const GfxShaderAnswers *a);

// The integer variant a draw or dispatch's answers ask of s (none for the other stages).
static const GfxShader *int_variant(GfxShader *s, const GfxShaderAnswers *a) {
    if (!s) return s;
    return s->stage == GFX_STAGE_CS ? int_image_variant(s, a)
         : s->stage == GFX_STAGE_FS ? int_output_variant(s, a)
                                    : int_input_variant(s, a);
}

// The variant of a prepared compute shader for the integer-format views in a (GfxShaderAnswers.img_uint/img_sint), or
// s (gfx_spirv_img.c).
static const GfxShader *int_image_variant(GfxShader *s, const GfxShaderAnswers *a) {
    if (!s || !a || s->stage != GFX_STAGE_CS || !(a->img_uint | a->img_sint)) return s;
    uint8_t key[32];
    memcpy(key, s->key, 32);
    key[30] ^= 0x3c, key[31] ^= 0xc3;   // as int_input_variant's, with other marks
    for (int i = 0; i < 8; i++) key[i + 14] ^= (uint8_t)(a->img_uint >> 8 * i), key[i + 6] ^= (uint8_t)(a->img_sint >> 8 * i);
    GfxShader *v = gfx_shader_find(key);
    if (v) return v;
    uint64_t nw = 0;
    const char *why = NULL;
    uint32_t *code = spirv_int_images(gfx_shader_spirv(s, NULL), s->words, a->img_uint, a->img_sint, &nw, &why);
    if (!code) {
        static volatile int32_t n_fail;
        if (plat_inc32(&n_fail) <= 8) {
            char hex[65];
            gfx_shader_key_hex(s->key, hex);
            rt_log("gfx_shader: integer images %#llx/%#llx not patched in program %s: %s; using original shader",
                   (unsigned long long)a->img_uint, (unsigned long long)a->img_sint, hex, why ? why : "unknown reason");
            // Keep the failing input too: successful variants alone cannot reproduce a failed conversion offline.
            const char *dd = cfg_get("MKW_SPIRV_INT_DUMP");
            if (dd && *dd) {
                char path[PATH_CAP + 128];
                plat_file_mkdir_p(dd);
                snprintf(path, sizeof path, "%s/%s-u%llx-s%llx.failed.spv", dd, hex,
                         (unsigned long long)a->img_uint, (unsigned long long)a->img_sint);
                plat_file_write(path, gfx_shader_spirv(s, NULL), s->words * 4);
            }
        }
        return s;
    }
    if (!(v = calloc(1, sizeof *v)) || !(v->reflection = malloc(s->reflection_size + 1))) rt_fatal("gfx_shader: out of memory");
    memcpy(v->key, key, 32), memcpy(v->iid, s->iid, 32);
    v->spirv = code, v->words = nw;
    memcpy(v->reflection, s->reflection, s->reflection_size), v->reflection[s->reflection_size] = 0;
    v->reflection_size = s->reflection_size;
    memcpy(v->cs_table, s->cs_table, sizeof v->cs_table), v->cs_known = s->cs_known;   // reflect places the resources by it
    gfx_shader_reflect(v);
    const char *dd = cfg_get("MKW_SPIRV_INT_DUMP");   // the variants for spirv-val
    if (dd && *dd) {
        char path[PATH_CAP + 96], hex[65];
        gfx_shader_key_hex(key, hex);
        plat_file_mkdir_p(dd);
        snprintf(path, sizeof path, "%s/%s.spv", dd, hex);
        plat_file_write(path, code, nw * 4);
    }
    static volatile int32_t n_var;
    if (plat_inc32(&n_var) <= 8)
        rt_log("gfx_shader: compute shader variant with integer images uint %#llx sint %#llx", (unsigned long long)a->img_uint,
               (unsigned long long)a->img_sint);
    return gfx_shader_insert(v);
}

const GfxShader *gfx_shader_int_variant(const GfxShader *s, const GfxShaderAnswers *a) {
    return int_variant((GfxShader *)s, a);
}
