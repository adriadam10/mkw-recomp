# tests/build

The Windows dependency setup (`tools/build/windeps.py`).

| File | What it checks |
|---|---|
| `test_windeps_unpack.py` | An MSYS2 package member never writes outside `build/win/deps`: a path that climbs out is refused, and ordinary members still unpack |
