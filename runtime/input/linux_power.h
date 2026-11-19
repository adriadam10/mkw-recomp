// Controller batteries the kernel reports but SDL does not (input_power.c): on Linux the xone driver for Xbox One
// and Series controllers (medusalix/xone, GPL-2.0) publishes each pad's battery as a power_supply device
// (driver/common.c: STATUS, CAPACITY_LEVEL, SCOPE and MODEL_NAME, no percentage), which UPower shows but
// SDL_GetGamepadPowerInfo leaves UNKNOWN. index picks the Nth matching supply (the player on the game side, the SDL
// gamepad order on the start screen); sdl_name gates the match to Xbox pads, so another pad whose battery SDL does
// not know is never shown an Xbox pad's level. 1 when out was filled, 0 when there is no such supply (out kept).
#pragma once
#include "input.h"
// 1 when sdl_name may be an Xbox pad (an empty name tells nothing, so it passes); the sysfs match below is
// Xbox-only, so a named non-Xbox pad never takes an Xbox level, and callers count only these pads for index.
int linux_power_is_xbox(const char *sdl_name);
int linux_power_lookup(int index, const char *sdl_name, InputPower *out);
// The same against another power_supply tree (tests: a fake sysfs), e.g. "/sys/class/power_supply".
int linux_power_lookup_at(const char *root, int index, const char *sdl_name, InputPower *out);
