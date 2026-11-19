# runtime/gfx/vk/mem

Game memory as the GPU sees it: NVN pools as Vulkan buffers, shadows and mirrors, uploads, write-backs and the
submit's copies of constant blocks.

| File | What it is |
|---|---|
| `vk_memory.c`, `vk_mem_internal.h` | NVN pools as Vulkan buffers: pool index, page table of virtual pools and address resolution; the header is what the `vk_mem_*.c` parts share |
| `vk_mem_back.c` | GPU storage of each pool: placed, imported, physical in chunks, lazy |
| `vk_mem_gen.c` | Write generations per page (all writes, and the CPU's alone: a GPU write reported at its fence stamps only the first, so the mirror, which holds those bytes, skips it), write-watch snapshots and frozen regions |
| `vk_mem_shadow.c` | Shadow copies of the CPU-visible pools, synced on their own thread as the replay touches them (`MKW_SHADOW_EARLY`) |
| `vk_mem_shadow_nc.c` | The shadow's memory type: cached, and where the driver has no cached coherent type (ANV on Meteor Lake, #283) not coherent, with explicit flush and invalidate around the CPU's copies (`MKW_SHADOW_CACHED`) |
| `vk_mem_wback.c` | GPU writes to shadowed pools: pending from recording and written back to game memory at their fence; `MKW_WB_ORDER`: a part that rewrites a page with its write-back pending waits on the GPU for that write-back |
| `vk_mem_merge.c` | Byte-wise merge of the shadow in the ranges the GPU may write (`MKW_SHADOW_MERGE`): the CPU's bytes reach the GPU and the write-back does not overwrite them |
| `vk_mem_capture.c` | Measurement `MKW_SHADOW_SUBMIT`: what the CPU writes, snapshotted at each submit and applied to the shadow when that submit syncs (discarded) |
| `vk_wb_par.c` | `MKW_WB_PAR=N` (default 0; `ab`): the fence's write-backs on N threads besides the completion thread: a callback's ranges shared out, and a range of 512 KiB or more in 128 KiB slices |
| `vk_mem_lazy.c` | Lazy merge bases (`MKW_LAZY_BASE`): instead of copying the shadow of a large range, its game pages stay read-only and are copied only if written; `MKW_LAZY_BASE_CHECK=1` compares with the usual copy |
| `vk_mem_lost.c` | Diagnostic `MKW_SHADOW_LOST`: CPU bytes the shadow loses per frame and indirect arguments the GPU does not see as the CPU wrote them |
| `vk_mem_mirror.c` | VRAM mirror of the pools used as buffers (dedicated GPU) and its `verify` mode; `MKW_MIRROR_CPU_GEN` (default 1) syncs by the CPU stamps only, `MKW_MIRROR_REUP=1` counts the pages uploaded again after only the GPU wrote them |
| `vk_mem_race.c` | Shadow diagnostics: `MKW_SHADOW_RACE` (copies over bytes an in-flight submit reads) and `MKW_SHADOW_WATCH=lo-hi[,...]` (every change, skip and write-back over those ranges); with `MKW_FENCE_EARLY`, those copies wait for the submit's GPU work (`race_wait`) |
| `vk_mem_dirty.c` | Dirty page census (`MKW_DIRTY_CENSUS` with `MKW_TRACE=perf`) |
| `vk_mem_cap.c` | For captures (`../cap/`): the chunks of physical pools, which only the GPU writes, read when capturing and written when replaying |
| `vk_mem_log.c` | Census of pools and traffic, VRAM summary, device class and memory mode (`MKW_MEMORY`); the mirror and shadow summaries also once at a capture replay's end |
| `vk_mem_spill.c` | `MKW_MEM_SPILL` (on by default on Linux; 0 off): a thread that every 5 s spills to a file the pages of pools without CPU access the GPU has never resolved (`core/memory/memory_spill.c`); they come back on first touch; `MKW_MEM_SPILL_ANY=1` (diagnosis) spills every lazy pool, CPU-accessible ones too |
| `vk_mem_pooldiag.c` | Diagnostic `MKW_MEM_POOL_LABELS`: every 300 frames, per kind of pool, its shadow copies against the game's pages in RAM, and the runtime's reads of CPU_NO_ACCESS pool storage (shadow fills, texture uploads, write-backs) |
| `vk_mem_upload.c`, `vk_headroom.c` | Upload batches, staging, deferred destruction and `vk_mem_relieve` (what to release when an allocation does not fit); new VMA blocks on another thread |
| `vk_mem_uptime.c` | Diagnostic `MKW_UPLOAD_TIME`: GPU time of each upload batch, with what it carried |
| `vk_usnap.c` | Constant blocks copied when recording of the submit starts, so the game can rewrite them afterwards |
| `vk_usnap_take.c`, `vk_usnap_int.h` | `MKW_USNAP_AT_SUBMIT`: that copy taken at submit, on the game thread, and handed to the replay in order |
