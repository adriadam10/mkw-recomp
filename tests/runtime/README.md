# tests/runtime

Pieces of `runtime/` built alone: audio, controllers, memory caps, settings and the menu's texts.

| File | What it checks |
|---|---|
| `test_ui_text.py` | The menu's text catalogs (`i18n/`): the English template has every id of `ui_text.h`, each `.po` is up to date with it, and each translation keeps the English's printf conversions in order (a swapped `%s`/`%d` would make printf read the wrong argument) |
| `test_switch2.py` | The Switch 2 controllers' Bluetooth LE protocol (`runtime/input/switch2_proto.h`): mkw-neo's test (`t_switch2.c`, reports recorded from real Joy-Con 2s) built against our copy |
| `test_cfg.py` | Every setting the in-game menu writes to `mkw.ini` is a key `runtime/core/base/cfg.c` reads back (#168: `HDR` was dropped as unknown) |
| `test_defaults.py` | The memory caps a player gets with nothing set (`runtime/gfx/vk/device/vk_defaults.c`), built alone and checked on the GPUs players reported: the Radeon 660M keeps its measured caps, a large discrete GPU is not held to them (issue #31) |
| `test_gfx9_block_ca.py` | The GFX9 RADV guard on block-view COLOR_ATTACHMENT (`runtime/gfx/vk/tex/vk_tex_gfx9.c`), built alone: RADV's pre-RDNA maxImageDimension3D of 2048 skips COLOR_ATTACHMENT for a 3D block-compressed image with mips and depth in blocks (the shape addrlib's swizzle filter empties into a SIGTRAP, the Vega 7 crash of #385), and RDNA, other drivers, 2D, one mip and shallow depth keep their block views |
| `test_lazy_pool.py` | Lazy Vulkan pool backing (`vk_mem_back.c`), built with the allocation stubbed: failed backing counts once and successful backing publishes the buffer without counting a failure |
| `test_audio_update.py` | The actual audren RequestUpdate parser (`runtime/audio/audren.c`, its section cut out and built alone) with synthetic REV11/REV15 records: zeroed unused slots cannot reset playing voice 0, freed slots stop, reused slots reset and output status stays with its slot |
| `test_adpcm.py` | DSP-ADPCM voices (`runtime/audio/audio_mixer.c`, its voice decoding cut out and built alone) against an exact decode of random buffers (`t_adpcm.c`): with a context on every buffer, loops included, the output is the exact decode; without contexts at most a few samples differ (a slot refilled before the walk decoded its last samples); high pitch and sample rate cross more than 128 input samples at once; each sample is decoded about once (#331) |
| `test_mod_settings.py` | A mod's settings per player (`runtime/plugin/mod_settings.c`, built alone with `t_mod_settings.c`): values round-trip, a set replaces only its key and NULL removes it, two mods or two players never share a file, and invalid keys, values and mod names are refused |
| `test_power.py` | Xbox pads on Linux's xone driver (`runtime/input/linux_power.c`, built alone with `t_power.c`): the sysfs level band reads as High/Medium/Low, never a percent; charging, full, no battery pack (wired) and per-Xbox-pad order; a non-Xbox pad never takes an Xbox level; the laptop battery is ignored |
| `test_framegen.py` | Frame generation's factor (`runtime/gfx/vk/device/vk_framegen.c`, its selector cut out and built alone with the display stubbed): the 120 fps target (60 x2, 30 x4) on 60-240 Hz and fractional refreshes, the swapchain's room, the uneven-refresh note with and without VRR, movies and turbo, and the existing Automatic and fixed factors |

C sources the tests here build with the compiler at hand and run:

| File | Built by | What it checks |
|---|---|---|
| `t_switch2.c` | `test_switch2.py` | The Switch 2 controllers' protocol (`runtime/input/switch2_proto.h`): from seedds/mkw-neo (MIT), with reports and replies recorded from a left and a right Joy-Con 2 |
| `t_audio_update.c` | `test_audio_update.py` | Voice slots in RequestUpdate: `audren.c`'s update section (`request.inc`, cut out by the test) with the mixer and platform stubbed |
| `t_adpcm.c` | `test_adpcm.py` | DSP-ADPCM voices: `audio_mixer.c`'s voice decoding (`voice.inc`, cut out by the test) against an exact decode of the same random buffers, five sample rates, seven pitches up to 3.9 (plus a high-rate case at 192 kHz and pitch 64), with and without contexts; prints output samples, wrong samples and decodes |
| `t_mod_settings.c` | `test_mod_settings.py` | A mod's settings: `mod_settings.c` with the platform's file and lock calls stubbed on stdio; prints ok or the failed check |
| `t_power.c` | `test_power.py` | Xbox batteries on xone: `linux_power.c` against a fake sysfs tree; prints found, state, percent and coarse |
