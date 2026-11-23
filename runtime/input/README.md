# runtime/input

| File | What it is |
|---|---|
| `input.c`, `input.h`, `input_map.*` | Controller state and remapping; one gamepad per player for local multiplayer (`MKW_VIRTUAL_PADS=N` adds N virtual SDL gamepads to try it without controllers); the last device used (keyboard or controller, for the button icons) |
| `input_power.c` | Each player's controller battery (discussion #516): SDL's power state, a Switch 2 controller's voltage band from `switch2.c`, or on Linux an xone Xbox pad's sysfs level band (`linux_power.c`); the text the menu and the start screen show, and a thread that puts up a notice once when a battery drops to 15 %. Test: `MKW_BATTERY_TEST=<percent>` puts every gamepad on battery at that level (with `MKW_VIRTUAL_PADS=N`) |
| `linux_power.*` | Linux only (not built on Windows): Xbox pads on the xone driver, whose battery SDL leaves UNKNOWN, read from `/sys/class/power_supply` (level band only, shown as High/Medium/Low; matched by Xbox order, other makes in earlier slots shift no index); checked by `tests/runtime/test_power.py` |
| `input.c`, `input.h`, `input_map.*` | Controller state and remapping; one gamepad per player for local multiplayer (`MKW_PAD_P2=1` assigns the first gamepad to player 2 for keyboard + controller play; a row per controller in the Controls tab trades places with the player before, live, `input_pad_swap`; `MKW_VIRTUAL_PADS=N` adds N virtual SDL gamepads to try it without controllers); the last device used (keyboard or controller, for the button icons) |
| `input_script.*` | Button script (`MKW_INPUT_SCRIPT`; a `p<N> ` prefix presses for player N, also through `ctl press`; paused while an `MKW_SEQ_HOLD` phase lasts; a `<phase>+<ticks> BUTTON` line counts from the start of that phase, with `MKW_SEQ_WATCH=1`); the A button of `MKW_SKIP_MOVIES` (`core/hacks/skip.c`) and `MKW_CTL` presses |
| `input_sdl.*`, `input_wsys.h` | SDL3 back end |
| `motion_fusion.c` | Gyro and accelerometer |
| `wheel.c` | Racing wheels SDL has no gamepad mapping for (#488, MOZA R3/R5): a mapping by position (wheel axis to the left stick, first buttons to A, B, X, Y, shoulders, Minus, Plus, first hat to the D-pad; pedals unmapped), and `gamecontrollerdb.txt` from the game folder; for the game and the launcher |
| `dsu.c`, `dsu.h` | Cemuhook / DSU (DualShock UDP) motion client (`MKW_DSU`, off by default; `MKW_DSU_SERVER`, `MKW_DSU_SLOT`): a DSU server's gyro and accelerometer for a gamepad whose driver gives none, such as an XInput pad (#435) |
| `switch2_proto.h` | The Switch 2 controllers' Bluetooth LE protocol (Pro Controller 2, Joy-Con 2: GATT ids, handshake, reports, stick calibration, rumble, pairing), from seedds/mkw-neo (MIT) with our button bits; checked by `tests/runtime/test_switch2.py` |
| `switch2.h`, `switch2.c` | Switch 2 controllers over Bluetooth LE (`MKW_SWITCH2`, off by default; SDL has them over USB only): the session on top of a transport (identity, pairing, calibration, features, player lights, rumble), each pad a virtual SDL gamepad with its gyro, so players, layout and rumble work as for any gamepad |
| `renhdrum_host.h`, `renhdrum_host_win.c` | RenHDrum (`MKW_RENHDRUM=1`, off by default, Windows only): the game's own vibration values (amplitude and frequency of two bands per side) played as HD haptics on player 1's DualSense (USB: speaker and trigger kick; Bluetooth: 0x36 report), Pro Controller, Xbox One S or Steam Controller, through `renhdrum.dll` beside the exe (RenHDrum 0.5.0's release asset, which the launcher or the in-game menu downloads when the option is turned on, `tools/prepare/libs.py`; absent or of another version: nothing changes and the log says why). The host side of the bridge below; the SDL motors stay silent while it plays the gamepad. Test: `tests/runtime/test_renhdrum.py` (no controller) |
| `renhdrum_recomp_bridge_win.c`, `renhdrum_recomp_bridge.h`, `renhdrum.h`, `renhdrum_LICENSE-MIT` | trzz1214/RenHDrum's C bridge and DLL header, unchanged (09f44ee, MIT or Apache-2.0, `docs/project/research.md`): loads `renhdrum.dll` and SDL3's functions by name at run time, attaches the gamepads and routes each vibration value to the controller it belongs to |
| `linux_switch2.c` | Its Linux transport: BlueZ over the system D-Bus, libdbus-1 loaded at run time; `MKW_SWITCH2=2` logs each advertising device once |
| `switch2_win.c` | Its Windows transport: WinRT through its C ABI (advertisement watcher, `BluetoothLEDevice`, GATT), its events queued for the transport's thread; `MKW_SWITCH2=2` as on Linux |

Default keyboard (`input_map.c`, changeable in the `.ini`):

| Key | Button | In MKW |
|---|---|---|
| WASD | left stick | steering, menus |
| Mouse, arrows | right stick | camera; world map zoom |
| Space, Enter, E | A | accelerate, accept |
| Left Shift, Backspace | B | brake, back |
| F, X | ZR | drift |
| R | R | drift |
| Q | L | item |
| Z, right click | ZL | item |
| Left click | Y | |
| V | X | |
| 1-4 | D-pad | |
| Tab | + | pause |
| M | − | |
