// --probe-pads: players and their gamepads (input/input.c) with four virtual SDL gamepads, without the game: each one's
// button reaches its player only, input_pad_swap makes it reach the other (players 1 and 2, then 2 and 4), twice puts it
// back, a gamepad goes to a player without one, player 1 keeps the keyboard with MKW_PAD_P2 (no swap), and bad indices do
// nothing. Why: which controller is which player is decided by the order they connect, and the menu's Controls tab
// changes it. Gamepads connected to the machine come first and only shift the players checked.
#include <SDL3/SDL.h>
#include <stdint.h>
#include <stdio.h>

#include "cfg.h"
#include "host.h"
#include "input.h"
#include "platform/platform.h"

static int failures;
#define CHECK(cond, ...)                                                                                               \
    do {                                                                                                               \
        int ok_ = (cond);                                                                                              \
        printf("%s: ", ok_ ? "ok  " : "FAIL");                                                                         \
        printf(__VA_ARGS__);                                                                                           \
        printf("\n");                                                                                                  \
        failures += !ok_;                                                                                              \
    } while (0)

enum { PADS = 4 };

static SDL_Joystick *vj[PADS];

static uint64_t held(int player) {
    uint64_t b = 0;
    int32_t x[4];
    input_poll_player(player, &b, &x[0], &x[1], &x[2], &x[3]);
    return b;
}

// The player (from base, else -1 none, -2 several) that gets the button of virtual gamepad i held.
static int who(int i, int base) {
    SDL_SetJoystickVirtualButton(vj[i], SDL_GAMEPAD_BUTTON_SOUTH, true);
    SDL_UpdateJoysticks();
    int found = -1;
    for (int p = base; p < INPUT_MAX_PLAYERS; p++)
        if (held(p)) found = found == -1 ? p : -2;
    SDL_SetJoystickVirtualButton(vj[i], SDL_GAMEPAD_BUTTON_SOUTH, false);
    SDL_UpdateJoysticks();
    return found;
}

int probe_pads(void) {
    plat_setenv("MKW_VIRTUAL_PADS", "4");
    plat_setenv("MKW_PAD_P2", "0");
    plat_setenv("MKW_KEYBOARD_MOUSE", "0");
    (void)input_player_mask();   // starts the gamepads
    int n = 0, k = 0, base = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    for (int i = 0; i < n; i++) {
        if (SDL_IsJoystickVirtual(ids[i]) && k < PADS) vj[k++] = SDL_OpenJoystick(ids[i]);
        else if (!k) base++;
    }
    SDL_free(ids);
    if (k < PADS || base + PADS > INPUT_MAX_PLAYERS) {
        printf("probe-pads: %d virtual gamepads after %d others: not enough to check\n", k, base);
        return 1;
    }
    printf("probe-pads: %d gamepad(s) of the machine first\n", base);
    for (int i = 0; i < PADS; i++) CHECK(who(i, 0) == base + i, "gamepad %d is player %d's only", i + 1, base + i + 1);
    CHECK(input_pad_swap(base, base + 1), "players %d and %d trade", base + 1, base + 2);
    CHECK(who(0, 0) == base + 1 && who(1, 0) == base, "their gamepads follow");
    CHECK(who(2, 0) == base + 2 && who(3, 0) == base + 3, "the others stay");
    CHECK(input_pad_swap(base + 1, base + 3) && who(0, 0) == base + 3 && who(3, 0) == base + 1, "players %d and %d trade", base + 2, base + 4);
    CHECK(input_pad_swap(base + 1, base + 3) && input_pad_swap(base, base + 1), "traded back");
    for (int i = 0; i < PADS; i++) CHECK(who(i, 0) == base + i, "gamepad %d is player %d's again", i + 1, base + i + 1);
    if (base + PADS < INPUT_MAX_PLAYERS) {
        CHECK(input_pad_swap(base + 1, base + PADS) && who(1, 0) == base + PADS, "a gamepad goes to a player without one");
        CHECK(!input_pad_can_move_up(base + 1), "no row to move for the player left without one");
        CHECK(input_pad_swap(base + 1, base + PADS), "and back");
    }
    CHECK(input_pad_can_move_up(base + 1) && !input_pad_can_move_up(0), "player 1 cannot move up, the next can");
    CHECK(!input_pad_swap(0, 0) && !input_pad_swap(-1, 1) && !input_pad_swap(0, INPUT_MAX_PLAYERS), "bad indices do nothing");
    CHECK(!input_pad_swap(INPUT_MAX_PLAYERS - 2, INPUT_MAX_PLAYERS - 1) || base + PADS > INPUT_MAX_PLAYERS - 2,
          "two players without a gamepad do nothing");
    plat_setenv("MKW_PAD_P2", "1");
    CHECK(!input_pad_swap(0, 1) && !input_pad_can_move_up(1), "MKW_PAD_P2: player 1 keeps the keyboard");
    CHECK(input_pad_swap(1, 2) && input_pad_swap(1, 2), "MKW_PAD_P2: players 2 and 3 can trade");
    printf("probe-pads: %s\n", failures ? "FAILED" : "passed");
    return failures != 0;
}
