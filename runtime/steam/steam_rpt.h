// Steam Remote Play Together (steam_rpt.c): Steam streams the game's window and sound to friends, and each guest's
// controller or keyboard becomes a player of local multiplayer. Off unless MKW_STEAM_APPID (steam_appid in mkw.ini)
// names the Steam App ID to run as; Steam's library is loaded at run time from mkw's folder (steam_flat.h), so nothing
// changes for a player without it. docs/remote-play.md has the set-up.
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    STEAM_RPT_OFF,       // no App ID set
    STEAM_RPT_IDLE,      // an App ID is set but this process does not run the game (the start screen)
    STEAM_RPT_FAILED,    // Steam could not be used: steam_rpt_reason says why
    STEAM_RPT_READY,     // running as the App ID: guests can join
};

// Main thread, once, before the game boots (main.c): before any Vulkan device, so the Steam overlay can attach, and
// before the first gamepad scan, so SDL lists Steam's virtual gamepads. Starts the "steam" thread when Steam is ready.
void steam_rpt_start(void);

// Any thread.
int steam_rpt_state(void);
const char *steam_rpt_reason(void);   // STEAM_RPT_FAILED: why, in English, for the log and the menu; "" otherwise
const char *steam_rpt_app_id(void);   // the App ID set, "" when none
int steam_rpt_guests(void);           // guests connected now

// The menu's buttons (any thread; the steam thread carries them out). invite: the Steam overlay's Remote Play Together
// friends list. link: a guest link anyone can open (browser, Steam Link), which steam_rpt_take_link hands out once.
void steam_rpt_invite(void);
void steam_rpt_request_link(void);
// 1 with the new link in out (once per request), 0 while none came, -1 when Steam refused the request.
int steam_rpt_take_link(char *out, size_t cap);
// 1 after steam_rpt_invite found no overlay to show (cleared by the next request).
int steam_rpt_invite_failed(void);

// Steam Input's virtual gamepad (Valve's USB vendor id and the virtual pad's product id, as SDL's
// SDL_IsJoystickSteamVirtualGamepad tests them on Linux and Windows): what a guest's controller is on the host.
enum { STEAM_VENDOR_ID = 0x28DE, STEAM_VIRTUAL_PAD_ID = 0x11FF };

#ifdef __cplusplus
}
#endif
