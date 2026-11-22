// HD rumble through RenHDrum (https://github.com/trzz1214/RenHDrum, MIT OR Apache-2.0): its recomp bridge
// (third_party/renhdrum) plays the game's vibration values on player 1's controller with both bands and their
// frequencies, instead of the two-motor mix of ov_input.c rumble_update:
//   - Nintendo Switch Pro Controller (USB, Bluetooth): HD rumble packets, as the console sends them;
//   - DualSense / DualSense Edge: USB, its 4-channel 48 kHz audio device (the haptic actuators and the speaker; on
//     Linux the controller's "Analog Surround 4.0" profile) plus the vibration-reactive adaptive triggers; Bluetooth,
//     HID report 0x36 (Opus) or 0x32;
//   - Xbox One / Series: the four motors (grips and impulse triggers);
//   - Steam Controller (2026) grips (experimental).
// The library is loaded at run time and is optional: renhdrum.dll next to mkw.exe, librenhdrum.so next to mkw on Linux
// (`cargo build --release` in RenHDrum). Without it, or for any other controller, the SDL rumble path stays as it was.
// Its own settings (strength, transport, DualSense volume...) are renhdrum.ini next to the executable.
// MKW_HD_RUMBLE=0: never loaded.
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "cfg.h"
#include "hd_rumble.h"
#include "host.h"
#include "platform/platform.h"
#include "renhdrum_recomp_bridge.h"

#ifdef _WIN32
#define RH_LIBRARY "renhdrum.dll"
#else
#define RH_LIBRARY "librenhdrum.so"   // renhdrum_posix.h LoadLibraryExW
#endif

// ---- the host functions the bridge calls
void hos_log(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    rt_log("%s", buf);   // the bridge's lines start with "renhdrum: "
}
int64_t hos_now_ns(void) { return (int64_t)plat_now_ns(); }
int hos_sdl_init(uint32_t subsystems) { return SDL_InitSubSystem(subsystems); }   // the DualSense's audio device

// ---- state
static volatile int32_t g_on;     // 1 once the library is loaded (the first gamepad), -1 off or missing
static volatile int32_t g_owns;   // the last rh_vibration: RenHDrum drives player 1's gamepad
static void *g_pad;               // input.c's lock: player 1's gamepad as given to the bridge

// Under input.c's lock (attach): loads the library once.
static int up(void) {
    int32_t on = __atomic_load_n(&g_on, __ATOMIC_ACQUIRE);
    if (on) return on > 0;
    const char *v = cfg_get("MKW_HD_RUMBLE");
    if (v && *v && strcmp(v, "0") && strcmp(v, "1")) rt_log("input: MKW_HD_RUMBLE=%s is neither 0 nor 1: on", v);
    if (v && !strcmp(v, "0")) {
        rt_log("renhdrum: off (MKW_HD_RUMBLE=0)");
        on = -1;
    } else {
        rh_set_switch_gen(2);   // the game's values are Switch 2's (RenHDrum's Mode 2 translation)
        on = rh_load() ? 1 : -1;   // logs why when it is not there
        if (on > 0) rt_log("renhdrum: loaded, settings in renhdrum.ini next to the executable");
        else rt_log("renhdrum: the SDL rumble only; HD rumble needs " RH_LIBRARY " next to the executable");
    }
    __atomic_store_n(&g_on, on, __ATOMIC_RELEASE);
    return on > 0;
}

void hd_rumble_attach(void *pad) {
    if (!pad || !up()) return;
    if (g_pad && g_pad != pad) hd_rumble_detach(g_pad);
    g_pad = pad;
    rh_attach(pad);   // a controller RenHDrum does not know stays on the SDL rumble path
    rt_log("input: %s", rh_status_line());
}

void hd_rumble_detach(void *pad) {
    if (!pad || pad != g_pad) return;
    __atomic_store_n(&g_owns, 0, __ATOMIC_RELEASE);
    rh_detach(pad);   // stops its rumble, speaker and trigger effects before SDL closes it
    g_pad = NULL;
}

int hd_rumble_play(int side, float amp_low, float freq_low, float amp_high, float freq_high) {
    if (__atomic_load_n(&g_on, __ATOMIC_ACQUIRE) <= 0) return 0;
    // 1 when the value went to the controller and the bridge owns it (a DualSense over USB only once its audio device
    // is open): until then, and after it is lost, the SDL path keeps the motors going
    int owns = rh_vibration(side & 1, amp_low, freq_low, amp_high, freq_high);
    __atomic_store_n(&g_owns, owns, __ATOMIC_RELEASE);
    return owns;
}

int hd_rumble_owns(void) { return __atomic_load_n(&g_owns, __ATOMIC_ACQUIRE); }

void hd_rumble_permit(int on) {
    if (__atomic_load_n(&g_on, __ATOMIC_ACQUIRE) > 0) rh_permit(on);
}

void hd_rumble_shutdown(void) {
    if (__atomic_load_n(&g_on, __ATOMIC_ACQUIRE) > 0) rh_shutdown();
}
