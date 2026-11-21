# Roadmap

What is being worked on, in order of priority: nothing may hang the GPU, then the picture as on the console (menus
before races), then the frame rate, then the player's side. The list is generated from
[roadmap.toml](roadmap.toml) (`tools/docs/roadmap.py`); measured numbers are in [performance.md](performance.md) and
what already shipped is in the [CHANGELOG](../../CHANGELOG.md).

<!-- [[[cog import roadmap; roadmap.emit(cog) ]]] -->
## 1. Nothing hangs the GPU

- GPU hangs on some AMD drivers: upload batches are split; breadcrumbs and `VK_EXT_device_fault` are in place to find the cause of the rest.
- Less GPU memory: races run in 4 GiB with every texture; sparse tiles above 768 MiB are next.

## 2. The picture as on the console

- Three shader translator bugs left (found by comparing with another Switch 2 port).
- The kart's shadow misses a frame now and then.
- Zero Vulkan validation errors: none from boot to a race in one to four players and in a time trial; free roam's one is fixed in #752; photo mode is unchecked.

## 3. Menus

- Title to race is right; mode, character and vehicle select still need checking screen by screen against the console.

## 4. 60 fps in races

- A race frame is a chain: the game's frame, then the Vulkan back end records its big submit, then the GPU runs it. Recording is on the critical path, so it needs less CPU.
- Done: recording overlapped with the game, submits on their own thread, AMD GPU clocks held on Linux, waits by address, hybrid-CPU core placement, the shadow write-back compared with AVX2.
- Next: recording in parallel (the texture lock has to be split first), the fence's completion work, the kernel's global lock, Windows hybrid-CPU placement. Profile-guided optimization of the game's code was tried ([#722](https://github.com/adriadam10/mkw-recomp/pull/722)) and gained nothing measurable on a Radeon 660M.

## 5. Loading and the player's side

- Less stutter after a race starts: texture deswizzle and format scans already run on workers; the many small uploads are left.
- A Linux package for non-rolling distributions and the launcher tested on more Windows machines.
- A "Detect" preset in the graphics settings, chosen from the GPU.
- Audio: two known SDK bugs and cuts in the movies' audio.

## 6. Game versions

- 1.0.0 and 1.8.0 work. Each update is a different executable, so another version needs someone who owns it to run the tools on their own dump and contribute the result ([#19](https://github.com/adriadam10/mkw-recomp/issues/19)).
- 1.8.0 on par with 1.0.0: ultrawide aspect, shadow map size and fixed seed.

<!-- [[[end]]] -->
