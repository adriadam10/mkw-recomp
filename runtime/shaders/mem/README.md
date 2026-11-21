# runtime/shaders/mem

Game memory on the GPU (`gfx/vk/mem/`).

| File | What it does |
|---|---|
| `mirror_merge.comp` | The VRAM mirror's write-back merged by bytes (`vk_mem_mmerge.c`, `MKW_MIRROR_MERGE`): only the bytes the GPU changed against a base copy reach the pool, so the CPU's writes to the rest of a range bound writable are not reverted; one dispatch for every range of a submission, the pools bound as arrays |
| `mirror_base.comp` | Its bases: in a part's prologue, the mirror words of the ranges the part writes, copied into a base chunk before it runs |
