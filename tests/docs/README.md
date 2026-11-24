# tests/docs
The checks the pre-commit hook runs (`tools/docs/check.py`, `tools/git/cfg_rows.py`) and the scripts contributors run on the repo's own files (`tools/docs/progress.py`, `tools/git/mover.py`).

| File | What it checks |
|---|---|
| `test_hook_encoding.py` | Both checks read the repository's UTF-8 files on a legacy code page (Windows 1252, the C locale) instead of stopping with a `UnicodeDecodeError` |
| `test_dev_tools_encoding.py` | `progress.py` reads the runtime's C files as UTF-8 on a legacy code page; `mover.py` rewrites a path in a UTF-8 file with CRLF endings byte for byte, and names a file it cannot read as UTF-8 instead of skipping it silently |
