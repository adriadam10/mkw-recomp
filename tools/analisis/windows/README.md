# tools/analisis/windows

Reports of a Windows run's sampled profile.

| Command | Answers |
|---|---|
| `prof_win.py` | Report of a Windows profile (`MKW_PROFILE=<path>`, `runtime/core/diag/prof_win.c`): samples per thread, per module (driver, ntdll…) and per function of `mkw.exe` and `mkwgame.dll` with `llvm-symbolizer` from `build/win` | `uv run tools/analisis/windows/prof_win.py PROFILE [--top 40]` |
