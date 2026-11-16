# runtime/probes

Self-tests of each subsystem, one command-line option each (`build/mkw --probe-…`). They come from totk-pc-recomp
except `texpre_probe.c`, `fg_probe.c` and `fps_probe.c`.

| File | Option | What it checks |
|---|---|---|
| `astc_probe.c` | `--probe-astc` | The compute ASTC decoder (`vk_astc.c`) against the CPU one, bit for bit, with synthetic blocks |
| `audio_probe.c` | `--probe-audio` | MKW's own: the SDK's translated `nn::audio` client against the objects of `audren_sf.c`: parameter, open, 200 updates at 5 ms, device and close (formerly `--probe-ipc`) |
| `cfg_probe.c` | `--probe-cfg` | Reading the `.ini`, environment variables and a missing file (`cfg.c`) |
| `fg_probe.c` | `--probe-framegen` | MKW's own: frame generation's passes (`vk_framegen.c`) on two pictures in `MKW_FG_PROBE` (from `tools/imagen/fg_check.py`), the frame halfway and the vectors of each level written back, and its GPU time |
| `fps_probe.c` | `--probe-fps` | MKW's own: `MKW_FPS=auto`'s controller (`fps_auto.c`) on synthetic frame times: the interval each steady frame time settles on with no changes after, a loading hitch, a return to 60 |
| `gfx_abi_probe.c` | `--probe-gfx-abi` | Our objects fit in NVN's and the fields the game reads are where it expects them |
| `gfx_window_probe.c` | `--probe-window` | Window present, vsync, focus, cursor and resize |
| `kernel_probe.c` | `--probe-kernel` | Waits, wakeups, virtual cores and their arguments (cores 0-5), fibers (also one moved between threads) and translated code on four threads |
| `nvn_probe.c` | `--probe-nvn` | The NVN front end over the null back end, called as the game calls it |
| `nvn_tex_probe.c` | `--probe-nvn-tex` | Size, alignment and mip offsets of textures (`nvn_tex.c`) |
| `ov_probe.c`, `ov_probe.h` | `--probe-services` | Each replacement of `runtime/sdk/` and NVN2's native images called through its synthetic address; a linked one the loaded game imports and no probe calls fails it |
| `ov_mkw_probe.c`, `ov_mkw_list.h` | `--probe-services` | MKW's own: the replacements only MKW imports (offline network, account, `oe`, clock, touch, the VP9 decoder with frames the test encodes itself, and sockets over the host's loopback) |
| `tex_probe.c` | `--probe-vkmem` | Texture memory: block-linear copies, ASTC, format table, imported pools |
| `texpre_probe.c` | `--probe-texpre` | MKW's own: quality of the GPU BC7 against `bc7enc` on the game's textures, listed in `MKW_TEXPRE_PROBE_LIST` |
| `vk_exec_probe.c` | `--probe-vkexec` | Vulkan replay: fence values, completion order, empty submits; `MKW_RTSTATS`'s rules on cleared targets; a pitch texture the GPU writes, in its pool bytes after the fence wait; which render targets count as rendered recently (one-shot draws) |
