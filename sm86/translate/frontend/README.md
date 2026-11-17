# sm86/translate/frontend

What the GLSL back end (`../glsl/`) works from: the program loaded with its header and decoded instructions, and its control flow structured from the branches, checked by simulation off the GPU.

| File | What it does |
|---|---|
| `program.py` | Loads a program: SPH (0x80-byte header, layout from Ryujinx `ShaderHeader.cs`, MIT) and its instructions decoded with `sm86/decode`; stops at the data that follows the code |
| `flow_sim.py` | Simulates off the GPU the original branch graph and the structured GLSL with the same pseudo-random answers for each condition and compares the instructions they execute; `flow.structure` rejects (and keeps the `switch` for) what does not match over 4 sets of answers |
| `flow.py` | Structured control flow: `if/else` and `do/while` from the `BRA`s instead of a `switch` on the PC (idea and cases from SavageWindRecomp `structure_flow`, MIT); also a `CALL` without `RET` (a jump), branches to the far join of an if / else-if chain and the conditional exit just before the back edge (`while (!P)`); a jump into a short tail that ptxas shares between the branches of an if / else gets its own copy of it (`_dup_tails`, checked by `flow_sim` against the original graph; a third pass also copies longer tails that fall into a join); if the branches do not nest, or there is BRX or a CALL/RET that is not a subroutine copied into its calls (`glsl/subroutines.py`, or `inline_single_call` for one caller), `glsl/core.py` keeps the `switch` |
