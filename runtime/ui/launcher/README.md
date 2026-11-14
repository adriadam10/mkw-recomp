# runtime/ui/launcher

The start screen, before the game, and the setup of the game files. **Game files → Choose game folder** selects a new
or existing installation and reopens the launcher with it. Prepared files, mods, saves, settings and caches use that folder;
the executable, setup tools and updates stay beside the launcher. Choosing a folder does not move existing files.
An unsupported path shows an error without changing the current installation. The full resolved path can contain at
most 768 UTF-8 bytes; paths ending in spaces or tabs are preserved.
On Windows it must also fit 131 UTF-16 units, leaving space for generated files below `MAX_PATH`; the error message
shows this platform-specific limit. Unsupported stored paths fall back to the launcher folder for recovery.

| File | What it is |
|---|---|
| `launcher.cpp`, `launcher.h` | Start screen before the game (a column on the left: the port's logo and the title, Play, the profile that plays, then Settings, Game files, Mods, Desktop shortcut and Quit; the game files' state at the lower left, the version at the lower right) in its own window with SDL_Renderer; always in the player's build, with `--launcher` in `build/mkw`. Test: `MKW_LAUNCHER_SHOT=<frame>[:<page>[:<tab>]]` |
| `launcher_backdrop.cpp`, `launcher_backdrop.h` | The start screen's background: the player's `splash.png` scaled to cover, over the whole main page, else the port's logo flag drawn large and waving, in the page's dark tones (no art of the game ships) |
| `launcher_logo.cpp`, `launcher_logo.h` | The port's logo drawn with ImGui, the main page's intro (its elements fade and rise in one after the other) and the fade of a page coming in |
| `launcher_look.cpp`, `launcher_look.h` | The start screen's own look: its palette (dark blue-grey, the logo's blue for the main action), its ImGui style and the colours it gives the settings rows; the in-game menu keeps `menu_look` |
| `launcher_online.cpp`, `launcher_online.h` | The Online play page: network choice, this PC's server (host, firewall), the server to play on, friend code, friend requests and friends, LAN play over Tailscale |
| `launcher_players.cpp`, `launcher_players.h` | The Profiles page: each player's name, which one the next start plays (`MKW_PLAYER`), a new one, and removing one (its folder moved to `profile/players/deleted`); the folders and names are `core/base/players.c`. Test: `MKW_LAUNCHER_SHOT=<frame>:players`, `MKW_LAUNCHER_PLAYER_NEW=<name>`, `MKW_LAUNCHER_PLAYER_PICK=<id>` |
| `launcher_setup.cpp`, `launcher_setup.h` | The setup of the files page: `tools/prepare/prepare.py` in a child process (SDL_Process) with the package's Python, `--game` for the chosen installation and `--runtime` for the executable, and its progress line by line |
| `launcher_lastrun.cpp`, `launcher_lastrun.h` | How the last game ended (`tools/prepare/lastrun.py` against `tools/github/signatures.toml`): a known problem with its issue, one a newer release fixes, or an unknown error with the bug form |
| `launcher_update.cpp`, `launcher_update.h` | The update: at start (`MKW_UPDATE_CHECK`, Menu tab), GitHub's latest published release offered on the start screen; `tools/prepare/update.py` downloads it, then the new files replace the old ones (Windows: renamed to `*.old`; Linux: the AppImage) and the program starts again, which sets up again only what changed. Test: `MKW_UPDATE_AS=<version> MKW_LAUNCHER_UPDATE=1` |
| `mods_browse.cpp`, `mods_browse.h` | The Mods page's GameBanana tab: list and search (`tools/mod/gamebanana.py`, one child process at a time), a mod's page with its author, license and a link to it, and Install with a confirmation, which downloads the file into `<game>/mods/.downloads` and hands it to the import. Test: `MKW_LAUNCHER_MODS_FIND=1` |
| `mods.cpp`, `mods.h` | The Mods page: the player's Switch-style mods in `<game>/mods`, imported from a zip or a folder by `tools/mod/import.py` (same child-process pattern as the setup) and enabled for the next start. Test: `MKW_LAUNCHER_SHOT=<frame>:mods` |
