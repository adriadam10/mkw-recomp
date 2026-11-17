# runtime/shaders/diag

Diagnostics and probes, not used in play.

| File | What it does |
|---|---|
| `solid.frag` | `MKW_FS_SOLID=<hashes>\|all`: replaces those fragment shaders, or all of them, with magenta |
| `clk_busy.comp` | Fixed ALU work for `--probe-gpu-clocks` (`probes/clk_probe.c`) |
