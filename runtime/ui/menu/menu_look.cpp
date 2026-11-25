// The look the in-game menu (ui/menu/menu.cpp) and the start screen (ui/launcher/launcher.cpp) share, split from menu.cpp. The colours
// follow SavageWindRecomp's runtime/ui/menu.mm (https://github.com/Revivedx/SavageWindRecomp @
// 575ad88e02f9a89a9a885d90039d48af33c982f4, MIT, Copyright (c) 2026 Revivedx and the Savage Wind Recomp contributors,
// Copyright (c) 2026 Peiikai).
#include <stdlib.h>
#include <string.h>

#include "ui/menu/menu_look.h"

extern "C" {
#include "platform/platform.h"
#include "ui_text.h"
void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
}

// Roboto Medium (Apache-2.0, ui/fonts/LICENSE-Roboto.txt) with Latin and Cyrillic glyphs.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"   // #embed in C++
// The path is relative to this file: #embed does not search the -I directories, and "ui/fonts/..." embedded nothing
// after the move to ui/menu/ (23c2d01): the button icons parsed an empty font and every run fell at the title.
static const unsigned char roboto_ttf[] = {
#embed "../fonts/Roboto-Medium.ttf"
};
#pragma clang diagnostic pop
static_assert(sizeof roboto_ttf > 100000, "Roboto-Medium.ttf not embedded");

extern "C" const unsigned char *menu_look_font_data(void) { return roboto_ttf; }   // ui/glyphs/glyphs.c

float menu_look_scale(uint32_t h) { return (h >= 720 ? h : 720) / 1080.0f; }

// SavageWindRecomp's colours: dark green panels, gold accents.
void menu_look_style(ImGuiStyle &s) {
    ImGui::StyleColorsDark(&s);
    s.WindowRounding = 14; s.FrameRounding = 8; s.GrabRounding = 8; s.TabRounding = 8; s.ChildRounding = 10;
    s.WindowPadding = ImVec2(28, 22); s.FramePadding = ImVec2(14, 8); s.ItemSpacing = ImVec2(14, 12);
    s.WindowBorderSize = 0; s.ScrollbarSize = 18;
    ImVec4 *c = s.Colors;
    ImVec4 gold(0.93f, 0.76f, 0.33f, 1), green(0.15f, 0.36f, 0.25f, 1);
    c[ImGuiCol_WindowBg] = ImVec4(0.06f, 0.09f, 0.08f, 0.94f);
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_Text] = ImVec4(0.96f, 0.94f, 0.88f, 1);
    c[ImGuiCol_TextDisabled] = ImVec4(0.62f, 0.64f, 0.58f, 1);
    c[ImGuiCol_Button] = ImVec4(0.14f, 0.22f, 0.18f, 1);
    c[ImGuiCol_ButtonHovered] = green;
    c[ImGuiCol_ButtonActive] = ImVec4(0.21f, 0.50f, 0.34f, 1);
    c[ImGuiCol_Tab] = ImVec4(0.12f, 0.18f, 0.15f, 1);
    c[ImGuiCol_TabHovered] = green;
    c[ImGuiCol_TabSelected] = ImVec4(0.21f, 0.44f, 0.31f, 1);
    c[ImGuiCol_TabSelectedOverline] = gold;
    c[ImGuiCol_NavCursor] = gold;
    c[ImGuiCol_Separator] = ImVec4(0.30f, 0.36f, 0.30f, 1);
}

// Roboto Medium (Latin with Latin Extended-A for Polish, Turkish and Catalan, Vietnamese's horned O and U and its
// letters of Latin Extended Additional, Cyrillic, dashes and quotes) at 26 px
// per 1080 lines, with the glyphs of the Japanese, Korean and Chinese language names merged in from a system font when
// there is one (each language is named in its own script).
static const ImWchar kRanges[] = {0x0020, 0x017F, 0x01A0, 0x01B0, 0x0400, 0x052F, 0x1EA0, 0x1EF9, 0x2010, 0x2027, 0};
static const struct { const char *lang; UiTextId name; } kCjk[] = {
    {"ja", UI_MENU_LANGNAME_JA}, {"ko", UI_MENU_LANGNAME_KO}, {"zh-Hans", UI_MENU_LANGNAME_ZH_HANS},
    {"zh-Hant", UI_MENU_LANGNAME_ZH_HANT}, {"th", UI_MENU_LANGNAME_TH},
};

static const char *g_font_lang;                        // the menu language the atlas was built for
static ImVector<ImWchar> g_ranges[sizeof kCjk / sizeof *kCjk];   // read when the texture is built, after this returns

int menu_look_fonts_stale(void) { return g_font_lang != menu_lang(); }

// A CJK menu language takes the glyphs of its whole catalog from the system font; the other rows only the names of
// the CJK languages in the language lists. Without the system font the CJK menu shows boxes.
void menu_look_fonts(float scale) {
    enum { N = sizeof kCjk / sizeof *kCjk };
    g_font_lang = menu_lang();
    ImFontAtlas *fa = ImGui::GetIO().Fonts;
    fa->Clear();
    ImFontConfig fc;
    fc.FontDataOwnedByAtlas = false;
    fa->AddFontFromMemoryTTF((void *)roboto_ttf, (int)sizeof roboto_ttf, 26.0f * scale, &fc, kRanges);
    int merged = 0;
    for (int i = 0; i < N; i++) {
        char path[512];
        uint64_t size = 0;
        uint8_t *data;
        if (!plat_font_path(kCjk[i].lang, path, sizeof path) || !(data = plat_file_read(path, &size))) continue;
        merged++;
        ImFontGlyphRangesBuilder b;
        b.AddText(ui_text_in("en", kCjk[i].name));
        if (!strcmp(kCjk[i].lang, g_font_lang))
            for (int id = 0; id < UI_TEXT_COUNT; id++) b.AddText(ui_text_in(g_font_lang, (UiTextId)id));
        g_ranges[i].clear();
        b.BuildRanges(&g_ranges[i]);
        ImFontConfig mc;
        mc.MergeMode = true;   // the atlas frees data (IM_FREE is free by default)
        fa->AddFontFromMemoryTTF(data, (int)size, 26.0f * scale, &mc, g_ranges[i].Data);
    }
    rt_log("menu: font atlas at %.1f px for menu language %s, CJK glyphs from %d of %d system fonts", 26.0f * scale,
           g_font_lang, merged, (int)N);
}

// The launcher's other sizes: Roboto, with the system font of a CJK or Thai menu language merged in like the main
// font, or its titles and buttons show "?" for every glyph (discussion #193). Call after menu_look_fonts.
ImFont *menu_look_font_at(float px) {
    ImFontAtlas *fa = ImGui::GetIO().Fonts;
    ImFontConfig fc;
    fc.FontDataOwnedByAtlas = false;
    ImFont *font = fa->AddFontFromMemoryTTF((void *)roboto_ttf, (int)sizeof roboto_ttf, px, &fc, kRanges);
    for (size_t i = 0; i < sizeof kCjk / sizeof *kCjk; i++) {
        char path[512];
        uint64_t size = 0;
        uint8_t *data;
        if (!g_font_lang || strcmp(kCjk[i].lang, g_font_lang) || g_ranges[i].empty() ||
            !plat_font_path(kCjk[i].lang, path, sizeof path) || !(data = plat_file_read(path, &size)))
            continue;
        ImFontConfig mc;
        mc.MergeMode = true;   // the atlas frees data
        fa->AddFontFromMemoryTTF(data, (int)size, px, &mc, g_ranges[i].Data);
    }
    return font;
}
