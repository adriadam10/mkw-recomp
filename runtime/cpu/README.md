# runtime/cpu

| File | What it is |
|---|---|
| `cpu.h` | `Cpu` (registers, NZCV, TPIDR, exclusives, FPCR/FPSR, vectors), memory accesses and the `rt_*` services the generated C uses |
| `simd.h`, `simd_fp.h` | AArch64 NEON and floating point in portable C |
| `dispatch.c`, `dispatch.h` | `rt_dispatch`: indirect calls through an address table |
| `fma.c` | Correctly rounded `fma`/`fmaf` (totk-pc-recomp, from musl) for game code built for x86-64-v2 (`MKW_SETUP_CPU=v2`, CPUs without FMA); empty in a v3 build |
| `sjlj.c` | Guest `setjmp`/`longjmp`: a setjmp caller's body runs under a host setjmp point, and a dispatch into the middle of a live one (the guest longjmp's `BR x30`) jumps back to it |
| `hostfn.c` | Host functions at guest addresses (`HOST_OVERRIDE`): this is how an `nn::*` function is replaced by name; `HOST_FUNCTION` gives an address to a function without a symbol (virtual tables of objects the host hands to the game). `MKW_HOST_TIME=1`: time each one keeps a game thread stopped (render and the rest), every 600 frames |

From totk-pc-recomp (see `../README.md`).
