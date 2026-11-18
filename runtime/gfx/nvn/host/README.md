# runtime/gfx/nvn/host

The host graphics layer: its interface and the null back end, present and window, the programs' prepared SPIR-V and
the texts the host shows.

| File | What it is |
|---|---|
| `gfx.h`, `gfx_null.c` | Graphics layer interface, null back end (no GPU) and `gfx_backend`, which picks the back end and wraps it with the capture of `../cap/` (`MKW_CAPTURE`) |
| `gfx_present.c`, `gfx_wsys.h`, `gfx_window_sdl.c` | Swapchain, present and vsync; SDL3 window |
| `gfx_hdr.c` | HDR output (`MKW_HDR` auto/off): what nn::vi tells the game about the display's HDR (`MKW_HDR_FAKE`), the HDR10 swapchain format and metadata, the player's peak and paper white over the system's (`MKW_HDR_PEAK`, `MKW_HDR_WHITE`; the last system values kept in `MKW_HDR_SYSTEM` for `gfx_hdr_headroom`), and 10-bit PQ pixels to sRGB bytes for shots, captures and thumbnails |
| `gfx_shader.c` | Loads MKW's prepared SPIR-V (`prepared_get`, key `nvn2_program_key`) into the memory cache; loads the pipeline prewarm shaders in parallel (`gfx_shader_prewarm`); frees the SPIR-V only the prewarm read (`gfx_shader_spirv_release`) and reads it again from its file if a draw asks; with `MKW_HDR_TONEMAP=psychov` the tone curve programs from `<spirv>/renodx` (tools/shaders/renodx.py) with the display's headroom set in them (`MKW_RDX_SPLIT=<px>`: left of it the game's curve, to compare) |
| `gfx_shader_pack.c` | `spirv/shaders.pack` (`tools/shaders/pack.py`): a program's `.spv`, `.res.json` or `.cs.spv` read from the one file, while its folder was not written after it; `gfx_shader.c` reads the loose file otherwise |
| `gfx_shader_int.c` | A prepared shader's variants with integer inputs, integer outputs or integer images; failed integer-image conversions name the program and reason and save their input with `MKW_SPIRV_INT_DUMP` |
| `gfx_shader_internal.h` | The `GfxShader` struct and the memory cache, shared by `gfx_shader.c`, `gfx_shader_int.c` and `gfx_shader_refl.c` |
| `gfx_shader_refl.c` | Reflection of a prepared shader: its `.res.json` into resource arrays, specialization words and flags, and its SPIR-V's interface Locations and workgroup size |
| `gfx_spirv_io.c` | The integer-inputs patch of a vertex shader and the integer-outputs patch of a fragment shader: the inputs whose attribute format, or the outputs whose colour target, is UINT/SINT become u/ivec4, their loads and stores bitcast (issue #64) |
| `gfx_spirv_img.c` | The integer-images patch of a compute program: the textures and storage images whose bound view is UINT/SINT become integer images, including separate image/sampler construction, their reads and writes bitcast (VUID format-07753, issue #64); failures return a diagnostic reason |
| `ui_text.c`, `ui_text.h` | Texts the host shows the player (software keyboard, menu and start screen; menu in English, Russian and Spanish) |
| `ui_text_cat.h` | What every language's catalog shares (the language names, the arrays' declarations); the catalogs themselves are generated from `i18n/ui/*.po` |
