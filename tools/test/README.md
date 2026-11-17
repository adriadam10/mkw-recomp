# tools/test

Runtime tests without the game, on frame captures (`MKW_CAPTURE`, [runtime/gfx/cap](../../runtime/gfx/cap/README.md)).
A capture is taken once with a game run and replayed as many times as needed, in seconds.

| Script | What it does | Usage |
|---|---|---|
| `all.sh` | Every capture in `build/cap/` through `replay.sh`, one after another: the back end's regression test | `tools/test/all.sh [TEST]` |
| `dsu_server.py` | A fake Cemuhook / DSU server on 127.0.0.1:26760: a controller rolling left and right for `MKW_DSU=1` (`runtime/input/dsu.c`), to see motion steering without a gyro controller | `uv run tools/test/dsu_server.py [--slot N] [--amplitude DEG] [--period S]` |
| `dumpcmp.py` | Two `MKW_DUMP` dumps of the same frame (the game's, saved by the capture, and the replay's) by groups of consecutive passes with the same targets (the game splits them every 64 draws): the first targets that differ | `uv run tools/test/dumpcmp.py GAME REPLAY [--all]` |
| `skip_bisect.py` | Names the draw program behind a coloured artifact on a GPU we do not have: replays one capture with half of the candidate program key prefixes skipped (`MKW_SKIP_PROGRAMS`) until one is left, a half counting only if it removes the colour and keeps most of the picture (#174) | `uv run tools/test/skip_bisect.py CAPTURE_DIR [--mkw PATH]` |
| `replay.sh` | Replays a capture serially and with `MKW_REPLAY_PAR` (`MODES` picks; `REPEAT=N`: N more chunked runs, for races between threads), saves each present as PNG and compares the two series with `tools/imagen/dircmp.py`; prints each pass's `replay in chunks` line (how many submits went in chunks); does not start with less than `MIN_AVAIL` (11 000 MB) free | `tools/test/replay.sh build/cap/NAME [TEST]` |
