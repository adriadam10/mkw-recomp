# Steam Remote Play Together

Play local multiplayer (split screen, up to 8 players) with friends who are not in the room. Steam streams the game's
window and sound to them, and their controllers or keyboards become players 2, 3 and so on. They need only Steam, or
the Steam Link app, or a browser with a guest link: not the game.

It is off by default and changes nothing until you set it up.

## What you need

- Steam installed, running and signed in on the PC that runs the game (the host). On Linux, Steam from Valve or your
  distribution: the Flatpak and Snap versions run in a sandbox the game cannot reach.
- A **Steam App ID** that your account owns (free games count once they are in your library) and that supports Remote
  Play Together. Steam treats mkw as that game while it runs, so your friends see its name. Use an App ID you own or are
  allowed to use. RetroArch (`1118310`) is free and supports Remote Play Together.
- Steam's runtime library next to `mkw` / `mkw.exe`: `libsteam_api.so` on Linux, `steam_api64.dll` on Windows,
  Steamworks SDK 1.62 or newer.

## Set up

1. **The library.** After building, run:

   ```sh
   uv run tools/build/steamlib.py                    # Linux: build/dist/linux/libsteam_api.so
   uv run tools/build/steamlib.py --target windows   # Windows: build/dist/windows/steam_api64.dll
   ```

   It downloads Valve's SDK 1.65 copy (from the Steamworks.NET release, checked by sha256). Pass `--dest DIR` for
   another folder: the one that holds `mkw`. The Linux AppImage runs its copy of `mkw` in `~/.local/share/mkw-recomp`,
   so that is the folder there.

2. **The App ID.** In `mkw.ini` next to `mkw`:

   ```ini
   [steam]
   steam_appid = 1118310
   ```

   or set the environment variable `MKW_STEAM_APPID=1118310`.

3. **Start the game from Steam** (recommended): Steam > Games > Add a Non-Steam Game to My Library, pick `mkw` /
   `mkw.exe`, and play it from there. The Steam overlay (Shift+Tab), and with it the **Invite friends** button, only
   works when Steam started the game. Started any other way, everything else still works: the guest link, and inviting
   from your Steam friends list (right-click a friend > Remote Play Together).

The log says `steam: running as App ID ...; Remote Play Together ready`. When something is missing, a notice at the
top of the picture says why, and the log has the line `steam: Remote Play Together is off: ...`.

## Play

Menu (F1) > Game > **Steam Remote Play Together**:

- **Invite friends**: the Steam overlay's Remote Play Together list. Beside it: how many guests are connected.
- **Copy guest link**: a link to send to anyone. They open it in a browser or the Steam Link app, no Steam friendship
  or account needed. The link is also written to the log.

**Picking local multiplayer** (2, 3 or 4 players) opens Steam's Remote Play Together invite list by itself, as long
as no friend has joined yet or there are fewer controllers than players. With too few controllers the game goes back
to its menu, as it does without Steam, and a notice says how many are missing: once your friends' controllers are in,
pick the mode again. The game does not wait inside its controller screen, because the picture (and with it the overlay
and your friends' stream) would freeze meanwhile. `steam_invite = always` in `mkw.ini` opens the list every time,
`steam_invite = off` never.

When a guest joins, a notice says so. Their controller becomes the next free player (a notice says which). A guest
without a controller gets a player at the first key they press, and plays with the same keys as you (WASD, Space,
Shift...; Settings > Controls). Then choose the number of players in the game as usual: the game needs one controller
per player.

## Good to know

- Steam does not say whose a virtual controller is. Controllers appear in the order guests connect them; a guest who
  joins with both a controller and a keyboard can get two players.
- Your own controller through Steam Input (Steam shows it as a "Steam Virtual Gamepad") stays player 1 and never
  becomes a second player.
- Guests cannot open the menu: Esc, F1, F10 and F11 stay yours. Their mouse does nothing.
- On Linux under Wayland, if guests see "Steam not launched with -pipewire", start Steam with `steam -pipewire` (or
  `-pipewire-dmabuf` when guests get a black picture).
- The App ID is read at start: changing it needs a restart of the game.
- A device loss with "instruction pointer invalid" in the log on NVIDIA cards is the port's known issue #22, with or
  without Steam; `skip_programs` in `mkw.ini` helps find its cause (CHANGELOG, 0.1.10).

## Tested

Against Valve's SDK 1.65 library and a mock of Steam built from Valve's headers, with SDL virtual gamepads standing in
for the guests' controllers (13/11/2026): start-up, every symbol bound (Linux and Windows libraries), guests joining and
leaving, a guest's controller as player 2 with the host's Steam Input twin left out, a guest keyboard as player 3, the
guest link, a missing overlay, and each failure (no library, no Steam, wrong App ID). Picking local multiplayer with too few controllers opens the invite list (13/11/2026, against the mock).
A real session with friends over Steam has not been run yet.
