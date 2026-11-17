# runtime/shaders/texture

Texture decoding and recompression on the GPU (`gfx/vk/tex/vk_astc.c`).

| File | What it does |
|---|---|
| `astc_decode.comp` | Compute ASTC decoder into RGBA8 |
| `bc7_encode.comp` | Mode 6 BC7 encoder: recompresses the decoded ASTC to 1 byte per texel instead of 4 |
| `bc1_encode.comp` | BC1 encoder: half a byte per texel, for ASTC without alpha |
