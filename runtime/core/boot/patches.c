// Code patch sets (recomp/patches.py, gen.py --patches). The generator lifts both the original and the replacement
// instructions of every patched site, and rt_patch_on[set] picks one when the site runs, so a set is switched at start
// without touching code, and also while the game runs (rt_patch_switch: the frame-limit setting of the menu). The sets,
// their environment switches and defaults, and the patch page come from the game code's tables (gamelib.h, from the
// generated patches.h); without them (C generated without --patches) there are no sets and every site runs the
// original.
//
// The patch page is the first 4 KiB page after main's image, inside the module window memory.c reserved: replacement
// code reaches it with one adrp, as MacNativeGames' data page after main (games/totk/runtime/game_patches.c). Its
// layout belongs to the sets that use it.
// Probe modes never call rt_patches_init: every set stays off.
#include "cfg.h"
#include "platform/platform.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gamelib.h"
#include "gfx.h"
#include "hacks.h"
#include "host.h"
#include "version.h"

#define OUTPUT_SIZE 0xfd0   // patch page offset of the output size (recomp/patches-mkw.txt, set output)
#define SHADOW_HOST 0xfc0   // patch page offset of host_shadow_rb (set shadow)
#define SEED_HOST 0xfb0     // patch page offset of host_seed_tick (set seed)
                            // 0xfa0: game.c host_frame_update (MKW_FPS)
#define SHADOW_POOL_HOST 0xf90   // patch page offset of host_shadow_pool (set shadow)
#define ASPECT_HOST 0xf80        // patch page offset of host_aspect (set aspect)
#define UI_PROJ_HOST 0xf70       // patch page offset of host_ui_proj (set aspect)
#define UI_ROWS_HOST 0xf60       // patch page offset of host_ui_rows (set aspect)
#define CAMERA_EVENT_HOST 0xf50  // patch page offset of host_camera_event (set camera, 1.8.0)
// The guest functions the sets call or recognise, per game version (version.h; 1.0.0: main+0x233358 RenderBuffer
// creation, main+0x8eb6b0 the render-buffer allocator setup that main+0x37b520 allocates from, main+0x67aa40 operator
// new(size, heap, alignment), main+0x8eb9dc one pool of that allocator, main+0x922088 and main+0x15585a8 two
// sead::Random constructors after their GetSystemTick call). A set whose sites are not in the version's patch table
// never runs these.
#define GUEST_RENDERBUFFER_CREATE rt_main_addr(rt_version()->rb_create)
#define GUEST_RB_ALLOCATOR_INIT rt_main_addr(rt_version()->rb_alloc_init)
#define GUEST_RB_POOL_NEW rt_main_addr(rt_version()->rb_pool_new)
#define GUEST_RB_POOL_INIT rt_main_addr(rt_version()->rb_pool_init)
#define GUEST_RANDOM_CTOR_LR rt_main_addr(rt_version()->random_ctor_lr)
#define GUEST_RANDOM_CTOR2_LR rt_main_addr(rt_version()->random_ctor2_lr)

// The shadow set: MKW_SHADOW_SIZE (Graphics tab) in place of the cascaded shadow map size the game picks (3072 docked,
// 2048 handheld). Only a square buffer of 512 or more is resized: what the site's caller (main+0x22d45c) creates.
static uint32_t shadow_size;
static uint64_t rb_obj, rb_heaps[8];   // the render-buffer allocator and its heap chain (host_shadow_pool)
static int rb_pool_done, rb_pool_ok;
static int shadow_second_pool(Cpu *c);
static void host_shadow_rb(Cpu *c) {
    uint32_t *w = (uint32_t *)(uintptr_t)(c->x[0] + 0x68), *h = (uint32_t *)(uintptr_t)(c->x[0] + 0x88);
    if (shadow_size && *w == *h && *w >= 512) {
        if (shadow_size > 3072 && !rb_pool_done) rb_pool_ok = shadow_second_pool(c);
        // Without the second pool a map over 3072 does not fit and main+0x37b520 reads a null block (issue #48, 4096
        // at 1440 lines: "no free second slot", then that crash after the character choice): the game's size then.
        if (shadow_size <= 3072 || rb_pool_ok) {
            static int logs;   // the game creates it again on every course change (#51: 43 000 lines in one log)
            if (*w != shadow_size && logs++ < 4)
                rt_log("patches: shadow map %ux%u instead of %ux%u (MKW_SHADOW_SIZE)", shadow_size, shadow_size, *w, *h);
            *w = *h = shadow_size;
        }
    }
    rt_call(c, GUEST_RENDERBUFFER_CREATE, 4, c->x[0], c->x[1], c->x[2], c->x[3]);
}

// The render-buffer allocator's pool is sized for the console's 3072 map (3 layers of D16), and the shadow map shares
// it with the scene's targets: with a larger map and full-resolution targets it runs out when the map changes (issue
// #9; build/romfs-res, at 720 lines, hid it on Linux). It cannot just get more memory at its init: the pool past
// ~256 MiB, or a second pool made then, takes bytes the game's later fixed-size heaps count on and it fails at boot
// (l9-d, l9-f, l9-k). And the allocator only adds a pool when full if its config names a heap (+0x30; MKW's is 0) and
// its pool table has a free slot (it has one), from a heap that by then is full (l9-j). So the table gets two slots
// (recomp/patches-mkw.txt) and the init's heap chain is kept here; when the first larger map is made, after boot, the
// second pool comes from the heap of that chain with the most room (l9-l: one keeps ~209 MiB from the title on).
static void host_shadow_pool(Cpu *c) {
    uint64_t cfg = c->x[1];
    rb_obj = c->x[0];
    uint64_t h = *(uint64_t *)(uintptr_t)(cfg + 0x28);
    for (int i = 0; i < 8 && h; i++) rb_heaps[i] = h, h = ((uint64_t *)(uintptr_t)h)[7];  // parent: +0x38
    rt_call(c, GUEST_RB_ALLOCATOR_INIT, 2, rb_obj, cfg);
}

// The second pool as the allocator makes one (main+0x37cdbc..0x37ce2c; its init's own, main+0x8eb85c..0x8eb890).
static int shadow_second_pool(Cpu *c) {
    rb_pool_done = 1;
    uint64_t a[4] = {c->x[0], c->x[1], c->x[2], c->x[3]};   // host_shadow_rb passes them on
    uint64_t size = (3 * 2 * ((uint64_t)shadow_size * shadow_size - 3072ull * 3072) + 0xffff) & ~0xffffull;
    uint64_t **table = (uint64_t **)(uintptr_t)(rb_obj + 0x2a90), heap = 0, room = 0;
    if (!rb_obj || *(int32_t *)(uintptr_t)(rb_obj + 0x2a88) < 2 || (*table)[1]) {
        rt_log("patches: render-buffer allocator has no free second slot; no pool for the %u shadow map: the game's "
               "size instead", shadow_size);
        return 0;
    }
    for (int i = 0; i < 8 && rb_heaps[i]; i++) {
        uint64_t h = rb_heaps[i], *vt = (uint64_t *)(uintptr_t)*(uint64_t *)(uintptr_t)h;
        uint64_t free_ = rt_call(c, vt[16], 1, h);   // sead::Heap::getFreeSize
        if (free_ > room) room = free_, heap = h;
    }
    uint64_t pool = room > size + 0x100000 ? rt_call(c, GUEST_RB_POOL_NEW, 3, 0x58, heap, 8) : 0;
    if (pool) {
        uint32_t count = *(uint32_t *)(uintptr_t)(rb_obj + 0x11c0);
        uint32_t flags = (*(uint8_t *)(uintptr_t)(rb_obj + 0x2a68) & 0x80) ? 0x104 : 4;
        rt_call(c, GUEST_RB_POOL_INIT, 8, pool, 0, 1, 7, count, size, flags, heap);
        (*table)[1] = pool;
    }
    rt_log("patches: render-buffer allocator: %s second pool of %#llx bytes for the %u shadow map (heap %#llx, "
           "%#llx free)", pool ? "a" : "no room for a", (unsigned long long)size, shadow_size,
           (unsigned long long)heap, (unsigned long long)room);
    for (int i = 0; i < 4; i++) c->x[i] = a[i];
    return pool != 0;
}

// The seed set: MKW_SEED (Hacks tab) in place of the clock the game seeds its random generator with (core/hacks/seed.c).
// Random::Random (main+0x922070 and 0x1558598) builds dozens of generators from several threads through one call site
// (hk-s4a/c: the same sites, another course): those two are told apart by their creator, whose return address their
// prologue saved at [sp+8] and [sp].
static void host_seed_tick(Cpu *c) {
    uint64_t lr = c->x[30];
    if (lr == GUEST_RANDOM_CTOR_LR) lr ^= *(const uint64_t *)(uintptr_t)(c->sp + 8) << 1;
    else if (lr == GUEST_RANDOM_CTOR2_LR) lr ^= *(const uint64_t *)(uintptr_t)c->sp << 1;
    c->x[0] = seed_tick(lr);
}

static void seed_set(const RtGame *g) {
    for (int i = 0; i < g->npatch_sets; i++)
        if (!strcmp(g->patch_sets[i].name, "seed")) g->patch_on[i] = *cfg_value("MKW_SEED") != 0;
}

static void shadow_set(const RtGame *g) {
    shadow_size = (uint32_t)atoi(cfg_value("MKW_SHADOW_SIZE"));   // "console": 0
    for (int i = 0; i < g->npatch_sets; i++)
        if (!strcmp(g->patch_sets[i].name, "shadow")) g->patch_on[i] = shadow_size != 0;
}

// The aspect set: MKW_ASPECT (Screen tab) wider than 16:9 shows more at the sides (Hor+): 21:9, 32:9 or the window's
// own. Each camera's aspect is widened by the output's over 16:9 (host_aspect), and the output gets that width
// (output_size); the 2D layouts keep their shape in the middle 16:9 (host_ui_proj). The race's HUD follows (uw-32x9b).
static float aspect_out;     // the output's width / height; 0: 16:9, the set off
static float aspect_scale;   // aspect_out / (16 / 9)

static float aspect_of(void) {
    const char *a = cfg_value("MKW_ASPECT");
    int w = 16, h = 9;
    if (!strcmp(a, "window")) gfx_window_shape(&w, &h);
    else sscanf(a, "%d:%d", &w, &h);
    float r = w > 0 && h > 0 ? (float)w / (float)h : 0;
    return r > 16.0f / 9 + 0.01f ? r : 0;   // narrower than 16:9 is not offered: the console's
}

// A 16:9 view, or an 8:9 one doubled (half of a two-player split), within the mod's 0.008, is widened; other views
// (the menus' own) keep their aspect. The race's full view is 1280x720 at any output (uw-32x9b), two players' halves
// 640x720 (uw14-split); a 16:9 view narrower than the full one is a quarter of a three- or four-player split.
static volatile uint64_t split_at;   // 1 + the tick of the latest view of a split screen

static float aspect_widen(float w, float h) {
    float a = w / h, r = a < 1.2f ? 2 * a : a, d = r - 16.0f / 9;
    int on = d < 0.008f && d > -0.008f;
    if (on && (a < 1.2f || w < 1000)) split_at = frame_step_ticks() + 1;
    return on ? aspect_scale : 1;
}

// In place of fdiv s0, s0, s1 (recomp/patches-mkw.txt): the camera's aspect, widened.
static void host_aspect(Cpu *c) {
    float w, h;
    memcpy(&w, &c->v[0], sizeof w), memcpy(&h, &c->v[1], sizeof h);
    float a = w / h * aspect_widen(w, h);
    c->v[0] = 0;
    memcpy(&c->v[0], &a, sizeof a);
}

// In place of the bl to nn::ui2d::DrawInfo's projection getter (main+0x1682c9c: the 4x4 at [x0+0x10] to [x1]) from the
// two draw constant-buffer setups of the 2D layouts: the same copy, and a 16:9 projection gets its x row narrowed by
// 16:9 over the output's aspect, so the 2D keeps its shape in the middle 16:9 of a wider picture: the idea of oot-mod's
// 2D canvas hook (MIT; also NX2navi's wide_hud). MKW's layouts are drawn in perspective (uw6-menu: rows 1.358 0 0 0 /
// 0 2.414 0 0 / 0 0 -1.0002 1301.94 / 0 0 -1 1303.68, 45 degrees at 16:9), not through the orthographic cameras of
// main+0x460c10 (uw5-menu: none of those calls). Row-major, x row first: x and y unskewed, its aspect m[5] / m[0].
static int is_16x9(const float *m) {
    if (m[1] || m[2] || m[4] || m[6] || m[8] || m[9] || m[12] || m[13]) return 0;
    float a = m[0] ? m[5] / m[0] : 0, d = (a < 0 ? -a : a) - 16.0f / 9;
    return d < 0.008f && d > -0.008f;
}

// The pane whose material host_ui_proj sets up: the panes' draws (main+0x16ac070...) pass its global matrix (pane +
// 0x70) and its size (pane + 0x50) as the setup's x5 and x6, which keeps them in x23 and x22; the text setup
// (main+0x169ada0, returning to main+0x169aeb4) holds the pane itself in x21 (uw15-split: the HUD's digits stayed
// where its frames moved). Whether its scaled size
// covers the canvas, 1920x1080 layout units (the projection: 45 degrees at a distance of 1303.68 is 1080 high); *tx:
// its x on the canvas, 0 from its middle. Returns -1 when x22 and x23 are not such a pane.
static int ui_full_screen(const Cpu *c, float *tx) {
    int text = rt_version()->ui_text_lr && c->x[30] == rt_main_addr(rt_version()->ui_text_lr);
    uint64_t mtx = text ? c->x[21] + 0x70 : c->x[23], size = text ? c->x[21] + 0x50 : c->x[22], pane = mtx - 0x70;
    *tx = 0;
    if (size != mtx - 0x20 || (mtx & 3) || mtx < 0x70 || !plat_mem_readable(pane, 0xa0)) return -1;
    const float *g = (const float *)(uintptr_t)mtx, *sz = (const float *)(uintptr_t)size;
    float w = fabsf(g[0] * sz[0]), h = fabsf(g[5] * sz[1]);
    *tx = g[3];
    return w > 1920 * 0.98f && h > 1080 * 0.98f;
}

static void host_ui_proj(Cpu *c) {
    float *s = (float *)(uintptr_t)(c->x[0] + 0x10);
    float *m = (float *)(uintptr_t)c->x[1];
    memcpy(m, s, 16 * sizeof *m);
    int on = is_16x9(m);
    // Narrowed in DrawInfo too, so its other readers see it: the labels drawn through a texture (the main menu's, the
    // cup select's) had their place stretched with their shape right (uw8-menu). Narrowed, it no longer passes
    // is_16x9: a second call leaves it.
    if (on) m[0] /= aspect_scale, m[3] /= aspect_scale, memcpy(s, m, 16 * sizeof *m);
    // A pane over the whole canvas (fades, blurred backdrops, the cup map: uw11-menu showed the race loading at the
    // sides of a fade) is drawn wide again, to the edges of the picture; a movie's stays 16:9 (bars at the present)
    float tx;
    int full = aspect_out ? ui_full_screen(c, &tx) : -1;
    if (full == 1 && !ov_movie_playing()) m[0] *= aspect_scale, m[3] *= aspect_scale;
    // A split screen (uw14-split): each player's HUD is on its half of the canvas (its quarter with three or four,
    // left or right all the same), now in the middle of the picture; moved to the middle of its half of the picture,
    // with its shape. In NDC, x' = x + d with d = +-0.5 *
    // (1 - 1 / aspect_scale), added as d times the w row
    uint64_t at = split_at;
    int moved = full == 0 && at && frame_step_ticks() + 1 - at < 30 && (tx > 1 || tx < -1);
    if (moved) {
        float d = (tx > 0 ? 0.5f : -0.5f) * (1 - 1 / aspect_scale);
        for (int i = 0; i < 4; i++) m[i] += d * m[12 + i];
    }
    if (seqwatch_probe())   // MKW_PROBE: every 2D projection setup in its window (the split HUD's digits, uw18-uw20)
        rt_log("patches: probe ui2d frame %llu di %#llx x %g -> %g lr main+%#llx full %d tx %g moved %d",
               (unsigned long long)frame_step_ticks(), (unsigned long long)c->x[0], s[0], m[0],
               (unsigned long long)(c->x[30] - 0x7000000000ull), full, full == -1 ? 0 : tx, moved);
}

// In place of ldp q2, q3, [x20, #0x10] at main+0x168a624: a pane drawn through a view-projection product made on the
// CPU (the cup select's labels and side panel stayed stretched in uw7-menu) reads DrawInfo's projection itself, not
// through the getter. The same two rows, the x row narrowed as host_ui_proj does.
static void host_ui_rows(Cpu *c) {
    float m[16];
    memcpy(m, (const void *)(uintptr_t)(c->x[20] + 0x10), sizeof m);
    int on = is_16x9(m);
    if (on) m[0] /= aspect_scale, m[3] /= aspect_scale;
    memcpy(&c->v[2], m, 16), memcpy(&c->v[3], m + 4, 16);
}

static int has_set(const RtGame *g, const char *name) {
    for (int i = 0; i < g->npatch_sets; i++)
        if (!strcmp(g->patch_sets[i].name, name)) return 1;
    return 0;
}

static void aspect_set(const RtGame *g) {
    // Without the set (a game version whose sites are not found yet) the picture stays 16:9: a wider output alone
    // would stretch the game's 16:9 cameras.
    aspect_out = has_set(g, "aspect") ? aspect_of() : 0;
    if (!aspect_out && strcmp(cfg_value("MKW_ASPECT"), "16:9") && !has_set(g, "aspect"))
        rt_log("patches: MKW_ASPECT %s has no effect on game version %s (no aspect set)", cfg_value("MKW_ASPECT"),
               rt_version()->display);
    aspect_scale = aspect_out / (16.0f / 9);
    gfx_aspect_set(aspect_out);
    for (int i = 0; i < g->npatch_sets; i++)
        if (!strcmp(g->patch_sets[i].name, "aspect")) g->patch_on[i] = aspect_out != 0;
    if (aspect_out) rt_log("patches: camera aspect %.3f, x%.3f over 16:9 (MKW_ASPECT %s)", aspect_out, aspect_scale,
                           cfg_value("MKW_ASPECT"));
}

// The output set: with MKW_RESOLUTION not original, that resolution's lines at 16:9 (720: 1280x720) in place of the
// 2560x1440 the game fixes in code; with original, the lines the picture gets in the window when fewer than 1440
// (Adrià 08/11/2026: the present shrank 2560x1440 to a 1080-line window with its 2x2 bilinear blit, jagged edges,
// nit-off; a smaller output goes through FSR instead). MKW_OUTPUT_PATCH=0 keeps 2560x1440 at any resolution. With
// MKW_ASPECT wider than 16:9, the width is the lines times that aspect (1440 lines when nothing else sets them).
static void output_size(const RtGame *g) {
    const char *res = cfg_value("MKW_RESOLUTION"), *off = cfg_get("MKW_OUTPUT_PATCH");
    int lines = strcmp(res, "original") ? atoi(res) : 0;
    if (!lines) {
        // the lines the picture gets in the window: fewer than the window's when it is narrower than the picture and
        // the present letterboxes it (gfx_fit_rect, MKW_PICTURE_FIT=keep). A 16:9 picture on the Steam Deck's
        // 1280x800 gets 720, and an output of 800 lines (1422x800, 1.23 times the pixels) was shrunk to them.
        int w = 0, h = 0;
        if (gfx_window_shape(&w, &h)) {
            float a = aspect_out ? aspect_out : 16.0f / 9;
            if (w > 0 && strcmp(cfg_value("MKW_PICTURE_FIT"), "stretch") && w < h * a) h = (int)(w / a + 0.5f);
            if (h >= 360 && h < 1440) lines = h;
        }
    }
    if (!lines && aspect_out) lines = 1440;
    for (int i = 0; i < g->npatch_sets; i++) {
        if (strcmp(g->patch_sets[i].name, "output")) continue;
        if (lines <= 0 || (off && !strcmp(off, "0"))) {
            g->patch_on[i] = 0;   // the page holds no size: the site would read 0x0
            return;
        }
        uint32_t wide = aspect_out ? ((uint32_t)(lines * aspect_out + 0.5f) + 1) & ~1u : 0;
        uint32_t wh[2] = {wide ? wide : (uint32_t)(lines * 16 / 9), (uint32_t)lines};
        memcpy((void *)(uintptr_t)(g->patch_page + OUTPUT_SIZE), wh, sizeof wh);
        g->patch_on[i] = 1;
        rt_log("patches: game output %ux%u instead of 2560x1440 (MKW_RESOLUTION %s, MKW_ASPECT %s, window lines %d)",
               wh[0], wh[1], res, cfg_value("MKW_ASPECT"), gfx_window_lines());
        return;
    }
}

void rt_patches_init(void) {
    const RtGame *g = rt_game();
    if (!g->npatch_sets) return;
    if (!plat_mem_commit(g->patch_page, 0x1000))
        rt_fatal("patches: cannot commit the patch page at %#llx", (unsigned long long)g->patch_page);
    char list[512];
    size_t n = 0;
    list[0] = 0;
    for (int i = 0; i < g->npatch_sets; i++) {
        const RtPatchSet *s = &g->patch_sets[i];
        const char *e = cfg_get(s->env);
        // a set with a row in the settings table takes its default from there; the generated default serves the rest
        g->patch_on[i] = (uint8_t)(cfg_meta(s->env) ? cfg_bool(s->env) : e && *e ? strcmp(e, "0") != 0 : s->on);
        n += (size_t)snprintf(list + n, sizeof list - n, "%s%s %s", i ? ", " : "", s->name, g->patch_on[i] ? "on" : "off");
        if (n >= sizeof list) n = sizeof list - 1;
    }
    rt_log("patches: %s (page %#llx)", list, (unsigned long long)g->patch_page);
    aspect_set(g);
    output_size(g);
    shadow_set(g);
    seed_set(g);
    frame_rate_init(g->patch_page);
}

// The camera set (recomp/patches-mkw-180.txt): the console with no camera attached. main+0xcdebc0, which opens the
// camera service, does not run; in place of main+0xcdc560, which attaches that service's event to the
// SystemEventType at x0, the event is a real one that never fires, and the global the service object lives in
// (version.c camera_service) gets a stand-in whose every request fails as a camera that is not there: result
// 0x7c4 (module 452, the camera's, description 3) for slot 0xd8, which main+0x25a680 asks at boot and passes over, and
// 0x5c4 (description 2), which the session functions main+0x1c6b630..0x1c6c270 pass over, for the rest.
#define CAMERA_SLOTS 64
static uint64_t camera_absent_fn, camera_unavailable_fn;
static void camera_absent(Cpu *c) { ret_x(c, 0x7c4); }
static void camera_unavailable(Cpu *c) { ret_x(c, 0x5c4); }
static void host_camera_event(Cpu *c) {
    static uint64_t create;
    if (!create && !(create = ldr_symbol("_ZN2nn2os17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb")))
        rt_fatal("patches: nn::os::CreateSystemEvent not found");
    uint64_t ev = c->x[0], at = rt_main_addr(rt_version()->camera_service);
    if (at && !*(uint64_t *)(uintptr_t)at) {
        uint64_t *obj = mem_kernel_alloc(16 + 8 * CAMERA_SLOTS, 16), *vt = obj + 2;
        for (int i = 0; i < CAMERA_SLOTS; i++) vt[i] = i == 0xd8 / 8 ? camera_absent_fn : camera_unavailable_fn;
        obj[0] = (uint64_t)(uintptr_t)vt;
        *(uint64_t *)(uintptr_t)at = (uint64_t)(uintptr_t)obj;
        rt_log("patches: no camera: the camera service is a stand-in whose requests fail (MKW_CAMERA_SKIP)");
    }
    rt_call(c, create, 3, ev, 0, 0);
}

void zstd_patch_hosts(uint64_t page);   // ov_zstd.c

void rt_patch_hosts(void) {
    const RtGame *g = rt_game();
    if (g->npatch_sets) zstd_patch_hosts(g->patch_page);
    if (g->npatch_sets) rt_register_host(g->patch_page + SHADOW_HOST, host_shadow_rb);
    if (g->npatch_sets) rt_register_host(g->patch_page + SHADOW_POOL_HOST, host_shadow_pool);
    if (g->npatch_sets) rt_register_host(g->patch_page + SEED_HOST, host_seed_tick);
    if (g->npatch_sets) rt_register_host(g->patch_page + ASPECT_HOST, host_aspect);
    if (g->npatch_sets) rt_register_host(g->patch_page + UI_PROJ_HOST, host_ui_proj);
    if (g->npatch_sets) rt_register_host(g->patch_page + UI_ROWS_HOST, host_ui_rows);
    if (g->npatch_sets) frame_rate_host(g->patch_page);
    if (g->npatch_sets) rt_register_host(g->patch_page + CAMERA_EVENT_HOST, host_camera_event);
    if (rt_version()->camera_service) {
        camera_absent_fn = hostfn_register("camera: absent", camera_absent);
        camera_unavailable_fn = hostfn_register("camera: unavailable", camera_unavailable);
    }
}

void rt_patch_switch(int set, int on) {
    const RtGame *g = rt_game();
    if (set >= 0 && set < g->npatch_sets) __atomic_store_n(&g->patch_on[set], (uint8_t)(on != 0), __ATOMIC_RELAXED);
}
