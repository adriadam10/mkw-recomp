# tests/c

C sources the Python tests next door build with the compiler at hand and run.

| File | Built by | What it checks |
|---|---|---|
| `t_switch2.c` | `test_switch2.py` | The Switch 2 controllers' protocol (`runtime/input/switch2_proto.h`): from seedds/mkw-neo (MIT), with reports and replies recorded from a left and a right Joy-Con 2 |
| `t_game_dir.c` | `test_game_dir.py` | Settings and installation discovery with real files and single-thread POSIX platform stubs; no game, SDL or GPU |
