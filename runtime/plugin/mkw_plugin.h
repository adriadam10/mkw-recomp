// mkw-recomp native plugin API, version 1. The one header a plugin includes; it links against nothing of mkw-recomp.
//
// A plugin is a shared library (.so on Linux, .dll on Windows) in mods/<mod>/plugin/ of the game folder. The runtime
// loads it at start when the player has the mod enabled (the Mods page, or `--mod <name>` / MKW_MODS), before the game
// code is loaded, and calls its exported mkw_plugin_init. Everything the plugin may use of the runtime is in the table
// it gets there; the table only grows, so a plugin built against version N keeps loading on later runtimes.
//
// Threads: guest code runs on guest threads. A function a plugin installs (override_symbol, override_address) runs on
// the guest thread that called it, with that thread's MkwCpu; call(), call_original() and current_cpu() are for such a
// thread only. A plugin thread of its own does not call guest code: it hands the work to an override that the game
// calls often, as the runtime's own services do. Guest memory (guest_ptr) can be read and written from any thread.
// A plugin that blocks inside an override (sockets, sleeps) wraps the wait in core_away / core_back so the guest's
// other threads keep the virtual core meanwhile.
//
// The API is generic: it names no game service. What a plugin replaces is its own business.
#ifndef MKW_PLUGIN_H
#define MKW_PLUGIN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MKW_PLUGIN_API_VERSION 1

// The start of the runtime's guest CPU state; ABI-stable for every API version (the runtime checks it at build time).
// Arguments in x[0..7], the result in x[0], the return address in x[30].
typedef struct MkwCpu {
    uint64_t x[32];   // x0-x30; x[31] unused (sp below)
    uint64_t sp;
    uint64_t pc;      // the guest address being called (an override_address target, or a symbol's host slot)
} MkwCpu;

typedef void (*MkwGuestFn)(MkwCpu *c);

// Guest addresses are host addresses: the game's memory is mapped 1:1 into the process. A guarantee of the ABI, so
// reading a guest structure costs nothing.
static inline void *mkw_guest_ptr(uint64_t guest) { return (void *)(uintptr_t)guest; }

// What a plugin's overlay drawing gets each frame (overlay_set). Pixels of the window's picture, origin top left.
// Colours are 0xRRGGBBAA. The text is the runtime menu's font, UTF-8.
typedef struct MkwOverlay {
    uint32_t size;          // sizeof(MkwOverlay) of the runtime: a field is there when its offset is below size
    float width, height;    // the picture
    float scale;            // the menu's UI scale: 1 at 1080 lines
    float line_height;      // of the text
    void (*rect)(float x0, float y0, float x1, float y1, uint32_t rgba, float rounding);   // filled
    void (*text)(float x, float y, uint32_t rgba, const char *utf8);
    void (*text_size)(const char *utf8, float *w, float *h);
} MkwOverlay;

typedef void (*MkwOverlayFn)(const MkwOverlay *o, void *user);

typedef struct MkwPluginApi {
    uint32_t size;      // sizeof(MkwPluginApi) of the runtime: a field is there when its offset is below size
    uint32_t version;   // MKW_PLUGIN_API_VERSION of the runtime

    // ---- who and where
    const char *port_version;   // mkw-recomp's version, "0.1.14"
    const char *game_version;   // the game's display version, "1.0.0" or "1.8.0": pick your address table by it
    const char *game_id;        // the game code's build id (its mkwgame.id): a new build may move addresses
    const char *mod_dir;        // this plugin's mod folder (mods/<mod>), for its settings, logs and libraries
    const char *mod_name;

    // ---- any time
    void (*log)(const char *fmt, ...);            // a line in the runtime's log (logs/mkw.log), prefixed with the mod
    const char *(*cfg_get)(const char *name);     // an MKW_* setting (environment, then mkw.ini); NULL if unset
    int (*guest_modules)(void);                   // guest modules loaded so far: non-zero once overrides by name are late
    uint64_t (*guest_symbol)(const char *name);   // a guest module's exported definition, 0 if none
    uint64_t (*module_base)(const char *name);    // "main", "subsdk0", "sdk": load address, 0 before the game loads

    // ---- only from mkw_plugin_init (before the game code loads); 1 done, 0 refused (logged)
    // Every reference of the game to name (an nn:: SDK function or any other import or export) runs fn instead. No
    // limit on how many. If two plugins override one name, the one loaded later wins.
    int (*override_symbol)(const char *name, MkwGuestFn fn);
    // The runtime's own replacement of name, NULL if it has none: a plugin chains to it by calling it with the same
    // MkwCpu (logging around the runtime's file system, say).
    MkwGuestFn (*stock)(const char *name);
    // cb(user) runs once the game code is loaded and before any guest code runs: module_base and guest_symbol answer
    // there, and override_address is still allowed.
    void (*on_game_loaded)(void (*cb)(void *user), void *user);

    // ---- from mkw_plugin_init or on_game_loaded's callback; 1 done, 0 refused (logged)
    // Calls that reach guest address addr through a register (virtual functions, function pointers, the PLT) run fn
    // instead, with c->pc == addr and the caller in c->x[30]. A direct call from translated code to addr is compiled
    // as a direct call and does not see it. addr must be the start of a function of the game.
    int (*override_address)(uint64_t addr, MkwGuestFn fn);

    // ---- on a guest thread (inside an override)
    // Calls guest code at fn with n (at most 8) integer arguments; returns x0. c is the MkwCpu the override got.
    uint64_t (*call)(MkwCpu *c, uint64_t fn, int n, ...);
    // Inside an override: what the call would have run without it (the game's own definition of the symbol, or the
    // function at the overridden address). Arguments as they are in c; returns x0.
    uint64_t (*call_original)(MkwCpu *c);
    MkwCpu *(*current_cpu)(void);    // the calling guest thread's MkwCpu, NULL on other threads
    int (*core_away)(void);          // before a blocking host wait: gives the virtual core up ...
    void (*core_back)(int *away);    // ... and after it takes it back (away: core_away's result)

    // ---- the screen, any thread
    // A line at the top middle of the picture for that many seconds, menu open or not; a newer notice (the plugin's or
    // the runtime's) replaces it.
    void (*notice)(const char *utf8, int seconds);
    // A drawing of the plugin's over the game picture, under the runtime's menu and windows: draw runs on the present
    // thread once per shown frame while overlay_show(draw, 1) is on. It runs no guest code and does not wait (it copies
    // what it shows under a lock of its own). overlay_set only from mkw_plugin_init; 1 done, 0 refused.
    int (*overlay_set)(MkwOverlayFn draw, void *user);
    void (*overlay_show)(MkwOverlayFn draw, int on);
} MkwPluginApi;

// Exported by the plugin. api_version is the runtime's (api->version); size tells which fields exist. Return 0 to stay
// loaded; anything else declines (an unsupported game build, say): the runtime logs it with message, if set, and
// shows it to the player.
#if defined(_WIN32)
#define MKW_PLUGIN_EXPORT __declspec(dllexport)
#else
#define MKW_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif
typedef int (*MkwPluginInit)(const MkwPluginApi *api, uint32_t api_version, const char **message);

#ifdef __cplusplus
}
#endif

#endif
