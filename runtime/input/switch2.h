// Switch 2 controllers (Pro Controller 2, Joy-Con 2) over Bluetooth LE, which SDL does not drive (it has them over USB
// only). A transport finds and connects them (linux_switch2.c: BlueZ over D-Bus; switch2_win.c: WinRT) and hands each
// one to switch2.c, which speaks the protocol (switch2_proto.h, from mkw-neo) and shows the controller to the rest of
// the input as a virtual SDL gamepad: players, button layout, gyro and rumble then work as for any other gamepad.
// MKW_SWITCH2=1 (Settings > Controls) turns it on; it runs on the transport's own thread, never the game's.
#pragma once
#include <stddef.h>
#include <stdint.h>

void switch2_start(void);   // input.c's start: the transport's thread, when MKW_SWITCH2=1
// The battery of the virtual gamepad with this SDL joystick id (SDL keeps no power state for a virtual one): its band
// (S2_BATTERY_*, switch2_proto.h; 0 not ours or none reported yet) and *mv its voltage. Any thread.
int switch2_battery(uint32_t sdl_id, int *mv);

// ---- between a transport and switch2.c

typedef struct S2Link S2Link;   // one connected controller, the transport's
typedef struct S2Session S2Session;

typedef struct {
    int (*write_cmd)(S2Link *l, const uint8_t *d, size_t n);   // to S2_UUID_CMD_WR, without response; 0 on failure
    int (*write_vib)(S2Link *l, const uint8_t *d, size_t n);   // to the vibration characteristic, if it has one
    int (*start_input)(S2Link *l);                              // notifications of S2_UUID_INPUT on
    void (*pump)(int ms);   // the transport's events for up to ms (replies and reports reach the session from here)
    void (*log_link)(S2Link *l);   // optional: the link's own figures in the log (Windows: its connection interval)
} S2LinkOps;

uint64_t switch2_now_ns(void);
// A controller connected, its characteristics found and replies subscribed: its session (replies reach it from here
// on), then the handshake (identity, calibration, pairing when use is S2_ADV_PAIR, features) and a gamepad. 0 when
// the handshake failed: the transport closes the session and disconnects it.
S2Session *switch2_new(S2Link *l, const S2LinkOps *ops, uint16_t pid, int use, const uint8_t host[6]);
int switch2_handshake(S2Session *s);
void switch2_reply(S2Session *s, const uint8_t *d, size_t n);               // a S2_UUID_CMD_RSP notification
void switch2_report(S2Session *s, const uint8_t *d, size_t n, uint64_t at);  // a S2_UUID_INPUT notification
void switch2_tick(void);                                                     // every ~15 ms: rumble, keep-alive
void switch2_close(S2Session *s);                                            // disconnected: its gamepad goes
