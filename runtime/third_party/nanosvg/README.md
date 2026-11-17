# runtime/third_party/nanosvg

`nanosvg.h` and `nanosvgrast.h` from [memononen/nanosvg](https://github.com/memononen/nanosvg) @ 239e102e (zlib: its
text at the top of each file), unchanged. `ui/launcher/launcher_brand.cpp` includes both with their
`*_IMPLEMENTATION` defines, to rasterise the start screen's GitHub and Discord marks at the size the screen needs.

| File | What it is |
|---|---|
| `nanosvg.h` | The SVG parser: paths and shapes into cubic Béziers |
| `nanosvgrast.h` | The rasteriser of a parsed image into RGBA pixels |
