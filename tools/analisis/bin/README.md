# tools/analisis/bin

The game's executable as a database (`build/bindb.sqlite`) and the queries, disassembly and searches over it.

| Command | Answers |
|---|---|
| `bindb.py` | Builds `build/bindb.sqlite` from `main`, `sdk` and `subsdk0`: functions, symbols (with their readable C++ name), PLT stubs resolved to the import, `BL`/`B` calls, `ADRP`+`ADD`/load-store references, strings, `SVC` instructions (table `svc`) and data words that point to a function (table `fptr`: 219 757, in 21 382 vtable columns; 109 475 functions are only reached that way); the NVN2 driver functions of `sdk` with their name (`tools/gpu/nvn2_procs.py`). Addresses from `recomp/layout.py`, the same as in the generated C | `uv run tools/analisis/bin/bindb.py` (~6 s) |
| `q.py` | Short queries over that database | `uv run tools/analisis/bin/q.py <command> <arg>` |
| `seq_nodes.py` | MKW's global sequence nodes (`AIGlobalSeq*`, the game's phases: 233 in 1.0.0, 285 in 1.8.0): name method and vtable slot, to `build/seq_nodes.tsv` (1.0.0) or `build/seq_nodes-180.tsv` (1.8.0) for `MKW_SEQ_WATCH` (`runtime/core/diag/seqwatch.c`); `--shared` lists the base's methods | `uv run tools/analisis/bin/seq_nodes.py`; 1.8.0: `--game ~/Juegos/mkw-copias/1.8.0 --db build/bindb-180.sqlite --out build/seq_nodes-180.tsv` |
| `findf.py` | float32 constants (±1e-4) in `main`'s image | `python3 tools/analisis/bin/findf.py VALUE...` |
| `disasm.py` | Disassembly of a single function (the one containing the address) with calls and imports named from the database; the last resort when `q.py` is not enough | `uv run tools/analisis/bin/disasm.py main+0x19e5860 [--count N]` |

`q.py` commands (addresses can be `0x7000…`, `main+0x19e5860`, a symbol name or an NVN2 function name):

| Command | Answers |
|---|---|
| `at ADDR` | Which function an address falls in |
| `fn ADDR` | Summary: who calls it, what it calls (named first), which strings it uses |
| `callers ADDR` / `callees ADDR` | Who calls it (also from other modules through their PLT) / what it calls |
| `str TEXT` | Strings containing it and which functions use them |
| `sym PATTERN` | Symbols (exported, imported, objects) by name |
| `imports PATTERN` | Imports of `main` with how many calls each has |
| `refs ADDR` | Functions that reference a datum |
| `vptrs ADDR` | Which data words point to the function (indirect calls): vtable column, index and who loads the column (constructors) |
| `vtable ADDR` | The pointers of the vtable column containing that datum |

Limits: functions end where the next one starts; a `BLR` does not say what it calls: `vptrs` gives the candidates
(pointers in data), not the call site.
