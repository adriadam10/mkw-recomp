// A mod's options page on the start screen (mod_page.cpp): what the mod's plugin draws through mkw_plugin.h's MkwPage,
// opened by the Options button of the Mods page. The plugin's library is loaded into the start screen for it (the
// mkw_mod_page export only; mkw_plugin_init is the game's) and stays loaded until the start screen ends.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// The mod's options page: its plugin libraries for this system (<game>/mods/<mod>/plugin/*.dll|.so) loaded and the
// first one exporting mkw_mod_page kept. Opens the page either way: without one it says the mod has none.
void mod_page_open(const char *mod, const char *title);
int mod_page_is_open(void);
const char *mod_page_title(void);   // the mod's title, for the page's heading
// One frame of the page, inside the current ImGui window (the Mods panel's body).
void mod_page_draw(float scale);
void mod_page_close(void);

#ifdef __cplusplus
}
#endif
