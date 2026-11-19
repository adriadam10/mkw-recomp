# tests/translator

The shader translator (`sm86/translate`), the SPIR-V patcher and the recompiler's code patches.

| File | What it checks |
|---|---|
| `test_warp32.py` | Executes the translator's actual shuffle/ballot helpers and emitted signed/unsigned reductions as C++ on synthetic 32/64/128-lane subgroups, with distinct values per logical warp and partially active masks |
| `test_spirv_images.py` | Integer image variants of synthetic compute shaders through the production patcher (`t_spirv_images.c`): signed and unsigned storage images, separate image/sampler construction, input preservation and actionable failure reasons; optional SPIR-V validation |
| `test_translate_base.py` | `translate.py --base`: a program's outputs linked from the played version's pass, a rewrite that leaves the played file, a resumed pass that keeps what is there |
| `test_rdx.py` | The RenoDX tone mapper: `tools/shaders/renodx.py`'s edit of spirv-cross shaped tone curve programs (PsychoV35 after the game's writes, in the HDR mode only; a channel made inside a block; programs it must leave alone; the result compiles when `glslangValidator` is on PATH) and the runtime's constant patch `gfx_rdx_patch` (`t_rdx.c`: only that constant's word, none without it, none past a truncated instruction) |
| `test_pack.py` | `tools/shaders/pack.py`: each program's three files in `shaders.pack` by key in the runtime's order, and a folder written after the pack makes it stale |
| `test_flow.py` | Shapes of control flow ptxas leaves that the translator must structure as the branch graph runs them (`sm86/translate/frontend/flow.py`, checked with `flow_sim`): a jump into a tail that an if / else shares |
| `test_int_hints.py` | The integer input and output hints (`sm86/translate/glsl/int_hints.py`) on MKW's texel-coordinate input, a float attribute compared as an integer and a flat-copy fragment program |
| `test_first_store.py` | The split of a compute program's first-invocation store (`sm86/translate/glsl/first_store.py`) on MKW's 32x1 and 8x4 preambles, and the shapes it must leave alone |
| `test_mods.py` | Code patches shipped by mods (`recomp/patches.py` `load_mods`): a mod with spans only, two mods on one address stop the generate, no `mods/` folder leaves the table as it was |

C sources the tests here build with the compiler at hand and run:

| File | Built by | What it checks |
|---|---|---|
| `t_rdx.c` | `test_rdx.py` | Runs the runtime's RenoDX constant patch (`runtime/gfx/nvn/host/gfx_rdx.c`) on a synthetic module |
| `t_spirv_images.c` | `test_spirv_images.py` | Runs the actual integer-image shader patcher on synthetic modules, verifies input preservation and the failure-result contract |
