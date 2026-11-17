// Racing wheels as gamepads (#488). SDL knows many wheels by vendor and product (SDL_IsJoystickWheel, the MOZA R3 and
// R5 among them) but on purpose guesses no gamepad mapping for them (SDL_gamepad.c, SDL_CreateMappingForHIDAPIGamepad),
// and the game and the launcher only open gamepads: a DirectInput wheel without a mapping was invisible. Each wheel
// without one gets a mapping by position: the wheel axis is the left stick's x, the first buttons are A, B, X, Y,
// shoulders, Minus and Plus, the first hat the D-pad. The pedals stay unmapped: their axis and rest position differ
// from model to model, so a guess could hold the accelerator down. A gamecontrollerdb.txt in the game folder (SDL's
// format, one line per device) maps them and overrides this.
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "cfg.h"
#include "host.h"
#include "input.h"
#include "platform/platform.h"

enum { TRIED_MAX = 16 };

// Wheels already handled: one whose mapping failed is not opened again at every rescan (once a second).
static SDL_JoystickID tried[TRIED_MAX];
static int ntried;

void input_wheel_hint(void) {
    char path[CFG_GAME_DIR_MAX + 32];
    snprintf(path, sizeof path, "%s/gamecontrollerdb.txt", cfg_game_dir());
    if (!plat_file_exists(path)) return;
    SDL_SetHint(SDL_HINT_GAMECONTROLLERCONFIG_FILE, path);
    rt_log("input: gamepad mappings from %s", path);
}

static int was_tried(SDL_JoystickID id) {
    for (int i = 0; i < ntried; i++)
        if (tried[i] == id) return 1;
    return 0;
}

// The mapping of a wheel with these counts, by position; 0 when it has no axis to steer with.
static int wheel_mapping(char *out, size_t cap, const char *guid, const char *name, int axes, int buttons, int hats) {
    static const char *const by_button[] = {"a", "b", "x", "y", "leftshoulder", "rightshoulder", "back", "start"};
    if (axes < 1) return 0;
    int len = snprintf(out, cap, "%s,", guid);
    for (const char *c = name; *c && len < (int)cap - 2; c++) out[len++] = *c == ',' ? ' ' : *c;   // ',' ends the field
    len += snprintf(out + len, cap - (size_t)len, ",leftx:a0,");
    for (int b = 0; b < buttons && b < (int)(sizeof by_button / sizeof *by_button); b++)
        len += snprintf(out + len, cap - (size_t)len, "%s:b%d,", by_button[b], b);
    if (hats > 0) snprintf(out + len, cap - (size_t)len, "dpup:h0.1,dpright:h0.2,dpdown:h0.4,dpleft:h0.8,");
    return 1;
}

void input_wheel_map(void) {
    int n = 0;
    SDL_JoystickID *ids = SDL_GetJoysticks(&n);
    for (int i = 0; ids && i < n; i++) {
        SDL_JoystickID id = ids[i];
        if (SDL_GetJoystickTypeForID(id) != SDL_JOYSTICK_TYPE_WHEEL || SDL_IsGamepad(id) || was_tried(id)) continue;
        if (ntried < TRIED_MAX) tried[ntried++] = id;
        SDL_Joystick *j = SDL_OpenJoystick(id);
        if (!j) {
            rt_log("input: wheel %d cannot be opened (%s)", (int)id, SDL_GetError());
            continue;
        }
        int axes = SDL_GetNumJoystickAxes(j), buttons = SDL_GetNumJoystickButtons(j), hats = SDL_GetNumJoystickHats(j);
        const char *name = SDL_GetJoystickName(j);
        char guid[64], mapping[512];
        SDL_GUIDToString(SDL_GetJoystickGUID(j), guid, sizeof guid);
        int ok = wheel_mapping(mapping, sizeof mapping, guid, name ? name : "Wheel", axes, buttons, hats) &&
                 SDL_AddGamepadMapping(mapping) >= 0;
        rt_log("input: wheel \"%s\" (%d axes, %d buttons, %d hats) %s%s", name ? name : "?", axes, buttons, hats,
               ok ? "mapped as a gamepad: " : "not mapped: ", ok ? mapping : (axes < 1 ? "no axis" : SDL_GetError()));
        SDL_CloseJoystick(j);
    }
    SDL_free(ids);
}
