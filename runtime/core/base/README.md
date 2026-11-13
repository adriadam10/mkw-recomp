# runtime/core/base

The runtime's common pieces: the `host.h` interface, settings (`cfg`), commands to a running game, system info, hashing
and UTF-16.

| File | What it is |
|---|---|
| `host.h` | The runtime's common interface |
| `cfg.c`, `cfg.h` | Settings (ini file and environment variables). `game_dir` in the launcher's `mkw.ini` (or `MKW_GAME_DIR`) selects an installation folder, relative to the executable or absolute; unset, the executable's folder. Its `mkw.ini` keeps that installation's settings and RomFS path; the selection stays beside the launcher. `cfg_game_dir` resolves this before the game library loads. `cfg_bool`/`cfg_value` require a row in the settings table (without one, fatal, also under `--selftest`); diagnostic `MKW_*` options without a row are read with `cfg_get` |
| `ctl.c` | `MKW_CTL=<fifo>`: commands to the running game (screenshot, dump, flight recorder, button presses, textures at an address, game memory to a file); `tools/run/ctl.sh` |
| `players.c`, `players.h` | The players of one install (the launcher's Profiles page): each its own console account, nickname and save under the profile folder; `MKW_PLAYER` in `mkw.ini` names the one the next start plays |
| `sysinfo.c`, `sysinfo.h`, `sha256.c`, `sha256.h`, `utf16.h` | System info, hashing, UTF-16 |
