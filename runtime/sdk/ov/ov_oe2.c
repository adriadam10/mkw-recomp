// nn::oe functions MKW imports that totk-pc-recomp does not replace (ov_system.c has the rest).
#include <string.h>

#include "host.h"
#include "version.h"

// The dump has no control.nacp, which the SDK reads the version from: the version of the copy the game code was
// generated from (version.c).

// void nn::oe::GetDisplayVersion(DisplayVersion *out): char[16]
static void ov_display_version(Cpu *c) {
    char *out = arg_p(c, 0);
    if (!out) return;
    const char *v = rt_version()->display;
    memset(out, 0, 16);
    memcpy(out, v, strlen(v) < 15 ? strlen(v) : 15);
}

// int nn::oe::GetCurrentGraphicsScale(): 10, 20 or 30 (eden-nx2 NX2_NOTES.md row 15); 10 until the scale is measured.
static void ov_graphics_scale(Cpu *c) { ret_x(c, 10); }

// Exit-request sections, server maintenance overlay, scale-change notifications: nothing for a host process to do.
static void ov_nothing(Cpu *c) { (void)c; }

HOST_OVERRIDE("_ZN2nn2oe17GetDisplayVersionEPNS0_14DisplayVersionE", ov_display_version);
HOST_OVERRIDE("_ZN2nn2oe23GetCurrentGraphicsScaleEv", ov_graphics_scale);
HOST_OVERRIDE("_ZN2nn2oe31EnterExitRequestHandlingSectionEv", ov_nothing);
HOST_OVERRIDE("_ZN2nn2oe31LeaveExitRequestHandlingSectionEv", ov_nothing);
HOST_OVERRIDE("_ZN2nn2oe40SendServerMaintenanceOverlayNotificationENS_4time9PosixTimeES2_", ov_nothing);
HOST_OVERRIDE("_ZN2nn2oe42SetGraphicsScaleChangedNotificationEnabledEb", ov_nothing);
