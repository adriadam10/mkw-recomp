// Steam Remote Play Together (steam_rpt.h). With MKW_STEAM_APPID set (a number, or auto: the game Steam started), the game process starts Steam's API as that App
// ID before the game boots; Steam then streams the window and the sound to the guests itself, and this file turns the
// guests into players:
//   - a guest's controller is a Steam virtual gamepad on the host, which input.c gives to the next free player once SDL
//     lists it (SDL_GAMECONTROLLER_ALLOW_STEAM_VIRTUAL_GAMEPAD, set here);
//   - a guest's keyboard comes through Remote Play's direct input (SDK 1.62+): at the guest's first key with a binding
//     it becomes a player of its own (input_remote_*, input.h), on the host's key table, when some guest has no
//     controller (more guests than guest gamepads and keyboard players) and not in the guest's first 5 s.
// The steam thread runs Steam's callbacks (manual dispatch: nothing of Steam's runs on a game thread), reads the
// guests' keys every 8 ms and carries out the menu's buttons (invite through the overlay, a guest link).
//
// Written from the public Steamworks SDK 1.65 headers (steam_flat.h). The idea of running under a chosen App ID and of
// mapping guests' virtual gamepads and Remote Play keyboards to players comes from Eglantine's Remote Play Together
// extension (github.com/damienjacinto/eglantine-remote-play-together); that code is GPL and none of it is copied here
// (AGENTS.md, Licenses).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "input.h"
#include "nvn_api.h"
#include "platform/platform.h"
#include "steam_flat.h"
#include "steam_rpt.h"
#include "ui/menu/menu.h"
#include "ui_text.h"

enum { TICK_NS = 8000000, SYNC_NS = 1000000000, MAX_GUESTS = 8, EVENTS = 32 };
// A guest's keys in their first 5 s give no player: their controller, if they have one, reaches SDL several seconds
// after the session starts (users' logs), and a keyboard player taken first would give them two players.
#define KEY_WAIT_NS 5000000000ull
enum { LINK_NONE, LINK_WAITING, LINK_READY, LINK_REFUSED };

static SteamApi api;
static void *rp, *friends;
static SteamPipe pipe_h;
static int direct_input;

static volatile int32_t g_state = -1;   // STEAM_RPT_*; -1 before steam_rpt_start
static char g_reason[512];
static volatile int32_t g_guests;
static volatile int32_t req_invite, req_link, invite_failed, link_state;
static PlatMutex link_lock = PLAT_MUTEX_INIT;
static char link_url[1024];

// The steam thread's own.
typedef struct {
    SteamSession id;
    int player;       // the player of the guest's keyboard, -1 before the first key
    int full;         // a key found no free player (said once)
    int held;         // a key came while every guest had a controller (logged once)
    uint64_t joined_ns;
    char name[64];
} Guest;
static Guest guests[MAX_GUESTS];
static int n_guests;

static int is_app_id(const char *s);

// steam_appid = auto: the App ID Steam put in the environment when it started mkw from a game's launch options
// (%command%), so the overlay and the guests' controllers are that game's. Read once, before steam_rpt_start sets it.
static char auto_id[16];
const char *steam_rpt_app_id(void) {
    const char *v = cfg_get("MKW_STEAM_APPID");
    if (!v) return "";
    if (strcmp(v, "auto")) return v;
    if (!auto_id[0]) {
        char e[32];
        if (plat_getenv("SteamAppId", e, sizeof e) && is_app_id(e)) snprintf(auto_id, sizeof auto_id, "%s", e);
    }
    return auto_id[0] ? auto_id : "auto";
}

int steam_rpt_state(void) {
    int32_t s = plat_cas32(&g_state, -1, -1);
    if (s >= 0) return s;
    return *steam_rpt_app_id() ? STEAM_RPT_IDLE : STEAM_RPT_OFF;
}

const char *steam_rpt_reason(void) { return steam_rpt_state() == STEAM_RPT_FAILED ? g_reason : ""; }
int steam_rpt_guests(void) { return plat_cas32(&g_guests, 0, 0); }
int steam_rpt_invite_failed(void) { return plat_cas32(&invite_failed, 0, 0); }

void steam_rpt_invite(void) {
    plat_xchg32(&invite_failed, 0);
    plat_xchg32(&req_invite, 1);
}

void steam_rpt_request_link(void) {
    plat_xchg32(&link_state, LINK_WAITING);
    plat_xchg32(&req_link, 1);
}

// The answer to steam_rpt_multiplayer's choice (present path, menu_choice): 0 local players, 1 Remote Play Together,
// -1 none. Either way the game went back to its menu; the notice says what to do before picking the mode again.
static volatile int32_t choice_need, choice_have;
static void choice_done(int choice, void *ctx) {
    (void)ctx;
    int need = plat_cas32(&choice_need, 0, 0), have = plat_cas32(&choice_have, 0, 0);
    char text[256];
    if (choice == 1) {
        rt_log("steam: local multiplayer: Remote Play Together chosen: opening the invite list");
        steam_rpt_invite();
        snprintf(text, sizeof text, menu_text(UI_NOTICE_RPT_INVITE), need, have);
        menu_notice(text, 10);
    } else if (choice == 0) {
        rt_log("steam: local multiplayer: local players chosen");
        snprintf(text, sizeof text, menu_text(UI_NOTICE_PADS_MISSING), need, have);
        menu_notice(text, 8);
    }
}

int steam_rpt_multiplayer(int need, int have) {
    if (need < 2 || steam_rpt_state() != STEAM_RPT_READY) return 0;
    const char *mode = cfg_get("MKW_STEAM_INVITE");
    if (mode && !strcmp(mode, "off")) return 0;
    int missing = have < need;
    // ask (default): with too few controllers the player chooses how the others join, local controllers or Steam's
    // invite list; with one each the game starts at once, local or with friends already in
    if (!mode || !*mode || !strcmp(mode, "ask")) {
        if (!missing) return 0;
        plat_xchg32(&choice_need, need);
        plat_xchg32(&choice_have, have);
        char title[320];
        snprintf(title, sizeof title, menu_text(UI_MENU_RPT_CHOICE), need, have);
        const char *labels[2] = {menu_text(UI_MENU_RPT_CHOICE_LOCAL), menu_text(UI_MENU_RPT_CHOICE_STEAM)};
        int shown = menu_choice(title, labels, 2, choice_done, NULL);
        if (shown) return 1;   // shown, or one is up already: the answer's notice comes then
        rt_log("steam: local multiplayer: the menu cannot ask: as steam_invite = auto");
    }
    // auto: the invite list opens while no guest has joined yet or controllers are missing; once friends are in and
    // every player has a controller, picking the mode again starts it without the overlay in the way
    int always = mode && !strcmp(mode, "always");
    if (!missing && !always && steam_rpt_guests() > 0) return 0;
    // The game asks again at once when the player picks the mode again: one overlay for a burst of requests
    static volatile int32_t last_s;
    int32_t now_s = (int32_t)(plat_now_ns() / 1000000000ull), was = plat_xchg32(&last_s, now_s);
    if (!was || now_s - was >= 3) {
        rt_log("steam: local multiplayer for %d players, %d with a controller: opening the Remote Play Together invite "
               "list", need, have);
        steam_rpt_invite();
    }
    if (!missing) return 0;
    char text[256];
    snprintf(text, sizeof text, menu_text(UI_NOTICE_RPT_INVITE), need, have);
    menu_notice(text, 10);
    return 1;
}

int steam_rpt_take_link(char *out, size_t cap) {
    int32_t s = plat_cas32(&link_state, 0, 0);
    if (s == LINK_REFUSED) { plat_cas32(&link_state, LINK_NONE, LINK_REFUSED); return -1; }
    if (s != LINK_READY || plat_cas32(&link_state, LINK_NONE, LINK_READY) != LINK_READY) return 0;
    plat_mutex_lock(&link_lock);
    snprintf(out, cap, "%s", link_url);
    plat_mutex_unlock(&link_lock);
    return 1;
}

// ---- the steam thread

static int guest_index(SteamSession id) {
    for (int i = 0; i < n_guests; i++)
        if (guests[i].id == id) return i;
    return -1;
}

static void notice(UiTextId fmt, const char *name, int player) {
    char text[200];
    if (fmt == UI_NOTICE_RPT_KEYBOARD) snprintf(text, sizeof text, menu_text(fmt), name, player + 1);
    else snprintf(text, sizeof text, menu_text(fmt), name);
    menu_notice(text, 5);
}

static void add_guest(SteamSession id) {
    if (!id || guest_index(id) >= 0) return;
    if (n_guests == MAX_GUESTS) { rt_log("steam: guest session %u ignored: %d guests already", id, MAX_GUESTS); return; }
    Guest *g = &guests[n_guests++];
    *g = (Guest){.id = id, .player = -1, .joined_ns = plat_now_ns()};
    const char *persona = friends ? api.persona_name(friends, api.session_steam_id(rp, id)) : NULL;
    const char *client = api.session_client_name(rp, id);
    snprintf(g->name, sizeof g->name, "%s", persona && *persona ? persona : client && *client ? client : "Guest");
    if (direct_input) api.mouse_visible(rp, id, false);   // the guest plays with a controller or keys, not a cursor
    plat_xchg32(&g_guests, n_guests);
    rt_log("steam: %s joined (session %u, %s; %d guests)", g->name, id, client ? client : "unknown device", n_guests);
    notice(UI_NOTICE_RPT_JOINED, g->name, 0);
}

static void remove_guest(int i) {
    Guest g = guests[i];
    guests[i] = guests[--n_guests];
    if (g.player >= 0) input_remote_detach(g.player);
    plat_xchg32(&g_guests, n_guests);
    rt_log("steam: %s left (session %u; %d guests)", g.name, g.id, n_guests);
    notice(UI_NOTICE_RPT_LEFT, g.name, 0);
}

// Steam's list of sessions over ours, once a second: a callback missed (sessions from before the start) changes nothing.
static void sync_sessions(void) {
    SteamSession now[MAX_GUESTS];
    uint32_t n = api.session_count(rp);
    if (n > MAX_GUESTS) n = MAX_GUESTS;
    for (uint32_t i = 0; i < n; i++) now[i] = api.session_id(rp, (int)i);
    for (int i = n_guests - 1; i >= 0; i--) {
        int found = 0;
        for (uint32_t k = 0; k < n && !found; k++) found = now[k] == guests[i].id;
        if (!found) remove_guest(i);
    }
    for (uint32_t k = 0; k < n; k++) add_guest(now[k]);
}

static void on_link(const SteamGuestInvite *inv) {
    plat_mutex_lock(&link_lock);
    snprintf(link_url, sizeof link_url, "%.*s", (int)sizeof inv->url, inv->url);
    plat_mutex_unlock(&link_lock);
    plat_xchg32(&link_state, LINK_READY);
    rt_log("steam: guest link %s", link_url);   // the log has it too, when the clipboard does not
}

static void callbacks(void) {
    api.dispatch_frame(pipe_h);
    SteamCallbackMsg m;
    while (api.dispatch_next(pipe_h, &m)) {
        size_t size = m.size > 0 ? (size_t)m.size : 0;
        if (m.id == STEAM_CB_SESSION_CONNECTED && size >= sizeof(SteamSessionEvent))
            add_guest(((const SteamSessionEvent *)m.param)->session);
        else if (m.id == STEAM_CB_SESSION_DISCONNECTED && size >= sizeof(SteamSessionEvent)) {
            int i = guest_index(((const SteamSessionEvent *)m.param)->session);
            if (i >= 0) remove_guest(i);
        } else if (m.id == STEAM_CB_GUEST_INVITE && size >= sizeof(SteamGuestInvite))
            on_link((const SteamGuestInvite *)m.param);
        api.dispatch_free(pipe_h);
    }
}

static int keyboard_players(void) {
    int n = 0;
    for (int i = 0; i < n_guests; i++) n += guests[i].player >= 0;
    return n;
}

// The guests' keys (direct input). A key without a key id (Esc, F1, F10, F11: the host's menu and window) is dropped.
static void read_keys(void) {
    SteamRemoteInput ev[EVENTS];
    uint32_t n;
    do {
        n = api.get_input(rp, ev, EVENTS);
        for (uint32_t i = 0; i < n && i < EVENTS; i++) {
            int down = ev[i].type == STEAM_RP_KEY_DOWN;
            if (!down && ev[i].type != STEAM_RP_KEY_UP) continue;
            int gi = guest_index(ev[i].session), key = input_key_of_usb(ev[i].key.scancode);
            if (gi < 0 || !key) continue;
            Guest *g = &guests[gi];
            // A keyboard is a player only for a guest without a controller. Steam does not say whose a controller is,
            // so: while guest controllers and keyboard players together are as many as the guests, every guest plays
            // already and keys play no one
            int attach = down && g->player < 0 && !g->full;
            if (attach && plat_now_ns() - g->joined_ns < KEY_WAIT_NS) attach = 0;   // too early: a later key decides
            else if (attach && input_guest_pads() + keyboard_players() >= n_guests) {
                if (!g->held) rt_log("steam: %s's keys give no player: every guest plays already", g->name);
                g->held = 1, attach = 0;
            }
            if (attach) {
                g->player = input_remote_attach();
                if (g->player >= 0) notice(UI_NOTICE_RPT_KEYBOARD, g->name, g->player);
                else {
                    g->full = 1;
                    rt_log("steam: %s's keyboard found no free player", g->name);
                    notice(UI_NOTICE_RPT_FULL, g->name, 0);
                }
            }
            if (g->player >= 0) input_remote_key(g->player, key, down);
        }
    } while (n == EVENTS);
}

static void requests(void) {
    if (plat_xchg32(&req_invite, 0) && !api.show_ui(rp)) {
        plat_xchg32(&invite_failed, 1);
        rt_log("steam: the overlay did not open (Steam overlay off, or mkw not started from Steam); the guest link works "
               "without it");
        menu_notice(menu_text(UI_NOTICE_RPT_NO_OVERLAY), 10);
    }
    if (plat_xchg32(&req_link, 0) && !api.send_invite(rp, 0)) {   // 0, an invalid friend: a link for anyone
        plat_xchg32(&link_state, LINK_REFUSED);
        rt_log("steam: no guest link (Steam refused: is App ID %s set up for Remote Play Together?)", steam_rpt_app_id());
    }
}

static void steam_main(void *arg) {
    (void)arg;
    uint64_t next_sync = 0;
    for (;;) {
        plat_wait_ns(TICK_NS);
        requests();
        callbacks();
        if (direct_input) read_keys();
        uint64_t now = plat_now_ns();
        if (now >= next_sync) {
            next_sync = now + SYNC_NS;
            sync_sessions();
        }
    }
}

// Steam could not be used: say why once the game shows its first picture (a notice before it would expire unseen).
static void failed_main(void *arg) {
    (void)arg;
    for (int i = 0; i < 600 && !__atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED); i++) plat_wait_ns(100000000);
    plat_wait_ns(2000000000);
    char text[256];
    snprintf(text, sizeof text, menu_text(UI_NOTICE_RPT_FAILED), g_reason);
    menu_notice(text, 12);
}

static void fail(const char *fmt, const char *arg) {
    snprintf(g_reason, sizeof g_reason, fmt, arg);
    plat_xchg32(&g_state, STEAM_RPT_FAILED);
    rt_log("steam: Remote Play Together is off: %s", g_reason);
    plat_thread_spawn(failed_main, NULL, "steam", 0);
}

static int is_app_id(const char *s) {
    size_t n = strlen(s);
    if (!n || n > 10) return 0;
    for (size_t i = 0; i < n; i++)
        if (s[i] < '0' || s[i] > '9') return 0;
    return strtoull(s, NULL, 10) > 0;
}

// Steam's library: the copy beside mkw first (what the release folder carries), then the system's search.
static void *open_library(char *tried, size_t cap) {
    char path[PLAT_PATH_MAX];
    void *lib = NULL;
    if (plat_exe_path(path, sizeof path)) {
        char *sep = plat_last_sep(path);
        size_t dir = sep ? (size_t)(sep - path + 1) : 0;
        snprintf(path + dir, sizeof path - dir, "%s", STEAM_LIBRARY);
        lib = plat_lib_open(path);
    } else {
        snprintf(path, sizeof path, "%s", STEAM_LIBRARY);
    }
    snprintf(tried, cap, "%s", path);
    return lib ? lib : plat_lib_open(STEAM_LIBRARY);
}

void steam_rpt_start(void) {
    if (plat_cas32(&g_state, -1, -1) >= 0) return;
    const char *app = steam_rpt_app_id();
    if (!*app) { plat_xchg32(&g_state, STEAM_RPT_OFF); return; }
    if (!strcmp(app, "auto")) {
        fail("steam_appid = %s, but Steam passed no App ID: start mkw from a Steam game's launch options (%%command%%)",
             app);
        return;
    }
    if (!is_app_id(app)) { fail("steam_appid = %s is not a Steam App ID (a number)", app); return; }
    char started[32];   // a number set while Steam started mkw as another game: the overlay is that game's
    if (plat_getenv("SteamAppId", started, sizeof started) && is_app_id(started) && strcmp(started, app))
        rt_log("steam: Steam started mkw as App ID %s, but steam_appid = %s: set steam_appid = auto to follow the game",
               started, app);

    char tried[PLAT_PATH_MAX];
    void *lib = open_library(tried, sizeof tried);
    if (!lib) { fail("Steam's library is missing: put " STEAM_LIBRARY " next to mkw (looked for %s)", tried); return; }
    const char *missing = NULL;
#define BIND(field, name) \
    if (!(*(void **)&api.field = plat_lib_sym(lib, name)) && !missing) missing = name;
    STEAM_SYMBOLS(BIND)
#undef BIND
    if (missing) { fail(STEAM_LIBRARY " is older than Steamworks SDK 1.62 (it lacks %s)", missing); return; }

    // Steam's client reads the App ID from the environment (a steam_appid.txt would do too); the setting wins over
    // what a Steam shortcut set, which names the shortcut and not a game Steam knows.
    plat_setenv("SteamAppId", app);
    plat_setenv("SteamGameId", app);
    PlatHandlers *h = plat_handlers_save();
    SteamErrMsg err = {0};
    int r = api.init(STEAM_INTERFACES, &err);
    int restored = plat_handlers_restore(h);
    if (restored) rt_log("steam: %d signal handlers Steam replaced were given back to the runtime", restored);
    err[sizeof err - 1] = 0;
    if (r != STEAM_INIT_OK) {
        // The generic failure covers both a Steam that is not running (13/11/2026, SDK 1.65's libsteam_api.so without
        // a client: FailedGeneric, "Failed to load module .../steamclient.so") and an account without the App ID, so
        // it names both and adds Steam's own words.
        static const char *const why[] = {
            [STEAM_INIT_FAILED] = "Steam did not start App ID %s: start Steam and sign in with an account that owns it",
            [STEAM_INIT_NO_CLIENT] = "Steam is not running or not signed in (App ID %s); on Linux, not the Flatpak or Snap Steam",
            [STEAM_INIT_VERSION_MISMATCH] = "Steam is out of date for App ID %s: let it update",
        };
        for (char *p = err; (p = strchr(p, '%')); p++) *p = ' ';   // Steam's words go into a format below
        char fmt[400];
        snprintf(fmt, sizeof fmt, "%s%s%s%s", r > 0 && r <= STEAM_INIT_VERSION_MISMATCH ? why[r] : why[STEAM_INIT_FAILED],
                 err[0] ? " (Steam: " : "", err, err[0] ? ")" : "");
        fail(fmt, app);
        return;
    }
    api.dispatch_init();
    pipe_h = api.pipe();
    friends = api.friends();
    rp = api.remote_play();
    if (!rp) { fail("this Steam has no Remote Play interface (App ID %s): update Steam", app); return; }
    direct_input = api.direct_input_on(rp);
    if (!direct_input) rt_log("steam: no direct input: guests' keyboards type on the host's keyboard (player 1)");

    // SDL hides Steam's virtual gamepads unless told, and those are the guests' controllers (input.c sorts the host's
    // own out). Read by SDL at each gamepad scan, so in time before input.c's first.
    char set[8];
    if (!plat_getenv("SDL_GAMECONTROLLER_ALLOW_STEAM_VIRTUAL_GAMEPAD", set, sizeof set))
        plat_setenv("SDL_GAMECONTROLLER_ALLOW_STEAM_VIRTUAL_GAMEPAD", "1");
    plat_xchg32(&g_state, STEAM_RPT_READY);
    if (!plat_thread_spawn(steam_main, NULL, "steam", 0)) rt_fatal("steam: cannot start the steam thread");
    rt_log("steam: running as App ID %s; Remote Play Together ready (guest keyboards %s)", app,
           direct_input ? "as players of their own" : "merged with the host's");
}
