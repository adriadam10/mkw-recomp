# runtime/gfx/vk/tex

Textures: a cache of NVN images as Vulkan images, uploads with deswizzle and ASTC, descriptor slots and samplers,
sparse and precomputed BC7.

| File | What it is |
|---|---|
| `vk_texture.c`, `vk_tex_internal.h`, `vk_formats.c` | Image cache: its state, keys and statistics (`MKW_TEX_TIMING`: a frame's texture creation and upload time by part, and the lead from each texture's registration to its first upload); the header is what the `vk_tex_*.c` parts share; NVN to Vulkan format table |
| `vk_tex_image.c` | Image creation, with cube compatibility only for NVN cube targets, and the preallocated texture pool, joined and freed before allocator shutdown |
| `vk_tex_gfx9.c`, `vk_tex_gfx9.h` | The GFX9 RADV decision on block-view COLOR_ATTACHMENT, free of Vulkan so `tests/runtime/test_gfx9_block_ca.py` builds it alone |
| `vk_tex_pressure.c` | Eviction under VRAM pressure (`MKW_TEX_PRESSURE`, off unless set: no gain measured on a GTX 1050 Ti): past 90 % of the live budget in VMA's blocks, the recreatable textures idle the longest go until the allocations are at 80 %; with the driver's usage over the budget, every idle texture that spilled into a host heap at the `MKW_GPU_MEM_LIMIT` cap goes too, as NVIDIA's driver keeps those in VRAM all the same (on Windows an allocation rarely fails: past the budget the driver moves memory out of the GPU and the game slows down); `MKW_TEX_RACE_PURGE` (off unless set): out of a race, every recreatable texture once, then the idle ones every 20 s, so the texture pool's blocks can empty and go back |
| `vk_tex_evict.c` | Image destruction, freed entries (kept up to a cap; past it, those a later image was written over go: `MKW_TEX_OVERWRITTEN`), eviction under memory pressure and the list of textures the GPU wrote |
| `vk_tex_upload.c` | Uploads with deswizzle and ASTC, and the check of whether the storage changed |
| `vk_tex_prefetch.c` | Texture prefetch (`MKW_TEX_PREFETCH`, off unless set): a texture the game registers is made and uploaded on a thread of its own ahead of its first draw (300 ms after its registration, one upload batch of its work on the GPU at a time, while VRAM is under 80 % of its budget); its first use re-uploads it if the storage was written since. The course flyover's worst frame ~125 -> ~70 ms on an RX 6700 XT, for up to ~2.7 GB more VRAM |
| `vk_tex_alias.c` | Storage aliases (`MKW_TEX_ALIAS`) and GPU writes to mirrored textures |
| `vk_tex_slots.c` | Texture and sampler descriptor slots, and the VkSamplers |
| `vk_tex_twin.c` | Depth twins: a D32_SFLOAT copy of an R32_SFLOAT texture a shader samples with a depth compare (Vulkan compares only on depth formats), refreshed by each upload of its entry |
| `vk_tex_texel.c` | ReadTexels / WriteTexels |
| `vk_tex_wb.c` | CPU reads of what the GPU wrote: mirrored textures written back to their storage (at an invalidate, memcpy or ReadTexels, and the small ones at their fence) |
| `vk_astc.c` | ASTC on the GPU: decoded by compute and recompressed to BC7 (`MKW_ASTC_BC7`) |
| `vk_sparse.c` | MKW's own: sparse textures as Vulkan sparse images, tiles bound at each `MapVirtual`; cube compatibility only for cube targets; private small-mip backing initialized separately from shared tiles and sparse resources released before allocator shutdown |
| `vk_texpre.c` | MKW's own: BC7 of the ASTC textures encoded offline (`MKW_TEXPRE`, off), and the GPU encoder's BC7/BC1 blocks kept from an earlier run (`MKW_TEXPRE_WRITEBACK`), read on threads of their own (`MKW_TEXPRE_ASYNC`) |
