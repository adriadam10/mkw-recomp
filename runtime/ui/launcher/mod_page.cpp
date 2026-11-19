// A mod's options page on the start screen (mod_page.h): its plugin's mkw_mod_page drawn by plugin/mod_page_ui.cpp.
//
// The plugin's library is loaded here only when the player presses the mod's Options button (the Mods page already
// says that a plugin runs its own program), and only its mkw_mod_page is called: no game exists in this process. The
// settings the widgets show and store are mod_settings.c's, for the player the next start plays, so the plugin reads
// them in the game (MkwPluginApi.setting_get).
#include <stdio.h>
#include <string.h>

#include "imgui.h"

#include "launcher_players.h"
#include "mkw_plugin.h"
#include "mod_page.h"
#include "mod_page_ui.h"

extern "C" {   // the runtime's C headers, as mods.cpp includes them
#include "cfg.h"
#include "platform/platform.h"
#include "ui_text.h"
}

extern "C" void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#ifdef _WIN32
#define PLUGIN_EXT ".dll"
#else
#define PLUGIN_EXT ".so"
#endif

enum { MAX_LIBS = 16 };

// Every library a page loaded stays loaded: its threads may still run when the page closes.
static struct {
    char path[PLAT_PATH_MAX * 2 + 300];
    MkwModPage fn;   // NULL: the library loaded but exports no page
} libs[MAX_LIBS];
static int nlibs;

static struct {
    int open;
    char title[96];
    MkwModPage fn;
    ModPageUi ui;
} G;

static int lib_cb(const char *name, void *ctx) {
    const char *dir = (const char *)ctx;
    if (nlibs == MAX_LIBS) return 0;
    char path[sizeof libs[0].path];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    for (int i = 0; i < nlibs; i++)   // loaded by an earlier opening of this page
        if (!strcmp(libs[i].path, path)) {
            if (libs[i].fn && !G.fn) G.fn = libs[i].fn;
            return !G.fn;
        }
    void *lib = plat_lib_open(path);
    if (!lib) {
        rt_log("mod %s: plugin/%s does not load: no options from it", G.ui.mod, name);
        return 1;
    }
    snprintf(libs[nlibs].path, sizeof libs[nlibs].path, "%s", path);
    libs[nlibs].fn = (MkwModPage)plat_lib_sym(lib, "mkw_mod_page");
    if (libs[nlibs].fn && !G.fn) G.fn = libs[nlibs].fn;
    rt_log("mod %s: plugin/%s loaded for its options page (%s)", G.ui.mod, name,
           libs[nlibs].fn ? "it has one" : "it has none");
    nlibs++;
    return !G.fn;   // stop at the first page
}

extern "C" void mod_page_open(const char *mod, const char *title) {
    memset(&G.ui, 0, sizeof G.ui);
    snprintf(G.ui.mod, sizeof G.ui.mod, "%s", mod ? mod : "");
    snprintf(G.title, sizeof G.title, "%s", title && *title ? title : G.ui.mod);
    snprintf(G.ui.mod_dir, sizeof G.ui.mod_dir, "%s/mods/%s", cfg_game_dir(), G.ui.mod);
    G.ui.reopen = 1;
    G.fn = nullptr;
    char dir[sizeof G.ui.mod_dir + 16];
    snprintf(dir, sizeof dir, "%s/plugin", G.ui.mod_dir);
    if (G.ui.mod[0] && plat_dir_exists(dir)) plat_file_list(dir, PLUGIN_EXT, lib_cb, dir);
    G.open = 1;
}

extern "C" int mod_page_is_open(void) { return G.open; }
extern "C" const char *mod_page_title(void) { return G.title; }
extern "C" void mod_page_close(void) { G.open = 0; }

extern "C" void mod_page_draw(float scale) {
    if (!G.open) return;
    if (!G.fn) {
        ImGui::TextDisabled("%s", menu_text(UI_MOD_NO_OPTIONS));
        return;
    }
    // the player the next start plays: the Profiles page may have changed it since the last frame
    snprintf(G.ui.player_name, sizeof G.ui.player_name, "%s", launcher_player_name());
    launcher_player_dir(G.ui.player_dir, sizeof G.ui.player_dir);
    mod_page_ui_draw(&G.ui, G.fn, scale);
}
