// The player's copies of the game found in the usual places (ui/launcher/launcher_finddumps.cpp): tools/prepare/
// finddumps.py lists .nup and .nx packages and loose exefs/ + romfs/ folders with their version, so the setup's step 1
// offers each as a button. In Steam's Gaming Mode no file dialog opens (no desktop portal under gamescope): without
// this a player with only a controller could not pick the copy.
#pragma once

#include <stddef.h>

#define FOUND_MAX 16

typedef struct {
    char kind[8];      // "nup", "nx" or "folder"
    char version[16];  // "1.8.0", or "?" when the package does not say
    char path[1024];
} FoundDump;

void finddumps_start(const char *exe_dir, const char *game_dir);   // once; nothing without the package's tools
int finddumps_list(FoundDump *out, int max, int *running);         // a copy of what was found; *running while it looks
