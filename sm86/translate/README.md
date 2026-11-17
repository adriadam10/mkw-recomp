# sm86/translate

SM86 → GLSL 4.6 translator, offline (`glslc` makes the SPIR-V).

| File | What it does |
|---|---|
| `frontend/` | What the GLSL back end works from: the program with its SPH header and decoded instructions (`frontend/program.py`), and its control flow structured from the `BRA`s (`frontend/flow.py`), checked off the GPU by simulation (`frontend/flow_sim.py`); details in [frontend/README.md](frontend/README.md). Callers put both this folder and `frontend/` on `sys.path` |
| `glsl/` | The GLSL translator split by responsibility (details in [glsl/README.md](glsl/README.md)): `uint` registers, `bool` predicates, uniform registers, structured control flow (`frontend/flow.py`) or, when not possible, a `switch` dispatcher on the PC, constant banks as UBOs, global memory as storage buffers (base in `c[0]`), textures by their handle `c[bank][idx]`, PS inputs/outputs from the SPH. Semantics from Ryujinx and Mesa NAK (MIT), cited in each function. What it does not handle raises `Unsupported` with the class |
