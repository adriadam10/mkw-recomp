// Pause: F5 stops the game and starts it again, as an emulator's pause key (Ryujinx's is F5 too; F3 and F4 are the
// freeze hack's). To the game it is the console's HOME button (gfx_present.c gfx_window_pause, ov_system.c
// oe_hold_focus): it hears that it lost the focus and is suspended in its present (freeze.c), the window keeps the last
// frame and the audio renderer stops (audren.c), so the music goes on where it stopped. The guest clock runs on, as
// the console's does behind HOME. While the game uses the network it asks to be only notified, and as on the console
// it then runs on, out of focus and silent: a LAN race does not stop for the others. Always on: the key does nothing
// until it is pressed. Polled from the game's present (freeze.c), also while it waits there.
#include "gfx.h"
#include "hacks.h"
#include "host.h"
#include "input.h"

static int key_was;

void pause_poll(void) {
    int down = input_hack_key(HACK_KEY_PAUSE);
    if (down && !key_was) {
        int on = !gfx_window_paused();
        int suspended = gfx_window_pause(on);
        rt_log("pause: %s", !on ? "off" : suspended ? "on, the game is suspended" : "on, the game runs on (online or starting)");
    }
    key_was = down;
}
