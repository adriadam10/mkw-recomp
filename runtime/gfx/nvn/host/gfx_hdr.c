// HDR output, as the console does it: Mario Kart World draws its own HDR picture when nn::vi says the display is in
// HDR10 (ov_gfx.c answers its queries from here), and the present hands that picture to an HDR10 swapchain.
// Why: players with HDR screens (#168) and Adrià's decisions of 12/11/2026: MKW_HDR auto (the default: HDR when the
// display is in HDR, as the console turns it on by itself) or off; peak and paper white from the system; followed live
// (the window's HDR event and the setting rebuild the swapchain, and the game asks again every frame).
// The display is in HDR when SDL says so (Windows, Wayland with colour management); where SDL cannot tell (X11, inside
// gamescope) when the surface offers HDR10, as gamescope does only with its HDR on.
// MKW_HDR_FAKE=1 answers HDR10 to the game on an SDR swapchain, to see what the game draws then on a machine without
// an HDR screen; the picture is then wrong.
// References: Hookshot and nx2navi (Switch 2 recompilations) answer nn::vi the same way for their games (format 1,
// HDR10) and pick the swapchain by its colour space; no code of them is taken.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "gfx.h"
#include "gfx_wsys.h"
#include "host.h"
#include "vk.h"

enum { NITS_SCRGB_ONE = 80, WHITE_DEFAULT = 203, PEAK_DEFAULT = 1000 };   // scRGB 1.0; BT.2408 paper white

static volatile int32_t g_on;              // the swapchain is HDR10
static volatile uint32_t g_peak, g_white;  // nits, as told to the game
static char g_cfg[8];                      // the MKW_HDR the swapchain was made with

// MKW_HDR_PEAK, MKW_HDR_WHITE: the player's nits in place of the system's ("auto"). Windows gives the SDR content
// brightness slider as paper white, often 400-500 nits on a bright screen: with a 1000-1200 nit peak that leaves the
// game's highlights little more than a stop above white (log 18/11/2026: peak 1200, paper white 480).
static uint32_t nits_setting(const char *key, uint32_t system) {
    int v = atoi(cfg_value(key));
    return v > 0 ? (uint32_t)v : system;
}

static void effective(uint32_t *peak, uint32_t *white) {
    uint32_t w = nits_setting("MKW_HDR_WHITE", g_white), p = nits_setting("MKW_HDR_PEAK", g_peak);
    *white = w, *peak = p < w ? w : p;
}

int gfx_hdr_output(uint16_t *peak, uint16_t *white) {
    static int fake = -1;
    if (fake < 0) fake = rt_knob("HDR_FAKE", "0");
    int on = __atomic_load_n(&g_on, __ATOMIC_ACQUIRE);
    if (!on && RT_KNOB_ON(fake)) {
        *peak = PEAK_DEFAULT, *white = WHITE_DEFAULT;
        return 1;
    }
    uint32_t p, w;
    effective(&p, &w);
    static uint32_t logged_p, logged_w;
    if (on && (p != logged_p || w != logged_w)) {
        logged_p = p, logged_w = w;
        rt_log("gfx: HDR to the game: peak %u nits, paper white %u (system %u, %u)", p, w, g_peak, g_white);
    }
    *peak = on ? (uint16_t)p : 0;
    *white = on ? (uint16_t)w : WHITE_DEFAULT;
    return on;
}

int gfx_hdr_cfg_changed(void) { return strcmp(cfg_value("MKW_HDR"), g_cfg) != 0; }

int gfx_hdr_pick(const VkSurfaceFormatKHR *fmts, uint32_t n, VkSurfaceFormatKHR *out) {
    snprintf(g_cfg, sizeof g_cfg, "%s", cfg_value("MKW_HDR"));
    float white = 0, headroom = 0;
    int sys = wsys_hdr(&white, &headroom), found = 0;
    if (strcmp(g_cfg, "off") && sys != 0 && vk_ctx()->has_colorspace)
        for (int pass = 0; pass < 2 && !found; pass++)   // A2B10G10R10 first, as the game's RGB10A2 window
            for (uint32_t i = 0; i < n && !found; i++)
                if (fmts[i].colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT &&
                    fmts[i].format == (pass ? VK_FORMAT_A2R10G10B10_UNORM_PACK32 : VK_FORMAT_A2B10G10R10_UNORM_PACK32))
                    *out = fmts[i], found = 1;
    uint32_t w = white > 0 ? (uint32_t)(white * NITS_SCRGB_ONE + 0.5f) : WHITE_DEFAULT;
    uint32_t p = white > 0 && headroom > 1 ? (uint32_t)(white * headroom * NITS_SCRGB_ONE + 0.5f) : PEAK_DEFAULT;
    if (p > 10000) p = 10000;
    if (p < w) p = w;
    g_white = w, g_peak = p;
    static int logged = -1;
    if (logged != found) {
        logged = found;
        if (found) rt_log("gfx: HDR10 output, peak %u nits, paper white %u (MKW_HDR %s)", p, w, g_cfg);
        else if (!strcmp(g_cfg, "off")) rt_log("gfx: SDR output (MKW_HDR off)");
        else if (sys == 0) rt_log("gfx: SDR output: the display is not in HDR");
        else rt_log("gfx: SDR output: the display offers no HDR10 format%s", vk_ctx()->has_colorspace ? "" :
                    " (no VK_EXT_swapchain_colorspace)");
    }
    if (found) {   // for gfx_hdr_headroom at the next start
        char seen[24];
        snprintf(seen, sizeof seen, "%u/%u", p, w);
        if (strcmp(cfg_value("MKW_HDR_SYSTEM"), seen)) cfg_set("MKW_HDR_SYSTEM", seen), cfg_save();
    }
    __atomic_store_n(&g_on, found, __ATOMIC_RELEASE);
    return found;
}

float gfx_hdr_headroom(void) {
    uint32_t p, w;
    if (__atomic_load_n(&g_on, __ATOMIC_ACQUIRE)) {
        effective(&p, &w);
    } else {   // before the swapchain: the system's of the last run, with the player's choices over them
        unsigned sp = 0, sw = 0;
        if (sscanf(cfg_value("MKW_HDR_SYSTEM"), "%u/%u", &sp, &sw) != 2 || !sp || !sw) return 0.0f;
        w = nits_setting("MKW_HDR_WHITE", sw), p = nits_setting("MKW_HDR_PEAK", sp);
        if (p < w) p = w;
    }
    return w ? (float)p / (float)w : 0.0f;
}

void gfx_hdr_metadata(VkSwapchainKHR sc) {
    VkCtx *c = vk_ctx();
    if (!g_on || !c->has_hdr_metadata) return;
    static PFN_vkSetHdrMetadataEXT set;
    if (!set) set = (PFN_vkSetHdrMetadataEXT)vkGetDeviceProcAddr(c->device, "vkSetHdrMetadataEXT");
    if (!set) return;
    // Rec.2020 primaries and D65, as HDR10; the content as bright as the display's peak, the frame average at paper white
    uint32_t peak, white;
    effective(&peak, &white);
    VkHdrMetadataEXT m = {.sType = VK_STRUCTURE_TYPE_HDR_METADATA_EXT,
                          .displayPrimaryRed = {0.708f, 0.292f}, .displayPrimaryGreen = {0.170f, 0.797f},
                          .displayPrimaryBlue = {0.131f, 0.046f}, .whitePoint = {0.3127f, 0.3290f},
                          .maxLuminance = (float)peak, .minLuminance = 0.0f,
                          .maxContentLightLevel = (float)peak, .maxFrameAverageLightLevel = (float)white};
    set(c->device, 1, &sc, &m);
}

// PQ (SMPTE ST 2084) code value 0..1 to nits
static float pq_nits(float e) {
    const float m1 = 0.1593017578125f, m2 = 78.84375f, c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
    float p = powf(e, 1.0f / m2);
    return 10000.0f * powf(fmaxf(p - c1, 0.0f) / (c2 - c3 * p), 1.0f / m1);
}

// 10-bit PQ Rec. 2020 pixels (A2B10G10R10: R in the low bits; a2r: A2R10G10B10) to sRGB bytes with paper white at 1.0:
// shots, captures and thumbnails of an HDR picture. From totk-pc-recomp runtime/vk_hdr.c vk_hdr_to_srgb8 (MIT).
void gfx_hdr_to_srgb8(const uint8_t *in, uint8_t *out, size_t px, int a2r) {
    uint16_t peak, white;
    gfx_hdr_output(&peak, &white);
    // Rec. 2020 to Rec. 709 (the inverse of ITU-R BT.2087's matrix)
    static const float m[9] = {1.6605f, -0.5876f, -0.0728f, -0.1246f, 1.1329f, -0.0083f, -0.0182f, -0.1006f, 1.1187f};
    for (size_t i = 0; i < px; i++) {
        uint32_t v;
        memcpy(&v, in + 4 * i, 4);
        float c[3] = {(float)(v & 1023), (float)(v >> 10 & 1023), (float)(v >> 20 & 1023)};
        if (a2r) { float t = c[0]; c[0] = c[2]; c[2] = t; }
        for (int k = 0; k < 3; k++) c[k] = pq_nits(c[k] / 1023.0f) / (float)white;
        for (int k = 0; k < 3; k++) {
            float l = fminf(fmaxf(m[3 * k] * c[0] + m[3 * k + 1] * c[1] + m[3 * k + 2] * c[2], 0.0f), 1.0f);
            float s = l <= 0.0031308f ? 12.92f * l : 1.055f * powf(l, 1.0f / 2.4f) - 0.055f;
            out[4 * i + k] = (uint8_t)(s * 255.0f + 0.5f);
        }
        out[4 * i + 3] = 255;
    }
}
