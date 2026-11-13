# tests

Tests that need neither the game nor a GPU, run by the CI on Linux and Windows (`.github/workflows/ci.yml`). The back
end's regression test runs over captures instead (`tools/test/`).

| File | What it checks |
|---|---|
| `test_setup.py` | The setup's command lines and files from folders with spaces and accents: the compiler's path on its way to the shader step (issue #29), the resume without `--source` after an update, the update's version order and its clean-up of `*.old` files, the update that regenerates the code cleanup removed, NVIDIA's downloads fetched again when they arrive damaged (issue #207), and setup tools, unpacking and warm-up when the installation is separate from the launcher, including symlink/junction aliases of long installation paths and response-file paths with spaces, quotes and backslashes for Clang, Zig and lld-link |
| `test_game_dir.py` | Installation selection and persistence, relative paths, UTF-8 length boundaries, trailing whitespace and quotes, environment overrides, settings saved in the chosen folder, library readiness before memory initialization, recovery from failed or invalid saved selections, and log-folder discovery with launcher fallbacks and active log destinations (`c/t_game_dir.c`, POSIX harness) |
| `test_game_dir_windows.py` | The actual Windows selector compiled with Clang/MinGW, run natively or under Wine: UTF-16 root limits with room for derived filenames, ASCII/BMP/supplementary characters, prepared DLL discovery, rejection before persistence, relative-prefix counting and recovery from invalid saved paths (`c/t_game_dir.c`; skipped when the toolchain or runner is absent) |
| `test_issues.py` | The issue moderation's reading of a report (`tools/github/classify.py`): the release from the form, RecompLauncher's report or the prose, the GPU without the CPU's vendor, empty reports, logs, known errors and their regressions, which maintainer replies close an issue and which player replies reopen it |
| `test_flow.py` | Shapes of control flow ptxas leaves that the translator must structure as the branch graph runs them (`sm86/translate/flow.py`, checked with `flow_sim`): a jump into a tail that an if / else shares |
| `test_ui_text.py` | The menu's text catalogs (`i18n/`): the English template has every id of `ui_text.h`, each `.po` is up to date with it, and each translation keeps the English's printf conversions in order (a swapped `%s`/`%d` would make printf read the wrong argument) |
| `test_switch2.py` | The Switch 2 controllers' Bluetooth LE protocol (`runtime/input/switch2_proto.h`): mkw-neo's test (`c/t_switch2.c`, reports recorded from real Joy-Con 2s) built against our copy |
| `test_mods.py` | Code patches shipped by mods (`recomp/patches.py` `load_mods`): a mod with spans only, two mods on one address stop the generate, no `mods/` folder leaves the table as it was |
| `test_cfg.py` | Every setting the in-game menu writes to `mkw.ini` is a key `runtime/core/base/cfg.c` reads back (#168: `HDR` was dropped as unknown) |
| `test_defaults.py` | The memory caps a player gets with nothing set (`runtime/gfx/vk/device/vk_defaults.c`), built alone and checked on the GPUs players reported: the Radeon 660M keeps its measured caps, a large discrete GPU is not held to them (issue #31) |

Usage: `uv run pytest tests`
