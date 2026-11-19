// A mod's page drawn with ImGui (mod_page_ui.h): mkw_plugin.h's MkwPage, for the start screen's options page
// (ui/launcher/mod_page.cpp) and the mods' tabs in the in-game menu's settings (ui/menu/settings_tabs.cpp).
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "imgui.h"

#include "mkw_plugin.h"
#include "mod_page_ui.h"
#include "mod_settings.h"

extern "C" {
#include "ui_text.h"
}

extern "C" void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
extern const char rt_port_version[];

static ModPageUi *volatile U;   // the page being drawn (and the last one drawn: page_setting_* from other threads)

// The start screen's palette (launcher_look.h), here too so the menu's tabs look the same
#define PAGE_HEADING IM_COL32(169, 177, 189, 255)
#define PAGE_DIM IM_COL32(141, 150, 164, 255)
#define PAGE_INFO IM_COL32(62, 118, 232, 255)
#define PAGE_OK IM_COL32(91, 201, 138, 255)
#define PAGE_WARN IM_COL32(230, 166, 74, 255)
#define PAGE_BAD IM_COL32(240, 118, 107, 255)

// ---- the entries that are not ImGui's (any thread)
static void page_log(const char *fmt, ...) {
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    ModPageUi *u = U;
    rt_log("mod %s: %s", u ? u->mod : "?", line);
}

// From other threads these are the last page drawn's: one page at a time in the start screen; in the game a plugin
// uses MkwPluginApi.setting_get / setting_set off the menu's thread.
static int page_setting_get(const char *key, char *out, size_t cap) {
    ModPageUi *u = U;
    return u ? mod_settings_get(u->player_dir, u->mod, key, out, cap) : 0;
}

static int page_setting_set(const char *key, const char *value) {
    ModPageUi *u = U;
    return u ? mod_settings_set(u->player_dir, u->mod, key, value) : 0;
}

static void page_redraw(void) {
    // the start screen and the menu draw every frame already; an event wakes them in case they ever wait for input
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_EVENT_USER;
    SDL_PushEvent(&e);
}

// ---- text and layout
static ImVec4 col4(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

static void page_heading(const char *text) {
    ImGui::Spacing();
    ImGui::TextColored(col4(PAGE_HEADING), "%s", text ? text : "");
    ImGui::Separator();
}

static void page_text(const char *text) { ImGui::TextWrapped("%s", text ? text : ""); }

static void page_note(int kind, const char *text) {
    ImU32 c = kind == MKW_NOTE_INFO ? PAGE_INFO : kind == MKW_NOTE_OK ? PAGE_OK : kind == MKW_NOTE_WARN ? PAGE_WARN
            : kind == MKW_NOTE_ERROR ? PAGE_BAD : PAGE_DIM;
    ImGui::PushStyleColor(ImGuiCol_Text, col4(c));
    ImGui::TextWrapped("%s", text ? text : "");
    ImGui::PopStyleColor();
}

static void page_separator(void) { ImGui::Separator(); }
static void page_same_line(void) { ImGui::SameLine(); }

static void page_disabled_begin(int disabled) {
    ImGui::BeginDisabled(disabled != 0);   // nested: disabled while any enclosing section is
    U->disabled++;
}

static void page_disabled_end(void) {
    if (!U->disabled) return;   // an end without its begin: ignored
    ImGui::EndDisabled();
    U->disabled--;
}

static void page_busy(const char *text) {
    float h = ImGui::GetFrameHeight();
    ImGui::ProgressBar(-1.0f * (float)ImGui::GetTime(), ImVec2(h * 3, h * 0.4f), "");
    ImGui::SameLine();
    ImGui::TextUnformatted(text ? text : "");
}

// ---- widgets bound to a setting
static int page_checkbox(const char *key, const char *label, int default_on) {
    char v[8];
    bool on = page_setting_get(key, v, sizeof v) ? !strcmp(v, "1") : default_on != 0;
    ImGui::PushID(key);
    bool changed = ImGui::Checkbox(label ? label : key, &on);
    ImGui::PopID();
    if (changed && !page_setting_set(key, on ? "1" : "0")) page_log("setting %s not stored", key);
    return changed;
}

static decltype(&U->inputs[0]) input_of(const char *key) {
    for (int i = 0; i < U->ninputs; i++)
        if (!strcmp(U->inputs[i].key, key)) return &U->inputs[i];
    if (U->ninputs == MOD_PAGE_INPUTS) return nullptr;
    auto *in = &U->inputs[U->ninputs++];
    snprintf(in->key, sizeof in->key, "%s", key);
    in->buf[0] = 0;
    in->editing = 0;
    return in;
}

static int digits_only(ImGuiInputTextCallbackData *d) { return !(d->EventChar >= '0' && d->EventChar <= '9'); }

static int page_input(const char *key, const char *label, const char *hint, int max_bytes, int flags) {
    if (!mod_settings_key_ok(key)) return 0;
    auto *in = input_of(key);
    if (!in) {
        page_log("more than %d text fields: %s not shown", MOD_PAGE_INPUTS, key);
        return 0;
    }
    if (max_bytes <= 0 || max_bytes > MOD_PAGE_INPUT_CAP - 1) max_bytes = MOD_PAGE_INPUT_CAP - 1;
    ImGui::PushID(key);
    if (!in->editing) {   // not being edited: show the stored value (a thread may have changed it)
        if (!page_setting_get(key, in->buf, sizeof in->buf)) in->buf[0] = 0;
    }
    if (label && *label) ImGui::TextUnformatted(label);
    ImGuiInputTextFlags f = ImGuiInputTextFlags_None;
    if (flags & MKW_INPUT_PASSWORD) f |= ImGuiInputTextFlags_Password;
    if (flags & MKW_INPUT_DIGITS) f |= ImGuiInputTextFlags_CallbackCharFilter;
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##field", hint ? hint : "", in->buf, (size_t)max_bytes + 1, f,
                             (flags & MKW_INPUT_DIGITS) ? digits_only : nullptr);
    in->editing = ImGui::IsItemActive();
    int changed = 0;
    if (ImGui::IsItemDeactivatedAfterEdit()) {   // Enter, a click away, the cursor moving on
        changed = 1;
        if (!page_setting_set(key, in->buf[0] ? in->buf : nullptr)) page_log("setting %s not stored", key);
    }
    ImGui::PopID();
    return changed;
}

static int page_choice(const char *key, const char *label, const char *const *values, const char *const *labels,
                       int n) {
    if (!values || n <= 0) return 0;
    char v[MOD_SETTING_VALUE_MAX + 1];
    int cur = 0;
    if (page_setting_get(key, v, sizeof v))
        for (int i = 0; i < n; i++)
            if (values[i] && !strcmp(values[i], v)) cur = i;
    auto shown = [&](int i) { return labels && labels[i] ? labels[i] : values[i] ? values[i] : ""; };
    int changed = 0;
    ImGui::PushID(key);
    if (label && *label) ImGui::TextUnformatted(label);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##choice", shown(cur))) {
        for (int i = 0; i < n; i++) {
            if (ImGui::Selectable(shown(i), i == cur) && i != cur && values[i]) {
                changed = page_setting_set(key, values[i]);
                if (!changed) page_log("setting %s not stored", key);
            }
            if (i == cur) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::PopID();
    return changed;
}

// ---- actions
static int page_button(const char *label) { return ImGui::Button(label ? label : "?"); }

// ---- lists
static void page_list_begin(const char *id, int visible_rows) {
    if (U->in_list) return;   // no nesting: the rows go into the open list
    if (visible_rows < 1) visible_rows = 1;
    float row = ImGui::GetFrameHeightWithSpacing();
    ImGui::PushID(id ? id : "list");
    ImGui::BeginChild("##list", ImVec2(0, row * (float)visible_rows + ImGui::GetStyle().WindowPadding.y * 2),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened);
    U->in_list = 1;
    U->list_rows = 0;
    U->list_empty[0] = 0;
}

static void page_list_empty(const char *text) { snprintf(U->list_empty, sizeof U->list_empty, "%s", text ? text : ""); }

static int page_list_row(const char *name, const char *detail, const char *const *buttons, int nbuttons) {
    if (nbuttons < 0) nbuttons = 0;
    if (nbuttons > 4) nbuttons = 4;
    ImGui::PushID(U->list_rows++);
    ImGui::PushID(name ? name : "");
    float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;   // the row's right edge
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(name ? name : "");
    if (detail && *detail) {
        ImGui::SameLine();
        ImGui::TextColored(col4(PAGE_DIM), "%s", detail);
    }
    int pressed = 0;
    if (nbuttons) {
        const ImGuiStyle &st = ImGui::GetStyle();
        float w = 0;
        for (int i = 0; i < nbuttons; i++)
            w += ImGui::CalcTextSize(buttons[i] ? buttons[i] : "?").x + st.FramePadding.x * 2 + (i ? st.ItemSpacing.x : 0);
        ImGui::SameLine();
        ImGui::SetCursorPosX(fmaxf(ImGui::GetCursorPosX(), right - w));
        for (int i = 0; i < nbuttons; i++) {
            if (i) ImGui::SameLine();
            ImGui::PushID(i);
            if (ImGui::SmallButton(buttons[i] ? buttons[i] : "?")) pressed = i + 1;
            ImGui::PopID();
        }
    }
    ImGui::PopID();
    ImGui::PopID();
    return pressed;
}

static void page_list_end(void) {
    if (!U->in_list) return;
    if (!U->list_rows && U->list_empty[0]) ImGui::TextColored(col4(PAGE_DIM), "%s", U->list_empty);
    ImGui::EndChild();
    ImGui::PopID();
    U->in_list = 0;
}


static int page_slider(const char *key, const char *label, int min, int max, int default_value) {
    if (!mod_settings_key_ok(key) || max <= min) return 0;
    char v[16];
    int value = page_setting_get(key, v, sizeof v) ? atoi(v) : default_value;
    if (value < min) value = min;
    if (value > max) value = max;
    ImGui::PushID(key);
    if (label && *label) ImGui::TextUnformatted(label);
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::SliderInt("##slider", &value, min, max);
    int changed = ImGui::IsItemDeactivatedAfterEdit();   // stored when the player lets go, not at each step
    if (changed) {
        snprintf(v, sizeof v, "%d", value);
        if (!page_setting_set(key, v)) page_log("setting %s not stored", key);
    }
    ImGui::PopID();
    return changed;
}

// ---- columns
static void page_columns_begin(int n) {
    if (U->in_columns || U->in_list) return;   // no nesting
    if (n < 1) n = 1;
    if (n > 4) n = 4;
    if (!ImGui::BeginTable("##columns", n, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_PadOuterX)) return;
    ImGui::TableNextColumn();
    U->in_columns = 1;
}

static void page_column_next(void) {
    if (U->in_columns && !U->in_list) ImGui::TableNextColumn();
}

static void page_columns_end(void) {
    if (!U->in_columns) return;
    if (U->in_list) page_list_end();
    ImGui::EndTable();
    U->in_columns = 0;
}


extern "C" void mod_page_ui_draw(ModPageUi *ui, MkwModPage fn, float scale) {
    if (!ui || !fn) return;
    // the player may change between two frames (the Profiles page, another start): the settings are theirs
    int opened = ui->reopen || strcmp(ui->shown_for, ui->player_dir) != 0;
    if (opened) {
        snprintf(ui->shown_for, sizeof ui->shown_for, "%s", ui->player_dir);
        ui->ninputs = 0;
        ui->reopen = 0;
    }
    U = ui;
    MkwPage *p = &ui->page;
    *p = MkwPage{};
    p->size = sizeof(MkwPage);
    p->version = MKW_PLUGIN_API_VERSION;
    p->port_version = rt_port_version;
    p->mod_dir = ui->mod_dir;
    p->mod_name = ui->mod;
    p->language = menu_lang();
    p->profile_name = ui->player_name;
    p->scale = scale;
    p->opened = opened;
    p->in_game = ui->in_game;
    p->log = page_log;
    p->setting_get = page_setting_get;
    p->setting_set = page_setting_set;
    p->redraw = page_redraw;
    p->heading = page_heading;
    p->text = page_text;
    p->note = page_note;
    p->separator = page_separator;
    p->same_line = page_same_line;
    p->disabled_begin = page_disabled_begin;
    p->disabled_end = page_disabled_end;
    p->busy = page_busy;
    p->checkbox = page_checkbox;
    p->input = page_input;
    p->choice = page_choice;
    p->button = page_button;
    p->list_begin = page_list_begin;
    p->list_row = page_list_row;
    p->list_end = page_list_end;
    p->list_empty = page_list_empty;
    p->columns_begin = page_columns_begin;
    p->column_next = page_column_next;
    p->columns_end = page_columns_end;
    p->slider = page_slider;
    ImGui::PushID(ui->mod);
    ImGui::PushTextWrapPos(0);
    fn(p);
    // whatever the plugin left open closes here, so one page's mistake cannot break the frame around it
    if (ui->in_list) page_list_end();
    if (ui->in_columns) page_columns_end();
    while (ui->disabled) page_disabled_end();
    ImGui::PopTextWrapPos();
    ImGui::PopID();
}
