// Native plugins of the enabled mods (plugin.c; the API they get: mkw_plugin.h).
#pragma once
#include "mkw_plugin.h"

#ifdef __cplusplus
extern "C" {
#endif

void plugins_load(void);          // before ldr_load: each enabled mod's plugin/ libraries, their mkw_plugin_init
void plugins_game_loaded(void);   // after ldr_load, before hostfn_seal: the plugins' on_game_loaded callbacks
void plugins_started(void);       // after hostfn_seal: no more address overrides
// The mods' tabs of the in-game menu's settings (MkwPluginApi.settings_tab), for ui/menu/settings_tabs.cpp
int plugins_tab_count(void);
const char *plugins_tab_title(int i);
void plugins_tab_draw(int i, float scale);   // the tab's page, inside the current ImGui window
void plugins_tab_shown(int i);               // the tab was selected: its next frame is the page's first (opened)
void plugins_exit(void);          // the game ends normally (window closed, the guest's exit): the on_exit callbacks, once

// The plugins' overlay drawings (plugin_overlay.cpp): registered and switched by the API, laid out by ui/menu/menu.cpp
int plugin_overlay_add(MkwOverlayFn fn, void *user);   // 0 when full
void plugin_overlay_show(MkwOverlayFn fn, int on);
int plugins_overlay_wanted(void);                       // one is on: the overlay lays out a frame
void plugins_overlay_draw(float width, float height, float scale);   // in the current ImGui frame

// Version 4 (plugin.c): the plugins' pad filters, run by input/input.c at each read of a player's input (sticks: left
// x, y, right x, y; guide NULL for players 2-8), and the shown picture a plugin asked for (gfx_present.c).
void plugins_input_filter(int player, uint64_t *buttons, int32_t sticks[4], int *guide);
int plugins_frame_wanted(void);                                    // this present copies its swapchain image
void plugins_frame_captured(const uint8_t *rgba, int w, int h);    // RGBA8, tight rows; the present thread

#ifdef __cplusplus
}
#endif
