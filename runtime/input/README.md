# runtime/input

| File | What it is |
|---|---|
| `input.c`, `input.h`, `input_map.*` | Controller state and remapping; one gamepad per player for local multiplayer (`MKW_VIRTUAL_PADS=N` adds N virtual SDL gamepads to try it without controllers); the last device used (keyboard or controller, for the button icons) |
| `input_script.*` | Button script (`MKW_INPUT_SCRIPT`; a `p<N> ` prefix presses for player N, also through `ctl press`; paused while an `MKW_SEQ_HOLD` phase lasts; a `<phase>+<ticks> BUTTON` line counts from the start of that phase, with `MKW_SEQ_WATCH=1`); the A button of `MKW_SKIP_MOVIES` (`core/hacks/skip.c`) and `MKW_CTL` presses |
| `input_sdl.*`, `input_wsys.h` | SDL3 back end |
| `motion_fusion.c` | Gyro and accelerometer |
| `switch2_proto.h` | The Switch 2 controllers' Bluetooth LE protocol (Pro Controller 2, Joy-Con 2: GATT ids, handshake, reports, stick calibration, rumble, pairing), from seedds/mkw-neo (MIT) with our button bits; checked by `tests/test_switch2.py` |
| `switch2.h`, `switch2.c` | Switch 2 controllers over Bluetooth LE (`MKW_SWITCH2`, off by default; SDL has them over USB only): the session on top of a transport (identity, pairing, calibration, features, player lights, rumble), each pad a virtual SDL gamepad with its gyro, so players, layout and rumble work as for any gamepad |
| `linux_switch2.c` | Its Linux transport: BlueZ over the system D-Bus, libdbus-1 loaded at run time; `MKW_SWITCH2=2` logs each advertising device once |
| `switch2_win.c` | Its Windows transport: WinRT through its C ABI (advertisement watcher, `BluetoothLEDevice`, GATT), its events queued for the transport's thread; `MKW_SWITCH2=2` as on Linux |
| `phonepad.h`, `phonepad.c` | Phones as controllers (`MKW_PHONE_PADS`, off by default, live): a phone on the same network opens the address the menu shows (as a QR code too) in its browser, on Android or iOS, with nothing to install. Each phone is a virtual SDL gamepad with a gyro and rumble, fed on the server's own thread, so players, layout, remapping and motion work as for any gamepad. The certificate and the join code are kept in `<game folder>/phonepad.bin`; `MKW_PHONE_PADS=2` logs every connection, `MKW_PHONE_PORT` is the port (6590) |
| `phonepad_proto.h` | What the page and the server say to each other, without sockets or SDL: the state packet (buttons, sticks, motion in the controller's axes), the HTTP request head, which addresses may join; checked by `tests/test_phonepad.py` |
| `phonepad_srv.h`, `phonepad_srv.c` | The server: HTTPS on one port of the LAN with BearSSL (`third_party/bearssl`), one thread, non-blocking sockets. HTTPS because browsers hand a page the phone's motion sensors only in a secure context; requests instead of a WebSocket because Safari on iOS refuses one to a certificate the player accepted by hand |
| `phonepad_cert.c` | Its certificate: a P-256 key and a self-signed X.509 certificate naming the PC's LAN address, its DER written by hand; the player accepts it once on the phone |
| `phonepad_page.html` | The page, embedded with `#embed`. Two pads: the first (the default) holds the accelerator and has only Drift, Item, the cross, Accept and Back; the second has every button and a stick. The wheel in both: the steering angle from where gravity points on the phone, sent as the left stick, with the raw sensors beside it for the game's own motion steering. The pad is drawn as the phone is held, whichever way the page stands |

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
