// The start screen's players: the Profiles page (each player's name, which one plays next, a new one, removing one)
// and the name the main page's profile row shows. Discussion #288 asked for a name other than "Local"; a player is a
// console user here, with its own save (core/base/players.h).
//
// Test hooks (with MKW_LAUNCHER_SHOT=<frame>:players): MKW_LAUNCHER_PLAYER_NEW=<name> makes that player once, as the
// New profile button does; MKW_LAUNCHER_PLAYER_PICK=<id> picks it once, as its Play as button does.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "imgui.h"
#include "ui/launcher/launcher_look.h"
#include "ui/launcher/launcher_players.h"

extern "C" {
#include "cfg.h"
#include "platform/platform.h"
#include "players.h"
#include "ui_text.h"
void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
}

static struct {
    char base[PLAT_PATH_MAX * 2];
    Player list[PLAYERS_MAX];
    char edit[PLAYERS_MAX][PLAYER_NAME_BYTES];   // the name fields, as typed
    int n;                                       // 0: not listed yet
    char fresh[PLAYER_NAME_BYTES];               // the new player's name field
    char removing[sizeof(Player::id)];           // the player the confirmation asks about
    int failed;                                  // the last write failed: said under the list
} P = {};

static const char *active_id(void) {
    const char *id = cfg_get("MKW_PLAYER");
    return id ? id : "";
}

static void refresh(void) {
    if (!P.base[0]) return;
    P.n = players_list(P.base, P.list, PLAYERS_MAX);
    for (int i = 0; i < P.n; i++) snprintf(P.edit[i], sizeof P.edit[i], "%s", P.list[i].name);
}

static void pick(const char *id) {
    cfg_set("MKW_PLAYER", id);
    cfg_save();
    rt_log("players: the next start plays player '%s'", id);
}

void launcher_players_set_base(const char *base) {
    snprintf(P.base, sizeof P.base, "%s", base);
    refresh();
}

void launcher_player_dir(char *out, size_t cap) {
    if (!P.base[0]) {
        if (cap) out[0] = 0;
        return;
    }
    player_active_dir(P.base, out, cap);
}

// The player the next start plays, as player_active_dir resolves it: MKW_PLAYER without a folder means the first one.
static const Player *active(void) {
    if (!P.n) refresh();
    const char *id = active_id();
    for (int i = 0; i < P.n; i++)
        if (!strcmp(P.list[i].id, id)) return &P.list[i];
    return P.n ? &P.list[0] : nullptr;
}

const char *launcher_player_name(void) {
    const Player *p = active();
    return p ? p->name : PLAYER_DEFAULT_NAME;
}

static void remove_modal(void) {
    if (!ImGui::BeginPopupModal("##player_remove", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    const char *name = "";
    for (int i = 0; i < P.n; i++)
        if (!strcmp(P.list[i].id, P.removing)) name = P.list[i].name;
    char text[512];
    snprintf(text, sizeof text, menu_text(UI_PLAYER_CONFIRM), name);
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    if (ImGui::Button(menu_text(UI_PLAYER_REMOVE))) {
        P.failed = !player_remove(P.base, P.removing);
        if (!P.failed && !strcmp(active_id(), P.removing)) pick("");
        refresh();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(menu_text(UI_LAUNCH_BACK))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

static void player_row(int i, float scale) {
    const Player &p = P.list[i];
    const ImGuiStyle &st = ImGui::GetStyle();
    bool on = active() == &p;
    ImGui::PushID(i);
    const char *play = menu_text(on ? UI_PLAYER_CHOSEN : UI_PLAYER_CHOOSE), *rm = menu_text(UI_PLAYER_REMOVE);
    float play_w = fmaxf(ImGui::CalcTextSize(menu_text(UI_PLAYER_CHOSEN)).x,   // one width: Remove stays in a column
                         ImGui::CalcTextSize(menu_text(UI_PLAYER_CHOOSE)).x) + st.FramePadding.x * 4;
    float rm_w = ImGui::CalcTextSize(rm).x + st.FramePadding.x * 4;
    float field_w = fminf(520 * scale, ImGui::GetContentRegionAvail().x - play_w - rm_w - st.ItemSpacing.x * 2);
    ImGui::SetNextItemWidth(field_w);
    ImGui::InputTextWithHint("##name", menu_text(UI_PLAYER_NAME_HINT), P.edit[i], sizeof P.edit[i]);
    if (ImGui::IsItemDeactivatedAfterEdit()) {   // written when the field is left: one file write per edit
        P.failed = !player_rename(P.base, p.id, P.edit[i]);
        refresh();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(on);
    if (ImGui::Button(play, ImVec2(play_w, 0))) pick(p.id);
    ImGui::EndDisabled();
    if (p.id[0]) {   // the first player is the profile folder itself: it stays
        ImGui::SameLine();
        if (ImGui::Button(rm, ImVec2(rm_w, 0))) {
            snprintf(P.removing, sizeof P.removing, "%s", p.id);
            ImGui::OpenPopup("##player_remove");
        }
    }
    remove_modal();
    ImGui::PopID();
}

void launcher_players_page(float scale, float bottom) {
    if (!P.n) refresh();
    static int tested = 0;   // (test) the hooks run once
    if (!tested) {
        tested = 1;
        char id[sizeof(Player::id)];
        if (const char *t = getenv("MKW_LAUNCHER_PLAYER_NEW"); t && player_create(P.base, t, id, sizeof id)) {
            pick(id);
            refresh();
        }
        if (const char *t = getenv("MKW_LAUNCHER_PLAYER_PICK")) pick(t);
    }
    ImGui::BeginChild("##players_body", ImVec2(0, -bottom), ImGuiChildFlags_NavFlattened);
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(LOOK_DIM), "%s", menu_text(UI_PLAYER_NOTE));
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0, 8 * scale));
    for (int i = 0; i < P.n; i++) player_row(i, scale);
    ImGui::Dummy(ImVec2(0, 12 * scale));
    if (P.n < PLAYERS_MAX) {
        ImGui::SetNextItemWidth(fminf(520 * scale, ImGui::GetContentRegionAvail().x * 0.6f));
        bool enter = ImGui::InputTextWithHint("##new", menu_text(UI_PLAYER_NAME_HINT), P.fresh, sizeof P.fresh,
                                              ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        char clean[PLAYER_NAME_BYTES];
        player_name_clean(P.fresh, clean);
        ImGui::BeginDisabled(!clean[0]);
        if ((ImGui::Button(menu_text(UI_PLAYER_NEW)) || enter) && clean[0]) {
            char id[sizeof(Player::id)];
            P.failed = !player_create(P.base, clean, id, sizeof id);
            if (!P.failed) pick(id), P.fresh[0] = 0;   // a new player is made to play
            refresh();
        }
        ImGui::EndDisabled();
    }
    if (P.failed) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(LOOK_BAD), "%s", menu_text(UI_PLAYER_FAILED));
    ImGui::EndChild();
}
