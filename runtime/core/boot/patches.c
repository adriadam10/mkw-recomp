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
#include "hud_layouts.h"
#include "host.h"
#include "version.h"

#define OUTPUT_SIZE 0xfd0   // patch page offset of the output size (recomp/patches-mkw.txt, set output)
#define OUTPUT_SCENE_SCALE 0.65   // the docked scene renders at this share of For3D (sdk/fs/romfs_res.c SCENE_SCALE)
#define SHADOW_HOST 0xfc0   // patch page offset of host_shadow_rb (set shadow)
#define SEED_HOST 0xfb0     // patch page offset of host_seed_tick (set seed)
                            // 0xfa0: game.c host_frame_update (MKW_FPS)
#define SHADOW_POOL_HOST 0xf90   // patch page offset of host_shadow_pool (set shadow)
#define CAMERA_FOV_HOST 0xf30    // race camera FOV before its caches and projection/frustum updates
#define ASPECT_HOST 0xf80        // patch page offset of host_aspect (set aspect)
#define UI_PROJ_HOST 0xf70       // patch page offset of host_ui_proj (set aspect)
#define UI_ROWS_HOST 0xf60       // patch page offset of host_ui_rows (set aspect)
#define CAMERA_EVENT_HOST 0xf50  // patch page offset of host_camera_event (set camera, 1.8.0)
#define UI_ROWS2_HOST 0xf40      // patch page offset of host_ui_rows2 (set aspect, 1.8.0)
#define FX_VIEW_HOST 0xf20       // patch page offset of host_fx_view (set aspect)
#define DISPLAY_SIZE_HOST 0xf10  // patch page offset of host_display_size (set display, 1.8.0)
#define DISPLAY_SET_HOST 0xf00   // patch page offset of host_display_set (set display, 1.8.0)
#define LOGIC_HZ_HOST 0xde0      // patch page offset of host_logic_step (set logichz)
#define LOGIC_IV_HOST 0xdd0      // patch page offset of host_logic_copy (set logichz)
#define PANE_MATRIX_HOST 0xdf0   // patch page offset of host_pane_matrix (set aspect, 1.8.0), below race_scene.c's
                                 // 0xe00-0xef0: race_scene.c, one per node (RT_RACE_HOST)
// The guest functions the sets call or recognise, per game version (version.h; 1.0.0: main+0x233358 RenderBuffer
// creation, main+0x8eb6b0 the render-buffer allocator setup that main+0x37b520 allocates from, main+0x67aa40 operator
// new(size, heap, alignment), main+0x8eb9dc one pool of that allocator, main+0x922088 and main+0x15585a8 two
// sead::Random constructors after their GetSystemTick call). A set whose sites are not in the version's patch table
// never runs these.
#define GUEST_RENDERBUFFER_CREATE rt_main_addr(rt_version()->rb_create)
#define GUEST_RB_ALLOCATOR_INIT rt_main_addr(rt_version()->rb_alloc_init)
#define GUEST_RB_POOL_NEW rt_main_addr(rt_version()->rb_pool_new)
#define GUEST_RB_POOL_INIT rt_main_addr(rt_version()->rb_pool_init)
#define GUEST_EXPHEAP_CREATE rt_main_addr(rt_version()->expheap_create)
#define GUEST_RANDOM_CTOR_LR rt_main_addr(rt_version()->random_ctor_lr)
#define GUEST_RANDOM_CTOR2_LR rt_main_addr(rt_version()->random_ctor2_lr)

// The shadow set: MKW_SHADOW_SIZE (Graphics tab) in place of the cascaded shadow map size the game picks (3072 docked,
// 2048 handheld). Only a square buffer of 512 or more is resized: what the site's caller (main+0x22d45c) creates.
static uint32_t shadow_size;
// The second pool of the render-buffer allocator (shadow_second_pool): the bytes a map over 3072 grows, the bytes the
// scene's targets grow over 1440 lines (1.8.0's 2160 layer makes them 3840x2160 and the game's pool runs out with the
// console's map too: #255, s255c), and MKW_RB_EXTRA_MIB more.
static uint32_t rb_extra_mib, scene_lines;
static uint64_t rb_obj;   // the render-buffer allocator (host_shadow_pool)
static int rb_pool_done, rb_pool_ok;
static int shadow_second_pool(Cpu *c);

static uint64_t map_bytes(uint32_t size) { return 3 * 2 * (uint64_t)size * size; }   // 3 cascades of D16

// 1.8.0's photo mode (with its stickers) makes targets of its own: at 1440 lines the game's pool has no room for them
// and main+0xa66e0 reads a null block as soon as Minus is pressed in free roam (#305; foto5 crashed, foto4 at 1080
// lines did not, 1.0.0 at 1440 did not: foto1). 128 MiB at 1440 lines, growing with the pixels, is a margin, not a
// measure (the runtime's region only takes memory as the pool uses it); foto6 checks it at 1440. Photo mode also makes
// a whole second shadow map while the race's stays (#440: "shadow map 4096x4096" and a 96 MiB image on entering it,
// then 3840x2160 targets, and a 0x1fe0000 block found no room in a 170 MiB pool): that map is counted whole on top.
#define PHOTO_MIB_1440 128u

// Hypothesis, measured in s255g: the game's 250 MiB pool holds the console's 1440-line targets with its 3072 map, so
// the targets take the rest and grow with the pixels.
static uint64_t second_pool_bytes(void) {
    uint64_t b = (uint64_t)rb_extra_mib << 20;
    if (shadow_size > 3072) b += map_bytes(shadow_size) - map_bytes(3072);
    uint64_t lines = scene_lines ? scene_lines : 1440;   // original: the window's lines, at most 1440
    if (rt_version()->number >= 180)
        b += ((uint64_t)PHOTO_MIB_1440 << 20) * lines * lines / (1440ull * 1440) + map_bytes(shadow_size ? shadow_size : 3072);
    if (scene_lines > 1440)
        b += ((250ull << 20) - map_bytes(3072)) * ((uint64_t)scene_lines * scene_lines - 1440ull * 1440) / (1440ull * 1440);
    return (b + 0xffff) & ~0xffffull;
}

static void host_shadow_rb(Cpu *c) {
    uint32_t *w = (uint32_t *)(uintptr_t)(c->x[0] + 0x68), *h = (uint32_t *)(uintptr_t)(c->x[0] + 0x88);
    int map = *w == *h && *w >= 512;
    if (map && !rb_pool_done && second_pool_bytes()) rb_pool_ok = shadow_second_pool(c);
    if (shadow_size && map) {
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

// The render-buffer allocator's pool is sized for the console's 3072 map (3 layers of D16) and 1440-line targets, and
// the shadow map shares it with the scene's targets: a larger map runs out when the map changes (issue #9; build/
// romfs-res, at 720 lines, hid it on Linux) and 1.8.0's 2160 layer runs out at the title (#255, #210). It cannot just
// get more memory at its init: the pool past ~256 MiB, or a second pool made then, takes bytes the game's later
// fixed-size heaps count on and it fails at boot (l9-d, l9-f, l9-k); the allocator adds a pool when full only with a
// heap in its config (+0x30; MKW's is 0) and a free slot in its pool table (it has one), from a heap that by then is
// full (l9-j); and the game's heaps keep ~146 MiB after boot, less than 2160 needs (s255d). So the table gets two
// slots (recomp/patches-mkw.txt) and, when the first map is made after boot, the second pool comes from memory of the
// runtime's own (mem_runtime_alloc) under a pead::ExpHeap of ours, taking nothing from the game.
static void host_shadow_pool(Cpu *c) {
    rb_obj = c->x[0];
    rt_call(c, GUEST_RB_ALLOCATOR_INIT, 2, rb_obj, c->x[1]);
}

// The second pool as the allocator makes one (main+0x37cdbc..0x37ce2c; its init's own, main+0x8eb85c..0x8eb890),
// from a heap of the game's own class over the runtime's region: pead::ExpHeap::create(start, size, name, lock, 0)
// places the heap at start, as the game's root heap is made (main+0xcf3050); name points at the characters' pointer.
static int shadow_second_pool(Cpu *c) {
    rb_pool_done = 1;
    uint64_t a[4] = {c->x[0], c->x[1], c->x[2], c->x[3]};   // host_shadow_rb passes them on
    uint64_t size = second_pool_bytes(), **table = (uint64_t **)(uintptr_t)(rb_obj + 0x2a90), heap = 0, pool = 0;
    if (!rb_obj || *(int32_t *)(uintptr_t)(rb_obj + 0x2a88) < 2 || (*table)[1]) {
        rt_log("patches: render-buffer allocator has no free second slot; no second pool (shadow map %u, %u lines)",
               shadow_size, scene_lines);
        return 0;
    }
    if (!GUEST_EXPHEAP_CREATE) {
        rt_log("patches: no ExpHeap::create for this version; no second pool (shadow map %u, %u lines)", shadow_size,
               scene_lines);
        return 0;
    }
    // the region holds the heap's header and the pool's alignment too
    uint64_t region_size = size + 0x200000, region = mem_runtime_alloc(region_size, "render-buffer pool");
    if (region) {
        static const char text[] = "mkw-recomp render-buffer pool";
        uint64_t *name = mem_kernel_alloc(8 + sizeof text, 8);
        memcpy(name + 1, text, sizeof text);
        name[0] = (uint64_t)(uintptr_t)(name + 1);
        heap = rt_call(c, GUEST_EXPHEAP_CREATE, 5, region, region_size, (uint64_t)(uintptr_t)name, 1, 0);
    }
    if (heap) pool = rt_call(c, GUEST_RB_POOL_NEW, 3, 0x58, heap, 8);
    if (pool) {
        uint32_t count = *(uint32_t *)(uintptr_t)(rb_obj + 0x11c0);
        uint32_t flags = (*(uint8_t *)(uintptr_t)(rb_obj + 0x2a68) & 0x80) ? 0x104 : 4;
        rt_call(c, GUEST_RB_POOL_INIT, 8, pool, 0, 1, 7, count, size, flags, heap);
        (*table)[1] = pool;
    }
    rt_log("patches: render-buffer allocator: %s second pool of %#llx bytes on the runtime's own heap %#llx (shadow map "
           "%u, scene %u lines, MKW_RB_EXTRA_MIB %u)", pool ? "a" : "no", (unsigned long long)size,
           (unsigned long long)heap, shadow_size, scene_lines, rb_extra_mib);
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

// The logichz set where the frame's delegate copies NVN's present interval to the ModuleSystem (1.0.0 main+0x45e790
// and main+0x261bb80, 1.8.0 main+0x8d388 and main+0x2977b94; str w8, [x19, #0x50c]: x19 the system, w8 the interval
// in vsync ticks at N Hz, 1 or 2). Every writer of the step record copies [sys+0x50c] whole into it (the per-frame
// one and the event virtual's three: lhz120tt, the time trial timer ran twice as fast when only the first was
// corrected), and its consumers take it whole: counters add it, loops run it times, so at N Hz each would run N/60
// fast. An accumulator hands out whole frames at the 60 Hz rate instead (120 Hz: 0, 1, 0, 1), as the game itself
// does when it skips one; it already expects less than 1 (main+0x2720ea0 skips its update then). One of the two
// copies runs per frame (the second is the delegate's body, the first its inlined copy in main+0x45e6c8).
// N changes on entering or leaving time trial (game.c frame_logic_hz): the remainder restarts with it.
static uint32_t logic_acc, logic_acc_hz;   // 60 Hz frames owed times N, < N: the game thread alone runs it
static uint32_t logic_v = 1;               // the last present interval, for host_logic_step
static void host_logic_copy(Cpu *c) {
    uint32_t hz = (uint32_t)frame_logic_hz(), v = (uint32_t)c->x[8];
    if (hz != logic_acc_hz) logic_acc_hz = hz, logic_acc = 0;
    logic_v = v;
    logic_acc += 60u * v;
    *(uint32_t *)(uintptr_t)(c->x[19] + 0x50c) = logic_acc / hz;
    logic_acc %= hz;
}

// The logichz set at each writer of the ModuleSystem's step record (1.0.0 main+0x45e7ec and the event virtual
// main+0x159fe4; x8 the record, [system+0x500]): the step's {seconds, 60 Hz frames} instead of {1/60, 1.0}. They
// follow the present interval (read by some 50 module functions), at most one 60 Hz frame as on the console: a PC
// that only keeps up with 60 steps a second (interval 2 at 120 Hz) gets exactly the console's {1/60 s, 1.0, 1}, and
// one slower than that slows down as the console does.
static void host_logic_step(Cpu *c) {
    float *rec = (float *)(uintptr_t)c->x[8];
    uint32_t hz = (uint32_t)frame_logic_hz(), v = logic_v;
    float frames = v * 60u >= hz ? 1.0f : (float)(v * 60u) / (float)hz;
    rec[0] = frames / 60.0f;
    rec[1] = frames;
}

static void logic_hz_set(const RtGame *g) {
    for (int i = 0; i < g->npatch_sets; i++)
        if (!strcmp(g->patch_sets[i].name, "logichz")) {
            g->patch_on[i] = frame_logic_hz() != 60;
            if (frame_logic_hz() != 60) rt_log("game: logic at %d Hz (MKW_LOGIC_HZ, set logichz on)", frame_logic_hz());
            return;
        }
    if (frame_logic_hz() != 60) rt_log("game: MKW_LOGIC_HZ=%d needs the logichz set, not in this game code: 60 Hz",
                                       frame_logic_hz());
    frame_logic_hz_60();
}

static void seed_set(const RtGame *g) {
    for (int i = 0; i < g->npatch_sets; i++)
        if (!strcmp(g->patch_sets[i].name, "seed")) g->patch_on[i] = *cfg_value("MKW_SEED") != 0;
}

static void shadow_set(const RtGame *g) {
    shadow_size = (uint32_t)cfg_int_in("MKW_SHADOW_SIZE", 0, 4096);   // "console": 0
    const char *extra = cfg_get("MKW_RB_EXTRA_MIB"), *res = cfg_value("MKW_RESOLUTION");
    rb_extra_mib = extra ? (uint32_t)atoi(extra) : 0;
    scene_lines = strcmp(res, "original") ? (uint32_t)cfg_int_in("MKW_RESOLUTION", 0, 4320) : 0;   // the targets' lines (romfs_res.c)
    for (int i = 0; i < g->npatch_sets; i++)
        if (!strcmp(g->patch_sets[i].name, "shadow")) g->patch_on[i] = shadow_size != 0 || second_pool_bytes() != 0;
}

// The aspect set: MKW_ASPECT (Screen tab) wider than 16:9 shows more at the sides (Hor+): 21:9, 32:9, 48:9 or the window's
// own. Window also extends vertically below 16:9 (Vert+, set vertplus: the race FOV, version.c). Each camera's aspect
// follows the output (host_aspect/output_size); the 2D layouts fit without stretching (host_ui_proj).
static float aspect_out;     // the output's width / height; 0: 16:9, the set off
static float aspect_scale;   // aspect_out / (16 / 9)

static float aspect_of(void) {
    const char *a = cfg_value("MKW_ASPECT");
    int w = 16, h = 9;
    if (!strcmp(a, "window")) gfx_window_shape(&w, &h);
    else sscanf(a, "%d:%d", &w, &h);
    float r = w > 0 && h > 0 ? (float)w / (float)h : 0;
    if (!strcmp(a, "window") && r > 0 && r < 16.0f / 9) {
        if (rt_version()->aspect_first_lr) return r;
        rt_log("patches: tall Window has no FOV site on %s; keeping 16:9", rt_version()->display);
    }
    return r > 16.0f / 9 + 0.01f ? r : 0;
}

// A 16:9 view, or an 8:9 one doubled (half of a two-player split), within the mod's 0.008, is widened; other views
// (the menus' own) keep their aspect. The race's full view is 1280x720 at any output (uw-32x9b), two players' halves
// 640x720 (uw14-split); a 16:9 view narrower than the full one is a quarter of a three- or four-player split.
static volatile uint64_t split_at;   // 1 + the tick of the latest view of a split screen

static float aspect_widen(float w, float h) {
    float a = w / h, r = a < 1.2f ? 2 * a : a, d = r - 16.0f / 9;
    int on = d < 0.008f && d > -0.008f;
    if (on && (a < 1.2f || w < 1000)) split_at = frame_step_ticks() + 1;
    // The first distinct views, with the decision: a stretched picture (#122) is a view left out here
    static float seen[16][2];
    static volatile int32_t nseen;
    int n = __atomic_load_n(&nseen, __ATOMIC_ACQUIRE), k = 0;
    while (k < n && (seen[k][0] != w || seen[k][1] != h)) k++;
    if (k == n && n < 16 && __atomic_compare_exchange_n(&nseen, &n, n + 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        seen[n][0] = w, seen[n][1] = h;
        rt_log("patches: camera view %.0fx%.0f (%.3f): %s", w, h, a, on ? "fitted" : "kept");
    }
    return on ? aspect_scale : 1;
}

// The requested angle remains the source for all projection and frustum users. At 1.0.0 main+45fba4 (1.8.0 894a4),
// the race update has already stored aspect (460c10), but is about to replace the FOV and all three caches.
// Change the fresh request before those writes, not the caches that the game immediately overwrites.
static float aspect_fov_angle(float angle, float scale) {
    return 2.0f * atanf(tanf(angle * 0.5f) / scale);
}

static void host_camera_fov(Cpu *c) {
    float angle, half;
    memcpy(&angle, &c->v[0], sizeof angle);
    memcpy(&half, &c->v[rt_version()->fov_half_v], sizeof half);
    float a = *(float *)(uintptr_t)(c->x[rt_version()->fov_entry_x] + 0xb0);
    if (aspect_scale > 0 && aspect_scale < 1 &&
        fabsf(a - (16.0f / 9) * aspect_scale) < 0.008f) {
        angle = aspect_fov_angle(angle, aspect_scale);
        c->v[0] = 0;
        memcpy(&c->v[0], &angle, sizeof angle);
    }
    // Emulate the replaced fmul s8, s0, s13 (s14 on 1.8.0), including scalar upper-lane clearing.
    half *= angle;
    c->v[8] = 0;
    memcpy(&c->v[8], &half, sizeof half);
}

// The output the game made (output_size): 1.8.0's window textures, which the display size must fit (display_fit)
static uint32_t output_wh[2];

// A 16:9 display size fitted to the output's aspect (set display, 1.8.0): on a taller output the same width and the
// rows of its aspect, on a wider one the same rows and the columns of its aspect (both even), as aspect_tall_target
// adds rows instead of removing columns. Other sizes, and a 16:9 output, keep theirs. 1.8.0 crops its picture from
// the window textures, the output's size: a size that does not fit them is shrunk to fit, with its shape (at 32:9 a
// 5120x1440 size in 3840x2160 textures came out with its outer 640 columns black on each side).
static uint64_t display_fit(uint64_t wh) {
    uint32_t w = (uint32_t)wh, h = (uint32_t)(wh >> 32);
    if (!(aspect_scale > 0) || aspect_scale == 1 || !w || !h || fabsf((float)w / (float)h - 16.0f / 9) > 0.01f)
        return wh;
    float fw = (float)w, fh = (float)h;
    if (aspect_scale > 1) fw = fh * aspect_out;
    else fh = fw / aspect_out;
    float fit = 1;
    if (output_wh[0] && output_wh[1]) fit = fminf(1.0f, fminf((float)output_wh[0] / fw, (float)output_wh[1] / fh));
    uint32_t nw = ((uint32_t)(fw * fit + 0.5f) + 1) & ~1u, nh = ((uint32_t)(fh * fit + 0.5f) + 1) & ~1u;
    return nw | (uint64_t)nh << 32;
}

static void display_note(uint64_t wh, uint64_t fitted) {
    static int logged;
    if (fitted != wh && !logged++)
        rt_log("patches: display %ux%u made %ux%u (MKW_ASPECT, output %ux%u)", (unsigned)wh, (unsigned)(wh >> 32),
               (unsigned)fitted, (unsigned)(fitted >> 32), output_wh[0], output_wh[1]);
}

// In place of 1.8.0's store of the display's requested size in its per-mode update (version.c display_store): the
// same store, of the fitted size. The registers keep the original size, so the mode index the game derives from it
// right after is unchanged.
static void host_display_size(Cpu *c) {
    const RtVersion *v = rt_version();
    uint64_t wh = c->x[v->display_value_reg], fitted = display_fit(wh);
    *(uint64_t *)(uintptr_t)(c->x[v->display_base_reg] + 0x10) = fitted;
    display_note(wh, fitted);
}

// In place of the calls to 1.8.0's display size setter (display, const size *, slot): the setter itself, which also
// keeps the mode index of the original size, then its slot rewritten with the fitted size.
static void host_display_set(Cpu *c) {
    uint64_t display = c->x[0], size = c->x[1], slot = (uint64_t)(int64_t)(int32_t)c->x[2];
    rt_call(c, rt_main_addr(rt_version()->display_set_fn), 3, display, size, slot);
    uint64_t *at = (uint64_t *)(uintptr_t)(display + 0x10 + 8 * slot), wh = *at, fitted = display_fit(wh);
    *at = fitted;
    display_note(wh, fitted);
}

// In place of fdiv s0, s0, s1 (recomp/patches-mkw.txt): the camera's aspect, fitted.
static void host_aspect(Cpu *c) {
    float w, h;
    memcpy(&w, &c->v[0], sizeof w), memcpy(&h, &c->v[1], sizeof h);
    float scale = aspect_widen(w, h);
    // Keep the first projection's 16:9 layout calibration. The second projection receives
    // the race camera's fresh corrected request at 45fba4. UI draw projections fit the 16:9 canvas separately.
    if (scale < 1 && c->x[30] == rt_main_addr(rt_version()->aspect_first_lr)) scale = 1;
    float a = w / h * scale;
    c->v[0] = 0;
    memcpy(&c->v[0], &a, sizeof a);
}

// In place of the bl to nn::ui2d::DrawInfo's projection getter (main+0x1682c9c: the 4x4 at [x0+0x10] to [x1]) from the
// two draw constant-buffer setups of the 2D layouts: the same copy, and a 16:9 projection gets its x row narrowed by
// 16:9 over the output's aspect, so the 2D keeps its shape in the middle 16:9 of a wider picture: the idea of oot-mod's
// 2D canvas hook (craftycorvid/oot-mod c5ccd1c, MIT; docs/project/research.md). MKW's layouts are drawn in perspective (uw6-menu: rows 1.358 0 0 0 /
// 0 2.414 0 0 / 0 0 -1.0002 1301.94 / 0 0 -1 1303.68, 45 degrees at 16:9), not through the orthographic cameras of
// main+0x460c10 (uw5-menu: none of those calls). Row-major, x row first: x and y unskewed, its aspect m[5] / m[0].
static int is_16x9(const float *m) {
    if (m[1] || m[2] || m[4] || m[6] || m[8] || m[9] || m[12] || m[13]) return 0;
    float a = m[0] ? m[5] / m[0] : 0, d = (a < 0 ? -a : a) - 16.0f / 9;
    return d < 0.008f && d > -0.008f;
}

// Whether the UI hooks may read what they are handed. ui_full_screen, ui_tall_backdrop and host_ui_proj test x22 and
// x23 on every 2D draw setup of an output that is not 16:9, a pane's or not, before reading the pane's first 0xa0 or
// 0xe0 bytes. The platform's test (plat_mem_readable) is a VirtualQuery on Windows, which walks the page tables from
// that page to the end of its committed run (a bench of ours: 5 us for 2 MiB, 195 us for 64 MiB, 7.2 ms for 1 GiB)
// inside the reservation the replay thread's GetWriteWatch reads, and the two contend. A pane is a nn::ui2d object in
// the game's memory, which memory.c's books answer with no system call (mem_guest_readable); the platform is asked
// only where they cannot vouch for the range. Windows, 1.8.0 at 3440x1440, a session of two races with MKW_AB=10
// (25/11/2026): about 3 800 checks a second, whose VirtualQuery calls took 0.49 s of the game's threads a second;
// with the books, GetWriteWatch took 11 us a query instead of 81 (the same ~175 queries a race frame) and the replay
// 9.0 ms a race frame instead of 21.1. In check mode (the title, then a Grand Prix) the books answered all 2.9 million
// checks, the same as the platform each time.
// MKW_UI_PANE_MAP (default 1; "ab": the books in the MKW_AB B blocks only): 0 asks the platform every time, as before;
// check asks both and keeps the platform's answer, counting where they differ. With MKW_TRACE=perf, or in check mode,
// a line every 10 s counts the checks, those the books answered and the platform's calls with their time.
enum { PANE_CHECKS, PANE_BOOKS, PANE_ASKED, PANE_ASK_NS, PANE_ASK_MAX_NS, PANE_BOOKS_ONLY, PANE_PLAT_ONLY, PANE_STATS };
static volatile int64_t pane_stat[2][PANE_STATS];   // [rt_ab()]
static volatile int64_t pane_report_ns;              // plat_now_ns of the last line, 0 before the first check
static volatile int pane_mode_v = -1;                // 0, 1, 2 (ab), 3 (check)

static int pane_mode(void) {
    int m = __atomic_load_n(&pane_mode_v, __ATOMIC_ACQUIRE);
    if (m >= 0) return m;
    const char *e = cfg_get("MKW_UI_PANE_MAP");
    m = e && !strcmp(e, "check") ? 3 : rt_knob("UI_PANE_MAP", "1");
    int unset = -1;
    if (!__atomic_compare_exchange_n(&pane_mode_v, &unset, m, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return unset;
    if (e && *e && strcmp(e, "0") && strcmp(e, "1") && strcmp(e, "ab") && strcmp(e, "check"))
        rt_log("patches: MKW_UI_PANE_MAP=%s is not 0, 1, ab or check: 1", e);
    if (m != 1)
        rt_log("patches: UI pane checks by %s (MKW_UI_PANE_MAP)", m == 0 ? "the platform"
                                                                  : m == 2 ? "the game's books in the B blocks"
                                                                           : "both, the platform's answer kept");
    return m;
}

static void pane_report(void) {
    int64_t now = (int64_t)plat_now_ns(), last = __atomic_load_n(&pane_report_ns, __ATOMIC_RELAXED);
    if (last && now - last < 10000000000ll) return;
    if (!__atomic_compare_exchange_n(&pane_report_ns, &last, now, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED) || !last)
        return;
    const char *ab = cfg_get("MKW_AB");
    int ab_on = ab && atof(ab) > 0;   // the lines name the blocks
    for (int b = 0; b < 2; b++) {
        int64_t v[PANE_STATS];
        for (int k = 0; k < PANE_STATS; k++) v[k] = plat_xchg64(&pane_stat[b][k], 0);
        if (!v[PANE_CHECKS]) continue;
        char differ[96] = "";
        if (pane_mode() == 3)
            snprintf(differ, sizeof differ, "; readable to the books only %lld, to the platform only %lld",
                     (long long)v[PANE_BOOKS_ONLY], (long long)v[PANE_PLAT_ONLY]);
        rt_log("patches: UI pane checks%s in %.1f s: %lld, %lld by the game's books, %lld asked the platform (%.2f ms, "
               "max %.0f us)%s",
               ab_on ? (b ? " (B blocks)" : " (A blocks)") : "", (now - last) / 1e9, (long long)v[PANE_CHECKS],
               (long long)v[PANE_BOOKS], (long long)v[PANE_ASKED], v[PANE_ASK_NS] / 1e6, v[PANE_ASK_MAX_NS] / 1e3,
               differ);
    }
}

static int pane_readable(uint64_t a, uint64_t n) {
    if (a + n < a) return 0;   // past 2^64: Linux's plat_mem_readable says 1 to such a range (its loop never runs)
    int mode = pane_mode();
    if (mode != 3 && !(rt_trace & RT_TRACE_PERF))
        return (RT_KNOB_ON(mode) && mem_guest_readable(a, n)) || plat_mem_readable(a, n);
    int b = rt_ab(), books = (mode == 3 || (mode == 2 ? b : mode)) && mem_guest_readable(a, n), plat = 0;
    volatile int64_t *s = pane_stat[b];
    if (mode == 3 || !books) {
        uint64_t t0 = plat_now_ns();
        plat = plat_mem_readable(a, n);
        int64_t ns = (int64_t)(plat_now_ns() - t0), max = __atomic_load_n(&s[PANE_ASK_MAX_NS], __ATOMIC_RELAXED);
        plat_inc64(&s[PANE_ASKED]);
        plat_add64(&s[PANE_ASK_NS], ns);
        while (ns > max && !__atomic_compare_exchange_n(&s[PANE_ASK_MAX_NS], &max, ns, 1, __ATOMIC_RELAXED,
                                                        __ATOMIC_RELAXED)) {}
    }
    plat_inc64(&s[PANE_CHECKS]);
    if (books) plat_inc64(&s[PANE_BOOKS]);
    if (mode == 3 && books != plat) plat_inc64(&s[books ? PANE_BOOKS_ONLY : PANE_PLAT_ONLY]);
    pane_report();
    return mode == 3 ? plat : books || plat;
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
    if (size != mtx - 0x20 || (mtx & 3) || mtx < 0x70 || !pane_readable(pane, 0xa0)) return -1;
    const float *g = (const float *)(uintptr_t)mtx, *sz = (const float *)(uintptr_t)size;
    float w = fabsf(g[0] * sz[0]), h = fabsf(g[5] * sz[1]);
    *tx = g[3];
    return w > 1920 * 0.98f && h > 1080 * 0.98f;
}

// Fit a complete 16:9 layout without losing either axis. Narrow windows spend the extra height, not width.
static void aspect_ui_fit(float *m, float scale) {
    if (scale < 1) m[5] *= scale, m[7] *= scale;
    else m[0] /= scale, m[3] /= scale;
}

// CharaSelect_00 and MachineSelect_00 use 100-unit screen-capture masks at the list's
// left/right edges (1.0.0 pane traces: height 1080 / 954.8). Their texture contains the
// already fitted screen, including the footer. Fitting that capture again creates
// rectangular seams. MenuCupSelect_00 also has a 1080-square left backdrop: it must
// cover the added height, not expose its top/bottom rectangle. Pane names and sizes
// come from those layouts, confirmed in live draws. Keep controls and wide modes unchanged.
static int ui_tall_backdrop(const Cpu *c) {
    if (!(aspect_scale > 0 && aspect_scale < 1)) return 0;
    uint64_t mtx = c->x[23];
    if (mtx < 0x70 || (mtx & 3) || c->x[22] != mtx - 0x20 ||
        !pane_readable(mtx - 0x70, 0xe0)) return 0;
    const float *sz = (const float *)(uintptr_t)c->x[22];
    const char *name = (const char *)(uintptr_t)(mtx + 0x40);
    if (fabsf(sz[0] - 1080) < 0.01f && fabsf(sz[1] - 1080) < 0.01f && !memcmp(name, "P_BG_00", 8)) return 1;
    return fabsf(sz[0] - 100) < 0.01f && sz[1] >= 900 && sz[1] <= 1080 &&
        (!memcmp(name, "P_Cap_00", 9) || !memcmp(name, "P_Cap_01", 9));
}

// The character and kart lists' capture masks on a wider output. CharaSelect_00 and MachineSelect_00 hold P_Cap_00 and
// P_Cap_01 (100 units wide) under N_Root_02, at the list's outer and inner edge (1.8.0's layouts: the only P_Cap_* with
// that parent). They cover the list's edges with the screen's own backdrop, so the cards fade under the glass. A mask
// takes that backdrop at texture coordinates from its own canvas place, u = (x + 960) / 1920, while the backdrop is
// drawn wide and the mask narrowed: at 21:9 the outer mask, drawn at x / 1.33, showed the backdrop of x, a column 300
// pixels to its left, with a hard seam through the cards and the footer (hiding P_Cap_00 removed the seam; stretching
// it to the edge smeared the footer, as its u stayed on the canvas). So the mask's canvas place is divided by s and
// host_ui_proj draws it with the widened projection: it shows on screen exactly where it was, and it samples the
// backdrop under itself. The outer mask, while it rests at the list's outer edge (-908.6 character, -928.1 kart; the
// list's State animation carries it to -450), first reaches out to the screen edge, so the glass and the footer run
// to x = 0 as the screen edge closes them at 16:9. Its inner side, at the list, does not move.
#define UI_MASK_REST 900.0f

static const char *pane_name(uint64_t pane) { return (const char *)(uintptr_t)(pane + 0xb0); }
static uint64_t pane_parent(uint64_t pane) { return *(const uint64_t *)(uintptr_t)(pane + 0x18); }

// -1: the outer mask, 1: the inner one, 0: another pane (P_Cap_0x < N_Root_02 < the RootPane)
static int ui_list_mask(uint64_t pane) {
    const char *name = pane_name(pane);
    if (strncmp(name, "P_Cap_0", 7) || (name[7] != '0' && name[7] != '1') || name[8]) return 0;
    uint64_t parent = pane_parent(pane);
    if (!parent || strncmp(pane_name(parent), "N_Root_02", 24) || !pane_parent(parent) ||
        pane_parent(pane_parent(parent)))
        return 0;
    return name[7] == '0' ? -1 : 1;
}

// The masks placed by host_pane_matrix (add) and whether a drawn pane is one (query): by address and name, so a
// freed pane's address taken by another pane is not drawn wide
static int ui_mask_mark(uint64_t pane, int add) {
    static struct { uint64_t pane; char name[24]; } set[8];
    static unsigned next;
    const char *name = pane_name(pane);
    for (unsigned i = 0; i < 8; i++)
        if (set[i].pane == pane && !strncmp(set[i].name, name, 24)) return 1;
    if (!add) return 0;
    unsigned at = next++ % 8;
    set[at].pane = pane;
    strncpy(set[at].name, name, 24);
    return 1;
}

// A mask's global x row and translation (3x4 row-major g, width w) for a wider output s: the outer side to the screen
// edge while it rests there, then divided by s
static void ui_mask_place(int side, float *g, float w, float scale) {
    float hw = g[0] * w / 2, l = g[3] - hw, r = g[3] + hw;
    if (side < 0 && l <= -UI_MASK_REST) l = -960.0f * scale;
    if (w > 0) g[0] = (r - l) / w;
    g[3] = (l + r) / 2;
    g[0] /= scale, g[1] /= scale, g[2] /= scale, g[3] /= scale;
}

// The race and pause prompts (KeyAssign_00: "Photo Mode", "Settings", the controller guide) on an output that is not
// 16:9. The game draws them with the 16:9 projection over the whole picture, not the fitted one, so they sit at the
// display's corners as on the console, but their shape is stretched by the output's aspect over 16:9 (s): the "Photo
// Mode" box's width over its height is 7.9 at 16:9, 10.5 at 21:9 and 16.3 at 32:9, and squashed on a taller output.
// Their two groups, L_Key_02 under N_LeftKey_00 (bottom left) and A_Align_00 under N_Key_00 (bottom right; the only
// panes of 1.8.0's layouts with those names and parents), are placed at (x + dx) / s with their x row divided by s,
// which is where and as wide as the fitted projection shows them moved by dx = 960 (s - 1) to their side: the same
// corner, the 16:9 shape. On a taller output the same on y. Their children follow through the matrix.
static int ui_prompt(uint64_t pane, int *ax) {
    uint64_t parent = pane_parent(pane);
    if (!parent) return 0;
    const char *name = pane_name(pane), *up = pane_name(parent);
    if (!strncmp(name, "L_Key_02", 24) && !strncmp(up, "N_LeftKey_00", 24)) return *ax = -1, 1;
    if (!strncmp(name, "A_Align_00", 24) && !strncmp(up, "N_Key_00", 24)) return *ax = 1, 1;
    return 0;
}

static void ui_prompt_place(float *g, int ax, float scale) {
    if (scale > 1) {
        g[0] /= scale, g[1] /= scale, g[2] /= scale;
        g[3] = (g[3] + (float)ax * 960.0f * (scale - 1)) / scale;
    } else {   // both groups are at the bottom: dy = -540 (1 / s - 1)
        g[4] *= scale, g[5] *= scale, g[6] *= scale;
        g[7] = (g[7] - 540.0f * (1 / scale - 1)) * scale;
    }
}

// ---- The HUD and the menus' chrome at the screen edges (MKW_HUD_POSITION=edges, opt-in; 1.8.0)
// The 2D is fitted to the middle 16:9 of a wider or taller picture (host_ui_proj). Like CSS position: fixed, the HUD
// and the menus' chrome (header and footer bands, title telops, button prompts) go to their own screen edge or corner,
// and content (lists, grids, 3D previews, dialogs) stays in the middle. MKW places every pane by a translation from the
// canvas centre (all origins C/C), so the layout carries no anchor to follow; these panes get their edge from tables.
// 1. The layout. Each pane is classified by the layout it belongs to first: hud_layouts.h (build/obj/rt/, written by
//    tools/build/build.py with recomp/hud_layouts.py from recomp/hud-layouts-180.tsv, the 1.8.0 layouts' signatures, and the reviewed
//    recomp/hud-classes-180.tsv) gives every layout a class. A pane's layout is found from the root of its run-time
//    tree: the RootPane, the pane without a parent (+0x18). Its signature is the sum of a hash of its size and of each
//    distinct (depth, name) of the panes at depths 1 to 3 below it (a part pane's run-time children are its part layout's top panes),
//    from each pane's intrusive child list (head at +0x20, linked through each child's +0x08). It is computed when
//    nn::ui2d builds the root's own matrix (a new layout always builds it before its children) and cached by root.
// 2. The pane. Only hud and menu layouts move panes, and only those named in hud_anchors for that layout: a hud
//    layout's HUD pieces, a menu layout's chrome. Everything else keeps its place: content, world-placed UI, panes
//    that game code reads for geometry (N_CameraArea_*, Vote_00's overlap panes) and all capture masks (P_Cap_*).
// The offset is added to the pane's global translation right after nn::ui2d builds it (main+0x17f89f0 on 1.8.0):
// the children, the text setup and the capture path all read that matrix, so digits stay with their frames.
// Offsets come only from the room beyond the 16:9 fit, so a 16:9 output is never offset, and
// a split screen (a split camera view in the last 30 frames) moves nothing.
// An anchor names the layout, the pane, its parent and its depth below the RootPane (1: the RootPane's child): part
// layouts repeat names (CupBtn_00 has its own N_Btn_00 under N_All_00, inside MenuCupSelect_00's cup buttons; the
// first tall run moved those twice, v180p3b-probe-tall-a), and the depth tells them apart.
//   ax: -1 left, +1 right; ay: +1 top, -1 bottom; 0 keeps that axis.
//   HUD_EDGE: to the edge. An axis moves only while the pane rests in that edge's zone (|x| or |y| >= HUD_ZONE), so a
//     pane an animation carries to the middle (the character select's key guide in its other state) is not pushed.
//   HUD_BAND: a 1920-wide header or footer band: to its edge (same zone rule), and as wide as the picture on a wider
//     one; its own children (a header's title) keep their width and place on the band.
//   HUD_GROUP: a container at the canvas centre that holds only one edge's chrome (the cup select's button band):
//     moved as one group whatever its own position, so it keeps its children's In/Out animations.
//   HUD_WIDEN: as wide as the picture on a wider one, no move, children included (a band inside a moved group, or a
//     container of bands).
//   HUD_MINIMAP: the race minimap, which game code places: to the right edge, at the same share of the height; the
//     racers' map icons (MapIcon_00, its own layouts, also placed by code) inside its rect move with it.
//   HUD_SUBMAP: free roam's round minimap (SubMonitor): to its edge as HUD_EDGE, and the map icons inside its rect
//     move with it (outside a race too; hud_map_icon).
// Names, parents, positions and animations: the 1.8.0 layouts (names, numbers and flags only) and MKW_HUD_PROBE logs.
enum { HUD_EDGE, HUD_BAND, HUD_GROUP, HUD_WIDEN, HUD_MINIMAP, HUD_SUBMAP };
#define HUD_ZONE 300.0f
typedef struct { const char *layout, *pane, *parent; int depth, ax, ay, kind; } HudAnchor;
static const HudAnchor hud_anchors[] = {
    // hud: RaceInfo_1 (1 player)
    {"RaceInfo_1", "L_ItemSlot_00", "N_Root", 2, -1, 1, HUD_EDGE},        // item slot: top-left
    {"RaceInfo_1", "N_Pers_00", "N_Root", 2, -1, -1, HUD_EDGE},           // coins, lap (and balloons): bottom-left
    {"RaceInfo_1", "L_Rank_00", "N_Root", 2, 1, -1, HUD_EDGE},            // place: bottom-right
    {"RaceInfo_1", "L_PassingRange_00", "N_Root", 2, 1, -1, HUD_EDGE},    // passing range, by the place
    {"RaceInfo_1", "L_Timer_00", "N_TimeAttack_00", 3, 1, 1, HUD_EDGE},   // time trial timer: top-right
    {"RaceInfo_1", "L_ItemWarning_00", "N_Root", 2, 0, -1, HUD_EDGE},     // incoming item warning: bottom centre
    {"RaceInfo_1", "L_Hit_00", "N_Root", 2, -1, 0, HUD_EDGE},             // hit notice: left
    {"MiniMap_01", "P_Map_00", "N_SunLight_00", 4, 1, 0, HUD_MINIMAP},
    {"MiniMap_01", "P_Map_01", "N_SunLight_00", 4, 1, 0, HUD_MINIMAP},
    // hud: free roam and missions
    {"Mission_00", "L_Timer_00", "N_All_00", 2, 1, 1, HUD_EDGE},          // mission timer: top-right
    {"Mission_00", "L_PlusTime_00", "N_PlusTime_00", 3, 1, 1, HUD_EDGE},  // its added time, under it
    {"Mission_00", "L_CoinCountSet_00", "N_CoinCountSet_00", 3, 1, 1, HUD_EDGE},
    {"Mission_00", "L_Sticker_00", "N_All_00", 2, 0, -1, HUD_EDGE},       // sticker: bottom centre
    {"Mission_00", "L_Lap_00", "N_All_00", 2, -1, -1, HUD_EDGE},          // lap: bottom-left
    {"MissionTitle_00", "P_NameBase_00", "N_All_00", 2, -1, -1, HUD_EDGE},   // mission name band from the left edge
    {"MissionTitle_00", "P_NameBasePattern_00", "N_All_00", 2, -1, -1, HUD_EDGE},
    {"MissionTitle_00", "T_MissionName_00", "N_All_00", 2, -1, -1, HUD_GROUP},   // its text, with the band on both axes
    {"MapInfo_00", "L_PeachMedal_00", "N_InOut_00", 2, -1, -1, HUD_EDGE},    // counters: bottom-left
    {"MapInfo_00", "L_Panel_00", "N_InOut_00", 2, -1, -1, HUD_EDGE},
    {"MapInfo_00", "L_PSwitch_00", "N_InOut_00", 2, -1, -1, HUD_EDGE},
    {"MapInfo_01", "N_Pos_00", "N_InOut_00", 2, -1, -1, HUD_EDGE},
    // hud: Knockout Tour, battle, time limit
    {"PassedNum_00", "N_InOut_00", "N_Pos_00", 3, 1, 1, HUD_EDGE},        // pass count: top-right
    {"PassedNum_00", "N_InOut_01", "N_Pos_01", 3, 0, 1, HUD_EDGE},        // its centred variant: top
    {"TimerBattleMode_00", "N_Pos_00", "N_All_00", 2, 1, 1, HUD_EDGE},    // battle timer and team score: top-right
    {"TimeLimit_00", "N_Layout_00", "N_All_00", 2, 1, 1, HUD_EDGE},
    // hud: spectate and replay
    {"Watching_00", "N_Base_00", "N_FadeInOut_00", 3, 1, -1, HUD_EDGE},
    {"Watching_00", "L_Rank_00", "N_FadeInOut_00", 3, 1, -1, HUD_EDGE},
    {"Watching_00", "L_Lap_00", "N_FadeInOut_00", 3, -1, -1, HUD_EDGE},
    {"Watching_00", "L_Balloon_00", "N_FadeInOut_00", 3, -1, -1, HUD_EDGE},
    {"Watching_00", "L_Coin_00", "N_FadeInOut_00", 3, -1, -1, HUD_EDGE},
    {"Watching_00", "A_UserName_00", "N_FadeInOut_00", 3, 0, -1, HUD_EDGE},
    {"Watching_00", "N_Live_00", "N_InOut_00", 2, -1, 1, HUD_EDGE},
    {"Replay_00", "N_Bar_00", "N_OutUI_00", 3, -1, -1, HUD_EDGE},
    {"Replay_00", "N_LR_00", "N_OutUI_00", 3, -1, -1, HUD_EDGE},
    {"Replay_00", "N_Key_03", "N_OutUI_00", 3, 1, -1, HUD_EDGE},
    {"Replay_00", "N_KeyIcon_00", "N_OutUI_00", 3, 1, -1, HUD_EDGE},
    // hud: photo mode (the frame, the stickers and the cursor stay with the picture), pause, misc
    {"PhotoMode_00", "P_KeyAssignBase_01", "N_GuideOnOff_00", 3, -1, 0, HUD_EDGE},
    {"PhotoMode_00", "N_KeyAssign_00", "N_GuideOnOff_00", 3, -1, 0, HUD_GROUP},
    {"PhotoMode_01", "P_KeyAssignBase_01", "N_GuideOnOff_00", 4, -1, 0, HUD_EDGE},
    {"PhotoMode_01", "N_KeyAssign_00", "N_GuideOnOff_00", 4, -1, 0, HUD_GROUP},
    {"PhotoMode_01", "L_Word_13", "N_ModeSticker_00", 3, 1, -1, HUD_EDGE},
    {"PauseMenu_00", "L_SongForward_00", "N_All_00", 2, 1, 1, HUD_EDGE},
    {"OnlinePauseMenu_00", "L_SongForward_00", "N_Alll_00", 2, 1, 1, HUD_EDGE},
    // free roam's round minimap: bottom-right. Its player icon (MapIcon_00, placed by code) is not its child and did
    // not follow a plain HUD_EDGE move at 21:9 (v180p3b-rc-w21-free): HUD_SUBMAP moves it with the map
    {"SubMonitor", "P_SubMonitorSdw_00", "N_InOut_00", 2, 1, -1, HUD_EDGE},
    {"SubMonitor", "P_SubMonitor_00", "N_InOut_00", 2, 1, -1, HUD_SUBMAP},
    {"Notifications_00", "L_Notification_00", "N_Root", 2, 1, 1, HUD_EDGE},
    {"CostumeChange_1", "N_All_00", "RootPane", 1, 0, -1, HUD_EDGE},
    {"AutomaticDemo_00", "P_Logo_01", "N_All_00", 2, 1, 1, HUD_EDGE},
    {"OnlineLoad_00", "P_Load_00", "N_All_00", 2, 1, -1, HUD_EDGE},
    // menu: mode select. Its dark diagonal stripe peeks in at the bottom-right corner, and the mode's caption sits
    // on it: both to that corner (on a taller output the added rows showed most of the stripe)
    {"MenuGameSelect_01", "P_pict_00", "N_InOut_00", 3, 1, -1, HUD_EDGE},
    {"MenuGameSelect_01", "N_ThumbnailIn_01", "N_InOut_00", 3, 1, -1, HUD_GROUP},
    // menu: cup select. The button band (cups, or 50/100/150cc) to the bottom as one group, its base as wide as the
    // picture; the ticket and the 3D cup's camera areas stay (MenuCupSelect_00_In ends with the band at -455)
    {"MenuCupSelect_00", "N_Btn_00", "N_All_00", 2, 0, -1, HUD_GROUP},
    {"MenuCupSelect_00", "P_Base_00", "N_Btn_00", 3, 0, 0, HUD_WIDEN},
    // menu: Knockout rally select. Its carousel holds capture masks, so it stays; the band is widened in place
    {"MenuSurvivalSelect_00", "N_Title_00", "N_All_00", 2, -1, 1, HUD_EDGE},
    {"MenuSurvivalSelect_00", "A_KeyAssign_00", "N_All_00", 2, 1, 1, HUD_EDGE},
    {"MenuSurvivalSelect_00", "N_Base_01", "N_Btn_00", 3, 0, 0, HUD_WIDEN},
    {"MenuManual_00", "P_Base_00", "N_All_00", 2, 0, -1, HUD_BAND},
    // menu: header and footer bands
    {"NowLoading_04", "P_HeaderSh_00", "N_Frame_00", 4, 0, 1, HUD_BAND},
    {"NowLoading_04", "P_Header_00", "N_Frame_00", 4, 0, 1, HUD_BAND},
    {"NowLoading_04", "P_FooterSh_00", "N_Frame_00", 4, 0, -1, HUD_BAND},
    {"NowLoading_04", "P_Footer_00", "N_Frame_00", 4, 0, -1, HUD_BAND},
    {"NowLoading_04", "P_Logo_00", "N_InOut_00", 3, 1, 1, HUD_EDGE},
    {"NowLoading_01", "P_Logo_00", "N_InOut_00", 3, 1, 1, HUD_EDGE},
    {"NowLoading_00", "L_parts_00", "N_InOUt_00", 3, 1, -1, HUD_GROUP},   // the loading item icon: bottom-right
    {"TireWipe_01", "L_LoadingItemSlot_00", "N_ItemSlotOnOff_00", 2, 1, -1, HUD_GROUP},
    {"CharacterSelectBG_00", "P_Base_00", "N_Screen_00", 2, 0, -1, HUD_BAND},
    {"CharacterSelectBG_00", "P_Base_01", "N_Screen_03", 2, 0, -1, HUD_BAND},
    {"CharaSelect_00", "P_Base_02", "N_Base_00", 3, 0, -1, HUD_BAND},
    {"MachineSelect_00", "P_Base_02", "N_Base_00", 3, 0, -1, HUD_BAND},
    {"FaceSelect_00", "P_Header_00", "N_All_00", 2, 0, 1, HUD_BAND},
    {"FaceSelect_00", "P_Footer_00", "N_All_00", 2, 0, -1, HUD_BAND},
    {"FaceSelect_00", "N_FaceList_00", "N_All_00", 2, -1, -1, HUD_EDGE},
    {"CharaChangeOk_00", "P_Base_00", "N_All_00", 2, 0, -1, HUD_BAND},
    {"CharaChangeOk_00", "L_parts_00", "N_All_00", 2, 1, -1, HUD_EDGE},
    {"CharaChangeOk_00", "T_Change_00", "N_All_00", 2, 0, -1, HUD_EDGE},
    {"TelopFooter_00", "N_VRS_00", "N_All_00", 2, 0, -1, HUD_EDGE},
    {"TelopFooter_00", "P_Base_00", "N_All_00", 2, 0, -1, HUD_EDGE},
    {"TelopFooter_00", "P_Base_01", "N_All_00", 2, 0, -1, HUD_EDGE},
    {"TelopFooter_00", "T_Message_01", "N_All_00", 2, 0, -1, HUD_EDGE},
    {"TelopFooter_01", "P_Base_00", "N_State_00", 4, 0, -1, HUD_EDGE},
    {"TelopFooter_01", "P_Base_01", "N_State_00", 4, 0, -1, HUD_EDGE},
    {"TelopFooter_01", "N_Message_00", "N_State_00", 4, 0, -1, HUD_EDGE},
    {"TelopFooter_01", "P_Base_02", "N_State_01", 4, 0, -1, HUD_BAND},
    {"TelopFooter_01", "P_Base_03", "N_State_01", 4, 0, -1, HUD_BAND},
    {"TelopFooter_01", "N_Message_01", "N_State_01", 4, 0, -1, HUD_EDGE},
    {"TelopFooter_01", "N_ProgressBar_00", "N_All_00", 2, 0, -1, HUD_EDGE},
    // menu: title screen and logos
    {"TitleLauncher_00", "N_Logo_00", "N_All_00", 2, -1, -1, HUD_EDGE},
    {"TitleLauncher_00", "N_Load_00", "N_All_00", 2, 1, -1, HUD_EDGE},
    {"TitleLogo_00", "T_VerNum_00", "N_Logo_00", 2, 1, -1, HUD_EDGE},
    {"TitleLogo_00", "T_Copyright_00", "N_Logo_00", 2, 1, -1, HUD_EDGE},
    // menu: button prompts (BtnIconFooter_00 in alignment panes; anchor the alignment pane)
    {"CharaSelect_00", "A_alignment_00", "N_Ready_01", 4, 1, -1, HUD_EDGE},
    {"MachineSelect_00", "A_alignment_00", "N_Ready_00", 4, 1, -1, HUD_EDGE},
    {"FaceSelect_00", "A_alignment_00", "N_All_00", 2, 1, -1, HUD_EDGE},
    {"Skip", "A_alignment_00", "N_Alll_00", 3, 1, -1, HUD_EDGE},
    // KeyAssign_00, the race and pause prompts: the game itself places them at the display's corners; ui_prompt keeps
    // their shape there, with or without these anchors (a HUD_EDGE anchor would move them a second time)
    {"MapKey_00", "A_alignment_00", "N_All_00", 2, 1, 1, HUD_EDGE},
    {"BattleCourseSelect_01", "A_alignment_00", "N_Key_00", 3, 1, 1, HUD_EDGE},
    {"StickerSelect_00", "A_alignment_01", "N_State_02", 3, 1, -1, HUD_EDGE},
    {"Observatory_00", "A_alignment_00", "N_keyAssign_00", 3, 0, -1, HUD_EDGE},
    {"Retry_00", "N_Retry_00", "N_All_00", 2, 0, -1, HUD_EDGE},
    {"RuleSelect_00", "N_OK_00", "N_All_00", 2, 0, -1, HUD_EDGE},
    {"ChatSelect_00", "N_KeyAsign_00", "N_All_00", 2, -1, -1, HUD_EDGE},
    {"Vote_00", "A_KeyAssign_00", "N_Key_00", 3, 1, 1, HUD_EDGE},
};

// The canvas offset of an anchor for the output's aspect s (over 16:9) and the pane's own global position (x, y):
// x by 960 (s - 1) on a wider output, y by 540 (1/s - 1) on a taller one, each only inside its edge's zone (a group
// always); the minimap's y by its share of the added height.
static void hud_offset(const HudAnchor *a, float scale, float x, float y, float *dx, float *dy) {
    *dx = *dy = 0;
    if (!(scale > 0)) return;
    int any = a->kind == HUD_GROUP || a->kind == HUD_MINIMAP;
    if (scale > 1 && a->ax && (any || a->ax * x >= HUD_ZONE)) *dx = (float)a->ax * 960.0f * (scale - 1);
    if (scale < 1) {
        if (a->kind == HUD_MINIMAP) *dy = y * (1 / scale - 1);
        else if (a->ay && (any || a->ay * y >= HUD_ZONE)) *dy = (float)a->ay * 540.0f * (1 / scale - 1);
    }
}


// FNV-1a 64 of a depth byte and a pane name (at most 24 bytes), as recomp/hud_layouts.py computes it
static uint64_t hud_fnv(int depth, const char *name) {
    uint64_t h = (1469598103934665603ull ^ (uint8_t)depth) * 1099511628211ull;
    for (int i = 0; i < 24 && name[i]; i++) h = (h ^ (uint8_t)name[i]) * 1099511628211ull;
    return h;
}

// The hashes of the run-time children at depths 1..3 (the intrusive list: head at +0x20, nodes at each child's +0x08,
// ending back at the head), at most HUD_SIG_MAX of them (a longer or broken list gives no signature)
#define HUD_SIG_MAX 1024
static void hud_sig_walk(uint64_t pane, int depth, uint64_t *h, int *n) {
    uint64_t head = pane + 0x20, node = *(const uint64_t *)(uintptr_t)head;
    while (node && node != head && *n <= HUD_SIG_MAX) {
        uint64_t child = node - 8;
        if (*n < HUD_SIG_MAX) h[*n] = hud_fnv(depth, pane_name(child));
        ++*n;
        if (depth < 3) hud_sig_walk(child, depth + 1, h, n);
        node = *(const uint64_t *)(uintptr_t)node;
    }
}

static int hud_u64_cmp(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

// The size's hash plus each distinct (depth, name) hash once: panes the game clones at run time (a second
// L_ItemWarning_00 in RaceInfo_1, v180p3b-probe5-tall) leave the signature as recomp/hud_layouts.py computes it
static uint64_t hud_layout_sig(uint64_t root) {
    const float *size = (const float *)(uintptr_t)(root + 0x50);
    char wh[32];
    snprintf(wh, sizeof wh, "%dx%d", (int)size[0], (int)size[1]);
    uint64_t h[HUD_SIG_MAX], sum = hud_fnv(0, wh);
    int n = 0;
    hud_sig_walk(root, 1, h, &n);
    if (n > HUD_SIG_MAX) return 0;
    qsort(h, (size_t)n, sizeof *h, hud_u64_cmp);
    for (int i = 0; i < n; i++)
        if (!i || h[i] != h[i - 1]) sum += h[i];
    return sum;
}

static int hud_layout_find(uint64_t sig) {
    for (int i = 0; i < (int)(sizeof hud_layouts / sizeof *hud_layouts); i++)
        if (hud_layouts[i].sig == sig) return i;
    return -1;
}

// Roots seen, with their layout (-1: unknown, nothing moves)
#define HUD_ROOTS 64
static struct { uint64_t root; int layout; } hud_roots[HUD_ROOTS];
static unsigned hud_roots_next;
static int hud_probe_mode(void) {
    static int mode = -1;
    if (mode < 0) mode = cfg_get("MKW_HUD_PROBE") ? atoi(cfg_get("MKW_HUD_PROBE")) : 0;
    return mode;
}

// MKW_HUD_PROBE=5: an unknown layout's whole signature input (depth and name of each pane at depths 1 to 3)
static void hud_probe_names(uint64_t pane, int depth, int *budget) {
    uint64_t head = pane + 0x20, node = *(const uint64_t *)(uintptr_t)head;
    while (node && node != head && --*budget > 0) {
        uint64_t child = node - 8;
        rt_log("hud:   name %d %.24s < %.24s", depth, pane_name(child), pane_name(pane));
        if (depth < 3) hud_probe_names(child, depth + 1, budget);
        node = *(const uint64_t *)(uintptr_t)node;
    }
}

static void hud_probe_layout(uint64_t root, uint64_t sig, int layout) {
    static uint64_t seen[256];
    static int n;
    if (hud_probe_mode() < 4) return;
    for (int i = 0; i < n; i++)
        if (seen[i] == sig) return;
    if (n < 256) seen[n++] = sig;
    rt_log("hud: layout %s (%s) sig %016llx root %#llx frame %llu", layout >= 0 ? hud_layouts[layout].name : "?",
           layout >= 0 ? hud_class_names[hud_layouts[layout].cls] : "unknown", (unsigned long long)sig,
           (unsigned long long)root, (unsigned long long)frame_step_ticks());
    if (layout < 0) {   // its top panes, to name it from the inventory
        uint64_t head = root + 0x20, node = *(const uint64_t *)(uintptr_t)head;
        for (int k = 0; node && node != head && k < 12; k++, node = *(const uint64_t *)(uintptr_t)node) {
            uint64_t child = node - 8, h2 = child + 0x20, n2 = *(const uint64_t *)(uintptr_t)h2;
            char kids[160] = "";
            for (int j = 0; n2 && n2 != h2 && j < 6; j++, n2 = *(const uint64_t *)(uintptr_t)n2)
                snprintf(kids + strlen(kids), sizeof kids - strlen(kids), " %.24s", pane_name(n2 - 8));
            rt_log("hud:   top %.24s:%s", pane_name(child), kids);
        }
        int budget = 600;
        if (hud_probe_mode() == 5) hud_probe_names(root, 1, &budget);
    }
}

// The layout of a root: computed when the root's own matrix is built (fresh), else from the cache
static int hud_layout_of_root(uint64_t root, int fresh) {
    unsigned at = HUD_ROOTS;
    for (unsigned i = 0; i < HUD_ROOTS; i++)
        if (hud_roots[i].root == root) {
            if (!fresh) return hud_roots[i].layout;
            at = i;
            break;
        }
    uint64_t sig = hud_layout_sig(root);
    int layout = sig ? hud_layout_find(sig) : -1;
    hud_probe_layout(root, sig, layout);
    if (at == HUD_ROOTS) at = hud_roots_next++ % HUD_ROOTS;
    hud_roots[at].layout = layout, hud_roots[at].root = root;
    return layout;
}

// The layout of a pane's run-time tree, and the pane's depth below its RootPane
static int hud_layout_of(uint64_t pane, int *depth) {
    uint64_t root = pane, parent;
    int d = 0;
    while (d < 32 && (parent = pane_parent(root))) root = parent, d++;
    *depth = d;
    return pane_parent(root) ? -1 : hud_layout_of_root(root, root == pane);
}

// The anchors' layouts as indices into hud_layouts (resolved once)
static int hud_anchor_layout[sizeof hud_anchors / sizeof *hud_anchors];

static void hud_resolve(void) {
    for (size_t i = 0; i < sizeof hud_anchors / sizeof *hud_anchors; i++) {
        hud_anchor_layout[i] = -1;
        for (int k = 0; k < (int)(sizeof hud_layouts / sizeof *hud_layouts); k++)
            if (!strcmp(hud_layouts[k].name, hud_anchors[i].layout)) hud_anchor_layout[i] = k;
    }
}

static const HudAnchor *hud_anchor_of(uint64_t pane, int layout, int depth) {
    if (layout < 0 || (hud_layouts[layout].cls != HUD_CLASS_HUD && hud_layouts[layout].cls != HUD_CLASS_MENU))
        return NULL;
    const char *name = pane_name(pane);
    uint64_t parent = pane_parent(pane);
    if (!parent) return NULL;
    for (size_t i = 0; i < sizeof hud_anchors / sizeof *hud_anchors; i++)
        if (hud_anchor_layout[i] == layout && hud_anchors[i].depth == depth && !strncmp(name, hud_anchors[i].pane, 24) &&
            !strncmp(pane_name(parent), hud_anchors[i].parent, 24))
            return &hud_anchors[i];
    return NULL;
}

// Bands widened (HUD_BAND), by pane and name: their children are drawn at their own width and place on the band
#define HUD_BANDS 16
static struct { uint64_t pane, name; } hud_bands[HUD_BANDS];
static unsigned hud_bands_next;

static void hud_band_add(uint64_t pane) {
    uint64_t name = hud_fnv(0, pane_name(pane));
    for (unsigned i = 0; i < HUD_BANDS; i++)
        if (hud_bands[i].pane == pane && hud_bands[i].name == name) return;
    unsigned at = hud_bands_next++ % HUD_BANDS;
    hud_bands[at].pane = pane, hud_bands[at].name = name;
}

static int hud_band_child(uint64_t pane) {
    uint64_t parent = pane_parent(pane);
    if (!parent) return 0;
    for (unsigned i = 0; i < HUD_BANDS; i++)
        if (hud_bands[i].pane == parent && hud_bands[i].name == hud_fnv(0, pane_name(parent))) return 1;
    return 0;
}

// The minimap as last built: its rect before the move and the move, for the map icons drawn over it.
// sub: the rect is SubMonitor's (HUD_SUBMAP).
static struct { float x0, y0, x1, y1, dx, dy; int valid, sub; } hud_map;

// A racer's map icon: the root content pane (N_All_00, under its layout's RootPane) of a MapIcon_* instance, in a race,
// inside the minimap's rect (with a margin for the icon) before the move. Its children (N_All_01 ...) follow it.
// (A first version took N_All_01 and required a RootPane with no parent; the icons are parts of another layout, so it
// never matched, and at 21:9 and 32:9 they stayed behind the moved map: v180p3-candidate-w21/w32.)
static int hud_in_map(const float *g) {
    return g[3] >= hud_map.x0 - 48 && g[3] <= hud_map.x1 + 48 && g[7] >= hud_map.y0 - 48 && g[7] <= hud_map.y1 + 48;
}

// Free roam's player icon on SubMonitor is a MapIcon layout of its own (its RootPane has no parent) whose N_All_00
// stays at 0,0; a pane below it carries the icon to the map (N_BigSmall_00 < N_All_01 < N_All_00 at 750,-450 while
// the map rests at 750,-330: v180p3c-probe1-w32-free, v180p3c-probe2-w21-free). With the round map, a pane of a
// MapIcon layout (or of an unidentified one) that is in the rect moves, unless its parent moved.
static int hud_map_icon(uint64_t pane, const float *g, int layout) {
    if (!hud_map.valid) return 0;
    uint64_t parent = pane_parent(pane);
    // (nn::ui2d rebuilds a matrix only when its pane changed, so the map's last build is not a sign of a live map; the
    // round map's rect stays in force until the race minimap replaces it, and only MapIcon panes use it)
    if (hud_map.sub) {
        // The icon's N_All_01 is not built through the hooked call sites (it was never seen here; its child
        // N_BigSmall_00 was, at the map: v180p3c-probe2c-w21-free), so the panes moved here are remembered: a pane
        // in the rect moves unless its parent did (its global already holds the move).
        static uint64_t moved[64];
        static unsigned next;
        if (!parent || !hud_in_map(g) || (layout >= 0 && strncmp(hud_layouts[layout].name, "MapIcon", 7))) return 0;
        int move = 1;
        for (int i = 0; i < 64; i++)
            if (moved[i] == parent) move = 0;
        if (move) {
            int known = 0;
            for (int i = 0; i < 64; i++)
                if (moved[i] == pane) known = 1;
            if (!known) moved[next++ % 64] = pane;
        }
        static int n;
        if (hud_probe_mode() == 2 && n++ < 60)
            rt_log("hud: round-map icon? %.24s < %.24s at %.1f,%.1f (layout %s): %d", pane_name(pane),
                   pane_name(parent), g[3], g[7], layout >= 0 ? hud_layouts[layout].name : "?", move);
        return move;
    }
    if (strncmp(pane_name(pane), "N_All_00", 24)) return 0;
    if (!parent || strncmp(pane_name(parent), "RootPane", 24)) return 0;
    int in = hud_in_map(g);
    static int n;
    if (hud_probe_mode() == 2 && n < 60) {
        n++;
        uint64_t grand = pane_parent(parent);
        rt_log("hud: icon? at %.1f,%.1f in map %.1f..%.1f,%.1f..%.1f: %d, race %d, root's parent %.24s", g[3], g[7],
               hud_map.x0, hud_map.x1, hud_map.y0, hud_map.y1, in, race_scene_on(), grand ? pane_name(grand) : "-");
    }
    return in && race_scene_on();
}

// MKW_HUD_PROBE=1: each distinct pane/parent/grandparent with its global position, once (finding anchors in a run)
static void hud_probe(uint64_t pane, const float *g) {
    static int n;
    if (hud_probe_mode() != 1 || n >= 2000) return;
    static uint64_t seen[2048];
    uint64_t parent = pane_parent(pane);
    uint64_t grand = parent ? pane_parent(parent) : 0;
    uint64_t h = hud_fnv(0, pane_name(pane)) + (parent ? hud_fnv(1, pane_name(parent)) : 0);
    for (int i = 0; i < n; i++)
        if (seen[i] == h) return;
    seen[n++] = h;
    const float *size = (const float *)(uintptr_t)(pane + 0x50);
    rt_log("hud: probe %.24s < %.24s < %.24s at %.1f,%.1f size %.1fx%.1f scale %.3f,%.3f frame %llu", pane_name(pane),
           parent ? pane_name(parent) : "-", grand ? pane_name(grand) : "-", g[3], g[7], size[0], size[1], g[0], g[5],
           (unsigned long long)frame_step_ticks());
}

// MKW_HUD_PROBE=4: each anchor's first move (layout, pane, its rest position and offset)
static void hud_probe_dock(const HudAnchor *a, const float *g, float dx, float dy) {
    static unsigned char seen[sizeof hud_anchors / sizeof *hud_anchors];
    size_t i = (size_t)(a - hud_anchors);
    if (hud_probe_mode() < 4 || seen[i]) return;
    seen[i] = 1;
    rt_log("hud: dock %s %s < %s at %.1f,%.1f by %.1f,%.1f", a->layout, a->pane, a->parent, g[3], g[7], dx, dy);
}


static int hud_edges(void) {
    static int on = -1;
    if (on < 0) on = !strcmp(cfg_value("MKW_HUD_POSITION"), "edges");
    return on;
}

// A pane of a hud or menu layout moved to its edge, a widened band's child kept at its width, or a map icon moved with
// its map (g: its global matrix, 3x4 row-major; layout and depth: hud_layout_of)
static void hud_dock(uint64_t pane, float *g, int layout, int depth) {
    const HudAnchor *a = hud_anchor_of(pane, layout, depth);
    float dx = 0, dy = 0;
    if (a) {
        hud_offset(a, aspect_scale, g[3], g[7], &dx, &dy);
        if (a->kind == HUD_MINIMAP || a->kind == HUD_SUBMAP) {
            // the extent through the whole 2x2: the map's own matrix need not be a plain scale (a first version read
            // only g[0]/g[5] and got an empty rect at run time)
            const float *size = (const float *)(uintptr_t)(pane + 0x50);
            float hw = (fabsf(g[0]) * size[0] + fabsf(g[1]) * size[1]) / 2;
            float hh = (fabsf(g[4]) * size[0] + fabsf(g[5]) * size[1]) / 2;
            hud_map.x0 = g[3] - hw, hud_map.x1 = g[3] + hw, hud_map.y0 = g[7] - hh, hud_map.y1 = g[7] + hh;
            hud_map.dx = dx, hud_map.dy = dy, hud_map.valid = 1;
            hud_map.sub = a->kind == HUD_SUBMAP;
        }
        if ((a->kind == HUD_BAND || a->kind == HUD_WIDEN) && aspect_scale > 1) {
            g[0] *= aspect_scale, g[1] *= aspect_scale, g[2] *= aspect_scale;
            if (a->kind == HUD_BAND) hud_band_add(pane);
        }
        hud_probe_dock(a, g, dx, dy);
    } else if (aspect_scale > 1 && hud_band_child(pane)) {
        // a widened band's child: undo the band's stretch (its x row was scaled by aspect_scale about the band)
        const float *b = (const float *)(uintptr_t)(pane_parent(pane) + 0x70);
        g[0] /= aspect_scale, g[1] /= aspect_scale, g[2] /= aspect_scale;
        g[3] = b[3] + (g[3] - b[3]) / aspect_scale;
        return;
    } else if (hud_map_icon(pane, g, layout)) {
        dx = hud_map.dx, dy = hud_map.dy;
    }
    g[3] += dx, g[7] += dy;
}

// In place of the 1.8.0 calls of nn::ui2d's pane global-matrix builder (pane, draw info; version.c pane_matrix_fn):
// the builder itself, then a list mask, a prompt or (MKW_HUD_POSITION=edges) a race HUD piece placed for an output
// that is not 16:9. Children built after it multiply by its matrix.
static void host_pane_matrix(Cpu *c) {
    static int resolved;   // a flag, not a counter: once++ on every pane overflowed a signed int (UB) in a long run
    if (!resolved) resolved = 1, hud_resolve();
    uint64_t pane = c->x[0], info = c->x[1];
    rt_call(c, rt_main_addr(rt_version()->pane_matrix_fn), 2, pane, info);
    if (!(aspect_scale > 0) || aspect_scale == 1) return;
    float *g = (float *)(uintptr_t)(pane + 0x70);
    int depth = 0, layout = hud_edges() || hud_probe_mode() ? hud_layout_of(pane, &depth) : -1;
    hud_probe(pane, g);
    uint64_t at = split_at;
    if (at && frame_step_ticks() + 1 - at < 30) return;   // a split screen keeps its place
    int side = aspect_scale > 1 ? ui_list_mask(pane) : 0, ax;
    if (side) {
        ui_mask_place(side, g, ((const float *)(uintptr_t)(pane + 0x50))[0], aspect_scale);
        ui_mask_mark(pane, 1);
    } else if (ui_prompt(pane, &ax)) {
        ui_prompt_place(g, ax, aspect_scale);
    } else if (hud_edges()) {
        hud_dock(pane, g, layout, depth);
    }
}

static void host_ui_proj(Cpu *c) {
    float *s = (float *)(uintptr_t)(c->x[0] + 0x10);
    float *m = (float *)(uintptr_t)c->x[1];
    memcpy(m, s, 16 * sizeof *m);
    int on = is_16x9(m);
    // Narrowed in DrawInfo too, so its other readers see it: the labels drawn through a texture (the main menu's, the
    // cup select's) had their place stretched with their shape right (uw8-menu). Narrowed, it no longer passes
    // is_16x9: a second call leaves it.
    if (on) aspect_ui_fit(m, aspect_scale), memcpy(s, m, 16 * sizeof *m);
    // A pane over the whole canvas (fades, blurred backdrops, the cup map: uw11-menu showed the race loading at the
    // sides of a fade) is drawn wide again, to the edges of the picture; a movie's stays 16:9 (bars at the present)
    float tx;
    int full = aspect_out ? ui_full_screen(c, &tx) : -1;
    if ((full == 1 || ui_tall_backdrop(c)) && !ov_movie_playing()) {
        if (aspect_scale < 1) m[5] /= aspect_scale, m[7] /= aspect_scale;
        else m[0] *= aspect_scale, m[3] *= aspect_scale;
    } else if (full == 0 && aspect_scale > 1 && c->x[22] == c->x[23] - 0x20 && pane_readable(c->x[23] - 0x70, 0xe0) &&
               ui_mask_mark(c->x[23] - 0x70, 0)) {
        m[0] *= aspect_scale, m[3] *= aspect_scale;   // a list mask placed by host_pane_matrix: drawn wide, where it was
    }
    // A split screen (uw14-split): each player's HUD is on its half of the canvas (its quarter with three or four,
    // left or right all the same), now in the middle of the picture; moved to the middle of its half of the picture,
    // with its shape. In NDC, x' = x + d with d = +-0.5 *
    // (1 - 1 / aspect_scale), added as d times the w row
    uint64_t at = split_at;
    int moved = aspect_scale > 1 && full == 0 && at && frame_step_ticks() + 1 - at < 30 && (tx > 1 || tx < -1);
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
    if (on) aspect_ui_fit(m, aspect_scale);
    memcpy(&c->v[2], m, 16), memcpy(&c->v[3], m + 4, 16);
}

// 1.8.0's same load, ldp q0, q4, [x21, #0x10] at main+0x17f5084 (its compiler moved DrawInfo to x21 and the rows to
// q0 and q4; q2 was loaded just before, so host_ui_rows would overwrite it).
static void host_ui_rows2(Cpu *c) {
    float m[16];
    memcpy(m, (const void *)(uintptr_t)(c->x[21] + 0x10), sizeof m);
    if (is_16x9(m)) aspect_ui_fit(m, aspect_scale);
    memcpy(&c->v[0], m, 16), memcpy(&c->v[4], m + 4, 16);
}

// The screen effects (issue #425: Knockout Tour's checkpoint rainbow, Drv_SurvivalScreenEffect) are a frame mesh and
// emitters held a few units in front of the camera, sized for a 16:9 view: under the widened projection they covered
// only its middle 16:9 (wi425b dump: their vertex shader places them with nn::vfx2's view constants, c8, projection
// rows 4-7, which carry the race camera's x scale 0.6727 at 21:9). A view reaches the GPU buffer a draw binds as c8
// through a memcpy of its ViewParam (0x250 bytes) in main+0x22d1f4 or in its twins main+0xd60a0 and main+0x166bfa0;
// the screen effects' goes through main+0xd60a0's, where host_fx_view makes the copy and, for them, gives the GPU
// copy 16:9's x scale: the projection's and the view-projection's x rows times the widening, the x columns of their
// inverses divided by it (rows of 16 bytes: projection 4-7, view-projection 8-11, inverse projection 16-19, inverse
// view-projection 20-23). The screen effects are draw group 7 of the group draw main+0x14df10 (its w3, kept in x27
// and saved by main+0xd58f8's prologue at [x29 + 0x18]; the world's effects are groups 2 and 12): RtVersion
// fx_group_lr is main+0xd58f8's return there, fx_group_frame/word where the group is saved and fx_screen_group the
// group (wi425i-j). MKW_FX_WIDE_LR=all
// (diagnosis) widens every copy there; with MKW_PROBE, each copy in the window logs its site, slot, group, return
// addresses and GPU buffer.
static int fx_wide_all = -1;

static void host_fx_view(Cpu *c) {
    float *vp = (float *)(uintptr_t)c->x[0];
    memcpy(vp, (const void *)(uintptr_t)c->x[1], c->x[2]);
    if (aspect_scale <= 1) return;
    int all = __atomic_load_n(&fx_wide_all, __ATOMIC_ACQUIRE);
    if (all < 0) {
        const char *e = cfg_get("MKW_FX_WIDE_LR");
        all = e && !strcmp(e, "all");
        if (all) rt_log("patches: every effect view at 16:9 (MKW_FX_WIDE_LR all)");
        __atomic_store_n(&fx_wide_all, all, __ATOMIC_RELEASE);
    }
    // the copy's frame record (x29) holds its caller's (main+0xd58f8's) frame and return address; one up, the group
    // draw's return address; the group is a register one of the two prologues saved
    const RtVersion *v = rt_version();
    const uint64_t *f1 = (const uint64_t *)(uintptr_t)c->x[29];
    const uint64_t *f2 = (const uint64_t *)(uintptr_t)f1[0];
    const uint64_t *fg = v->fx_group_frame == 1 ? f1 : f2;
    uint64_t lr1 = f1[1], lr2 = f2 ? f2[1] : 0, group = fg ? fg[v->fx_group_word] : 0;
    int wide = all || (v->fx_group_lr && lr2 == rt_main_addr(v->fx_group_lr) && group == v->fx_screen_group);
    if (seqwatch_probe()) {
        uint64_t base = rt_main_addr(4) - 4;
        rt_log("patches: probe fxview frame %llu site main+%#llx slot %d group %#llx lr main+%#llx main+%#llx dst %#llx "
               "x %g%s", (unsigned long long)frame_step_ticks(), (unsigned long long)(c->x[30] - base), (int)c->x[23],
               (unsigned long long)group, (unsigned long long)(lr1 - base), (unsigned long long)(lr2 - base),
               (unsigned long long)c->x[0], vp[16], wide ? " -> 16:9" : "");
    }
    if (!wide) return;
    for (int i = 0; i < 4; i++) {
        vp[16 + i] *= aspect_scale, vp[32 + i] *= aspect_scale;          // rows 4 and 8
        vp[64 + 4 * i] /= aspect_scale, vp[80 + 4 * i] /= aspect_scale;  // column 0 of rows 16-19 and 20-23
    }
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
    if (aspect_out > 0 && aspect_out < 16.0f / 9 && !has_set(g, "vertplus")) {
        rt_log("patches: tall Window needs regenerated game code with the vertplus FOV set; keeping 16:9");
        aspect_out = 0;
    }
    if (!aspect_out && strcmp(cfg_value("MKW_ASPECT"), "16:9") && !has_set(g, "aspect"))
        rt_log("patches: MKW_ASPECT %s has no effect on game version %s (no aspect set)", cfg_value("MKW_ASPECT"),
               rt_version()->display);
    aspect_scale = aspect_out / (16.0f / 9);
    gfx_aspect_set(aspect_out);
    for (int i = 0; i < g->npatch_sets; i++) {
        if (!strcmp(g->patch_sets[i].name, "aspect")) g->patch_on[i] = aspect_out != 0;
        if (!strcmp(g->patch_sets[i].name, "vertplus"))
            g->patch_on[i] = aspect_out > 0 && aspect_scale < 1;
        if (!strcmp(g->patch_sets[i].name, "display")) g->patch_on[i] = aspect_out != 0;   // 1.8.0's table only
    }
    if (aspect_out) rt_log("patches: camera aspect %.3f, x%.3f over 16:9 (MKW_ASPECT %s)", aspect_out, aspect_scale,
                           cfg_value("MKW_ASPECT"));
}

// The output set: with MKW_RESOLUTION not original, that resolution's lines at 16:9 (720: 1280x720) in place of the
// output the game fixes in code (1.0.0 2560x1440, 1.8.0 3840x2160: RtVersion.output_lines); with original, the lines
// the picture gets in the window when fewer than that
// (Adrià 08/11/2026: the present shrank 2560x1440 to a 1080-line window with its 2x2 bilinear blit, jagged edges,
// nit-off; a smaller output goes through FSR instead). MKW_OUTPUT_PATCH=0 keeps the game's at any resolution. With
// MKW_ASPECT set, the width is the lines times that aspect (1440 lines when nothing else sets them).
static void aspect_tall_target(uint32_t wh[2], int lines, float aspect, int native_w, int native_h) {
    // Keep horizontal resolution; add render rows rather than shrinking the image's width.
    wh[0] = (uint32_t)(lines * 16 / 9);
    wh[1] = ((uint32_t)(wh[0] / aspect + 0.5f) + 1) & ~1u;
    if (native_w > 0 && native_h > 0) wh[0] = (uint32_t)native_w, wh[1] = (uint32_t)native_h;
}

static void output_size(const RtGame *g) {
    const char *res = cfg_value("MKW_RESOLUTION"), *off = cfg_get("MKW_OUTPUT_PATCH");
    int lines = strcmp(res, "original") ? cfg_int_in("MKW_RESOLUTION", 0, 4320) : 0, native = rt_version()->output_lines;
    // 1.8.0 draws its last pass at the screen render's size (2560x1440 at original) whatever the output: an output
    // with fewer lines cuts the picture to its top-left corner (pr450-180b/d, vp 2560x1440 on a 1280x800 texture).
    // Its output keeps at least those lines, 16:9; the present stretches the crop to MKW_ASPECT (gfx_present.c).
    int at_screen = rt_version()->final_at_screen;
    if (!lines && !at_screen) {
        // the lines the picture gets in the window: fewer than the window's when it is narrower than the picture and
        // the present letterboxes it (gfx_fit_rect, MKW_PICTURE_FIT=keep). A 16:9 picture on the Steam Deck's
        // 1280x800 gets 720, and an output of 800 lines (1422x800, 1.23 times the pixels) was shrunk to them.
        int w = 0, h = 0;
        if (gfx_window_shape(&w, &h)) {
            float a = aspect_out ? aspect_out : 16.0f / 9;
            if (w > 0 && strcmp(cfg_value("MKW_PICTURE_FIT"), "stretch") && w < h * a) h = (int)(w / a + 0.5f);
            if (h >= 360 && h < native) lines = h;
            // with a temporal upscaler (gfx/vk/exec/vk_temporal.c) a taller window gets its own lines: the upscaler and
            // the HUD then reach the window at its resolution instead of being scaled up from the game's own output
            else if (h > native && strcmp(cfg_value("MKW_TEMPORAL"), "off")) lines = h;
        }
    }
    if (!lines && aspect_out && !(at_screen && aspect_scale < 1)) lines = 1440;
    // With the display set (1.8.0) the picture takes the output's shape itself (display_fit): a tall output gets the
    // rows of that shape too, or the fitted picture would be shrunk into 16:9's lines. Without a size of its own the
    // output stays the game's (output_lines at 16:9: 3840x2160), which display_fit fits.
    int display = 0;
    for (int i = 0; i < g->npatch_sets; i++)
        if (!strcmp(g->patch_sets[i].name, "display")) display = g->patch_on[i];
    output_wh[0] = (uint32_t)(native * 16 / 9), output_wh[1] = (uint32_t)native;
    for (int i = 0; i < g->npatch_sets; i++) {
        if (strcmp(g->patch_sets[i].name, "output")) continue;
        if (lines <= 0 || (off && !strcmp(off, "0"))) {
            g->patch_on[i] = 0;   // the page holds no size: the site would read 0x0
            return;
        }
        int tall_at_screen = at_screen && aspect_out > 0 && aspect_scale < 1;   // 16:9 at the lines
        uint32_t wide = aspect_out && !tall_at_screen ? ((uint32_t)(lines * aspect_out + 0.5f) + 1) & ~1u : 0;
        uint32_t wh[2] = {wide ? wide : (uint32_t)(lines * 16 / 9), (uint32_t)lines};
        if (aspect_out > 0 && aspect_scale < 1 && (!at_screen || display)) {
            int w = 0, h = 0;
            if (!strcmp(res, "original") && !at_screen) gfx_window_shape(&w, &h);
            aspect_tall_target(wh, lines, aspect_out, w, h);
        }
        if (at_screen) {
            // 1.8.0 crops its picture out of the window textures (WindowCrop), so a bigger texture costs memory only,
            // while a smaller one cuts what the game draws past it: the last pass at the 3D scene's lines (For3D =
            // lines / 0.65 docked, #512) and the photo mode's 1.5 x Screen supersample (#515 #545 #687, 2880x1620 at
            // 1080) were clipped to the corner, a x1.5 zoom (p515a/p515b, 26/11/2026: the same photo with the output
            // at 3840x2160 came out right). So the textures hold 1/0.65 of the lines, 8K at most.
            for (int k = 0; k < 2; k++) wh[k] = ((uint32_t)ceil(wh[k] / OUTPUT_SCENE_SCALE) + 1) & ~1u;
            if (wh[1] > 4320) wh[0] = wh[0] * 4320u / wh[1] & ~1u, wh[1] = 4320;
        }
        output_wh[0] = wh[0], output_wh[1] = wh[1];
        memcpy((void *)(uintptr_t)(g->patch_page + OUTPUT_SIZE), wh, sizeof wh);
        g->patch_on[i] = 1;
        rt_log("patches: game output %ux%u instead of %dx%d (MKW_RESOLUTION %s, MKW_ASPECT %s, window lines %d)",
               wh[0], wh[1], native * 16 / 9, native, res, cfg_value("MKW_ASPECT"), gfx_window_lines());
        return;
    }
}

void rt_patches_init(void) {
    const RtGame *g = rt_game();
    if (!g->npatch_sets) {
        logic_hz_set(g);   // no sets: MKW_LOGIC_HZ back to 60
        return;
    }
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
    logic_hz_set(g);
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
void graphchk_hosts(uint64_t page);     // diag/graphchk.c

void rt_patch_hosts(void) {
    const RtGame *g = rt_game();
    if (g->npatch_sets) zstd_patch_hosts(g->patch_page);
    if (g->npatch_sets) rt_register_host(g->patch_page + SHADOW_HOST, host_shadow_rb);
    if (g->npatch_sets) rt_register_host(g->patch_page + SHADOW_POOL_HOST, host_shadow_pool);
    if (g->npatch_sets) rt_register_host(g->patch_page + SEED_HOST, host_seed_tick);
    if (g->npatch_sets) rt_register_host(g->patch_page + ASPECT_HOST, host_aspect);
    if (g->npatch_sets) rt_register_host(g->patch_page + CAMERA_FOV_HOST, host_camera_fov);
    if (g->npatch_sets) rt_register_host(g->patch_page + UI_PROJ_HOST, host_ui_proj);
    if (g->npatch_sets) rt_register_host(g->patch_page + UI_ROWS_HOST, host_ui_rows);
    if (g->npatch_sets) rt_register_host(g->patch_page + UI_ROWS2_HOST, host_ui_rows2);
    if (g->npatch_sets) rt_register_host(g->patch_page + FX_VIEW_HOST, host_fx_view);
    if (g->npatch_sets) rt_register_host(g->patch_page + DISPLAY_SIZE_HOST, host_display_size);
    if (g->npatch_sets) rt_register_host(g->patch_page + DISPLAY_SET_HOST, host_display_set);
    if (g->npatch_sets) rt_register_host(g->patch_page + PANE_MATRIX_HOST, host_pane_matrix);
    if (g->npatch_sets) rt_register_host(g->patch_page + LOGIC_HZ_HOST, host_logic_step);
    if (g->npatch_sets) rt_register_host(g->patch_page + LOGIC_IV_HOST, host_logic_copy);
    if (g->npatch_sets) frame_rate_host(g->patch_page);
    if (g->npatch_sets) race_scene_host(g->patch_page);
    if (g->npatch_sets) graphchk_hosts(g->patch_page);
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
