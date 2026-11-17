# tools/analisis/nvn

Recordings and replays of the game's NVN command stream compared frame by frame and draw by draw.

| Command | Answers |
|---|---|
| `recdiff.py` | Compares two NVN recordings command by command (aligned by type): how much of a frame repeats in the next and which fields change; recordings come from `mkw --replay` with `MKW_CAP_RECS=<dir>` | `uv run tools/analisis/nvn/recdiff.py A.bin B.bin` |
| `opjoin.py` | Two serial replays of a capture compared draw by draw (same position in the same submit): cost in each, per submit and by what B did (reused, captured, normal); between two replays the frame moves ±3 ms, more than a per-draw change. Dumps from `build/mkw --replay` with `MKW_TRACE=draw MKW_OP_DUMP=FILE` | `uv run tools/analisis/nvn/opjoin.py A.txt B.txt` |
| `recruns.py` | Which spans of an NVN recording repeat in another without the UBO addresses: draws in equal passes and in runs of equal spans by length (the unit of `MKW_REUSE`) | `uv run tools/analisis/nvn/recruns.py A.bin B.bin` |
