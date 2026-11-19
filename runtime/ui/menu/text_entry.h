// The overlay's text entry (text_entry.cpp): an on-screen numeric keypad for the software keyboard (swkbd_sdl.c),
// drawn by the in-game menu's overlay (menu.cpp) and used with a controller, the mouse or the keyboard.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Shows the keypad and waits for the player, on the calling thread (any thread but the present path's). prompt and
// initial are UTF-8; only digits of initial are kept. Returns 1 with the digits in out (NUL-terminated), 0 when
// cancelled, -1 when no overlay draws it (no window, or no frame presented within 3 s): the caller answers otherwise.
int text_entry_keypad(const char *prompt, const char *initial, int min_len, int max_len, int password, char *out,
                      size_t cap);

// ---- for menu.cpp, present path
int text_entry_up(void);                                   // a keypad is waiting: lay out a frame, give it the input
void text_entry_draw(float width, float height, float scale);   // in the current ImGui frame
// 1 in the poll where it went up (menu.cpp drops stale input then)
int text_entry_opened(void);
void text_entry_test(uint64_t frame);   // MKW_KEYPAD_TEST=<frame>: asks for a code at that frame (each presented frame)

#ifdef __cplusplus
}
#endif
