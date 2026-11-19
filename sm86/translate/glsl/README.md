# sm86/translate/glsl

The SM86 → GLSL 4.6 translator, split by responsibility. Used as `import glsl` (`glsl.translate`,
`glsl.compute_shape`, `glsl.Unsupported`) with `sm86/translate` on `sys.path`.

| File | Contents |
|---|---|
| `__init__.py` | What the package exports (`translate`, `compute_shape`, `Unsupported`) |
| `core.py` | `Builder` (state, operands, dispatch by opcode in `insn`) and `translate()`: branches, structured form (`flow.py`) or a `switch` on the PC, `inline_single_call`, `compute_shape` |
| `alu.py` | Integer, float, predicate and packing instructions (`IADD3`, `IMAD`, `LOP3`, `FSET`, `HADD2`…) |
| `mem.py` | Memory: constant banks (`ULDC`, `LDC`), global as storage buffers (`LDG`/`STG`, atomics, base tracking), shared, local, attributes (`ALD`/`AST`) and `IPA` with its `w` fold |
| `tex.py` | Textures and images (`TEX`, `TLD`, `TLD4`, `TMML`, `TXQ`, `SUST`/`SULD`/`SURED`) |
| `thread.py` | Logical 32-lane warps within host subgroups (`SHFL`, `VOTE`, `MATCH`, `REDUX`), system values (`S2R`), task/mesh outputs (`ISBEWR`/`ISBERD`) and `EXIT` |
| `head.py` | Declarations before `main()` (extensions, helper functions, inputs and outputs, bindings and their reflection) and the task/mesh wrappers |
| `int_hints.py` | The vertex inputs a program uses only as integers (`int_inputs`) and the outputs of a fragment program that only moves flat inputs' bits (`raw_outputs`), in the reflection: the setup's prewarm builds their integer variants (`vk_pipe_pairs.c`) |
| `first_store.py` | A compute program's preamble store that only the dispatch's first invocation makes (an append counter's reset) under the specialization constant `FIRST_STORE`, so the runtime runs it as its own one-workgroup dispatch first (`MKW_FIRST_STORE`, idea of nxnative) |
| `subroutines.py` | Subroutines called from several places (CALL/RET): a copy of the body in each call for the structured form, once the return address is proven to be the instruction after the CALL |
| `common.py` | Shared pieces: `Unsupported`, GLSL helpers including warp-local shuffles and ballots on 32/64/128-lane subgroups, comparison and `LOP3` tables, register and predicate names |

The groups in `alu`, `mem`, `tex` and `thread` are mixins of `Builder`: they use its operand helpers (`src_u`, `dst`…)
and its state.
