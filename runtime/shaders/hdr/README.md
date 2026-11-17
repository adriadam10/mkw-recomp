# runtime/shaders/hdr

The F1 menu on an HDR10 display (`gfx/vk/device/vk_hdr_ui.c`; from totk-pc-recomp, MIT).

| File | What it does |
|---|---|
| `hdr_fullscreen.vert` | One triangle over the whole target, for `hdr_ui.frag` |
| `hdr_ui.frag` | The menu's sRGB image to PQ at paper white, blended onto the game's picture |
