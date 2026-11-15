# Steam Deck

These notes come from a Steam Deck LCD (SteamOS 3, Mesa RADV `VANGOGH`) running game version 1.8.0 with runtime
0.1.9 to 0.1.11, measured on 13–14/11/2026. The code changes every week, so read the numbers as what those builds did.
Frame times are race medians with `MKW_TRACE=perf`.

The runtime detects the Deck by itself and starts it fullscreen at 1280x800 with VSync (`DEVICE_PROFILE`, see the
wiki's *Settings and mkw.ini*). From 0.1.12 it also renders at 720 lines.

## Before the first setup

**Video memory in the BIOS: leave it at 1 GB.** The Deck's 16 GB are shared, and the BIOS reserves part of them for
the GPU (*Setup Utility > Advanced > UMA Frame Buffer Size*). Guides for emulators often say 4 GB. Here that leaves
Linux 11.7 GB. In a race the game held 4.2 GB of RAM and 5.6 GB of video memory, so the system swapped and frames took
200–330 ms for seconds at a time. At 1 GB (Valve's default) Linux has 14.8 GB and the stalls are gone. The log says
how much is reserved: `vk: integrated GPU, <n> MiB carved out of RAM by the firmware`. To get into the BIOS, power
off, then hold Volume Up and press Power.

**Game folder on the microSD card.** The setup needs about 30 GB, which the internal drive often does not have.
**Choose game folder** on the start screen (0.1.11) can put it on the card. Format the card as ext4. After a
restart, SteamOS sometimes did not mount the card again, and the game then could not find its folder. Taking the
card out and putting it back in mounts it.

**Setup time.** The first setup took 51 minutes (0.1.9), and the update from 0.1.10 to 0.1.11 took 16. Plug the Deck
in for it.

**Gaming Mode.** Add the AppImage as a non-Steam game, with `--play` in its launch options to skip the start screen
(see the wiki's *Installation*).

## Settings

| Setting | On the Deck | Why |
|---|---|---|
| Graphics > **Internal resolution** | **720** (the default from 0.1.12) | With *Original* the 3D scene renders at the console's docked 2560x1440 and is scaled down to the screen. That is 16 of the GPU's ~42 ms a frame. Time trial: 35.7 → 24.6 ms (≈28 → 41 fps). 540 was ~2 ms faster still, but one 540 run hung the GPU on 0.1.10. |
| Graphics > **Quality** | **Low** | Quarter textures, detail distance 1, 1024 shadow map: less video memory, which on the Deck is RAM. |
| Screen > **Frame generation** | Off | On 0.1.9 the base frame rate dropped from ~30 to ~24 fps, with a ~160 ms hitch every ~6 s. |
| Steam's performance overlay: **frame limit** | Your choice | A 40 fps limit did not slow the game down. The game picks its own frame pacing (below). |
| Steam's performance overlay: **manual GPU clock** | 1000 MHz or off | 1600 MHz uses up the shared 15 W and leaves the CPU less. 1000 MHz was as fast as 1300 MHz. |

Docked on a large screen, choose a higher internal resolution in the menu: the default only applies while
`mkw.ini` has no `resolution` line.

## What to expect

| Scene (0.1.11, 720 lines, Low) | Frame time | Limited by |
|---|---|---|
| Time trial, one kart | ~23 ms (~43 fps) | GPU (~13–20 ms) and the replay |
| Grand Prix, 24 karts | ~36 ms (~27 fps) | CPU: the game's own code and the replay of its GPU commands. The GPU needs ~20 ms. |

The game picks a frame time of 1/60 or 1/30 s itself (Settings > Screen > Frame rate on Automatic). At 40 fps the
race runs at full speed (`game: speed 1.00x` in the log). Below 30 fps it runs in slow motion: in a Grand Prix that
was ~0.9x.

## If the Deck slows itself down every few seconds

On the Deck these notes come from, the CPU dropped to 400 MHz and the GPU to 200 MHz again and again, at ~3 W, for
4–20 s at a time. In those stretches the game ran at 0.1–0.3x. It took 36–46 % of every long run. It happened:
- on the charger and on battery,
- at 12 W and 15 W,
- at 56–74 °C,
- and with a CPU stress test without the game too.

The cause was the fan's speed sensor. The fan turned, but the Deck read 0 rpm and reported a fan fault, and its
firmware then held the clocks down under load. With *Updated fan control* off in Steam's settings, the clocks stayed
at 400 MHz all the time. Reseating the fan's cable did not help, so it needs a new fan.

To check a Deck, read the fan fault and the speed (in Desktop Mode's terminal):

```
cat /sys/class/hwmon/hwmon*/fan1_fault /sys/class/hwmon/hwmon*/fan1_input 2>/dev/null
```

`1` and `0` mean the same fault. A healthy Deck reports `0` and a speed of some thousand rpm. The game cannot change
this. The frame times above come from that Deck, so a healthy one may be a little faster.

## Fixed along the way

- **GPU resets** (`amdgpu: ring gfx_0.0.0 timeout ... Process mkw`): 3 in ~8 long runs on 0.1.10, none in the runs
  on 0.1.11, after #281. The log just stops when this happens: look in `journalctl -k`.
- **Out of video memory** (`physical pool chunk ... device-local allocation failed`): 13–14 per run on 0.1.10. #295
  (0.1.11) retries after dropping released textures, which was not enough on the Deck. #326, still open, frees memory
  the way textures do and leaves 0–2 per run.

## Tried, no gain on the Deck

Each of these was an A/B inside a run (`MKW_AB`), plain and inverted, comparing scene-balanced medians. The first
three are from a Grand Prix on 0.1.11:
- `REPLAY_PAR`: +2.5 ms, worse (four cores).
- `DRAW_CACHE`: ±0.2 ms.
- `REUSE` with `DRAW_CACHE`: +12 ms, worse.
- `ONE_PER_CORE` (time trial, 0.1.10): +1.1 ms, worse.

A shorter idle spin on the recorder thread changed neither the frame time nor the power draw.
