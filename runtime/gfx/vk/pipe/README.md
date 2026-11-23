# runtime/gfx/vk/pipe

Pipelines: creation and cache, pipeline library, key file and prewarm, SPIR-V rewrites and specialized variants.

| File | What it is |
|---|---|
| `vk_pipeline.c`, `vk_pipe_internal.h` | Graphics and compute pipelines, descriptor layouts and their caches; the header, the state and types it shares with `vk_pipe_gpl.c` and `vk_pipe_keys.c` |
| `vk_pipe_layout.c` | Descriptor layouts: bindings of each stage, set layouts with their write templates, layouts shared by binding list and vertex outputs the fragment shader asks for |
| `vk_pipe_gpl.c` | Graphics pipeline library (`vk_caps.gpl`): the four libraries per stage, with their cache, and the fast link of a draw's pipeline |
| `vk_pipe_keys.c` | Pipeline key file (`pipeline-keys.bin`), background build threads and prewarm at boot |
| `vk_pipe_pairs.c` | Prewarm of the shader libraries of every program pair of the game copy (`pipeline-pairs.bin`, written at setup by `tools/shaders/pipeline_pairs.py`); afterwards the SPIR-V only the prewarms read (its and the key records') is released (`pairs_release`); it stops, leaving the rest to the draws, when free RAM runs low (#733) |
| `vk_spirv_patch.c`, `vk_spirv_patch.h` | Rewrites of the translated SPIR-V before the module is created: bindings and set for the pipeline library, dual-source blending, missing outputs, fixes for every module |
| `vk_spirv_alpha.c` | NVN's alpha test as a rewrite of the fragment shader (declared in `vk_spirv_patch.h`) |
| `vk_spec.c` | MKW's own: shader variants with the stable words of the constant bank fixed (`MKW_SPEC`, off); `MKW_CB_CENSUS` census |

On Windows with AMD's proprietary driver and Ellesmere (PCI device `0x67df`, tested on RX 570), alpha-to-coverage
is part of the pipeline key instead of dynamic state. Dynamic coverage produced stippled UI even with coverage
disabled; static coverage preserves the requested on/off state and restores the character-select cards and fades.
`MKW_VK_DYN_A2C=0` selects static coverage, `1` selects dynamic coverage for driver comparisons. Other devices keep
the dynamic path when supported.
