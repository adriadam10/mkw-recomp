// A mod's page drawn with ImGui (mod_page_ui.cpp): what a plugin asks through mkw_plugin.h's MkwPage, inside the
// current ImGui window. Used by the start screen's options page (ui/launcher/mod_page.cpp) and by the mods' tabs of
// the in-game menu's settings (ui/menu/settings_tabs.cpp, through plugin.c). One ModPageUi per page shown; the menu's
// thread only (or the start screen's).
#pragma once

#include "mkw_plugin.h"
#include "mod_settings.h"

#ifdef __cplusplus
extern "C" {
#endif
#include "platform/platform.h"   // C linkage (it has no guard of its own)

enum { MOD_PAGE_INPUTS = 32, MOD_PAGE_INPUT_CAP = 256 };

typedef struct {
    // set by the owner
    char mod[64], mod_dir[PLAT_PATH_MAX * 2];
    char player_dir[PLAT_PATH_MAX * 2], player_name[96];   // whose settings the widgets show
    int in_game;   // MkwPage.in_game
    int reopen;    // 1: the next frame is the page's first again (MkwPage.opened)
    // the page's own
    MkwPage page;
    char shown_for[PLAT_PATH_MAX * 2];
    int disabled, in_list, list_rows, in_columns;
    char list_empty[256];
    struct {
        char key[MOD_SETTING_KEY_MAX + 1];
        char buf[MOD_PAGE_INPUT_CAP];
        int editing;   // the field had the keyboard in the last frame: its text is the player's, not the setting's
    } inputs[MOD_PAGE_INPUTS];
    int ninputs;
} ModPageUi;

// One frame of fn's page. MkwPage.opened is 1 in the first frame for these settings (or after ui->reopen).
void mod_page_ui_draw(ModPageUi *ui, MkwModPage fn, float scale);

#ifdef __cplusplus
}
#endif
