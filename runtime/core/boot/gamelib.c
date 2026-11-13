// The game code's tables (gamelib.h) and what the runtime reads from them. build/mkw links them in; the player's build
// loads them from mkwgame.so / mkwgame.dll in the selected game folder, which the start screen's setup builds from their own
// game files (SavageWindRecomp runtime/gamelib.c, MIT: same idea, one entry point found by name, an id check).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "dispatch.h"
#include "gamelib.h"
#include "host.h"
#include "platform/platform.h"

static const RtGame *game;

static const char *check(const RtGame *g) {
    if (g->abi != RT_GAME_ABI || !g->id || strcmp(g->id, RT_GAME_ID)) return "built for another version of the runtime";
    if (g->nmodules < 1 || g->nmodules > RT_MAX_MODULES || g->npatch_sets > RT_MAX_PATCH_SETS) return "malformed";
    return NULL;
}

// The library in the selected game folder, its tables if they suit this runtime; why not in *why.
static const RtGame *load_library(const char **why) {
    char path[PLAT_PATH_MAX * 2];
    *why = "the game files are not set up";
    snprintf(path, sizeof path, "%s/%s", cfg_game_dir(), RT_GAME_LIB);
    if (!plat_file_exists(path)) return NULL;
    void *lib = plat_lib_open(path);
    const RtGame *(*entry)(void) = lib ? (const RtGame *(*)(void))plat_lib_sym(lib, RT_GAME_ENTRY) : NULL;
    const RtGame *g = entry ? entry() : NULL;
    *why = !lib ? "the game code library does not load" : !g ? "the game code library has no tables" : check(g);
    return *why ? NULL : g;
}

static const RtGame *found;
static const char *found_why;
static int looked;

static const RtGame *find(const char **why) {
    if (!looked) {
        looked = 1;
        found = rt_game_builtin();
        found_why = found ? check(found) : NULL;
        if (!found) found = load_library(&found_why);
        if (found_why) found = NULL;
    }
    if (why) *why = found_why;
    return found;
}

const RtGame *rt_game(void) {
    if (game) return game;
    const char *why;
    const RtGame *g = find(&why);
    if (!g) rt_fatal("game code: %s", why);
    game = g;
    return game;
}

// Without loading it: the start screen keeps no library mapped, so a new setup can replace it (Windows does not let a
// loaded DLL be overwritten). The id file beside it (build.py --dist, prepare.py) says which headers it was built with;
// the game's own start loads it and checks it again.
int rt_game_present(void) {
    if (game || rt_game_builtin()) return 1;
    char path[PLAT_PATH_MAX * 2];
    snprintf(path, sizeof path, "%s/%s", cfg_game_dir(), RT_GAME_LIB);
    if (!plat_file_exists(path)) return 0;
    snprintf(path, sizeof path, "%s/mkwgame.id", cfg_game_dir());
    uint64_t size = 0;
    char *id = (char *)plat_file_read(path, &size);
    int ok = id && size >= strlen(RT_GAME_ID) && !strncmp(id, RT_GAME_ID, strlen(RT_GAME_ID));
    free(id);
    return ok;
}

void rt_fnprof_set(int on) { *rt_game()->fnprof = on; }

int rt_linked_modules(RtModule *out, int max) {
    const RtGame *g = rt_game();
    int n = 0;
    for (; n < g->nmodules && n < max; n++) {
        const RtGameModule *m = &g->modules[n];
        out[n] = (RtModule){m->name, m->base, m->size, m->addrs, m->fns, *m->count};
    }
    return n;
}

int rt_game_version(void) { return rt_game()->version; }
