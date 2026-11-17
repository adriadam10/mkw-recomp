# runtime/shaders

The runtime's own shaders, by what they serve. Compiled into `gfx/vk/spv/*_spv.h`; a new or changed one is regenerated
with `uv run tools/shaders/embed_spv.py runtime/shaders/<folder>/<name>.comp`.

| Folder | What it holds |
|---|---|
| [texture/](texture/README.md) | ASTC decoding and BC1/BC7 recompression on the GPU |
| [mesh/](mesh/README.md) | Task/mesh groups compacted as compute |
| [fsr/](fsr/README.md) | AMD FSR 1: scaling and sharpening |
| `ffx/` | AMD's FidelityFX headers that `fsr/` includes (MIT, `ffx/LICENSE.txt`) |
| [framegen/](framegen/README.md) | Frame generation |
| [hdr/](hdr/README.md) | The F1 menu on an HDR10 display |
| [diag/](diag/README.md) | Diagnostics and probes |
