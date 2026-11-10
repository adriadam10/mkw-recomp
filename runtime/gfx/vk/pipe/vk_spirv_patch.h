// SPIR-V rewrites of the translated shaders that the pipeline builder (vk_pipeline.c) applies before creating a module:
// binding and set moves for the graphics pipeline library, dual-source blending, outputs a stage lacks, and the fixups
// every module gets. Each returns a malloc'd copy (NULL: unchanged or not in the expected layout).
#pragma once

#include <stdint.h>

#include "gfx.h"

enum { SPV_OP_ENTRY_POINT = 15, SPV_OP_TYPE_INT = 21, SPV_OP_TYPE_FLOAT = 22, SPV_OP_TYPE_VECTOR = 23,
       SPV_OP_TYPE_POINTER = 32, SPV_OP_CONSTANT = 43, SPV_OP_CONSTANT_COMPOSITE = 44,
       SPV_OP_CONSTANT_NULL = 46, SPV_OP_FUNCTION = 54, SPV_OP_VARIABLE = 59,
       SPV_OP_DECORATE = 71, SPV_DEC_BUILTIN = 11, SPV_DEC_LOCATION = 30, SPV_DEC_INDEX = 32, SPV_DEC_BINDING = 33, SPV_DEC_DESCRIPTOR_SET = 34,
       SPV_SC_OUTPUT = 3 };

uint32_t *spirv_rebind(const uint32_t *w, uint64_t n, uint32_t off);
uint32_t *spirv_set(const uint32_t *w, uint64_t n, uint32_t set);
uint32_t *spirv_dual_source(const uint32_t *w, uint64_t n, uint64_t *out_n);
uint32_t *spirv_add_outputs(const uint32_t *w, uint64_t n, uint32_t missing, const GfxShaderIo io[32],
                            uint64_t *out_n);
uint32_t *spirv_alpha_test(const uint32_t *w, uint64_t n, int func, float ref, uint64_t *out_n);
uint32_t *spirv_fixup(const uint32_t *w, uint64_t n, uint64_t *out_n);
uint32_t *spirv_clamp_shared(const uint32_t *w, uint64_t n, uint32_t limit, uint64_t *out_n, uint32_t *from,
                             uint32_t *to);
int uses_subgroup_ops(const uint32_t *w, uint64_t words);
