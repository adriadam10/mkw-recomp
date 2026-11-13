// The Mods page of the start screen (ui/launcher/launcher.cpp draws it): the player's Switch-style mods in
// <game>/mods, imported from a zip or a folder by tools/mod/import.py. Enabling a mod takes effect at the next start:
// its romfs/ overlays the game's (runtime/sdk/fs/romfs.c reads the MODS list) and its mod.json switches are set into
// the config, which is how a ported code mod's patch sets turn on (patches stay compiled in; the menu only flips
// their switches). A port declares them bare in "switches" ({"CLASSICGP_MAPLINES": "1"}); the page adds the MKW_
// prefix when it sets them.
#pragma once

#include <SDL3/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { MODS_MAX = 64, MOD_NAME_MAX = 64, MOD_SWITCHES_MAX = 8 };

typedef struct {
    char name[MOD_NAME_MAX], value[32];
} ModSwitch;

typedef struct {
    char name[MOD_NAME_MAX], title[96];
    int has_romfs, romfs_files, has_exefs, exefs_match, nswitches, enabled;
    int warn_graphics, warn_shaders;   // mod.json's "warnings" (import.py): files the port treats its own way
    int code;                          // changes the game's code ("image" or "patches"): built into the game code
    char exefs_build[17];   // the first .ips build id, short (16 hex) for the "not this build" line
    ModSwitch switches[MOD_SWITCHES_MAX];
} Mod;

// The import/run state for the page's progress line (launcher_setup.cpp's SetupState, smaller: no steps).
typedef struct {
    int running, finished;   // finished: 1 imported, -1 failed
    char done[64];           // the mod's folder name, from import.py's "done <name> ..."
    char code[32];           // import.py's "error <code> ...", or "stopped"
    char error[512];         // the detail after the code
} ModsState;

// Scans <game>/mods (missing is fine: no mods). The list stays sorted; enabled flags re-read every call.
void mods_refresh(const char *exe_dir);
void mods_poll(void);
// A code mod was enabled, disabled, imported or deleted since the last setup: Play sets the game code up again
// first (only generate and compile run: tools/prepare/prepare.py's keys). mods/.rebuild marks it.
int mods_code_changed(void);
// Play: with another set of mods than the last start, the save of the player about to play (player_dir, its profile
// folder: core/base/players.h) is copied first (<player_dir>/save-before-mods/<time>)
void mods_before_play(const char *player_dir);
int mods_risk_draw(void);   // the risk text (and the first time, its "I understand" box): 1 when an install may go on
void mods_code_built(void);   // the UI thread, every frame of the page and before Play: what an import or merge left
int mods_count(void);
const Mod *mods_at(int i);
// The dialog callbacks hand the pick over; the rename field stays empty unless the player typed a name.
void mods_pick(const char *path, int is_zip);
int mods_has_pick(void);
int mods_pick_is_zip(void);
void mods_set_romfs(const char *path);
void mods_set_exefs(const char *path);
const char *mods_pick_path(void);
// The rename field's buffer for the page's InputText (MOD_NAME_MAX); empty means the mod's own name.
char *mods_name_buf(void);
// Stops a running import (the reader thread reports it as failed).
void mods_cancel(void);
// Runs import.py on the pick (no-op without tools or while one runs); clears the pick when it ends.
void mods_import_start(const char *exe_dir);
void mods_clear_pick(void);
// import.py found several mods in the archive ("error variants", tab-separated): import the one chosen
void mods_import_variant(const char *exe_dir, const char *variant);
ModsState mods_state(void);
// Enabled on/off: writes mkw.ini's MODS (enabled order is the overlay precedence) and re-applies every scanned
// mod's switches, so deleting a mod folder cannot strand a switch on.
void mods_toggle(const char *name, int on);
// 1 when the named mod is enabled (its checkbox state).
int mods_is_enabled(const char *name);
// Recursive delete of our own import, then the MODS fix-up and switch re-apply of a toggle-off.
void mods_delete(const char *name);
// Runs merge.py on the enabled mods (skips when already merged); the merged tree lands in
// <game>/mods/.merged/romfs, which the game reads as one more overlay, last.
void mods_merge_start(const char *exe_dir);
int mods_merge_running(void);
// Blocking merge for the Play button: waits out a running merge, then merges when stale (no-op when
// fresh, so Play usually waits only for the Python startup). A failed merge still plays: the per-mod
// overlays apply and the last enabled mod wins shared files, as without merging.
void mods_merge_sync(const char *exe_dir);
void mods_merge_cancel(void);
// Conflicts the last merge found (inner files changed by several enabled mods): the Mods page lists
// them with a winner picker. Cycles mod, ..., vanilla.
typedef struct {
    char archive[256], inner[256];
    char mods[8][MOD_NAME_MAX];
    int nmods;
    char winner[MOD_NAME_MAX];
    int resolved;   // a pick that still matches the mods in the conflict
} ModConflict;
int mods_conflict_count(void);
const ModConflict *mods_conflict_at(int i);
int mods_conflict_unresolved(void);   // how many still wait for a pick (the page's button count)
void mods_conflict_pick(const char *archive, const char *inner);
// A pick applies to this session only: nothing is written that a later run would take as settled,
// so the picker always shows the conflicts and re-adding a mod asks again.
void mods_conflict_set(const char *archive, const char *inner, const char *winner);
void mods_conflict_commit(void);   // the modal's OK: keep every row's current winner as a pick

#ifdef __cplusplus
}
#endif
