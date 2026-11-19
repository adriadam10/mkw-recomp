// The plugins' drawings over the picture (mkw_plugin.h overlay_set, overlay_show), laid out in the in-game menu's
// overlay frame (ui/menu/menu.cpp) on the present thread. Drawn on ImGui's background list: under the menu, the number
// pad and the notices, which the player must always reach.
#include <atomic>
#include <stdint.h>

#include "imgui.h"

extern "C" {
#include "plugin/plugin.h"
}

enum { MAX_DRAWS = 16 };

static struct {
    MkwOverlayFn fn;
    void *user;
    std::atomic<int> shown;
} g_draws[MAX_DRAWS];
static std::atomic<int> g_n;       // registered; entries below it are complete
static std::atomic<int> g_shown;   // how many are on

extern "C" int plugin_overlay_add(MkwOverlayFn fn, void *user) {
    int n = g_n.load();
    if (!fn || n == MAX_DRAWS) return 0;
    g_draws[n].fn = fn;
    g_draws[n].user = user;
    g_n.store(n + 1);
    return 1;
}

extern "C" void plugin_overlay_show(MkwOverlayFn fn, int on) {
    for (int i = 0, n = g_n.load(); i < n; i++)
        if (g_draws[i].fn == fn && g_draws[i].shown.exchange(on != 0) != (on != 0)) g_shown.fetch_add(on ? 1 : -1);
}

extern "C" int plugins_overlay_wanted(void) { return g_shown.load(std::memory_order_relaxed) > 0; }

static ImDrawList *g_dl;
static ImU32 col(uint32_t rgba) { return IM_COL32(rgba >> 24, rgba >> 16 & 255, rgba >> 8 & 255, rgba & 255); }
static void ov_rect(float x0, float y0, float x1, float y1, uint32_t rgba, float rounding) {
    g_dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), col(rgba), rounding);
}
static void ov_text(float x, float y, uint32_t rgba, const char *utf8) {
    if (utf8) g_dl->AddText(ImVec2(x, y), col(rgba), utf8);
}
static void ov_text_size(const char *utf8, float *w, float *h) {
    ImVec2 v = ImGui::CalcTextSize(utf8 ? utf8 : "");
    if (w) *w = v.x;
    if (h) *h = v.y;
}

extern "C" void plugins_overlay_draw(float width, float height, float scale) {
    if (!plugins_overlay_wanted()) return;
    g_dl = ImGui::GetBackgroundDrawList();
    MkwOverlay o = {sizeof(MkwOverlay), width, height, scale, ImGui::GetTextLineHeight(), ov_rect, ov_text, ov_text_size};
    for (int i = 0, n = g_n.load(); i < n; i++)
        if (g_draws[i].shown.load(std::memory_order_relaxed)) g_draws[i].fn(&o, g_draws[i].user);
}
