// Native plugins (mkw_plugin.h): shared libraries in mods/<mod>/plugin/ of the enabled mods, loaded at start before the
// game code, each handed the API table. The runtime knows nothing of what a plugin does: it gives generic hooks (an
// override by symbol or by guest address, the game-loaded callback, calls into guest code) and the plugin decides.
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "cfg.h"
#include "dispatch.h"
#include "gfx.h"
#include "host.h"
#include "mkw_plugin.h"
#include "mod_page_ui.h"
#include "mod_settings.h"
#include "plugin.h"
#include "players.h"
#include "ui_text.h"
#include "version.h"
#include "platform/platform.h"
#include "ui/menu/menu.h"

_Static_assert(offsetof(MkwCpu, x) == offsetof(Cpu, x), "MkwCpu.x");
_Static_assert(offsetof(MkwCpu, sp) == offsetof(Cpu, sp), "MkwCpu.sp");
_Static_assert(offsetof(MkwCpu, pc) == offsetof(Cpu, pc), "MkwCpu.pc");

#ifdef _WIN32
#define PLUGIN_EXT ".dll"
#else
#define PLUGIN_EXT ".so"
#endif

enum { MAX_PLUGINS = 32, MAX_LOADED_CB = 64 };

typedef struct {
    MkwPluginApi api;   // first: the table the plugin keeps
    char mod_dir[PLAT_PATH_MAX * 2], mod_name[64], file[256];
} Plugin;

static Plugin plugins[MAX_PLUGINS];
static int nplugins, in_init;
static struct { void (*cb)(void *); void *user; } loaded_cb[MAX_LOADED_CB];
static int nloaded_cb, game_loaded;
static struct { void (*cb)(void *); void *user; } exit_cb[MAX_LOADED_CB];
static int nexit_cb;

static void api_log(const char *fmt, ...) {
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    rt_log("plugin: %s", line);
}

static int api_override_symbol(const char *name, MkwGuestFn fn) {
    if (!in_init || !name || !*name || !fn) {
        rt_log("plugin: override_symbol(%s) refused: only from mkw_plugin_init, with a name and a function", name ? name : "NULL");
        return 0;
    }
    char *copy = strdup(name);
    if (!copy) rt_fatal("plugin: out of memory");
    hostfn_add_override(copy, (GuestFn)fn);
    return 1;
}

static MkwGuestFn api_stock(const char *name) { return name ? (MkwGuestFn)hostfn_stock(name) : NULL; }

static void api_on_game_loaded(void (*cb)(void *user), void *user) {
    if (!in_init || !cb || nloaded_cb == MAX_LOADED_CB) {
        rt_log("plugin: on_game_loaded refused: only from mkw_plugin_init, at most %d callbacks", MAX_LOADED_CB);
        return;
    }
    loaded_cb[nloaded_cb].cb = cb;
    loaded_cb[nloaded_cb++].user = user;
}

static int api_on_exit(void (*cb)(void *user), void *user) {
    if (!in_init || !cb || nexit_cb == MAX_LOADED_CB) {
        rt_log("plugin: on_exit refused: only from mkw_plugin_init, at most %d callbacks", MAX_LOADED_CB);
        return 0;
    }
    exit_cb[nexit_cb].cb = cb;
    exit_cb[nexit_cb++].user = user;
    return 1;
}

static char profile_name[PLAYER_NAME_BYTES];   // the player playing (player_name of rt_config()->profile)

// ---- the mods' tabs in the in-game menu's settings (settings_tabs.cpp draws them through plugins_tab_*)
enum { MAX_TABS = 8 };
static struct {
    char title[64];
    void (*draw)(const MkwPage *page);
    ModPageUi ui;
} tabs[MAX_TABS];
static int ntabs;
static int cur_plugin = -1;   // the plugin whose mkw_plugin_init runs (load_one)

static int api_settings_tab(const char *title, void (*draw)(const struct MkwPage *page)) {
    if (!in_init || !draw || !title || !*title || ntabs == MAX_TABS || cur_plugin < 0) {
        rt_log("plugin: settings_tab refused: only from mkw_plugin_init, with a title, at most %d tabs", MAX_TABS);
        return 0;
    }
    snprintf(tabs[ntabs].title, sizeof tabs[ntabs].title, "%s", title);
    tabs[ntabs].draw = draw;
    ModPageUi *ui = &tabs[ntabs].ui;
    memset(ui, 0, sizeof *ui);
    snprintf(ui->mod, sizeof ui->mod, "%s", plugins[cur_plugin].mod_name);
    snprintf(ui->mod_dir, sizeof ui->mod_dir, "%s", plugins[cur_plugin].mod_dir);
    snprintf(ui->player_dir, sizeof ui->player_dir, "%s", rt_config()->profile);
    ui->in_game = 1;
    ntabs++;
    rt_log("plugin: %s: settings tab \"%s\"", plugins[cur_plugin].mod_name, title);
    return 1;
}

int plugins_tab_count(void) { return ntabs; }
const char *plugins_tab_title(int i) { return i >= 0 && i < ntabs ? tabs[i].title : ""; }
void plugins_tab_draw(int i, float scale) {
    if (i < 0 || i >= ntabs) return;
    snprintf(tabs[i].ui.player_name, sizeof tabs[i].ui.player_name, "%s", profile_name);
    mod_page_ui_draw(&tabs[i].ui, tabs[i].draw, scale);
}
void plugins_tab_shown(int i) {
    if (i >= 0 && i < ntabs) tabs[i].ui.reopen = 1;   // MkwPage.opened in its first frame each time it is shown
}

static int api_override_address(uint64_t addr, MkwGuestFn fn) {
    if ((!in_init && !game_loaded) || !rt_dispatch_override(addr, (GuestFn)fn)) {
        rt_log("plugin: override_address(%#llx) refused: only from mkw_plugin_init or on_game_loaded, at a function "
               "start", (unsigned long long)addr);
        return 0;
    }
    return 1;
}

static int api_guest_modules(void) { return ldr_count(); }

static uint64_t api_guest_symbol(const char *name) { return ldr_count() && name ? ldr_symbol(name) : 0; }

static uint64_t api_module_base(const char *name) {
    for (int i = 0; name && i < ldr_count(); i++)
        if (!strcmp(ldr_module(i)->name, name)) return ldr_module(i)->base;
    return 0;
}

static uint64_t api_call(MkwCpu *c, uint64_t fn, int n, ...) {
    uint64_t a[8] = {0};
    if (n < 0 || n > 8) rt_fatal("plugin: call with %d arguments (at most 8)", n);
    va_list ap;
    va_start(ap, n);
    for (int i = 0; i < n; i++) a[i] = va_arg(ap, uint64_t);
    va_end(ap);
    return rt_call((Cpu *)c, fn, n, a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
}

// A symbol override sits at its host slot (pc in the host-function window): the loader kept the game's definition as
// the slot's original. An address override sits at the game's address: dispatch.c kept the translated function.
static uint64_t api_call_original(MkwCpu *m) {
    Cpu *c = (Cpu *)m;
    if (c->pc >= RT_HOSTFN_BASE && c->pc < RT_RET_HOST) return rt_original(c);
    GuestFn f = rt_dispatch_original(c->pc);
    if (!f) rt_fatal("plugin: call_original at %#llx: not inside an override", (unsigned long long)c->pc);
    uint64_t pc = c->pc;
    f(c);   // translated code returns to its C caller at the guest's RET
    c->pc = pc;
    return c->x[0];
}

static MkwCpu *api_current_cpu(void) { return (MkwCpu *)kern_cpu(); }

static int api_overlay_set(MkwOverlayFn draw, void *user) {
    if (!in_init || !plugin_overlay_add(draw, user)) {
        rt_log("plugin: overlay_set refused: only from mkw_plugin_init, with a function, at most 16");
        return 0;
    }
    return 1;
}

// ---- version 2: the mod's settings, the player's. The table has no context argument, so each plugin slot gets its own
// pair of entries (one table per plugin: plugins[i].api), generated here.
static int setting_get_of(int i, const char *key, char *out, size_t cap) {
    return mod_settings_get(rt_config()->profile, plugins[i].mod_name, key, out, cap);
}
static int setting_set_of(int i, const char *key, const char *value) {
    return mod_settings_set(rt_config()->profile, plugins[i].mod_name, key, value);
}
#define SLOT(i)                                                                                                        \
    static int sget##i(const char *k, char *o, size_t c) { return setting_get_of(i, k, o, c); }                        \
    static int sset##i(const char *k, const char *v) { return setting_set_of(i, k, v); }
SLOT(0) SLOT(1) SLOT(2) SLOT(3) SLOT(4) SLOT(5) SLOT(6) SLOT(7) SLOT(8) SLOT(9) SLOT(10) SLOT(11) SLOT(12) SLOT(13)
SLOT(14) SLOT(15) SLOT(16) SLOT(17) SLOT(18) SLOT(19) SLOT(20) SLOT(21) SLOT(22) SLOT(23) SLOT(24) SLOT(25) SLOT(26)
SLOT(27) SLOT(28) SLOT(29) SLOT(30) SLOT(31)
#undef SLOT
#define S(i) sget##i
static int (*const setting_get_slot[MAX_PLUGINS])(const char *, char *, size_t) = {
    S(0), S(1), S(2), S(3), S(4), S(5), S(6), S(7), S(8), S(9), S(10), S(11), S(12), S(13), S(14), S(15),
    S(16), S(17), S(18), S(19), S(20), S(21), S(22), S(23), S(24), S(25), S(26), S(27), S(28), S(29), S(30), S(31)};
#undef S
#define S(i) sset##i
static int (*const setting_set_slot[MAX_PLUGINS])(const char *, const char *) = {
    S(0), S(1), S(2), S(3), S(4), S(5), S(6), S(7), S(8), S(9), S(10), S(11), S(12), S(13), S(14), S(15),
    S(16), S(17), S(18), S(19), S(20), S(21), S(22), S(23), S(24), S(25), S(26), S(27), S(28), S(29), S(30), S(31)};
#undef S
_Static_assert(MAX_PLUGINS == 32, "one setting_get / setting_set pair per plugin slot");


// ---- version 4: the pad before the game reads it, the menu, the shown picture
static struct { MkwPadFn fn; void *user; } filters[MAX_PLUGINS];
static int nfilters;

static int api_input_filter(MkwPadFn fn, void *user) {
    if (!in_init || !fn || nfilters == MAX_PLUGINS) {
        rt_log("plugin: input_filter refused: only from mkw_plugin_init, with a function");
        return 0;
    }
    filters[nfilters].fn = fn;
    filters[nfilters++].user = user;
    return 1;
}

// input.c, under its lock: the filters see the 16 buttons (the stick directions are made after them).
void plugins_input_filter(int player, uint64_t *buttons, int32_t sticks[4], int *guide) {
    if (!nfilters) return;
    MkwPad p = {.size = sizeof p, .player = player, .buttons = *buttons & 0xffff, .lx = sticks[0], .ly = sticks[1],
                .rx = sticks[2], .ry = sticks[3], .guide = guide ? *guide : 0};
    for (int i = 0; i < nfilters; i++) filters[i].fn(&p, filters[i].user);
    *buttons = (*buttons & ~0xffffull) | (p.buttons & 0xffff);
    sticks[0] = p.lx, sticks[1] = p.ly, sticks[2] = p.rx, sticks[3] = p.ry;
    if (guide) *guide = p.guide != 0;
}

static void api_menu_open(int open) {
    if ((menu_is_open() != 0) != (open != 0)) menu_toggle();
}

static struct { MkwFrameFn cb; void *user; int max_w, max_h; } frame_req;
static volatile int32_t frame_state;   // 0 none, 1 being asked, 2 asked, 3 being answered

static int api_frame_capture(int max_w, int max_h, MkwFrameFn cb, void *user) {
    if (!cb || max_w < 16 || max_h < 16 || max_w > 4096 || max_h > 4096 ||
        (plat_cas32(&frame_state, 1, 0) != 0 && plat_cas32(&frame_state, 1, 2) != 2)) {
        rt_log("plugin: frame_capture refused: a function, a size of 16..4096, not while one is being answered");
        return 0;
    }
    frame_req.cb = cb, frame_req.user = user, frame_req.max_w = max_w, frame_req.max_h = max_h;
    plat_xchg32(&frame_state, 2);
    return 1;
}

int plugins_frame_wanted(void) { return frame_state == 2; }

// gfx_present.c, after the present that copied its image: box-filtered down to fit the request, never up.
void plugins_frame_captured(const uint8_t *rgba, int w, int h) {
    if (w <= 0 || h <= 0 || plat_cas32(&frame_state, 3, 2) != 2) return;
    int ow = w, oh = h;
    if ((int64_t)ow * frame_req.max_h > (int64_t)oh * frame_req.max_w) {   // the width binds
        if (ow > frame_req.max_w) oh = (int)((int64_t)h * frame_req.max_w / w), ow = frame_req.max_w;
    } else if (oh > frame_req.max_h) {
        ow = (int)((int64_t)w * frame_req.max_h / h), oh = frame_req.max_h;
    }
    if (ow < 1) ow = 1;
    if (oh < 1) oh = 1;
    uint8_t *out = ow == w && oh == h ? NULL : malloc(4ull * (size_t)ow * (size_t)oh);
    if (out)
        for (int y = 0; y < oh; y++)
            for (int x = 0; x < ow; x++) {
                int x0 = (int)((int64_t)x * w / ow), x1 = (int)((int64_t)(x + 1) * w / ow);
                int y0 = (int)((int64_t)y * h / oh), y1 = (int)((int64_t)(y + 1) * h / oh);
                if (x1 <= x0) x1 = x0 + 1;
                if (y1 <= y0) y1 = y0 + 1;
                uint32_t sum[3] = {0}, k = 0;
                for (int sy = y0; sy < y1; sy++)
                    for (int sx = x0; sx < x1; sx++, k++)
                        for (int c = 0; c < 3; c++) sum[c] += rgba[4ull * ((size_t)sy * (size_t)w + (size_t)sx) + (size_t)c];
                uint8_t *o = out + 4ull * ((size_t)y * (size_t)ow + (size_t)x);
                o[0] = (uint8_t)(sum[0] / k), o[1] = (uint8_t)(sum[1] / k), o[2] = (uint8_t)(sum[2] / k), o[3] = 255;
            }
    if (out) frame_req.cb(out, ow, oh, ow * 4, frame_req.user);
    else if (ow == w && oh == h) frame_req.cb(rgba, w, h, w * 4, frame_req.user);
    free(out);
    plat_xchg32(&frame_state, 0);
}

// ---- version 4, continued: the plugin's overlay covers the game (focus, with the console's suspension), the game's end
static int api_focus_set(int in_focus, int suspend) { return gfx_window_focus_hold(!in_focus, suspend); }

static void api_quit(void) {
    rt_log("plugin: the game's end asked (MkwPluginApi.quit)");
    gfx_window_request_close();
}

// gfx/nvn/procs/nvn2.c: images a plugin fills (version 3)
uint32_t nvn2_native_image_create(int w, int h, void **pixels, int *row_bytes);
int nvn2_native_image_touch(uint32_t handle);

static const MkwPluginApi api_template = {
    .size = sizeof(MkwPluginApi),
    .version = MKW_PLUGIN_API_VERSION,
    .log = api_log,
    .cfg_get = cfg_get,
    .guest_modules = api_guest_modules,
    .guest_symbol = api_guest_symbol,
    .module_base = api_module_base,
    .override_symbol = api_override_symbol,
    .stock = api_stock,
    .on_game_loaded = api_on_game_loaded,
    .override_address = api_override_address,
    .call = api_call,
    .call_original = api_call_original,
    .current_cpu = api_current_cpu,
    .core_away = kern_core_away,
    .core_back = kern_core_back,
    .notice = menu_notice,
    .overlay_set = api_overlay_set,
    .overlay_show = plugin_overlay_show,
    .on_exit = api_on_exit,
    .settings_tab = api_settings_tab,
    .native_image_create = nvn2_native_image_create,
    .native_image_touch = nvn2_native_image_touch,
    .input_filter = api_input_filter,
    .menu_open = api_menu_open,
    .frame_capture = api_frame_capture,
    .focus_set = api_focus_set,
    .quit = api_quit,
};

// A plugin that declines or fails to load is told to the player too: the mod they enabled does nothing this run.
static void plugin_problem(const Plugin *p, const char *what) {
    rt_log("plugin: %s/plugin/%s: %s", p->mod_name, p->file, what);
    char msg[512];
    snprintf(msg, sizeof msg, "The mod \"%s\" did not start (%s): %s.\nThe game goes on without it.", p->mod_name,
             p->file, what);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, "mkw-recomp", msg, NULL);
}

static int load_one(const char *name, void *ctx) {
    Plugin *p = ctx;
    if (nplugins == MAX_PLUGINS) {
        rt_log("plugin: more than %d plugins: %s/plugin/%s not loaded", MAX_PLUGINS, p->mod_name, name);
        return 0;
    }
    Plugin *q = &plugins[nplugins];
    *q = *p;
    snprintf(q->file, sizeof q->file, "%s", name);
    char path[sizeof q->mod_dir + 300];
    snprintf(path, sizeof path, "%s/plugin/%s", q->mod_dir, name);
    void *lib = plat_lib_open(path);
    MkwPluginInit init = lib ? (MkwPluginInit)plat_lib_sym(lib, "mkw_plugin_init") : NULL;
    if (!init) {
        plugin_problem(q, lib ? "it has no mkw_plugin_init" : "the library does not load");
        return 1;
    }
    q->api = api_template;
    q->api.port_version = rt_port_version;
    q->api.game_version = rt_version()->display;
    q->api.game_id = RT_GAME_ID;
    q->api.mod_dir = q->mod_dir;
    q->api.mod_name = q->mod_name;
    q->api.language = menu_lang();
    if (!profile_name[0]) player_name(rt_config()->profile, profile_name);
    q->api.profile_name = profile_name;
    q->api.setting_get = setting_get_slot[nplugins];
    q->api.setting_set = setting_set_slot[nplugins];
    const char *message = NULL;
    in_init = 1;
    cur_plugin = nplugins;
    int r = init(&q->api, MKW_PLUGIN_API_VERSION, &message);
    in_init = 0;
    cur_plugin = -1;
    if (r) {
        char why[300];
        snprintf(why, sizeof why, "%s (code %d)", message && *message ? message : "it declined to start", r);
        plugin_problem(q, why);
        return 1;   // its overrides stay: a plugin declines before it installs any
    }
    rt_log("plugin: %s/plugin/%s loaded (API %d)", q->mod_name, name, MKW_PLUGIN_API_VERSION);
    nplugins++;
    return 1;
}

void plugins_load(void) {
    const char *list = cfg_get("MKW_MODS");
    if (!list || !*list) return;
    // by hand, not strtok: its state is the C library's, which a plugin's init may use too
    for (const char *s = list; *s;) {
        const char *end = strchr(s, ','), *next = end ? end + 1 : s + strlen(s);
        if (!end) end = next;
        while (s < end && (*s == ' ' || *s == '\t')) s++;
        while (end > s && (end[-1] == ' ' || end[-1] == '\t')) end--;
        char t[64];
        size_t n = (size_t)(end - s);
        int fits = n && n < sizeof t;
        if (fits) memcpy(t, s, n), t[n] = 0;
        s = next;
        if (!fits) continue;
        int seen = 0;   // a mod listed twice (MKW_MODS set by hand) loads once
        for (int i = 0; i < nplugins; i++) seen |= !strcmp(plugins[i].mod_name, t);
        if (seen) {
            rt_log("plugin: %s is listed twice in MKW_MODS: loaded once", t);
            continue;
        }
        Plugin p = {0};
        snprintf(p.mod_name, sizeof p.mod_name, "%s", t);
        snprintf(p.mod_dir, sizeof p.mod_dir, "%s/mods/%s", cfg_game_dir(), t);
        char dir[sizeof p.mod_dir + 16];
        snprintf(dir, sizeof dir, "%s/plugin", p.mod_dir);
        if (plat_dir_exists(dir)) plat_file_list(dir, PLUGIN_EXT, load_one, &p);
    }
}

void plugins_game_loaded(void) {
    game_loaded = 1;
    for (int i = 0; i < nloaded_cb; i++) loaded_cb[i].cb(loaded_cb[i].user);
}

void plugins_started(void) { game_loaded = 0; }

void plugins_exit(void) {
    static volatile int32_t done;
    if (plat_xchg32(&done, 1)) return;   // the window closing and the game's exit may both get here
    if (nexit_cb) rt_log("plugin: game ending: %d exit callback(s)", nexit_cb);
    for (int i = 0; i < nexit_cb; i++) exit_cb[i].cb(exit_cb[i].user);
}
