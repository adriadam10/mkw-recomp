// The players' controller batteries (discussion #516): what SDL reports for each player's gamepad (a percentage for
// DualShock 4 and DualSense, nine levels for a Switch Pro Controller and Joy-Con, four for an Xbox pad over XInput),
// or for a Switch 2 controller over Bluetooth, which SDL sees as a virtual gamepad without one, its voltage band from
// switch2.c, or on Linux for an Xbox pad on the xone driver, whose battery SDL leaves UNKNOWN: its sysfs level band
// from linux_power.c (High/Medium/Low, never a percent). The game never asks (nn::hid::system::GetNpadPowerInfo has
// no caller in MKW): the menu (F1), the start screen and a notice show it.
#include <stdio.h>
#include <stdlib.h>
#include <SDL3/SDL.h>

#include "cfg.h"
#include "host.h"
#include "input.h"
#include "switch2.h"
#include "switch2_proto.h"
#include "ui/menu/menu.h"
#include "ui_text.h"
#include "platform/platform.h"
#ifdef __linux__
#include "linux_power.h"
#endif

// A battery at or under LOW_PCT warns once; it warns again only after it was back over REARM_PCT or charging. Our
// choice, not a measure: about the last tenth of a play session on a DualSense.
enum { LOW_PCT = 15, REARM_PCT = 30, WATCH_NS = 2000000000 };

int input_power_from_sdl(int st, int pct, uint32_t id, int index, InputPower *out) {
    // MKW_BATTERY_TEST=<percent>: every gamepad on battery at that level (virtual gamepads have none), for the rows,
    // the start screen and the notice without a controller running down
    const char *t = cfg_get("MKW_BATTERY_TEST");
    if (t && *t) st = SDL_POWERSTATE_ON_BATTERY, pct = atoi(t);
    int mv = 0, band = switch2_battery(id, &mv);
    if (band != S2_BATTERY_UNKNOWN) {
        static const int band_pct[] = {-1, 10, 50, 90};
        out->state = INPUT_POWER_BATTERY, out->percent = band_pct[band], out->coarse = 1;
        return out->state;
    }
    out->state = st == SDL_POWERSTATE_ON_BATTERY ? INPUT_POWER_BATTERY
                 : st == SDL_POWERSTATE_CHARGING ? INPUT_POWER_CHARGING
                 : st == SDL_POWERSTATE_CHARGED  ? INPUT_POWER_CHARGED
                 : st == SDL_POWERSTATE_NO_BATTERY ? INPUT_POWER_WIRED
                                                   : INPUT_POWER_UNKNOWN;
    out->percent = out->state == INPUT_POWER_WIRED || out->state == INPUT_POWER_UNKNOWN ? -1 : pct;
#ifdef __linux__
    // xone (the Xbox wireless dongle driver) reports the battery to sysfs/UPower only, so SDL says UNKNOWN: read its
    // level band there (index: the pad's order among Xbox-named pads).
    if (out->state == INPUT_POWER_UNKNOWN && linux_power_lookup(index, out->name, out)) return out->state;
#endif
    return out->state;
}

int input_pad_power(int player, InputPower *out) {
    *out = (InputPower){INPUT_POWER_NONE, -1, 0, ""};
    uint32_t id = 0;
    int pct = -1, st = input_pad_power_sdl(player, out->name, sizeof out->name, &id, &pct);
#ifdef __linux__
    int xi = input_pad_xbox_index(player);   // the Nth Xbox-named pad reads the Nth Xbox sysfs supply
#else
    int xi = player;
#endif
    return st == -2 ? out->state : input_power_from_sdl(st, pct, id, xi, out);
}

const char *input_power_text(const InputPower *p, char *out, int out_size) {
    char level[32] = "";
    if (p->coarse) snprintf(level, sizeof level, "%s", menu_text(p->percent <= 10 ? UI_POWER_LOW
                                                                 : p->percent <= 50 ? UI_POWER_MEDIUM
                                                                                    : UI_POWER_HIGH));
    else if (p->percent >= 0) snprintf(level, sizeof level, "%d %%", p->percent);
    UiTextId word = p->state == INPUT_POWER_CHARGING ? UI_POWER_CHARGING
                    : p->state == INPUT_POWER_CHARGED ? UI_POWER_CHARGED
                    : p->state == INPUT_POWER_WIRED   ? UI_POWER_WIRED
                                                      : UI_POWER_UNKNOWN;
    if (p->state == INPUT_POWER_BATTERY && level[0]) snprintf(out, (size_t)out_size, "%s", level);
    else if (level[0] && word != UI_POWER_UNKNOWN) snprintf(out, (size_t)out_size, "%s · %s", menu_text(word), level);
    else snprintf(out, (size_t)out_size, "%s", menu_text(word));
    return out;
}

// Every 2 s: each player's battery; a notice (and a log line) when one goes low on battery.
static void watch_main(void *arg) {
    (void)arg;
    int warned[INPUT_MAX_PLAYERS] = {0};
    for (;;) {
        plat_wait_ns(WATCH_NS);
        for (int p = 0; p < INPUT_MAX_PLAYERS; p++) {
            InputPower w;
            int st = input_pad_power(p, &w);
            if (st != INPUT_POWER_BATTERY || w.percent < 0) {
                if (st != INPUT_POWER_UNKNOWN) warned[p] = 0;   // gone, wired or charging; unknown keeps it
                continue;
            }
            if (w.percent > REARM_PCT) warned[p] = 0;
            if (warned[p] || w.percent > LOW_PCT) continue;
            warned[p] = 1;
            char level[64], text[256];
            input_power_text(&w, level, sizeof level);
            rt_log("input: player %d's gamepad \"%s\" is low on battery (%s)", p + 1, w.name, level);
            snprintf(text, sizeof text, menu_text(UI_NOTICE_BATTERY_LOW), p + 1, level);
            menu_notice(text, 8);
        }
    }
}

void input_power_start(void) {
    static int started;
    if (started) return;
    started = 1;
    if (!plat_thread_spawn(watch_main, NULL, "battery", 0)) rt_log("input: no battery watch thread");
}
