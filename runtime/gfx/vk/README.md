# runtime/gfx/vk

Vulkan back end: replays the operation logs the front end (`../nvn/`) recorded into command buffers and submits them.
It comes from LAS Recomp and MacNativeGames (MIT) through totk-pc-recomp; MKW's own environment variables are in
[../README.md](../README.md).

| Folder | Contents |
|---|---|
| [device/](device/README.md) | Instance, device and capabilities, VMA, recorder thread, submits and fences, present (FSR 1) |
| [exec/](exec/README.md) | Replay of each submit: passes, copies, draws, dispatches and descriptors; parallel recording |
| [pipe/](pipe/README.md) | Pipelines: creation, cache, pipeline library, keys and prewarm, SPIR-V |
| [mem/](mem/README.md) | NVN pools as buffers: shadows, mirrors, uploads, write-backs, constant copies |
| [tex/](tex/README.md) | Textures: images under `tex_lock`, slots under `slots_lock` (`tex` → `slots` → `view`), ASTC/BC7, sparse |
| [diag/](diag/README.md) | Frame dump, GPU time per pass, submit timeline |
| [spv/](spv/README.md) | Embedded SPIR-V of the runtime's shaders (generated) |

Every subfolder with headers is on the include path (`tools/build/build.py`): `#include` goes by name.
