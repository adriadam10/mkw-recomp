// Native plugins (mkw_plugin.h): shared libraries in mods/<mod>/plugin/ of the enabled mods, loaded at start before the
// game code, each handed the API table. The runtime knows nothing of what a plugin does: it gives generic hooks (an
// override by symbol or by guest address, the game-loaded callback, calls into guest code) and the plugin decides.
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "cfg.h"
#include "dispatch.h"
#include "host.h"
#include "mkw_plugin.h"
#include "plugin.h"
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
    const char *message = NULL;
    in_init = 1;
    int r = init(&q->api, MKW_PLUGIN_API_VERSION, &message);
    in_init = 0;
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
