// HD rumble through RenHDrum (hd_rumble.c): the game's vibration values, both bands with their frequencies, played on
// player 1's controller as the Switch plays them, instead of the two-motor mix of ov_input.c rumble_update.
#pragma once

// Player 1's gamepad (an SDL_Gamepad *) opened, and before it closes. Under input.c's lock.
void hd_rumble_attach(void *pad);
void hd_rumble_detach(void *pad);
// One side (0 left, 1 right) of player 1's controller: the game's VibrationValue as it came (no strength or curve:
// RenHDrum shapes it with its own settings). Returns 1 while RenHDrum drives the gamepad. Any thread.
int hd_rumble_play(int side, float amp_low, float freq_low, float amp_high, float freq_high);
// 1 while RenHDrum drives player 1's gamepad: input_rumble leaves its motors alone. Any thread.
int hd_rumble_owns(void);
// PermitVibration(false) silences the controller at once.
void hd_rumble_permit(int on);
// The process ends (gfx_window_closed; plat_exit runs no atexit handlers): stop the controller's rumble, speaker and
// trigger effects and RenHDrum's threads.
void hd_rumble_shutdown(void);
