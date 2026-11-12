# Guide for coding agents

Read this before changing mkw-recomp with an AI agent. The rules are those of [CONTRIBUTING.md](CONTRIBUTING.md);
this file says how to work by them without wasting runs.

## What this is

A static recompilation of Mario Kart World 1.0.0 and 1.8.0 (Switch 2) to x86-64: the game's AArch64 code becomes C
([recomp/](recomp/README.md)), the runtime ([runtime/](runtime/README.md)) replaces the console (kernel, SDK by
function name, NVN2 over Vulkan), and the game's SM86 shaders become SPIR-V offline ([sm86/](sm86/README.md)). Every
folder has a README that names each file: read it before opening the code. The open work is in
[docs/roadmap.md](docs/roadmap.md).

## Hard rules

- **No emulation.** No CPU interpreter or JIT, no GPU emulation below NVN2 (command streams, MME macros, QMDs, console
  services). If a fix seems to need one, it is the wrong fix.
- **Nothing of the game in git.** No game code, assets, keys or anything generated from them (C, SPIR-V, dumps,
  screenshots of the game). Generated output lives in `build/`. Install the hook: `git config core.hooksPath tools/git`.
- **Licenses.** Copy code only from MIT-compatible projects, citing source and commit in the code and in
  [docs/research.md](docs/research.md#sources). From GPL projects (eden-nx2, oboromi, Eglantine) and unlicensed ones,
  ideas only.
- **Console defaults.** Settings for weaker hardware are options; the default is what the console does.
- **Fix, don't disable.** A feature that causes a bug is fixed where it breaks, not switched off.
- **Nothing serial that can run elsewhere.** Do not add work to the game thread, or to the replay thread that records
  the frame, when another thread can do it: both are on the race frame's critical path (see the roadmap).

## Before you guess, look

- The executable database answers most questions about the game's code in one line:
  `uv run tools/analisis/bindb.py` once, then `uv run tools/analisis/q.py fn main+0x19e5860` (callers, callees,
  strings, vtables). Disassemble a single function only when that is not enough (`tools/analisis/disasm.py`).
- Check how other projects solved the same thing before inventing: the sources table in
  [docs/research.md](docs/research.md#sources) says what each one is good for.
- `uv run tools/run/envs.py [REGEX]` lists every `MKW_*` switch the runtime reads, with its comment.

## Test without the game first

A game run takes minutes; most changes can be checked in seconds:

- **Back end** (replay, textures, memory, translator output): record a capture once with `MKW_CAPTURE=1`, then
  `tools/test/replay.sh build/cap/<name>` replays it and compares each frame with the game's own image.
- **Shader translator**: `uv run tools/shaders/translate.py` retranslates only what changed; a change to control flow
  (`sm86/translate/flow.py`, loops, branches) must pass `uv run tools/shaders/flow_check.py` before any GPU run, since a
  broken loop can hang the GPU of the whole desktop. `tools/shaders/glsl_hash.py` proves a refactor changes no output.
- **Runtime pieces**: `build/mkw --selftest` and the `--probe-*` options ([runtime/probes](runtime/probes/README.md)).
- **CPU lifter**: `uv run --with unicorn tools/cpu/lift_diff.py` compares translated instructions with Unicorn.
- **Comments and docs only**: no rebuild needed; a comment in `runtime/cpu` rebuilds the whole game.

## When you do run the game

- Write down first what you expect to see and what each outcome would mean; one run per hypothesis is too many.
- Performance: an A/B inside one run (`MKW_AB=<seconds>` with the switch set to `ab`, then
  `uv run tools/run/ab.py build/run/NAME.log`). Two separate runs of the same build differ by up to 12 ms per frame.
- Rendering: compare the final screen image with the console's (`tools/imagen/vsref.py`), not an intermediate pass.
- Read a log with `uv run tools/run/summary.py build/run/NAME.log` before grepping it.

## Conventions

- C for the runtime, Python (run with `uv`) for the tools; `ruff` for Python. Comments say why, with the measurement or
  source behind a choice. Every script starts with what it does, why, and how to run it.
- A change that adds, moves or removes a file updates its folder's README in the same commit;
  `uv run tools/docs/check.py` checks it.
- Settings the player sees need a row in `runtime/core/base/cfg.c` (`tools/git/cfg_rows.py` checks it).
- Conventional Commits in English. Say in the pull request that the change was AI-assisted and what was tested.
