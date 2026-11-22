# runtime/gfx/vk/exec

Replay of each submit (operations recorded by the front end, `../../nvn/`) into a command buffer: passes, copies, draws
and dispatches, descriptors, and their parallel recording.

| File | What it is |
|---|---|
| `vk_exec.h`, `vk_exec.c`, `vk_exec_internal.h` | Replay of a submit: state, passes and barriers, and the operation loop; the internal header is what it shares with `vk_exec_rt.c` and `vk_exec_clear.c` |
| `vk_exec_rt.c` | Each submit's render targets, their views (freed with the submit) and which targets the last frames drew |
| `vk_exec_clear.c` | Colour and depth-stencil clears, as an image clear or inside a pass; layers cleared ahead (`MKW_DEPTH_CLEAR_ALL`) |
| `vk_exec_draw.c`, `vk_draw_internal.h` | Draws: program to shaders, feedback loops and the setup of each draw (pass, pipeline, state); shadow-map draws left out with 3-4 split-screen views (`MKW_SHADOW_DRAWS=auto`); the header is what the `vk_exec_*` parts of the draw replay share |
| `vk_exec_ops.c` | Draw, dispatch and bind operations (`vk_exec_op_draw`, `vk_exec_bind_state`) |
| `vk_exec_passhz.c`, `vk_passhz.h` | `MKW_PASS_HAZARDS` (default 0; `1`, `ab`, `census`): no full barrier before a pass or an image clear independent of what came since the last one; a draw that samples what was rendered since, or stores anywhere, ends the pass and begins it behind the barrier |
| `vk_exec_xbind.c` | `MKW_BIND_XF=check\|use` (a measurement for the draw cache): a draw's descriptors kept across frames by their inputs; `check` counts hits, `use` gives the ceiling of the saving |
| `vk_exec_bind.c` | Descriptor sets of each draw or dispatch, with their memos and skipping of repeats (`MKW_BIND_SKIP`) |
| `vk_exec_cb.c` | About the command buffer being recorded: transient memory, descriptor pools and ranges the GPU writes, and fixed state, vertex buffers and feedback barriers |
| `vk_exec_written.c` | `MKW_REPLAY_PAR_WSHARE`: the ranges the GPU writes in a submission recorded in chunks, each reported pending and written back by one thread only |
| `vk_exec_view.c` | Stand-ins for what is not bound (zeros, 1x1 image, sampler) and image views of textures |
| `vk_exec_ubuf.c` | Constant bank c0, the submit's UBO updates and addresses read from uniform blocks |
| `vk_exec_dstat.c` | Draw replay diagnostics: at a device loss, the driver's fault report (`VK_EXT_device_fault`: address and kind on RADV) and the last programs, `MKW_IND_TRACE`, `MKW_BIND_STATS`, time per stage |
| `vk_exec_dcache.c` | `MKW_DRAW_CACHE`: descriptor sets kept across frames by their contents, with dynamic-offset UBOs; and the probe that measured its hit rate (`MKW_DRAW_CACHE_PROBE`) |
| `vk_exec_reuse.c`, `vk_reuse.h` | `MKW_REUSE=1\|check\|ab` (with `MKW_DRAW_CACHE=1`): a draw equal to one of an earlier submit (program, order, fixed state and its inputs) takes its stored packets instead of being redone; `check` redoes them too and compares |
| `vk_exec_xfer.c` | Copies, texture clears, events, counters and conditional rendering |
| `vk_exec_par.c` | `MKW_REPLAY_PAR`: a submit recorded in chunks on several threads after a light walk that tracks the state (`MKW_REPLAY_WALK` measures it) |
| `vk_mesh_cs.c` | Task/mesh as compute without mesh shaders: VRAM buffers per submit, compaction pipeline and whether a draw takes that path (`MKW_MESH_COMPUTE`) |
| `vk_ind_read.c` | Diagnostics `MKW_IND_READ` (indirect arguments the GPU used, copied at the end of each pass) and `MKW_IND_BARRIER` (their barriers without copies) |
| `vk_temporal.h`, `vk_temporal.c`, `vk_tmp_ffx.cpp` | `MKW_TEMPORAL=dlss\|fsr3`: NVIDIA DLSS (NGX from the driver) or AMD FSR 3.1 (FidelityFX API DLLs, `vk_tmp_ffx.cpp`) in the place of the game's upscale from its 3D resolution to its screen's: jitter, camera vectors (`shaders/temporal/tmp_mv.comp`), upscale. FSR generation keeps the official provider and substitutes the embedded source-built game-vector kernel only for its SHA-256-matched 3.1.6 shader: verified stationary surfaces win equal-depth vector ties. `tools/ffx/builder/build.py` reproduces the kernel; `MKW_FFX_STILL_TIE=0` disables it for comparison. Explicit `MKW_FFX_FG_LIBRARY` selects a diagnostic generation provider independently of the upscaler |
