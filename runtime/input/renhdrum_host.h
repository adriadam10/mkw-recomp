// RenHDrum (github.com/trzz1214/RenHDrum, MIT or Apache-2.0): the DualSense's haptics, and HD Rumble on a Pro Controller,
// an Xbox One S and a Steam Controller, from the game's own vibration values (amplitude and frequency of two bands per
// side), not the two motor strengths SDL_RumbleGamepad takes. Off by default (MKW_RENHDRUM=1, mkw.ini [input]); Windows
// only, and only when renhdrum.dll (a release asset of RenHDrum, 0.5.x) is beside the exe: without it, or with another
// version, every call here does nothing and the gamepad rumbles as before.
// Only player 1's gamepad: the bridge sends every value to every gamepad it holds, and each player has a vibration of
// their own. Called from input.c (start, gamepad opened and closed) and sdk/ov/ov_input.c (the game's values).
#pragma once

#ifdef _WIN32
void renhdrum_start(void);                     // input.c's start: loads renhdrum.dll when MKW_RENHDRUM=1; logs what it found
void renhdrum_attach(void *gamepad);           // player 1's SDL_Gamepad was opened
void renhdrum_detach(void *gamepad);           // ... and is about to be closed
void renhdrum_vibration(int side, const float v[4]);   // side 0 left 1 right; v = amplitude low, Hz low, amplitude high, Hz high
int renhdrum_claims(void);                     // 1 while RenHDrum plays player 1's gamepad: the SDL motors stay silent
void renhdrum_permit(int on);                  // PermitVibration: off silences it at once
void renhdrum_shutdown(void);                  // before the process ends (plat_exit): stops the worker and the haptics streams
#else
static inline void renhdrum_start(void) {}
static inline void renhdrum_attach(void *gamepad) { (void)gamepad; }
static inline void renhdrum_detach(void *gamepad) { (void)gamepad; }
static inline void renhdrum_vibration(int side, const float v[4]) { (void)side; (void)v; }
static inline int renhdrum_claims(void) { return 0; }
static inline void renhdrum_permit(int on) { (void)on; }
static inline void renhdrum_shutdown(void) {}
#endif
