// The start screen's own file browser (launcher_browse.cpp): a folder or a file picked with a controller, the mouse or
// the keys, inside the start screen's window. The system's file dialogs need a desktop portal (or zenity), which Steam's
// Gaming Mode does not have: under gamescope they never open, so a player with only a controller could not choose the
// game folder, a copy the search did not find, or a mod to import.
#pragma once

#include <SDL3/SDL.h>

enum { BROWSE_FOLDER, BROWSE_FILE };

// The system's dialog, or this browser where the system's cannot open: always in Steam's Big Picture and Gaming Mode
// (cfg_steam_console) or with MKW_LAUNCHER_BROWSER=1, elsewhere when SDL's dialog reports an error. cb gets the answer
// as SDL's dialogs give it (the picked path then NULL; NULL alone when cancelled; NULL for the list on an error the
// browser could not take over), from the browser on the UI thread. title: the browser's heading (the button's label).
// exts: for BROWSE_FILE, the extensions as SDL's filter pattern ("nup;nx"), NULL for any file; filter_name its
// description in the system dialog.
void browse_dialog(SDL_Window *win, int mode, const char *title, const char *start, const char *filter_name,
                   const char *exts, SDL_DialogFileCallback cb, void *ud);

// Once a frame on the UI thread, before the page: opens the browser a failed system dialog asked for. Then 1 while the
// browser is up, and the caller draws it (browse_draw) in the page's place.
int browse_active(void);
void browse_draw(float w, float h, float scale);
