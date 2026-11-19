// An example plugin that changes nothing in the game: a notice when the game code is loaded and a small box in the top
// left corner of the picture with the runtime's and the game's versions. Built into a mod folder (or its .zip) that the
// launcher's Mods page imports by tools/plugin/example.sh. It includes only mkw_plugin.h and links against nothing of
// mkw-recomp.
#include <stdio.h>

#include "mkw_plugin.h"

static const MkwPluginApi *api;
static char line[128];

static void draw(const MkwOverlay *o, void *user) {
    (void)user;
    float pad = 6 * o->scale, w = 0, h = 0;
    o->text_size(line, &w, &h);
    float x = 12 * o->scale, y = 12 * o->scale;
    o->rect(x, y, x + w + pad * 2, y + h + pad * 2, 0x000000a0u, 4 * o->scale);
    o->text(x + pad, y + pad, 0xffd75affu, line);
}

static void loaded(void *user) {
    (void)user;
    api->notice("Example plugin loaded", 5);
    api->overlay_show(draw, 1);
}

MKW_PLUGIN_EXPORT int mkw_plugin_init(const MkwPluginApi *a, uint32_t version, const char **message) {
    // notice and the overlay came with the table's last fields: an older runtime's table is shorter
    if (version < 1 || a->size < sizeof(MkwPluginApi)) {
        *message = "needs a newer mkw-recomp";
        return 1;
    }
    api = a;
    snprintf(line, sizeof line, "Example plugin - mkw-recomp %s, game %s", api->port_version, api->game_version);
    api->overlay_set(draw, 0);
    api->on_game_loaded(loaded, 0);
    api->log("example plugin: init in %s", api->mod_dir);
    return 0;
}
