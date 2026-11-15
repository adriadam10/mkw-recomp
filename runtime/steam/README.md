# runtime/steam

Steam Remote Play Together: friends join local multiplayer over Steam, which streams the window and the sound; each
guest's controller or keyboard becomes a player. Off unless `steam_appid` is set in `mkw.ini` (or `MKW_STEAM_APPID`; `auto` takes the App ID Steam passes to the game it starts, and the menu's General tab has a row for it);
set-up in [docs/remote-play.md](../../docs/remote-play.md).

| File | What it is |
|---|---|
| `steam_rpt.c`, `steam_rpt.h` | Starts Steam's API as the App ID before the game boots (main.c), then the `steam` thread: Steam's callbacks by manual dispatch (guests joining and leaving, the guest link), the guests' keys through Remote Play direct input every 8 ms, and the menu's buttons (General tab: Invite friends, Copy guest link). Picking local multiplayer with too few controllers (the controller applet, `sdk/ov/ov_input.c`) asks whether the others join as local players or through Remote Play Together (`menu_choice`), which opens the invite list (`steam_invite`: ask, auto, always, off) |
| `steam_flat.h` | The part of Steam's flat C API used, declared from the Steamworks SDK 1.65 headers so the build needs no SDK; `libsteam_api.so` / `steam_api64.dll` is loaded at run time from mkw's folder (`tools/build/steamlib.py` puts it there) |

How the guests become players, in `runtime/input/input.c`:

- **Controllers**: a guest's controller reaches the host as a Steam virtual gamepad (USB 28DE:11FF). SDL hides those
  unless `SDL_GAMECONTROLLER_ALLOW_STEAM_VIRTUAL_GAMEPAD` is set, which `steam_rpt_start` does once Steam is ready. They
  then join like any controller: the next free player, 2 to 8. Steam does not say whose a virtual gamepad is; the ones
  already there while no guest is connected are the host's own controllers through Steam Input, and they never become
  another player.
- **Keyboards**: with direct input (Steamworks SDK 1.62+) a guest's keys come to `steam_rpt.c` instead of the host's
  keyboard. At the guest's first key that has a binding, that keyboard takes the next free player
  (`input_remote_attach`) and plays with the host's key table (WASD, Space...), but only while there are more guests
  than guest gamepads (`input_guest_pads`) and keyboard players and not in the guest's first 5 s, so a guest with a controller does not also
  get a keyboard player. Esc, F1, F10 and F11 stay the host's.
  The guest's mouse drives nothing.

Steam's client may install crash handlers when it starts: `steam_rpt_start` saves the signal handlers before
`SteamAPI_Init` and gives back any it replaced (`plat_handlers_save`, `platform.h`), since the runtime's fault handler
must keep `SIGSEGV`.

The idea (an emulator running under a chosen App ID, guests' virtual gamepads and Remote Play keyboards as players)
comes from Eglantine's [Remote Play Together extension](https://github.com/damienjacinto/eglantine-remote-play-together);
that code is GPL-3.0 and none of it is copied (see [AGENTS.md](../../AGENTS.md), Licenses).
