# runtime/sdk/fs

Files, saves and RomFS, and the RomFS overlay with the internal resolution.

| File | What it is |
|---|---|
| `fs_host.c`, `fs.h`, `romfs.c`, `romfs_path.h` | Files, saves and RomFS (with `ROMFS_LOOSE`, the dump's loose `romfs/` folder, or its `.nup` or `.nx` package) |
| `pack.c`, `pack.h` | A packed dump (the dump as one file), `.nup` or `.nx`, behind one interface: its files, read in place, whole or at any offset; `romfs.c` serves the RomFS from it |
| `nup.c`, `nup.h` | `.nup` packages: the table of contents, its titles layered (the base game and its updates, an update's RomFS over the base's, as Eglantine's `--pack-nup` stores only what an update changes), and each file in place or zstd-decompressed whole |
| `nx.cpp` | `.nx` packages (NXFormat) through its reader (`third_party/nxformat`): any range of a file, decoding only its blocks |
| `pack_unpack.c` | `mkw --unpack PACKAGE DEST [PREFIX...]`: a package's files written out, for the setup (exefs, the shader archives, a copied RomFS) |
| `romfs_res.c` | Internal resolution (`MKW_RESOLUTION`, the original by default): at boot writes the RomFS overlay with the chosen lines to `cache/resolution-<lines>/`, as `tools/romfs/resolution.py` does |
