// Host input sampling (runtime/input.c, with input_sdl.c): SDL3 gamepad and the keyboard as one Switch npad.
#pragma once
#include <stddef.h>
#include <stdint.h>

// Bits of libnx HidNpadButton (A=0 B=1 X=2 Y=3 StickL=4 StickR=5 L=6 R=7 ZL=8 ZR=9 Plus=10 Minus=11 Left=12 Up=13 Right=14
// Down=15) and the stick directions (StickLLeft..Down 16-19, StickRLeft..Down 20-23, derived from the sticks).
// Sticks are -32767..32767, up is +y. MKW_INPUT=none reports no input. Callable from any thread.
void input_poll(uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry);
// The same for host UI (the text entry): never blocked. input_block(1) while a text entry is open: input_poll then
// reports nothing; input_block(0) when it closes: nothing until all buttons and stick directions are released.
void input_poll_ui(uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry);
void input_block(int on);
// Racing wheels (wheel.c, #488): input_wheel_hint before SDL's gamepad init reads gamecontrollerdb.txt from the game
// folder when there is one; input_wheel_map gives each wheel SDL has no mapping for a gamepad mapping, so
// SDL_GetGamepads lists it. Same thread as the other SDL gamepad calls.
void input_wheel_hint(void);
void input_wheel_map(void);
// Local multiplayer, players 1-8 (player index 0-7). Player 1 is input_poll: the keyboard, the first gamepad and the
// script. Each other player is one more gamepad, in connection order, plus the script's "p<N>" lines; MKW_VIRTUAL_PADS=N
// attaches N virtual gamepads at start to try it without controllers. Any thread.
enum { INPUT_MAX_PLAYERS = 8 };
// Players a and b (index 0-7) trade gamepads, live: either may have none. 0 and nothing done when neither has one, for a bad
// index, and for player 1 with MKW_PAD_P2 (theirs is the keyboard). Until the game closes. Any thread.
int input_pad_swap(int a, int b);
// 1 when player p's gamepad could go to the player before (a row of the menu's Controls tab).
int input_pad_can_move_up(int p);
uint32_t input_player_mask(void);   // bit k: player k + 1 has a controller; bit 0 always (the keyboard)
void input_poll_player(int player, uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry);
// The in-game menu (ui/menu/menu.h): input_menu(1) when it opens (input_poll reports nothing, the cursor is released), (0) when
// it closes (nothing until every button and stick direction is released). Independent of input_block.
void input_menu(int open);
// The gamepad alone, for the menu: Switch button bits and the left stick (up +y). 0 without a connected gamepad.
// Minus+Plus combo (or the Guide button when MKW_MENU_SHORTCUT=guide) and the neutral pad while the menu is open
// (input_map.c, window-system free): *held is the caller's memory of the combo, guide the Guide button held,
// sticks are lx, ly, rx, ry. Returns 1 in the poll where the shortcut goes down (toggle the menu once).
int input_menu_gate(int *held, int menu_open, uint64_t *buttons, int32_t sticks[4], int guide);
int input_overlay_gate(int *held, uint64_t *buttons);   // L3+R3: 1 in the poll both go down
int input_script_inject(const char *line);   // MKW_CTL: "<ticks from now> <button> [hold]", 1 when queued
void input_script_phase(const char *node);   // seqwatch.c: a phase started (script lines "<node>+<ticks> ...")
int input_pad_read(uint64_t *buttons, int32_t *lx, int32_t *ly);
// The device the player used last (a press, or a stick moved): the button icons follow it (ui/glyphs/glyphs.c). Before any
// input, the gamepad when one is connected. Any thread.
enum { INPUT_DEVICE_KEYBOARD = 1, INPUT_DEVICE_PAD = 2 };
int input_last_device(void);
// The open gamepad's default left stick dead zone in % when MKW_STICK_DEADZONE is unset (an Xbox pad's is Microsoft's,
// not the 8 % of the settings table); -1 without a gamepad. Any thread.
double input_stick_deadzone_default(void);
// The gamepad layout in effect (PAD_LAYOUT_*, input_map.h; MKW_PAD_LAYOUT, live); -1 without a gamepad. Any thread.
int input_pad_layout(void);
// The label on a face button of the open gamepad (input_map.h PAD_SOUTH..PAD_NORTH, as SDL reports it); PAD_LABEL_NONE for another
// button, an unknown label or no gamepad. Any thread.
enum { PAD_LABEL_NONE, PAD_LABEL_A, PAD_LABEL_B, PAD_LABEL_X, PAD_LABEL_Y, PAD_LABEL_CROSS, PAD_LABEL_CIRCLE,
       PAD_LABEL_SQUARE, PAD_LABEL_TRIANGLE };
int input_pad_label(int pad);
// The menu's remapping rows (Controls tab). input_map_snapshot copies the tables in effect (after any changed MKW_BIND_* line).
// input_capture polls the keys, the mouse buttons and the gamepad for a press: reset 1 takes what is held now as the
// start (a held key counts once released and pressed again) and returns NONE; then it returns KEY or PAD with *id the
// key or pad id (input_map.h) pressed since the last call, or CANCEL for Esc. Keys count only while the game window is in front.
enum { INPUT_CAPTURE_NONE, INPUT_CAPTURE_KEY, INPUT_CAPTURE_PAD, INPUT_CAPTURE_CANCEL };
struct InputMap;   // input_map.h
void input_map_snapshot(struct InputMap *m);
int input_capture(int reset, int *id);
// The character on a punctuation key (KEY_MINUS..KEY_GRAVE) in the keyboard layout in use, 0 for another key.
uint32_t input_key_char(int key);
// F8, the flight recorder's key (no KEY_ id), held while the game window is in front.
int input_flight_key(void);
// The hacks' keys (core/hacks/), held while the game window is in front: F2 turbo, F3 freeze, F4 one frame on.
enum { HACK_KEY_TURBO, HACK_KEY_FREEZE, HACK_KEY_STEP };
int input_hack_key(int which);

// Mouse camera and cursor (input_sdl.c, window thread). input_cursor_suspend(1) releases the cursor around an F11
// window change, (0) captures it again once the client area has its new size.
void input_cursor_suspend(int on);
int input_cursor_released(void);   // Esc or a non-client activation released the cursor (probe)

// Battery of the player's gamepad (index 0-7; input_power.c). state INPUT_POWER_*; percent 0-100, -1 when the gamepad
// gives none; coarse: percent is a band's (a Switch 2 controller over Bluetooth gives a voltage: 10 low, 50 medium,
// 90 high; an Xbox pad on Linux's xone driver a sysfs level: 10 low, 50 medium, 90 high). Returns state. Any thread.
enum { INPUT_POWER_NONE, INPUT_POWER_UNKNOWN, INPUT_POWER_BATTERY, INPUT_POWER_CHARGING, INPUT_POWER_CHARGED,
       INPUT_POWER_WIRED };
typedef struct { int state, percent, coarse; char name[64]; } InputPower;
int input_pad_power(int player, InputPower *out);
// The same from what SDL says of a gamepad (SDL_GetGamepadPowerInfo's state and percent, its joystick id): the start
// screen, which reads the gamepads without input.c. index is the pad's order among Xbox-named pads (other makes in
// earlier slots not counted): on Linux, when SDL reports UNKNOWN, an xone Xbox pad's sysfs level band fills it in.
// Keeps out->name.
int input_power_from_sdl(int sdl_state, int percent, uint32_t sdl_id, int index, InputPower *out);
#ifdef __linux__
// The player's order among Xbox-named gamepads (input.c, for the index above).
int input_pad_xbox_index(int player);
#endif
// The battery as the player reads it, in the menu language: "80 %", "Charging, 80 %", "Low"... (out holds 64 bytes).
const char *input_power_text(const InputPower *p, char *out, int out_size);
// input_power.c's watch (started with the gamepads): a notice when a player's battery runs low, once per discharge.
void input_power_start(void);
// The game's gamepad input is up (SDL gamepads opened by input.c): not on the start screen, which reads SDL itself.
int input_gamepads_running(void);
// A gamepad arrived or left (SDL_EVENT_GAMEPAD_ADDED/REMOVED): the start screen calls this from its event loop so the
// settings follow hotplug the same way; a no-op unless the gamepads are running (hotplug alone never starts them).
void input_hotplug(void);
// input.c, for input_power.c: SDL's SDL_PowerState of the player's gamepad (-2 none), its name, joystick id and percent.
int input_pad_power_sdl(int player, char *name, size_t name_size, uint32_t *id, int *percent);

// Rumble: SDL_RumbleGamepad on the player's gamepad (index 0-7), low (heavy, left) and high (light, right) motor
// 0..65535 for ms milliseconds. 1 if a gamepad with rumble took it. Callable from any thread.
int input_rumble(int player, uint16_t low, uint16_t high, uint32_t ms);
// Motor value for a normalised amplitude: pow(a, curve) * strength, saturated to 0..65535. Pure.
uint16_t rumble_motor(float a, float curve, float strength);
// MKW_RUMBLE_TRIGGERS=1: input_rumble also drives the impulse triggers (Xbox) with the high motor's value.
int input_rumble_triggers_enabled(void);

// Motion (MKW_MOTION=0 turns it off, live). input_motion_start starts the sensor thread once: every 4 ms it reads the
// gamepad's gyro and accelerometer (when it has them, else a DSU server's with MKW_DSU=1, dsu.h) and feeds each
// player's motion_player(). Callable from any thread.
void input_motion_start(void);
// MKW_MOTION is not "0" (a live setting: read again when cfg_version changes). Off: the sensor reads at rest.
int input_motion_enabled(void);
// SDL sensor values (accelerometer m/s^2, gyro rad/s, SDL gamepad axes) to the Switch controller frame motion_feed
// takes (acceleration in G as gravity reads, rotation rad/s).
void input_motion_axes(const float sdl_acc[3], const float sdl_gyro[3], double acc_g[3], double gyro[3]);

// motion_fusion.c: nn::hid::SixAxisSensorState and the sensor fusion that produces it.
typedef struct {
    int64_t delta_ns, sampling;
    float acc[3], gyro[3], angle[3], dir[9];   // G, revolutions/s, revolutions, controller axes in the reference frame
    uint32_t attributes, reserved;             // bit 0 connected
} MotionState;
_Static_assert(sizeof(MotionState) == 0x60, "SixAxisSensorState ABI");
typedef struct MotionFusion MotionFusion;
MotionFusion *motion_new(void);
MotionFusion *motion_player(int player);   // player 0-7's gamepad's, fed by input.c's motion thread
void motion_feed(MotionFusion *m, const double acc_g[3], const double gyro_rad_s[3], uint64_t now_ns);
int motion_read(MotionFusion *m, MotionState *out, int max, uint64_t now_ns);   // newest first; 0 = none in 200 ms
void motion_restart(MotionFusion *m);   // angle and direction start over at the next sample
