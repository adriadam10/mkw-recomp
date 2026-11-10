// The start screen, shown before the game: Play, Settings, Game files and Quit, the game files' state at the lower left
//. The player's build (build.py --dist) always starts here; Play starts the process again
// with the launcher skipped (main.c), so the game claims its address space in a fresh process. build/mkw shows it only
// with --launcher.
//
// Its own window and SDL_Renderer (Dear ImGui's SDL3 and SDL_Renderer back ends), not the game's Vulkan device: nothing of
// the game runs yet. The pages and the look are the in-game menu's (ui/menu/settings_tabs.cpp, ui/menu/menu_look.cpp). The idea
// and the page set follow SavageWindRecomp's runtime/ui/launcher.mm (https://github.com/Revivedx/SavageWindRecomp @
// 5173d27, MIT, Copyright (c) 2026 Revivedx and the Savage Wind Recomp contributors).
//
// Test hooks: MKW_LAUNCHER_SHOT=<frame>[:<page>[:<tab>[:<scroll 0..1>]]] saves that frame as MKW_LAUNCHER_SHOT_OUT (default
// launcher.png) and quits; page main, settings (tab 0..5) or files. The user's input is ignored while it runs.
// MKW_LAUNCHER_SETUP=<folder>[:link] starts the setup from that folder at once, on the game files page.
// MKW_LAUNCHER_SHORTCUT=1 makes the desktop shortcut at start, as its button does.
// MKW_LAUNCHER_EXIT_AFTER_SETUP=1 quits when that setup ends, after saving the page as <shot out>-end.png (its result
// is in the log); MKW_LAUNCHER_SHOT then takes its shot without quitting.
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_sdlrenderer3.h"
#include "imgui.h"
#include "ui/launcher/launcher.h"
#include "ui/launcher/launcher_setup.h"
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
}

enum Page { PAGE_MAIN, PAGE_SETTINGS, PAGE_FILES };

static struct {
    SDL_Window *win;
    SDL_Renderer *ren;
    SDL_Texture *splash;            // splash.png next to the executable, the player's own picture
    int splash_w, splash_h;
    ImGuiStyle base_style;
    float scale;                    // of the style and fonts in use; 0 none yet
    ImFont *big;
    Page page;
    int focus_main;                 // the main page's first button takes the cursor
    int result;                     // LAUNCHER_PLAY or LAUNCHER_QUIT once chosen; -1 running
    int save_pending;
    // test hook
    long shot_at;
    Page shot_page;
    int shot_tab;
    char exe_dir[1024];
    char source[1024];              // the player's copy, from the folder dialog
    int link_romfs;                 // 0 copy it into the game folder, 1 use it where it is
    int tools;                      // the setup tools are there
    char shortcut_note[1200];       // what the shortcut button did
} L = {};

static void exe_dir(char *out, size_t cap) {
    out[0] = 0;
    if (!plat_exe_path(out, cap)) return;
    char *cut = plat_last_sep(out);
    if (cut) *cut = 0;
}

// The Vulkan devices for the Screen tab's GPU row: an instance only to list them.
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

static void load_splash(void) {
    char path[1100];
    snprintf(path, sizeof path, "%s/splash.png", L.exe_dir);
    if (!plat_file_exists(path)) return;
    SDL_Surface *s = SDL_LoadPNG(path);
    if (!s) return;
    L.splash = SDL_CreateTextureFromSurface(L.ren, s);
    L.splash_w = s->w, L.splash_h = s->h;
    SDL_DestroySurface(s);
}

static void parse_test(void) {
    const char *e = getenv("MKW_LAUNCHER_SHOT");
    if (!e || !*e) return;
    L.shot_at = atol(e);
    const char *p = strchr(e, ':');
    if (!p) return;
    p++;
    L.shot_page = !strncmp(p, "settings", 8) ? PAGE_SETTINGS : !strncmp(p, "files", 5) ? PAGE_FILES : PAGE_MAIN;
    const char *t = strchr(p, ':');
    L.shot_tab = t ? atoi(t + 1) : 0;
    const char *sc = t ? strchr(t + 1, ':') : NULL;   // settings:<tab>:<scroll 0..1>
    if (sc) opt_test_scroll((float)atof(sc + 1));
}

static void restyle(float scale) {
    if (scale == L.scale) return;
    ImGui::GetStyle() = L.base_style;
    ImGui::GetStyle().ScaleAllSizes(scale);
    menu_look_fonts(scale);
    L.big = menu_look_font_big(scale);
    ImGui_ImplSDLRenderer3_DestroyFontsTexture();
    ImGui_ImplSDLRenderer3_CreateFontsTexture();
    L.scale = scale;
}

// ---- the background: the player's splash.png scaled to cover the window, else a road drawn in perspective (no art of
// the game ships with the port)
static void background(ImDrawList *dl, float w, float h) {
    if (L.splash) {
        float k = fmaxf(w / L.splash_w, h / L.splash_h), sw = L.splash_w * k, sh = L.splash_h * k;
        dl->AddImage((ImTextureID)(intptr_t)L.splash, ImVec2((w - sw) / 2, (h - sh) / 2), ImVec2((w + sw) / 2, (h + sh) / 2));
        dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(w * 0.6f, h), IM_COL32(0, 0, 0, 170), IM_COL32(0, 0, 0, 0),
                                    IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 170));
        return;
    }
    float hy = h * 0.42f;   // the horizon
    dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(w, hy), IM_COL32(18, 40, 70, 255), IM_COL32(18, 40, 70, 255),
                                IM_COL32(236, 160, 84, 255), IM_COL32(236, 160, 84, 255));
    dl->AddRectFilledMultiColor(ImVec2(0, hy), ImVec2(w, h), IM_COL32(24, 70, 44, 255), IM_COL32(24, 70, 44, 255),
                                IM_COL32(10, 30, 20, 255), IM_COL32(10, 30, 20, 255));
    float vx = w * 0.62f;   // the vanishing point, right of the buttons
    dl->AddQuadFilled(ImVec2(vx - 4, hy), ImVec2(vx + 4, hy), ImVec2(w * 1.15f, h), ImVec2(w * 0.10f, h),
                      IM_COL32(52, 54, 60, 255));
    float t = (float)fmod(ImGui::GetTime() * 0.6, 1.0);
    for (int i = 0; i < 12; i++) {   // the centre dashes, moving towards the viewer
        float a = (i + t) / 12.0f, b = (i + t + 0.45f) / 12.0f;
        float ya = hy + (h - hy) * a * a, yb = hy + (h - hy) * b * b;
        float xa = vx + (w * 0.625f - vx) * a * a, xb = vx + (w * 0.625f - vx) * b * b;
        float wa = 1 + 10 * a * a, wb = 1 + 10 * b * b;
        dl->AddQuadFilled(ImVec2(xa - wa, ya), ImVec2(xa + wa, ya), ImVec2(xb + wb, yb), ImVec2(xb - wb, yb),
                          IM_COL32(238, 194, 84, 255));
    }
    for (int side = 0; side < 2; side++) {   // the kerbs, red and white
        float ex = side ? w * 1.15f : w * 0.10f;
        for (int i = 0; i < 16; i++) {
            float a = i / 16.0f, b = (i + 1) / 16.0f;
            float ya = hy + (h - hy) * a * a, yb = hy + (h - hy) * b * b;
            float xa = vx + (ex - vx) * a * a, xb = vx + (ex - vx) * b * b, k = 1 + 18 * b * b;
            ImU32 c = i & 1 ? IM_COL32(230, 230, 230, 255) : IM_COL32(200, 40, 40, 255);
            float d = side ? 1 : -1;
            dl->AddQuadFilled(ImVec2(xa, ya), ImVec2(xa + d * k * 0.6f, ya), ImVec2(xb + d * k, yb), ImVec2(xb, yb), c);
        }
    }
    dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(w * 0.55f, h), IM_COL32(0, 0, 0, 150), IM_COL32(0, 0, 0, 0),
                                IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 150));
}

// ---- the pages
static bool big_button(const char *label, float width, bool enabled) {
    ImGui::BeginDisabled(!enabled);
    bool hit = ImGui::Button(label, ImVec2(width, ImGui::GetFrameHeight() * 1.6f));
    ImGui::EndDisabled();
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

static void page_main(float w, float h) {
    float s = L.scale, bw = 420 * s;
    bool ready = rt_game_present();
    ImGui::SetNextWindowPos(ImVec2(90 * s, h * 0.18f));
    ImGui::Begin("##main", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground |
                                        ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::PushFont(L.big);
    ImGui::TextColored(ImVec4(0.93f, 0.76f, 0.33f, 1), "Mario Kart World");
    ImGui::PopFont();
    ImGui::TextDisabled("%s", menu_text(UI_LAUNCH_SUBTITLE));
    ImGui::Dummy(ImVec2(0, 30 * s));
    if (L.focus_main) {
        ImGui::SetKeyboardFocusHere();
        L.focus_main = 0;
    }
    if (big_button(menu_text(UI_LAUNCH_PLAY), bw, ready)) L.result = LAUNCHER_PLAY;
    if (big_button(menu_text(UI_LAUNCH_SETTINGS), bw, true)) {
        L.page = PAGE_SETTINGS;
        settings_opened();
    }
    if (big_button(menu_text(UI_LAUNCH_FILES), bw, true)) L.page = PAGE_FILES;
    if (big_button(menu_text(UI_LAUNCH_QUIT), bw, true)) L.result = LAUNCHER_QUIT;
    ImGui::Dummy(ImVec2(0, 10 * s));
    if (ImGui::Button(menu_text(UI_LAUNCH_SHORTCUT), ImVec2(bw, 0))) make_shortcut();
    if (L.shortcut_note[0]) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + bw);
        ImGui::TextDisabled("%s", L.shortcut_note);
        ImGui::PopTextWrapPos();
    }
    ImGui::End();

    // the game files at the lower left, the version at the lower right
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    const char *st = menu_text(ready ? UI_LAUNCH_FILES_READY : UI_LAUNCH_FILES_MISSING);
    float lh = ImGui::GetTextLineHeight();
    dl->AddText(ImVec2(90 * s, h - 50 * s - lh), ready ? IM_COL32(150, 220, 160, 255) : IM_COL32(240, 170, 90, 255), st);
    char ver[96];
    snprintf(ver, sizeof ver, "MKW Recomp %s", rt_port_version);
    dl->AddText(ImVec2(w - 40 * s - ImGui::CalcTextSize(ver).x, h - 50 * s - lh), IM_COL32(200, 200, 190, 200), ver);
}

static bool back_pressed(void) {
    return ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false);
}

static void go_main(void) {
    L.page = PAGE_MAIN;
    L.focus_main = 1;
}

// A window like the in-game menu's, centred; returns the space its bottom row needs.
static float panel_begin(const char *id, UiTextId title, float w, float h) {
    float s = L.scale, ww = fminf(w * 0.88f, 1400 * s), wh = fminf(h * 0.86f, 820 * s);
    ImGui::SetNextWindowPos(ImVec2(w * 0.5f, h * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(ww, wh), ImGuiCond_Always);
    ImGui::Begin(id, nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextColored(ImVec4(0.93f, 0.76f, 0.33f, 1), "%s", menu_text(title));
    const ImGuiStyle &st = ImGui::GetStyle();
    return ImGui::GetFrameHeight() + st.ItemSpacing.y * 2;
}

// The bottom row: the save note on the left, Back on the right. true: Back.
static bool panel_back_row(UiTextId label = UI_LAUNCH_BACK) {
    const ImGuiStyle &st = ImGui::GetStyle();
    ImGui::Spacing();
    float y = ImGui::GetCursorPosY();
    if (!cfg_last_save_ok()) {
        ImGui::SetCursorPosY(y + st.FramePadding.y);
        ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.35f, 1), "%s", menu_text(UI_MENU_SAVE_FAILED));
    }
    const char *back = menu_text(label);
    float bw = ImGui::CalcTextSize(back).x + st.FramePadding.x * 4;
    ImGui::SetCursorPos(ImVec2(ImGui::GetWindowWidth() - st.WindowPadding.x - bw, y));
    return ImGui::Button(back, ImVec2(bw, 0));
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

static void SDLCALL picked(void *, const char *const *files, int) {
    if (files && files[0]) snprintf(L.source, sizeof L.source, "%s", files[0]);
}

static void setup_progress(const SetupState &st) {
    for (int i = 0; i < SETUP_STEPS; i++) {
        bool now = st.running && st.step == i;
        ImVec4 col = st.done[i] ? ImVec4(0.55f, 0.85f, 0.6f, 1) : now ? ImVec4(0.93f, 0.76f, 0.33f, 1)
                                                                       : ImVec4(0.62f, 0.64f, 0.58f, 1);
        const char *mark = st.done[i] == 2 ? menu_text(UI_LAUNCH_STEP_KEPT) : st.done[i] ? "OK" : now ? ">" : "";
        ImGui::TextColored(col, "%-3s %s", mark, menu_text(kStepText[i]));
        if (now) {
            ImGui::SameLine();
            if (st.total > 0) {
                char over[48];
                snprintf(over, sizeof over, "%ld / %ld", st.progress, st.total);
                ImGui::ProgressBar((float)st.progress / (float)st.total, ImVec2(-1, 0), over);
            } else {
                ImGui::ProgressBar(-1.0f * (float)ImGui::GetTime(), ImVec2(-1, 0), "");
            }
        }
    }
}

static void page_files(float w, float h) {
    float bottom = panel_begin("##files", UI_LAUNCH_FILES, w, h);
    ImGui::BeginChild("##files_body", ImVec2(0, -bottom), ImGuiChildFlags_NavFlattened);
    bool ready = rt_game_present();
    SetupState st = setup_state();
    ImGui::TextColored(ready ? ImVec4(0.55f, 0.85f, 0.6f, 1) : ImVec4(0.94f, 0.67f, 0.35f, 1), "%s",
                       menu_text(ready ? UI_LAUNCH_FILES_READY : UI_LAUNCH_FILES_MISSING));
    ImGui::TextWrapped("%s", menu_text(UI_LAUNCH_FILES_HELP));
    ImGui::Spacing();
    ImGui::TextDisabled("%s", menu_text(UI_LAUNCH_FILES_FOLDER));
    ImGui::SameLine();
    ImGui::TextUnformatted(L.exe_dir);
    ImGui::Separator();
    if (!L.tools) {
        ImGui::TextWrapped("%s", menu_text(UI_LAUNCH_NO_TOOLS));
    } else {
        ImGui::BeginDisabled(st.running);
        ImGui::TextDisabled("%s", menu_text(UI_LAUNCH_SOURCE));
        ImGui::SameLine();
        ImGui::TextUnformatted(L.source[0] ? L.source : "-");
        if (ImGui::Button(menu_text(UI_LAUNCH_PICK))) SDL_ShowOpenFolderDialog(picked, nullptr, L.win, nullptr, false);
        ImGui::TextDisabled("%s", menu_text(UI_LAUNCH_ROMFS_MODE));
        ImGui::RadioButton(menu_text(UI_LAUNCH_ROMFS_COPY), &L.link_romfs, 0);
        ImGui::SameLine();
        ImGui::RadioButton(menu_text(UI_LAUNCH_ROMFS_LINK), &L.link_romfs, 1);
        ImGui::BeginDisabled(!L.source[0]);
        if (ImGui::Button(menu_text(ready ? UI_LAUNCH_SETUP_AGAIN : UI_LAUNCH_SETUP)))
            setup_start(L.exe_dir, L.source, L.link_romfs);
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        if (st.running) {
            ImGui::SameLine();
            if (ImGui::Button(menu_text(UI_LAUNCH_CANCEL))) setup_cancel();
        }
        if (st.running || st.finished) {
            ImGui::Spacing();
            setup_progress(st);
            if (st.finished > 0) ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.6f, 1), "%s", menu_text(UI_LAUNCH_SETUP_DONE));
            if (st.finished < 0) {
                char msg[700];
                snprintf(msg, sizeof msg, menu_text(UI_LAUNCH_SETUP_FAILED), st.error);
                ImGui::PushTextWrapPos(0);
                ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.35f, 1), "%s", msg);
                ImGui::PopTextWrapPos();
            }
        }
    }
    ImGui::EndChild();
    // Not set up: this page is the start screen (decided 06/11/2026), its bottom button quits; set up, it goes back.
    bool leave = panel_back_row(ready ? UI_LAUNCH_BACK : UI_LAUNCH_QUIT) || (!st.running && back_pressed());
    ImGui::End();
    if (!leave) return;
    if (st.running) setup_cancel();
    if (ready) go_main();
    else L.result = LAUNCHER_QUIT;
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
    SDL_WindowFlags f = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | (background ? SDL_WINDOW_NOT_FOCUSABLE : 0);
    L.win = SDL_CreateWindow("Mario Kart World", (int)(1280 * ds), (int)(720 * ds), f);
    L.ren = L.win ? SDL_CreateRenderer(L.win, nullptr) : nullptr;
    if (!L.ren) {
        rt_log("launcher: no window: %s", SDL_GetError());
        return false;
    }
    SDL_SetRenderVSync(L.ren, 1);
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

extern "C" int launcher_run(void) {
    L.result = -1;
    L.focus_main = 1;
    exe_dir(L.exe_dir, sizeof L.exe_dir);
    parse_test();
    if (!open_window()) return LAUNCHER_PLAY;   // no start screen: the game itself says what is wrong
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    menu_look_style(L.base_style);
    ImGui_ImplSDL3_InitForSDLRenderer(L.win, L.ren);
    ImGui_ImplSDLRenderer3_Init(L.ren);
    list_gpus();
    load_splash();
    if (L.shot_at) {
        L.page = L.shot_page;
        if (L.page == PAGE_SETTINGS) {
            settings_select(L.shot_tab);
            settings_opened();
        }
    }
    L.tools = setup_available(L.exe_dir);
    if (!rt_game_present() && !L.shot_at) L.page = PAGE_FILES;
    if (getenv("MKW_LAUNCHER_SHORTCUT")) make_shortcut();   // (test) as the button does
    if (const char *t = getenv("MKW_LAUNCHER_SETUP")) {   // (test) the setup started from that folder, as Set up does
        snprintf(L.source, sizeof L.source, "%s", t);
        char *link = strstr(L.source, ":link");
        if (link) *link = 0, L.link_romfs = 1;
        L.page = PAGE_FILES;
        setup_start(L.exe_dir, L.source, L.link_romfs);
    }
    rt_log("launcher: start screen, game files %s, setup tools %s", rt_game_present() ? "ready" : "not set up",
           L.tools ? "present" : "missing");
    for (long frame = 1; L.result < 0; frame++) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
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
        background(ImGui::GetBackgroundDrawList(), (float)pw, (float)ph);
        switch (L.page) {
        case PAGE_MAIN: page_main((float)pw, (float)ph); break;
        case PAGE_SETTINGS: page_settings((float)pw, (float)ph); break;
        case PAGE_FILES: page_files((float)pw, (float)ph); break;
        }
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
        SDL_RenderPresent(L.ren);
    }
    if (L.save_pending) opt_save_tick(1);
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    if (L.splash) SDL_DestroyTexture(L.splash);
    SDL_DestroyRenderer(L.ren);
    SDL_DestroyWindow(L.win);
    SDL_Quit();
    rt_log("launcher: %s", L.result == LAUNCHER_PLAY ? "play" : "quit");
    return L.result;
}
