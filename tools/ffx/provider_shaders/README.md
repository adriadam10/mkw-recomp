# tools/ffx/provider_shaders

| File | Purpose |
|---|---|
| `compile_shaders.ps1` | Diagnostic-provider mode only: compiles and validates 18 SDR and seven HDR optical-flow SPIR-V blobs |
| `gen_shader_blobs.py` | Diagnostic-provider mode only: reads reflection and emits the complete provider lookup implementation |

The production game-field build is implemented in `tools/ffx/builder/build.py`. It
compiles only `ffx_frameinterpolation_game_motion_vector_field_pass.hlsl`, checks
the official Vulkan binding layout, and emits no embedded lookup table.
