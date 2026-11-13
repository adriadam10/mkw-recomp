// The start screen, shown before the game: Play, Settings, Game files and Quit, the game files' state at the lower left
//. The player's build (build.py --dist) always starts here; Play starts the process again
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
// launcher.png) and quits; page main, settings (tab 0..5), files, mods or players. The user's input is ignored while it
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
// at once, as its button does.
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_sdlrenderer3.h"
#include "imgui.h"
#include "ui/launcher/launcher.h"
#include "ui/launcher/launcher_backdrop.h"
#include "ui/launcher/launcher_logo.h"
#include "ui/launcher/launcher_look.h"
#include "ui/launcher/launcher_players.h"
#include "ui/launcher/launcher_setup.h"
#include "ui/launcher/launcher_update.h"
#include "ui/launcher/mods.h"
#include "ui/launcher/mods_browse.h"
#include "ui/menu/menu_look.h"
#include "ui/menu/option_rows.h"
#include "ui/menu/settings_tabs.h"

extern "C" {
#include "cfg.h"
#include "gamelib.h"
#include "platform/platform.h"
#include "ui_text.h"
void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
extern const char rt_port_version[];
void app_icon_set(SDL_Window *w);   // build/obj/rt/app_icon.c, drawn by tools/build/icon.py
}

enum Page { PAGE_MAIN, PAGE_SETTINGS, PAGE_FILES, PAGE_MODS, PAGE_PLAYERS };

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
    long shot_at;
    Page shot_page;
    int shot_tab;
    char exe_dir[1024];
    Uint32 picked_event;            // dialogs can finish on another thread: consume their result on the UI thread
    int picking;
    int directory_restart;
    char source[1024];              // the player's copy: a folder, or a .nup file, from the dialogs
    int link_romfs;                 // 0 copy it into the game folder, 1 use it where it is
    int tools;                      // the setup tools are there
    char shortcut_note[1200];       // what the shortcut button did
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

// The Vulkan devices for the Screen tab's GPU row: an instance only to list them.
// The last setup was made by another version: a player who updated by hand (a new AppImage, or the zip over the old
// folder) kept the old shaders and game code (#50: 0.1.8 played with 0.1.7's shaders). States from before 0.1.9 keep
// no version, so they set up again once too; prepare.py runs only the steps whose key changed.
static int setup_from_other_version(const char *state) {
    FILE *f = fopen(state, "rb");
    if (!f) return 0;
    static char buf[16384];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    char want[96];
    snprintf(want, sizeof want, "\"port\": \"%s\"", rt_port_version);
    return !strstr(buf, want);
}

static void list_gpus(void) {
    VkApplicationInfo ai = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    ai.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ci = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &ai;
    VkInstance inst;
    if (vkCreateInstance(&ci, nullptr, &inst) != VK_SUCCESS) return;
    settings_list_gpus(inst);
    vkDestroyInstance(inst, nullptr);
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

// The main page's one blue button: a play triangle and the label on the left, in the heading's size.
static bool play_button(const char *label, ImVec2 size, bool enabled) {
    float s = L.scale;
    ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + size.x, a.y + size.y);
    ImGui::BeginDisabled(!enabled);
    bool hit = ImGui::InvisibleButton("##play", size);
    if (enabled && ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);   // it can be clicked
    bool held = ImGui::IsItemActive(), cursor = ImGui::IsItemFocused() && ImGui::GetIO().NavVisible;
    float hot = eased(ImGui::GetItemID(), enabled && (ImGui::IsItemHovered() || cursor));
    ImGui::EndDisabled();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImU32 fill = !enabled ? LOOK_RAISED : held ? LOOK_ACCENT_DOWN : mix(LOOK_ACCENT, LOOK_ACCENT_HOT, hot);
    ImU32 text = enabled ? IM_COL32(255, 255, 255, 255) : LOOK_OFF;
    dl->AddRectFilled(a, b, fill, 4 * s);
    if (cursor) dl->AddRect(ImVec2(a.x - 4 * s, a.y - 4 * s), ImVec2(b.x + 4 * s, b.y + 4 * s), LOOK_FOCUS, 6 * s, 0, 2 * s);
    float fs = L.head->FontSize, k = fs * 0.32f, gap = 18 * s;
    float x = a.x + 28 * s, cy = (a.y + b.y) * 0.5f;   // on the left, in line with the menu's labels
    dl->AddTriangleFilled(ImVec2(x, cy - k), ImVec2(x + k * 1.6f, cy), ImVec2(x, cy + k), text);
    dl->AddText(L.head, fs, ImVec2(x + k * 1.6f + gap, cy - fs * 0.5f), text, label);
    return hit;
}

// The height of a row of the main page's menu and of a page's buttons: 60 at 1080 lines, never under 44 pixels (the
// smallest target a finger or a hurried mouse hits; at 720 lines 60 would give 40).
static float item_height(void) { return fmaxf(44.0f, 60 * L.scale); }

// A row of the main page's menu: the label alone at rest; under the mouse or the controller's cursor a band of the
// accent that fades out to the right comes in under it (as Dusklight's start screen does), with a bar at its left edge.
static bool menu_item(const char *label, float width, bool enabled) {
    float s = L.scale, hgt = item_height();
    ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + width, a.y + hgt);
    ImGui::BeginDisabled(!enabled);
    ImGui::PushID(label);
    bool hit = ImGui::InvisibleButton("##item", ImVec2(width, hgt));
    if (enabled && ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    bool held = ImGui::IsItemActive(), cursor = ImGui::IsItemFocused() && ImGui::GetIO().NavVisible;
    float hot = eased(ImGui::GetItemID(), enabled && (ImGui::IsItemHovered() || cursor));
    ImGui::PopID();
    ImGui::EndDisabled();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    if (hot > 0.01f) {
        ImVec4 c = col4(held ? LOOK_ACCENT_DOWN : LOOK_ACCENT);
        ImU32 from = ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, 0.85f * hot)),
              to = ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, 0));
        dl->AddRectFilledMultiColor(a, b, from, to, to, from);
        dl->AddRectFilled(a, ImVec2(a.x + 3 * s, b.y), ImGui::ColorConvertFloat4ToU32(ImVec4(1, 1, 1, hot)));
    }
    float fs = L.item->FontSize;
    dl->AddText(L.item, fs, ImVec2(a.x + 24 * s + 4 * s * hot, (a.y + b.y - fs) * 0.5f),
                enabled ? mix(LOOK_HEADING, LOOK_TEXT, hot) : LOOK_OFF, label);
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

// The update offered (launcher_update.h): its button, then the download's progress; once downloaded, installed and
// the program started again. A failed check shows nothing, a failed download says why.
static void update_row(float bw) {
    UpdateState u = update_state();
    char text[700];
    if (u.phase == UPDATE_OFFER) {
        snprintf(text, sizeof text, menu_text(UI_LAUNCH_UPDATE), u.tag, (int)((u.bytes + (1 << 19)) >> 20));
        if (ImGui::Button(text, ImVec2(bw, 0)) || getenv("MKW_LAUNCHER_UPDATE")) update_fetch_start(L.exe_dir);
    } else if (u.phase == UPDATE_FETCHING) {
        snprintf(text, sizeof text, menu_text(UI_LAUNCH_UPDATE_FETCHING), u.tag);
        ImGui::ProgressBar(u.total > 0 ? (float)u.done / (float)u.total : 0, ImVec2(bw, 0), text);
    } else if (u.phase == UPDATE_READY) {
        if (update_apply(L.exe_dir)) L.relaunch = 1, L.result = LAUNCHER_QUIT;
    } else if (u.phase == UPDATE_FAILED) {
        snprintf(text, sizeof text, menu_text(UI_LAUNCH_UPDATE_FAILED), u.error);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + bw);
        ImGui::TextColored(col4(LOOK_BAD), "%s", text);
        ImGui::PopTextWrapPos();
    } else {
        return;
    }
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

// The main page, in the original's structure: a column on the left with the title and the menu (Play, the profile that
// plays, then Settings, Game files and the desktop shortcut, Quit last and apart), the game files' state at the lower
// left and the version at
// the lower right. The picture is the background's (launcher_backdrop.cpp): the player's splash.png, or the logo's flag.
static void page_main(float w, float h) {
    float s = L.scale, left = 96 * s, cw = 440 * s, lh = ImGui::GetTextLineHeight();
    bool ready = rt_game_present() && update_state().phase != UPDATE_FETCHING;
    // the column's height, to centre it: logo and title, the gap, Play, the menu, the gap and Quit
    float mark = 84 * s, items = item_height(), gap = 12 * s;
    float play_h = fmaxf(52.0f, 76 * s), col_h = mark + 48 * s + play_h + gap * 2 + items * 5 + 28 * s + items;
    ImGui::SetNextWindowPos(ImVec2(left - 8 * s, fmaxf(32 * s, (h - col_h) * 0.45f) - 8 * s));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8 * s, 8 * s));   // room for the cursor's ring
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, gap));
    ImGui::Begin("##main", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground |
                                        ImGuiWindowFlags_AlwaysAutoResize);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    int v0 = dl->VtxBuffer.Size;
    launcher_logo(dl, p, mark);
    float ts = L.big->FontSize, tx = p.x + mark + 24 * s, by = p.y + (mark - ts - 4 * s - lh) * 0.5f;
    dl->AddText(L.big, ts, ImVec2(tx, by - ts * 0.06f), LOOK_TEXT, "Mario Kart World");
    dl->AddText(ImVec2(tx, by + ts + 4 * s), LOOK_HEADING, menu_text(UI_LAUNCH_SUBTITLE));
    intro_apply(dl, v0, intro(0));
    float title_w = mark + 24 * s + L.big->CalcTextSizeA(ts, FLT_MAX, 0, "Mario Kart World").x;
    ImGui::Dummy(ImVec2(fmaxf(cw, title_w), mark + 48 * s - gap));   // the window as wide as the title: it clips
    update_row(cw);
    if (L.focus_main) {
        ImGui::SetKeyboardFocusHere();
        L.focus_main = 0;
    }
    v0 = dl->VtxBuffer.Size;
    static bool play_test = getenv("MKW_LAUNCHER_PLAY") != nullptr;   // (test) once, as the button does
    bool play = play_button(menu_text(UI_LAUNCH_PLAY), ImVec2(cw, play_h), ready);
    if (play || (play_test && ready)) {
        play_test = false;
        char state[PLAT_PATH_MAX + 32];
        snprintf(state, sizeof state, "%s/setup-state.json", cfg_game_dir());
        if (mods_code_changed() && L.tools && plat_file_exists(state)) {   // a set-up folder (not build/mkw)
            // a code mod changed: the game code is built again first (generate and compile), on the files page
            L.play_after_setup = 1;
            L.page = PAGE_FILES;
            setup_start(L.exe_dir, cfg_game_dir(), nullptr, 0);
        } else {
            mods_merge_sync(L.exe_dir);   // a stale .merged must not boot: usually a no-op fresh-check
            char player[PLAT_PATH_MAX * 2];
            launcher_player_dir(player, sizeof player);
            if (player[0]) mods_before_play(player);
            L.result = LAUNCHER_PLAY;
        }
    }
    intro_apply(dl, v0, intro(1));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    ImGui::Dummy(ImVec2(0, gap));
    v0 = dl->VtxBuffer.Size;
    char profile[160];
    snprintf(profile, sizeof profile, menu_text(UI_LAUNCH_PLAYER), launcher_player_name());
    if (menu_item(profile, cw, true)) L.page = PAGE_PLAYERS;
    intro_apply(dl, v0, intro(2));
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
    if (menu_item(menu_text(UI_LAUNCH_SHORTCUT), cw, true)) make_shortcut();
    intro_apply(dl, v0, intro(6));
    ImGui::Dummy(ImVec2(0, 28 * s));
    v0 = dl->VtxBuffer.Size;
    if (menu_item(menu_text(UI_LAUNCH_QUIT), cw, true)) L.result = LAUNCHER_QUIT;
    intro_apply(dl, v0, intro(7));
    ImGui::PopStyleVar();
    if (L.shortcut_note[0]) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + cw);
        ImGui::TextColored(col4(LOOK_DIM), "%s", L.shortcut_note);
        ImGui::PopTextWrapPos();
    }
    ImGui::End();
    ImGui::PopStyleVar(2);

    // the game files at the lower left; at the lower right that the port ships no game data, over the version
    ImDrawList *fg = ImGui::GetForegroundDrawList();
    int f0 = fg->VtxBuffer.Size;
    float y = h - 48 * s - lh, r = lh * 0.24f;
    const char *state = menu_text(ready ? UI_LAUNCH_FILES_READY : UI_LAUNCH_FILES_MISSING);
    fg->AddCircleFilled(ImVec2(left + r, y + lh * 0.5f), r, ready ? LOOK_OK : LOOK_WARN, 16);
    fg->AddText(ImVec2(left + r * 2 + 10 * s, y), ready ? LOOK_HEADING : LOOK_WARN, state);
    char ver[96];
    snprintf(ver, sizeof ver, "MKW Recomp %s", rt_port_version);
    const char *note = menu_text(UI_LAUNCH_NO_GAME_DATA);
    float right = w - 56 * s;
    fg->AddText(ImVec2(right - ImGui::CalcTextSize(ver).x, y), LOOK_DIM, ver);
    fg->AddText(ImVec2(right - ImGui::CalcTextSize(note).x, y - lh - 6 * s), LOOK_DIM, note);
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
// menu's (wider, a row's value would be too far from its name); returns the space its bottom row needs.
static float panel_begin(const char *id, UiTextId title, float w, float h) {
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
    float bottom = panel_begin("##settings", UI_LAUNCH_SETTINGS, w, h);
    settings_tabs(L.scale, bottom);
    bool back = panel_back_row();
    if (!settings_modal() && back_pressed()) back = true;
    settings_tabs_done();
    ImGui::End();
    if (back) go_main();
}

// ---- the game files page: the player's copy, how the RomFS is kept, the setup and its progress
static const UiTextId kStepText[SETUP_STEPS] = {UI_LAUNCH_STEP_CHECK, UI_LAUNCH_STEP_EXEFS, UI_LAUNCH_STEP_ROMFS,
                                                UI_LAUNCH_STEP_GENERATE, UI_LAUNCH_STEP_COMPILE, UI_LAUNCH_STEP_SHADERS,
                                                UI_LAUNCH_STEP_WARM, UI_LAUNCH_STEP_CLEANUP};

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
                {"moved", UI_SETUP_ERR_MOVED}};
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

static void page_files(float w, float h) {
    const float act_h = item_height();   // the page's buttons: a target easy to hit, as the main page's
    float bottom = panel_begin("##files", UI_LAUNCH_FILES, w, h);
    ImGui::BeginChild("##files_body", ImVec2(0, -bottom), ImGuiChildFlags_NavFlattened);
    bool ready = rt_game_present();
    SetupState st = setup_state();
    ImGui::BeginDisabled(L.picking);
    state_line(ready, menu_text(ready ? UI_LAUNCH_FILES_READY : UI_LAUNCH_FILES_MISSING));
    update_row(ImGui::GetContentRegionAvail().x * 0.5f);   // a folder not set up starts on this page
    if (L.updated[0]) {
        char note[300];
        snprintf(note, sizeof note, menu_text(UI_LAUNCH_UPDATED), L.updated);
        ImGui::TextColored(col4(LOOK_OK), "%s", note);
    }
    // a line of some 75 letters at most: wider, the eye loses its place on the way back
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + fminf(ImGui::GetContentRegionAvail().x, 940 * L.scale));
    ImGui::TextUnformatted(menu_text(UI_LAUNCH_FILES_HELP));
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    ImGui::TextDisabled("%s", menu_text(UI_LAUNCH_FILES_FOLDER));
    ImGui::SameLine();
    ImGui::TextWrapped("%s", cfg_game_dir());
    ImGui::BeginDisabled(st.running || update_state().phase == UPDATE_FETCHING || cfg_from_env("MKW_GAME_DIR"));
    if (ImGui::Button(menu_text(UI_LAUNCH_PICK_GAME_DIR), ImVec2(0, act_h))) {
        L.picking = 1;
        SDL_ShowOpenFolderDialog(picked, &L.directory_restart, L.win, cfg_game_dir(), false);
    }
    ImGui::EndDisabled();
    ImGui::TextWrapped("%s", menu_text(UI_LAUNCH_GAME_DIR_HELP));
    if (cfg_from_env("MKW_GAME_DIR"))
        ImGui::TextDisabled("%s MKW_GAME_DIR", menu_text(UI_MENU_SET_BY_ENV));
    if (cfg_game_dir_invalid()) {
        ImGui::PushTextWrapPos(0);
#ifdef _WIN32
        ImGui::TextColored(col4(LOOK_BAD), menu_text(UI_LAUNCH_GAME_DIR_INVALID_WIN32), CFG_GAME_DIR_WIN32_MAX);
#else
        ImGui::TextColored(col4(LOOK_BAD), menu_text(UI_LAUNCH_GAME_DIR_INVALID), CFG_GAME_DIR_MAX);
#endif
        ImGui::PopTextWrapPos();
    }
    if (!cfg_last_save_ok()) ImGui::TextColored(col4(LOOK_BAD), "%s", menu_text(UI_MENU_SAVE_FAILED));
    // the groups apart: more space between them than inside one
    ImGui::Dummy(ImVec2(0, 8 * L.scale));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, 8 * L.scale));
    if (!L.tools) {
        ImGui::TextWrapped("%s", menu_text(UI_LAUNCH_NO_TOOLS));
    } else {
        ImGui::BeginDisabled(st.running);
        ImGui::TextDisabled("%s", menu_text(UI_LAUNCH_SOURCE));
        ImGui::SameLine();
        ImGui::TextUnformatted(L.source[0] ? L.source : "-");
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
        ImGui::Dummy(ImVec2(0, 8 * L.scale));
        ImGui::TextDisabled("%s", menu_text(UI_LAUNCH_ROMFS_MODE));
        // a ring with a red dot in the chosen one: ImGui's own is a filled disc, which reads as a blank button
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2 * L.scale);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, ImVec2(10 * L.scale, 0));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Border, col4(LOOK_DIM));
        ImGui::RadioButton(menu_text(UI_LAUNCH_ROMFS_COPY), &L.link_romfs, 0);
        ImGui::SameLine(0, 28 * L.scale);
        ImGui::RadioButton(menu_text(UI_LAUNCH_ROMFS_LINK), &L.link_romfs, 1);
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(2);
        ImGui::BeginDisabled(!L.source[0]);
        // the page's main action, in the blue: until the game is set up there is nothing else to do here
        ImGui::PushStyleColor(ImGuiCol_Button, col4(LOOK_ACCENT));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, col4(LOOK_ACCENT_HOT));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, col4(LOOK_ACCENT_DOWN));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
        if (ImGui::Button(menu_text(ready ? UI_LAUNCH_SETUP_AGAIN : UI_LAUNCH_SETUP), ImVec2(0, act_h)))
            setup_start(L.exe_dir, cfg_game_dir(), L.source, L.link_romfs);
        ImGui::PopStyleColor(4);
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        if (st.running) {
            ImGui::SameLine();
            if (ImGui::Button(menu_text(UI_LAUNCH_CANCEL), ImVec2(0, act_h))) setup_cancel();
        }
        // the result right under Set up, by the button that started it: after the step list it would be under the
        // fold of a 720-line window, and a failed setup would seem to have done nothing
        if (st.finished > 0) ImGui::TextColored(col4(LOOK_OK), "%s", menu_text(UI_LAUNCH_SETUP_DONE));
        if (st.finished < 0) {
            char msg[700];
            setup_error_text(st, msg, sizeof msg);
            ImGui::PushTextWrapPos(0);
            ImGui::TextColored(col4(LOOK_BAD), "%s", msg);
            ImGui::PopTextWrapPos();
        }
        if (st.running || st.finished) {
            ImGui::Dummy(ImVec2(0, 12 * L.scale));
            setup_progress(st);
        }
    }
    ImGui::EndDisabled();
    ImGui::EndChild();
    // Not set up: this page is the start screen (decided 06/11/2026), its bottom button quits; set up, it goes back.
    // While the setup runs the button takes no input: one click would throw away minutes of work. Cancel, next to Set
    // up, is the way to stop it on purpose.
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

static void page_mods(float w, float h) {
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
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(menu_text(UI_MOD_DELETE)).x -
                        ImGui::GetStyle().FramePadding.x * 2);
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
            for (int k = 0; k <= c->nmods; k++) {
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

// ---- the window
static bool open_window(void) {
    // MKW_BACKGROUND_WINDOW=1 (agents' shots, never the player's default): the runs' app id, which the KWin rule opens
    // minimized and without focus (tools/run/kwin-rule.sh), as gfx_window_sdl.c does for the game's window.
    const char *bg = getenv("MKW_BACKGROUND_WINDOW");
    bool background = bg && *bg && strcmp(bg, "0");
    SDL_SetHint(SDL_HINT_APP_ID, background ? "mkw-recomp-run" : "mkw-recomp");
    if (background) SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        rt_log("launcher: SDL_Init failed: %s", SDL_GetError());
        return false;
    }
    float ds = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    if (!(ds > 0)) ds = 1;
    // Always a window: the game's MKW_FULLSCREEN is the game's; the start screen goes fullscreen only by its own F11.
    // Hidden until its first frame is drawn (launcher_run): shown at once, it is black while the first frame is made.
    SDL_WindowFlags f = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN |
                        (background ? SDL_WINDOW_NOT_FOCUSABLE : 0);
    L.win = SDL_CreateWindow("Mario Kart World", (int)(1280 * ds), (int)(720 * ds), f);
    if (L.win) app_icon_set(L.win);
    L.ren = L.win ? SDL_CreateRenderer(L.win, nullptr) : nullptr;
    if (!L.ren) {
        rt_log("launcher: no window: %s", SDL_GetError());
        return false;
    }
    SDL_SetRenderVSync(L.ren, 1);
    // under 960 x 540 the column and the pages' two panes no longer fit side by side
    SDL_SetWindowMinimumSize(L.win, (int)(960 * ds), (int)(540 * ds));
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

extern "C" void launcher_set_romfs(const char *romfs) { mods_set_romfs(romfs); }
extern "C" void launcher_set_exefs(const char *exefs) { mods_set_exefs(exefs); }
extern "C" void launcher_set_profile(const char *profile) { launcher_players_set_base(profile); }

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

extern "C" int launcher_run(void) {
    L.result = -1;
    L.focus_main = 1;
    exe_dir(L.exe_dir, sizeof L.exe_dir);
    parse_test();
    if (!open_window()) return LAUNCHER_PLAY;   // no start screen: the game itself says what is wrong
    L.picked_event = SDL_RegisterEvents(1);
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    launcher_look_style(L.base_style);
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
    if (!rt_game_present() && !L.shot_at) L.page = PAGE_FILES;
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
    int updated = update_just_installed(L.exe_dir, L.updated, sizeof L.updated);
    int other = !updated && !L.shot_at && setup_from_other_version(state);
    if ((updated || other) && L.tools && plat_file_exists(state)) {
        if (other) rt_log("launcher: the game files were set up by another version: setting up again what changed");
        L.page = PAGE_FILES;   // set up again what the new version changed, from the last setup's copy
        setup_start(L.exe_dir, cfg_game_dir(), nullptr, 0);
    } else if (!L.shot_at || getenv("MKW_UPDATE_AS")) {
        update_check_start(L.exe_dir);
    }
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
        if (L.play_after_setup && setup_state().finished) {   // the code mods' build ended: play if it went well
            L.play_after_setup = 0;
            if (setup_state().finished > 0) {
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
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        io.DisplaySize = ImVec2((float)pw, (float)ph);   // pixels: the style and fonts follow the height
        io.DisplayFramebufferScale = ImVec2(1, 1);
        ImGui::NewFrame();
        // the intro's clock starts with the first frame of the main page, before its background: started in
        // page_main, frame 1 (the first one shown) would have the flag at full strength, gone the frame after
        if (L.page == PAGE_MAIN && L.intro_t0 <= 0) L.intro_t0 = ImGui::GetTime();
        ImDrawList *bgl = ImGui::GetBackgroundDrawList();
        backdrop_draw(bgl, (float)pw, (float)ph, L.page == PAGE_MAIN);
        if (L.page == PAGE_MAIN && L.intro_t0 > 0 && !L.shot_at)   // the flag: in over the intro's first 0.8 s
            launcher_fade(bgl, (float)pw, (float)ph, LOOK_BG, L.intro_t0, 0.8f);
        if (L.page != L.shown) {
            if (frame > 1 && !L.shot_at) L.page_t0 = ImGui::GetTime();
            L.shown = L.page;
        }
        switch (L.page) {
        case PAGE_MAIN: page_main((float)pw, (float)ph); break;
        case PAGE_SETTINGS: page_settings((float)pw, (float)ph); break;
        case PAGE_FILES: page_files((float)pw, (float)ph); break;
        case PAGE_MODS: page_mods((float)pw, (float)ph); break;
        case PAGE_PLAYERS: page_players((float)pw, (float)ph); break;
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
    backdrop_free();
    SDL_DestroyRenderer(L.ren);
    SDL_DestroyWindow(L.win);
    SDL_Quit();
    if (L.directory_restart) plat_reexec();
    rt_log("launcher: %s", L.result == LAUNCHER_PLAY ? "play" : L.relaunch ? "the update starts" : "quit");
    if (L.relaunch) update_relaunch(L.exe_dir);   // Linux: does not come back
    return L.result;
}
