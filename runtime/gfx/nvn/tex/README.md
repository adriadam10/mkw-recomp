# runtime/gfx/nvn/tex

Block-linear textures: size and mips, deswizzling copies and a CPU ASTC decoder.

| File | What it is |
|---|---|
| `nvn_tex.c`, `nvn_tex.h`, `tex.h`, `tex_deswizzle.c` | Size and mips of block-linear textures (from deko3d, zlib) and block-linear ↔ linear copies |
| `tex_astc.c`, `tex_astc_mt.c` | CPU ASTC decoder (from LAS Recomp, MIT) and its split across threads |
| `tex_par.c` | The texture workers: a job's items (row bands of a deswizzle, block ranges of an ASTC scan, ASTC decode rows) over a fork-join pool of background threads |
