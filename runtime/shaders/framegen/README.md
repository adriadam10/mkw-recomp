# runtime/shaders/framegen

Frame generation (`gfx/vk/device/vk_framegen.c`).

| File | What it does |
|---|---|
| `fg_luma.comp` | The luma pyramid |
| `fg_flow.comp` | The block matching of each level |
| `fg_median.comp` | The vectors' median |
| `fg_synth.comp` | A made frame |
| `fg_hud.comp` | The game frame's HUD over the frames DLSS-G and FSR 3.1 make (they interpolate the pictures without the HUD) |
| `fg_fsr_mv.comp` | Opt-in full-resolution, object-aware motion vectors for FSR 3.1 frame generation |
| `of_luma.comp` | The R8 inputs of the optional hardware optical flow (`VK_NV_optical_flow`, `device/vk_oflow.c`) |
| `of_mv.comp` | The hardware optical flow's packed vectors decoded into FSR 3.1's motion |
