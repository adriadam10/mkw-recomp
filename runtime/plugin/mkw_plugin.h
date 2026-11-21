// mkw-recomp native plugin API, version 4 (version 1; a mod's settings and its options page in 2; images in 3; the pad
// filter, the menu and the shown picture in 4). The one header a plugin includes; it links against nothing of mkw-recomp.
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

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MKW_PLUGIN_API_VERSION 4

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

struct MkwPage;   // a mod's page (below): MkwPluginApi.settings_tab draws one

// A player's pad as the game is about to read it (input_filter, version 4): the buttons after the layout, the
// remapping and the keyboard's share, so what a remapper outside the game (Steam Input) made of the pad.
enum {   // MkwPad.buttons bits
    MKW_PAD_A, MKW_PAD_B, MKW_PAD_X, MKW_PAD_Y, MKW_PAD_STICK_L, MKW_PAD_STICK_R, MKW_PAD_L, MKW_PAD_R, MKW_PAD_ZL,
    MKW_PAD_ZR, MKW_PAD_PLUS, MKW_PAD_MINUS, MKW_PAD_LEFT, MKW_PAD_UP, MKW_PAD_RIGHT, MKW_PAD_DOWN,
};
typedef struct MkwPad {
    uint32_t size;           // sizeof(MkwPad) of the runtime
    int player;              // 0: player 1 (with the keyboard), 1-7: the others
    uint64_t buttons;        // 1 << MKW_PAD_*
    int32_t lx, ly, rx, ry;  // the sticks, -32767..32767, up and right positive
    int guide;               // the Guide (Xbox, PS, Home) button held; player 1 only
} MkwPad;
typedef void (*MkwPadFn)(MkwPad *pad, void *user);
typedef void (*MkwFrameFn)(const uint8_t *rgba, int width, int height, int row_bytes, void *user);

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

    // ---- version 2: who plays, in which language, and the mod's settings (any thread)
    const char *language;       // the menu language: "en", "ja", "pt-BR", "zh-Hans"... (the start screen's setting)
    const char *profile_name;   // the player playing (the start screen's profile), UTF-8
    // This mod's settings for the player playing: what its options page (MkwPage below) stored, kept per player in
    // <player folder>/mod-settings/<mod>.ini, so a mod's update or re-import keeps them. setting_get copies the value
    // into out (NUL-terminated, cut at cap) and returns 1; 0 when the key is unset. setting_set stores value (NULL
    // removes the key) and writes the file at once; 1 done. Keys: letters, digits, '.', '_', '-'; values one line.
    int (*setting_get)(const char *key, char *out, size_t cap);
    int (*setting_set)(const char *key, const char *value);
    // cb(user) when the game ends normally (the window closed, Quit in the menu, the game's own exit), before the
    // process does, on the thread that ends it; callbacks run once, in registration order. Keep it short (a few
    // network goodbyes with a timeout of a second or two): the process ends right after. A crash or a killed process
    // runs none. Only from mkw_plugin_init; 1 done, 0 refused.
    int (*on_exit)(void (*cb)(void *user), void *user);
    // A tab of the plugin's in the in-game menu's settings (F1, the controller's menu button), after General: draw is
    // called each frame the tab is shown, with the MkwPage below (in_game 1), on the menu's thread, while the game
    // runs: it must not block (the game's picture waits for it). title: UTF-8, copied. Only from mkw_plugin_init;
    // 1 done, 0 refused. The same function can serve as the start screen's options page (mkw_mod_page).
    int (*settings_tab)(const char *title, void (*draw)(const struct MkwPage *page));

    // ---- version 3: images the plugin fills and the game shows (any thread)
    // An image of width x height pixels, RGBA8 (the format the game's texture asks for, sRGB or not), in memory of the
    // runtime's: *pixels gets its first row and *row_bytes the distance between rows (at least width * 4). Returns
    // its nn::gs::NativeImageHandle, 0 when refused (size outside 1..4096, too many images, no memory): hand it to the
    // game where it asks for a native image handle (an override of the call that gives one), and the texture the game
    // makes from it (nvnTextureInitializeFromNativeImage) samples these pixels. Images live until the process ends.
    uint32_t (*native_image_create)(int width, int height, void **pixels, int *row_bytes);
    // After writing an image's pixels: the GPU reads them again before the game's next use of the texture. Write
    // only an image the game is not showing (triple buffer as the game's own native images do, with their syncs).
    // 1 done, 0 not one of native_image_create's handles.
    int (*native_image_touch)(uint32_t handle);

    // ---- version 4: the pad before the game reads it, the menu and the shown picture (an overlay of the plugin's own)
    // filter(pad, user) runs at every read of a player's input, on the thread that reads it (the game's, several times
    // a frame), before the runtime's own menu shortcut and before the game: what it clears or sets is what both get,
    // so a button the plugin takes opens nothing else, and a pad it empties is an idle pad for the game. It does not
    // wait and runs no guest code. One filter per plugin, in load order. Only from mkw_plugin_init; 1 done, 0 refused.
    int (*input_filter)(MkwPadFn filter, void *user);
    // Opens (1) or closes (0) the runtime's in-game menu, as F1 does. Any thread.
    void (*menu_open)(int open);
    // The next shown frame as the player sees it (the game's picture with the menu, notices and overlays; sRGB also on
    // an HDR display), scaled down to fit max_w x max_h (not up), RGBA8 with opaque alpha: cb(rgba, width, height,
    // row_bytes, user) on the present thread, once; the pixels are the runtime's and gone when cb returns. cb does not
    // wait. No frame is shown while the window is minimised: then cb is not called. A newer request replaces one not
    // answered yet. 1 asked, 0 refused (a size outside 16..4096).
    int (*frame_capture)(int max_w, int max_h, MkwFrameFn cb, void *user);

    // ---- version 4, continued: the plugin's overlay covers the game as a console's HOME menu does
    // in_focus 0: the game is told it is out of focus (nn::oe::GetCurrentFocusState 2 and the FocusStateChanged
    // message), whatever the window says and also with MKW_ASSUME_FOCUS, and the game's sound is silent. With suspend
    // 1 the game is also suspended as the console suspends an application behind HOME, when the game lets it: by its
    // own nn::oe::SetFocusHandlingMode at that moment and at each change of it (MKW: suspended, except while it uses
    // the network; then it is only notified and runs on). Suspended, the game thread waits in its present: the
    // picture stays, the game's clock (it counts frames) stands still, no input is read, so the plugin's pad filter
    // is not called; loading and audio threads go on. in_focus 1: what the window says again, and the game runs.
    // Returns 1 when the game is suspended now, else 0. Any thread.
    int (*focus_set)(int in_focus, int suspend);
    // Ends the game as the menu's Quit and the window's close button do: the plugins' on_exit, the saves finished,
    // the pipeline cache written, then the process ends. Returns at once; also while suspended. Any thread.
    void (*quit)(void);
} MkwPluginApi;

// Exported by the plugin. api_version is the runtime's (api->version); size tells which fields exist. Return 0 to stay
// loaded; anything else declines (an unsupported game build, say): the runtime logs it with message, if set, and
// shows it to the player.
// In C++ the export keeps its C name (extern "C"), which is the name the runtime looks up.
#ifdef __cplusplus
#define MKW_PLUGIN_EXTERN_C extern "C"
#else
#define MKW_PLUGIN_EXTERN_C
#endif
#if defined(_WIN32)
#define MKW_PLUGIN_EXPORT MKW_PLUGIN_EXTERN_C __declspec(dllexport)
#else
#define MKW_PLUGIN_EXPORT MKW_PLUGIN_EXTERN_C __attribute__((visibility("default")))
#endif
typedef int (*MkwPluginInit)(const MkwPluginApi *api, uint32_t api_version, const char **message);

// ---- The mod's options page (version 2)
//
// A plugin may also export mkw_mod_page. The start screen's Mods page then shows an Options button beside the mod;
// pressed, the page below is drawn with what the plugin asks for, each frame, until the player leaves it. That runs in
// the start screen's process, before any game: mkw_plugin_init is not called there and no game entry exists, so the
// page only shows and stores the mod's settings (the plugin reads them in the game with setting_get) and may do work of
// its own on threads of its own (a sign-in, a list fetched from a server), drawing its state on the next frames.
//
// Immediate mode, as the start screen's own pages: the plugin calls the widgets in the order they appear, every
// frame, and a widget answers in the frame the player used it. The look, the fonts, controller and keyboard navigation
// and the language are the start screen's. Text is UTF-8; the plugin translates its own (page->language).
//
// Every entry is for the page's thread (the one calling mkw_mod_page), except log, setting_get / setting_set and
// redraw, which any thread may call (in the game, a plugin's own threads use MkwPluginApi's setting_get / setting_set).

enum {   // MkwPage.note kinds
    MKW_NOTE_DIM,     // secondary text
    MKW_NOTE_INFO,    // the theme's accent
    MKW_NOTE_OK,      // green: done, connected
    MKW_NOTE_WARN,    // amber: needs attention
    MKW_NOTE_ERROR,   // red: failed
};
enum {   // MkwPage.input flags
    MKW_INPUT_PASSWORD = 1,   // shown as dots
    MKW_INPUT_DIGITS = 2,     // only 0-9 accepted
};

typedef struct MkwPage {
    uint32_t size;      // sizeof(MkwPage) of the runtime: a field is there when its offset is below size
    uint32_t version;   // MKW_PLUGIN_API_VERSION of the runtime
    const char *port_version, *mod_dir, *mod_name;
    const char *language;       // the menu language ("en", "pt-BR", ...), as MkwPluginApi.language
    const char *profile_name;   // the player the next start plays; the settings below are this player's
    float scale;                // the start screen's UI scale: 1 at 1080 lines
    int opened;                 // 1 in the first frame after the page opened (or the player changed), else 0
    int in_game;                // 1: a settings tab of the in-game menu (settings_tab); 0: the start screen's page

    void (*log)(const char *fmt, ...);   // the start screen's log, prefixed with the mod
    int (*setting_get)(const char *key, char *out, size_t cap);   // as MkwPluginApi's: the same file
    int (*setting_set)(const char *key, const char *value);
    void (*redraw)(void);       // a thread of the plugin's changed what the page shows: draw it again soon

    // ---- text and layout
    void (*heading)(const char *text);
    void (*text)(const char *text);               // wrapped to the page
    void (*note)(int kind, const char *text);     // wrapped, coloured by kind (MKW_NOTE_*)
    void (*separator)(void);
    void (*same_line)(void);                      // the next widget on this line
    // The widgets between the two greyed out and inert when disabled is non-zero, as they are otherwise. Always
    // paired (a disabled_begin(0) too), and nested sections stay disabled when an outer one is.
    void (*disabled_begin)(int disabled);
    void (*disabled_end)(void);
    void (*busy)(const char *text);               // a spinner and text: work in progress

    // ---- widgets bound to a setting: they show its value and store the player's change themselves (setting_set);
    // each returns 1 in the frame the value changed. key is also the widget's identity on the page.
    int (*checkbox)(const char *key, const char *label, int default_on);   // stores "1" / "0"
    // A one-line field; hint shows while it is empty. Stored when the player leaves the field (Enter, a click away,
    // the controller moving on), not at each key. max_bytes: of the value, at most 255.
    int (*input)(const char *key, const char *label, const char *hint, int max_bytes, int flags);
    // One of n values; labels[i] is shown for values[i]. Stores the value.
    int (*choice)(const char *key, const char *label, const char *const *values, const char *const *labels, int n);

    // ---- actions
    int (*button)(const char *label);            // 1 in the frame it was pressed; label is also its identity

    // ---- lists (friends, servers, saves...): rows of a name, a dim detail and up to 4 buttons each
    // Between list_begin and list_end, one list_row per row. visible_rows: the list's height (it scrolls past it).
    // list_row returns 1 + the index of the button pressed in that row this frame, else 0. Rows are told apart by
    // their position and name: two rows with the same name need different details.
    void (*list_begin)(const char *id, int visible_rows);
    int (*list_row)(const char *name, const char *detail, const char *const *buttons, int nbuttons);
    void (*list_end)(void);
    void (*list_empty)(const char *text);         // between begin and end: the line shown when there is no row

    // ---- columns: what follows columns_begin(n) goes into the first of n columns of equal width, column_next moves
    // to the next one, columns_end closes them (a list stays inside its column). Not nested.
    void (*columns_begin)(int n);
    void (*column_next)(void);
    void (*columns_end)(void);
    // An integer from min to max as a slider, bound to a setting like the widgets above (stored when let go).
    int (*slider)(const char *key, const char *label, int min, int max, int default_value);
} MkwPage;

typedef void (*MkwModPage)(const MkwPage *page);   // the export: MKW_PLUGIN_EXPORT void mkw_mod_page(const MkwPage *)

#ifdef __cplusplus
}
#endif

#endif
