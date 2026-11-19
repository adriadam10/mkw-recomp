// Native plugins of the enabled mods (plugin.c; the API they get: mkw_plugin.h).
#pragma once
#include "mkw_plugin.h"

void plugins_load(void);          // before ldr_load: each enabled mod's plugin/ libraries, their mkw_plugin_init
void plugins_game_loaded(void);   // after ldr_load, before hostfn_seal: the plugins' on_game_loaded callbacks
void plugins_started(void);       // after hostfn_seal: no more address overrides

// The plugins' overlay drawings (plugin_overlay.cpp): registered and switched by the API, laid out by ui/menu/menu.cpp
int plugin_overlay_add(MkwOverlayFn fn, void *user);   // 0 when full
void plugin_overlay_show(MkwOverlayFn fn, int on);
int plugins_overlay_wanted(void);                       // one is on: the overlay lays out a frame
void plugins_overlay_draw(float width, float height, float scale);   // in the current ImGui frame
