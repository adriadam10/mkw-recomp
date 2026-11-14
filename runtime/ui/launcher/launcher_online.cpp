// The start screen's Online page, in the order a player needs it: who they are (name, friend code), how they play
// (off, hosting the server built into the game, or joining a friend's), then their friends, then LAN play through
// Tailscale (net_online.h). The server calls run on a thread of their own, one at a time, so the page never waits for
// the network.
//
// Why a page of its own: the game's own friend screens show only what the server already knows, and the game cannot
// type a friend code; before this page a player had no way to become friends with anyone from the port.
#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

#include "imgui.h"
#include "ui/launcher/launcher_online.h"
#include "core/base/players.h"
#include "ui/launcher/launcher_players.h"

extern "C" {
#include "cfg.h"
#include "net_online.h"
#include "ui_text.h"
void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
}

enum { JOB_REFRESH, JOB_ACTION, JOB_TEST };
enum { MODE_OFF, MODE_HOST, MODE_JOIN };

typedef struct {
    int job;
    char server[160], op[16], code[40];
} Task;

static struct {
    SDL_Mutex *lock;
    volatile int busy;
    int started;                 // the server text was read from the settings
    char server[160];            // as edited
    char add[40];                // the code being typed
    char peers[256];             // MKW_LAN_PEERS as edited
    char code[18];               // this player's, formatted; "" without a server
    uint64_t copied_ms;
    // the last answer, under lock
    NetFriendLists lists;
    int have_lists;
    char status[320];
    int status_ok;               // 1 green, 0 red, -1 grey
} O;

static NetFriendLists g_tmp;     // the worker's (one job at a time)

static void set_status(const char *text, int ok) {
    snprintf(O.status, sizeof O.status, "%s", text);
    O.status_ok = ok;
}

static void say(const char *text, int ok) {
    SDL_LockMutex(O.lock);
    set_status(text, ok);
    SDL_UnlockMutex(O.lock);
}

static int SDLCALL worker(void *arg) {
    Task *t = (Task *)arg;
    char err[300] = "", msg[400] = "";
    int ok = 1;
    if (t->job == JOB_TEST) {
        // Connect: the server may be a new one at an old address (a friend's PC whose server made new keys): its
        // certificate is fetched again, now that the player asks for this address
        net_forget_ca(t->server);
        const char *e = net_probe(t->server, err, sizeof err);
        ok = !*e;
        if (ok) snprintf(msg, sizeof msg, "%s", menu_text(UI_ONLINE_REACHED));
        else snprintf(msg, sizeof msg, menu_text(UI_ONLINE_FAILED), e);
    } else if (t->job == JOB_ACTION) {
        const char *e = net_friends_do(t->server, t->op, t->code, err, sizeof err);
        ok = !*e;
        snprintf(msg, sizeof msg, "%s", ok ? menu_text(UI_ONLINE_DONE) : e);
        rt_log("online page: %s %s: %s", t->op, t->code, ok ? "done" : e);
    }
    // every job ends with the server's lists, which also fetches its certificate the first time
    int lists_ok = 0;
    if (ok) {
        net_friends_get(t->server, &g_tmp);
        lists_ok = g_tmp.reached && !g_tmp.error[0];
        if (!lists_ok && t->job != JOB_ACTION) {
            // reached but refused: the server's own words; a code it never saw is the normal first state
            if (g_tmp.reached && strstr(g_tmp.error, "play online once first")) {
                ok = -1;
                snprintf(msg, sizeof msg, "%s", menu_text(UI_ONLINE_FIRST_SIGNIN));
            } else {
                ok = 0;
                snprintf(msg, sizeof msg, menu_text(g_tmp.reached ? UI_ONLINE_SERVER_SAYS : UI_ONLINE_FAILED),
                         g_tmp.error);
            }
        }
    }
    SDL_LockMutex(O.lock);
    if (lists_ok) {
        O.lists = g_tmp;
        O.have_lists = 1;
    }
    if (msg[0]) set_status(msg, ok);
    O.busy = 0;
    SDL_UnlockMutex(O.lock);
    SDL_free(t);
    return 0;
}

static void start(int job, const char *op = "", const char *code = "") {
    if (O.busy) return;
    if (!O.server[0]) {
        say(menu_text(UI_ONLINE_NEED_SERVER), 0);
        return;
    }
    Task *t = (Task *)SDL_calloc(1, sizeof *t);
    if (!t) return;
    t->job = job;
    snprintf(t->server, sizeof t->server, "%s", O.server);
    snprintf(t->op, sizeof t->op, "%s", op);
    snprintf(t->code, sizeof t->code, "%s", code);
    O.busy = 1;
    if (job == JOB_TEST) say(menu_text(UI_ONLINE_TESTING), -1);
    SDL_Thread *th = SDL_CreateThread(worker, "online", t);
    if (th) {
        SDL_DetachThread(th);
    } else {
        O.busy = 0;
        SDL_free(t);
    }
}

// This player's code for the server written (made on first view: the server keeps it at the first sign-in).
static void load_code(void) {
    NetProfile p;
    O.code[0] = 0;
    if (O.server[0] && net_profile_load(O.server, &p, 1)) net_format_code(p.code, O.code);
}

static void save_server(void) {
    cfg_set("MKW_ONLINE_SERVER", O.server);
    cfg_save();
    load_code();
    SDL_LockMutex(O.lock);
    O.have_lists = 0;
    SDL_UnlockMutex(O.lock);
}

void online_opened(void) {
    if (!O.lock) O.lock = SDL_CreateMutex();
    if (!O.started) {
        const char *s = cfg_get("MKW_ONLINE_SERVER");
        snprintf(O.server, sizeof O.server, "%s", s ? s : "");
        const char *p = cfg_get("MKW_LAN_PEERS");
        snprintf(O.peers, sizeof O.peers, "%s", p ? p : "");
        O.started = 1;
    }
    load_code();
    if (O.server[0] && net_enabled()) start(JOB_REFRESH);
}

static const ImVec4 kGold(0.93f, 0.76f, 0.33f, 1), kGrey(0.65f, 0.65f, 0.62f, 1), kGreen(0.55f, 0.85f, 0.6f, 1),
    kRed(0.95f, 0.45f, 0.35f, 1);

static void wrapped(const char *text, const ImVec4 &col) {
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

// ---- hosting (net_host.c): this PC's address on the chosen network becomes the game's server
static void host_apply(bool restart) {
    NetAddrs a;
    net_addrs(&a);
    const char *ip = net_network_address(&a);
    if (*ip) {
        char addr[64];
        snprintf(addr, sizeof addr, "%s:%d", ip, NET_HOST_PORT);
        const char *cur = cfg_get("MKW_ONLINE_SERVER");
        if (!cur || strcmp(cur, addr)) cfg_set("MKW_ONLINE_SERVER", addr);
        cfg_save();
        snprintf(O.server, sizeof O.server, "%s", addr);
        load_code();
    }
    if (restart) net_host_stop();
    char err[200];
    if (net_host_start(err, sizeof err)) say(err, 0);
}

void online_host_boot(void) {
    if (!O.lock) O.lock = SDL_CreateMutex();
    if (net_enabled() && net_host_enabled() && net_host_available()) host_apply(false);
}

// The server's state, read again once a second (each read tries the port)
static NetHostState host_state(char *detail, size_t cap) {
    static NetHostState st;
    static char last[200];
    static uint64_t at;
    uint64_t now = SDL_GetTicks();
    if (!at || now - at >= 1000) {
        at = now;
        st = net_host_state(last, sizeof last);
        if (net_host_enabled() && (st == NET_HOST_RUNNING || st == NET_HOST_STARTING) && net_host_listen_changed())
            host_apply(true);   // Tailscale came up or went down, the box gave another address: listen there
    }
    snprintf(detail, cap, "%s", last);
    return st;
}

static int mode_now(void) { return !net_enabled() ? MODE_OFF : net_host_enabled() ? MODE_HOST : MODE_JOIN; }

static void set_mode(int mode) {
    int was = mode_now();
    cfg_set("MKW_ONLINE", mode == MODE_OFF ? "0" : "1");
    cfg_set("MKW_SERVER_HOST", mode == MODE_HOST ? "1" : "0");
    cfg_save();
    say("", -1);
    if (mode == MODE_HOST) {
        host_apply(false);
    } else {
        net_host_stop();
        if (mode == MODE_JOIN && was == MODE_HOST) {   // hosting set this PC's own address: the friend's goes there
            O.server[0] = 0;
            save_server();
        }
    }
    if (mode != MODE_OFF && O.server[0]) start(JOB_REFRESH);
}

// ---- who the player is: the name the others see, the friend code to give
static void me_section(void) {
    char text[200];
    const char *name = launcher_player_name();
    snprintf(text, sizeof text, menu_text(UI_ONLINE_PROFILE), name);
    ImGui::TextUnformatted(text);
    if (!strcmp(name, PLAYER_DEFAULT_NAME)) wrapped(menu_text(UI_ONLINE_PROFILE_DEFAULT), kGold);
    if (!O.code[0] || !net_enabled()) return;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(menu_text(UI_ONLINE_MY_CODE));
    ImGui::SameLine();
    ImGui::TextColored(kGold, "%s", O.code);
    ImGui::SameLine();
    if (ImGui::Button(menu_text(UI_ONLINE_COPY))) {
        ImGui::SetClipboardText(O.code);
        O.copied_ms = SDL_GetTicks();
    }
    if (O.copied_ms && SDL_GetTicks() - O.copied_ms < 2000) {
        ImGui::SameLine();
        ImGui::TextColored(kGreen, "%s", menu_text(UI_ONLINE_COPIED));
    }
    wrapped(menu_text(UI_ONLINE_CODE_NOTE), kGrey);
}

// ---- hosting: the server's state, the address to give, the network, the firewall
static void host_section(float s) {
    static const char *const kNet[] = {"auto", "home", "tailscale"};
    static const UiTextId kName[] = {UI_ONLINE_NET_AUTO, UI_ONLINE_NET_HOME, UI_ONLINE_NET_TS};
    static NetAddrs A;
    static uint64_t at;
    if (!at || SDL_GetTicks() - at >= 3000) at = SDL_GetTicks(), net_addrs(&A);
    wrapped(menu_text(UI_ONLINE_HOST_DESC), kGrey);
    char detail[200], text[320];
    NetHostState st = host_state(detail, sizeof detail);
    switch (st) {
    case NET_HOST_RUNNING: snprintf(text, sizeof text, menu_text(UI_ONLINE_HOST_RUNNING), detail); break;
    case NET_HOST_STARTING: snprintf(text, sizeof text, "%s", menu_text(UI_ONLINE_HOST_STARTING)); break;
    case NET_HOST_FAILED: snprintf(text, sizeof text, menu_text(UI_ONLINE_HOST_FAILED), detail); break;
    case NET_HOST_OTHER: snprintf(text, sizeof text, "%s", menu_text(UI_ONLINE_HOST_OTHER)); break;
    default: snprintf(text, sizeof text, "%s", menu_text(UI_ONLINE_HOST_OFF)); break;
    }
    wrapped(text, st == NET_HOST_RUNNING ? kGreen : st == NET_HOST_FAILED ? kRed : kGrey);
    if (st == NET_HOST_RUNNING || st == NET_HOST_STARTING || st == NET_HOST_OTHER) {   // what the friends write
        const char *n = net_network();
        if (A.home[0] && strcmp(n, "tailscale")) {
            snprintf(text, sizeof text, menu_text(UI_ONLINE_HOST_GIVE_HOME), A.home, NET_HOST_PORT);
            ImGui::TextColored(kGold, "%s", text);
        }
        if (A.ts[0] && strcmp(n, "home")) {
            snprintf(text, sizeof text, menu_text(UI_ONLINE_HOST_GIVE_TS), A.ts, NET_HOST_PORT);
            ImGui::TextColored(kGold, "%s", text);
        }
    }
    ImGui::PushID("host");
    if (st == NET_HOST_RUNNING || st == NET_HOST_STARTING) {
        if (ImGui::Button(menu_text(UI_ONLINE_HOST_STOP))) net_host_stop();
    } else {
        ImGui::BeginDisabled(st == NET_HOST_OTHER);
        if (ImGui::Button(menu_text(UI_ONLINE_HOST_START))) host_apply(false);
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    if (ImGui::Button(menu_text(UI_ONLINE_HOST_FIREWALL))) net_host_firewall();
    ImGui::PopID();
    wrapped(menu_text(UI_ONLINE_HOST_FIREWALL_DESC), kGrey);
    int cur = 0;
    for (int i = 0; i < 3; i++)
        if (!strcmp(net_network(), kNet[i])) cur = i;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(menu_text(UI_ONLINE_NETWORK));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(520 * s);
    if (ImGui::BeginCombo("##network", menu_text(kName[cur]))) {
        for (int i = 0; i < 3; i++)
            if (ImGui::Selectable(menu_text(kName[i]), i == cur) && i != cur) {
                cfg_set("MKW_NETWORK", kNet[i]);
                cfg_save();
                host_apply(true);
            }
        ImGui::EndCombo();
    }
    const char *none = menu_text(UI_ONLINE_NET_NONE);
    snprintf(text, sizeof text, menu_text(UI_ONLINE_NET_ADDRS), A.home[0] ? A.home : none, A.ts[0] ? A.ts : none);
    wrapped(text, kGrey);
}

// ---- joining: the friend's address, and a check that it answers
static void join_section(float s) {
    wrapped(menu_text(UI_ONLINE_JOIN_DESC), kGrey);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(menu_text(UI_ONLINE_SERVER));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(380 * s);
    bool go = ImGui::InputTextWithHint("##server", menu_text(UI_ONLINE_SERVER_HINT), O.server, sizeof O.server,
                                       ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CharsNoBlank);
    ImGui::SameLine();
    ImGui::BeginDisabled(O.busy || !O.server[0]);
    if (ImGui::Button(menu_text(UI_ONLINE_TEST))) go = true;
    ImGui::EndDisabled();
    if (go && O.server[0] && !O.busy) {
        save_server();
        start(JOB_TEST);
    }
}

// One line per friend or request: presence, name, code, then the buttons of the list.
static void entry_row(const NetFriend &f, int i, const char *list, bool presence) {
    char code[18];
    net_format_code(f.code, code);
    ImGui::PushID(list);
    ImGui::PushID(i);
    if (presence) {
        ImGui::TextColored(f.online ? kGreen : kGrey, "%s", f.online ? "\xe2\x97\x8f" : "\xe2\x97\x8b");   // ● ○
        ImGui::SameLine();
    }
    ImGui::TextUnformatted(f.name[0] ? f.name : code);
    if (f.name[0] || presence) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s%s%s", f.name[0] ? code : "", f.name[0] && presence ? "  " : "",
                            presence ? menu_text(f.online ? UI_ONLINE_IS_ONLINE : UI_ONLINE_IS_OFFLINE) : "");
    }
    ImGui::BeginDisabled(O.busy);
    if (!strcmp(list, "received")) {
        ImGui::SameLine();
        if (ImGui::Button(menu_text(UI_ONLINE_ACCEPT))) start(JOB_ACTION, "accept", f.code);
        ImGui::SameLine();
        if (ImGui::Button(menu_text(UI_ONLINE_DECLINE))) start(JOB_ACTION, "decline", f.code);
    } else if (!strcmp(list, "sent")) {
        ImGui::SameLine();
        if (ImGui::Button(menu_text(UI_ONLINE_CANCEL))) start(JOB_ACTION, "cancel", f.code);
    } else {
        ImGui::SameLine();
        if (ImGui::SmallButton(menu_text(UI_ONLINE_REMOVE))) start(JOB_ACTION, "remove", f.code);
    }
    ImGui::EndDisabled();
    ImGui::PopID();
    ImGui::PopID();
}

static void list_section(UiTextId title, const NetFriend *v, int n, const char *list, bool presence, bool always) {
    if (!n && !always) return;
    char head[96];
    snprintf(head, sizeof head, menu_text(title), n);
    ImGui::SeparatorText(head);
    if (!n) ImGui::TextDisabled("%s", menu_text(UI_ONLINE_NONE));
    for (int i = 0; i < n; i++) entry_row(v[i], i, list, presence);
}

// ---- friends: a code to add, requests to answer, the friends with their presence
static void friends_section(float s) {
    ImGui::SeparatorText(menu_text(UI_ONLINE_ADD));
    ImGui::SetNextItemWidth(260 * s);
    bool go = ImGui::InputTextWithHint("##add", "SW-1234-5678-9012", O.add, sizeof O.add,
                                       ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    ImGui::BeginDisabled(O.busy || !O.add[0]);
    if (ImGui::Button(menu_text(UI_ONLINE_SEND))) go = true;
    ImGui::EndDisabled();
    if (go && O.add[0] && !O.busy) {
        start(JOB_ACTION, "request", O.add);
        O.add[0] = 0;
    }
    static NetFriendLists L;   // a copy: the buttons start jobs that take the lock
    SDL_LockMutex(O.lock);
    int have = O.have_lists;
    if (have) L = O.lists;
    SDL_UnlockMutex(O.lock);
    if (have) {
        list_section(UI_ONLINE_RECEIVED, L.received, L.nreceived, "received", false, false);
        list_section(UI_ONLINE_FRIENDS, L.friends, L.nfriends, "friends", true, true);
        list_section(UI_ONLINE_SENT, L.sent, L.nsent, "sent", false, false);
    }
    ImGui::Spacing();
    ImGui::BeginDisabled(O.busy);
    if (ImGui::Button(menu_text(UI_ONLINE_REFRESH))) start(JOB_REFRESH);
    ImGui::EndDisabled();
    if (O.busy) {
        ImGui::SameLine();
        ImGui::TextDisabled("...");
    }
}

// ---- LAN play between PCs of a tailnet: Tailscale carries no broadcast, so each lists the others
static void lan_section(float s) {
    ImGui::PushID("lan");
    if (ImGui::CollapsingHeader(menu_text(UI_ONLINE_LAN))) {
        bool lan = cfg_bool("MKW_LAN") != 0;
        if (ImGui::Checkbox(menu_text(UI_ONLINE_LAN_ENABLE), &lan)) {
            cfg_set("MKW_LAN", lan ? "1" : "0");
            cfg_save();
        }
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(menu_text(UI_ONLINE_LAN_PEERS));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(380 * s);
        bool save_peers = ImGui::InputTextWithHint("##peers", menu_text(UI_ONLINE_LAN_PEERS_HINT), O.peers,
                                                   sizeof O.peers, ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if (ImGui::Button(menu_text(UI_ONLINE_SAVE)) || save_peers) {
            cfg_set("MKW_LAN_PEERS", O.peers);
            if (O.peers[0]) cfg_set("MKW_LAN", "1");   // listing PCs means wanting LAN play with them
            cfg_save();
        }
        wrapped(menu_text(UI_ONLINE_LAN_DESC), kGrey);
    }
    ImGui::PopID();
}

void online_page(float scale, float bottom) {
    if (!O.lock) online_opened();
    float s = scale;
    ImGui::BeginChild("##online_body", ImVec2(0, -bottom), ImGuiChildFlags_None);

    me_section();

    ImGui::SeparatorText(menu_text(UI_ONLINE_ENABLE));   // how the player plays online
    static const UiTextId kMode[] = {UI_ONLINE_MODE_OFF, UI_ONLINE_MODE_HOST, UI_ONLINE_MODE_JOIN};
    int mode = mode_now();
    for (int i = 0; i < 3; i++)
        if (ImGui::RadioButton(menu_text(kMode[i]), mode == i) && mode != i) set_mode(mode = i);
    ImGui::Spacing();
    if (mode == MODE_OFF) wrapped(menu_text(UI_ONLINE_ENABLE_DESC), kGrey);
    else if (mode == MODE_HOST) host_section(s);
    else join_section(s);
    SDL_LockMutex(O.lock);
    if (mode != MODE_OFF && O.status[0])
        wrapped(O.status, O.status_ok == 1 ? kGreen : O.status_ok == 0 ? kRed : kGrey);
    SDL_UnlockMutex(O.lock);

    if (mode != MODE_OFF && O.code[0]) friends_section(s);

    ImGui::Spacing();
    lan_section(s);
    if (ImGui::CollapsingHeader(menu_text(UI_ONLINE_HOWTO))) {
        ImGui::TextWrapped("%s", menu_text(UI_ONLINE_HOWTO_TEXT));
        ImGui::Spacing();
        wrapped(menu_text(UI_ONLINE_FIREWALL), kGrey);
    }
    ImGui::EndChild();
}
