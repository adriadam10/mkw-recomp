// The start screen (ui/launcher/launcher.cpp), before the game: its own window, gone again when it returns.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

enum { LAUNCHER_QUIT = 0, LAUNCHER_PLAY = 1, LAUNCHER_FAILED = 2 };
// after cfg_init, before mem_init; LAUNCHER_PLAY: start the game, LAUNCHER_QUIT: end, LAUNCHER_FAILED: end, no window (told)
int launcher_run(void);
// The romfs the game will boot (argv, MKW_ROMFS/mkw.ini, the exe folder or the baked-in default): the Mods page
// merges and imports against it, so overlays validate against the same originals the game reads.
void launcher_set_romfs(const char *romfs);
// Boot's resolver, on the launcher thread after setup reloads ROMFS; keeps explicit argv overrides.
void launcher_refresh_romfs(void);
// The profile folder the game will use: the Profiles page lists and makes its players (core/base/players.h), and a
// new set of mods backs up the save of the player about to play.
void launcher_set_profile(const char *profile);
// The exefs the game will boot: an import checks a mod's build id against it.
void launcher_set_exefs(const char *exefs);
// --gb-install URL (GameBanana's 1-Click, "mkw-recomp:https://gamebanana.com/mmdl/FILE,Mod,MOD"): the start screen
// opens on that mod's GameBanana page and asks before installing; any other URL is refused (logged)
void launcher_gb_install(const char *url);

#ifdef __cplusplus
}
#endif
