# tools/analisis

| Folder | What it holds | Usage |
|---|---|---|
| `bin/` | The game's executable as a database (`bindb.py`, `q.py`), single-function disassembly and constant searches | see `bin/README.md` |
| `nvn/` | NVN recordings and replays compared frame by frame and draw by draw | see `nvn/README.md` |
| `runtime/` | SDK imports without a replacement in `runtime/` and dead code inherited from totk-pc-recomp | see `runtime/README.md` |
| `windows/` | Reports of a Windows run's sampled profile | see `windows/README.md` |
| `linux/` | Probes without root: `ioctl` time per thread and request (`LD_PRELOAD`), each thread's state and `wchan` sampled through `/proc` | see `linux/README.md` |

