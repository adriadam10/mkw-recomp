# tools

Analysis and preparation tools. They run with `uv run` from the repo root; they read the game copy (`game/` by
default, or `MKW_GAME=<dir>`, with `exefs/` and `romfs/` inside) and write to `build/` (ignored by git).

| Folder | Contents |
|---|---|
| [analisis/](analisis/README.md) | Executable database (`bindb.py`) and short queries (`q.py`) |
| [build/](build/README.md) | Building the generated C with Ninja and assembling the player's folder |
| [cpu/](cpu/README.md) | Lifter coverage over the NSOs |
| [docs/](docs/README.md) | Docs checks at every commit and the README's generated progress bars |
| [git/](git/README.md) | Pre-commit hook that keeps generated code and game files out of the repo |
| [i18n/](i18n/README.md) | The menu's text catalogs: from `i18n/*.po` to the C the runtime compiles |
| [github/](github/README.md) | Issue moderation: release and GPU labels, missing logs, known errors, replies that close |
| [gpu/](gpu/README.md) | NVN2 functions: which ones the game uses, their address in the driver and object sizes |
| [imagen/](imagen/README.md) | Flight recorder thumbnails, contact sheets and image comparison |
| [nup/](nup/README.md) | .nup packages (a55uka/nup v1): the single-file dumps into `exefs/` + `romfs/` |
| [romfs/](romfs/README.md) | RomFS overlays: the game's internal resolutions |
| [prepare/](prepare/README.md) | Builds the game on the player's machine from their copy (started by the start screen) |
| [run/](run/README.md) | Game runs (live with a control channel, to the race, under perf) and diagnostics: stacks, memory |
| [test/](test/README.md) | Runtime tests without the game: frame captures replayed serially and in chunks |
| [shaders/](shaders/README.md) | Extraction of the GPU programs, SM86 opcode census and translation to SPIR-V |
