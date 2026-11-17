# runtime/shaders/framegen

Frame generation (`gfx/vk/device/vk_framegen.c`).

| File | What it does |
|---|---|
| `fg_luma.comp` | The luma pyramid |
| `fg_flow.comp` | The block matching of each level |
| `fg_median.comp` | The vectors' median |
| `fg_synth.comp` | A made frame |
