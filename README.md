<p align="center"><img src="docs/logo.svg" width="128" alt="mkw-recomp logo"></p>

# mkw-recomp

A static recompilation of *Mario Kart World* (Nintendo Switch 2, versions 1.0.0 and 1.8.0) into a native PC program for Linux and
Windows. It needs your own copy of the game, dumped from your own console: nothing of the game is in this repository
or in its releases.

**It is a recompilation, not an emulator.** The game's AArch64 code is translated to C ahead of time and compiled
for x86-64; there is no CPU interpreter or JIT. The GPU is not emulated either: NVN2, the Switch 2 graphics API, is
reimplemented on top of Vulkan at the API level, and the game's SM86 shaders are translated to SPIR-V ahead of time.
Nintendo SDK functions (`nn::*`) are replaced by name with our own code.

## Status

Early, and moving fast. On 09/11/2026:

- It boots, plays the intro movies, goes through the menus and races, on Linux and on Windows.
- The start screen sets the game up from your copy on your machine (generates and compiles the C, translates the
  shaders, warms up the pipeline cache) and holds every setting: display, graphics quality, controls, hacks.
- Races run at about 30 fps on a Radeon 660M at 720p and below that on a GTX 1050 Ti at 1080p; 60 fps is the goal.
  Below 60 fps, choose Settings > Screen > Frame rate > 30: the Automatic one (the console's) flips between 60 and 30
  for now and the race runs in slow motion. See [Performance](#performance).
- Frame generation (Settings > Screen > Frame generation, off by default): frames made between the game's, so a race
  at 30 fps is shown at 60, or at 60 on a 240 Hz monitor at 240. Smoother, not faster: the game's own frame rate stays
  the same, the picture is up to one frame later, and fast motion and the minimap can smear.
- Known problems: a few menus (mode, character and vehicle select) still need work, the kart's shadow misses a frame
  now and then, some audio cuts during movies, and on some AMD drivers the GPU can hang. There is no online play.

## Roadmap

The main open work, in order; the details are in [docs/roadmap.md](docs/roadmap.md).

- **60 fps in races.** Today a race frame is a serial chain: the game finishes its frame (~3 ms\*), then the Vulkan
  back end records the game's big submit (~19 ms\*), then the GPU runs it (~10 ms\*), and only then does the next frame
  start.
  Recording is on the critical path, so the work is recording with less CPU: dynamic state only when it changes, draw
  caches across frames, parallel recording, and a faster game thread.
- **The picture as on the console**: the last shader translator bugs, the kart's shadow, push constants, zero Vulkan
  validation errors.
- **Every menu checked** against the console: mode, character and vehicle select are next.
- **No GPU hangs** and less GPU memory, then loading without stutter and a Linux package for non-rolling distributions.
- **Later game versions**: 1.8.0 is set up from its own dump (first support, 11/11/2026: the ultrawide aspect, the
  shadow map size and the fixed seed are 1.0.0 only for now). Each version is a different executable to recompile,
  and since the game cannot be shared, it needs a contributor who owns that version ([#19](https://github.com/adriadam10/mkw-recomp/issues/19)).

\* Measured on 09/11/2026 on our test machine: Radeon 660M APU (Ryzen 5 7535HS), Linux, 720p internal resolution.

## Performance

Measured frame times, each with its date, are in [docs/performance.md](docs/performance.md). To add your machine,
open a [performance report](https://github.com/adriadam10/mkw-recomp/issues/new?template=performance.yml): it goes
into the table with the date of the report. The times are the game's own frames: frame generation shows more frames
but does not shorten them.

| Date | GPU | Internal resolution | Race frame time |
|---|---|---|---|
| 09/11/2026 | Radeon 660M (Linux, RADV) | 720p | 31-33 ms |
| 09/11/2026 | GTX 1050 Ti (Windows) | 1080p | 42-45 ms |

## Playing

<p align="center">
  <img src="docs/images/launcher-setup.png" width="400" alt="The start screen setting the game up from the player's dump">
  <img src="docs/images/launcher-main.png" width="400" alt="The start screen once the game is set up">
</p>

1. Dump Mario Kart World 1.0.0 or 1.8.0 from your Switch 2 (`exefs/` and `romfs/`, decrypted; 1.8.0 with the update
   merged into the base game's RomFS).
2. Download the launcher for your system from [Releases](https://github.com/adriadam10/mkw-recomp/releases): on Linux
   the `.AppImage` (it keeps the game it builds in `~/.local/share/mkw-recomp`; glibc 2.31 or newer: Debian 11,
   Ubuntu 20.04, SteamOS and later), on Windows the `.zip`, unpacked into a folder. Either needs about 30 GB free.
3. Run the AppImage (`chmod +x` first) or `mkw.exe`, open **Game files**, point it at your dump and press **Prepare**.
   The first setup takes a while (it compiles the whole game and downloads NVIDIA's `nvdisasm` and `ptxas`, 84 MB on
   Linux and 126 MB on Windows, to describe the shader encoding on your machine); after that, **Play**.
4. When a newer release is out, the start screen offers it (**Update to vX**): it downloads it, checks it, replaces
   the program and sets up again only what changed. **Settings → Menu → Check for updates** turns the check off.

Requirements: x86-64 CPU with AVX2, a GPU with Vulkan 1.3 (`VK_EXT_mesh_shader` recommended; without it, task and
mesh shaders run as compute), 16 GB of RAM recommended.

## Building from source

Linux, with `uv`, `clang`, `lld`, `ninja`, SDL3, Opus, libvpx, zstd, xxHash, the Vulkan headers and VMA 3.4.0 (details in
[tools/build/README.md](tools/build/README.md)). Put your dump in `game/` (`game/exefs`, `game/romfs`) or set
`MKW_GAME=<dir>`, then from the repo root:

```sh
uv run python recomp/gen.py --exefs game/exefs --out build/generated --jobs 8 --patches recomp/patches-mkw.txt  # 1.8.0: patches-mkw-180.txt
uv run tools/shaders/extract.py && uv run tools/shaders/translate.py
uv run tools/build/build.py              # build/mkw; --target windows builds mkw.exe with llvm-mingw
build/mkw
```

Translating the shaders needs the SM86 encoding specification, generated from NVIDIA's `nvdisasm` (not shipped):
see [tools/shaders/README.md](tools/shaders/README.md). Everything generated goes to `build/`, never to git.

| Folder | Contents |
|---|---|
| [recomp/](recomp/README.md) | The generator: AArch64 → C |
| [runtime/](runtime/README.md) | What plays the console: kernel, memory, SDK replacements, NVN2 over Vulkan, audio, input, menus and launcher |
| [sm86/](sm86/README.md) | The shader translator: SM86 SASS → GLSL → SPIR-V |
| [i18n/](i18n/README.md) | The menu, start screen and setup texts in every language (gettext `.po`) |
| [tools/](tools/README.md) | Analysis, setup, build, game runs and diagnostics |
| [tests/](tests/README.md) | Tests without the game or a GPU, in the CI on Linux and Windows |
| [docs/](docs/README.md) | Research, sources and performance |
| `.github/` | CI, issue and pull request templates |

At the root: `pyproject.toml` and `uv.lock` (the Python environment of the tools), `.gitignore`, `LICENSE`,
`CONTRIBUTING.md`, `CHANGELOG.md` (each release's changes, its notes), `AGENTS.md` (a guide for coding agents),
`.agents-clean` and `.agents-heavy` (what an agent's tooling cleans up and runs under a resource cap here).

## Design decisions (30/10/2026)

| Topic | Decision | Why |
|---|---|---|
| CPU | AArch64→C lifter based on [totk-pc-recomp](https://github.com/valentderah/totk-pc-recomp)'s (MIT), extended to ARMv8.1-8.3: LSE atomics, CAS, LDAPR; PAC as identity, MTE tags as 0 | Same engine family as MKW, and it already scales to a large executable |
| System | The SDK's `nn::*` functions replaced by name; signatures from the mangled names of `main`'s 1 289 imports | No console services to emulate |
| Graphics | NVN2 over Vulkan: 529 of its 699 functions share their name with NVN1 (signatures known from totk-pc-recomp and nxnative); 170 are new | Recompilation, not GPU emulation |
| Shaders | Our own SM86→SPIR-V translator, offline, with a decoder generated from the DocumentSASS / nvsass-disassembler specification | Covers mesh/task and what Maxwell lacks |
| Optional Switch 2 features | HDR on by itself when the display is in HDR, as on the console (Settings > Screen > HDR: Automatic or Off); DLSS, ray tracing and touch behind options, off by default; off, their NVN2 functions exist but do nothing so the game keeps its normal path | Console hardware extras, not needed to play |
| Defaults | The console's look and resolution; lower settings are options | The player chooses |

## How it was made

This port was written with [Claude](https://claude.com/claude-code) (Anthropic's coding agent) doing most of the
typing, under my direction, in about ten days of full-time work: 489 commits from 30/10/2026, every one of them
reviewed, measured or tested before it went in. It was not one prompt and done. Behind it:

- **Method more than prompts.** Every change of performance was measured with an A/B inside the same run (two runs
  of the same build differ by up to 12 ms per frame); every rendering fix was checked against the console's own
  image; shader translator changes are tested off the GPU on every one of MKW's 11 561 programs before a game run;
  frame captures replay the back end in seconds without the game. The rules that came out of mistakes are written
  down and enforced by scripts, so the same mistake does not happen twice.
- **What I had learnt before.** The recompiler and the runtime grew out of
  [totk-pc-recomp](https://github.com/valentderah/totk-pc-recomp) and its Linux port, and out of my earlier work on
  other recomps (SavageWindRecomp, smo-recomp): the lifter, the launcher and the setup that builds the game on the
  player's machine, the button icons, the shader control flow. As a maintainer of
  [AnyPS5](https://github.com/boykopovar/AnyPS5) I learnt how to translate GPU shaders faithfully, run mesh shaders
  as compute on GPUs that lack them, and keep a large project honest with checks and measurements.
- **The community's research.** [eden-nx2](https://git.eden-emu.dev/Takoroka/eden-nx2)'s notes on Switch 2, NVIDIA's
  SASS encoding as documented by DocumentSASS and nvsass-disassembler, Ryujinx and Mesa NAK for shader semantics, and
  the other Switch 2 ports listed in the sources.

## Licenses and credits

This project is under the [MIT License](LICENSE). Code taken from other projects keeps its own license, next to it
(`LICENSE.*` files in [recomp/](recomp/), [runtime/](runtime/), [sm86/decode/](sm86/decode/) and
[runtime/third_party/](runtime/third_party/README.md)): totk-pc-recomp, nvsass-disassembler, Dear ImGui, bc7enc,
AMD FidelityFX FSR 1 and Roboto. Every source used, with its license and what was taken from it (code from MIT
projects only; from GPL projects, ideas only), is listed in [docs/research.md](docs/research.md#sources).

Mario Kart World is © Nintendo. This project is not affiliated with or endorsed by Nintendo, contains no game code
or assets, and does not help to obtain them.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md). Bugs and performance reports go to
[Issues](https://github.com/adriadam10/mkw-recomp/issues), questions to
[Discussions](https://github.com/adriadam10/mkw-recomp/discussions).
