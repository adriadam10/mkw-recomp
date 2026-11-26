# runtime/core/memory

Guest address space, write tracking and read traps.

| File | What it is |
|---|---|
| `memory.c` | Guest address space and write tracking; frozen regions; read traps (`mem_trap`: what the GPU writes reaches the page when the game touches it; off with host pages larger than 4 KiB, and with them early reads, base traps and spilled pages, which need one guest page to fault alone); 9 809 MiB pool and Switch 2's GetInfo (29-33) |
| `memory_early.c` | `MKW_EARLY_READ`: a lock from the submit on what the GPU is going to write; =1 counts the game's touches before the GPU finishes (read or write, with the reading function); =2 makes an early read of bytes the GPU writes wait for its submit; =3 (default on Linux) for the previous submit that writes them, and only watches the pages whose previous writer has not finished; `MKW_EARLY_READ_DRY` logs the waits without doing them, or what each one changes, and counts the laps with the next writer running |
| `memory_spill.c` | Spilled pages (`trapped == 4`, `MKW_MEM_SPILL`): game pages moved to a scratch file beside the executable and dropped from RAM, inaccessible; the first touch (or a thaw) reads them back. Linux only |
| `memory_btrap.c` | Base traps (`trapped == 3`): game pages kept read-only that serve as the shadow's merge base until something writes them; before that, the hook copies the page to the base (`vk_mem_lazy.c`). Linux only |
| `memory_early_diag.c` | Diagnostics of `MKW_EARLY_READ_DRY`: events with the game's stack and a census of laps with the next writer running |
| `memory_int.h` | What `memory.c` shares with `memory_early.c` (the trap map and its lock) |
