// An example plugin that changes nothing in the game: a notice when the game code is loaded and a small box in a
// corner of the picture with the runtime's and the game's versions. Its options page (the Mods page's Options button,
// mkw_mod_page, also a tab of the in-game menu's settings) shows every widget of MkwPage in two columns: whether to
// draw the box, which corner, the notice's text and seconds, and a list of names to add and remove; the game reads
// them back with setting_get. Built into a mod folder (or its .zip) that the
// launcher's Mods page imports by tools/plugin/example.sh. It includes only mkw_plugin.h and links against nothing of
// mkw-recomp.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mkw_plugin.h"

static const MkwPluginApi *api;
static char line[128], greeting[128];
static int right;   // the box in the top right corner

static void draw(const MkwOverlay *o, void *user) {
    (void)user;
    float pad = 6 * o->scale, w = 0, h = 0;
    o->text_size(line, &w, &h);
    float x = right ? o->width - w - pad * 2 - 12 * o->scale : 12 * o->scale, y = 12 * o->scale;
    o->rect(x, y, x + w + pad * 2, y + h + pad * 2, 0x000000a0u, 4 * o->scale);
    o->text(x + pad, y + pad, 0xffd75affu, line);
}

MKW_PLUGIN_EXPORT void mkw_mod_page(const MkwPage *p);

static void loaded(void *user) {
    (void)user;
    char secs[8];
    api->notice(greeting, api->setting_get("seconds", secs, sizeof secs) ? atoi(secs) : 5);
    char v[8];
    if (!api->setting_get("box", v, sizeof v) || !strcmp(v, "1")) api->overlay_show(draw, 1);
}

MKW_PLUGIN_EXPORT int mkw_plugin_init(const MkwPluginApi *a, uint32_t version, const char **message) {
    // the settings came with the table's version 2 fields: an older runtime's table is shorter
    if (version < 2 || a->size < sizeof(MkwPluginApi)) {
        *message = "needs a newer mkw-recomp";
        return 1;
    }
    api = a;
    char corner[16];
    right = api->setting_get("corner", corner, sizeof corner) && !strcmp(corner, "right");
    if (!api->setting_get("greeting", greeting, sizeof greeting))
        snprintf(greeting, sizeof greeting, "Example plugin loaded");
    snprintf(line, sizeof line, "Example plugin - mkw-recomp %s, game %s, %s", api->port_version, api->game_version,
             api->profile_name);
    api->overlay_set(draw, 0);
    api->on_game_loaded(loaded, 0);
    api->settings_tab("Example", mkw_mod_page);   // the same page as a tab of the in-game menu's settings
    api->log("example plugin: init in %s (language %s)", api->mod_dir, api->language);
    return 0;
}

// ---- the options page. Its texts in two languages, as a plugin translates its own (page->language).
enum { T_INTRO, T_BOX, T_CORNER, T_LEFT, T_RIGHT, T_GREETING, T_GREETING_HINT, T_NAMES, T_NO_NAMES, T_REMOVE, T_ADD,
       T_NEW_NAME, T_FULL, T_SECONDS, T_IN_GAME, T_COUNT };
static const char *const en[T_COUNT] = {
    "This plugin changes nothing in the game: it shows its options page and a box over the picture.",
    "Show the version box over the game", "Corner", "Top left", "Top right", "Notice at start", "Example plugin loaded",
    "Names", "No names yet", "Remove", "Add", "New name", "The list is full", "Notice seconds",
    "The box and the notice change at the next start."};
static const char *const pt_br[T_COUNT] = {
    "Este plugin não muda nada no jogo: mostra sua página de opções e uma caixa sobre a imagem.",
    "Mostrar a caixa de versão sobre o jogo", "Canto", "Superior esquerdo", "Superior direito", "Aviso ao iniciar",
    "Plugin de exemplo carregado", "Nomes", "Nenhum nome ainda", "Remover", "Adicionar", "Novo nome",
    "A lista está cheia", "Segundos do aviso", "A caixa e o aviso mudam no próximo início."};

// The names: one setting, "a,b,c" (a list a plugin keeps itself; a server's would come from a thread of its own).
enum { MAX_NAMES = 8, NAME_CAP = 33 };

static int read_names(const MkwPage *p, char names[MAX_NAMES][NAME_CAP]) {
    char list[MAX_NAMES * NAME_CAP] = "";
    int n = 0;
    p->setting_get("names", list, sizeof list);
    for (char *s = list; *s && n < MAX_NAMES;) {
        char *e = strchr(s, ',');
        size_t len = e ? (size_t)(e - s) : strlen(s);
        if (len && len < NAME_CAP) memcpy(names[n], s, len), names[n++][len] = 0;
        s = e ? e + 1 : s + len;
    }
    return n;
}

static void write_names(const MkwPage *p, char names[MAX_NAMES][NAME_CAP], int n) {
    char list[MAX_NAMES * NAME_CAP] = "";
    size_t k = 0;
    for (int i = 0; i < n; i++) k += (size_t)snprintf(list + k, sizeof list - k, "%s%s", i ? "," : "", names[i]);
    p->setting_set("names", n ? list : NULL);
}

MKW_PLUGIN_EXPORT void mkw_mod_page(const MkwPage *p) {
    if (p->size < sizeof(MkwPage)) return;
    const char *const *t = !strcmp(p->language, "pt-BR") || !strcmp(p->language, "pt") ? pt_br : en;
    if (p->opened) p->log("example plugin: options page opened for %s", p->profile_name);
    p->text(t[T_INTRO]);
    p->separator();
    p->columns_begin(2);   // the box's settings on the left, the names on the right
    p->checkbox("box", t[T_BOX], 1);
    char v[8];
    int box = !p->setting_get("box", v, sizeof v) || !strcmp(v, "1");
    p->disabled_begin(!box);
    static const char *const corners[] = {"left", "right"};
    const char *const labels[] = {t[T_LEFT], t[T_RIGHT]};
    p->choice("corner", t[T_CORNER], corners, labels, 2);
    p->disabled_end();
    p->input("greeting", t[T_GREETING], t[T_GREETING_HINT], 100, 0);
    p->slider("seconds", t[T_SECONDS], 1, 10, 5);

    p->column_next();
    p->heading(t[T_NAMES]);
    char names[MAX_NAMES][NAME_CAP];
    int n = read_names(p, names), remove = -1;
    p->list_begin("names", 4);
    p->list_empty(t[T_NO_NAMES]);
    const char *const buttons[] = {t[T_REMOVE]};
    for (int i = 0; i < n; i++)
        if (p->list_row(names[i], NULL, buttons, 1) == 1) remove = i;
    p->list_end();
    p->input("new_name", t[T_NEW_NAME], "", NAME_CAP - 1, 0);
    char add[NAME_CAP] = "";
    p->setting_get("new_name", add, sizeof add);
    p->disabled_begin(!add[0] || strchr(add, ',') || n == MAX_NAMES);
    int pressed = p->button(t[T_ADD]);
    p->disabled_end();
    if (n == MAX_NAMES) p->note(MKW_NOTE_WARN, t[T_FULL]);
    if (pressed && add[0] && n < MAX_NAMES) {
        snprintf(names[n++], NAME_CAP, "%s", add);
        p->setting_set("new_name", NULL);
    }
    if (remove >= 0) {
        for (int i = remove; i + 1 < n; i++) memcpy(names[i], names[i + 1], NAME_CAP);
        n--;
    }
    if (pressed || remove >= 0) write_names(p, names, n);
    p->columns_end();
    if (p->in_game) p->note(MKW_NOTE_DIM, t[T_IN_GAME]);
}
