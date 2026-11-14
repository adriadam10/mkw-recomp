// Phones as controllers (MKW_PHONE_PADS, Settings > Controls, off by default): a phone on the same network opens
// the address the menu shows (as a QR code too) in its browser and becomes a controller, with nothing to install, on
// Android and on iOS: buttons and a stick on its screen, and its accelerometer as a steering wheel. phonepad_srv.c is
// the HTTPS server on the LAN and phonepad_page.html the page it serves; phonepad.c gives each phone a virtual SDL
// gamepad with a gyro and rumble, so players, layout, remapping and motion work as for any gamepad (as switch2.c
// does for the Switch 2 controllers). It runs on a thread of its own, never the game's.
// MKW_PHONE_PADS=2 also logs every connection; MKW_PHONE_PORT is the port (6590).
#pragma once

// input.c: at its start and at every look for new gamepads. Starts the server's thread when the setting is on and it
// is not running; the thread ends by itself when the setting goes off (it is live).
void phonepad_start(void);

typedef struct {
    int on;             // the server answers
    int failed;         // the setting is on but there is no address to give: no local network, or the port is taken
    int port, pads;     // phones connected now
    char address[48];   // "https://192.168.1.5:6590", to type
    char code[8];       // what the page asks for
    char url[64];       // the address with the code ("https://192.168.1.5:6590/#4821"): what the QR code holds
} PhonePadInfo;
void phonepad_info(PhonePadInfo *out);   // any thread: the menu's rows
