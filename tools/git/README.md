# tools/git

| File | What it does |
|---|---|
| `commit-msg` | Rejects a commit message in Spanish (this repo's commits go in English): accents or a common Spanish word in the subject, or Spanish words over 8% of the body outside quoted lines |
| `cfg_rows.py` | Every name read with `cfg_value`/`cfg_int`/`cfg_double`/`cfg_bool` has its row in `runtime/core/base/cfg.c` (without it the game stops when reading it unset); called by `pre-commit` |
| `mover.py` | Spreads the files of a folder into subfolders (`git mv` from a map) and rewrites the full paths that cite them across the repo; dry run by default, `--apply` to do it |
| `pre-push` | Rejects pushing a release tag `vX.Y.Z` whose commit's `CHANGELOG.md` section `tools/build/notes.py` would refuse (no summary, `### Changes` or `### Tested`: `TODO` until the setup check), the check `release.yml`'s publish job makes after building every package |
| `pre-commit` | Rejects the commit if it adds a file with `recomp/gen.py`'s marker (generated game code) or files of the game or the console (`.nso`, `.nca`, `.keys`, `main`, `sdk`…), if the docs do not match the repo (`tools/docs/check.py`), if a setting has no row (`cfg_rows.py`) or if an executable has no `#!` line (`shebangs.sh`) |
| `shebangs.sh` | Staged executables without a `#!` line (called by path, bash runs them: a Python script's `import` became ImageMagick's); called by `pre-commit` |

Install in a fresh clone: `git config core.hooksPath tools/git`.
