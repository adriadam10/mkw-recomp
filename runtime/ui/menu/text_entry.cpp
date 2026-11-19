// The overlay's numeric keypad (text_entry.h). Why: on the Switch a digits-only request of nn::swkbd (a code, a number)
// opens the keyboard applet's number pad, which a controller works; here the request used to take its starting text
// at once (swkbd_sdl.c), so the player could not type a number at all.
//
// The guest thread that called ShowKeyboard posts the request and sleeps on g_cond (its virtual core given up by
// swkbd.c); the present path lays the keypad out in the menu overlay's ImGui frame (menu.cpp gives it the window's
// input and the gamepad while it is up) and hands the answer back. A request no frame picks up within 3 s (no window:
// probes, a headless run) is withdrawn and the caller answers it as before.
//
// Controls: D-pad or stick and A on the keys; B deletes a digit, or cancels when there is none; Plus confirms. Digit
// keys, Backspace, Enter and Esc on the keyboard; the mouse on the keys.
#include <atomic>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "imgui.h"

extern "C" {
#include "cfg.h"
#include "input.h"
void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));   // host.h is C only
#include "gfx/nvn/host/ui_text.h"
#include "platform/platform.h"
#include "ui/menu/text_entry.h"
}

enum { MAX_DIGITS = 500, ST_NONE = 0, ST_WAITING, ST_DONE };
enum { ACT_NONE = -1, ACT_DELETE = 10, ACT_OK, ACT_CANCEL };

static PlatMutex g_lock = PLAT_MUTEX_INIT;
static PlatCond g_cond;
static int g_cond_ok;
static std::atomic<int> g_state;    // ST_*; the request's fields below belong to the present path while ST_WAITING
static std::atomic<int> g_drawn;    // a frame laid the request out
static std::atomic<int> g_opened;   // went up since text_entry_opened last asked
static int g_focus;                 // the first layout puts the cursor on a key
static int g_result;

static char g_prompt[0x101 * 3 + 1];
static char g_digits[MAX_DIGITS + 1];
static int g_n, g_min, g_max, g_password;

extern "C" int text_entry_keypad(const char *prompt, const char *initial, int min_len, int max_len, int password,
                                 char *out, size_t cap) {
    plat_mutex_lock(&g_lock);
    if (!g_cond_ok) plat_cond_init(&g_cond), g_cond_ok = 1;
    snprintf(g_prompt, sizeof g_prompt, "%s", prompt ? prompt : "");
    g_max = max_len < 1 || max_len > MAX_DIGITS ? MAX_DIGITS : max_len;
    if (cap && (size_t)g_max > cap - 1) g_max = (int)cap - 1;
    g_min = min_len < 0 ? 0 : min_len > g_max ? g_max : min_len;
    g_password = password;
    g_n = 0;
    for (const char *s = initial ? initial : ""; *s && g_n < g_max; s++)
        if (*s >= '0' && *s <= '9') g_digits[g_n++] = *s;
    g_digits[g_n] = 0;
    g_focus = 1;
    g_drawn.store(0);
    g_opened.store(1);
    g_state.store(ST_WAITING);
    uint64_t t0 = plat_now_ns();
    while (g_state.load() == ST_WAITING) {
        plat_cond_wait_ms(&g_cond, &g_lock, 100);
        if (g_state.load() == ST_WAITING && !g_drawn.load() && plat_now_ns() - t0 > 3000000000ull) {
            g_state.store(ST_NONE);
            plat_mutex_unlock(&g_lock);
            return -1;
        }
    }
    int r = g_result;
    if (cap) snprintf(out, cap, "%s", r ? g_digits : "");
    g_state.store(ST_NONE);
    plat_mutex_unlock(&g_lock);
    return r;
}

extern "C" int text_entry_up(void) { return g_state.load(std::memory_order_relaxed) == ST_WAITING; }
extern "C" int text_entry_opened(void) { return g_opened.exchange(0); }

// MKW_KEYPAD_TEST=<frame>: at that presented frame a thread of the runtime asks for a 6-digit code, as a game's code
// request does, and logs the answer: the pad tried with a controller (tools/run/vpad.py) without a game that asks.
static void test_main(void *arg) {
    (void)arg;
    char out[16];
    input_block(1);   // as swkbd.c around its front end: the game reads none of the pad's presses
    int r = text_entry_keypad("Test: a 6-digit code", "", 6, 6, 0, out, sizeof out);
    input_block(0);
    rt_log("keypad test: %s \"%s\"", r > 0 ? "OK" : r == 0 ? "cancelled" : "not drawn", out);
}

extern "C" void text_entry_test(uint64_t frame) {
    static uint64_t at = ~0ull;
    if (at == ~0ull) {
        const char *v = cfg_get("MKW_KEYPAD_TEST");
        at = v && *v ? strtoull(v, nullptr, 10) : 0;
    }
    if (!at || frame < at) return;
    at = 0;
    plat_thread_spawn(test_main, nullptr, "keypad-test", 0);
}

static void finish(int result) {
    plat_mutex_lock(&g_lock);
    g_result = result;
    g_state.store(ST_DONE);
    plat_cond_broadcast(&g_cond);
    plat_mutex_unlock(&g_lock);
}

// The keyboard's and the gamepad's shortcuts this frame, ACT_NONE when none.
static int shortcut(void) {
    for (int d = 0; d < 10; d++)
        if (ImGui::IsKeyPressed((ImGuiKey)(ImGuiKey_0 + d), false) ||
            ImGui::IsKeyPressed((ImGuiKey)(ImGuiKey_Keypad0 + d), false))
            return d;
    if (ImGui::IsKeyPressed(ImGuiKey_Backspace)) return ACT_DELETE;
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight)) return g_n ? ACT_DELETE : ACT_CANCEL;
    if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) ||
        ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false))
        return ACT_OK;
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) return ACT_CANCEL;
    return ACT_NONE;
}

// One key of the grid; focus puts the cursor on it (the first frame).
static bool key(const char *label, ImVec2 size, bool enabled, bool focus) {
    if (focus) {
        ImGui::SetKeyboardFocusHere();
        ImGui::SetNavCursorVisible(true);
    }
    ImGui::BeginDisabled(!enabled);
    bool hit = ImGui::Button(label, size);
    ImGui::EndDisabled();
    return hit;
}

extern "C" void text_entry_draw(float width, float height, float scale) {
    if (!text_entry_up()) return;
    g_drawn.store(1);
    bool first = g_focus != 0;
    g_focus = 0;
    ImGuiStyle &st = ImGui::GetStyle();
    // the keys fit the longest word on them ("Aceptar", "Esborra")
    float side = 76 * scale, gap = st.ItemSpacing.x;
    side = fmaxf(side, ImGui::CalcTextSize(menu_text(UI_KEYPAD_OK)).x + st.FramePadding.x * 2);
    side = fmaxf(side, ImGui::CalcTextSize(menu_text(UI_KEYPAD_DELETE)).x + st.FramePadding.x * 2);
    float inner = side * 3 + gap * 2;
    ImGui::SetNextWindowPos(ImVec2(width / 2, height / 2), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowBgAlpha(0.94f);
    if (first) ImGui::SetNextWindowFocus();
    ImGui::Begin("##keypad", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse);
    if (g_prompt[0]) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + inner);
        ImGui::TextUnformatted(g_prompt);
        ImGui::PopTextWrapPos();
    }
    char shown[MAX_DIGITS + 2];
    for (int i = 0; i < g_n; i++) shown[i] = g_password ? '*' : g_digits[i];
    shown[g_n] = g_n < g_max ? '_' : 0;
    shown[g_n + 1] = 0;
    ImGui::SetWindowFontScale(1.6f);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + inner);
    ImGui::TextUnformatted(shown);
    ImGui::PopTextWrapPos();
    ImGui::SetWindowFontScale(1.0f);

    int act = ACT_NONE;   // the keys' clicks; a shortcut wins (Enter also activates the focused key)
    bool room = g_n < g_max, enough = g_n >= g_min;
    bool focus_ok = first && enough && g_n > 0;
    ImVec2 sz(side, side * 0.8f);
    static const char *const grid[12] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", nullptr, "0", nullptr};
    for (int i = 0; i < 12; i++) {
        if (i % 3) ImGui::SameLine();
        ImGui::PushID(i);
        if (i == 9) {
            if (key(menu_text(UI_KEYPAD_DELETE), sz, g_n > 0, false)) act = ACT_DELETE;
        } else if (i == 11) {
            if (key(menu_text(UI_KEYPAD_OK), sz, enough, focus_ok)) act = ACT_OK;
        } else if (key(grid[i], sz, room, first && !focus_ok && i == 0)) {
            act = grid[i][0] - '0';
        }
        ImGui::PopID();
    }
    if (key(menu_text(UI_KEYPAD_CANCEL), ImVec2(inner, 0), true, false)) act = ACT_CANCEL;
    ImGui::End();
    if (int s = shortcut(); s != ACT_NONE) act = s;

    if (act >= 0 && act <= 9 && room) g_digits[g_n++] = (char)('0' + act), g_digits[g_n] = 0;
    else if (act == ACT_DELETE && g_n) g_digits[--g_n] = 0;
    else if (act == ACT_OK && enough) finish(1);
    else if (act == ACT_CANCEL) finish(0);
}
