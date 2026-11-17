# runtime/core/diag

Diagnostics: flight recorder, counters and profiling, RenderDoc, game phases and the census of SDK calls.

| File | What it is |
|---|---|
| `flight.c`, `flight.h`, `perf.c`, `prof*`, `perfmon.*`, `rdoc.c` | Diagnostics: flight recorder, counters, profiling, RenderDoc |
| `seqwatch.c` | `MKW_SEQ_WATCH=1`: entries and exits of the game's phases (`seq: enter TitleMenuMain frame N`) through the function-entry hook of the translated code; the `seq` command of the control channel; `MKW_SEQ_HOLD` holds the button script while a phase lasts; `MKW_SEQ_UNTIL`/`MKW_SEQ_EXIT`: goes off at a phase and exits N s later (`tools/run/race.sh`) |
| `impcensus.c` | `MKW_IMPORT_CENSUS=<file>`: which SDK functions the game code calls during the run, how many times and from which frame, through the same entry hook as `seqwatch.c`; read by `tools/analisis/runtime/sdk_gaps.py --census` |
