# tools/ffx

`builder/build.py` builds the production StillTie correction as one game-field SPIR-V
shader for the official FidelityFX provider. It starts from pinned MIT
FidelityFX SDK commit `7539ae8b6568ad29ed78a753404351daf3b5cca7`, applies
[`patches/fsr3-1-6-vulkan.patch`](patches/fsr3-1-6-vulkan.patch), compiles with
pinned DXC 1.9.2609.5, and runs `spirv-val`.

| Folder | What it holds |
|---|---|
| [builder/](builder/README.md) | `build.py`: the production shader, and with `--provider` the diagnostic provider |
| [patches/](patches/README.md) | The tracked changes to the pinned SDK |
| [provider/](provider/README.md) | The optional diagnostic provider's CMake and API glue |
| [provider_shaders/](provider_shaders/README.md) | The diagnostic provider's shader compiler and lookup generator |

The production shader uses the official provider layout:

- UAV 128 and 129 for `rw_game_motion_vector_field_x/y`
- CB 256 for `cbFI`
- SRV 0 through 4
- sampler 320

The StillTie fix reserves priority 31 for a verified stationary vector and caps
moving vectors at 30. It is always enabled in the production artifact.
The builder also requires the reviewed output SHA-256
`1d7e3e8d3b7046764a2a9f98f5e04147befaf4b0ea4f9fa017242f9824af5c6e`;
an unreviewed byte change fails before anything is packaged.

```powershell
uv run tools/ffx/builder/build.py
```

The default build writes:

- `build/ffx/out/bin/mkw_fsr_game_field.spv`
- `build/ffx/out/include/ffx_game_field_spv.h`
- `build/ffx/out/SOURCE-NOTICES.txt`
- `build/ffx/out/licenses/`
- `build/ffx/out/source/mkw_fsr_game_field_source.zip`

Normal runtime builds embed the same shader through
`runtime/gfx/vk/spv/ffx_game_field_spv.h`. The builder never edits that tracked
file automatically. After reviewing the generated SPIR-V and its SHA-256,
regenerate the checked-in copy explicitly:

```powershell
Copy-Item build/ffx/out/include/ffx_game_field_spv.h `
  runtime/gfx/vk/spv/ffx_game_field_spv.h
```

The embedded header is the default production input. An executable-adjacent
SPIR-V file remains an explicit diagnostic override.

The source ZIP contains the pinned sparse SDK tree after applying the tracked
patch, plus this builder. It is a reproducibility aid; the SDK is MIT licensed,
not a copyleft source-distribution requirement.

The full diagnostic provider remains available explicitly:

```powershell
uv run tools/ffx/builder/build.py --provider
```

That mode compiles all 25 SDR/HDR frame-interpolation and optical-flow shaders
and writes the diagnostic `mkw_fsr_fg_vk.dll`. The runtime selects it through
`MKW_FFX_FG_LIBRARY`; it is not a drop-in replacement for
`amd_fidelityfx_loader_vk.dll`.

All generated work stays below `build/`. The builder rejects unrelated SDK
changes, pins both downloaded archives by SHA-256, and never writes into the
player's game folder.
