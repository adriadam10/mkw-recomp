// The settings pages: the tab bar (L / R or Q / E switch tabs) and the rows of each tab over the cfg settings (core/base/cfg.h),
// shared by the in-game menu (ui/menu/menu.cpp) and the start screen (ui/launcher/launcher.cpp). Split from menu.cpp, whose look
// follows SavageWindRecomp's runtime/ui/menu.mm (MIT); the rows are option_rows.cpp's.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "imgui.h"
#include "ui/menu/binds_page.h"
#include "ui/menu/option_rows.h"
#include "ui/glyphs/glyphs.h"
#include "ui/menu/settings_tabs.h"
#include "ui/launcher/libs_fetch.h"
#include "plugin/plugin.h"

extern "C" {
#include "cfg.h"
#include "fs.h"
#include "gfx.h"
#include "input.h"
#include "input_map.h"
#include "platform/platform.h"
#include "sysinfo.h"
#include "ui/menu/menu.h"
#include "sdk/ov/ov_lanif.h"
#include "ui_text.h"
void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
}

// ---- the lists the Screen and General tabs build
enum { MAX_GPUS = 8 };
static char g_gpu_names[MAX_GPUS][VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
static int g_gpu_n = -1;   // -1: not listed yet
static uint32_t g_gpu_vram_mb[MAX_GPUS];   // the largest device-local heap, for Auto-set
static bool g_gpu_discrete[MAX_GPUS];
static bool g_gpu_nvidia[MAX_GPUS];   // DLSS's files are downloaded only for an NVIDIA card (its licence: theirs only)

// The Vulkan devices in vkEnumeratePhysicalDevices order, which MKW_GPU_INDEX counts in (vk_device.c pick_device).
void settings_list_gpus(VkInstance inst) {
    if (g_gpu_n >= 0) return;
    VkPhysicalDevice pd[MAX_GPUS];
    uint32_t n = MAX_GPUS;
    if (vkEnumeratePhysicalDevices(inst, &n, pd) < 0) n = 0;   // VK_INCOMPLETE keeps the first MAX_GPUS
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pd[i], &p);
        snprintf(g_gpu_names[i], sizeof g_gpu_names[i], "%s", p.deviceName);
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(pd[i], &mp);
        uint64_t vram = 0;
        for (uint32_t h = 0; h < mp.memoryHeapCount; h++)
            if (mp.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT && mp.memoryHeaps[h].size > vram)
                vram = mp.memoryHeaps[h].size;
        g_gpu_vram_mb[i] = (uint32_t)(vram >> 20);
        g_gpu_discrete[i] = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
        g_gpu_nvidia[i] = p.vendorID == 0x10DE;
    }
    g_gpu_n = (int)n;
}

// Window sizes: the common 16:9 sizes that fit the monitor, the 1920x1080 default and the current size.
static const int kSizes[][2] = {{1280, 720}, {1366, 768}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3200, 1800},
                                {3840, 2160}};
enum { MAX_SIZES = sizeof kSizes / sizeof *kSizes + 1 };

static void row_window_size(void) {
    int sz[MAX_SIZES][2], n = 0, mw = 0, mh = 0, cw = 0, ch = 0;
    int have = gfx_window_monitor_size(&mw, &mh);
    int cur_ok = sscanf(cfg_value("MKW_WINDOW_SIZE"), "%dx%d", &cw, &ch) == 2;
    bool listed = false;
    for (size_t i = 0; i < sizeof kSizes / sizeof *kSizes; i++) {
        int w = kSizes[i][0], h = kSizes[i][1];
        bool cur = cur_ok && w == cw && h == ch;
        if (have && (w > mw || h > mh) && !cur && !(w == 1920 && h == 1080)) continue;
        listed |= cur;
        sz[n][0] = w, sz[n][1] = h, n++;
    }
    if (cur_ok && !listed) {   // the current size where its area puts it
        int at = n;
        while (at > 0 && (long)sz[at - 1][0] * sz[at - 1][1] > (long)cw * ch) at--;
        memmove(sz[at + 1], sz[at], sizeof sz[0] * (n - at));
        sz[at][0] = cw, sz[at][1] = ch, n++;
    }
    char vals[MAX_SIZES][24], shown[MAX_SIZES][32];
    const char *pv[MAX_SIZES], *pn[MAX_SIZES];
    for (int i = 0; i < n; i++) {
        snprintf(vals[i], sizeof vals[i], "%dx%d", sz[i][0], sz[i][1]);
        snprintf(shown[i], sizeof shown[i], "%d × %d", sz[i][0], sz[i][1]);   // U+00D7, the multiplication sign
        pv[i] = vals[i];
        pn[i] = shown[i];
    }
    opt_choice_list("MKW_WINDOW_SIZE", UI_MENU_WINDOW_SIZE, pv, pn, n, UI_MENU_WINDOW_SIZE_DESC);
}

static void row_gpu(void) {
    const char *vals[MAX_GPUS + 1], *names[MAX_GPUS + 1];
    char idx[MAX_GPUS][4];
    vals[0] = "";
    names[0] = menu_text(UI_MENU_GPU_AUTO);
    for (int i = 0; i < g_gpu_n; i++) {
        snprintf(idx[i], sizeof idx[i], "%d", i);
        vals[i + 1] = idx[i];
        names[i + 1] = g_gpu_names[i];
    }
    opt_choice_list("MKW_GPU_INDEX", UI_MENU_GPU, vals, names, g_gpu_n + 1, UI_MENU_GPU_DESC);
}

// The game language: one entry per language MKW ships (romfs/Mals: a Switch code that names it), each under its
// own name.
static void row_game_lang(void) {
    static const struct { const char *code; UiTextId name; } langs[] = {
        {"en-US", UI_MENU_LANGNAME_EN}, {"ja", UI_MENU_LANGNAME_JA}, {"fr", UI_MENU_LANGNAME_FR},
        {"de", UI_MENU_LANGNAME_DE}, {"it", UI_MENU_LANGNAME_IT}, {"es", UI_MENU_LANGNAME_ES},
        {"nl", UI_MENU_LANGNAME_NL}, {"pt", UI_MENU_LANGNAME_PT}, {"pt-BR", UI_MENU_LANGNAME_PT_BR},
        {"ru", UI_MENU_LANGNAME_RU},
        {"zh-CN", UI_MENU_LANGNAME_ZH_HANS}, {"zh-TW", UI_MENU_LANGNAME_ZH_HANT}, {"ko", UI_MENU_LANGNAME_KO},
        // ours, rebuilt over the game's Spanish and British English (tools/i18n/game_texts.py)
        {"ca", UI_MENU_LANGNAME_CA}, {"gl", UI_MENU_LANGNAME_GL}, {"eu", UI_MENU_LANGNAME_EU},
        {"tr", UI_MENU_LANGNAME_TR},
        // added by updates (Polish 1.5.0, Thai 1.7.0): listed only when this copy of the game ships them
        {"pl", UI_MENU_LANGNAME_PL}, {"th", UI_MENU_LANGNAME_TH},
    };
    int n_langs = 0;
    for (const auto &l : langs)
        if ((strcmp(l.code, "pl") && strcmp(l.code, "th")) || romfs_has_language(l.code)) n_langs++;
    const int N = n_langs + 1;
    const char *vals[sizeof langs / sizeof *langs + 1], *names[sizeof langs / sizeof *langs + 1];
    vals[0] = "";
    names[0] = menu_text(UI_MENU_GAME_LANG_AUTO);
    const char *now = cfg_value("MKW_LANG");
    int cur = *now ? -1 : 0;
    // Prefer a dedicated row over a shared UI catalog. Aliases such as en-GB still show their language.
    int i = 1;
    for (const auto &l : langs) {
        if ((!strcmp(l.code, "pl") || !strcmp(l.code, "th")) && !romfs_has_language(l.code)) continue;
        vals[i] = l.code;
        names[i] = menu_text(l.name);
        if (!strcmp(now, vals[i])) cur = i;
        else if (cur < 0 && !strcmp(ui_text_lang(now), ui_text_lang(vals[i]))) cur = i;
        i++;
    }
    opt_choice_list("MKW_LANG", UI_MENU_GAME_LANG, vals, names, N, UI_MENU_GAME_LANG_DESC, cur);
}

// The menu's name of a MKW_VK_PRESENT mode (gfx_window_present_mode); NULL for another.
const char *settings_present_name(const char *mode) {
    static const struct { const char *mode; UiTextId name; } names[] = {
        {"mailbox", UI_MENU_PRESENT_MAILBOX}, {"fifo", UI_MENU_PRESENT_VSYNC}, {"immediate", UI_MENU_PRESENT_IMMEDIATE},
    };
    for (const auto &n : names)
        if (!strcmp(n.mode, mode)) return menu_text(n.name);
    return nullptr;
}

// "in use: VSync" under the Presentation value when the swapchain got another mode than the setting (a driver without
// mailbox gets fifo); NULL when they agree or before the first swapchain.
static const char *present_note(void) {
    const char *got = gfx_window_present_mode(), *name = settings_present_name(got);
    if (!*got || !strcmp(got, cfg_value("MKW_VK_PRESENT")) || !name) return nullptr;
    static char note[96];
    snprintf(note, sizeof note, menu_text(UI_MENU_PRESENT_IN_USE), name);
    return note;
}

// ---- the tabs
static void hdr_rows(void);   // after the HDR row, below

// The window and the picture's shape on it.
static void tab_screen(float scale) {
    opt_begin("##screen", scale);
    opt_toggle("MKW_FULLSCREEN", UI_MENU_FULLSCREEN, UI_MENU_FULLSCREEN_DESC);
    row_window_size();
    row_gpu();
    static const UiTextId present[] = {UI_MENU_PRESENT_MAILBOX, UI_MENU_PRESENT_VSYNC, UI_MENU_PRESENT_IMMEDIATE};
    opt_choice("MKW_VK_PRESENT", UI_MENU_PRESENT, present, 3, UI_MENU_PRESENT_DESC, present_note());
    opt_group(UI_MENU_GRAPHICS);
    static const UiTextId fit[] = {UI_MENU_PICTURE_FIT_KEEP, UI_MENU_PICTURE_FIT_STRETCH};
    opt_choice("MKW_PICTURE_FIT", UI_MENU_PICTURE_FIT, fit, 2, UI_MENU_PICTURE_FIT_DESC);
    {   // the cameras' aspect and the output's width (core/boot/patches.c, set aspect), read at start
        static const char *const vals[] = {"16:9", "window", "21:9", "32:9", "48:9"};   // 48:9: three screens, #264
        const char *names[5] = {"16:9", menu_text(UI_MENU_ASPECT_WINDOW), "21:9", "32:9", "48:9"};
        opt_choice_list("MKW_ASPECT", UI_MENU_ASPECT, vals, names, 5, UI_MENU_ASPECT_DESC);
    }
    {   // the race HUD in the middle 16:9 of a wider or taller picture, or at its edges (core/boot/patches.c), at start
        static const char *const vals[] = {"16:9", "edges"};
        const char *names[2] = {menu_text(UI_MENU_HUD_POSITION_16X9), menu_text(UI_MENU_HUD_POSITION_EDGES)};
        opt_choice_list("MKW_HUD_POSITION", UI_MENU_HUD_POSITION, vals, names, 2, UI_MENU_HUD_POSITION_DESC);
    }
    {   // the game's own HDR picture on an HDR display (gfx/nvn/host/gfx_hdr.c), live
        static const char *const vals[] = {"auto", "off"};
        const char *names[2] = {menu_text(UI_MENU_FPS_AUTO), menu_text(UI_MENU_PERF_OFF)};
        opt_choice_list("MKW_HDR", UI_MENU_HDR, vals, names, 2, UI_MENU_HDR_DESC);
    }
    hdr_rows();
    opt_end();
}

// The HDR rows after HDR: the tone mapper (gfx_shader.c; its programs come from tools/shaders/renodx.py, so without
// them the row is greyed out) and the peak and paper white the game is told (gfx_hdr.c), all only while HDR is on, and
// the two values only with PsychoV35 (with the game's curve the console's system values hold).
static void hdr_rows(void) {
    uint16_t peak, white;
    const bool on = gfx_hdr_output(&peak, &white) != 0;
    {
        static const char *const vals[] = {"vanilla", "psychov"};
        const char *names[2] = {menu_text(UI_MENU_HDR_TONEMAP_VANILLA), menu_text(UI_MENU_HDR_TONEMAP_PSYCHOV)};
        static int have = -1;
        if (have < 0) {
            char p[1200];
            const char *sp = cfg_get("MKW_SPIRV");
            if (sp && *sp) snprintf(p, sizeof p, "%s-renodx", sp);   // gfx_shader.c tonemap_dir
            else snprintf(p, sizeof p, "%s/spirv-renodx", cfg_game_dir());
            have = plat_dir_exists(p);
        }
        if (!have) opt_disable_next(UI_MENU_HDR_TONEMAP_MISSING);
        else if (!on) opt_disable_next(UI_MENU_HDR_DESC);
        opt_choice_list("MKW_HDR_TONEMAP", UI_MENU_HDR_TONEMAP, vals, names, 2, UI_MENU_HDR_TONEMAP_DESC);
    }
    // 1 nit a step (ten held); automatic below the lowest, right from it starts at the system's value
    char fmt[32];
    snprintf(fmt, sizeof fmt, menu_text(UI_MENU_HDR_NITS), "%d");
    unsigned sp = 1000, sw = 203;
    sscanf(cfg_value("MKW_HDR_SYSTEM"), "%u/%u", &sp, &sw);
    const bool psychov = !strcmp(cfg_value("MKW_HDR_TONEMAP"), "psychov");
    if (!on) opt_disable_next(UI_MENU_HDR_DESC);
    else if (!psychov) opt_disable_next(UI_MENU_HDR_NEEDS_PSYCHOV);
    opt_slider_auto("MKW_HDR_PEAK", UI_MENU_HDR_PEAK, 100, 10000, UI_MENU_HDR_PEAK_DESC, fmt, menu_text(UI_MENU_FPS_AUTO), (int)sp);
    if (!on) opt_disable_next(UI_MENU_HDR_DESC);
    else if (!psychov) opt_disable_next(UI_MENU_HDR_NEEDS_PSYCHOV);
    opt_slider_auto("MKW_HDR_WHITE", UI_MENU_HDR_WHITE, 40, 1000, UI_MENU_HDR_WHITE_DESC, fmt, menu_text(UI_MENU_FPS_AUTO), (int)sw);
}

// The Quality presets: the values each one gives the four rows below it, in the order
// of kQualityKeys. Console is every row's default. The others are a first guess until each row's cost is measured on
// the Radeon 660M and the GTX 1050 Ti (its PLAN).
static const char *const kQualityKeys[] = {"MKW_ANISO", "MKW_TEX_QUALITY", "MKW_DETAIL_DISTANCE", "MKW_SHADOW_SIZE"};
static const struct { const char *name; const char *v[4]; } kPresets[] = {
    {"low", {"game", "quarter", "1", "1024"}},     {"medium", {"game", "half", "1.5", "2048"}},
    {"console", {"game", "full", "2", "console"}}, {"high", {"8", "full", "3", "4096"}},
    {"ultra", {"16", "full", "4", "4096"}},
};
enum { NPRESETS = sizeof kPresets / sizeof *kPresets };

// The preset whose values the four rows hold now; NPRESETS (Custom) when none.
static int quality_now(void) {
    for (int p = 0; p < NPRESETS; p++) {
        int k = 0;
        while (k < 4 && !strcmp(cfg_value(kQualityKeys[k]), kPresets[p].v[k])) k++;
        if (k == 4) return p;
    }
    return NPRESETS;
}

static void row_quality(void) {
    const char *vals[NPRESETS + 1], *names[NPRESETS + 1];
    static const UiTextId shown[] = {UI_MENU_DETAIL_LOW, UI_MENU_DETAIL_MEDIUM, UI_MENU_DETAIL_CONSOLE,
                                     UI_MENU_DETAIL_HIGH, UI_MENU_DETAIL_ULTRA};
    for (int p = 0; p < NPRESETS; p++) vals[p] = kPresets[p].name, names[p] = menu_text(shown[p]);
    vals[NPRESETS] = "custom", names[NPRESETS] = menu_text(UI_MENU_QUALITY_CUSTOM);
    if (!opt_choice_list("MKW_QUALITY", UI_MENU_QUALITY, vals, names, NPRESETS + 1, UI_MENU_QUALITY_DESC,
                         quality_now()))
        return;
    const char *picked = cfg_value("MKW_QUALITY");
    for (int p = 0; p < NPRESETS; p++)
        if (!strcmp(picked, kPresets[p].name))
            for (int k = 0; k < 4; k++) cfg_set(kQualityKeys[k], kPresets[p].v[k]);
}

// Auto-set: the preset, the internal resolution and AMD FSR 1 from the graphics card in use and the screen, only when
// the player asks (the defaults stay the console's). A first guess from the cards in the README's table, like the
// presets: no dedicated card or under 6 GB (GTX 1050 Ti, 1650 SUPER, RTX 3050 Laptop: 22-37 fps at 720-1080p) Medium
// at 720p; under 16 GB (RTX 3060: ~48 fps at 1080p) Console at 1080p; above (RX 7900 XTX: 60 fps at 2160p) High at the
// screen's height. Never above the screen. Undo puts back what it changed.
static const char *const kAutoKeys[] = {"MKW_QUALITY",         "MKW_ANISO",       "MKW_TEX_QUALITY", "MKW_DETAIL_DISTANCE",
                                        "MKW_SHADOW_SIZE", "MKW_RESOLUTION", "MKW_UPSCALER"};
enum { NAUTO = sizeof kAutoKeys / sizeof *kAutoKeys };
static char g_auto_before[NAUTO][32];   // the rows' values before Auto-set ("" unset)
static bool g_auto_done;

// The card the game runs on: MKW_GPU_INDEX, else the first dedicated one, as the device pick prefers; 0 without a list
static int gpu_index(void) {
    const char *pick = cfg_get("MKW_GPU_INDEX");
    int gi = pick && *pick ? atoi(pick) : -1;
    for (int i = 0; gi < 0 && i < g_gpu_n; i++)
        if (g_gpu_discrete[i]) gi = i;
    return gi < 0 || gi >= g_gpu_n ? 0 : gi;
}

static void auto_set(void) {
    int gi = gpu_index();
    // the game's window when there is one (the in-game menu), else the primary display (the start screen has no game
    // window: gfx_window_monitor_size says nothing there)
    int mw = 0, mh = 0;
    if (!gfx_window_monitor_size(&mw, &mh)) {
        const SDL_DisplayMode *m = SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay());
        mh = m ? (int)(m->h * m->pixel_density + 0.5f) : 1080;
    }
    uint32_t mb = g_gpu_n > 0 ? g_gpu_vram_mb[gi] : 0;
    bool weak = g_gpu_n <= 0 || !g_gpu_discrete[gi] || mb < 6 * 1024, strong = !weak && mb >= 16 * 1024;
    const char *preset = weak ? "medium" : strong ? "high" : "console";
    int want = weak ? 720 : strong ? mh : 1080;
    static const int kRes[] = {540, 720, 900, 1080, 1440, 2160};
    int res = kRes[0];
    for (int r : kRes)
        if (r <= want && r <= mh) res = r;
    for (int k = 0; k < NAUTO && !g_auto_done; k++) {   // pressed again, Undo still goes back to before the first
        const char *v = cfg_get(kAutoKeys[k]);
        snprintf(g_auto_before[k], sizeof g_auto_before[k], "%s", v ? v : "");
    }
    for (int p = 0; p < NPRESETS; p++)
        if (!strcmp(kPresets[p].name, preset))
            for (int k = 0; k < 4; k++) cfg_set(kQualityKeys[k], kPresets[p].v[k]);
    char r[8];
    snprintf(r, sizeof r, "%d", res);
    cfg_set("MKW_QUALITY", preset);
    cfg_set("MKW_RESOLUTION", r);
    cfg_set("MKW_UPSCALER", "fsr");
    g_auto_done = true;
    opt_save_soon();
    rt_log("menu: auto-set for %s (%u MB, %s), screen %d lines: %s at %dp", g_gpu_n > 0 ? g_gpu_names[gi] : "no device",
           mb, weak ? "light" : strong ? "strong" : "middle", mh, preset, res);
}

static void auto_undo(void) {
    for (int k = 0; k < NAUTO; k++) {
        if (g_auto_before[k][0]) cfg_set(kAutoKeys[k], g_auto_before[k]);
        else cfg_reset(&kAutoKeys[k], 1);
    }
    g_auto_done = false;
    opt_save_soon();
}

// What the graphics card draws and how much, each row the console's by default: the preset first with the four rows it
// sets hanging under it, then the resolution, then how fast frames come.
// Under an upscaler, frame generation engine or Reflex row: its value applies after a restart (1), or this PC cannot
// run it (2: the game's own picture and pacing stay)
static void engine_note(int state) {
    if (state == 1) opt_text(UI_MENU_AFTER_RESTART, true);
    else if (state == 2) opt_text(UI_MENU_NOT_AVAILABLE, true);
}

// The libraries the upscaler, the frame generation engine and RenHDrum picked now need (tools/prepare/libs.tsv's
// features), fetched beside the program when that pick changes (ui/launcher/libs_fetch.cpp): players had to download
// and copy them by hand. Windows only, where the runtime loads them; DLSS only for an NVIDIA card, its licence's
// condition.
static void libs_sync(void) {
#ifdef _WIN32
    bool nv = g_gpu_n <= 0 || g_gpu_nvidia[gpu_index()];   // no list yet: the runtime still says when it cannot run
    const char *t = cfg_value("MKW_TEMPORAL"), *e = cfg_value("MKW_FG_ENGINE");
    bool fg = strcmp(cfg_value("MKW_FRAMEGEN"), "off") != 0;
    char want[64] = "";
    auto add = [&](const char *f) { snprintf(want + strlen(want), sizeof want - strlen(want), "%s%s", *want ? "," : "", f); };
    if (nv && !strcmp(t, "dlss")) add("dlss");
    if (!strcmp(t, "fsr3")) add("fsr3");
    if (fg && nv && !strcmp(e, "dlss")) add("dlssg");
    if (fg && !strcmp(e, "fsr3")) add("fsr3,fsr3fg");
    if (!strcmp(cfg_value("MKW_RENHDRUM"), "1")) add("renhdrum");
    static char last[64];
    if (!strcmp(want, last)) return;
    snprintf(last, sizeof last, "%s", want);
    if (*want) libs_fetch_start(want);
#endif
}

// engine_note, or what the download of feature (name: the engine's) is doing: its progress, its failure, or that the
// files it brought load at the next start (an engine that found none at start says "not available" until then)
static void libs_note(const char *feature, const char *name, int state) {
    LibsState s = libs_state();
    char text[512];
    bool mine = state != 0 && strstr(s.features, feature);
    if (mine && s.running) {
        int pct = s.total > 0 ? (int)(s.done * 100 / s.total) : 0;
        snprintf(text, sizeof text, menu_text(UI_MENU_LIB_FETCHING), name, pct);
        opt_text(text);
    } else if (mine && s.finished < 0) {
        snprintf(text, sizeof text, menu_text(UI_MENU_LIB_FAILED), name, s.error);
        opt_text(text, true);
    } else if (mine && s.fetched > 0) {
        snprintf(text, sizeof text, menu_text(UI_MENU_LIB_READY), name);
        opt_text(text, true);
    } else engine_note(state);
}

static void tab_graphics(float scale) {
    opt_begin("##graphics", scale);
    static bool auto_test = getenv("MKW_LAUNCHER_AUTOSET") != nullptr;   // (test) once, as the button does
    if (opt_button("##autoset", UI_MENU_AUTOSET, UI_MENU_AUTOSET_DESC) || auto_test) auto_set(), auto_test = false;
    if (g_auto_done && opt_button("##autoset_undo", UI_MENU_AUTOSET_UNDO, UI_MENU_AUTOSET_UNDO_DESC)) auto_undo();
    row_quality();
    opt_indent(true);
    {   // a floor on anisotropic filtering (gfx/vk/tex/vk_tex_slots.c make_sampler)
        static const char *const vals[] = {"game", "2", "4", "8", "16"};
        const char *names[5] = {menu_text(UI_MENU_ANISO_GAME), "2x", "4x", "8x", "16x"};
        opt_choice_list("MKW_ANISO", UI_MENU_ANISO, vals, names, 5, UI_MENU_ANISO_DESC);
    }
    static const UiTextId texq[] = {UI_MENU_TEX_QUALITY_FULL, UI_MENU_TEX_QUALITY_HALF, UI_MENU_TEX_QUALITY_QUARTER};
    opt_choice("MKW_TEX_QUALITY", UI_MENU_TEX_QUALITY, texq, 3, UI_MENU_TEX_QUALITY_DESC);
    {   // the game's LODDistanceRatio (sdk/fs/romfs_res.c), 2 on the console
        static const char *const vals[] = {"1", "1.5", "2", "3", "4"};
        const char *names[5] = {menu_text(UI_MENU_DETAIL_LOW), menu_text(UI_MENU_DETAIL_MEDIUM),
                                menu_text(UI_MENU_DETAIL_CONSOLE), menu_text(UI_MENU_DETAIL_HIGH),
                                menu_text(UI_MENU_DETAIL_ULTRA)};
        opt_choice_list("MKW_DETAIL_DISTANCE", UI_MENU_DETAIL, vals, names, 5, UI_MENU_DETAIL_DESC);
    }
    {   // the cascaded shadow map size (core/boot/patches.c, set shadow)
        static const char *const vals[] = {"console", "1024", "2048", "3072", "4096"};
        const char *names[5] = {menu_text(UI_MENU_DETAIL_CONSOLE), "1024", "2048", "3072", "4096"};
        opt_choice_list("MKW_SHADOW_SIZE", UI_MENU_SHADOW, vals, names, 5, UI_MENU_SHADOW_DESC);
    }
    opt_indent(false);
    opt_group(UI_MENU_GROUP_RESOLUTION);
    {   // the game's internal render resolution (sdk/fs/romfs_res.c), read at start
        static const char *const vals[] = {"original", "540", "720", "900", "1080", "1440", "2160"};   // 2160: issue #55
        const char *names[7] = {menu_text(UI_MENU_RESOLUTION_ORIGINAL), "540p", "720p", "900p", "1080p", "1440p", "2160p"};
        opt_choice_list("MKW_RESOLUTION", UI_MENU_RESOLUTION, vals, names, 7, UI_MENU_RESOLUTION_DESC);
    }
    static const UiTextId up[] = {UI_MENU_UPSCALER_FSR, UI_MENU_UPSCALER_BILINEAR};
    opt_choice("MKW_UPSCALER", UI_MENU_UPSCALER, up, 2, UI_MENU_UPSCALER_DESC);
    if (strcmp(cfg_value("MKW_UPSCALER"), "fsr")) opt_disable_next(UI_MENU_SHARPNESS_FSR_ONLY);   // FSR's own pass
    opt_slider("MKW_SHARPNESS", UI_MENU_SHARPNESS, 0, 100, UI_MENU_SHARPNESS_DESC, 5, "%.0f %%");
    {   // DLSS or FSR 3.1 in the place of the game's upscale (gfx/vk/exec/vk_temporal.c): the engine live, the quality
        // at the next start
        static const char *const vals[] = {"off", "dlss", "fsr3"};
        const char *names[3] = {menu_text(UI_MENU_PERF_OFF), "NVIDIA DLSS", "AMD FSR 3.1"};
        opt_choice_list("MKW_TEMPORAL", UI_MENU_TEMPORAL, vals, names, 3, UI_MENU_TEMPORAL_DESC);
        libs_sync();
        bool dlss = !strcmp(cfg_value("MKW_TEMPORAL"), "dlss");
        libs_note(dlss ? "dlss" : "fsr3", names[dlss ? 1 : 2], vk_tmp_menu_state("MKW_TEMPORAL"));
        static const char *const qv[] = {"native", "quality", "balanced", "performance", "ultra"};
        const char *qn[5] = {menu_text(UI_MENU_TQ_NATIVE), menu_text(UI_MENU_TQ_QUALITY), menu_text(UI_MENU_TQ_BALANCED),
                              menu_text(UI_MENU_TQ_PERFORMANCE), menu_text(UI_MENU_TQ_ULTRA)};
        opt_choice_list("MKW_TEMPORAL_QUALITY", UI_MENU_TEMPORAL_QUALITY, qv, qn, 5, UI_MENU_TEMPORAL_QUALITY_DESC);
        static const char *const pv[] = {"default", "j", "k", "l", "m"};
        const char *pn[5] = {menu_text(UI_MENU_DLSS_PRESET_DEFAULT), "J", "K", "L", "M"};
        opt_choice_list("MKW_DLSS_PRESET", UI_MENU_DLSS_PRESET, pv, pn, 5, UI_MENU_DLSS_PRESET_DESC);
        static const char *const cv[] = {"off", "on"};   // CAS on the upscaler's picture (vk_temporal.c), live
        const char *cn[2] = {menu_text(UI_MENU_PERF_OFF), menu_text(UI_MENU_ON)};
        opt_choice_list("MKW_CAS", UI_MENU_CAS, cv, cn, 2, UI_MENU_CAS_DESC);
        opt_slider("MKW_CAS_SHARPNESS", UI_MENU_CAS_SHARPNESS, 0, 100, UI_MENU_CAS_SHARPNESS_DESC, 5, "%.0f %%");
    }
    static const UiTextId mode[] = {UI_MENU_MODE_DOCKED, UI_MENU_MODE_HANDHELD};
    opt_choice("MKW_OPERATION_MODE", UI_MENU_OPERATION_MODE, mode, 2, UI_MENU_OPERATION_MODE_DESC);
    opt_group(UI_MENU_GROUP_PERFORMANCE);
    {   // the game's present interval (core/boot/game.c), live; "game" (its own 1-2 choice) is for A/B only
        static const char *const vals[] = {"auto", "60", "30", "20", "15"};
        const char *names[5] = {menu_text(UI_MENU_FPS_AUTO), "60", "30", "20", "15"};
        opt_choice_list("MKW_FPS", UI_MENU_FPS, vals, names, 5, UI_MENU_FPS_DESC);
    }
    {   // game steps a second (core/boot/game.c frame_logic_hz), read at start
        static const char *const vals[] = {"60", "120", "144", "240"};
        const char *names[4] = {"60 Hz", "120 Hz", "144 Hz", "240 Hz"};
        opt_choice_list("MKW_LOGIC_HZ", UI_MENU_LOGIC_HZ, vals, names, 4, UI_MENU_LOGIC_HZ_DESC);
    }
    {   // frames made between the game's (gfx/vk/device/vk_framegen.c), live
        static const char *const vals[] = {"off", "auto", "2", "3", "4", "120"};
        const char *names[6] = {menu_text(UI_MENU_PERF_OFF), menu_text(UI_MENU_FPS_AUTO), "2x", "3x", "4x", "120 fps"};
        opt_choice_list("MKW_FRAMEGEN", UI_MENU_FRAMEGEN, vals, names, 6, UI_MENU_FRAMEGEN_DESC);
        if (vk_fg_uneven()) opt_text(UI_MENU_FRAMEGEN_UNEVEN, true);   // a fixed factor that judders on this screen
    }
    {   // who makes those frames (gfx/vk/device/vk_framegen.c, gfx/vk/exec/vk_temporal.c), live
        static const char *const vals[] = {"flow", "dlss", "fsr3"};
        const char *names[3] = {"MKW Recomp", "NVIDIA DLSS", "AMD FSR 3.1"};
        opt_choice_list("MKW_FG_ENGINE", UI_MENU_FG_ENGINE, vals, names, 3, UI_MENU_FG_ENGINE_DESC);
        libs_sync();
        bool dlss = !strcmp(cfg_value("MKW_FG_ENGINE"), "dlss");
        libs_note(dlss ? "dlssg" : "fsr3fg", names[dlss ? 1 : 2], vk_tmp_menu_state("MKW_FG_ENGINE"));
    }
    {   // NVIDIA Reflex (gfx/vk/device/vk_reflex_frame.c), live
        static const char *const vals[] = {"off", "on", "boost"};
        const char *names[3] = {menu_text(UI_MENU_PERF_OFF), menu_text(UI_MENU_ON), menu_text(UI_MENU_REFLEX_BOOST)};
        opt_choice_list("MKW_REFLEX", UI_MENU_REFLEX, vals, names, 3, UI_MENU_REFLEX_DESC);
        engine_note(vk_rfx_menu_state());
    }
    opt_toggle("MKW_VRR", UI_MENU_VRR, UI_MENU_VRR_DESC);   // gfx/vk/device/vk_framegen.c fits, live
    opt_toggle("MKW_LOW_LATENCY", UI_MENU_LOW_LATENCY, UI_MENU_LOW_LATENCY_DESC);   // core/boot/frame_jit.c, live
    static const UiTextId perf[] = {UI_MENU_PERF_OFF, UI_MENU_PERF_FPS, UI_MENU_PERF_FULL};
    opt_choice("MKW_PERF_OVERLAY", UI_MENU_PERF_OVERLAY, perf, 3, UI_MENU_PERF_OVERLAY_DESC);
    opt_end();
}

static void tab_sound(float scale) {
    opt_begin("##sound", scale);
    opt_slider("MKW_VOLUME", UI_MENU_VOLUME, 0, 100, UI_MENU_VOLUME_DESC, 5, "%.0f %%");
    {   // the game's 5.1 mix on a 6-channel output (audio/audio_mixer.c audio_output_channels), at the next start
        static const char *const vals[] = {"auto", "2", "6"};
        const char *names[3] = {menu_text(UI_MENU_FPS_AUTO), menu_text(UI_MENU_SPEAKERS_STEREO),
                                menu_text(UI_MENU_SPEAKERS_51)};
        opt_choice_list("MKW_AUDIO_CHANNELS", UI_MENU_SPEAKERS, vals, names, 3, UI_MENU_SPEAKERS_DESC);
    }
    opt_toggle("MKW_BACKGROUND_MUTE", UI_MENU_BACKGROUND_MUTE, UI_MENU_BACKGROUND_MUTE_DESC);   // audio/audio_mixer.c
    opt_end();
}

// "in use: Xbox" under the layout when it is Automatic and a gamepad is connected; NULL otherwise.
static const char *pad_layout_note(void) {
    int got = input_pad_layout();
    if (got < 0 || strcmp(cfg_value("MKW_PAD_LAYOUT"), "auto")) return nullptr;
    static char note[96];
    snprintf(note, sizeof note, menu_text(UI_MENU_PAD_LAYOUT_IN_USE),
             menu_text(got == PAD_LAYOUT_XBOX ? UI_MENU_PAD_LAYOUT_XBOX : UI_MENU_PAD_LAYOUT_NINTENDO));
    return note;
}

// "in use: Xbox" under the icons when they are Automatic; NULL otherwise.
static const char *button_icons_note(void) {
    if (strcmp(cfg_value("MKW_BUTTON_ICONS"), "auto")) return nullptr;
    static const UiTextId names[] = {UI_MENU_ICONS_NINTENDO, UI_MENU_ICONS_XBOX, UI_MENU_ICONS_PLAYSTATION,
                                     UI_MENU_ICONS_KEYBOARD};
    static char note[96];
    snprintf(note, sizeof note, menu_text(UI_MENU_ICONS_IN_USE), menu_text(names[glyphs_kind()]));
    return note;
}

// Each player's controller and its battery (input/input_power.c), over the settings (discussion #516). On the start
// screen the gamepads start on first use and follow hotplug through input_hotplug (the main page keeps its own list).
static void pad_rows(void) {
    if (!input_gamepads_running()) return;
    opt_group(UI_MENU_PADS);
    int shown = 0;
    for (int p = 0; p < INPUT_MAX_PLAYERS; p++) {
        InputPower w;
        if (input_pad_power(p, &w) == INPUT_POWER_NONE) continue;
        char id[16], label[128], value[64];
        snprintf(id, sizeof id, "##pad%d", p + 1);
        snprintf(label, sizeof label, menu_text(UI_MENU_PAD_PLAYER), p + 1, w.name);
        input_power_text(&w, value, sizeof value);
        // A controller with a player before it trades places with that player's, so any order takes a few presses.
        // Not on the start screen: Play runs the game as another process, which gives them out again.
        if (opt_start_screen() || !input_pad_can_move_up(p)) opt_info(id, label, value, UI_MENU_PAD_BATTERY_DESC);
        else if (opt_button_text(id, label, UI_MENU_PAD_MOVE_DESC, value) && input_pad_swap(p, p - 1))
            input_rumble(p - 1, 0x8000, 0x8000, 250);   // which one moved, among equal ones
        shown++;
    }
    if (!shown) opt_text(UI_MENU_PADS_NONE);
}

// Under the Menu shortcut description while Guide is picked: Steam's overlay opens on it too (as does the
// Xbox Game Bar on Windows), so the game may never see it.
static const char *shortcut_guide_note(void) {
    if (strcmp(cfg_value("MKW_MENU_SHORTCUT"), "guide")) return nullptr;
    return menu_text(UI_MENU_SHORTCUT_STEAM_NOTE);
}

static void tab_controls(float scale) {
    opt_begin("##controls", scale);
    pad_rows();
    static const UiTextId layout[] = {UI_MENU_PAD_LAYOUT_AUTO, UI_MENU_PAD_LAYOUT_NINTENDO, UI_MENU_PAD_LAYOUT_XBOX};
    opt_choice("MKW_PAD_LAYOUT", UI_MENU_PAD_LAYOUT, layout, 3, UI_MENU_PAD_LAYOUT_DESC, pad_layout_note());
    static const UiTextId icons[] = {UI_MENU_AUTO, UI_MENU_ICONS_NINTENDO, UI_MENU_ICONS_XBOX, UI_MENU_ICONS_PLAYSTATION,
                                     UI_MENU_ICONS_KEYBOARD};
    opt_choice("MKW_BUTTON_ICONS", UI_MENU_BUTTON_ICONS, icons, 5, UI_MENU_BUTTON_ICONS_DESC, button_icons_note());
    static const UiTextId shortcut[] = {UI_MENU_SHORTCUT_MINUS_PLUS, UI_MENU_SHORTCUT_GUIDE};
    opt_choice("MKW_MENU_SHORTCUT", UI_MENU_MENU_SHORTCUT, shortcut, 2, UI_MENU_MENU_SHORTCUT_DESC, nullptr,
               shortcut_guide_note());
    opt_toggle("MKW_MOTION", UI_MENU_MOTION, UI_MENU_MOTION_DESC);
    opt_slider("MKW_RUMBLE_STRENGTH", UI_MENU_RUMBLE_STRENGTH, 0, 300, UI_MENU_RUMBLE_STRENGTH_DESC, 10, "%.0f %%");
    opt_slider("MKW_RUMBLE_CURVE", UI_MENU_RUMBLE_CURVE, 0.2f, 1.5f, UI_MENU_RUMBLE_CURVE_DESC, 0.05f, "%.2f");
    opt_toggle("MKW_RUMBLE_TRIGGERS", UI_MENU_RUMBLE_TRIGGERS, UI_MENU_RUMBLE_TRIGGERS_DESC);
#ifdef _WIN32   // RenHDrum (input/renhdrum_host_win.c) has a Windows DLL only
    opt_toggle("MKW_RENHDRUM", UI_MENU_RENHDRUM, UI_MENU_RENHDRUM_DESC);
    libs_sync();
    // 3: only the download's notes (engine_note has nothing for it); the launcher cannot tell whether the game loaded it
    libs_note("renhdrum", "RenHDrum", strcmp(cfg_value("MKW_RENHDRUM"), "1") ? 0 : 3);
#endif
    opt_toggle("MKW_SWITCH2", UI_MENU_SWITCH2, UI_MENU_SWITCH2_DESC);   // input/switch2.c, at the next start
    opt_toggle("MKW_DSU", UI_MENU_DSU, UI_MENU_DSU_DESC);   // input/dsu.c, live
    opt_slider("MKW_STICK_DEADZONE", UI_MENU_STICK_DEADZONE, 0, 40, UI_MENU_STICK_DEADZONE_DESC, 1, "%.0f %%");   // input.c
    opt_toggle("MKW_KEYBOARD_MOUSE", UI_MENU_KEYBOARD_MOUSE, UI_MENU_KEYBOARD_MOUSE_DESC);   // input.c, live
    opt_slider("MKW_MOUSE_SENS", UI_MENU_MOUSE_SENS, 0, 10, UI_MENU_MOUSE_SENS_DESC, 0.1f, "%.1f");
    binds_rows(scale);
    opt_end();
    binds_after_page();
}

// The network LAN play uses (MKW_LAN_IP): Automatic, then each address of a local network with its adapter's name, so
// a player on ZeroTier or N2N picks theirs instead of writing the address in mkw.ini (#707). The list is read again
// every two seconds (listing the adapters is slow on Windows). A set address that is not in it any more (the VPN is
// down) stays shown as it is.
static void row_lan_network(void) {
    static OvLanIf ifs[16];
    static int n;
    static uint64_t read_at;
    if (!read_at || plat_now_ns() - read_at > 2000000000ull) n = ov_lan_list(ifs, 16), read_at = plat_now_ns();
    static char labels[16][80];
    const char *vals[17] = {""}, *names[17] = {menu_text(UI_MENU_AUTO)};
    for (int i = 0; i < n; i++) {
        snprintf(labels[i], sizeof labels[i], "%s (%s)", ifs[i].ip, ifs[i].name);
        vals[i + 1] = ifs[i].ip, names[i + 1] = labels[i];
    }
    opt_choice_list("MKW_LAN_IP", UI_MENU_LAN_NET, vals, names, n + 1, UI_MENU_LAN_NET_DESC);
}

// The languages, LAN play and the update check: the Game and Menu tabs of before, two rows each.
static void tab_general(float scale) {
    opt_begin("##general", scale);
    row_game_lang();
    // the order of cfg.c's MENU_LANG values, after Automatic
    static const UiTextId langs[] = {
        UI_MENU_MENU_LANG_AUTO, UI_MENU_LANGNAME_EN, UI_MENU_LANGNAME_JA, UI_MENU_LANGNAME_FR,
        UI_MENU_LANGNAME_DE, UI_MENU_LANGNAME_IT, UI_MENU_LANGNAME_ES, UI_MENU_LANGNAME_NL,
        UI_MENU_LANGNAME_PT, UI_MENU_LANGNAME_PT_BR, UI_MENU_LANGNAME_RU, UI_MENU_LANGNAME_ZH_HANS,
        UI_MENU_LANGNAME_ZH_HANT, UI_MENU_LANGNAME_KO, UI_MENU_LANGNAME_ES_419, UI_MENU_LANGNAME_CA,
        UI_MENU_LANGNAME_GL, UI_MENU_LANGNAME_EU, UI_MENU_LANGNAME_PL, UI_MENU_LANGNAME_TR, UI_MENU_LANGNAME_UK,
        UI_MENU_LANGNAME_TH,
    };
    opt_choice("MKW_MENU_LANG", UI_MENU_MENU_LANG, langs, sizeof langs / sizeof *langs, UI_MENU_MENU_LANG_DESC);
    opt_toggle("MKW_LAN", UI_MENU_LAN, UI_MENU_LAN_DESC);   // sdk/ov/ov_socket.c, the lan set of recomp/patches-mkw.txt
    row_lan_network();   // sdk/ov/ov_lanaddr.c
    opt_toggle("MKW_UPDATE_CHECK", UI_MENU_UPDATE_CHECK, UI_MENU_UPDATE_CHECK_DESC);   // ui/launcher/launcher_update.cpp
    static const UiTextId start[] = {UI_MENU_AUTO, UI_MENU_START_ALWAYS, UI_MENU_START_SKIP};
    opt_choice("MKW_START_SCREEN", UI_MENU_START_SCREEN, start, 3, UI_MENU_START_SCREEN_DESC);   // launcher.cpp console_start
    opt_toggle("MKW_DISCORD", UI_MENU_DISCORD, UI_MENU_DISCORD_DESC);   // core/presence/presence.c
    opt_end();
}

// ---- Advanced: the testing and troubleshooting keys, the bug report helpers and the reset of every setting
static bool g_modal;   // the reset confirmation is open: B and Esc close it, not the menu; L / R stay on the page

// A free-text key shown as Off (unset) / On (value set): On writes on_value, any set value reads as On.
static void row_set_or_not(const char *key, UiTextId label, UiTextId desc, const char *on_value) {
    const char *vals[] = {"", on_value}, *names[] = {menu_text(UI_MENU_OFF), menu_text(UI_MENU_ON)};
    opt_choice_list(key, label, vals, names, 2, desc, *cfg_value(key) ? 1 : 0);
}

static bool validation_layer_present(void) {
    static int have = -1;
    if (have < 0) {
        uint32_t n = 0;
        vkEnumerateInstanceLayerProperties(&n, nullptr);
        VkLayerProperties *l = (VkLayerProperties *)calloc(n ? n : 1, sizeof *l);
        if (l && vkEnumerateInstanceLayerProperties(&n, l) >= 0)
            for (uint32_t i = 0; i < n && have <= 0; i++) have = !strcmp(l[i].layerName, "VK_LAYER_KHRONOS_validation");
        free(l);
        if (have < 0) have = 0;
    }
    return have != 0;
}

// Every setting the menu shows back to its default (the environment's keys stay), written to mkw.ini. The Mods page's
// keys have rows too but are the player's choices, not settings: a reset left every mod disabled.
static void reset_all(void) {
    static const char *const keep[] = {"MODS", "MODS_ACK", "MODS_PLAYED"};
    const char *keys[128];
    int n = 0;
    for (int i = 0; n < 128 && cfg_meta_at(i); i++) {
        const char *k = cfg_meta_at(i)->key;
        bool kept = false;
        for (const char *x : keep) kept |= !strcmp(k, x);
        if (!kept) keys[n++] = k;
    }
    int done = cfg_reset(keys, n);
    opt_save_soon();
    rt_log("menu: reset %d settings to their defaults", done);
}

// A button's result shown in its value for a few seconds: 1 done (done_text), -1 failed.
struct ActionNote { uint64_t until; int result; };

static const char *action_value(const ActionNote &a, UiTextId done_text) {
    if (!a.result || plat_now_ns() > a.until) return "";
    return menu_text(a.result > 0 ? done_text : UI_MENU_ACTION_FAILED);
}

static void action_done(ActionNote &a, bool ok) { a = {plat_now_ns() + 3000000000ull, ok ? 1 : -1}; }

static void tab_advanced(float scale) {
    static ActionNote copy_note, log_note;
    opt_begin("##advanced", scale);
    opt_text(UI_MENU_ADV_WARNING, true);

    opt_group(UI_MENU_GROUP_SCHED);
    static const UiTextId preempt[] = {UI_MENU_PREEMPT_OFF, UI_MENU_PREEMPT_ON, UI_MENU_PREEMPT_INHERIT};   // 0|1|2
    opt_choice("MKW_ASYNC_PREEMPT", UI_MENU_ASYNC_PREEMPT, preempt, 3, UI_MENU_ASYNC_PREEMPT_DESC);
    opt_toggle("MKW_REPLAY_ASYNC", UI_MENU_REPLAY_ASYNC, UI_MENU_REPLAY_ASYNC_DESC);
    opt_toggle("MKW_FREE_CORES", UI_MENU_FREE_CORES, UI_MENU_FREE_CORES_DESC);
    opt_toggle("MKW_TIMER_1MS", UI_MENU_TIMER_1MS, UI_MENU_TIMER_1MS_DESC);

    opt_group(UI_MENU_GROUP_SHADERS);
    static const UiTextId wait[] = {UI_MENU_PIPE_WAIT_DEFAULT, UI_MENU_PIPE_WAIT_DRAW};
    opt_choice("MKW_PIPE_WAIT", UI_MENU_PIPE_WAIT, wait, 2, UI_MENU_PIPE_WAIT_DESC);
    static const UiTextId skip[] = {UI_MENU_PIPE_SKIP_STRICT, UI_MENU_PIPE_SKIP_ANY, UI_MENU_PIPE_SKIP_FULLSCREEN};
    opt_choice("MKW_PIPE_SKIP", UI_MENU_PIPE_SKIP, skip, 3, UI_MENU_PIPE_SKIP_DESC);
    opt_toggle("MKW_PIPE_PREBUILD", UI_MENU_PIPE_PREBUILD, UI_MENU_PIPE_PREBUILD_DESC);
    opt_toggle("MKW_GPL", UI_MENU_GPL, UI_MENU_GPL_DESC);

    opt_group(UI_MENU_GROUP_TEXMEM);
    static const UiTextId astc[] = {UI_MENU_ASTC_AUTO, UI_MENU_ASTC_GPU, UI_MENU_ASTC_NATIVE, UI_MENU_ASTC_CPU};
    opt_choice("MKW_ASTC", UI_MENU_ASTC, astc, 4, UI_MENU_ASTC_DESC);
    static const UiTextId mem[] = {UI_MENU_MEMORY_AUTO, UI_MENU_MEMORY_IMPORT, UI_MENU_MEMORY_MIRROR,
                                   UI_MENU_MEMORY_VERIFY};
    opt_choice("MKW_MEMORY", UI_MENU_MEMORY, mem, 4, UI_MENU_MEMORY_DESC);
    opt_toggle("MKW_TEX_ALIAS", UI_MENU_TEX_ALIAS, UI_MENU_TEX_ALIAS_DESC);

    opt_group(UI_MENU_GROUP_FIXES);
    opt_toggle("MKW_FIX_C0", UI_MENU_FIX_C0, UI_MENU_FIX_C0_DESC);
    opt_toggle("MKW_FIX_ZERO", UI_MENU_FIX_ZERO, UI_MENU_FIX_ZERO_DESC);
    opt_toggle("MKW_BIND_SKIP", UI_MENU_BIND_SKIP, UI_MENU_BIND_SKIP_DESC);
    opt_toggle("MKW_SHADER_MEMO", UI_MENU_SHADER_MEMO, UI_MENU_SHADER_MEMO_DESC);
    opt_toggle("MKW_PIPE_MEMO", UI_MENU_PIPE_MEMO, UI_MENU_PIPE_MEMO_DESC);
    opt_toggle("MKW_TEX_MEMO", UI_MENU_TEX_MEMO, UI_MENU_TEX_MEMO_DESC);
    opt_toggle("MKW_DESC_TEMPLATE", UI_MENU_DESC_TEMPLATE, UI_MENU_DESC_TEMPLATE_DESC);
    opt_toggle("MKW_PUSH_SKIP", UI_MENU_PUSH_SKIP, UI_MENU_PUSH_SKIP_DESC);
    opt_slider("MKW_SPLIT_DRAWS", UI_MENU_SPLIT_DRAWS, 0, 1000, UI_MENU_SPLIT_DRAWS_DESC, 8, "%.0f");

    opt_group(UI_MENU_GROUP_VULKAN);
    if (!validation_layer_present() && !cfg_bool("MKW_VK_VALIDATION")) opt_disable_next(UI_MENU_VK_VALIDATION_MISSING);
    opt_toggle("MKW_VK_VALIDATION", UI_MENU_VK_VALIDATION, UI_MENU_VK_VALIDATION_DESC);
    static const UiTextId dyn[] = {UI_MENU_AUTO, UI_MENU_OFF, UI_MENU_ON};   // unset, 0, 1
    opt_choice("MKW_VK_DYN_VERTEX", UI_MENU_VK_DYN_VERTEX, dyn, 3, UI_MENU_VK_DYN_VERTEX_DESC);
    opt_toggle("MKW_VK_SUBGROUP_PIN", UI_MENU_VK_SUBGROUP_PIN, UI_MENU_VK_SUBGROUP_PIN_DESC);

    opt_group(UI_MENU_GROUP_DIAG);
    row_set_or_not("MKW_TRACE", UI_MENU_TRACE, UI_MENU_TRACE_DESC, "perf");
    opt_toggle("MKW_FLIGHT", UI_MENU_FLIGHT, UI_MENU_FLIGHT_DESC);
    opt_slider("MKW_FLIGHT_SPIKE_MS", UI_MENU_FLIGHT_SPIKE_MS, 20, 1000, UI_MENU_FLIGHT_SPIKE_MS_DESC, 10, "%.0f");
    static char rec_path[PLAT_PATH_MAX * 3 + 32];   // the recording goes beside the log
    if (!rec_path[0]) {
        char dir[PLAT_PATH_MAX * 3];
        snprintf(rec_path, sizeof rec_path, "%s/input-record.txt", sysinfo_log_folder(dir, sizeof dir) ? dir : ".");
    }
    row_set_or_not("MKW_INPUT_RECORD", UI_MENU_INPUT_RECORD, UI_MENU_INPUT_RECORD_DESC, rec_path);
    if (opt_button("##open_log", UI_MENU_OPEN_LOG, UI_MENU_OPEN_LOG_DESC, action_value(log_note, UI_MENU_OPEN_LOG))) {
        char dir[PLAT_PATH_MAX * 3];
        bool ok = sysinfo_log_folder(dir, sizeof dir) && plat_open_folder(dir);
        if (!ok) action_done(log_note, false);   // success shows as the opened folder
    }
    if (opt_button("##copy_info", UI_MENU_COPY_INFO, UI_MENU_COPY_INFO_DESC,
                   action_value(copy_note, UI_MENU_COPY_INFO_DONE))) {
        static char text[16384];
        action_done(copy_note, sysinfo_text(text, sizeof text) > 0 && plat_clipboard_set(text));
    }
    bool ask = opt_button("##reset", UI_MENU_RESET, UI_MENU_RESET_DESC);
    opt_end();
    if (ask) ImGui::OpenPopup("##reset_confirm");   // at the level BeginPopupModal looks it up

    // the confirmation, over the menu; Cancel has the cursor first
    g_modal = false;
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("##reset_confirm", nullptr,
                               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize)) {
        g_modal = true;
        ImGui::TextUnformatted(menu_text(UI_MENU_RESET_CONFIRM));
        ImGui::Spacing();
        const char *yes = menu_text(UI_MENU_RESET_YES), *no = menu_text(UI_MENU_RESET_NO);
        const ImGuiStyle &st = ImGui::GetStyle();
        float bw = fmaxf(ImGui::CalcTextSize(yes).x, ImGui::CalcTextSize(no).x) + st.FramePadding.x * 4;
        bool close = false;
        if (ImGui::Button(no, ImVec2(bw, 0))) close = true;
        if (ImGui::IsWindowAppearing()) ImGui::SetItemDefaultFocus();
        ImGui::SameLine();
        if (ImGui::Button(yes, ImVec2(bw, 0))) {
            reset_all();
            close = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))
            close = true;
        if (close) {
            ImGui::CloseCurrentPopup();
            opt_focus_key("##reset");   // back on the row it came from
        }
        ImGui::EndPopup();
    }
}

// Shortcuts the console does not have (core/hacks/): all off by default, marked experimental.
static void tab_hacks(float scale) {
    opt_begin("##hacks", scale);
    opt_text(UI_MENU_HACKS_WARNING, true);
    // the first four values; "until" is the run scripts' (core/hacks/turbo.c)
    static const UiTextId turbo[] = {UI_MENU_OFF, UI_MENU_TURBO_HOLD, UI_MENU_TURBO_TOGGLE, UI_MENU_ON};
    opt_choice("MKW_TURBO", UI_MENU_TURBO, turbo, 4, UI_MENU_TURBO_DESC);
    opt_slider("MKW_TURBO_SPEED", UI_MENU_TURBO_SPEED, 2, 8, UI_MENU_TURBO_SPEED_DESC, 1, "x%.0f");
    opt_toggle("MKW_SKIP_MOVIES", UI_MENU_SKIP_MOVIES, UI_MENU_SKIP_MOVIES_DESC);
    row_set_or_not("MKW_SEED", UI_MENU_SEED, UI_MENU_SEED_DESC, "1");
    opt_toggle("MKW_UNLOCK_ALL", UI_MENU_UNLOCK_ALL, UI_MENU_UNLOCK_ALL_DESC);
    static const UiTextId quick[] = {UI_MENU_OFF, UI_MENU_QUICK_RACE_TT, UI_MENU_QUICK_RACE_GP, UI_MENU_QUICK_RACE_FREE};
    opt_choice("MKW_QUICK_RACE", UI_MENU_QUICK_RACE, quick, 4, UI_MENU_QUICK_RACE_DESC);
    opt_toggle("MKW_FREEZE_KEYS", UI_MENU_FREEZE_KEYS, UI_MENU_FREEZE_KEYS_DESC);
    opt_toggle("MKW_CLASSIC_GP", UI_MENU_CLASSIC_GP, UI_MENU_CLASSIC_GP_DESC);
    opt_toggle("MKW_CLASSIC_VS", UI_MENU_CLASSIC_VS, UI_MENU_CLASSIC_VS_DESC);
    opt_end();
}

static const UiTextId kTabNames[TAB_COUNT] = {UI_MENU_TAB_SCREEN,   UI_MENU_TAB_GRAPHICS, UI_MENU_TAB_SOUND,
                                              UI_MENU_TAB_CONTROLS, UI_MENU_TAB_GENERAL,  UI_MENU_TAB_ADVANCED,
                                              UI_MENU_TAB_HACKS};
static int g_tab;               // the tab shown
static int g_tab_request = -1;  // select this tab at the next layout; -1 none
static int g_focus_page = -1;   // this tab's first row takes the cursor once it is shown; -1 none

void settings_select(int tab) { g_tab_request = tab; }

void settings_opened(void) {
    g_focus_page = g_tab_request >= 0 ? g_tab_request : g_tab;   // the first row has the cursor
    g_modal = false;
}

// The in-game menu opened (discussion #577): no row takes the cursor, layout_menu focuses "Continue" instead, so a
// stray D-pad deflection on open cannot retune a setting. A pending tab request survives: tab switches in
// settings_tabs still focus the new page's first row, and the test hook's settings_select(0) keeps its screenshots
// on the first row.
void settings_opened_menu(void) {
    g_focus_page = -1;
    g_modal = false;
}

bool settings_modal(void) { return g_modal || binds_modal(); }

// The tabs in their order: the players' (Screen to General), the mods' (MkwPluginApi.settings_tab: ids from TAB_MOD,
// in the game only), then Advanced and Hacks. Returns how many.
enum { TAB_MOD = 100 };
static int tab_order(int *out) {
    int n = 0;
    for (int k = TAB_SCREEN; k <= TAB_GENERAL; k++) out[n++] = k;
    for (int i = 0; i < plugins_tab_count(); i++) out[n++] = TAB_MOD + i;
    out[n++] = TAB_ADVANCED;
    out[n++] = TAB_HACKS;
    return n;
}

void settings_tabs(float scale, float bottom) {
    int order[TAB_COUNT + 16], ntabs = tab_order(order);
    int step = settings_modal()                                                                          ? 0
             : ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) || ImGui::IsKeyPressed(ImGuiKey_E, false) ? 1
             : ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) || ImGui::IsKeyPressed(ImGuiKey_Q, false) ? -1
                                                                                                         : 0;
    if (step) {
        int at = 0;
        for (int i = 0; i < ntabs; i++)
            if (order[i] == g_tab) at = i;
        g_tab_request = order[(at + step + ntabs) % ntabs];
    }
    if (g_tab_request >= 0) g_focus_page = g_tab_request;   // the new page's first row takes the cursor

    if (ImGui::BeginTabBar("##tabs", ImGuiTabBarFlags_NoTabListScrollingButtons)) {
        for (int at = 0; at < ntabs; at++) {
            int k = order[at];
            char label[128];
            snprintf(label, sizeof label, "  %s  ###tab%d", k >= TAB_MOD ? plugins_tab_title(k - TAB_MOD)
                                                                          : menu_text(kTabNames[k]),
                     k);   // the id stays per language
            // the testing and shortcut tabs in the dim colour: the ones most players need come first and stand out
            bool quiet = k >= TAB_ADVANCED && k < TAB_MOD && k != g_tab;
            if (quiet) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            bool open = ImGui::BeginTabItem(label, nullptr, g_tab_request == k ? ImGuiTabItemFlags_SetSelected : 0);
            if (quiet) ImGui::PopStyleColor();
            if (!open) continue;
            if (k >= TAB_MOD && g_tab != k) plugins_tab_shown(k - TAB_MOD);   // its page's first frame again
            g_tab = k;
            if (g_focus_page == k) {
                opt_focus_first();
                g_focus_page = -1;
            }
            ImGui::Spacing();
            // the page's rows are a NavFlattened child of this window itself: ImGui clips the items of a direct child
            // only, so behind a second child level the rows scrolled out above the tabs won D-pad moves from the
            // buttons and the tabs (up from "Continue" went to the tabs, down from the tabs to "Continue")
            opt_reserve_bottom(bottom);
            switch (k) {
            case TAB_SCREEN: tab_screen(scale); break;
            case TAB_GRAPHICS: tab_graphics(scale); break;
            case TAB_SOUND: tab_sound(scale); break;
            case TAB_CONTROLS: tab_controls(scale); break;
            case TAB_GENERAL: tab_general(scale); break;
            case TAB_ADVANCED: tab_advanced(scale); break;
            case TAB_HACKS: tab_hacks(scale); break;
            default: plugins_tab_draw(k - TAB_MOD, scale); break;   // a mod's (MkwPage)
            }
            ImGui::EndTabItem();
        }
        g_tab_request = -1;
        ImGui::EndTabBar();
    }
}

void settings_tabs_done(void) {
    if (g_tab != TAB_ADVANCED) g_modal = false;
}
