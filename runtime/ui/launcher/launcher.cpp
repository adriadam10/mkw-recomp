// The start screen, shown before the game: Play and the profile, the controllers, Settings, Game files, Mods and Quit,
// the update offered, the game files' state at the lower left; Tools for saves, shortcuts, updates and problems. The
// player's build (build.py --dist) always starts here; Play starts the process again
// with the launcher skipped (main.c), so the game claims its address space in a fresh process. build/mkw shows it only
// with --launcher.
//
// Its own window and SDL_Renderer (Dear ImGui's SDL3 and SDL_Renderer back ends), not the game's Vulkan device: nothing of
// the game runs yet. The settings pages and the fonts are the in-game menu's (ui/menu/settings_tabs.cpp,
// ui/menu/menu_look.cpp); the look is the start screen's own (launcher_look.cpp), so the in-game menu keeps its own, and
// the logo and the motion are in launcher_logo.cpp. The Play button and the setup's step list are drawn here. The idea
// and the page set follow SavageWindRecomp's runtime/ui/launcher.mm (https://github.com/Revivedx/SavageWindRecomp @
// 5173d27, MIT, Copyright (c) 2026 Revivedx and the Savage Wind Recomp contributors).
//
// Test hooks: MKW_LAUNCHER_SHOT=<frame>[:<page>[:<tab>[:<scroll 0..1>]]] saves that frame as MKW_LAUNCHER_SHOT_OUT (default
// launcher.png) and quits; page main, settings (tab 0..6), files, mods, players or tools. The user's input is ignored while it
// runs.
// MKW_LAUNCHER_SETUP=<folder>[:link] starts the setup from that folder at once, on the game files page.
// MKW_LAUNCHER_MODS_PICK=<zip|folder> picks it on the Mods page, as its dialogs do; MKW_LAUNCHER_MODS_TOGGLE=<mod>
// toggles that mod once, as its checkbox does; MKW_LAUNCHER_MODS_PICKWINNER=<archive> cycles the first conflict of
// that archive once, as its winner button does; MKW_LAUNCHER_MODS_DELETE=<mod> deletes it once, as its confirmed
// modal does; MKW_LAUNCHER_MODS_CONFLICT_OK=1 presses the picker's OK, MKW_LAUNCHER_MODS_MODAL=1 opens it;
// MKW_LAUNCHER_MODS_IMPORT=1 runs the import of the picked zip or folder; MKW_LAUNCHER_NO_MERGE=1
// skips the page-entry merge (pre-merged shots).
// MKW_LAUNCHER_SHORTCUT=1 makes the desktop shortcut at start, as its button does. MKW_LAUNCHER_PLAY=1 presses Play once
// the game files are there (with MKW_GPU=x the game's process then stops at once: the save copy before mods, alone).
// MKW_LAUNCHER_EXIT_AFTER_SETUP=1 quits when that setup ends, after saving the page as <shot out>-end.png (its result
// is in the log); MKW_LAUNCHER_SHOT then takes its shot without quitting. During a shot the update check runs only
// with MKW_UPDATE_AS (tools/prepare/update.py: compare as that version). MKW_LAUNCHER_UPDATE=1 takes the update offered
// at once, as its button does, and MKW_LAUNCHER_RESTART=1 its Restart with it once it is set up beside;
// MKW_LAUNCHER_BACKUP=1 and MKW_LAUNCHER_UNDO=1 press the Tools page's Back up and Undo
// update once (Undo confirmed too; MKW_LAUNCHER_UNDO=ask leaves its dialog open, for a shot). The last run's notice (launcher_lastrun.h) is read during a shot only with
// MKW_LAUNCHER_LASTRUN=1.
#include <SDL3/SDL.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_sdlrenderer3.h"
#include "imgui.h"
#include "ui/menu/id_check.h"
#include "imgui_internal.h"   // RenderTextEllipsis
#include "ui/launcher/launcher.h"
#include "ui/launcher/launcher_backdrop.h"
#include "ui/launcher/launcher_brand.h"
#include "ui/launcher/launcher_logo.h"
#include "ui/launcher/launcher_look.h"
#include "ui/launcher/launcher_players.h"
#include "ui/launcher/launcher_setup.h"
#include "ui/launcher/launcher_tools.h"
#include "ui/launcher/launcher_finddumps.h"
#include "ui/launcher/launcher_lastrun.h"
#include "ui/launcher/launcher_update.h"
#include "ui/launcher/mods.h"
#include "ui/launcher/mods_browse.h"
#include "ui/launcher/mod_page.h"
#include "ui/menu/menu_look.h"
#include "ui/menu/option_rows.h"
#include "ui/menu/settings_tabs.h"

extern "C" {
#include "cfg.h"
#include "gamelib.h"
#include "input.h"
#ifdef __linux__
#include "linux_power.h"
#endif
#include "platform/platform.h"
#include "sysinfo.h"
#include "ui_text.h"
#include "vk_gpu_check.h"
void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
extern const char rt_port_version[];
void app_icon_set(SDL_Window *w);   // build/obj/rt/app_icon.c, drawn by tools/build/icon.py
}

enum Page { PAGE_MAIN, PAGE_SETTINGS, PAGE_FILES, PAGE_MODS, PAGE_PLAYERS, PAGE_TOOLS };

static struct {
    SDL_Window *win;
    SDL_Renderer *ren;
    ImGuiStyle base_style;
    float scale;                    // of the style and fonts in use; 0 none yet
    ImFont *big;                    // Roboto for the title
    ImFont *head;                   // and for a page's heading and the Play button
    ImFont *item;                   // and for the main page's menu rows, at its own size (a scaled font blurs)
    Page page;
    int focus_main;                 // the main page's first button takes the cursor
    int result;                     // LAUNCHER_PLAY or LAUNCHER_QUIT once chosen; -1 running
    int play_after_setup;           // Play with code mods changed: the setup builds the game code, then the game
    int gb_open;                    // a 1-Click link: the Mods page's GameBanana tab first (launcher_gb_install)
    int save_pending;
    // test hook
    char no_window[512];            // why open_window failed, for the player
    long shot_at;
    Page shot_page;
    int shot_tab;
    char exe_dir[1024];
    Uint32 picked_event;            // dialogs can finish on another thread: consume their result on the UI thread
    int picking;
    int steam_art;                  // Add to Steam pressed: the library images are drawn before the next frame
    int directory_restart;
    char source[1024];              // the player's copy: a folder, or a .nup file, from the dialogs
    int link_romfs;                 // 0 copy it into the game folder, 1 use it where it is
    int tools;                      // the setup tools are there
    char shortcut_note[1200];       // what the shortcut button did
    // the last setup's record (read_setup_info): the game version, the port version, the copy and how its RomFS is kept
    char game_version[32], setup_port[48], setup_source[1024], setup_mode[8];
    int pads;                       // the gamepads SDL sees, and the first one's name (pads_refresh)
    char pad_name[128];
    SDL_Gamepad *pad_open[8];       // the first eight, open for their batteries (input_power_from_sdl)
    char updated[96];               // the release the previous run installed (launcher_update.h)
    int relaunch;                   // an update is in place: start the new program when the window is gone
    double intro_t0;                // when the main page was first shown (its intro, once a run); 0 not yet
    double page_t0;                 // when the page last changed (the new page fades in); 0 none yet
    Page shown;                     // the page drawn last frame
} L = {};

static void exe_dir(char *out, size_t cap) {
    out[0] = 0;
    if (!plat_exe_path(out, cap)) return;
    char *cut = plat_last_sep(out);
    if (cut) *cut = 0;
}

// The last setup's record (tools/prepare/prepare.py), NUL-terminated in buf; false when there is none.
// plat_file_read takes the UTF-8 path: fopen reads it in the ANSI code page on Windows, so a game folder with an accent
// in its name had no record.
static bool read_setup_state(char *buf, size_t cap) {
    char path[PLAT_PATH_MAX * 3];
    snprintf(path, sizeof path, "%s/setup-state.json", cfg_game_dir());
    uint64_t size = 0;
    uint8_t *data = plat_file_read(path, &size);
    if (!data) return false;
    size_t n = size < cap - 1 ? (size_t)size : cap - 1;
    memcpy(buf, data, n);
    buf[n] = 0;
    free(data);
    return true;
}

// The last setup was made by another version: a player who updated by hand (a new AppImage, or the zip over the old
// folder) kept the old shaders and game code (#50: 0.1.8 played with 0.1.7's shaders). States from before 0.1.9 keep
// no version, so they set up again once too; prepare.py runs only the steps whose key changed.
static int setup_from_other_version(void) {
    static char buf[16384];
    if (!read_setup_state(buf, sizeof buf)) return 0;
    char want[96];
    snprintf(want, sizeof want, "\"port\": \"%s\"", rt_port_version);
    return !strstr(buf, want);
}

// A string of the setup's record: the first "key": "..." with its escapes (\\, \") undone; "" when absent.
static void json_str(const char *buf, const char *key, char *out, size_t cap) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\": \"", key);
    const char *p = strstr(buf, pat);
    size_t n = 0;
    if (p)
        for (p += strlen(pat); *p && *p != '"' && n + 1 < cap; p++) {
            if (*p == '\\' && p[1]) p++;
            out[n++] = *p;
        }
    out[n] = 0;
}

// What the last setup installed: the game version (its compile step's key is "<build id> <version> ..."), the port
// version, the player's copy and how its RomFS is kept. The copy and the mode fill the setup's steps when the player
// has not chosen others, so setting up again needs no new pick. Read when a page is shown, not every frame.
static void read_setup_info(void) {
    static char buf[16384];
    char compile[160];
    L.game_version[0] = L.setup_port[0] = L.setup_source[0] = L.setup_mode[0] = 0;
    if (!read_setup_state(buf, sizeof buf)) return;
    json_str(buf, "compile", compile, sizeof compile);
    sscanf(compile, "%*s %31[0-9.]", L.game_version);
    json_str(buf, "port", L.setup_port, sizeof L.setup_port);
    json_str(buf, "source", L.setup_source, sizeof L.setup_source);
    json_str(buf, "romfs_mode", L.setup_mode, sizeof L.setup_mode);
    if (!L.source[0] && L.setup_source[0]) {
        snprintf(L.source, sizeof L.source, "%s", L.setup_source);
        L.link_romfs = !strcmp(L.setup_mode, "link");
    }
}

// The gamepads SDL sees: their count and the first one's name. Called at start and when one is connected or removed.
static void pads_refresh(void) {
    input_wheel_map();   // a wheel SDL has no mapping for, as a gamepad (#488); its GAMEPAD_ADDED calls here again
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    const char *name = ids && n > 0 ? SDL_GetGamepadNameForID(ids[0]) : nullptr;
    L.pads = ids ? n : 0;
    snprintf(L.pad_name, sizeof L.pad_name, "%s", name ? name : "");
    for (SDL_Gamepad *&g : L.pad_open)
        if (g) SDL_CloseGamepad(g), g = nullptr;
    for (int i = 0; i < L.pads && i < (int)(sizeof L.pad_open / sizeof *L.pad_open); i++) L.pad_open[i] = SDL_OpenGamepad(ids[i]);
    SDL_free(ids);
}

// The battery of the i-th gamepad SDL sees, as pad_row shows it (input_power_text); its name in w->name.
static bool pad_power(int i, InputPower *w, char *text, int size) {
    *w = InputPower{INPUT_POWER_NONE, -1, 0, ""};
    SDL_Gamepad *g = i < (int)(sizeof L.pad_open / sizeof *L.pad_open) ? L.pad_open[i] : nullptr;
    if (!g) return false;
    const char *n = SDL_GetGamepadName(g);
    snprintf(w->name, sizeof w->name, "%s", n ? n : "");
    int pct = -1;
    SDL_PowerState st = SDL_GetGamepadPowerInfo(g, &pct);
#ifdef __linux__
    int xi = 0;   // this pad's order among Xbox-named pads: other makes in earlier slots shift no index
    for (int j = 0; j < i; j++) {
        const char *m = L.pad_open[j] ? SDL_GetGamepadName(L.pad_open[j]) : nullptr;
        if (linux_power_is_xbox(m)) xi++;
    }
#else
    int xi = i;
#endif
    input_power_from_sdl(st, pct, SDL_GetGamepadID(g), xi, w);
    input_power_text(w, text, size);
    return true;
}

// The GitHub repository the releases and the issues are in: MKW_UPDATE_REPO (tests, forks), else upstream's.
static const char *repo(void) {
    const char *r = getenv("MKW_UPDATE_REPO");
    return r && *r ? r : "adriadam10/mkw-recomp";
}

// Whether a GPU here can run the game (vk_gpu_check.h): the setup takes many minutes and the game's window used to
// close at its first frame with "no usable Vulkan device" in the log only. Debian 11's and Ubuntu 20.04's own Mesa
// (20.3, 21.2) offer Vulkan 1.2 on AMD and Intel (containers of both, 18/11/2026); Mesa's RADV and ANV have 1.3 since
// 22.0, NVIDIA's driver since 510. Checked again when the Screen tab's GPU (MKW_GPU_INDEX, MKW_GPU_NAME) changes.
static VkGpuReport g_gpu;
static char g_gpu_key[96] = "\x01";   // the GPU settings the report was made with ("\x01": not made yet)

// The GPU settings as pick_device reads them (cfg_get: MKW_GPU_NAME has no settings row).
static void gpu_key(char *out, size_t cap) {
    const char *i = cfg_get("MKW_GPU_INDEX"), *n = cfg_get("MKW_GPU_NAME");
    snprintf(out, cap, "%s|%s", i ? i : "", n ? n : "");
}

// The Vulkan devices for the Screen tab's GPU row and the check above: an instance only to list them. No instance at
// all (no Vulkan driver): a report of no devices.
static void list_gpus(void) {
    gpu_key(g_gpu_key, sizeof g_gpu_key);
    VkApplicationInfo ai = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    ai.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ci = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &ai;
    VkInstance inst;
    memset(&g_gpu, 0, sizeof g_gpu);
    g_gpu.best = -1;
    if (vkCreateInstance(&ci, nullptr, &inst) == VK_SUCCESS) {
        settings_list_gpus(inst);
        vk_gpu_report(inst, &g_gpu);
        vkDestroyInstance(inst, nullptr);
    }
    if (g_gpu.best >= 0 && g_gpu.gpu[g_gpu.best].type != VK_PHYSICAL_DEVICE_TYPE_CPU)
        rt_log("launcher: the game would run on %s", g_gpu.gpu[g_gpu.best].name);
    else if (g_gpu.best >= 0) rt_log("launcher: only a software renderer can run the game: %s", g_gpu.gpu[g_gpu.best].name);
    else rt_log("launcher: no GPU here can run the game (%d listed)", g_gpu.n);
    for (int i = 0; i < g_gpu.n; i++)
        if (g_gpu.gpu[i].why != VK_GPU_OK)
            rt_log("launcher:   %s (Vulkan %u.%u, %s): %s%s", g_gpu.gpu[i].name, VK_API_VERSION_MAJOR(g_gpu.gpu[i].api),
                   VK_API_VERSION_MINOR(g_gpu.gpu[i].api), g_gpu.gpu[i].driver[0] ? g_gpu.gpu[i].driver : "driver unknown",
                   g_gpu.gpu[i].why == VK_GPU_OLD   ? "Vulkan below 1.3"
                   : g_gpu.gpu[i].why == VK_GPU_EXT ? "missing extension "
                   : g_gpu.gpu[i].why == VK_GPU_QUEUE ? "no graphics and compute queue"
                                                      : "not the one MKW_GPU_INDEX or MKW_GPU_NAME names",
                   g_gpu.gpu[i].why == VK_GPU_EXT ? g_gpu.gpu[i].ext : "");
}

// The GPU the game would use cannot run it (none, or only a CPU renderer): the main page's warning and the files
// page's card. The report is made again when the GPU settings change.
static bool gpu_bad(void) {
    char key[sizeof g_gpu_key];
    gpu_key(key, sizeof key);
    if (strcmp(key, g_gpu_key)) list_gpus();
    return g_gpu.best < 0 || g_gpu.gpu[g_gpu.best].type == VK_PHYSICAL_DEVICE_TYPE_CPU;
}

static void parse_test(void) {
    const char *e = getenv("MKW_LAUNCHER_SHOT");
    if (!e || !*e) return;
    L.shot_at = atol(e);
    const char *p = strchr(e, ':');
    if (!p) return;
    p++;
    L.shot_page = !strncmp(p, "settings", 8) ? PAGE_SETTINGS : !strncmp(p, "files", 5) ? PAGE_FILES
                  : !strncmp(p, "mods", 4)                                              ? PAGE_MODS
                  : !strncmp(p, "players", 7)                                           ? PAGE_PLAYERS
                  : !strncmp(p, "tools", 5)                                             ? PAGE_TOOLS
                                                                                        : PAGE_MAIN;
    const char *t = strchr(p, ':');
    L.shot_tab = t ? atoi(t + 1) : 0;
    const char *sc = t ? strchr(t + 1, ':') : NULL;   // settings:<tab>:<scroll 0..1>
    if (sc) opt_test_scroll((float)atof(sc + 1));
}

static void restyle(float scale) {
    if (scale == L.scale && !menu_look_fonts_stale()) return;
    ImGui::GetStyle() = L.base_style;
    ImGui::GetStyle().ScaleAllSizes(scale);
    menu_look_fonts(scale);
    L.head = menu_look_font_at(36.0f * scale);
    L.big = menu_look_font_at(60.0f * scale);
    L.item = menu_look_font_at(30.0f * scale);
    ImGui_ImplSDLRenderer3_DestroyFontsTexture();
    ImGui_ImplSDLRenderer3_CreateFontsTexture();
    L.scale = scale;
}

// ---- the pages
static ImVec4 col4(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

static ImU32 mix(ImU32 a, ImU32 b, float t) {
    ImVec4 x = col4(a), y = col4(b);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(x.x + (y.x - x.x) * t, x.y + (y.y - x.y) * t, x.z + (y.z - x.z) * t,
                                                 x.w + (y.w - x.w) * t));
}

// 0..1 following on (the cursor over a button) with an exponential ease-out, kept per id in the window's storage.
// During a shot the end value at once: a frame taken on the way would differ from run to run.
static float eased(ImGuiID id, bool on) {
    float *v = ImGui::GetStateStorage()->GetFloatRef(id, 0);
    float to = on ? 1.0f : 0.0f;
    *v = L.shot_at ? to : *v + (to - *v) * (1 - expf(-16 * ImGui::GetIO().DeltaTime));
    return *v;
}

// Element i of the main page's intro (launcher_logo.h); during a shot, or once it has played, 1.
static float intro(int i) { return launcher_intro(L.shot_at ? 0 : L.intro_t0, i); }
static void intro_apply(ImDrawList *dl, int start, float k) { launcher_intro_apply(dl, start, k, L.scale); }

static ImU32 with_alpha(ImU32 c, int a) { return (c & ~IM_COL32_A_MASK) | ((ImU32)a << IM_COL32_A_SHIFT); }

// The start screen's drawn buttons (Play, the profile, the menu rows): an invisible button with the hand cursor, how
// far its hover has eased in (mouse or the controller's cursor), and whether the controller's cursor is on it.
struct Hot {
    bool hit, held, cursor;
    float t;
};

static Hot hot_button(const char *id, ImVec2 size, bool enabled = true) {
    ImGui::BeginDisabled(!enabled);
    Hot h;
    // ImGui 1.91 leaves an InvisibleButton out of navigation unless asked (imgui_widgets.cpp, ItemAdd's NoNav): without
    // EnableNav the controller reaches only Play, which SetKeyboardFocusHere puts the cursor on. ImGui's own cursor is
    // hidden: each button draws its own (focus_ring, or the menu row's band), as the in-game menu's rows do.
    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
    h.hit = ImGui::InvisibleButton(id, size, ImGuiButtonFlags_EnableNav);
    ImGui::PopStyleColor();
    bool over = ImGui::IsItemHovered();
    if (enabled && over) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);   // it can be clicked
    h.held = ImGui::IsItemActive();
    h.cursor = ImGui::IsItemFocused() && ImGui::GetIO().NavVisible;
    h.t = eased(ImGui::GetItemID(), enabled && (over || h.cursor));
    ImGui::EndDisabled();
    return h;
}

// Text on one line from pos to max_x in col, cut with an ellipsis when it does not fit (a long name).
static void text_fit(ImDrawList *dl, ImVec2 pos, float max_x, ImU32 col, const char *text) {
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::RenderTextEllipsis(dl, pos, ImVec2(max_x, pos.y + ImGui::GetTextLineHeight()), max_x, max_x, text, nullptr,
                              nullptr);
    ImGui::PopStyleColor();
}

// The controller's cursor around a drawn button, a little outside it.
static void focus_ring(ImVec2 a, ImVec2 b) {
    float s = L.scale;
    ImGui::GetWindowDrawList()->AddRect(ImVec2(a.x - 4 * s, a.y - 4 * s), ImVec2(b.x + 4 * s, b.y + 4 * s), LOOK_FOCUS,
                                        6 * s, 0, 2 * s);
}

// The main page's one blue button: a play triangle and the label on the left, in the heading's size.
static bool play_button(const char *label, ImVec2 size, bool enabled) {
    float s = L.scale;
    ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + size.x, a.y + size.y);
    Hot h = hot_button("##play", size, enabled);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImU32 fill = !enabled ? LOOK_RAISED : h.held ? LOOK_ACCENT_DOWN : mix(LOOK_ACCENT, LOOK_ACCENT_HOT, h.t);
    ImU32 text = enabled ? IM_COL32(255, 255, 255, 255) : LOOK_OFF;
    dl->AddRectFilled(a, b, fill, 4 * s);
    if (h.cursor) focus_ring(a, b);
    float fs = L.head->FontSize, k = fs * 0.32f, gap = 18 * s;
    float x = a.x + 28 * s, cy = (a.y + b.y) * 0.5f;   // on the left, in line with the menu's labels
    dl->AddTriangleFilled(ImVec2(x, cy - k), ImVec2(x + k * 1.6f, cy), ImVec2(x, cy + k), text);
    dl->AddText(L.head, fs, ImVec2(x + k * 1.6f + gap, cy - fs * 0.5f), text, label);
    return h.hit;
}

// The profile that plays, beside Play: its initial in a disc and its name, clipped to the chip; it opens the Profiles
// page. A value with a place behind it, so a chip and not a menu row ("Profile: Player" read as both).
static bool profile_chip(ImVec2 size) {
    float s = L.scale, lh = ImGui::GetTextLineHeight();
    ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + size.x, a.y + size.y);
    Hot h = hot_button("##profile", size);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(a, b, mix(LOOK_ROW, LOOK_ROW_HOT, h.t), 4 * s);
    dl->AddRect(a, b, LOOK_LINE, 4 * s, 0, s);
    if (h.cursor) focus_ring(a, b);
    const char *name = launcher_player_name();
    unsigned char c0 = (unsigned char)name[0];   // the first UTF-8 character, upper case when it is ASCII
    int n = c0 < 0x80 ? 1 : c0 < 0xE0 ? 2 : c0 < 0xF0 ? 3 : 4;
    char initial[5] = {};
    for (int i = 0; i < n && name[i]; i++) initial[i] = name[i];
    if (c0 < 0x80) initial[0] = (char)toupper(c0);
    float r = size.y * 0.24f, cx = a.x + 12 * s + r, cy = (a.y + b.y) * 0.5f, caret = 5 * s;
    dl->AddCircleFilled(ImVec2(cx, cy), r, mix(LOOK_RAISED, LOOK_HOVER, h.t), 24);
    ImVec2 isz = ImGui::CalcTextSize(initial);
    dl->AddText(ImVec2(cx - isz.x * 0.5f, cy - isz.y * 0.5f), LOOK_TEXT, initial);
    float tx = cx + r + 10 * s, cr = b.x - 14 * s;
    text_fit(dl, ImVec2(tx, cy - lh * 0.5f), cr - caret * 2 - 8 * s, mix(LOOK_HEADING, LOOK_TEXT, h.t), name);
    dl->AddTriangleFilled(ImVec2(cr - caret * 2, cy - caret * 0.5f), ImVec2(cr, cy - caret * 0.5f),
                          ImVec2(cr - caret, cy + caret * 0.5f), LOOK_DIM);
    return h.hit;
}

// Under Play, whether a controller is there before the race starts: the first gamepad's name (and how many more), or
// that the keyboard and mouse work; it opens the Controls tab.
static bool pad_row(float w, float hgt) {
    float s = L.scale, lh = ImGui::GetTextLineHeight();
    ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + w, a.y + hgt);
    Hot h = hot_button("##pads", ImVec2(w, hgt));
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(a, b, mix(with_alpha(LOOK_ROW, 0), LOOK_ROW_HOT, h.t), 4 * s);
    dl->AddRect(a, b, LOOK_LINE, 4 * s, 0, s);
    if (h.cursor) focus_ring(a, b);
    char text[200];
    if (L.pads > 0) {
        int n = snprintf(text, sizeof text, menu_text(UI_LAUNCH_PAD), L.pad_name);
        InputPower w;
        char power[64];
        // its battery when it gives one (discussion #516); hovered, every controller's
        if (pad_power(0, &w, power, sizeof power) && w.state != INPUT_POWER_UNKNOWN && n > 0 && n < (int)sizeof text)
            n += snprintf(text + n, sizeof text - n, " · %s", power);
        if (L.pads > 1 && n > 0 && n < (int)sizeof text) snprintf(text + n, sizeof text - n, " +%d", L.pads - 1);
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            for (int i = 0; pad_power(i, &w, power, sizeof power); i++)
                ImGui::Text(menu_text(UI_MENU_PAD_PLAYER), i + 1, w.name), ImGui::SameLine(0, 8 * s),
                    ImGui::TextDisabled("%s", power);
            ImGui::EndTooltip();
        }
    } else {
        snprintf(text, sizeof text, "%s", menu_text(UI_LAUNCH_PAD_NONE));
    }
    const char *go = menu_text(UI_LAUNCH_CONFIGURE);
    float cy = (a.y + b.y) * 0.5f, ty = cy - lh * 0.5f, gw = ImGui::CalcTextSize(go).x, x = a.x + 14 * s;
    dl->AddCircleFilled(ImVec2(x + 4 * s, cy), 4 * s, L.pads > 0 ? LOOK_OK : LOOK_DIM, 12);
    text_fit(dl, ImVec2(x + 18 * s, ty), b.x - gw - 28 * s, mix(LOOK_HEADING, LOOK_TEXT, h.t), text);
    dl->AddText(ImVec2(b.x - 14 * s - gw, ty), mix(LOOK_ACCENT_HOT, LOOK_TEXT, h.t), go);
    return h.hit;
}

// The height of a row of the main page's menu and of a page's buttons: 60 at 1080 lines, never under 44 pixels (the
// smallest target a finger or a hurried mouse hits; at 720 lines 60 would give 40).
static float item_height(void) { return fmaxf(44.0f, 60 * L.scale); }

// A row of the main page's menu: the label alone at rest; under the mouse or the controller's cursor a band of the
// accent that fades out to the right comes in under it (as Dusklight's start screen does), with a bar at its left edge.
static bool menu_item(const char *label, float width, bool enabled) {
    float s = L.scale, hgt = item_height();
    ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + width, a.y + hgt);
    ImGui::PushID(label);
    Hot h = hot_button("##item", ImVec2(width, hgt), enabled);
    ImGui::PopID();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    if (h.t > 0.01f) {
        ImU32 c = h.held ? LOOK_ACCENT_DOWN : LOOK_ACCENT;
        ImU32 from = with_alpha(c, (int)(217 * h.t)), to = with_alpha(c, 0);
        dl->AddRectFilledMultiColor(a, b, from, to, to, from);
        dl->AddRectFilled(a, ImVec2(a.x + 3 * s, b.y), with_alpha(IM_COL32_WHITE, (int)(255 * h.t)));
    }
    float fs = L.item->FontSize;
    dl->AddText(L.item, fs, ImVec2(a.x + 24 * s + 4 * s * h.t, (a.y + b.y - fs) * 0.5f),
                enabled ? mix(LOOK_HEADING, LOOK_TEXT, h.t) : LOOK_OFF, label);
    return h.hit;
}

// A bordered card around what is drawn between card_begin and card_end, w wide and as tall as its contents. Its fill is
// drawn under them on a second draw channel once their height is known. Cards do not nest.
static struct {
    ImVec2 a;
    float w;
} g_card;

static float card_pad(void) { return 16 * L.scale; }

static void card_begin(float w) {
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->ChannelsSplit(2);
    dl->ChannelsSetCurrent(1);
    g_card.a = ImGui::GetCursorScreenPos();
    g_card.w = w;
    float p = card_pad();
    ImGui::SetCursorScreenPos(ImVec2(g_card.a.x + p, g_card.a.y + p));
    ImGui::BeginGroup();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w - 2 * p);
}

static void card_end(ImU32 edge, ImU32 fill) {
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
    float s = L.scale;
    ImVec2 b(g_card.a.x + g_card.w, ImGui::GetItemRectMax().y + card_pad());
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->ChannelsSetCurrent(0);
    dl->AddRectFilled(g_card.a, b, fill, 6 * s);
    dl->AddRect(g_card.a, b, edge, 6 * s, 0, 1.5f * s);
    dl->ChannelsMerge();
    ImGui::SetCursorScreenPos(g_card.a);
    ImGui::Dummy(ImVec2(g_card.w, b.y - g_card.a.y));
}

// The page's one main action in the accent, white on blue as Play.
static bool accent_button(const char *label, ImVec2 size = ImVec2(0, 0)) {
    ImGui::PushStyleColor(ImGuiCol_Button, col4(LOOK_ACCENT));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, col4(LOOK_ACCENT_HOT));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, col4(LOOK_ACCENT_DOWN));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
    bool hit = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return hit;
}

// The desktop shortcut to this executable, the system's own kind (platform.h plat_desktop_shortcut); the result shows
// under the button. From the Linux AppImage the shortcut starts the AppImage ($APPIMAGE, set by its runtime): the copy of
// mkw its AppRun runs needs the AppImage's libraries and its setup/ link.
static void make_shortcut(void) {
    char exe[1100], made[1100];
    const char *appimage = getenv("APPIMAGE");
    if (appimage && *appimage && strlen(appimage) < sizeof exe) snprintf(exe, sizeof exe, "%s", appimage);
    else if (!plat_exe_path(exe, sizeof exe)) return;
    int ok = plat_desktop_shortcut("Mario Kart World", exe, L.exe_dir, made, sizeof made);
    snprintf(L.shortcut_note, sizeof L.shortcut_note, menu_text(ok ? UI_LAUNCH_SHORTCUT_DONE : UI_LAUNCH_SHORTCUT_FAILED),
             made);
    rt_log("launcher: desktop shortcut %s %s", ok ? "made:" : "failed", ok ? made : "");
}

static const UiTextId kStepText[SETUP_STEPS] = {UI_LAUNCH_STEP_CHECK, UI_LAUNCH_STEP_EXEFS, UI_LAUNCH_STEP_ROMFS,
                                                UI_LAUNCH_STEP_GENERATE, UI_LAUNCH_STEP_COMPILE, UI_LAUNCH_STEP_SHADERS,
                                                UI_LAUNCH_STEP_WARM, UI_LAUNCH_STEP_CLEANUP};

// The update offered (launcher_update.h) as a card in the accent: the release, the version in use and the download's
// size, with Update and a link to the release's notes; then the download's progress, and its setup beside the version
// played (Play stays the first button and works all along, #429); once set up, Restart with it. A failed download in
// red with its reason and Try again. A failed check shows nothing.
static void update_card(float w) {
    UpdateState u = update_state();
    bool failed = u.phase == UPDATE_FAILED && u.tag[0];
    if (u.phase == UPDATE_READY || u.phase == UPDATE_PREPARING) u.phase = UPDATE_PREPARING;   // READY: starting it
    if (u.phase != UPDATE_OFFER && u.phase != UPDATE_FETCHING && u.phase != UPDATE_PREPARING &&
        u.phase != UPDATE_PREPARED && !failed)
        return;
    char text[700];
    ImGui::PushID("update");   // its Try again and the failed setup's share a page (files) and a label
    card_begin(w);
    if (failed) {
        snprintf(text, sizeof text, menu_text(UI_LAUNCH_UPDATE_FAILED), u.error);
        ImGui::TextColored(col4(LOOK_BAD), "%s", text);
        if (ImGui::Button(menu_text(UI_LAUNCH_RETRY))) update_fetch_start(L.exe_dir);
    } else if (u.phase == UPDATE_OFFER) {
        snprintf(text, sizeof text, menu_text(UI_LAUNCH_UPDATE_TITLE), u.tag);
        ImGui::TextUnformatted(text);
        snprintf(text, sizeof text, menu_text(UI_LAUNCH_UPDATE_DETAIL), rt_port_version,
                 (int)((u.bytes + (1 << 19)) >> 20));
        ImGui::TextColored(col4(LOOK_HEADING), "%s", text);
        if (ImGui::Button(menu_text(UI_LAUNCH_UPDATE_NOW)) || getenv("MKW_LAUNCHER_UPDATE")) update_fetch_start(L.exe_dir);
        ImGui::SameLine();
        if (ImGui::Button(menu_text(UI_LAUNCH_WHATS_NEW))) {
            snprintf(text, sizeof text, "https://github.com/%s/releases/tag/%s", repo(), u.tag);
            SDL_OpenURL(text);
        }
    } else if (u.phase == UPDATE_PREPARING) {
        snprintf(text, sizeof text, menu_text(UI_LAUNCH_UPDATE_PREPARING), u.tag);
        ImGui::TextWrapped("%s", text);
        snprintf(text, sizeof text, "%s", u.step >= 0 ? menu_text(kStepText[u.step]) : "");
        ImGui::ProgressBar(u.total > 0 ? (float)u.done / (float)u.total : 0, ImVec2(w - 2 * card_pad(), 0), text);
    } else if (u.phase == UPDATE_PREPARED) {
        snprintf(text, sizeof text, menu_text(UI_LAUNCH_UPDATE_PREPARED), u.tag);
        ImGui::TextUnformatted(text);
        if (u.error[0]) {
            snprintf(text, sizeof text, menu_text(UI_LAUNCH_UPDATE_PREPARE_FAILED), u.error);
            ImGui::TextColored(col4(LOOK_HEADING), "%s", text);
        }
        bool press = ImGui::Button(menu_text(UI_LAUNCH_UPDATE_RESTART)) || getenv("MKW_LAUNCHER_RESTART");
        if (press && update_apply(L.exe_dir))
            L.relaunch = 1, L.result = LAUNCHER_QUIT;
    } else {
        snprintf(text, sizeof text, menu_text(UI_LAUNCH_UPDATE_FETCHING), u.tag);
        ImGui::TextUnformatted(text);
        snprintf(text, sizeof text, "%lld / %lld MB", u.done >> 20, u.total >> 20);
        ImGui::ProgressBar(u.total > 0 ? (float)u.done / (float)u.total : 0, ImVec2(w - 2 * card_pad(), 0), text);
    }
    card_end(failed ? LOOK_BAD : LOOK_ACCENT_HOT, with_alpha(failed ? LOOK_BAD : LOOK_ACCENT, 40));
    ImGui::PopID();
}

// After the game is set up, once: the desktop shortcut offered on the main page until it is made or put off. The
// button stays on the Tools page; MKW_SHORTCUT_ASKED (no settings row, like MKW_PLAYER) keeps the answer.
static bool shortcut_asked(void) {
    const char *v = cfg_get("MKW_SHORTCUT_ASKED");
    return v && *v && strcmp(v, "0");
}

enum { JOB_BACKUP = 1, JOB_STEAM };   // the Tools page's jobs (launcher_tools.h), the card's Add to Steam too
static bool shortcut_target(char *exe, size_t cap);

// Add to Steam: tools/prepare/steam.py adds this program as a non-Steam game, with its library images, so Big Picture
// and Gaming Mode (Bazzite, a Steam Deck) start it; the Tools page and the card below both run it.
// The images are drawn first (steam_art_draw, between two frames), then the job runs with them.
static void steam_add(void) { L.steam_art = 1; }

enum { ART_PORTRAIT, ART_WIDE, ART_HERO, ART_LOGO, ART_ICON };

// The width of art_header's logo and title at logo m px and title t px.
static float art_header_w(float m, float t) { return m * 1.28f + L.big->CalcTextSizeA(t, FLT_MAX, 0, "Mario Kart World").x; }

// The port's logo and the title, as the main page's header, its top left at p: the logo m px, the title t px.
static void art_header(ImDrawList *dl, ImVec2 p, float m, float t, bool sub) {
    launcher_logo(dl, p, m);
    float x = p.x + m + m * 0.28f, sh = t * 0.42f, gap = t * 0.08f, th = t + (sub ? gap + sh : 0);
    float y = p.y + (m - th) * 0.5f;
    dl->AddText(L.big, t, ImVec2(x, y), LOOK_TEXT, "Mario Kart World");
    if (sub) dl->AddText(nullptr, sh, ImVec2(x + t * 0.03f, y + t + gap), LOOK_HEADING, menu_text(UI_LAUNCH_SUBTITLE));
}

// One of Steam's library images in the start screen's look: its backdrop (the waving chequered flag, or the player's
// splash.png), the port's logo and the title; no art of the game. Drawn by ImGui into a render target and saved as
// PNG, between two frames of the start screen.
static bool art_one(int kind, int w, int h, const char *path) {
    SDL_Texture *t = SDL_CreateTexture(L.ren, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET, w, h);
    if (!t) return false;
    SDL_SetRenderTarget(L.ren, t);
    SDL_SetRenderDrawColor(L.ren, 0, 0, 0, 0);
    SDL_RenderClear(L.ren);
    ImGuiIO &io = ImGui::GetIO();
    ImGui_ImplSDLRenderer3_NewFrame();
    io.DisplaySize = ImVec2((float)w, (float)h);
    io.DisplayFramebufferScale = ImVec2(1, 1);
    ImGui::NewFrame();
    ImDrawList *dl = ImGui::GetBackgroundDrawList();
    float fw = (float)w, fh = (float)h;
    ImU32 dark = IM_COL32(18, 21, 27, 230), none = IM_COL32(18, 21, 27, 0);
    if (kind == ART_PORTRAIT || kind == ART_WIDE || kind == ART_HERO) backdrop_draw(dl, fw, fh, true);
    if (kind == ART_PORTRAIT) {   // a box's front: the logo and the title centred low, over a dark band
        dl->AddRectFilledMultiColor(ImVec2(0, fh * 0.45f), ImVec2(fw, fh), none, none, dark, dark);
        float m = fw * 0.34f, t = fw * 0.105f;
        launcher_logo(dl, ImVec2((fw - m) * 0.5f, fh * 0.50f), m);
        ImVec2 ts = L.big->CalcTextSizeA(t, FLT_MAX, 0, "Mario Kart World");
        dl->AddText(L.big, t, ImVec2((fw - ts.x) * 0.5f, fh * 0.50f + m + t * 0.35f), LOOK_TEXT, "Mario Kart World");
        const char *sub = menu_text(UI_LAUNCH_SUBTITLE);
        float st = t * 0.42f;
        ImVec2 ss = ImGui::GetFont()->CalcTextSizeA(st, FLT_MAX, 0, sub);
        dl->AddText(nullptr, st, ImVec2((fw - ss.x) * 0.5f, fh * 0.50f + m + t * 1.45f), LOOK_HEADING, sub);
    } else if (kind == ART_WIDE) {   // the main page's header on the left, over a dark side
        dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(fw * 0.7f, fh), dark, none, none, dark);
        float m = fh * 0.30f;
        art_header(dl, ImVec2(fw * 0.07f, (fh - m) * 0.5f), m, m * 0.62f, true);
    } else if (kind == ART_LOGO) {   // over Steam's hero: the header alone, on nothing, as wide as fits
        float m = fh * 0.8f, k = fminf(1.0f, fw * 0.96f / art_header_w(m, m * 0.62f));
        m *= k;
        art_header(dl, ImVec2((fw - art_header_w(m, m * 0.62f)) * 0.5f, (fh - m) * 0.5f), m, m * 0.62f, false);
    } else if (kind == ART_ICON) {
        launcher_logo(dl, ImVec2(0, 0), fw);
    }
    ImGui::Render();
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), L.ren);
    SDL_Surface *px = SDL_RenderReadPixels(L.ren, nullptr);
    if (px && (kind == ART_LOGO || kind == ART_ICON)) {   // on nothing: the edges blended with the clear black, undone
        SDL_Surface *c = SDL_ConvertSurface(px, SDL_PIXELFORMAT_RGBA32);
        SDL_DestroySurface(px);
        px = c;
        for (int y = 0; px && y < px->h; y++) {
            uint8_t *r = (uint8_t *)px->pixels + (size_t)y * px->pitch;
            for (int x = 0; x < px->w; x++, r += 4)
                if (r[3] && r[3] < 255)
                    for (int i = 0; i < 3; i++) r[i] = (uint8_t)(r[i] * 255 / r[3] > 255 ? 255 : r[i] * 255 / r[3]);
        }
    }
    bool ok = px && SDL_SavePNG(px, path);
    SDL_DestroySurface(px);
    SDL_SetRenderTarget(L.ren, nullptr);
    SDL_DestroyTexture(t);
    return ok;
}

// Add to Steam's images into <game>/steam-art, then tools/prepare/steam.py with them (--art): Big Picture and Gaming
// Mode show a console's library, and a grey tile or a bare flag looked like a tool there (17/11/2026). Called between
// two frames of the start screen.
static void steam_art_draw(void) {
    L.steam_art = 0;
    char dir[PLAT_PATH_MAX * 2], path[PLAT_PATH_MAX * 2 + 32], exe[PLAT_PATH_MAX * 2];
    snprintf(dir, sizeof dir, "%s/steam-art", cfg_game_dir());
    SDL_CreateDirectory(dir);
    static const struct { int kind, w, h; const char *name; } kArt[] = {
        {ART_PORTRAIT, 600, 900, "portrait.png"}, {ART_WIDE, 920, 430, "wide.png"}, {ART_HERO, 1920, 620, "hero.png"},
        {ART_LOGO, 1280, 360, "logo.png"},        {ART_ICON, 256, 256, "icon.png"}};
    int drawn = 0;
    restyle(2.0f);   // fonts drawn at twice the 1080-line size: the titles are up to 200 px high, not stretched from 60
    for (const auto &a : kArt) {
        snprintf(path, sizeof path, "%s/%s", dir, a.name);
        drawn += art_one(a.kind, a.w, a.h, path);
    }   // the next frame's restyle puts the window's own scale back
    rt_log("launcher: Steam's library images drawn: %d of 5 in %s", drawn, dir);
    if (!shortcut_target(exe, sizeof exe)) return;
    const char *args[] = {"add", "--name", "Mario Kart World", "--exe", exe, "--dir", L.exe_dir,
                          drawn == 5 ? "--art" : nullptr, dir, nullptr};
    tool_start(L.exe_dir, JOB_STEAM, "prepare/steam.py", args);
}

// Add to Steam's result in words ("" while it has none): added, added with Steam to restart, or why it failed.
static void steam_note(const ToolState &job, char *note, size_t cap, bool *failed) {
    int users = 0, running = 0, skipped = 0;
    *failed = job.job == JOB_STEAM && job.phase == TOOL_FAILED;
    note[0] = 0;
    if (job.job == JOB_STEAM && job.phase == TOOL_DONE && sscanf(job.line, "%d %d %d", &users, &running, &skipped) >= 2) {
        if (skipped) {
            *failed = true;
            snprintf(note, cap, menu_text(running ? UI_TOOLS_STEAM_PARTIAL_RESTART : UI_TOOLS_STEAM_PARTIAL),
                     users, users + skipped);
        } else {
            snprintf(note, cap, "%s", menu_text(running ? UI_TOOLS_STEAM_RESTART : UI_TOOLS_STEAM_DONE));
        }
    } else if (*failed) {
        snprintf(note, cap, menu_text(UI_TOOLS_FAILED),
                 !strncmp(job.line, "nosteam", 7) ? menu_text(UI_TOOLS_STEAM_NONE) : job.line);
    }
}

// The card's Add to Steam was pressed this start: the card stays, with its result, until the start screen closes.
static bool steam_card;

// The next button on this line when it fits the card, else on the next one (three buttons overflow the main page's
// column in Spanish and German).
static void same_line_if_fits(const char *label, float right) {
    float gap = 8 * L.scale, need = ImGui::CalcTextSize(label).x + 2 * ImGui::GetStyle().FramePadding.x;
    float x = ImGui::GetItemRectMax().x + gap;
    if (x + need <= right) ImGui::SameLine(0, gap);
}

static void shortcut_card(float w) {
    ToolState job = tool_state();
    card_begin(w);
    float right = ImGui::GetCursorScreenPos().x + w - 2 * card_pad();
    ImGui::TextUnformatted(menu_text(UI_LAUNCH_SHORTCUT_ASK));
    bool make = ImGui::Button(menu_text(UI_LAUNCH_SHORTCUT_MAKE));
    same_line_if_fits(menu_text(UI_TOOLS_STEAM), right);
    // Steam's Big Picture and Gaming Mode start the game from its library: one press here instead of finding the Tools
    // page (a controller-only player on Bazzite or a Steam Deck)
    ImGui::BeginDisabled(job.phase == TOOL_RUNNING);
    bool steam = ImGui::Button(menu_text(UI_TOOLS_STEAM));
    ImGui::EndDisabled();
    same_line_if_fits(menu_text(UI_LAUNCH_NOT_NOW), right);
    bool later = ImGui::Button(menu_text(UI_LAUNCH_NOT_NOW));
    char note[800];
    bool failed = false;
    if (steam_card) steam_note(job, note, sizeof note, &failed);
    if (steam_card && note[0]) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w - 2 * card_pad());
        ImGui::TextColored(col4(failed ? LOOK_WARN : LOOK_OK), "%s", note);
        ImGui::PopTextWrapPos();
    }
    card_end(LOOK_LINE, LOOK_ROW);
    if (make) make_shortcut();
    if (steam) steam_add(), steam_card = true;
    if (make || later || steam) {
        cfg_set("MKW_SHORTCUT_ASKED", "1");
        opt_save_soon();
    }
}

// How the last game ended (launcher_lastrun.h), when it did not end well: a known problem with the button to its
// issue, one an update fixes (the update's own card offers it), or an unknown error with the bug form. Close hides it.
static void lastrun_row(float bw) {
    LastRun r = lastrun_state();
    if (r.kind == LASTRUN_NONE) return;
    char text[700], button[160], url[300];
    url[0] = 0;
    if (r.kind == LASTRUN_KNOWN) {
        snprintf(text, sizeof text, menu_text(UI_LAUNCH_LASTRUN_KNOWN), r.issue);
        snprintf(button, sizeof button, menu_text(UI_LAUNCH_LASTRUN_OPEN), r.issue);
        snprintf(url, sizeof url, "https://github.com/%s/issues/%d", repo(), r.issue);
    } else if (r.kind == LASTRUN_FIXED) {
        snprintf(text, sizeof text, menu_text(UI_LAUNCH_LASTRUN_FIXED), r.tag);
    } else {
        snprintf(text, sizeof text, "%s", menu_text(UI_LAUNCH_LASTRUN_CRASH));
        snprintf(button, sizeof button, "%s", menu_text(UI_LAUNCH_LASTRUN_REPORT));
        snprintf(url, sizeof url, "https://github.com/%s/issues/new?template=bug_report.yml", repo());
    }
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + bw);
    ImGui::TextColored(col4(r.kind == LASTRUN_FIXED ? LOOK_WARN : LOOK_BAD), "%s", text);
    ImGui::PopTextWrapPos();
    if (url[0]) {
        if (ImGui::SmallButton(button)) SDL_OpenURL(url);
        ImGui::SameLine();
    }
    if (ImGui::SmallButton(menu_text(UI_LAUNCH_LASTRUN_DISMISS))) lastrun_dismiss();
    ImGui::Dummy(ImVec2(0, 10 * L.scale));
}

// The game files' state: a dot and the line, in the state's colour.
static void state_line(bool ok, const char *text) {
    float lh = ImGui::GetTextLineHeight(), r = lh * 0.26f;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + r, p.y + lh * 0.5f), r, ok ? LOOK_OK : LOOK_WARN, 20);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + r * 2 + 10 * L.scale);
    ImGui::TextColored(col4(ok ? LOOK_OK : LOOK_WARN), "%s", text);
}

// gpu_bad's card on the files page: why each GPU cannot run the game, what to install, and that the setup can go on
// (what it prepares does not depend on the driver).
static void gpu_card(float w) {
    card_begin(w);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w - 32 * L.scale);
    ImGui::TextColored(col4(LOOK_WARN), "%s", menu_text(UI_LAUNCH_GPU_NONE));
    if (!g_gpu.n) ImGui::TextWrapped("%s", menu_text(UI_LAUNCH_GPU_NO_DRIVER));
    bool driver = !g_gpu.n || g_gpu.best >= 0;   // what to install: a driver's fault, not only the GPU setting's
    for (int i = 0; i < g_gpu.n; i++) {
        const VkGpuInfo &g = g_gpu.gpu[i];
        char line[600], have[200];
        snprintf(have, sizeof have, "Vulkan %u.%u%s%s", VK_API_VERSION_MAJOR(g.api), VK_API_VERSION_MINOR(g.api),
                 g.driver[0] ? ", " : "", g.driver);
        if (i == g_gpu.best) snprintf(line, sizeof line, menu_text(UI_LAUNCH_GPU_SOFTWARE), g.name);
        else if (g.why == VK_GPU_OLD) snprintf(line, sizeof line, menu_text(UI_LAUNCH_GPU_OLD), g.name, have);
        else if (g.why == VK_GPU_EXT) snprintf(line, sizeof line, menu_text(UI_LAUNCH_GPU_EXT), g.name, have, g.ext);
        else if (g.why == VK_GPU_QUEUE) snprintf(line, sizeof line, menu_text(UI_LAUNCH_GPU_QUEUE), g.name);
        else if (g.why == VK_GPU_SKIPPED) snprintf(line, sizeof line, menu_text(UI_LAUNCH_GPU_SKIPPED), g.name);
        else continue;   // a usable one other than best: not the case gpu_bad shows
        driver |= g.why == VK_GPU_OLD || g.why == VK_GPU_EXT;
        ImGui::Bullet();
        ImGui::TextWrapped("%s", line);
    }
#ifdef _WIN32
    if (driver) ImGui::TextWrapped("%s", menu_text(UI_LAUNCH_GPU_HELP_WIN));
#else
    if (driver) ImGui::TextWrapped("%s", menu_text(UI_LAUNCH_GPU_HELP));
#endif
    ImGui::PopTextWrapPos();
    card_end(LOOK_WARN, with_alpha(LOOK_WARN, 22));
}

// Where the game will read its files (main.c portable_paths, handed over by launcher_set_romfs and _exefs).
static char g_romfs[PLAT_PATH_MAX * 2], g_exefs[PLAT_PATH_MAX * 2];

// The game files the setup left that are not there now, or NULL: a RomFS package or folder (the "link" mode keeps the
// player's own copy where it was) or the exefs/ folder. Issue #486: a Steam Deck whose package had gone from where the
// setup found it booted from Play into a fatal error, and in Gaming Mode the game just closed. Checked at most once a
// second, so a card or drive connected while the start screen is open is seen; the log says it when it changes, not on
// every check.
static const char *files_gone(void) {
    static uint64_t at;
    static const char *gone;
    uint64_t now = SDL_GetTicks();
    if (at && now - at < 1000) return gone;
    at = now;
    const char *was = gone;
    gone = NULL;
    if (!rt_game_present()) return gone;   // not set up: the files page says that already
    if (g_romfs[0] && !plat_dir_exists(g_romfs) && !plat_file_exists(g_romfs)) gone = g_romfs;
    else if (g_exefs[0] && !plat_dir_exists(g_exefs)) gone = g_exefs;
    if (gone != was) {
        if (gone) rt_log("launcher: the game files are not at %s", gone);
        else rt_log("launcher: the game files are back");
    }
    return gone;
}

// The game files' state in words, with the version the last setup installed when it is known.
static void files_state_text(bool ready, char *out, size_t cap) {
    if (files_gone()) {
        snprintf(out, cap, "%s", menu_text(UI_LAUNCH_FILES_GONE));
        return;
    }
    if (ready && L.game_version[0]) snprintf(out, cap, menu_text(UI_LAUNCH_FILES_READY_AS), L.game_version);
    else snprintf(out, cap, "%s", menu_text(ready ? UI_LAUNCH_FILES_READY : UI_LAUNCH_FILES_MISSING));
}

// A state at the lower left, a dot in its colour (ready: the good one, else the warning's); a link to the Game files page.
static bool state_link(const char *id, const char *text, bool ready) {
    float s = L.scale, lh = ImGui::GetTextLineHeight(), r = lh * 0.24f, chev = 4 * s;
    float tw = ImGui::CalcTextSize(text).x;
    ImVec2 a = ImGui::GetCursorScreenPos(), size(r * 2 + 10 * s + tw + 12 * s + chev, lh);
    Hot h = hot_button(id, size);
    if (h.cursor) focus_ring(a, ImVec2(a.x + size.x, a.y + size.y));
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImU32 c = ready ? mix(LOOK_HEADING, LOOK_TEXT, h.t) : LOOK_WARN;
    float cy = a.y + lh * 0.5f, x = a.x + r * 2 + 10 * s;
    dl->AddCircleFilled(ImVec2(a.x + r, cy), r, ready ? LOOK_OK : LOOK_WARN, 16);
    dl->AddText(ImVec2(x, a.y), c, text);
    x += tw + 12 * s;
    ImVec2 v[3] = {ImVec2(x, cy - chev), ImVec2(x + chev, cy), ImVec2(x, cy + chev)};
    dl->AddPolyline(v, 3, c, 0, 1.5f * s);
    return h.hit;
}

// The main page: a column on the left with the title, Play with the profile beside it, the controllers, then Settings,
// Game files, Mods and Tools, Quit last and apart, and once after setup the desktop shortcut; the update at the upper
// right;
// the game files' state at the lower left, the version and the keys at the lower right. One window over the whole page,
// so the controller's cursor reaches the update's buttons from the column. The picture is the background's
// (launcher_backdrop.cpp): the player's splash.png, or the logo's flag.
static void page_main(float w, float h) {
    float s = L.scale, left = 96 * s, cw = 440 * s, edge = 56 * s, lh = ImGui::GetTextLineHeight();
    bool files = rt_game_present() && !files_gone(), ready = files;   // the update never touches the files played
    // the column's height, to centre it: logo and title, the gap, Play, the controllers, the menu, the gap and Quit
    float mark = 84 * s, items = item_height(), gap = 12 * s;
    float play_h = fmaxf(52.0f, 76 * s), pad_h = fmaxf(36.0f, 48 * s);
    float col_h = mark + 48 * s + play_h + gap + pad_h + gap + items * 4 + 28 * s + items;
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(w, h));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, gap));
    ImGui::Begin("##main", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground |
                                        ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 p(left, fmaxf(40 * s, (h - col_h) * 0.45f));
    ImGui::SetCursorScreenPos(p);
    ImGui::BeginGroup();   // the column's lines start at its left edge, not the window's
    int v0 = dl->VtxBuffer.Size;
    launcher_logo(dl, p, mark);
    float ts = L.big->FontSize, tx = p.x + mark + 24 * s, by = p.y + (mark - ts - 4 * s - lh) * 0.5f;
    dl->AddText(L.big, ts, ImVec2(tx, by - ts * 0.06f), LOOK_TEXT, "Mario Kart World");
    dl->AddText(ImVec2(tx, by + ts + 4 * s), LOOK_HEADING, menu_text(UI_LAUNCH_SUBTITLE));
    intro_apply(dl, v0, intro(0));
    ImGui::Dummy(ImVec2(cw, mark + 48 * s - gap));
    lastrun_row(cw);
    if (L.focus_main) {
        ImGui::SetKeyboardFocusHere();
        L.focus_main = 0;
    }
    v0 = dl->VtxBuffer.Size;
    static bool play_test = getenv("MKW_LAUNCHER_PLAY") != nullptr;   // (test) once, as the button does
    float chip_w = fminf(170 * s, cw * 0.4f);
    bool play = play_button(menu_text(UI_LAUNCH_PLAY), ImVec2(cw - chip_w - gap, play_h), ready);
    ImGui::SameLine(0, gap);
    if (profile_chip(ImVec2(chip_w, play_h))) L.page = PAGE_PLAYERS;
    if (play || (play_test && ready)) {
        play_test = false;
        char state[PLAT_PATH_MAX + 32];
        snprintf(state, sizeof state, "%s/setup-state.json", cfg_game_dir());
        if (mods_code_changed() && L.tools && plat_file_exists(state)) {   // a set-up folder (not build/mkw)
            // a code mod changed: the game code is built again first (generate and compile), on the files page
            L.play_after_setup = 1;
            L.page = PAGE_FILES;
            setup_start(L.exe_dir, cfg_game_dir(), nullptr, -1);   // the RomFS mode is not part of this: the record's
        } else {
            mods_merge_sync(L.exe_dir);   // a stale .merged must not boot: usually a no-op fresh-check
            char player[PLAT_PATH_MAX * 2];
            launcher_player_dir(player, sizeof player);
            if (player[0]) mods_before_play(player);
            L.result = LAUNCHER_PLAY;
        }
    }
    intro_apply(dl, v0, intro(1));
    v0 = dl->VtxBuffer.Size;
    if (pad_row(cw, pad_h)) {
        settings_select(TAB_CONTROLS);
        settings_opened();
        L.page = PAGE_SETTINGS;
    }
    intro_apply(dl, v0, intro(2));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    v0 = dl->VtxBuffer.Size;
    if (menu_item(menu_text(UI_LAUNCH_SETTINGS), cw, true)) {
        L.page = PAGE_SETTINGS;
        settings_opened();
    }
    intro_apply(dl, v0, intro(3));
    v0 = dl->VtxBuffer.Size;
    if (menu_item(menu_text(UI_LAUNCH_FILES), cw, true)) L.page = PAGE_FILES;
    intro_apply(dl, v0, intro(4));
    v0 = dl->VtxBuffer.Size;
    if (menu_item(menu_text(UI_LAUNCH_MODS), cw, true)) L.page = PAGE_MODS;
    intro_apply(dl, v0, intro(5));
    v0 = dl->VtxBuffer.Size;
    if (menu_item(menu_text(UI_LAUNCH_TOOLS), cw, true)) L.page = PAGE_TOOLS;
    intro_apply(dl, v0, intro(6));
    ImGui::Dummy(ImVec2(0, 28 * s));
    v0 = dl->VtxBuffer.Size;
    if (menu_item(menu_text(UI_LAUNCH_QUIT), cw, true)) L.result = LAUNCHER_QUIT;
    intro_apply(dl, v0, intro(7));
    ImGui::PopStyleVar();
    v0 = dl->VtxBuffer.Size;
    if (files && (!shortcut_asked() || steam_card)) {
        shortcut_card(cw);
    } else if (L.shortcut_note[0]) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + cw);
        ImGui::TextColored(col4(LOOK_DIM), "%s", L.shortcut_note);
        ImGui::PopTextWrapPos();
    }
    ImGui::EndGroup();
    // the update at the upper right, apart from the column it would push down; a card narrower than its own buttons
    // would wrap them, so on a window too narrow for both it is not drawn here (the files page shows it regardless)
    float uw = fminf(460 * s, w - left - cw - 2 * edge);
    if (uw >= 360 * s) {
        ImGui::SetCursorScreenPos(ImVec2(w - edge - uw, 40 * s));
        update_card(uw);
    }
    ImGui::SetCursorScreenPos(ImVec2(left, h - 48 * s - lh));
    char state[160];
    files_state_text(files, state, sizeof state);
    if (state_link("##status", state, files)) L.page = PAGE_FILES;
    if (gpu_bad()) {   // above it: the files page says why and what to install
        ImGui::SetCursorScreenPos(ImVec2(left, h - 48 * s - lh * 2 - 10 * s));
        if (state_link("##gpu", menu_text(UI_LAUNCH_GPU_NONE), false)) L.page = PAGE_FILES;
    }
    // the project's GitHub page and Discord server above the version, as their marks (brand names: nothing to
    // translate), GitHub on the right as the text link was
    {
        char github[96];
        snprintf(github, sizeof github, "https://github.com/%s", repo());
        struct {
            Brand brand;
            const char *id, *name, *url;
        } links[] = {{BRAND_DISCORD, "##discord", "Discord", "https://discord.gg/8uYBkjQkEg"},
                     {BRAND_GITHUB, "##github", "GitHub", github}};
        float is = roundf(34 * s), gap = 18 * s;
        float x = w - edge - 2 * is - gap, y = h - 48 * s - lh - 2 * (lh + 6 * s) - (is - lh) - 8 * s;
        for (auto &k : links) {
            ImVec2 a(x, y), b(x + is, y + is);
            ImGui::SetCursorScreenPos(a);
            Hot g = hot_button(k.id, ImVec2(is, is));
            if (g.cursor) focus_ring(a, b);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", k.name);
            ImTextureID t = launcher_brand_icon(L.ren, k.brand, (int)is);
            if (t) dl->AddImage(t, a, b, ImVec2(0, 0), ImVec2(1, 1), mix(LOOK_HEADING, LOOK_TEXT, g.t));
            else dl->AddText(a, mix(LOOK_HEADING, LOOK_TEXT, g.t), k.name);
            if (g.hit) SDL_OpenURL(k.url);
            x += is + gap;
        }
    }
    intro_apply(dl, v0, intro(8));   // the lower left comes in last, after Quit's own intro(7)
    ImGui::End();
    ImGui::PopStyleVar(2);

    // at the lower right: that the port ships no game data and its version, over the keys
    ImDrawList *fg = ImGui::GetForegroundDrawList();
    int f0 = fg->VtxBuffer.Size;
    char foot[200];
    snprintf(foot, sizeof foot, "%s · MKW Recomp %s", menu_text(UI_LAUNCH_NO_GAME_DATA), rt_port_version);
    const char *keys = menu_text(UI_LAUNCH_KEYS);
    float right = w - edge, y = h - 48 * s - lh;
    fg->AddText(ImVec2(right - ImGui::CalcTextSize(keys).x, y), LOOK_DIM, keys);
    fg->AddText(ImVec2(right - ImGui::CalcTextSize(foot).x, y - lh - 6 * s), LOOK_DIM, foot);
    intro_apply(fg, f0, intro(7));
}

static bool back_pressed(void) {
    return ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false);
}

static void go_main(void) {
    L.page = PAGE_MAIN;
    L.focus_main = 1;
}

// A page that takes the whole window: its colour to the edges, its contents in a column no wider than the in-game
// menu's (wider, a row's value would be too far from its name), its title with an optional note at the right; returns
// the space its bottom row needs.
static float panel_begin(const char *id, UiTextId title, float w, float h, const char *note = nullptr) {
    float s = L.scale, ww = fminf(w, 1500 * s);
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), ImVec2(w, h), LOOK_PANEL);
    ImGui::SetNextWindowPos(ImVec2(floorf((w - ww) * 0.5f), 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(ww, h), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(64 * s, 44 * s));
    ImGui::Begin(id, nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                  ImGuiWindowFlags_NoBackground);
    ImGui::PopStyleVar();
    ImGui::PushFont(L.head);
    ImGui::TextUnformatted(menu_text(title));
    ImGui::PopFont();
    if (note) {   // on the title's line, at the right, in the dim colour
        ImVec2 t = ImGui::GetItemRectMin(), tb = ImGui::GetItemRectMax();
        float x = ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x;
        ImGui::GetWindowDrawList()->AddText(ImVec2(x - ImGui::CalcTextSize(note).x,
                                                   (t.y + tb.y - ImGui::GetTextLineHeight()) * 0.5f),
                                            LOOK_DIM, note);
    }
    ImGui::Dummy(ImVec2(0, 4 * s));
    const ImGuiStyle &st = ImGui::GetStyle();
    return item_height() + st.ItemSpacing.y * 2;   // the bottom row's Back button (panel_back_row)
}

// The bottom row: the save note on the left, Back on the right. true: Back.
static bool panel_back_row(UiTextId label = UI_LAUNCH_BACK) {
    const ImGuiStyle &st = ImGui::GetStyle();
    ImGui::Spacing();
    float y = ImGui::GetCursorPosY();
    if (!cfg_last_save_ok()) {
        ImGui::SetCursorPosY(y + st.FramePadding.y);
        ImGui::TextColored(col4(LOOK_BAD), "%s", menu_text(UI_MENU_SAVE_FAILED));
    }
    const char *back = menu_text(label);
    float bw = ImGui::CalcTextSize(back).x + st.FramePadding.x * 4;
    ImGui::SetCursorPos(ImVec2(ImGui::GetWindowWidth() - st.WindowPadding.x - bw, y));
    return ImGui::Button(back, ImVec2(bw, item_height()));
}

static void page_players(float w, float h) {
    float bottom = panel_begin("##players", UI_LAUNCH_PLAYERS, w, h);
    launcher_players_page(L.scale, bottom);
    bool back = panel_back_row();
    if (!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) && !ImGui::GetIO().WantTextInput && back_pressed())
        back = true;
    ImGui::End();
    if (back) go_main();
}

static void page_settings(float w, float h) {
    float bottom = panel_begin("##settings", UI_LAUNCH_SETTINGS, w, h, menu_text(UI_LAUNCH_SETTINGS_NOTE));
    settings_tabs(L.scale, bottom);
    bool back = panel_back_row();
    if (!settings_modal() && back_pressed()) back = true;
    settings_tabs_done();
    ImGui::End();
    if (back) go_main();
}

// ---- the game files page: the player's copy, how the RomFS is kept, the setup and its progress
static void SDLCALL picked(void *kind, const char *const *files, int) {
    SDL_Event e = {};
    e.type = L.picked_event;
    e.user.code = kind ? 1 : 0;   // installation directory, else the source dump
    e.user.data1 = files && files[0] ? SDL_strdup(files[0]) : nullptr;
    if (!SDL_PushEvent(&e)) SDL_free(e.user.data1);
}

// Why the setup failed, in the menu language: prepare.py's error code picks the text, its detail (a folder, a build
// id, a tool's English message) goes in as is. An unknown code (a newer prepare.py) shows the detail alone.
static void setup_error_text(const SetupState &st, char *out, size_t cap) {
    static const struct {
        const char *code;
        UiTextId text;
    } kWhy[] = {{"source", UI_SETUP_ERR_SOURCE},       {"nogame", UI_SETUP_ERR_NOGAME},
                {"encrypted", UI_SETUP_ERR_ENCRYPTED}, {"version", UI_SETUP_ERR_VERSION},
                {"romfs", UI_SETUP_ERR_ROMFS},         {"failed", UI_SETUP_ERR_FAILED},
                {"io", UI_SETUP_ERR_IO},               {"stopped", UI_SETUP_ERR_STOPPED},
                {"cancelled", UI_SETUP_ERR_CANCELLED}, {"no_setup", UI_SETUP_ERR_NO_SETUP},
                {"moved", UI_SETUP_ERR_MOVED},         {"mismatch", UI_SETUP_ERR_MISMATCH},
                {"space", UI_SETUP_ERR_SPACE}};
    char why[600];
    snprintf(why, sizeof why, "%s", st.error);
    for (const auto &w : kWhy)
        if (!strcmp(st.code, w.code)) snprintf(why, sizeof why, menu_text(w.text), st.error);
    snprintf(out, cap, menu_text(UI_LAUNCH_SETUP_FAILED), why);
}

// A step's mark, drawn (the font has no ticks): done a green disc with a dark tick, under way a ring with a blue arc going
// round, to come an empty ring.
static void step_mark(ImDrawList *dl, ImVec2 c, float r, bool done, bool now) {
    float s = L.scale;
    if (done) {
        dl->AddCircleFilled(c, r, LOOK_OK, 28);
        ImVec2 tick[3] = {ImVec2(c.x - r * 0.46f, c.y + r * 0.04f), ImVec2(c.x - r * 0.10f, c.y + r * 0.38f),
                          ImVec2(c.x + r * 0.48f, c.y - r * 0.32f)};
        dl->AddPolyline(tick, 3, LOOK_PANEL, 0, 2.4f * s);
        return;
    }
    dl->AddCircle(c, r - s, LOOK_LINE, 28, 2.2f * s);
    if (!now) return;
    float a = L.shot_at ? 0.6f : (float)ImGui::GetTime() * 5.0f;   // still in a shot: the same picture every run
    dl->PathArcTo(c, r - s, a, a + 1.9f, 16);
    dl->PathStroke(LOOK_ACCENT, 0, 2.6f * s);
}

// The setup's steps, one per line: its mark, its name and, for the one under way, a bar to the right edge with the
// count after it (a band going along the bar while the step has no count yet).
static void setup_progress(const SetupState &st) {
    float s = L.scale, lh = ImGui::GetTextLineHeight(), row = lh + 14 * s, r = lh * 0.48f;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    for (int i = 0; i < SETUP_STEPS; i++) {
        bool now = st.running && st.step == i;
        ImVec2 p = ImGui::GetCursorScreenPos();
        float avail = ImGui::GetContentRegionAvail().x, cy = p.y + row * 0.5f, ty = cy - lh * 0.5f;
        ImGui::Dummy(ImVec2(avail, row - ImGui::GetStyle().ItemSpacing.y));
        step_mark(dl, ImVec2(p.x + r, cy), r, st.done[i] != 0, now);
        const char *name = menu_text(kStepText[i]);
        float x = p.x + r * 2 + 14 * s;
        dl->AddText(ImVec2(x, ty), st.done[i] || now ? LOOK_TEXT : LOOK_DIM, name);
        x += ImGui::CalcTextSize(name).x + 18 * s;
        if (st.done[i] == 2) dl->AddText(ImVec2(x, ty), LOOK_DIM, menu_text(UI_LAUNCH_STEP_KEPT));
        if (!now) continue;
        char count[48] = "";
        if (st.total > 0) snprintf(count, sizeof count, "%ld / %ld", st.progress, st.total);
        float cw = count[0] ? ImGui::CalcTextSize(count).x + 16 * s : 0, right = p.x + avail;
        ImVec2 a(x, cy - 5 * s), b(right - cw, cy + 5 * s);
        if (b.x - a.x < 60 * s) continue;   // a narrow window: the name alone
        dl->AddRectFilled(a, b, LOOK_RAISED, 5 * s);
        if (st.total > 0) {
            float f = fminf(1.0f, fmaxf(0.0f, (float)st.progress / (float)st.total));
            dl->AddRectFilled(a, ImVec2(a.x + fmaxf(10 * s, (b.x - a.x) * f), b.y), LOOK_ACCENT, 5 * s);
            dl->AddText(ImVec2(right - cw + 16 * s, ty), LOOK_HEADING, count);
        } else {
            float span = b.x - a.x, band = span * 0.28f;
            float x0 = a.x + (span + band) * (float)fmod(ImGui::GetTime() * 0.7, 1.0) - band;
            dl->AddRectFilled(ImVec2(fmaxf(a.x, x0), a.y), ImVec2(fminf(b.x, x0 + band), b.y), LOOK_ACCENT, 5 * s);
        }
    }
}

// The indent of a step's contents, past its number.
static float step_indent(void) { return ImGui::GetTextLineHeight() * 1.24f + 14 * L.scale; }

// A numbered step's heading: its number in a ring (a tick in a green disc once done) and its title; what follows is
// indented under the title until step_end.
static void step_head(int n, bool done, const char *title) {
    float s = L.scale, lh = ImGui::GetTextLineHeight(), r = lh * 0.62f;
    ImVec2 p = ImGui::GetCursorScreenPos(), c(p.x + r, p.y + lh * 0.5f);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    if (done) {
        step_mark(dl, c, r, true, false);
    } else {
        char d[4];
        snprintf(d, sizeof d, "%d", n);
        ImVec2 ds = ImGui::CalcTextSize(d);
        dl->AddCircle(c, r - s, LOOK_ACCENT_HOT, 28, 2 * s);
        dl->AddText(ImVec2(c.x - ds.x * 0.5f, c.y - ds.y * 0.5f), LOOK_TEXT, d);
    }
    ImGui::SetCursorScreenPos(ImVec2(p.x + step_indent(), p.y));
    ImGui::TextUnformatted(title);
    ImGui::Indent(step_indent());
}

static void step_end(void) {
    ImGui::Unindent(step_indent());
    ImGui::Dummy(ImVec2(0, 10 * L.scale));
}

// One way to keep the RomFS, as a card: a ring with a dot when chosen, its name and what it costs. true: picked.
static bool romfs_card(const char *id, bool on, UiTextId title, UiTextId note, float w) {
    float s = L.scale, lh = ImGui::GetTextLineHeight(), pad = 12 * s, r = lh * 0.42f;
    float tx = pad + r * 2 + 10 * s, wrap = w - tx - pad;
    const char *t = menu_text(title), *n = menu_text(note);
    float nh = ImGui::CalcTextSize(n, nullptr, false, wrap).y, hgt = pad * 2 + lh + 4 * s + nh;
    ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + w, a.y + hgt);
    Hot h = hot_button(id, ImVec2(w, hgt));
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(a, b, on ? with_alpha(LOOK_ACCENT, 34) : mix(LOOK_ROW, LOOK_ROW_HOT, h.t), 4 * s);
    dl->AddRect(a, b, on ? LOOK_ACCENT_HOT : LOOK_LINE, 4 * s, 0, on ? 2 * s : s);
    if (h.cursor) focus_ring(a, b);
    ImVec2 c(a.x + pad + r, a.y + pad + lh * 0.5f);
    dl->AddCircle(c, r, on ? LOOK_ACCENT_HOT : LOOK_DIM, 20, 2 * s);
    if (on) dl->AddCircleFilled(c, r * 0.45f, LOOK_ACCENT_HOT, 16);
    dl->AddText(ImVec2(a.x + tx, a.y + pad), LOOK_TEXT, t);
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(a.x + tx, a.y + pad + lh + 4 * s), LOOK_HEADING, n, nullptr,
                wrap);
    return h.hit;
}

// Where the game is installed: Choose game folder (the launcher reopens with it), then under the buttons why the folder
// chosen cannot be used and that the environment fixes it.
static void game_dir_button(const SetupState &st, float act_h) {
    int up = update_state().phase;   // the update's tools are writing beside the game folder
    ImGui::BeginDisabled(st.running || up == UPDATE_FETCHING || up == UPDATE_READY || up == UPDATE_PREPARING ||
                         cfg_from_env("MKW_GAME_DIR"));
    if (ImGui::Button(menu_text(UI_LAUNCH_PICK_GAME_DIR), ImVec2(0, act_h))) {
        L.picking = 1;
        SDL_ShowOpenFolderDialog(picked, &L.directory_restart, L.win, cfg_game_dir(), false);
    }
    ImGui::EndDisabled();
}

static void game_dir_notes(void) {
    if (cfg_from_env("MKW_GAME_DIR")) ImGui::TextDisabled("%s MKW_GAME_DIR", menu_text(UI_MENU_SET_BY_ENV));
    if (cfg_game_dir_invalid()) {
        ImGui::PushTextWrapPos(0);
#ifdef _WIN32
        ImGui::TextColored(col4(LOOK_BAD), menu_text(UI_LAUNCH_GAME_DIR_INVALID_WIN32), CFG_GAME_DIR_WIN32_MAX);
#else
        ImGui::TextColored(col4(LOOK_BAD), menu_text(UI_LAUNCH_GAME_DIR_INVALID), CFG_GAME_DIR_MAX);
#endif
        ImGui::PopTextWrapPos();
    }
}

// The copies tools/prepare/finddumps.py found in the usual places (launcher_finddumps.h), one row each to pick with
// a controller: in Steam's Gaming Mode the file dialogs above do not open. With no copy chosen yet the first of a
// known version is (a .nup of a version the setup does not take would only fail it), else the first.
static void found_copies(void) {
    FoundDump f[FOUND_MAX];
    int running = 0, n = finddumps_list(f, FOUND_MAX, &running);
    if (!n) {
        if (running) ImGui::TextColored(col4(LOOK_DIM), "%s", menu_text(UI_LAUNCH_FOUND_SEARCHING));
        return;
    }
    if (!L.source[0]) {
        int pick = 0;
        while (pick < n && !strcmp(f[pick].version, "?")) pick++;
        snprintf(L.source, sizeof L.source, "%s", f[pick < n ? pick : 0].path);
    }
    ImGui::Dummy(ImVec2(0, 4 * L.scale));
    ImGui::TextColored(col4(LOOK_HEADING), "%s", menu_text(UI_LAUNCH_FOUND));
    for (int i = 0; i < n; i++) {
        const char *kind = !strcmp(f[i].kind, "folder") ? menu_text(UI_LAUNCH_FOUND_FOLDER)
                         : !strcmp(f[i].kind, "nx")     ? ".nx"
                                                        : ".nup";
        char label[sizeof f[i].path + 96];
        snprintf(label, sizeof label, "Mario Kart World %s  \xc2\xb7  %s  \xc2\xb7  %s##found%d", f[i].version, kind,
                 f[i].path, i);
        if (ImGui::Selectable(label, !strcmp(L.source, f[i].path))) snprintf(L.source, sizeof L.source, "%s", f[i].path);
    }
}

// The setup's three steps: the player's copy, how its RomFS is kept, then Set up with the folder it installs to.
static void setup_steps(const SetupState &st, bool ready, float act_h) {
    float s = L.scale;
    ImGui::BeginDisabled(st.running);
    step_head(1, L.source[0] != 0, menu_text(UI_LAUNCH_SOURCE));
    ImGui::TextColored(col4(L.source[0] ? LOOK_TEXT : LOOK_DIM), "%s",
                       L.source[0] ? L.source : menu_text(UI_LAUNCH_NO_SOURCE));
    if (ImGui::Button(menu_text(UI_LAUNCH_PICK), ImVec2(0, act_h))) {
        L.picking = 1;
        SDL_ShowOpenFolderDialog(picked, nullptr, L.win, nullptr, false);
    }
    ImGui::SameLine();
    static const SDL_DialogFileFilter kPack[] = {{"Switch 2 package (.nup, .nx)", "nup;nx"}};
    if (ImGui::Button(menu_text(UI_LAUNCH_PICK_NUP), ImVec2(0, act_h))) {   // the dump as one file (runtime/sdk/fs/pack.h)
        L.picking = 1;
        SDL_ShowOpenFileDialog(picked, nullptr, L.win, kPack, 1, nullptr, false);
    }
    found_copies();
    step_end();
    step_head(2, false, menu_text(UI_LAUNCH_ROMFS_MODE));
    float cw = fminf((ImGui::GetContentRegionAvail().x - 12 * s) * 0.5f, 440 * s);
    if (romfs_card("##copy", !L.link_romfs, UI_LAUNCH_ROMFS_COPY, UI_LAUNCH_ROMFS_COPY_NOTE, cw)) L.link_romfs = 0;
    ImGui::SameLine(0, 12 * s);
    if (romfs_card("##link", L.link_romfs, UI_LAUNCH_ROMFS_LINK, UI_LAUNCH_ROMFS_LINK_NOTE, cw)) L.link_romfs = 1;
    step_end();
    step_head(3, false, menu_text(UI_LAUNCH_SETUP));
    ImGui::BeginDisabled(!L.source[0]);
    if (accent_button(menu_text(ready ? UI_LAUNCH_SETUP_AGAIN : UI_LAUNCH_SETUP), ImVec2(0, act_h)))
        setup_start(L.exe_dir, cfg_game_dir(), L.source, L.link_romfs);
    ImGui::EndDisabled();
    char to[PLAT_PATH_MAX + 64];
    snprintf(to, sizeof to, menu_text(UI_LAUNCH_INSTALLS_TO), cfg_game_dir());
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(col4(LOOK_HEADING), "%s", to);
    ImGui::PopTextWrapPos();
    if (!ready) {   // set up, the folder changes from the summary's buttons
        game_dir_button(st, act_h);
        game_dir_notes();
        ImGui::TextWrapped("%s", menu_text(UI_LAUNCH_GAME_DIR_HELP));
    }
    step_end();
    ImGui::EndDisabled();
}

// Set up: what is installed, as a card (the game version, the port version that set it up, how the RomFS is kept, the
// copy it came from and the game folder), with the folder's buttons.
static void files_summary(const SetupState &st, float act_h) {
    char port[64];
    snprintf(port, sizeof port, "MKW Recomp %s", L.setup_port);
    const struct {
        UiTextId label;
        const char *value;
    } rows[] = {
        {UI_LAUNCH_INFO_VERSION, L.game_version},
        {UI_LAUNCH_INFO_PORT, L.setup_port[0] ? port : ""},
        {UI_LAUNCH_INFO_ROMFS, !L.setup_mode[0]                  ? ""
                               : !strcmp(L.setup_mode, "link") ? menu_text(UI_LAUNCH_ROMFS_LINKED)
                                                                 : menu_text(UI_LAUNCH_ROMFS_COPIED)},
        {UI_LAUNCH_SOURCE, L.setup_source},
        {UI_LAUNCH_FILES_FOLDER, cfg_game_dir()},
    };
    float lw = 0;
    for (const auto &r : rows) lw = fmaxf(lw, ImGui::CalcTextSize(menu_text(r.label)).x);
    card_begin(fminf(ImGui::GetContentRegionAvail().x, 1100 * L.scale));
    float x = lw + 24 * L.scale;   // SameLine measures from the group's left edge
    for (const auto &r : rows) {
        if (!r.value[0]) continue;   // a record from an older setup lacks some
        ImGui::TextColored(col4(LOOK_DIM), "%s", menu_text(r.label));
        ImGui::SameLine(x);
        ImGui::TextUnformatted(r.value);
    }
    ImGui::Dummy(ImVec2(0, 4 * L.scale));
    if (ImGui::Button(menu_text(UI_LAUNCH_OPEN_FOLDER), ImVec2(0, act_h))) plat_open_folder(cfg_game_dir());
    ImGui::SameLine();
    game_dir_button(st, act_h);
    game_dir_notes();
    card_end(LOOK_LINE, LOOK_ROW);
}

// A failed setup, in red where the eye lands first: why, Try again (prepare.py keeps the steps already done) and the
// log folder (logs/setup.log has the tools' whole messages).
static void setup_failed_card(const SetupState &st) {
    char msg[700];
    setup_error_text(st, msg, sizeof msg);
    card_begin(fminf(ImGui::GetContentRegionAvail().x, 1100 * L.scale));
    ImGui::TextColored(col4(LOOK_BAD), "%s", msg);
    if (accent_button(menu_text(UI_LAUNCH_RETRY)))
        setup_start(L.exe_dir, cfg_game_dir(), L.source[0] ? L.source : nullptr, L.link_romfs);
    ImGui::SameLine();
    if (ImGui::Button(menu_text(UI_MENU_OPEN_LOG))) {
        char logs[PLAT_PATH_MAX + 16];
        snprintf(logs, sizeof logs, "%s/logs", cfg_game_dir());
        plat_open_folder(logs);
    }
    card_end(LOOK_BAD, with_alpha(LOOK_BAD, 22));
}

// The game files page. Not set up: what the setup needs and its three steps, which is the start screen then. Set up:
// what is installed, and the steps folded under Set up again. A setup under way or finished shows its steps' progress
// first, a failed one its reason with Try again above them.
static void page_files(float w, float h) {
    const float act_h = item_height();   // the page's buttons: a target easy to hit, as the main page's
    float bottom = panel_begin("##files", UI_LAUNCH_FILES, w, h);
    ImGui::BeginChild("##files_body", ImVec2(0, -bottom), ImGuiChildFlags_NavFlattened);
    bool ready = rt_game_present();
    SetupState st = setup_state();
    ImGui::BeginDisabled(L.picking);
    char state[160];
    files_state_text(ready, state, sizeof state);
    state_line(ready && !files_gone(), state);
    if (const char *gone = ready ? files_gone() : nullptr) {   // where they were, and what brings them back
        char help[PLAT_PATH_MAX * 2 + 400];
        snprintf(help, sizeof help, menu_text(UI_LAUNCH_FILES_GONE_HELP), gone);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + fminf(ImGui::GetContentRegionAvail().x, 620 * L.scale));
        ImGui::TextWrapped("%s", help);
        ImGui::PopTextWrapPos();
    }
    if (gpu_bad()) gpu_card(fminf(ImGui::GetContentRegionAvail().x, 760 * L.scale));
    update_card(fminf(ImGui::GetContentRegionAvail().x, 620 * L.scale));   // a folder not set up starts on this page
    if (L.updated[0]) {
        char note[300];
        snprintf(note, sizeof note, menu_text(UI_LAUNCH_UPDATED), L.updated);
        ImGui::TextColored(col4(LOOK_OK), "%s", note);
    }
    if (!cfg_last_save_ok()) ImGui::TextColored(col4(LOOK_BAD), "%s", menu_text(UI_MENU_SAVE_FAILED));
    ImGui::Dummy(ImVec2(0, 4 * L.scale));
    if (st.finished < 0) setup_failed_card(st);
    if (st.finished > 0) ImGui::TextColored(col4(LOOK_OK), "%s", menu_text(UI_LAUNCH_SETUP_DONE));
    if (st.running || st.finished) {
        setup_progress(st);
        if (st.running && ImGui::Button(menu_text(UI_LAUNCH_CANCEL), ImVec2(0, act_h))) setup_cancel();
        ImGui::Dummy(ImVec2(0, 8 * L.scale));
    }
    if (ready) files_summary(st, act_h);
    if (!L.tools) {
        ImGui::TextWrapped("%s", menu_text(UI_LAUNCH_NO_TOOLS));
    } else if (ready) {
        ImGui::Dummy(ImVec2(0, 8 * L.scale));
        static bool open_test = getenv("MKW_LAUNCHER_SETUP_OPEN") != nullptr;   // (test) unfolded, for a shot
        if (st.running || st.finished || open_test) ImGui::SetNextItemOpen(true, ImGuiCond_Always);   // its steps stay in view
        if (ImGui::CollapsingHeader(menu_text(UI_LAUNCH_SETUP_AGAIN))) {
            ImGui::TextColored(col4(LOOK_HEADING), "%s", menu_text(UI_LAUNCH_SETUP_AGAIN_HINT));
            ImGui::Dummy(ImVec2(0, 6 * L.scale));
            setup_steps(st, ready, act_h);
        }
    } else {
        // a line of some 75 letters at most: wider, the eye loses its place on the way back
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + fminf(ImGui::GetContentRegionAvail().x, 940 * L.scale));
        ImGui::TextUnformatted(menu_text(UI_LAUNCH_FILES_HELP));
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, 8 * L.scale));
        setup_steps(st, ready, act_h);
    }
    ImGui::EndDisabled();
    ImGui::EndChild();
    // Not set up: this page is the start screen (decided 06/11/2026), its bottom button quits; set up, it goes back.
    // While the setup runs the button takes no input: one click would throw away minutes of work. Cancel, under the
    // steps, is the way to stop it on purpose.
    ImGui::BeginDisabled(st.running || L.picking);
    bool leave = panel_back_row(ready ? UI_LAUNCH_BACK : UI_LAUNCH_QUIT);
    ImGui::EndDisabled();
    leave = leave || (!st.running && !L.picking && back_pressed());
    ImGui::End();
    if (!leave) return;
    if (st.running) setup_cancel();
    if (ready) go_main();
    else L.result = LAUNCHER_QUIT;
}

// ---- the Mods page: the player's mods, imported from a zip or a folder, enabled for the next start
static void SDLCALL pick_zip(void *, const char *const *files, int) {
    if (files && files[0]) mods_pick(files[0], 1);
}

static void SDLCALL pick_folder(void *, const char *const *files, int) {
    if (files && files[0]) mods_pick(files[0], 0);
}

// import.py's "error <code> <detail>" in the menu language; an unknown code shows the detail alone.
static void mods_error_text(const ModsState &st, char *out, size_t cap) {
    static const struct {
        const char *code;
        UiTextId text;
    } kWhy[] = {{"layout", UI_MOD_ERR_LAYOUT}, {"name", UI_MOD_ERR_NAME}, {"io", UI_MOD_ERR_IO},
                {"notported", UI_MOD_ERR_NOPORT}, {"nogame", UI_MOD_ERR_NOGAME},
                {"no_setup", UI_SETUP_ERR_NO_SETUP}, {"big", UI_MOD_ERR_BIG}, {"unpack", UI_MOD_ERR_UNPACK}};
    char why[600];
    snprintf(why, sizeof why, "%s", st.error);
    for (const auto &w : kWhy)
        if (!strcmp(st.code, w.code)) {
            const char *text = menu_text(w.text);
            // a text with %s takes the detail as its argument (never printed raw); otherwise the detail
            // is appended after a colon, so an untranslated format can never reach the player verbatim
            if (strstr(text, "%s"))
                snprintf(why, sizeof why, text, st.error);
            else if (st.error[0])
                snprintf(why, sizeof why, "%s: %s", text, st.error);
            else
                snprintf(why, sizeof why, "%s", text);
        }
    snprintf(out, cap, "%s", why);
}

// One modal OK/Cancel confirmation shared by the page's destructive buttons (delete for now):
// the caller fills `subject`, opens it with ImGui::OpenPopup(id), and this runs the dialog.
// Returns true once, on OK. The subject is left for the caller to read (it names what to act on)
// and cleared here on Cancel; the caller clears it after acting.
static bool confirm_modal(const char *id, char *subject, size_t cap, UiTextId ask, UiTextId ok) {
    (void)cap;
    bool done = false;
    if (ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        char text[MOD_NAME_MAX + 32];
        snprintf(text, sizeof text, menu_text(ask), subject);
        ImGui::TextWrapped("%s", text);
        if (ImGui::Button(menu_text(ok))) {
            done = true;   // the subject still names the item: the caller needs it
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(menu_text(UI_LAUNCH_BACK))) {
            subject[0] = 0;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    } else if (subject[0] && !ImGui::IsPopupOpen(id)) {
        subject[0] = 0;   // the popup never opened (modal already open): drop a stale subject
    }
    return done;
}

// A mod's options page (mod_page.cpp), in the Mods panel: the mod's title at the right of the heading, what its
// plugin draws, Back to the list. MKW_LAUNCHER_MOD_PAGE=<mod> opens it once (tests, shots).
static bool page_mod_options(float w, float h) {
    static int test_opened = 0;
    if (!test_opened) {
        test_opened = 1;
        if (const char *t = getenv("MKW_LAUNCHER_MOD_PAGE")) {
            mods_refresh(L.exe_dir);
            const char *title = t;
            for (int i = 0; i < mods_count(); i++)
                if (!strcmp(mods_at(i)->name, t)) title = mods_at(i)->title;
            mod_page_open(t, title);
        }
    }
    if (!mod_page_is_open()) return false;
    float bottom = panel_begin("##mods", UI_LAUNCH_MODS, w, h, mod_page_title());
    ImGui::BeginChild("##mod_page_body", ImVec2(0, -bottom), ImGuiChildFlags_NavFlattened);
    mod_page_draw(L.scale);
    ImGui::EndChild();
    bool leave = panel_back_row() || (back_pressed() && !ImGui::IsAnyItemActive());
    ImGui::End();
    if (leave) mod_page_close();
    return true;
}

static void page_mods(float w, float h) {
    if (page_mod_options(w, h)) return;
    float bottom = panel_begin("##mods", UI_LAUNCH_MODS, w, h);
    ImGui::BeginChild("##mods_body", ImVec2(0, -bottom), ImGuiChildFlags_NavFlattened);
    bool ready = rt_game_present();
    static char shown[PLAT_PATH_MAX] = "";
    if (strcmp(shown, L.exe_dir)) {   // entered (buttons and the shot hook both land here)
        mods_refresh(L.exe_dir);
        snprintf(shown, sizeof shown, "%s", L.exe_dir);
        if (!getenv("MKW_LAUNCHER_NO_MERGE")) mods_merge_start(L.exe_dir);   // (test) skip: pre-merged shots
    }
    // two tabs: the player's mods, and GameBanana's (MKW_LAUNCHER_MODS_FIND=1 opens the second, for shots)
    static int find_tab = getenv("MKW_LAUNCHER_MODS_FIND") ? 1 : 0;
    if (L.gb_open) find_tab = 1, L.gb_open = 0;
    if (ImGui::RadioButton(menu_text(UI_MOD_TAB_INSTALLED), find_tab == 0)) find_tab = 0;
    ImGui::SameLine();
    if (ImGui::RadioButton(menu_text(UI_MOD_TAB_FIND), find_tab == 1)) find_tab = 1;
    ImGui::Separator();
    if (find_tab == 1) {
        mods_poll();
        if (!ready) ImGui::TextWrapped("%s", menu_text(UI_LAUNCH_FILES_MISSING));
        else if (mods_browse_draw(L.exe_dir, L.ren)) find_tab = 0;   // an install went to the import: show it
        ImGui::EndChild();
        bool leave = panel_back_row() || back_pressed();
        ImGui::End();
        if (!leave) return;
        if (mods_merge_running()) mods_merge_cancel();
        go_main();
        return;
    }
    if (const char *t = getenv("MKW_LAUNCHER_MODS_PICK")) {   // (test) the picked zip or folder, as its dialog does
        if (!mods_has_pick()) {
            size_t n = strlen(t);
            mods_pick(t, n > 4 && t[n - 4] == '.' && (t[n - 3] == 'z' || t[n - 3] == 'Z'));
        }
    }
    static int toggled = 0;   // (test) toggle one mod once, as its checkbox does
    if (!toggled) {
        if (const char *t = getenv("MKW_LAUNCHER_MODS_TOGGLE")) {
            toggled = 1;
            mods_toggle(t, !mods_is_enabled(t));
        }
    }
    static int picked_winner = 0;   // (test) cycle the first conflict of an archive once, as its button does
    if (!picked_winner) {
        if (const char *t = getenv("MKW_LAUNCHER_MODS_PICKWINNER")) {
            for (int i = 0; i < mods_conflict_count(); i++) {
                const ModConflict *c = mods_conflict_at(i);
            if (c->resolved) continue;   // picked: it leaves the list until the mods around it change
                if (!strcmp(c->archive, t)) {
                    picked_winner = 1;
                    mods_conflict_pick(c->archive, c->inner);
                    break;
                }
            }
        }
    }
    static int imported_one = 0;   // (test) run the import once, as its Import button does
    if (!imported_one && getenv("MKW_LAUNCHER_MODS_IMPORT") && mods_has_pick()) {
        imported_one = 1;
        mods_import_start(L.exe_dir);
    }
    static int deleted_one = 0;   // (test) delete one mod once, as its confirmed modal does
    if (!deleted_one) {
        if (const char *t = getenv("MKW_LAUNCHER_MODS_DELETE")) {
            deleted_one = 1;
            mods_delete(t);
        }
    }
    static char confirm_delete[MOD_NAME_MAX] = "";
    static int delete_cooldown = 0;
    if (delete_cooldown > 0) delete_cooldown--;   // no re-arm on the same double-click: rows shift up under a fast cursor
    mods_poll();
    if (mods_count() == 0) {
        ImGui::TextDisabled("%s", menu_text(UI_LAUNCH_MODS_EMPTY));
    }
    for (int i = 0; i < mods_count(); i++) {
        const Mod *m = mods_at(i);
        ImGui::PushID(i);   // same labels every row (checkbox title can repeat too): unique IDs per row
        bool on = m->enabled;
        if (ImGui::Checkbox(m->title, &on)) mods_toggle(m->name, on);
        // Options (a plugin's page: mod_page.cpp) and Delete at the right
        const ImGuiStyle &rs = ImGui::GetStyle();
        float del_w = ImGui::CalcTextSize(menu_text(UI_MOD_DELETE)).x + rs.FramePadding.x * 2;
        float opt_w = m->has_plugin ? ImGui::CalcTextSize(menu_text(UI_MOD_OPTIONS)).x + rs.FramePadding.x * 2 +
                                          rs.ItemSpacing.x
                                    : 0;
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - del_w - opt_w);
        if (m->has_plugin) {
            if (ImGui::Button(menu_text(UI_MOD_OPTIONS))) mod_page_open(m->name, m->title);
            ImGui::SameLine();
        }
        bool can_delete = !delete_cooldown;
        if (!can_delete) ImGui::BeginDisabled();
        if (ImGui::Button(menu_text(UI_MOD_DELETE)))
            snprintf(confirm_delete, sizeof confirm_delete, "%s", m->name);
        if (!can_delete) ImGui::EndDisabled();
        // what the mod ships: romfs files, exefs code sets, or both; a line only for what it has
        char detail[192] = "";
        size_t n = 0;
        if (m->has_romfs && m->romfs_files == 1)
            n += snprintf(detail + n, sizeof detail - n, "%s", menu_text(UI_MOD_ROMFS_1));
        else if (m->has_romfs)
            n += snprintf(detail + n, sizeof detail - n, menu_text(UI_MOD_ROMFS), m->romfs_files);
        if (m->has_exefs) {
            if (n) n += snprintf(detail + n, sizeof detail - n, "  -  ");
            char codebuf[96];
            const char *code;
            if (m->nswitches == 1) code = menu_text(UI_MOD_CODE_SWITCH_1);
            else if (m->nswitches > 1) {
                snprintf(codebuf, sizeof codebuf, menu_text(UI_MOD_CODE_SWITCHES), m->nswitches);
                code = codebuf;
            } else if (m->code) code = menu_text(UI_MOD_CODE_REBUILD);
            else if (m->exefs_match) code = menu_text(UI_MOD_CODE_PORT);
            else {
                snprintf(codebuf, sizeof codebuf, menu_text(UI_MOD_CODE_BUILD), m->exefs_build);
                code = codebuf;
            }
            snprintf(detail + n, sizeof detail - n, "%s", code);
        }
        if (n) ImGui::TextDisabled("%s", detail);
        // files the port treats its own way (import.py): the mod is still installed, the player told what to expect
        ImGui::PushTextWrapPos(0);
        if (m->warn_graphics) ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.35f, 1), "%s", menu_text(UI_MOD_WARN_GRAPHICS));
        if (m->warn_shaders) ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.35f, 1), "%s", menu_text(UI_MOD_WARN_SHADERS));
        if (m->has_plugin) ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.35f, 1), "%s", menu_text(UI_MOD_PLUGIN));
        ImGui::PopTextWrapPos();
        ImGui::Separator();
        ImGui::PopID();
    }
    // opened outside any PushID row, like the modal itself: opening it inside the row's PushID(i)
    // registers a different popup id than BeginPopupModal looks up, so the dialog never appears
    if (confirm_delete[0]) ImGui::OpenPopup("##delete_mod");
    if (confirm_modal("##delete_mod", confirm_delete, sizeof confirm_delete, UI_MOD_CONFIRM, UI_MOD_DELETE)) {
        mods_delete(confirm_delete);
        confirm_delete[0] = 0;
        delete_cooldown = 45;
    }
    ImGui::Spacing();
    ModsState ms = mods_state();
    if (!L.tools || !ready) {
        ImGui::TextWrapped("%s", menu_text(!ready ? UI_LAUNCH_FILES_MISSING : UI_LAUNCH_NO_TOOLS));
    } else if (ms.running) {
        ImGui::ProgressBar(-1.0f * (float)ImGui::GetTime(), ImVec2(-1, 0), "");
        ImGui::SameLine();
        if (ImGui::Button(menu_text(UI_LAUNCH_CANCEL))) mods_cancel();
    } else if (!mods_has_pick() && !ms.finished) {
        static const SDL_DialogFileFilter zip_filter = {"Mod (.zip, .rar, .7z)", "zip;rar;7z"};
        if (ImGui::Button(menu_text(UI_MOD_IMPORT_ZIP)))
            SDL_ShowOpenFileDialog(pick_zip, nullptr, L.win, &zip_filter, 1, nullptr, false);
        ImGui::SameLine();
        if (ImGui::Button(menu_text(UI_MOD_IMPORT_FOLDER)))
            SDL_ShowOpenFolderDialog(pick_folder, nullptr, L.win, nullptr, false);
    } else if (ms.finished) {
        if (ms.finished > 0) {
            char done[128];
            snprintf(done, sizeof done, menu_text(UI_MOD_IMPORTED), ms.done);
            ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.6f, 1), "%s", done);
        } else if (!strcmp(ms.code, "variants")) {
            // one button per mod the archive holds (import.py lists their folders, tab-separated)
            ImGui::TextWrapped("%s", menu_text(UI_MOD_VARIANTS));
            char list[sizeof ms.error];
            snprintf(list, sizeof list, "%s", ms.error);
            int k = 0;
            for (char *v = strtok(list, "\t"); v; v = strtok(NULL, "\t"), k++) {
                ImGui::PushID(k);
                if (ImGui::Button(v)) mods_import_variant(L.exe_dir, v);
                ImGui::PopID();
            }
        } else {
            char msg[700];
            mods_error_text(ms, msg, sizeof msg);
            ImGui::PushTextWrapPos(0);
            ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.35f, 1), "%s", msg);
            ImGui::PopTextWrapPos();
        }
        if (ImGui::Button(menu_text(UI_LAUNCH_BACK))) mods_clear_pick();
    } else {
        ImGui::TextUnformatted(mods_pick_path());
        ImGui::TextUnformatted(menu_text(UI_MOD_NAME));
        ImGui::SameLine();
        ImGui::InputText("##modname", mods_name_buf(), MOD_NAME_MAX);
        int may = mods_risk_draw();
        ImGui::BeginDisabled(!may);
        if (ImGui::Button(menu_text(UI_MOD_IMPORT))) mods_import_start(L.exe_dir);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button(menu_text(UI_LAUNCH_BACK))) mods_clear_pick();
    }
    // The picker lives in a modal (its own button, so it is not a permanent block on the page). One
    // heading per archive, then its inner files: the archive is the file the mods ship, the inner file
    // is what actually clashes.
    static int ok_forced = 0;   // (test) press the modal's OK once conflicts exist
    if (!ok_forced && getenv("MKW_LAUNCHER_MODS_CONFLICT_OK") && mods_conflict_unresolved() > 0) {
        ok_forced = 1;
        mods_conflict_commit();
    }
    static int modal_forced = 0;   // (test) open the picker once conflicts exist, as its button does
    if (!modal_forced && getenv("MKW_LAUNCHER_MODS_MODAL") && mods_conflict_unresolved() > 0) {
        modal_forced = 1;
        ImGui::OpenPopup("##conflicts");
    }
    if (mods_conflict_unresolved() > 0) {
        ImGui::Separator();
        char label[128];
        snprintf(label, sizeof label, "%s (%d)", menu_text(UI_MOD_RESOLVE), mods_conflict_unresolved());
        if (ImGui::Button(label)) ImGui::OpenPopup("##conflicts");
        ImGui::Separator();
    }
    // centred like the page's panels, not wherever the last item left the cursor
    ImVec2 vp = ImGui::GetMainViewport()->Pos, vs = ImGui::GetMainViewport()->Size;
    ImGui::SetNextWindowPos(ImVec2(vp.x + vs.x * 0.5f, vp.y + vs.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("##conflicts", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextDisabled("%s", menu_text(UI_MOD_CONFLICTS));
        ImGui::BeginChild("##conflict_list", ImVec2(760 * L.scale, 380 * L.scale), ImGuiChildFlags_Borders);
        const char *shown_archive = nullptr;
        for (int i = 0; i < mods_conflict_count(); i++) {
            const ModConflict *c = mods_conflict_at(i);
            if (!shown_archive || strcmp(shown_archive, c->archive)) {
                if (shown_archive) ImGui::Spacing();
                ImGui::TextColored(col4(LOOK_HEADING), "%s", c->archive);
                shown_archive = c->archive;
            }
            ImGui::PushID(1000 + i);   // winners repeat across conflicts: unique IDs per row
            ImGui::Indent(16.0f);
            // the path is long: wrapped on its own line, or the radios are pushed out of the panel
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(c->inner);
            ImGui::PopTextWrapPos();
            ImGui::Indent(16.0f);
            for (int k = 0; k < c->nmods + c->has_original; k++) {
                const char *name = k < c->nmods ? c->mods[k] : "vanilla";
                if (k) ImGui::SameLine();
                bool sel = !strcmp(c->winner, name);
                if (ImGui::RadioButton(k < c->nmods ? name : menu_text(UI_MOD_VANILLA), sel)) {
                    mods_conflict_set(c->archive, c->inner, name);
                    ImGui::CloseCurrentPopup();   // the pick is made: back to the page, not left hanging
                    break;
                }
            }
            ImGui::Unindent(32.0f);
            ImGui::Spacing();
            ImGui::PopID();
        }
        ImGui::EndChild();
        if (ImGui::Button(menu_text(UI_MOD_OK))) {   // keeps what the rows show, defaults included
            mods_conflict_commit();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(menu_text(UI_LAUNCH_BACK))) ImGui::CloseCurrentPopup();   // cancel: nothing written
        ImGui::EndPopup();
    }
    // last, so its appearing never pushes the list, the picker button or the import buttons around
    if (mods_merge_running()) {
        ImGui::Spacing();
        ImGui::ProgressBar(-1.0f * (float)ImGui::GetTime(), ImVec2(-1, 0), "");
        ImGui::SameLine();
        ImGui::TextDisabled("%s", menu_text(UI_MOD_MERGING));
    }
    ImGui::EndChild();
    bool leave = panel_back_row() || back_pressed();
    ImGui::End();
    if (!leave) return;
    if (mods_merge_running()) mods_merge_cancel();
    go_main();
}

// ---- the Tools page: the player's saves, the shortcuts, the updates (with Undo) and reporting a problem

// A heading over a group of the Tools page's rows, in a row of its own.
static void tools_group(UiTextId title) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::Dummy(ImVec2(0, 10 * L.scale));
    ImGui::TextColored(col4(LOOK_HEADING), "%s", menu_text(title));
    ImGui::TableNextColumn();
}

// A row: its name and, under it in the dim colour (the warning colour for a failure), what it does or what it did; the
// caller's buttons follow in the right column.
static void tools_row(const char *label, const char *note, bool bad = false) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(label);
    if (note && *note) ImGui::TextColored(col4(bad ? LOOK_WARN : LOOK_DIM), "%s", note);
    ImGui::TableNextColumn();
}

// The program the shortcuts start: the AppImage on Linux (its AppRun's copy of mkw needs the AppImage's libraries).
static bool shortcut_target(char *exe, size_t cap) {
    const char *appimage = getenv("APPIMAGE");
    if (appimage && *appimage && strlen(appimage) < cap) return snprintf(exe, cap, "%s", appimage), true;
    return plat_exe_path(exe, cap);
}

static void page_tools(float w, float h) {
    float s = L.scale, act_h = item_height();
    float bottom = panel_begin("##tools", UI_LAUNCH_TOOLS, w, h);
    ImGui::BeginChild("##tools_body", ImVec2(0, -bottom), ImGuiChildFlags_NavFlattened);
    ToolState job = tool_state();
    bool busy = job.phase == TOOL_RUNNING;
    char note[800], text[300], player[PLAT_PATH_MAX * 2], backups[PLAT_PATH_MAX + 16];
    launcher_player_dir(player, sizeof player);
    snprintf(backups, sizeof backups, "%s/backups", cfg_game_dir());
    static char report_note[200], undo_tag[96], undo_why[400];
    static bool undo_test = getenv("MKW_LAUNCHER_UNDO") != nullptr;   // (test) once, as the button does
    ImGui::PushTextWrapPos(0);
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(8 * s, 6 * s));   // rows apart, as the settings rows are
    if (ImGui::BeginTable("##tools", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX)) {
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);

        tools_group(UI_TOOLS_SAVES);
        bool done = job.job == JOB_BACKUP && job.phase == TOOL_DONE, failed = job.job == JOB_BACKUP && job.phase == TOOL_FAILED;
        if (done) {   // the zip's name: Open backups shows where
            const char *z = job.line + strlen(job.line);
            while (z > job.line && z[-1] != '/' && z[-1] != '\\') z--;
            snprintf(note, sizeof note, menu_text(UI_TOOLS_BACKUP_DONE), z);
        }
        else if (failed) snprintf(note, sizeof note, menu_text(UI_TOOLS_FAILED), job.line);
        else snprintf(note, sizeof note, menu_text(UI_TOOLS_BACKUP_NOTE), launcher_player_name());
        tools_row(menu_text(UI_TOOLS_BACKUP), note, failed);
        ImGui::BeginDisabled(busy || !player[0]);
        static bool backup_test = getenv("MKW_LAUNCHER_BACKUP") != nullptr;   // (test) once, as the button does
        if (ImGui::Button(menu_text(UI_TOOLS_BACKUP_BUTTON), ImVec2(0, act_h)) || (backup_test && player[0])) {
            backup_test = false;
            const char *args[] = {"backup", "--profile", player, "--dest", backups, "--name", launcher_player_name(), nullptr};
            tool_start(L.exe_dir, JOB_BACKUP, "prepare/saves.py", args);
        }
        ImGui::EndDisabled();
        tools_row(menu_text(UI_TOOLS_SAVES_FOLDERS), nullptr);
        ImGui::BeginDisabled(!player[0]);
        if (ImGui::Button(menu_text(UI_TOOLS_OPEN_SAVES), ImVec2(0, act_h))) plat_open_folder(player);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button(menu_text(UI_TOOLS_OPEN_BACKUPS), ImVec2(0, act_h))) {
            SDL_CreateDirectory(backups);
            plat_open_folder(backups);
        }

        tools_group(UI_TOOLS_SHORTCUTS);
        tools_row(menu_text(UI_LAUNCH_SHORTCUT), L.shortcut_note);
        if (ImGui::Button(menu_text(UI_LAUNCH_SHORTCUT_MAKE), ImVec2(0, act_h))) make_shortcut();
        steam_note(job, note, sizeof note, &failed);
        if (!note[0]) snprintf(note, sizeof note, "%s", menu_text(UI_TOOLS_STEAM_NOTE));
        tools_row(menu_text(UI_TOOLS_STEAM), note, failed);
        ImGui::BeginDisabled(busy);
        if (ImGui::Button(menu_text(UI_TOOLS_ADD), ImVec2(0, act_h))) steam_add();
        ImGui::EndDisabled();

        tools_group(UI_TOOLS_UPDATES);
        UpdateState u = update_state();
        snprintf(text, sizeof text, menu_text(UI_TOOLS_VERSION), rt_port_version);
        if (u.phase == UPDATE_FAILED) snprintf(note, sizeof note, menu_text(UI_TOOLS_FAILED), u.error);
        else if (u.phase == UPDATE_CHECKING) snprintf(note, sizeof note, "%s", menu_text(UI_TOOLS_CHECKING));
        else if (u.phase == UPDATE_CURRENT) snprintf(note, sizeof note, "%s", menu_text(UI_TOOLS_UPDATE_CURRENT));
        else if (u.tag[0]) snprintf(note, sizeof note, menu_text(UI_LAUNCH_UPDATE_TITLE), u.tag);
        else note[0] = 0;
        tools_row(text, note, u.phase == UPDATE_FAILED);
        if (ImGui::Button(menu_text(UI_LAUNCH_WHATS_NEW), ImVec2(0, act_h))) {
            snprintf(note, sizeof note, "https://github.com/%s/releases", repo());
            SDL_OpenURL(note);
        }
        ImGui::SameLine();
        bool busy = u.phase == UPDATE_FETCHING || u.phase == UPDATE_READY || u.phase == UPDATE_PREPARING;
        ImGui::BeginDisabled(u.phase == UPDATE_CHECKING || u.phase == UPDATE_PREPARED || busy);
        if (ImGui::Button(menu_text(UI_TOOLS_CHECK), ImVec2(0, act_h))) update_check_start(L.exe_dir, true);
        ImGui::EndDisabled();
        char kept[96];
        if (update_previous(L.exe_dir, kept, sizeof kept)) {
            snprintf(text, sizeof text, menu_text(UI_TOOLS_UNDO_ROW), kept);
            if (undo_why[0]) snprintf(note, sizeof note, menu_text(UI_TOOLS_FAILED), undo_why);
            else snprintf(note, sizeof note, "%s", menu_text(UI_TOOLS_UNDO_NOTE));
            tools_row(text, note, undo_why[0] != 0);
            ImGui::BeginDisabled(busy || setup_state().running);
            if (ImGui::Button(menu_text(UI_TOOLS_UNDO), ImVec2(0, act_h)) || undo_test)
                snprintf(undo_tag, sizeof undo_tag, "%s", kept);   // the dialog opens after the table (below)
            ImGui::EndDisabled();
        }

        tools_group(UI_TOOLS_PROBLEMS);
        tools_row(menu_text(UI_TOOLS_REPORT), report_note[0] ? report_note : menu_text(UI_TOOLS_REPORT_NOTE));
        if (ImGui::Button(menu_text(UI_TOOLS_REPORT_BUTTON), ImVec2(0, act_h))) {
            // what a bug report needs, on the clipboard to paste into the form the button opens
            static char info[16384];
            bool copied = sysinfo_text(info, sizeof info) > 0 && plat_clipboard_set(info);
            snprintf(report_note, sizeof report_note, "%s",
                     menu_text(copied ? UI_TOOLS_REPORT_COPIED : UI_MENU_ACTION_FAILED));
            snprintf(note, sizeof note, "https://github.com/%s/issues/new?template=bug_report.yml", repo());
            SDL_OpenURL(note);
        }
        tools_row(menu_text(UI_TOOLS_LOGS), nullptr);
        if (ImGui::Button(menu_text(UI_MENU_OPEN_LOG), ImVec2(0, act_h))) {
            char dir[PLAT_PATH_MAX];
            if (sysinfo_log_folder(dir, sizeof dir)) plat_open_folder(dir);
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleVar();
    ImGui::PopTextWrapPos();
    ImVec2 vp = ImGui::GetMainViewport()->Pos, vs = ImGui::GetMainViewport()->Size;
    ImGui::SetNextWindowPos(ImVec2(vp.x + vs.x * 0.5f, vp.y + vs.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    // opened here, in the window's ID scope where confirm_modal looks for it: inside the table it named another popup
    if (undo_tag[0] && !ImGui::IsPopupOpen("##undo")) ImGui::OpenPopup("##undo");
    bool undo_confirmed = undo_test && undo_tag[0];   // (test) MKW_LAUNCHER_UNDO=1 confirms too; =ask leaves it open
    if (undo_confirmed && !strcmp(getenv("MKW_LAUNCHER_UNDO"), "ask")) undo_confirmed = false;
    undo_test = false;
    if (confirm_modal("##undo", undo_tag, sizeof undo_tag, UI_TOOLS_UNDO_ASK, UI_TOOLS_UNDO) || undo_confirmed) {
        if (update_revert(L.exe_dir, undo_why, sizeof undo_why)) L.relaunch = 1, L.result = LAUNCHER_QUIT;
        undo_tag[0] = 0;
    }
    ImGui::EndChild();
    bool leave = panel_back_row();
    if (!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) && back_pressed()) leave = true;
    ImGui::End();
    if (leave) go_main();
}

// ---- the window
static bool open_window(void) {
    // MKW_BACKGROUND_WINDOW=1 (agents' shots, never the player's default): the runs' app id, which the KWin rule opens
    // minimized and without focus (tools/run/kwin-rule.sh), as gfx_window_sdl.c does for the game's window.
    const char *bg = getenv("MKW_BACKGROUND_WINDOW");
    bool background = bg && *bg && strcmp(bg, "0");
    SDL_SetHint(SDL_HINT_APP_ID, background ? "mkw-recomp-run" : "mkw-recomp");
    if (background) SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
    const char *drv = cfg_steam_video_driver();   // as the game's window (cfg.h)
    if (drv) SDL_SetHint(SDL_HINT_VIDEO_DRIVER, drv);
    input_wheel_hint();
    bool video = SDL_Init(SDL_INIT_VIDEO);
    if (!video && drv) {   // the driver asked for Steam's sake: without it
        rt_log("launcher: SDL's %s driver does not start (%s): SDL's own choice", drv, SDL_GetError());
        SDL_ResetHint(SDL_HINT_VIDEO_DRIVER);
        video = SDL_Init(SDL_INIT_VIDEO);
    }
    if (!video) {
        cfg_video_failed(SDL_GetError(), L.no_window, sizeof L.no_window);
        rt_log("launcher: SDL video init failed: %s", SDL_GetError());
        return false;
    }
    // Apart from the video: without controllers the start screen still opens, for the mouse and the keyboard
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) rt_log("launcher: SDL gamepad init failed: %s", SDL_GetError());
    float ds = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    if (!(ds > 0)) ds = 1;
    // A window: the game's MKW_FULLSCREEN is the game's; the start screen goes fullscreen by its own F11, and from
    // Steam's Big Picture or Gaming Mode (it opened there: a setup to run, a notice), as Big Picture is.
    // Hidden until its first frame is drawn (launcher_run): shown at once, it is black while the first frame is made.
    SDL_WindowFlags f = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN |
                        (background ? SDL_WINDOW_NOT_FOCUSABLE : 0) | (cfg_steam_console() && !L.shot_at ? SDL_WINDOW_FULLSCREEN : 0);
    // 1280 x 720 at the display's scale, but never past its usable area (#633: a 4K TV at Windows' 300 % made it
    // 3840 x 2160 plus the title bar, cut at the screen's edges): at most 90 % of it, still 16:9.
    int w = (int)(1280 * ds), h = (int)(720 * ds);
    SDL_Rect usable;
    if (SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &usable) && usable.w > 0 && usable.h > 0) {
        float fit = SDL_min(0.9f * usable.w / w, 0.9f * usable.h / h);
        if (fit < 1) w = (int)(w * fit), h = (int)(h * fit);
    }
    L.win = SDL_CreateWindow("Mario Kart World", w, h, f);
    if (L.win) app_icon_set(L.win);
    L.ren = L.win ? SDL_CreateRenderer(L.win, nullptr) : nullptr;
    if (!L.ren) {
        snprintf(L.no_window, sizeof L.no_window, "The start screen's window cannot open (%.200s), and the game files are "
                 "not set up or not where the setup left them", SDL_GetError());   // else the game starts (no_window)
        rt_log("launcher: no window: %s", SDL_GetError());
        return false;
    }
    SDL_SetRenderVSync(L.ren, 1);
    // under 960 x 540 the column and the pages' two panes no longer fit side by side
    // (never above the window it opened with: a minimum past the screen would push it out again)
    SDL_SetWindowMinimumSize(L.win, SDL_min((int)(960 * ds), w), SDL_min((int)(540 * ds), h));
    return true;
}

static void save_shot(const char *suffix) {
    SDL_Surface *s = SDL_RenderReadPixels(L.ren, nullptr);
    const char *base = getenv("MKW_LAUNCHER_SHOT_OUT");
    char out[1100];
    snprintf(out, sizeof out, "%s%s", base && *base ? base : "launcher.png", suffix);
    bool ok = s && SDL_SavePNG(s, out);
    rt_log("launcher: shot %s %s", out, ok ? "saved" : SDL_GetError());
    SDL_DestroySurface(s);
}

extern "C" void launcher_set_romfs(const char *romfs) {
    snprintf(g_romfs, sizeof g_romfs, "%s", romfs);
    mods_set_romfs(romfs);
}
extern "C" void launcher_set_exefs(const char *exefs) {
    snprintf(g_exefs, sizeof g_exefs, "%s", exefs);
    mods_set_exefs(exefs);
}
extern "C" void launcher_set_profile(const char *profile) { launcher_players_set_base(profile); }

static void setup_paths_finished(const SetupState *state) {
    static unsigned seen;
    if (!state->finished || seen == state->generation) return;
    seen = state->generation;
    // prepare.py wrote the linked package/folder (or "romfs" for a copy) into mkw.ini in another process. Play
    // reexecs and sees it, but without this refresh the Mods page kept startup's missing or previous RomFS.
    if (state->finished > 0 && cfg_reload_key("MKW_ROMFS")) launcher_refresh_romfs();
}

extern "C" void launcher_gb_install(const char *url) {
    const char *p = !strncmp(url, "mkw-recomp:", 11) ? url + 11 : url;
    int file = 0, mod = 0;
    char tail = 0;
    if (sscanf(p, "https://gamebanana.com/mmdl/%d,Mod,%d%c", &file, &mod, &tail) == 2 && file > 0 && mod > 0) {
        L.gb_open = 1;
        mods_browse_open(mod, file);
        rt_log("launcher: GameBanana 1-Click: mod %d, file %d", mod, file);
    } else {
        rt_log("launcher: not a GameBanana 1-Click link, ignored: %.200s", url);
    }
}

// Started by Steam's Big Picture or Gaming Mode (on a Steam Deck, Bazzite): Steam marks the games it starts there with
// SteamGamepadUI=1 (SteamTenfoot=1 before 2023), and Gaming Mode runs them under gamescope.
static int steam_console(void) { return cfg_steam_console(); }

// The last game run ended in a fatal error (logs/mkw.log, the previous run's until the next Play moves it): the start
// screen shows its notice (launcher_lastrun.h) instead of starting a game that may stop the same way.
static int last_run_fatal(void) {
    char p[PLAT_PATH_MAX * 2];
    snprintf(p, sizeof p, "%s/logs/mkw.log", cfg_game_dir());
    PlatFile *f = plat_file_open(p);
    if (!f) return 0;
    static char tail[65536];
    PlatFileId id;
    int64_t got = 0;
    if (plat_file_id(f, &id)) {
        uint64_t from = id.size > sizeof tail - 1 ? id.size - (sizeof tail - 1) : 0;
        got = plat_file_pread(f, tail, sizeof tail - 1, from);
    }
    plat_file_close(f);
    if (got <= 0) return 0;
    tail[got] = 0;
    return strstr(tail, "FATAL") != nullptr || strstr(tail, "fatal signal") != nullptr;
}

// View / Select (Minus) held on any gamepad in the first 400 ms: the player asks for the start screen.
static int start_screen_asked(void) {
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) return 0;
    int asked = 0;
    for (int t = 0; t < 8 && !asked; t++) {
        SDL_UpdateGamepads();
        int n = 0;
        SDL_JoystickID *ids = SDL_GetGamepads(&n);
        for (int i = 0; ids && i < n && !asked; i++) {
            SDL_Gamepad *g = SDL_OpenGamepad(ids[i]);
            if (g) SDL_UpdateGamepads(), asked = SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_BACK), SDL_CloseGamepad(g);
        }
        SDL_free(ids);
        if (!asked) SDL_Delay(50);
    }
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    return asked;
}

// MKW_START_SCREEN (General tab): auto skips the start screen when Steam's Big Picture or Gaming Mode started the game,
// as a console starts a game from its own menu (a controller-only player on Bazzite or a Steam Deck), skip always,
// always never. It opens anyway when it has something to do or say: the game files not set up or gone from where the
// setup left them (issue #486: a Steam Deck in Gaming Mode whose .nup was not there any more), a setup to
// run again after an update, a code mod to build, the last run's fatal error, or View / Select held at start. Skipped,
// it does what Play does (the mods merged, the player's profile) and the game starts.
static int console_start(int updated, int other) {
    if (L.shot_at || L.gb_open) return 0;
    const char *mode = cfg_value("MKW_START_SCREEN");
    int skip = !strcmp(mode, "skip") || (!strcmp(mode, "auto") && steam_console());
    if (!skip) return 0;
    char state[PLAT_PATH_MAX * 3];
    snprintf(state, sizeof state, "%s/setup-state.json", cfg_game_dir());
    const char *why = !rt_game_present()                                   ? "the game files are not set up"
                    : files_gone()                                         ? "the game files are not where the setup left them"
                    : updated || other                                     ? "the game files are to be set up again"
                    : mods_code_changed() && plat_file_exists(state)       ? "a code mod is to be built"
                    : last_run_fatal()                                     ? "the last run ended in an error"
                    : start_screen_asked()                                 ? "View / Select held"
                                                                           : nullptr;
    if (why) {
        rt_log("launcher: start screen shown (MKW_START_SCREEN=%s): %s", mode, why);
        return 0;
    }
    mods_merge_sync(L.exe_dir);   // as Play does
    char player[PLAT_PATH_MAX * 2];
    launcher_player_dir(player, sizeof player);
    if (player[0]) mods_before_play(player);
    rt_log("launcher: start screen skipped (MKW_START_SCREEN=%s%s): the game starts", mode,
           steam_console() ? ", started by Steam's Big Picture or Gaming Mode" : "");
    return 1;
}

// No start screen. The game may still open its own window (it draws with Vulkan, not with SDL's renderer) when SDL's
// video started and the game files are there; else the player is told here: the game would close with an error about
// its files, or about the graphics card ("no usable Vulkan device"), neither the reason.
static int no_window(void) {
    if (SDL_WasInit(SDL_INIT_VIDEO) && rt_game_present() && !files_gone()) {
        rt_log("launcher: no start screen: the game starts");
        return LAUNCHER_PLAY;
    }
    fprintf(stderr, "mkw-recomp: %s\n", L.no_window);
    SDL_Quit();   // its SIGTERM handler too: the box blocks, and a process stopped from outside must end
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "mkw-recomp", L.no_window, nullptr);
    return LAUNCHER_FAILED;
}

extern "C" int launcher_run(void) {
    L.result = -1;
    L.focus_main = 1;
    exe_dir(L.exe_dir, sizeof L.exe_dir);
    parse_test();
    int updated = update_just_installed(L.exe_dir, L.updated, sizeof L.updated);
    int other = !updated && !L.shot_at && setup_from_other_version();
    if (console_start(updated, other)) return LAUNCHER_PLAY;
    if (!open_window()) return no_window();
    L.picked_event = SDL_RegisterEvents(1);
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    launcher_look_style(L.base_style);
    ui_id_check_init();
    opt_look(&launcher_look_rows);   // the settings rows in the start screen's palette, not the in-game menu's
    ImGui_ImplSDL3_InitForSDLRenderer(L.win, L.ren);
    ImGui_ImplSDLRenderer3_Init(L.ren);
    list_gpus();
    backdrop_load(L.ren, L.exe_dir);
    if (L.shot_at) {
        L.page = L.shot_page;
        if (L.page == PAGE_SETTINGS) {
            settings_select(L.shot_tab);
            settings_opened();
        }
    }
    L.tools = setup_available(L.exe_dir);
    if ((!rt_game_present() || files_gone()) && !L.shot_at) L.page = PAGE_FILES;
    pads_refresh();
    char state[PLAT_PATH_MAX * 3];
    snprintf(state, sizeof state, "%s/setup-state.json", cfg_game_dir());
    if (L.gb_open && rt_game_present()) L.page = PAGE_MODS;
    if (!L.shot_at && plat_file_exists(state)) {   // a set-up folder: GameBanana's 1-Click opens this program
        char exe[PLAT_PATH_MAX * 2];
        const char *appimage = getenv("APPIMAGE");   // Linux: the AppImage, not the mounted copy inside it
        if (appimage && *appimage) snprintf(exe, sizeof exe, "%s", appimage);
        else if (!plat_exe_path(exe, sizeof exe)) exe[0] = 0;
        if (exe[0] && !plat_url_scheme("mkw-recomp", "MKW Recomp mods", exe, "--gb-install"))
            rt_log("launcher: the mkw-recomp: links (GameBanana 1-Click) could not be registered");
    }
    if ((updated || other) && L.tools && plat_file_exists(state)) {
        if (other) rt_log("launcher: the game files were set up by another version: setting up again what changed");
        L.page = PAGE_FILES;   // set up again what the new version changed, from the last setup's copy
        // no source and no mode: prepare.py takes both from the record, so the player's RomFS choice survives an
        // update (asking for "copy" here would copy the whole RomFS over a linked one)
        setup_start(L.exe_dir, cfg_game_dir(), nullptr, -1);
    } else if (!L.shot_at || getenv("MKW_UPDATE_AS")) {
        update_check_start(L.exe_dir, false);
    }
    if (!L.shot_at || getenv("MKW_LAUNCHER_LASTRUN")) lastrun_start(L.exe_dir, cfg_game_dir());
    // MKW_LAUNCHER_FIND=1 (test): the search in a shot too, as at a plain start
    if (L.tools && (!L.shot_at || getenv("MKW_LAUNCHER_FIND"))) finddumps_start(L.exe_dir, cfg_game_dir());
    if (getenv("MKW_LAUNCHER_SHORTCUT")) make_shortcut();   // (test) as the button does
    if (const char *t = getenv("MKW_LAUNCHER_SETUP")) {   // (test) the setup started from that folder, as Set up does
        snprintf(L.source, sizeof L.source, "%s", t);
        char *link = strstr(L.source, ":link");
        if (link) *link = 0, L.link_romfs = 1;
        L.page = PAGE_FILES;
        setup_start(L.exe_dir, cfg_game_dir(), L.source, L.link_romfs);
    }
    rt_log("launcher: start screen, game files %s, setup tools %s", rt_game_present() ? "ready" : "not set up",
           L.tools ? "present" : "missing");
    for (long frame = 1; L.result < 0; frame++) {
        SetupState setup = setup_state();
        setup_paths_finished(&setup);   // before the code-mod merge below, or a new import from this frame's input
        if (L.play_after_setup && setup.finished) {   // the same snapshot: a new completion must refresh paths first
            L.play_after_setup = 0;
            if (setup.finished > 0) {
                mods_code_built();
                mods_merge_sync(L.exe_dir);
                char player[PLAT_PATH_MAX * 2];
                launcher_player_dir(player, sizeof player);
                if (player[0]) mods_before_play(player);
                L.result = LAUNCHER_PLAY;
                break;
            }
        }
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == L.picked_event) {
                L.picking = 0;
                const char *path = (const char *)e.user.data1;
                if (path && e.user.code) {
                    if (cfg_select_game_dir(path)) {
                        L.directory_restart = 1;
                        L.result = LAUNCHER_QUIT;
                    }
                } else if (path) snprintf(L.source, sizeof L.source, "%s", path);
                SDL_free(e.user.data1);
                continue;
            }
            if (e.type == SDL_EVENT_QUIT) L.result = LAUNCHER_QUIT;
            if (e.type == SDL_EVENT_GAMEPAD_ADDED || e.type == SDL_EVENT_GAMEPAD_REMOVED) {
                pads_refresh();   // the main page's own list
                input_hotplug();   // the settings' list (input.c) follows the same event, when it is running
            }
            if (e.type == SDL_EVENT_JOYSTICK_ADDED && !SDL_IsGamepad(e.jdevice.which)) input_wheel_map();
            if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_F11 && !e.key.repeat)
                SDL_SetWindowFullscreen(L.win, !(SDL_GetWindowFlags(L.win) & SDL_WINDOW_FULLSCREEN));
            if (L.shot_at) continue;
            // The mouse comes in window units, the interface is laid out in pixels (DisplaySize below): on a scaled
            // display (Wayland 1.85x) the pointer would land at 1/1.85 of its place.
            SDL_ConvertEventToRenderCoordinates(L.ren, &e);
            ImGui_ImplSDL3_ProcessEvent(&e);
        }
        int pw, ph;
        SDL_GetCurrentRenderOutputSize(L.ren, &pw, &ph);
        restyle(menu_look_scale((uint32_t)ph));
        if (L.steam_art || (frame == 2 && getenv("MKW_LAUNCHER_STEAM_ART"))) steam_art_draw();   // (test) as Add to Steam
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        io.DisplaySize = ImVec2((float)pw, (float)ph);   // pixels: the style and fonts follow the height
        io.DisplayFramebufferScale = ImVec2(1, 1);
        static const char *const kPage[] = {"main", "settings", "files", "mods", "players", "tools"};
        ui_id_check(kPage[L.page]);
        ImGui::NewFrame();
        // the intro's clock starts with the first frame of the main page, before its background: started in
        // page_main, frame 1 (the first one shown) would have the flag at full strength, gone the frame after
        if (L.page == PAGE_MAIN && L.intro_t0 <= 0) L.intro_t0 = ImGui::GetTime();
        ImDrawList *bgl = ImGui::GetBackgroundDrawList();
        backdrop_draw(bgl, (float)pw, (float)ph, L.page == PAGE_MAIN);
        if (L.page == PAGE_MAIN && L.intro_t0 > 0 && !L.shot_at)   // the flag: in over the intro's first 0.8 s
            launcher_fade(bgl, (float)pw, (float)ph, LOOK_BG, L.intro_t0, 0.8f);
        if (L.page != L.shown || frame == 1) {
            if (frame > 1 && !L.shot_at) L.page_t0 = ImGui::GetTime();
            read_setup_info();   // a setup may have changed the game files meanwhile
            L.shown = L.page;
        }
        switch (L.page) {
        case PAGE_MAIN: page_main((float)pw, (float)ph); break;
        case PAGE_SETTINGS: page_settings((float)pw, (float)ph); break;
        case PAGE_FILES: page_files((float)pw, (float)ph); break;
        case PAGE_MODS: page_mods((float)pw, (float)ph); break;
        case PAGE_PLAYERS: page_players((float)pw, (float)ph); break;
        case PAGE_TOOLS: page_tools((float)pw, (float)ph); break;
        }
        // the page just switched to comes in from its own colour in 0.15 s, over everything drawn this frame
        if (L.page_t0 > 0 && !launcher_fade(ImGui::GetForegroundDrawList(), (float)pw, (float)ph,
                                            L.page == PAGE_MAIN ? LOOK_BG : LOOK_PANEL, L.page_t0, 0.15f))
            L.page_t0 = 0;
        ImGui::Render();
        L.save_pending = opt_save_tick(0);
        SDL_SetRenderDrawColor(L.ren, 0, 0, 0, 255);
        SDL_RenderClear(L.ren);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), L.ren);
        bool setup_test = getenv("MKW_LAUNCHER_EXIT_AFTER_SETUP") && L.source[0];
        if (setup_test && setup_state().finished) {   // the page as the setup left it, then out
            save_shot("-end.png");
            L.result = LAUNCHER_QUIT;
        }
        if (L.shot_at && frame == L.shot_at) {   // during a setup test the shot is taken and the setup goes on
            save_shot("");
            if (!setup_test) L.result = LAUNCHER_QUIT;
        }
        // shown once its first frame is drawn, right before that frame is presented: shown at creation it is black
        // for as long as the start takes, and presenting to a window still hidden can wait on Wayland for a frame
        // callback that an unmapped surface never gets
        if (frame == 1) SDL_ShowWindow(L.win);
        SDL_RenderPresent(L.ren);
    }
    if (L.save_pending) opt_save_tick(1);
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    launcher_brand_free();
    backdrop_free();
    for (SDL_Gamepad *&g : L.pad_open)
        if (g) SDL_CloseGamepad(g), g = nullptr;
    SDL_DestroyRenderer(L.ren);
    SDL_DestroyWindow(L.win);
    SDL_Quit();
    if (L.directory_restart) plat_reexec();
    rt_log("launcher: %s", L.result == LAUNCHER_PLAY ? "play" : L.relaunch ? "the update starts" : "quit");
    if (L.relaunch) update_relaunch(L.exe_dir);   // Linux: does not come back
    return L.result;
}
