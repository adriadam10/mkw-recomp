# tests/docs
The checks the pre-commit hook runs (`tools/docs/check.py`, `tools/git/cfg_rows.py`).

| File | What it checks |
|---|---|
| `test_hook_encoding.py` | Both checks read the repository's UTF-8 files on a legacy code page (Windows 1252, the C locale) instead of stopping with a `UnicodeDecodeError` |
