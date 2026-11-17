# tools/analisis/runtime

What the runtime still lacks or no longer needs: SDK imports without a replacement and dead inherited code.

| Command | Answers |
|---|---|
| `sdk_gaps.py` | The system work list: imports of `main` with no replacement in `runtime/`, by namespace, first the ones that depend on a console service (the runtime only serves `audren` and `sm`); with `--census`, which ones the game called in a run with `MKW_IMPORT_CENSUS` and how many times | `uv run tools/analisis/runtime/sdk_gaps.py [--namespace nn::hid] [--census build/run/NAME.imports]` |
| `sdk_class.py` | Each import of `main` by where it ends up as recompiled SDK code: a console service, the runtime's kernel or nothing of the console (pure library), following the direct calls down to their `SVC`s; gives the README's SDK bar (`tools/docs/progress.py`) | `uv run tools/analisis/runtime/sdk_class.py [--class kernel] [--namespace nn::os]` |
| `muerto.py` | Dead code in the runtime inherited from totk-pc-recomp: global functions nothing asks for (`nm`), `cfg.c` keys without a reader (menu options that do nothing), `#if` on macros `build.py` fixes, sources not built in any target and catalogue texts never shown; list only | `uv run tools/analisis/runtime/muerto.py [funcs\|keys\|ifdefs\|files\|texts]` (<1 s, after a build) |
