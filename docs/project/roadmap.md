# Roadmap

What is being worked on, in order of priority: nothing may hang the GPU, then the picture as on the console (menus
before races), then the frame rate, then the player's side. Updated 17/11/2026.

## 1. 60 fps in races: the serial frame

A race frame takes 31-33 ms\* on the Radeon 660M at 720p, against the 16.7 ms the game asks for. The time does not go
to one slow piece but to a chain where each step waits for the previous one (measured with `MKW_TRACE_TL`):

| Step | Time\* |
|---|---|
| The game runs its frame, from the release of the previous frame's fence to its last submit | ~3.4 ms |
| The Vulkan back end records that submit into command buffers, on the replay thread | ~19 ms |
| The GPU runs what was submitted last | ~9.7 ms |
| Completion work before the game is released for the next frame | ~1.5 ms |

The game's big submit only reaches the back end at the end of its frame, and the GPU cannot start until it is
recorded, so recording is on the critical path: every millisecond saved there is a millisecond off the frame, down to
~21 ms, where the GPU (busy ~20 ms) takes over. Spreading the same CPU work over more threads does not help by
itself; recording with less CPU does. Already done: recordings reused across frames (−4.3 ms), recording overlapped
with the game (−2.4 ms), submits on their own thread (−1.2 to −1.6 ms), the shadow copies off the replay thread
(−0.8 ms), the replay thread on its own core (−1.1 ms), dynamic state recorded only when it changes (idea from
totk-pc-recomp, MIT; −10.6 % recorder instructions on Steam Deck; no measurable frame-time change). Next:

- **AMD GPU clocks on Linux** (done, `MKW_GPU_CLOCKS`): amdgpu judges a GPU's clock by how busy it is, and this
  chain keeps it half idle, so it stayed at its lowest clock (500 of 2855 MHz on an RX 6700 XT, 2560x1440: races at
  23-42 ms a frame). Held at the peak stable pstate: 16.7-17.2 ms, -15.1 ms a frame (medians, in-run A/B and its
  inverted twin, 17/11/2026). Integrated GPUs (Steam Deck, the 660M) are left to amdgpu: on the 660M the peak made the
  race 1.9 ms a frame slower, its GPU faster but the CPU, which shares the chip's power, slower (in-run A/B, 17/11/2026).
  It also explains #496 (`MKW_QUEUE_WAITS` costing 60 → 50 fps on a 7900 XT): +15.3 ms a frame with amdgpu's clock,
  +0.01 ms with it held.
- **Less work per draw**: descriptor sets kept across frames by their contents (`MKW_DRAW_CACHE`). On a fast CPU
  (i7-14700F, RADV) it changes nothing measurable: a race capture's recording thread ran the same instructions with
  it on and off, and its binds made ~20 % more descriptor work (sets are allocated) (16/11/2026); left for slower CPUs.
- **Recording in parallel**: chunks of a submit on several threads; today they fight over the texture lock, which has
  to be split first.
- **The kernel's global lock**: waits by address are done (−1.3 ms\*); the rest of the lock remains.
- **Hybrid CPUs** (`MKW_FAST_CORES`, #490 by @AngeloAscheri): on Linux the frame's threads stay on the fast cores
  and background work (pipeline builds, prewarming, video) on all of them; on an i7-14700F at a fixed 60 fps, races
  went from 12 of 34 windows over 17.5 ms to none (in-run A/B and its inverted twin, 17/11/2026). Still to do:
  - **Windows**: off by default, never measured there. `SetProcessAffinityMask` bounds every thread, so the
    background threads would lose the efficiency cores too (`plat_thread_background` does nothing there); it needs a
    per-thread placement (CPU Sets, or each frame thread's own mask) and then the same A/B on a hybrid CPU.
  - **Few P-cores**: on by default only with 8 fast processors left besides the replay core, a hypothesis; no CPU
    with 2-4 P-cores (i5-1235U, Lunar Lake, Ryzen AI 9 HX 370) has been measured either way.
  - `vk submit` and `vk present` started on the replay thread after it pinned itself and shared its processor; now
    with the frame's threads (`MKW_SUBMIT_APART`): a part waits 11-13 us for its `vkQueueSubmit2` instead of 34-39,
    and the replay thread sends 1.5-2.3 % of the parts itself instead of 9-10 % (in-run A/B and its inverted twin,
    i7-14700F, 18/11/2026). The frame the same there, at the 60 fps cap; a CPU-bound machine (the 660M) to measure.

  A warning for whoever measures this (16/11/2026): every race up to 17:31 took 38-41 ms a frame and every one from
  17:33 on 19-22 ms, with no change to the code; that was the GPU clock (the item above), and comparing a `taskset`
  run of one period with an unpinned run of the other first gave a 2x effect that was not there. Compare inside one
  run (`MKW_AB`).
- **The game thread**: the generated C at `-O1`, with profile-guided optimization next (12-15 % in totk-pc-recomp).

\* Our numbers (not the ones said to come from totk-pc-recomp) were measured on 07-09/11/2026 on our test machine:
Radeon 660M APU (Ryzen 5 7535HS), Linux, 720p internal resolution.

Two defaults of the test runs mislead on another machine (16/11/2026): `run_perf.sh` caps the GPU heap at 4096 MiB
(`MKW_GPU_MEM_LIMIT`, set for the 660M), and on a 12 GB RX 6700 XT the race then took 59 ms a frame against 39-43 ms
without the cap (sl-base, sl-nocap: two separate runs, both with the GPU at its lowest clock, before `MKW_GPU_CLOCKS`;
not measured with the clock held); and `race.sh`'s 720p needs the RomFS overlay `build/romfs-res`
(`tools/romfs/resolution.py`): without it the log says "RomFS overlay: none" and a 1.8.0 race renders at 2560x1440
with a 3840x2160 output. Players have neither: no cap, the game's resolution.

## 2. The picture as on the console

- Shader translator bugs found by comparing with another Switch 2 port: three of six fixed, three to go, each
  reapplied alone and checked on a race capture.
- The kart's shadow misses a frame now and then; two fixes from Ryujinx (MIT) to try.
- Zero Vulkan validation errors, which may be rendering bugs or hangs on other GPUs: whole runs from boot to a race,
  one player (Grand Prix) and two (split screen), give none since the depth target left in the feedback-loop layout
  for the first recording chunk was fixed (RX 6700 XT, RADV, 17/11/2026); photo mode and the other modes not checked.
- Task/mesh shaders as compute: a race on the first thread in the scene lists (bushes and grass).

## 3. Menus

From the title to the race everything is right; the mode, character and vehicle select screens still need checking
screen by screen against the console.

## 4. Stability and memory

- GPU hangs on AMD: upload batches already split; next, breadcrumbs and `VK_EXT_device_fault` to find the cause of the
  remaining ones.
- GPU memory: races run in 4 GiB with every texture; the sparse tiles above 768 MiB (the asphalt) are next.
- Five kernel and platform bugs inherited from totk-pc-recomp (the fiber stack alignment can crash a run).

## 5. Loading and the player's side

- The stutter right after a race starts: block-linear deswizzle on the GPU (totk-pc-recomp, MIT: texture waits from
  900 to 270 ms in that game), VMA reservations on Windows. Measured on an i7-14700F (16/11/2026), the CPU deswizzle
  (`tex_bl_copy`) runs at memcpy speed (16-25 GB/s, 9.6 for a 16 MiB BC7 level), so on the GPU it only pays if the
  GPU reads the storage from its mirror without a CPU copy. In the course load's peak (1.5 s, user call graphs of the
  replay thread, tl-fp1) the replay spent ~66 ms in the BC1/BC7 choice for ASTC textures (`astc_alpha_one`, at image
  creation) and ~44 ms in the deswizzle, both serial on the critical path. Both are now spread over the texture
  workers (`tex_par.c`, `MKW_TEX_PAR`): in a Grand Prix run from boot to the race, at the same 3 328 images, the scan
  took 215 -> 138 ms of the replay thread and the deswizzle of textures of 512 KiB or more 139 -> 77 ms (i7-14700F,
  17/11/2026, two runs). Left: the deswizzle of the smaller ones (~85 ms over ~9 000 uploads of ~140 KiB), too small
  to wake the workers for each; it needs uploads batched before they are split.
- A Linux package that starts on non-rolling distributions (glibc 2.31 through zig), and the launcher tested on real
  Windows machines.
- Windows with a dedicated GPU: memory the GPU wrote is not uploaded again to its mirror.
- "Detect" in the graphics settings: a preset picked from the GPU.
- Audio: two known SDK bugs (splitters, voice 0) and cuts in the movies' audio.

## 6. Later game versions

- Updates (issue #19): each version is a different executable, so the recompilation (function list, patches, the
  runtime's addresses) is redone for it; the game cannot be shared, so this needs someone who owns that version to run
  the tools on their own dump and contribute the result.
- 1.8.0 (11/11/2026): recognised by the setup (`recomp/versions.py`), its own patch table
  (`recomp/patches-mkw-180.txt`: spin waits, zstd, output size and every set of 1.0.0 — aspect, vertplus (the tall
  Window's race FOV), shadow, seed and LAN play) and runtime row (`runtime/core/boot/version.c`: frame update and game
  clock, zstd, NVN2 proc table and `nvnDeviceGetInteger` answers, boot checkpoint, the new `subsdk1` module's
  constructors, the menu focus probe). Checked (16/11/2026): no game code calls the NVN2 procs new in its sdk (its
  proc loader stores them and nothing reads them back, `runtime/gfx/nvn/procs/nvn2.c`); its 11 604 shader stages,
  the 39 inside `exefs/main` included, translate to valid SPIR-V and none is missing in a title, race or photo mode
  capture; the button icon sheet is recognised by its hash (`runtime/ui/glyphs/glyphs.c`).
