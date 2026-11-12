// Adapted from MacNativeGames games/totk/runtime/input.c and hid_feedback.c @ b75be8cf (MIT, Copyright (c) 2026
// MacNativeGames contributors).
// nn::hid overrides: one connected controller per player (input_player_mask), player 1 fed by input_poll (gamepad and
// keyboard, input/input_sdl.c; nothing pressed with MKW_INPUT=none), the others by input_poll_player (one gamepad
// each). Docked (default) player N is a full-key controller with id N - 1 (style bit 0); with
// MKW_OPERATION_MODE=handheld player 1 is the handheld controller (id 0x20, style bit 1) and no other is connected,
// until the controller applet hands out two or more players: then, as on the console when controllers are detached,
// every player is full-key with ids from 0. Mouse and keyboard report no input. Each controller has two vibration
// devices (left, right) played on its gamepad's motors (MKW_RUMBLE=0: none); player 1's six-axis sensor is fed by its
// gamepad's gyro and accelerometer (MKW_MOTION=0, live: reads at rest), the others read at rest. State layouts follow
// libnx HID.
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "input.h"
#include "ov.h"
#include "platform/platform.h"
#include "ui/menu/menu.h"
#include "ui_text.h"

typedef struct { int64_t sampling; uint64_t buttons; int32_t lx, ly, rx, ry; uint32_t attributes, pad; } NpadState;

typedef struct { int8_t players; uint8_t pad[3]; uint32_t selected; } ControllerSupportResult;
_Static_assert(sizeof(NpadState) == 40, "hid state ABI");
_Static_assert(sizeof(ControllerSupportResult) == 8, "controller support result ABI");

enum { STYLE_FULL_KEY = 1, STYLE_HANDHELD = 2, ATTR_CONNECTED = 1 };

enum { ID_HANDHELD = 0x20, BIT_HANDHELD = 8 };   // id bits (supported, disconnected): 0-7 the players' ids, 8 handheld

static PlatMutex lock = PLAT_MUTEX_INIT;
static uint32_t styles = STYLE_FULL_KEY;
static uint32_t supported = ~0u, disconnected;   // id bits: SetSupportedNpadIdType, DisconnectNpad
static int enabled, multi;                      // multi: the applet handed out two or more players
static int8_t assigned[8] = {0, 1, 2, 3, 4, 5, 6, 7};   // the player whose controller each id reads (SwapNpadAssignment)
static int64_t sample;

// Player 1 on the handheld controller: handheld mode, until the applet hands out more than one player. Under lock.
static int on_handheld(void) { return ov_handheld() && !multi; }
static uint32_t pad_id(void) { return on_handheld() ? ID_HANDHELD : 0u; }
static int id_bit(uint32_t id) { return id == ID_HANDHELD ? BIT_HANDHELD : id < 8 ? (int)id : -1; }

// The player (0-7) on controller id, -1 for none; *style its style bit. Under lock.
static int id_player(uint32_t id, uint32_t *style) {
    int bit = id_bit(id), player = on_handheld() ? (id == ID_HANDHELD ? 0 : -1) : (id < 8 ? (int)id : -1);
    if (player < 0 || !enabled || !(supported >> bit & 1) || (disconnected >> bit & 1)) return -1;
    player = assigned[player];
    if (player && !(input_player_mask() >> player & 1)) return -1;
    if (style) *style = on_handheld() ? STYLE_HANDHELD : STYLE_FULL_KEY;
    return player;
}

static const uint32_t *need_id(Cpu *c, int i, const char *fn) {
    const uint32_t *id = arg_p(c, i);
    if (!id) rt_fatal("nn::hid::%s: null controller id", fn);
    return id;
}

// void InitializeNpad()
static void ov_initialize_npad(Cpu *c) {
    (void)c;
    plat_mutex_lock(&lock);
    enabled = 1;
    plat_mutex_unlock(&lock);
    rt_log("hid: controllers initialized (player 1 %s, %d players with a controller)", ov_handheld() ? "handheld" : "full-key",
           __builtin_popcount(input_player_mask()));
}

// void SetSupportedNpadIdType(const NpadIdType *ids, size_t count)
static void ov_supported_ids(Cpu *c) {
    const uint32_t *ids = arg_p(c, 0);
    uint64_t n = arg_x(c, 1);
    if (n > 10 || (!ids && n)) rt_fatal("nn::hid::SetSupportedNpadIdType: invalid id list (%llu ids)", (unsigned long long)n);
    uint32_t m = 0;
    for (uint64_t i = 0; i < n; i++) {
        int bit = id_bit(ids[i]);
        if (bit >= 0) m |= 1u << bit;
    }
    plat_mutex_lock(&lock);
    supported = m;
    plat_mutex_unlock(&lock);
}

// void SetSupportedNpadStyleSet(NpadStyleSet styles)
static void ov_supported_styles(Cpu *c) {
    plat_mutex_lock(&lock);
    styles = (uint32_t)arg_x(c, 0);
    plat_mutex_unlock(&lock);
}

// NpadStyleSet GetNpadStyleSet(const NpadIdType &id)
static void ov_style_set(Cpu *c) {
    uint32_t id = *need_id(c, 0, "GetNpadStyleSet"), style = 0;
    plat_mutex_lock_shared(&lock);
    uint32_t v = id_player(id, &style) >= 0 ? styles & style : 0;
    plat_mutex_unlock_shared(&lock);
    ret_x(c, v);
}

// int GetNpadStates(<State> *out, int count, const NpadIdType &id): the newest state only. want: the style of the
// overload, 0 for the Joy-Con overloads (never connected).
static void npad_states(Cpu *c, uint32_t want) {
    NpadState *out = arg_p(c, 0);
    int max = (int)arg_x(c, 1);
    uint32_t id = *need_id(c, 2, "GetNpadStates");
    if (max <= 0) { ret_x(c, 0); return; }
    if (!out) rt_fatal("nn::hid::GetNpadStates: null output");
    plat_mutex_lock(&lock);
    NpadState s = {.sampling = ++sample};
    uint32_t style = 0;
    int player = want ? id_player(id, &style) : -1;
    int live = player >= 0 && want == style && (styles & want);
    plat_mutex_unlock(&lock);
    if (live) {
        s.attributes = ATTR_CONNECTED;
        input_poll_player(player, &s.buttons, &s.lx, &s.ly, &s.rx, &s.ry);
    }
    *out = s;
    ret_x(c, 1);
}
static void ov_states_full_key(Cpu *c) { npad_states(c, STYLE_FULL_KEY); }
static void ov_states_handheld(Cpu *c) { npad_states(c, STYLE_HANDHELD); }
static void ov_states_joycon(Cpu *c) { npad_states(c, 0); }

// void SetNpadJoyHoldType(NpadJoyHoldType type)
static void ov_hold_type(Cpu *c) {
    int64_t t = (int32_t)arg_x(c, 0);
    if (t < 0 || t > 1) rt_fatal("nn::hid::SetNpadJoyHoldType: invalid hold type %lld", (long long)t);
}

// void SwapNpadAssignment(const NpadIdType &a, const NpadIdType &b): the two ids trade controllers (the game calls it
// when players swap seats in the menus; on the host it reached nn::sm through the hid service, issue #123)
static void ov_swap_assignment(Cpu *c) {
    uint32_t a = *need_id(c, 0, "SwapNpadAssignment"), b = *need_id(c, 1, "SwapNpadAssignment");
    if (a >= 8 || b >= 8) return;   // the handheld id has a single controller
    plat_mutex_lock(&lock);
    int8_t t = assigned[a];
    assigned[a] = assigned[b], assigned[b] = t;
    plat_mutex_unlock(&lock);
    rt_log("hid: controller ids %u and %u swapped controllers", a, b);
}

// void DisconnectNpad(const NpadIdType &id) (handheld mode disconnects player 1's id 0): until the controller applet
// connects the controllers again
static void ov_disconnect(Cpu *c) {
    int bit = id_bit(*need_id(c, 0, "DisconnectNpad"));
    if (bit < 0) return;
    plat_mutex_lock(&lock);
    disconnected |= 1u << bit;
    plat_mutex_unlock(&lock);
}

// Result ShowControllerSupport(ControllerSupportResultInfo *out, const ControllerSupportArg &arg): the controllers are
// already connected, so the applet returns at once with as many players as have one, up to the arg's maximum. Fewer
// than its minimum is what a player who closes the applet gets: no players (the game goes back). ControllerSupportArg
// (libnx HidLaControllerSupportArg; MKW fills it at main+0x25da6c8): s8 minimum and maximum players at 0 and 1.
static void ov_controller_support(Cpu *c) {
    ControllerSupportResult *out = arg_p(c, 0);
    const int8_t *arg = arg_p(c, 1);
    if (!arg) rt_fatal("nn::hid::ShowControllerSupport: null arg");
    int lo = arg[0] < 1 ? 1 : arg[0], hi = arg[1] < 1 || arg[1] > INPUT_MAX_PLAYERS ? INPUT_MAX_PLAYERS : arg[1];
    uint32_t mask = input_player_mask();
    int have = 0;
    while (have < hi && (mask >> have & 1)) have++;   // players 1..have, without a gap
    int players = have >= lo ? have : 0;
    plat_mutex_lock(&lock);
    if (players) multi = players > 1, disconnected = 0;
    uint32_t selected = pad_id();
    plat_mutex_unlock(&lock);
    rt_log("hid: controller applet asks for %d-%d players, %d with a controller: %d players%s", arg[0], arg[1], have,
           players, players ? "" : " (connect more controllers: the game takes it as closed)");
    if (!players) {   // the game goes back to its title without a word (#106, #160): say why on screen
        char text[256];
        snprintf(text, sizeof text, menu_text(UI_NOTICE_PADS_MISSING), lo, have);
        menu_notice(text, 8);
    }
    if (out) *out = (ControllerSupportResult){.players = (int8_t)players, .selected = selected};
    ret_x(c, 0);
}

// ---- vibration and six-axis sensor (MacNativeGames hid_feedback.c)
// Handles are the SDK's (nn::hid::utility, sdk+0x4a6e00 and sdk+0x4a6ab0): byte 0 the style index (3 full-key,
// 4 handheld), byte 1 the controller id, byte 2 the device (vibration 0 left / 1 right, six-axis 2). Every call under
// the public functions goes through nn::hid::detail (sdk's PLT), answered here instead of by the hid service, including
// the batches VibrationTarget queues through detail::EnqueueVibrationValue into detail::SendVibrationValues.
// VibrationValue {amplitude low, frequency low, amplitude high, frequency high}: 0..1 and Hz, neutral 0/160/0/320.
typedef struct { float amp_low, freq_low, amp_high, freq_high; } VibrationValue;
typedef struct { uint32_t type, position; } VibrationDeviceInfo;   // type 1 linear resonant actuator; position 1 left, 2 right
_Static_assert(sizeof(VibrationValue) == 16 && sizeof(VibrationDeviceInfo) == 8, "vibration ABI");

enum { STYLE_INDEX_FULL_KEY = 3, STYLE_INDEX_HANDHELD = 4, SIX_AXIS_DEVICE = 2, RUMBLE_MS = 250 };
#define RUMBLE_STALE_NS 250000000ull     // a side the game has not updated for 250 ms is silent
#define RUMBLE_REFRESH_NS 50000000ull    // an unchanged value is sent again every 50 ms (its 250 ms start over)
#define RUMBLE_MIN_GAP_NS 10000000ull    // a changed value at most every 10 ms (the game streams every few ms)
static const VibrationValue vib_neutral = {0, 160, 0, 320};

static int env_zero(const char *name) {
    const char *e = cfg_get(name);
    return e && !strcmp(e, "0");
}

static void once(volatile int32_t *flag, const char *fmt, uint32_t v) {
    if (!plat_xchg32(flag, 1)) rt_log(fmt, v);
}

// MKW_RUMBLE_STRENGTH (percent, 0..300, default 130; 0 = off) and MKW_RUMBLE_CURVE (exponent on the amplitude, 0.2..1.5,
// default 0.6). HD rumble is strong at low amplitude, an ERM motor hardly turns there, so weak values are lifted by the
// curve (0.1 -> 0.25) and the strength scales the result (saturating at 65535; 0.66 and up reach full drive at 130 %).
// Both are live settings (the menu): read again whenever cfg_version changes.
static float rumble_strength = 1.3f, rumble_curve = 0.6f;
static volatile int32_t rumble_cfg_logged;
static unsigned rumble_cfg_seen = ~0u;
static float cfg_float(const char *name, float lo, float hi) {
    double v = cfg_double(name);
    return (float)(v < lo ? lo : v > hi ? hi : v);
}
static void rumble_cfg(void) {
    unsigned v = cfg_version();
    if (v == rumble_cfg_seen) return;
    rumble_cfg_seen = v;
    rumble_strength = cfg_float("MKW_RUMBLE_STRENGTH", 0, 300) / 100;
    rumble_curve = cfg_float("MKW_RUMBLE_CURVE", 0.2f, 1.5f);
    int trig = input_rumble_triggers_enabled();
    if (!plat_xchg32(&rumble_cfg_logged, 1))
        rt_log("hid: rumble strength %.0f%%, curve %.2f, triggers %s", rumble_strength * 100, rumble_curve, trig ? "on" : "off");
}
static uint16_t motor(float a) { return rumble_motor(a, rumble_curve, rumble_strength); }

// The player on the controller a vibration or six-axis handle names (its style index and id), -1 for none.
static int handle_player(uint32_t v) {
    uint32_t style = 0;
    plat_mutex_lock_shared(&lock);
    int player = id_player(v >> 8 & 0xff, &style);
    plat_mutex_unlock_shared(&lock);
    uint32_t index = style == STYLE_HANDHELD ? STYLE_INDEX_HANDHELD : STYLE_INDEX_FULL_KEY;
    return player >= 0 && (v & 0xff) == index ? player : -1;
}

// The SDK's id check (utility functions): players 1-8, other (0x10) and handheld (0x20); the public functions abort
// on another id.
static uint32_t handle_id(Cpu *c, const char *fn) {
    uint32_t id = *need_id(c, 2, fn);
    if (id >= 8 && id != 0x10 && id != 0x20) rt_fatal("nn::hid::%s: invalid controller id %#x", fn, id);
    return id;
}

// int GetVibrationDeviceHandles(VibrationDeviceHandle *out, int count, const NpadIdType &id, NpadStyleSet style):
// utility::GetVibrationDeviceHandles' table, the first style bit in its order; none with MKW_RUMBLE=0.
static void ov_vibration_handles(Cpu *c) {
    uint32_t *out = arg_p(c, 0), style = (uint32_t)arg_x(c, 3);
    int max = (int)arg_x(c, 1);
    uint32_t id = handle_id(c, "GetVibrationDeviceHandles");
    if (!out || max <= 0 || __builtin_popcount(style) != 1)
        rt_fatal("nn::hid::GetVibrationDeviceHandles: invalid arguments (count %d, styles %#x)", max, style);
    static volatile int32_t asked;
    rumble_cfg();
    // strength 0 keeps the devices (silent), so the menu can turn rumble up again
    if (env_zero("MKW_RUMBLE")) { once(&asked, "hid: no vibration devices (MKW_RUMBLE=%u)", 0); ret_x(c, 0); return; }
    static const struct { uint8_t bit, n; uint32_t base; } t[] = {
        {0, 2, 0x3}, {1, 2, 0x4}, {2, 2, 0x5}, {3, 1, 0x6}, {4, 1, 0x10007}, {5, 1, 0x8}, {10, 1, 0xd}, {8, 2, 0x4},
        {30, 2, 0x21}, {29, 2, 0x20}};
    int n = 0;
    for (size_t i = 0; i < sizeof t / sizeof *t && !n; i++) {
        if (!(style >> t[i].bit & 1)) continue;
        n = t[i].n < max ? t[i].n : max;
        for (int k = 0; k < n; k++) out[k] = (t[i].base | id << 8) + ((uint32_t)k << 16);
    }
    once(&asked, "hid: the game asked for its vibration devices (first handle %#x)", n ? out[0] : 0);
    ret_x(c, (uint64_t)n);
}

// int GetSixAxisSensorHandles(SixAxisSensorHandle *out, int count, const NpadIdType &id, NpadStyleSet style):
// utility::GetSixAxisSensorHandle's order (one sensor per style, two for the Joy-Con pair). Also with MKW_MOTION=0: the
// sensor then reads at rest (six_fill), so the menu can turn motion on while the game runs.
static void ov_six_axis_handles(Cpu *c) {
    uint32_t *out = arg_p(c, 0), style = (uint32_t)arg_x(c, 3);
    int max = (int)arg_x(c, 1);
    uint32_t id = handle_id(c, "GetSixAxisSensorHandles");
    if (!out || max <= 0) rt_fatal("nn::hid::GetSixAxisSensorHandles: invalid arguments (count %d)", max);
    static volatile int32_t asked;
    uint32_t base = 0, n = 1;
    if (style & 1) base = 0x20003;
    else if (style & 2) base = 0x20004;
    else if (style & 4) base = 0x5, n = 2;
    else if (style & 8) base = 0x6;
    else if (style & 0x10) base = 0x10007;
    else if (style & 0x20) base = 0x20003;
    else if (style & 0xe80) n = 0;
    else if (style & 0x100) base = 0x20004;
    else if ((style & 0x60000040) == 0x40) base = 0x20003;
    else n = 0;
    if (n > (uint32_t)max) n = (uint32_t)max;
    for (uint32_t k = 0; k < n; k++) out[k] = (base | id << 8) + (k << 16);
    once(&asked, "hid: the game asked for its six-axis sensor (first handle %#x)", n ? out[0] : 0);
    ret_x(c, n);
}

static const uint32_t *need_handle(const void *p, const char *fn) {
    if (!p) rt_fatal("nn::hid::detail::%s: null handle", fn);
    return p;
}

// The vibration device a handle names on a connected controller, player * 2 + side (0 left, 1 right), or -1 (logged
// once).
static int vibration_side(const uint32_t *h, const char *fn) {
    uint32_t v = *need_handle(h, fn), dev = v >> 16 & 0xff;
    int player = dev < 2 ? handle_player(v) : -1;
    if (player >= 0) return player * 2 + (int)dev;
    static volatile int32_t warned;
    once(&warned, "hid: vibration call for handle %#x, not a device of the connected controller: ignored", v);
    return -1;
}

static PlatMutex vib_lock = PLAT_MUTEX_INIT;
enum { VIB_DEVICES = INPUT_MAX_PLAYERS * 2 };                                // vibration_side's index
static VibrationValue vib_last[VIB_DEVICES];                                 // vib_lock; vib_neutral until sent
static uint64_t vib_at[VIB_DEVICES], sent_at[INPUT_MAX_PLAYERS];             // vib_lock
static uint16_t sent_low[INPUT_MAX_PLAYERS], sent_high[INPUT_MAX_PLAYERS];   // vib_lock
static int vib_allowed = 1;                                                  // vib_lock: PermitVibration

static float clamp01(float v) { return v > 0 ? (v < 1 ? v : 1) : 0; }   // NaN -> 0
// Share of a band at f Hz that goes to the light (high-frequency, right) motor: none at 160 Hz and below (the low band's
// neutral frequency), all at 320 Hz and above (the high band's), log scale between.
static float light_share(float f) { return f > 160 ? (f < 320 ? log2f(f / 160) : 1) : 0; }

// Switch HD rumble -> SDL_RumbleGamepad. Each side's two bands split by frequency between the heavy (low) and the light
// (high) motor (light_share) and add up as energy per motor (root of the summed squares); the stronger side drives each
// motor. A side the game has not updated for 250 ms is silent. Each send lasts 250 ms and is renewed every 50 ms while
// the game streams, so the pad stops by itself when the game stops sending (pause, freeze). Under vib_lock.
static void rumble_update(int player, uint64_t now) {
    float heavy = 0, light = 0;
    rumble_cfg();
    for (int s = player * 2; s < player * 2 + 2 && vib_allowed; s++) {
        if (!vib_at[s] || now - vib_at[s] > RUMBLE_STALE_NS) continue;
        const VibrationValue *v = &vib_last[s];
        float al = clamp01(v->amp_low), ah = clamp01(v->amp_high), sl = light_share(v->freq_low), sh = light_share(v->freq_high);
        float h = sqrtf(al * (1 - sl) * al * (1 - sl) + ah * (1 - sh) * ah * (1 - sh));
        float l = sqrtf(al * sl * al * sl + ah * sh * ah * sh);
        if (h > heavy) heavy = h;
        if (l > light) light = l;
    }
    uint16_t lo = motor(clamp01(heavy)), hi = motor(clamp01(light));
    int changed = lo != sent_low[player] || hi != sent_high[player], on = lo || hi;
    if (changed ? on && now - sent_at[player] < RUMBLE_MIN_GAP_NS : !on || now - sent_at[player] < RUMBLE_REFRESH_NS) return;
    input_rumble(player, lo, hi, on ? RUMBLE_MS : 0);
    sent_low[player] = lo, sent_high[player] = hi, sent_at[player] = now;
}

static void play(int side, const VibrationValue *v) {
    if (!v) rt_fatal("nn::hid::detail::SendVibrationValue: null value");
    if (side < 0) return;
    VibrationValue c = *v;
    if (!isfinite(c.amp_low) || !isfinite(c.amp_high) || !isfinite(c.freq_low) || !isfinite(c.freq_high)) c = vib_neutral;
    static volatile int32_t first, first_on;
    if (!plat_xchg32(&first, 1))
        rt_log("hid: first vibration value from the game: %s %.2f@%.0f Hz %.2f@%.0f Hz", side & 1 ? "right" : "left",
               (double)c.amp_low, (double)c.freq_low, (double)c.amp_high, (double)c.freq_high);
    if ((c.amp_low > 0.01f || c.amp_high > 0.01f) && !plat_xchg32(&first_on, 1))
        rt_log("hid: first non-zero vibration from the game: %s %.2f@%.0f Hz %.2f@%.0f Hz", side & 1 ? "right" : "left",
               (double)c.amp_low, (double)c.freq_low, (double)c.amp_high, (double)c.freq_high);
    uint64_t now = kern_now_ns();
    plat_mutex_lock(&vib_lock);
    vib_last[side] = c;
    vib_at[side] = now;
    rumble_update(side / 2, now);
    plat_mutex_unlock(&vib_lock);
}

// Result detail::InitializeVibrationDevice(const VibrationDeviceHandle &)
static void ov_vib_init(Cpu *c) {
    static volatile int32_t logged;
    const uint32_t *h = arg_p(c, 0);
    if (vibration_side(h, "InitializeVibrationDevice") >= 0)
        once(&logged, "hid: the game initialized its vibration devices (handle %#x)", *h);
    ret_x(c, 0);
}

// Result detail::SendVibrationValue(const VibrationDeviceHandle &, const VibrationValue &)
static void ov_vib_send(Cpu *c) {
    play(vibration_side(arg_p(c, 0), "SendVibrationValue"), arg_p(c, 1));
    ret_x(c, 0);
}

// Result detail::SendVibrationValues(const VibrationDeviceHandle *, const VibrationValue *, int count)
static void ov_vib_send_many(Cpu *c) {
    const uint32_t *h = arg_p(c, 0);
    const VibrationValue *v = arg_p(c, 1);
    int n = (int)arg_x(c, 2);
    if (n < 0 || (n && (!h || !v))) rt_fatal("nn::hid::detail::SendVibrationValues: invalid batch (%d values)", n);
    for (int i = 0; i < n; i++) play(vibration_side(&h[i], "SendVibrationValues"), &v[i]);
    ret_x(c, 0);
}

// Result detail::GetActualVibrationValue(VibrationValue *out, const VibrationDeviceHandle &): the last value sent
static void ov_vib_actual(Cpu *c) {
    VibrationValue *out = arg_p(c, 0);
    if (!out) rt_fatal("nn::hid::detail::GetActualVibrationValue: null output");
    int side = vibration_side(arg_p(c, 1), "GetActualVibrationValue");
    VibrationValue v = vib_neutral;
    plat_mutex_lock_shared(&vib_lock);
    if (side >= 0 && vib_at[side]) v = vib_last[side];
    plat_mutex_unlock_shared(&vib_lock);
    *out = v;
    ret_x(c, 0);
}

// Result detail::GetVibrationDeviceInfo(VibrationDeviceInfo *out, const VibrationDeviceHandle &)
static void ov_vib_info(Cpu *c) {
    VibrationDeviceInfo *out = arg_p(c, 0);
    if (!out) rt_fatal("nn::hid::detail::GetVibrationDeviceInfo: null output");
    int side = vibration_side(arg_p(c, 1), "GetVibrationDeviceInfo");
    *out = side >= 0 ? (VibrationDeviceInfo){1, (uint32_t)(side & 1) + 1} : (VibrationDeviceInfo){0, 0};
    ret_x(c, 0);
}

// Result detail::IsVibrationDeviceMounted(bool *out, const VibrationDeviceHandle &)
static void ov_vib_mounted(Cpu *c) {
    uint8_t *out = arg_p(c, 0);
    if (!out) rt_fatal("nn::hid::detail::IsVibrationDeviceMounted: null output");
    *out = vibration_side(arg_p(c, 1), "IsVibrationDeviceMounted") >= 0;
    ret_x(c, 0);
}

// Result detail::IsVibrationPermitted(bool *out)
static void ov_vib_permitted(Cpu *c) {
    uint8_t *out = arg_p(c, 0);
    if (!out) rt_fatal("nn::hid::detail::IsVibrationPermitted: null output");
    plat_mutex_lock_shared(&vib_lock);
    *out = (uint8_t)vib_allowed;
    plat_mutex_unlock_shared(&vib_lock);
    ret_x(c, 0);
}

// Result detail::PermitVibration(bool): off silences the gamepad at once
static void ov_vib_permit(Cpu *c) {
    int on = (arg_x(c, 0) & 0xff) != 0;
    plat_mutex_lock(&vib_lock);
    vib_allowed = on;
    uint64_t now = kern_now_ns();
    for (int p = 0; p < INPUT_MAX_PLAYERS; p++) rumble_update(p, now);
    plat_mutex_unlock(&vib_lock);
    ret_x(c, 0);
}

static volatile int32_t six_running;   // StartSixAxisSensor .. StopSixAxisSensor

static int six_axis_ours(const uint32_t *h, const char *fn) {
    uint32_t v = *need_handle(h, fn);
    if ((v >> 16 & 0xff) == SIX_AXIS_DEVICE && handle_player(v) == 0) return 1;
    static volatile int32_t warned;
    once(&warned, "hid: six-axis call for handle %#x, not the connected controller's sensor: reads at rest", v);
    return 0;
}

// Result detail::StartSixAxisSensor(const SixAxisSensorHandle &): angle and direction start over
static void ov_six_start(Cpu *c) {
    const uint32_t *h = arg_p(c, 0);
    if (six_axis_ours(h, "StartSixAxisSensor")) {
        static volatile int32_t logged;
        once(&logged, "hid: the game started the six-axis sensor (handle %#x)", *h);
        motion_restart(motion_global());
        input_motion_start();
        plat_xchg32(&six_running, 1);
    }
    ret_x(c, 0);
}

// Result detail::StopSixAxisSensor(const SixAxisSensorHandle &)
static void ov_six_stop(Cpu *c) {
    if (six_axis_ours(arg_p(c, 0), "StopSixAxisSensor")) plat_xchg32(&six_running, 0);
    ret_x(c, 0);
}

// All max entries, newest first: the game reads all 16 it asks for and ignores the count (main+0x2a268e0). A short
// history repeats its oldest sample. No motion (sensor not started, no gamepad, one without a gyro, MKW_MOTION=0):
// a controller lying still, not marked connected.
static int six_fill(MotionState *s, int max, const uint32_t *h, const char *fn) {
    if (max <= 0) return 0;
    if (!s) rt_fatal("nn::hid::detail::%s: null output", fn);
    int live = six_axis_ours(h, fn) && six_running && input_motion_enabled();
    int n = live ? motion_read(motion_global(), s, max, kern_now_ns()) : 0;
    if (!n) {
        static const MotionState rest = {.acc = {0, 0, -1}, .dir = {1, 0, 0, 0, 1, 0, 0, 0, 1}};
        for (int i = 0; i < max; i++) s[i] = rest;
    } else
        for (int i = n; i < max; i++) s[i] = s[n - 1];
    static volatile int32_t first_rest, first_live;
    if (!n && six_running) once(&first_rest, "hid: six-axis read with no motion samples: at rest (%u states)", (uint32_t)max);
    if (n) once(&first_live, "hid: first six-axis read with live motion (%u samples)", (uint32_t)n);
    return max;
}

// Result detail::GetSixAxisSensorStates(int *count, SixAxisSensorState *out, int max, const SixAxisSensorHandle &)
static void ov_six_states(Cpu *c) {
    int32_t *count = arg_p(c, 0);
    if (!count) rt_fatal("nn::hid::detail::GetSixAxisSensorStates: null count");
    *count = six_fill(arg_p(c, 1), (int)arg_x(c, 2), arg_p(c, 3), "GetSixAxisSensorStates");
    ret_x(c, 0);
}

// Result detail::GetSixAxisSensorState(SixAxisSensorState *out, const SixAxisSensorHandle &)
static void ov_six_state(Cpu *c) {
    six_fill(arg_p(c, 0), 1, arg_p(c, 1), "GetSixAxisSensorState");
    ret_x(c, 0);
}

HOST_OVERRIDE("_ZN2nn3hid14InitializeNpadEv", ov_initialize_npad);
HOST_OVERRIDE("_ZN2nn3hid22SetSupportedNpadIdTypeEPKjm", ov_supported_ids);
HOST_OVERRIDE("_ZN2nn3hid24SetSupportedNpadStyleSetENS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", ov_supported_styles);
HOST_OVERRIDE("_ZN2nn3hid15GetNpadStyleSetERKj", ov_style_set);
HOST_OVERRIDE("_ZN2nn3hid13GetNpadStatesEPNS0_16NpadFullKeyStateEiRKj", ov_states_full_key);
HOST_OVERRIDE("_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyDualStateEiRKj", ov_states_joycon);
HOST_OVERRIDE("_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyLeftStateEiRKj", ov_states_joycon);
HOST_OVERRIDE("_ZN2nn3hid13GetNpadStatesEPNS0_17NpadJoyRightStateEiRKj", ov_states_joycon);
HOST_OVERRIDE("_ZN2nn3hid13GetNpadStatesEPNS0_17NpadHandheldStateEiRKj", ov_states_handheld);
HOST_OVERRIDE("_ZN2nn3hid18SetNpadJoyHoldTypeENS0_15NpadJoyHoldTypeE", ov_hold_type);
HOST_OVERRIDE("_ZN2nn3hid14DisconnectNpadERKj", ov_disconnect);
HOST_OVERRIDE("_ZN2nn3hid18SwapNpadAssignmentERKjS2_", ov_swap_assignment);
HOST_OVERRIDE("_ZN2nn3hid21ShowControllerSupportEPNS0_27ControllerSupportResultInfoERKNS0_20ControllerSupportArgE",
              ov_controller_support);
HOST_OVERRIDE("_ZN2nn3hid25GetVibrationDeviceHandlesEPNS0_21VibrationDeviceHandleEiRKjNS_4util10BitFlagSetILi32ENS0_"
              "12NpadStyleTagEEE", ov_vibration_handles);
HOST_OVERRIDE("_ZN2nn3hid23GetSixAxisSensorHandlesEPNS0_19SixAxisSensorHandleEiRKjNS_4util10BitFlagSetILi32ENS0_"
              "12NpadStyleTagEEE", ov_six_axis_handles);
HOST_OVERRIDE("_ZN2nn3hid6detail25InitializeVibrationDeviceERKNS0_21VibrationDeviceHandleE", ov_vib_init);
HOST_OVERRIDE("_ZN2nn3hid6detail18SendVibrationValueERKNS0_21VibrationDeviceHandleERKNS0_14VibrationValueE", ov_vib_send);
HOST_OVERRIDE("_ZN2nn3hid6detail19SendVibrationValuesEPKNS0_21VibrationDeviceHandleEPKNS0_14VibrationValueEi",
              ov_vib_send_many);
HOST_OVERRIDE("_ZN2nn3hid6detail23GetActualVibrationValueEPNS0_14VibrationValueERKNS0_21VibrationDeviceHandleE",
              ov_vib_actual);
HOST_OVERRIDE("_ZN2nn3hid6detail22GetVibrationDeviceInfoEPNS0_19VibrationDeviceInfoERKNS0_21VibrationDeviceHandleE",
              ov_vib_info);
HOST_OVERRIDE("_ZN2nn3hid6detail24IsVibrationDeviceMountedEPbRKNS0_21VibrationDeviceHandleE", ov_vib_mounted);
HOST_OVERRIDE("_ZN2nn3hid6detail20IsVibrationPermittedEPb", ov_vib_permitted);
HOST_OVERRIDE("_ZN2nn3hid6detail15PermitVibrationEb", ov_vib_permit);
HOST_OVERRIDE("_ZN2nn3hid6detail18StartSixAxisSensorERKNS0_19SixAxisSensorHandleE", ov_six_start);
HOST_OVERRIDE("_ZN2nn3hid6detail17StopSixAxisSensorERKNS0_19SixAxisSensorHandleE", ov_six_stop);
HOST_OVERRIDE("_ZN2nn3hid6detail22GetSixAxisSensorStatesEPiPNS0_18SixAxisSensorStateEiRKNS0_19SixAxisSensorHandleE",
              ov_six_states);
HOST_OVERRIDE("_ZN2nn3hid6detail21GetSixAxisSensorStateEPNS0_18SixAxisSensorStateERKNS0_19SixAxisSensorHandleE",
              ov_six_state);

// ---- 1.8.0's mouse APIs (none in 1.0.0): the Joy-Con 2 as a mouse (Jcms) and a USB mouse (Nmouse). A PC is a
// console with neither attached: Jcms handles exist but report no samples, and there is no mouse. Their SDK versions
// open the hid server through sm (svc 0x1f, IPC), which this runtime does not have: the first 1.8.0 boot stopped in
// nn::hid::InitializeJcms (main+0x412728).
static void ov_mouse_nothing(Cpu *c) { ret_x(c, 0); }
// JcmsHandle GetJcmsHandle(const NpadId &, const JcmsType &): a u32 (main+0x2590f30 stores w0); id << 8 | type
static void ov_jcms_handle(Cpu *c) {
    const uint32_t *id = arg_p(c, 0);
    const uint8_t *type = arg_p(c, 1);
    ret_x(c, (id ? *id : 0) << 8 | (type ? *type : 0));
}
// int GetNmouseStates(int *count, NmouseState *, int max, NmouseHandle): no samples
static void ov_nmouse_states(Cpu *c) {
    int32_t *count = arg_p(c, 0);
    if (count) *count = 0;
    ret_x(c, 0);
}
// void GetNmouseTypeSet(BitFlagSet<32, NmouseType> *, NmouseHandle): no type
static void ov_nmouse_types(Cpu *c) {
    uint32_t *set = arg_p(c, 0);
    if (set) *set = 0;
}
HOST_OVERRIDE("_ZN2nn3hid14InitializeJcmsEPvm", ov_mouse_nothing);
HOST_OVERRIDE("_ZN2nn3hid12FinalizeJcmsEv", ov_mouse_nothing);
HOST_OVERRIDE("_ZN2nn3hid13GetJcmsHandleERKjRKNS0_8JcmsTypeE", ov_jcms_handle);
HOST_OVERRIDE("_ZN2nn3hid9StartJcmsERKNS0_10JcmsHandleE", ov_mouse_nothing);
HOST_OVERRIDE("_ZN2nn3hid8StopJcmsERKNS0_10JcmsHandleE", ov_mouse_nothing);
HOST_OVERRIDE("_ZN2nn3hid13GetJcmsStatesEPNS0_9JcmsStateEiRKNS0_10JcmsHandleE", ov_mouse_nothing);   // 0 samples
HOST_OVERRIDE("_ZN2nn3hid16InitializeNmouseEPvmii", ov_mouse_nothing);
HOST_OVERRIDE("_ZN2nn3hid14FinalizeNmouseEv", ov_mouse_nothing);
HOST_OVERRIDE("_ZN2nn3hid16GetNmouseHandlesEPNS0_12NmouseHandleEij", ov_mouse_nothing);   // 0 mice
HOST_OVERRIDE("_ZN2nn3hid15GetNmouseStatesEPiPNS0_11NmouseStateEiNS0_12NmouseHandleE", ov_nmouse_states);
HOST_OVERRIDE("_ZN2nn3hid16GetNmouseTypeSetEPNS_4util10BitFlagSetILi32ENS0_10NmouseTypeEEENS0_12NmouseHandleE",
              ov_nmouse_types);
HOST_OVERRIDE("_ZN2nn3hid19EnableNmousePointerENS0_12NmouseHandleEb", ov_mouse_nothing);
HOST_OVERRIDE("_ZN2nn3hid22SetNmousePointerRangesEii", ov_mouse_nothing);
HOST_OVERRIDE("_ZN2nn3hid24SetNmousePointerPositionENS0_12NmouseHandleEii", ov_mouse_nothing);
