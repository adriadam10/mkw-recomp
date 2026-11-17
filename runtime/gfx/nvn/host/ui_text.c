// Catalog of the text the host shows the player (ui_text.h). One array per catalog language, indexed by UiTextId: a new
// string gets an id in ui_text.h and an entry in every language that has one (a missing entry falls back to English).
// Every language's array is generated from i18n/ui/<lang>.po (tools/i18n/catalog.py, English from i18n/ui/mkw.pot); the
// menu, the start screen and the setup page read them in the menu language, the software keyboard in the game's.
#include <stdatomic.h>
#include <string.h>

#include "cfg.h"
#include "ov.h"
#include "platform/platform.h"
#include "ui_text.h"

#include "ui_text_cat.h"

typedef struct { const char *lang; const char *const *text; } Row;

static const Row rows[] = {
    {"en", text_en}, {"ja", text_ja}, {"fr", text_fr}, {"de", text_de}, {"it", text_it}, {"es", text_es},
    {"nl", text_nl}, {"pt", text_pt}, {"pt-BR", text_pt_br}, {"ru", text_ru}, {"zh-Hans", text_zh_hans},
    {"zh-Hant", text_zh_hant}, {"ko", text_ko}, {"es-419", text_es_419}, {"ca", text_ca}, {"gl", text_gl},
    {"eu", text_eu}, {"pl", text_pl}, {"tr", text_tr}, {"uk", text_uk}, {"th", text_th},
};

const char *ui_text_lang(const char *code) {
    static const struct { const char *code, *lang; } map[] = {
        {"en-US", "en"}, {"en-GB", "en"}, {"fr-CA", "fr"}, {"zh-CN", "zh-Hans"}, {"zh-TW", "zh-Hant"},
    };
    for (size_t i = 0; i < sizeof map / sizeof *map; i++)
        if (!strcmp(code, map[i].code)) return map[i].lang;
    return code;
}

static const Row *find_row(const char *lang) {
    for (size_t i = 0; i < sizeof rows / sizeof *rows; i++)
        if (!strcmp(rows[i].lang, lang)) return &rows[i];
    return NULL;
}

int ui_text_defined(const char *lang, UiTextId id) {
    const Row *r = find_row(lang);
    return (unsigned)id < UI_TEXT_COUNT && r && r->text[id] && *r->text[id];
}

const char *ui_text_in(const char *lang, UiTextId id) {
    if ((unsigned)id >= UI_TEXT_COUNT) return "";
    return ui_text_defined(lang, id) ? find_row(lang)->text[id] : rows[0].text[id];
}

const char *ui_text(UiTextId id) { return ui_text_in(ui_text_lang(ov_language()), id); }

// The catalog language for a system UI language tag ("pt-BR", "zh_TW", "de-AT"): Brazilian Portuguese only for
// Brazil, Traditional Chinese for the Hant script or Taiwan, Hong Kong and Macao, Latin American Spanish for any
// region but Spain (as switch_language picks the game's); NULL for one we have no row for.
static const char *system_menu_lang(const char *tag) {
    if (!tag || strlen(tag) < 2) return NULL;
    char lang[3] = {tag[0], tag[1], 0};
    const char *rest = tag + 2;
    if (*rest && *rest != '-' && *rest != '_') return NULL;
    if (!strcmp(lang, "pt")) return strstr(rest, "BR") ? "pt-BR" : "pt";
    if (!strcmp(lang, "es")) return rest[0] && rest[1] && strncmp(rest + 1, "ES", 2) ? "es-419" : "es";
    if (!strcmp(lang, "zh"))
        return strstr(rest, "Hant") || strstr(rest, "TW") || strstr(rest, "HK") || strstr(rest, "MO") ? "zh-Hant"
                                                                                                    : "zh-Hans";
    const Row *r = find_row(lang);
    return r ? r->lang : NULL;
}

// The menu language, re-read when cfg_version changes: MKW_MENU_LANG when it names a row, else the system UI language
// when we have it, else English.
static const char *resolve_menu_lang(void) {
    const char *v = cfg_value("MKW_MENU_LANG");
    const Row *r = v && *v ? find_row(v) : NULL;
    if (r) return r->lang;
    const char *sys = system_menu_lang(plat_ui_language());
    return sys ? sys : "en";
}

static _Atomic(const char *) g_menu_lang = "en";
static _Atomic unsigned g_menu_seen = ~0u;

const char *menu_lang(void) {
    unsigned v = cfg_version();
    if (v != atomic_load_explicit(&g_menu_seen, memory_order_relaxed)) {
        atomic_store_explicit(&g_menu_lang, resolve_menu_lang(), memory_order_relaxed);
        atomic_store_explicit(&g_menu_seen, v, memory_order_relaxed);
    }
    return atomic_load_explicit(&g_menu_lang, memory_order_relaxed);
}

const char *menu_text(UiTextId id) { return ui_text_in(menu_lang(), id); }
