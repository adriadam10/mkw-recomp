// The host side of renhdrum_recomp_bridge_win.c (renhdrum_host.h says what it is for): the three functions the bridge
// expects of its host, and the switch that keeps it all off unless MKW_RENHDRUM=1. The bridge loads renhdrum.dll from the
// exe's folder with LoadLibraryExW and SDL3.dll's functions by name, so nothing is linked.
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <SDL3/SDL.h>

#include "cfg.h"
#include "host.h"
#include "platform/platform.h"
#include "renhdrum_host.h"
#include "renhdrum_recomp_bridge.h"

enum { PLAYER_1_SLOT = 0 };   // the one gamepad attached to the bridge takes its first slot

static volatile int32_t loaded;   // renhdrum_start found a usable renhdrum.dll

void hos_log(const char *fmt, ...) {
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    rt_log("%s", line);
}

int64_t hos_now_ns(void) { return (int64_t)plat_now_ns(); }

int hos_sdl_init(uint32_t subsystems) { return SDL_InitSubSystem(subsystems) ? 0 : -1; }

void renhdrum_start(void) {
    if (!cfg_bool("MKW_RENHDRUM")) return;
    rh_set_switch_gen(2);   // a Switch 2 game: its rumble curves
    if (!rh_load()) {
        rt_log("renhdrum: not used, normal rumble (%s)", rh_status_line());
        return;
    }
    plat_xchg32(&loaded, 1);
    rt_log("renhdrum: renhdrum.dll loaded (MKW_RENHDRUM=1), player 1's gamepad");
}

void renhdrum_attach(void *gamepad) {
    if (loaded) rh_attach(gamepad);
}

void renhdrum_detach(void *gamepad) {
    if (loaded) rh_detach(gamepad);
}

void renhdrum_vibration(int side, const float v[4]) {
    if (loaded) rh_vibration(side, v[0], v[1], v[2], v[3]);
}

int renhdrum_claims(void) {
    return loaded && rh_pad_claimed(PLAYER_1_SLOT);
}

void renhdrum_permit(int on) {
    if (loaded) rh_permit(on);
}

void renhdrum_shutdown(void) {
    if (plat_xchg32(&loaded, 0)) rh_shutdown();
}
