// Cemuhook / DSU (DualShock UDP) motion client (dsu.c): the gyro and accelerometer a DSU server serves for a
// controller whose own driver has none (an XInput pad, a phone, a Joy-Con through BetterJoy...). MKW_DSU=1 (Settings >
// Controls, live) turns it on; MKW_DSU_SERVER (host:port, 127.0.0.1:26760 by default) and MKW_DSU_SLOT (the server's
// slot for player 1; player N takes slot + N - 1) say where. input.c's motion thread uses a player's DSU sample when
// that player's gamepad has no sensors of its own.
#pragma once

#include <stdint.h>

enum { DSU_SLOTS = 4 };   // the protocol's slots, 0-3

void dsu_start(void);   // input.c: the client's thread, once; it idles while MKW_DSU=0
// The newest sample of server slot `slot`, converted to the Switch controller frame motion_feed takes (dsu_axes);
// 1 when it is newer than 200 ms (the fusion's own staleness), 0 otherwise or while MKW_DSU=0.
int dsu_motion(int slot, double acc_g[3], double gyro_rad_s[3]);
// DSU axes and units to the Switch controller frame (acceleration in G as gravity reads it, rate in rad/s).
void dsu_axes(const float acc_g[3], const float gyro_deg_s[3], double acc[3], double gyro[3]);
// The protocol's packets, for the probe's fake server and the client: CRC32 (zlib's) over the packet with its CRC
// field zeroed; a pad data response of 100 bytes for slot with the given motion (DSU axes) and timestamp.
uint32_t dsu_crc32(const uint8_t *p, int n);
int dsu_pad_packet(uint8_t out[100], int slot, uint32_t number, uint64_t stamp_us, const float acc_g[3],
                   const float gyro_deg_s[3]);
