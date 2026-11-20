# runtime/shaders/temporal

The temporal upscalers, NVIDIA DLSS and AMD FSR 3.1 (`gfx/vk/exec/vk_temporal.c`).

| File | What it does |
|---|---|
| `tmp_mv.comp` | The camera motion vectors and R32F depth the upscalers read: each pixel's depth taken back to the previous frame |
| `tmp_cas.comp` | Contrast adaptive sharpening (after AMD FidelityFX CAS, MIT) of the upscaler's picture, before the HUD |
