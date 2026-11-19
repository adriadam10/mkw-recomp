# Contributing

## Rules of the project

- **Recompilation, not emulation.** No CPU interpreter or JIT, and no GPU emulation below NVN2 (Ampere command
  streams, MME macros, QMDs, the `gsv`/`nvsrv`/`vi2`/`syncpt` services). NVN2 is reimplemented at the API level and the
  SDK is replaced by function name.
- **Nothing of the game in the repository.** No game code, assets, keys, dumps, or C or SPIR-V generated from them:
  all of that is regenerated in `build/` from each user's own copy. `tools/git/pre-commit` refuses the generated
  code's marker and the game's files; install it with `git config core.hooksPath tools/git`.
- **No keys and no decryption.** Setup works only from a dump that is already decrypted (folders, `.nup` or `.nx`).
  Formats that need the console's keys to be read (NSP, XCI, NCA, NSZ, XCZ) are not supported, and neither is code
  that asks the user for keys or a keyset or runs a tool that decrypts with them (NSTool, hactool…). Pull requests that
  add them are closed.
- **Online play lives in its own project.** Code that talks to or stands in for Nintendo's online services (NPLN,
  Gamesync, friends, matchmaking, leaderboards, Pia relays), redirects their hostnames or replaces the game's TLS roots,
  and servers, relays or room systems for playing over the internet, are not accepted here: they belong in a separate
  repository, such as [MKWMP](https://github.com/anarqz/mkwmp). Pull requests that add them are closed. What stays
  here is what the game already does offline: local play and its LAN mode (`MKW_LAN`, also over a VPN), and generic,
  documented hooks that such projects can build on: the native plugin API of
  [runtime/plugin/mkw_plugin.h](runtime/plugin/mkw_plugin.h), loaded from a mod the player enables.
- **Licenses.** Code may be taken from MIT-compatible projects only, naming the source and commit in the code and in
  the table of [docs/project/research.md](docs/project/research.md#sources) in the same pull request. From GPL projects (eden-nx2,
  oboromi, Eglantine…) and from projects without a license, ideas only, never code.
- **The console's behaviour by default.** Anything for weaker hardware (lower resolution, caps, quality presets) is an
  option whose default is what the console does.
- **Fix, don't switch off.** A feature that causes a bug is fixed where it breaks; turning it off by default needs the
  cause measured and written down.
- **Nothing serial that can run elsewhere.** Work the runtime can do on another thread does not run on the game
  thread that asks for it.

## Measuring and testing

- **Performance** changes are measured before and after with an A/B inside the same run: a switch `MKW_<X>=ab` acts
  only in the B blocks of `MKW_AB=<seconds>`, and [tools/run/ab.py](tools/run/ab.py) compares them. Two runs of the
  same build differ by up to 12 ms per frame, so two separate runs prove nothing. Diagnostics that add work to the
  measured path have their own switch.
- **Back end** changes (replay, translator, textures, memory) are checked in seconds on frame captures:
  `MKW_CAPTURE` records a few frames once, and [tools/test/replay.sh](tools/test/replay.sh) replays them against the
  game's own image. A game run is for what never reaches the back end (CPU, loading).
- **Rendering** fixes are judged on the final screen image against the console's (`tools/imagen/vsref.py`), never on
  an intermediate pass.
- **Shader translator** changes that touch control flow are checked off the GPU first (`tools/shaders/flow_check.py`
  on every program); a broken structured loop can hang the GPU of the whole desktop.
- **Self-tests**: `build/mkw --selftest` and the `--probe-*` options of [runtime/probes](runtime/probes/README.md).

## Code

- C for the runtime (C++ only where a library needs it: Dear ImGui, VMA), Python for the tools, run with `uv`.
  `ruff` for Python (`pyproject.toml`).
- Comments say why, not what, and name the measurement or source behind a choice. Every script starts with what it
  does, why it exists and how to run it.
- Every folder has a `README.md` that names each of its files, updated in the same change that adds, moves or removes
  one; `uv run tools/docs/check.py` (run by the pre-commit and the CI) checks it.
- Settings the player sees have a row in the settings table (`runtime/core/base/cfg.c`), with the console's value as
  default; `tools/git/cfg_rows.py` checks it.
- Commits follow [Conventional Commits](https://www.conventionalcommits.org/) (`fix(sm86): ...`), in English.

## Translations

The menu, start screen and setup texts are in `i18n/` as gettext `.po` files, one per language (see
[i18n/README.md](i18n/README.md)). The first versions of most languages are machine-assisted: corrections from native
speakers are very welcome, edited in Poedit or any text editor and sent as a pull request.

## Pull requests

The [pull request template](.github/pull_request_template.md) is the checklist for these rules.

- One branch per topic, based on current `main`; follow-up work goes in a new pull request.
- Say what was tested: OS, GPU, and how (a game run, a capture replay, a probe). "Not tested" is a valid answer.
- Investigation notes, logs and screenshots go in the pull request, not in the repository.
- Say whether the change was written with AI assistance. The author of the pull request is responsible for every
  line of it.
