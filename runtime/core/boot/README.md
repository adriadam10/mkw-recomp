# runtime/core/boot

Boot: the program entry point, the game code tables, module loading, patches and the game clock.

| File | What it is |
|---|---|
| `main.c`, `boot.c` | Program entry (`MKW_ALLOW_PTRACE=1` lets gdb attach) and boot sequence; `--boot` stops at MKW's first NVN call (1.0.0 `main+0xcf3a5c`, 1.8.0 `main+0xdd07c8`) |
| `gamelib.c`, `gamelib.h` | Game code tables (modules, functions, ExeFS hashes, patches): linked into `build/mkw`, or loaded from `mkwgame.so`/`mkwgame.dll` next to the executable in the player's build (`build.py --dist`), with its header id |
| `loader.c` | Loads the modules at their addresses, relocations, import binding, constructor flag (with Switch 2's `PACIBSP`); `--probe-loader` compares against MKW 1.0.0's numbers (another version's are printed only) |
| `game.c` | MKW's own: the game clock (`frame_step_*`, counts presents; totk-pc-recomp takes it from `frame_rate.c` and its patches); the world clock of A/B runs (`MKW_AB`) counts from the `MKW_SEQ_UNTIL` event; the frame rate setting (`MKW_FPS` auto/30/60: the game's fixed present interval, through its frame object's vtable slot) and the `game: speed` line every 5 s |
| `version.c`, `version.h` | The fixed guest addresses of each game version (1.0.0, 1.8.0), chosen by `RtGame.version`: the frame update, the game clock, zstd, the NVN2 proc table, the boot checkpoint and `init_array` sizes; a field at 0 turns its feature off |
| `patches.c` | Patches by address |
