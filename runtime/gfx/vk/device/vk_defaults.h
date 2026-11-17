// The memory caps a player gets with nothing set, from their machine. Pure functions (no Vulkan), so
// tests/runtime/test_defaults.py builds them alone and checks them on the GPUs players have reported.
#ifndef VK_DEFAULTS_H
#define VK_DEFAULTS_H

#include <stdint.h>

uint64_t vk_default_sparse_mib(int discrete, uint64_t vram_mib);        // MKW_SPARSE_MIB unset
uint64_t vk_default_mem_limit_mib(uint64_t ram_mib, uint64_t carveout_mib);   // MKW_GPU_MEM_LIMIT unset, integrated
uint32_t vk_default_tex_pool_blocks(int discrete, uint64_t vram_mib);  // MKW_VK_TEX_POOL_MIN unset

#endif
