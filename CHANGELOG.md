# Changelog

Each version's section is that release's notes: `tools/build/notes.py` puts it into `docs/release-template.md` (install,
updating, known issues, reporting) for the GitHub release. A section has a summary paragraph, `### Changes` (each with
its issue, #N), `### Tested` (what was run from the packages before publishing) and optionally `### Known issues`.

## Unreleased

Mario Kart World 1.8.0: the setup takes a 1.8.0 dump as well as 1.0.0 and builds that version.

### Changes

- **Game version 1.8.0** (#19): the setup recognises the dump by its `exefs/main` build id (`recomp/versions.py`) and
  generates the game with that version's patch table (`recomp/patches-mkw-180.txt`: its spin waits, the native zstd
  decoder and the output size); the runtime takes that version's fixed addresses from one table
  (`runtime/core/boot/version.c`): the frame update and game clock (Settings > Screen > Frame rate), the zstd
  functions, the NVN2 driver's proc table and its `nvnDeviceGetInteger` answers (API 231.39), the boot checkpoint and
  the constructors of the new `subsdk1` module. The lifter learnt PACGA and FP16 FNEG/FABS, which 1.8.0's sdk uses.
  On 1.8.0 the ultrawide aspect, the shadow map size and the fixed seed have no effect yet.
- 1.8.0's first runs (11/11/2026, RTX 4070 Laptop, Windows): what stopped it, in order.
  - The loader bound a module's references to symbols it defines itself to its own copy; rtld binds them to the first
    definition in load order, so main's operator new/delete (sead's heaps) stand in for the sdk's. 1.8.0's main imports
    the sdk's sized operator delete, which freed sead's blocks into the SDK heap (CentralHeap::Free).
  - New SDK APIs the game calls at boot, which open console services: the Joy-Con 2 mouse and USB mouse (`nn::hid`
    Jcms/Nmouse: none attached), GameChat (`nn::chat`: never started), its camera images (`nvnNativeImage*`: textures
    with no storage), the USB camera (patch set `camera`: the service is a stand-in whose requests fail as a missing
    camera), and the HDR metadata and VRR of `nn::vi` that the HDR support does not take (no VRR).
  - The movies go through subsdk0's movie library in 1.8.0, with the newer `nn::codec` names, an RGBA read and
    `nn::gs::TryWaitNativeSync`.
  - The shader translator's indirect-branch lookup raised on an instruction without a destination register (DEPBAR.LE
    before a BRX): two of 1.8.0's pixel shaders had no SPIR-V, and the GPU faulted (instruction pointer invalid) at
    their first draw.
  - A fatal stop at a guest instruction (undefined, brk) logs the guest's frame chain.

### Tested

- 1.8.0 on Windows (RTX 4070 Laptop, driver 610.47): boot, movies, the title, 7 minutes at ~0.95x without a stop, from
  a build of this tree (not yet from a release package).
## 0.1.9

HDR on HDR screens, the menus in 19 languages and the game itself in Catalan, Galician and Basque, 5.1 sound, and
fixes for crashes and freezes reported since 0.1.8.

### Changes

- **HDR** (#168): Settings > Screen > HDR: Automatic (default) or Off. On a display in HDR the game shows its own HDR
  picture, as the console does, with the system's peak and paper white; it follows the system's HDR setting live.
- **Languages**: the menu (F1), the start screen and the setup page in English, Japanese, French, German, Italian,
  Spanish (Spain and Latin America), Dutch, Portuguese (Portugal and Brazil), Russian, Simplified and Traditional
  Chinese, Korean, Catalan, Galician, Basque, Polish, Turkish and Ukrainian; Automatic follows the system's language.
  These first translations are ours: corrections from native speakers are very welcome (`i18n/*.po`, by pull request).
  The setup's errors (a folder without the game, an encrypted or other version, an incomplete copy) are shown in the
  menu language.
- **The game in Catalan, Galician and Basque**: Settings > Game > Language. The game's own texts are rebuilt from your
  copy at setup (`i18n/game/`); the rest of the game stays in Spanish. A system set to one of them picks it.
- Brazilian Portuguese: a Brazilian system picks the game's Brazilian Portuguese (discussion #42, #134 by
  @GabrielAndrade419).
- **5.1 surround sound** when the system's output has 6 channels (discussion #90).
- A notice on screen when the players chosen lack controllers (#106, #160).
- The game stopped when swapping two controllers' players (#123).
- 16 GB machines keep their pipeline workers (#132).
- The game stopped on some error screens (#147): they are logged instead of opening the console's error applet.
- Long freezes on the title or at start (#111 and others): shaders the translator now structures instead of running
  them as a slow switch; a start that died building pipelines, or a failed present, no longer breaks every next start.
- Starting on Windows takes 20 s less (#128): the game files are indexed per folder.
- Game files set up by another version are set up again, also after updating by hand (#50).
- Saves survive an antivirus or OneDrive holding the file, and only one copy of the game runs per profile.
- Windows with the window minimized or hidden keeps its timing (no slowdown in the background).
- Fixes in threads and fibers inherited from the Switch 1 port (core masks, waits on 64-bit values, fibers resumed on
  another thread).
- From contributors: the window's picture takes the lines it is shown at (#130), the start no longer waits forever for
  a pipeline the driver never returns (#131), and a faster shader lookup (#129), by @guillo1990; replay chunks ended on
  their own thread (#141, by @riyadhHDA); development runs capped and stoppable (#145, by @EdouardSence).
- Diagnostics: a log line when the game waits too long for its frame, saying what the submission was doing (#138), and
  GPU breadcrumbs on more commands (#43).

### Tested

From these packages, before publishing:

- (to fill at release)

## 0.1.8

Frame generation, races that render again on Intel, Adreno and (likely) AMD's Windows driver, a first take on
ultrawide screens, and a faster setup.

### Changes

- **Ultrawide** (#77, #40): Settings > Screen > Aspect ratio: 16:9 (the console's, default), the window's, 21:9 or
  32:9. Races and the title's 3D show more at the sides instead of stretching; the menus and the HUD keep their shape
  in the middle 16:9, and full-screen backdrops and fades reach the edges. What is still missing is under Known issues.
- **Frame generation** (#101, by @guillo1990): Settings > Screen > Frame generation: Off (default), Automatic, 2x, 3x,
  4x. Frames made between the game's own from the pictures (optical flow), so a race at 30 fps is shown at 60; the
  FPS overlay shows game / shown frames. It makes the picture smoother, not the game faster.
- Races black, and menus cut along a diagonal, on Intel (ANV) and Adreno (Turnip) (#26, found and fixed by
  @titooo7): full-screen passes left the position's w undefined. The black 3D with the HUD visible on AMD's Windows
  driver 26.10 (#105) looks like the same cause. After updating, the setup's shader step runs again
  (it now does whenever the translator changes).
- Black terrain on cards whose own memory filled up (#63): sparse tiles fall back to system memory.
- The game stopped when it started its network (#63, after a while in free roam): its sockets now answer as a
  console with no network, so it stays offline.
- A crash pressing HOME or switching windows during the startup logo (#83, #89, #92).
- AMD on Windows: the race's mesh draws run as compute passes, where AMD's driver lost the device (#88).
- Intel Arc drew the race black (#64): integer images and integer colour targets are read and written as integers.
- Big AMD cards ran out of video memory on the title (#43, #68): the texture pool starts at 8 blocks.
- **Faster setup**: the shader step compiles each shader inside its own worker instead of starting a compiler for
  each of the ~11 600 (3.4x faster for that part, same output), and decodes the console's shader code 40 % faster:
  the whole step takes about 43 % less processor time. Generating the game code is 13 % faster. The compiled game
  code now stays in `setup-work/obj`, so from the next update on only the parts that changed are compiled again
  (seconds instead of 8-12 minutes).
- Integrated GPUs: the memory cap counts the firmware carve-out (#50).
- The setup's downloads failed certificate checks on systems without the usual CA file (#102, NixOS).
- Diagnostics: GPU breadcrumbs around the present (#43, #50).

### Tested

From these packages, before publishing:

- The whole setup on Linux (Radeon 660M, RADV) and on Windows 11 (GTX 1050 Ti), with its one-minute warm-up and no
  shader left behind: about 15.5 minutes on Linux (22 with 0.1.7) and 30 on Windows (45), where the shader step went
  from 23 to 14 minutes.

### Known issues

- Ultrawide (#77, #40): during the cup movie the "Skip" prompt is not shown (pressing the button still skips it); in
  two-player split screen the coin and lap numbers sit beside their frames; three and four players are not checked
  yet. 16:9, the default, is not affected.

## 0.1.7

The game starts on AMD graphics cards under Windows, where the window did not open or the title froze.

### Changes

- On AMD's Windows drivers the window did not open after Play, or opened minutes later and froze on the title (#36,
  #38, #54, #20). Some of the game's shaders hold a `switch`, and our translation gave every instruction of them an
  entry point; AMD's compiler took minutes on those (with AMD's own offline compiler, 50 of 81 did not finish in 30 s;
  now none, 84 s for the 81). The translation now takes the switch's own cases. After updating, the setup's shader
  step runs again. Also: a shader the driver does not return holds the game at most 30 s, once, and the start no
  longer waits minutes for the whole prewarm. `GPL=0` in `mkw.ini` is not needed: remove it.
- The window opens black at once instead of after the first image (#36, #20).
- A crash in a race, `no function at guest pc main+0x35bdf8` (#40): the recompiler took an address loaded into a
  register that was overwritten in between, and split a game function in 67 pieces.
- A crash after choosing the character with a 4096 shadow map at 1440p (#48): when that map does not fit, it keeps
  the game's size.
- 2160p in Graphics > Resolution (#55).
- A 64-bit integer compare in 6 shaders was done on 32 bits.
- Diagnostics for graphics-card crashes: the log names the clear, copy or blit after the frame's last pass (#43), and
  `GPU_CRUMBS_DRAW=1` in `mkw.ini` names the shader of each draw (#37, #40, NVIDIA's "instruction pointer invalid").

### Tested

From these packages, before publishing:

- The whole setup on Linux (Radeon 660M, RADV) and on Windows 11 (GTX 1050 Ti), with its one-minute warm-up and no
  shader left behind. We have no AMD card on Windows: the shader fix was checked with AMD's own offline compiler.

## 0.1.6

Graphics cards with more video memory now get more of it for the course's textures.

### Changes

- On a graphics card with lots of video memory the course's ground came out flat and some textures plain (#31): the
  memory for the large tiled textures was capped at 768 MiB on every PC, the figure of the integrated GPU we develop
  on. It now comes from the card: a quarter of its video memory, from 768 MiB to 3 GiB (an 8 GB card gets 1792 MiB;
  uncapped, the title screen took 1472 MiB in our tests). Integrated GPUs keep 768 MiB. `SPARSE_MIB` in `mkw.ini` still sets it by hand.
- `GPU_MEM_LIMIT` in `mkw.ini` on a dedicated graphics card capped the video memory at a small share of it (#28: 6400
  left an 8 GB card 1040 MiB), since it was split with the system memory: it now caps the video memory alone. Without
  it, an integrated GPU on a PC with more than 16 GB of RAM may use a quarter of it instead of 4 GiB.
- `GPU_MEM_LIMIT`, `SPARSE_MIB` and `VRAM_MARGIN` in `mkw.ini` no longer log "unknown key".

### Tested

From these packages, before publishing:

- The whole setup on Linux (Radeon 660M, RADV) and the warm-up step on Windows 11 (GTX 1050 Ti), where the tile memory
  stays at 768 MiB as before (a 4 GB card). The caps for larger cards are checked by the tests on the GPUs players
  reported; we have no card of 6 GB or more to run them on.

## 0.1.5

Local multiplayer, the game no longer runs in slow motion below 60 fps, and the start screen now updates itself.

### Changes

- Local multiplayer with one controller per player (#23): each connected controller is its own player (the first one
  and the keyboard are player 1), the console's controller screen gets the number connected, and rumble goes to each
  player's own controller. Before, Multiplayer went back to the home screen.
- Below 60 fps the race ran in slow motion with Frame rate on Automatic: the game picks 30 or 60 fps from the time the
  GPU reports for each frame, and ours reported the time the work was sent, so it kept choosing 60. It now reports when
  the GPU finishes, and the game stays at 30 (on a Radeon 660M: 110 switches between 30 and 60 in 20 s of race to 0).
- The start screen offers a newer release and installs it (Settings > Menu > Check for updates turns it off).
- "Translate the shaders" failed with `FileNotFoundError` when the game folder's path had a space (#29): the compiler's
  path was split at the space on its way to the shader step.
- The 4096 shadow map (Settings > Graphics) no longer crashes at the kart screen (#9).
- On scaled displays the mouse no longer lands off its place on the start screen (#27).

### Tested

From these packages, before publishing:

- The whole setup on Linux (Radeon 660M, RADV) and on Windows 11 (GTX 1050 Ti), and the update from the start screen on
  both.

## 0.1.4

Fixes the setup that stopped at 359/360 on many Windows PCs, and the shader step failing when a worker runs out of
memory.

### Changes

- "Compile the game code" stopped at 359/360 on Windows (#2, #12): zig, the C compiler in the package, builds its own C
  runtime the first time it links, and on those PCs that build never ended (`setup.log` had no `link probe` line). The
  package now carries that runtime already built and the setup links with it, so zig builds nothing of its own on the
  player's PC, neither for the game library nor for the shader step's helper DLL, where #24 stopped after 360/360.
- The shader step failed with `BrokenProcessPool` when the system ended a worker, usually when memory ran out (#24): it
  now goes on with half the workers, down to one, and `setup.log` says so.

### Tested

From these packages, before publishing:

- The whole setup (generate, compile, translate the shaders, the first run of the game) on Linux (Radeon 660M, RADV)
  with no system tools on PATH, and on Windows 11 (GTX 1050 Ti) with zig's cache emptied first.

## 0.1.3

Fixes two ways the setup could fail or look stuck and a GPU crash on NVIDIA under Linux, keeps a log of the setup, and
makes a GPU crash say where it happened.

### Changes

- "Compile the game code" sat at 359/359 with no sign of life (#2): after the last file the game library is linked,
  and on its first link zig also builds its C runtime for the target (76 s on a 6-thread Windows PC, more on a slow
  disk or with an antivirus scanning). That now runs while the files compile, the link counts in the progress bar,
  and a link that takes over 30 minutes stops with an error instead of waiting forever.
- `unable to find zig installation directory`: zig now gets its `lib/` folder named, so it no longer depends on the
  path a launcher or sandbox gives its executable.
- The setup writes `logs/setup.log`: every tool's output and how long each step took; a failed setup points to it.
- NVIDIA on Linux lost the device as soon as sparse textures were in use (at boot or when a race started): a texture
  was destroyed while a bind of its memory was still queued (#7, thanks to @guillo1990; likely #6).
- When the GPU is lost (#4, #5: AMD discrete cards that went black after the startup logo), `logs/mkw.log` now says
  what the driver knows (the faulting address, `VK_EXT_device_fault`) and which upload batch, render pass or dispatch
  the GPU had started and finished last (`VK_AMD_buffer_marker` marks). Nothing to set: attach the log.

### Tested

From these packages, before publishing:

- The whole setup (generate, compile, translate the 11 561 shaders, the first run of the game) on Linux (Radeon 660M,
  RADV) with no system tools on PATH, and on Windows 11 (GTX 1050 Ti).
- Races on both with the GPU marks on and off (no measurable cost), and #7's fix on AMD (a 60 s race, no GPU
  faults).

## 0.1.2

Fixes the setup, which could not build the game in 0.1.0 and 0.1.1.

### Changes

- It stopped right after "Generate the game code" with `fatal error: 'gamelib.h' file not found` (#1), on Linux and
  Windows.
- Translating the shaders needed `glslc` and `spirv-val` installed on the system.

### Tested

The whole setup (generate, compile, shaders and the first run of the game) from these packages on Linux, with no
system tools on PATH, and on Windows.

## 0.1.1

The Windows zip; its setup builds the shader spec too.

### Changes

- Windows 10/11 x64 package.

### Tested

The start screen on Linux and Windows.

## 0.1.0

First release: the start screen for Linux as an AppImage.

### Changes

- Linux AppImage of the start screen and its setup tools.

### Tested

The start screen on Linux.
