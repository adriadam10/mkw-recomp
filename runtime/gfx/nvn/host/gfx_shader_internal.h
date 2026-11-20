// The prepared shader as gfx_shader.c keeps it, shared with its reflection (gfx_shader_refl.c).
#ifndef GFX_SHADER_INTERNAL_H
#define GFX_SHADER_INTERNAL_H
#include <stdint.h>

#include "gfx.h"

struct GfxShader {
    struct GfxShader *next;
    uint8_t key[32];
    uint8_t iid[32];                                       // the id in pipeline key records: the program key
    uint32_t *spirv;                                       // read it through gfx_shader_spirv: it may be released
    uint64_t words;
    volatile int32_t spirv_state;                          // SPIRV_*: whether spirv may be released after the prewarm
    char *reflection;                                      // the service's .res.json text, parsed into the fields below
    uint64_t reflection_size;
    // Reflection (reflect(), once per shader before it enters the cache)
    int stage;                                             // GFX_STAGE_*
    int mesh_kind;                                         // GFX_MESH_*: task and mesh programs count as GFX_STAGE_VS
    uint8_t cs_table[5];   // NVN2 compute c0 table: uniform, storage, texture, image counts; push constant words before
                                                           // entries (control +0xb1/+0xb2/+0xb3/+0xb6); see cs_res
    int cs_known;                                          // cs_table read from the program's control
    GfxShaderRes *res[GFX_RES_KINDS];
    int nres[GFX_RES_KINDS];
    uint32_t in_mask, out_mask;                            // user Locations of Input / Output variables
    GfxShaderIo in[32], out[32];
    uint32_t local[3];
    uint32_t *spec;                                        // "spec": word k = bank << 16 | byte (specialization ids 2k, 2k+1)
    int nspec;
    int first_store;                                       // "first_store" (glsl/first_store.py): 0, 1 or 2
    uint32_t int_inputs;                                   // "int_inputs" (glsl/int_hints.py): Locations used as integers
    uint32_t raw_outputs;                                  // "raw_outputs": outputs that only move flat inputs' bits
    const struct GfxShader *variant_of;                    // gfx_shader_variant: the generic shader; NULL for one
    struct GfxShader *cs_variant;                          // task/mesh: the compute form (gfx_shader_cs_variant)
    int cs_of_mesh;                                        // this is the compute form of a task or mesh program
    uint64_t spirv_fnv;   // gfx_shader_spirv_fnv, 0 until first asked for; last: gfx_shader_variant copies up to it
};

#define PATH_CAP 1024                                      // bytes of a UTF-8 path

// GfxShader.spirv_state. A prepared shader read from its file starts PREPARED; the first read of its SPIR-V outside
// the prewarm keeps it, and gfx_shader_spirv_release frees the rest, read again from the file if a draw asks later.
enum { SPIRV_KEPT, SPIRV_PREPARED, SPIRV_RELEASED };

// Parses s->reflection (.res.json) and scans s->spirv into the reflection fields, once before s enters the cache.
void gfx_shader_reflect(GfxShader *s);

// The memory cache by key (gfx_shader.c): find, and insert s or free it and return the entry another thread put first.
GfxShader *gfx_shader_find(const uint8_t key[32]);
GfxShader *gfx_shader_insert(GfxShader *s);
void gfx_shader_key_hex(const uint8_t key[32], char out[65]);

// spirv/shaders.pack (gfx_shader_pack.c): opened once with the prepared folder; a program's file of kind 0 .spv,
// 1 .res.json, 2 .cs.spv. read returns 0 when the loose file is to be read instead.
void gfx_shader_pack_open(const char *dir);
int gfx_shader_pack_read(const uint8_t key[32], int kind, uint8_t **out, uint64_t *size);
size_t gfx_shader_pack_keys(uint8_t (**keys)[32]);   // a malloc'd copy of the pack's keys; 0 (and NULL) without one
#endif
