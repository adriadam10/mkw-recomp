# tests

Tests that need neither the game nor a GPU, run by the CI on Linux and Windows (`.github/workflows/ci.yml`). The back
end's regression test runs over captures instead (`tools/test/`).

| File | What it checks |
|---|---|
| `test_setup.py` | The setup's command lines and files from folders with spaces and accents: the compiler's path on its way to the shader step (issue #29), the resume without `--source` after an update, the update's version order and its clean-up of `*.old` files |
| `test_issues.py` | The issue moderation's reading of a report (`tools/github/classify.py`): the release from the form, RecompLauncher's report or the prose, the GPU without the CPU's vendor, empty reports, logs, known errors and their regressions, which maintainer replies close an issue and which player replies reopen it |
| `test_flow.py` | Shapes of control flow ptxas leaves that the translator must structure as the branch graph runs them (`sm86/translate/flow.py`, checked with `flow_sim`): a jump into a tail that an if / else shares |
| `test_ui_text.py` | The menu's text catalogs (`i18n/`): the English template has every id of `ui_text.h`, each `.po` is up to date with it, and each translation keeps the English's printf conversions in order (a swapped `%s`/`%d` would make printf read the wrong argument) |
| `test_defaults.py` | The memory caps a player gets with nothing set (`runtime/gfx/vk/device/vk_defaults.c`), built alone and checked on the GPUs players reported: the Radeon 660M keeps its measured caps, a large discrete GPU is not held to them (issue #31) |
| `test_nup.py` | The .nup reader (`tools/nup/extract.py`) on a synthetic container packed against the spec by an independent packer: the update shadowing the base, a standalone update as the whole game, zstd and raw files, deduplicated extents, the resume (skip by size, rewrite on a size mismatch), the sha256 mismatch, the fail-closed validation (magic, TOC hash, package count, the standalone rule, unsafe and duplicate names, unknown codec) and the meta block's display version and build id |

Usage: `uv run pytest tests`
