// The rows of version.h. 1.0.0's are the addresses the runtime had before there was a second version; 1.8.0's were
// found in its executable (11/11/2026) by the same features the 1.0.0 comments describe:
// - nvn_ret: main+0xdd07c8 is the first of the two nvnBootstrapLoader calls in the graphics bring-up (main+0xdd0720,
//   after nv::InitializeGraphics and nn::vi::GetDisplayVsyncEvent, as 1.0.0's main+0xcf39b4).
// - frame_update: main+0x69cbb0 is the only function of main using the frame object's +0x210 (fixed interval), +0x214,
//   +0x218, +0x128 and +0xf0 (game.c); vtable column main+0x367a9e0 slot 28, as 1.0.0's; the game clock is the u32
//   it adds [+0x214] to (adrp/ldr/add/str at main+0x69cd30).
// - zstd: recomp/patches-mkw-180.txt says how each function was found.
// - sdk: tools/gpu/nvn2_procs.py locate(), checked by disassembly.
// Not found yet for 1.8.0 (0): the shadow, seed and aspect sets' functions and the menu focus probe.
#include <string.h>

#include "version.h"
#include "host.h"

static const RtVersion versions[] = {
    {
        .number = 100,
        .display = "1.0.0",
        .nvn_ret = 0xcf3a60,
        .frame_update = 0x3a9e28,
        .frame_slot = 0x329fd50,
        .game_clock = 0x33e6ae8,
        .zstd_multiframe = 0x407e00,
        .zstd_stream = 0x6532b4,
        .rb_create = 0x233358,
        .rb_alloc_init = 0x8eb6b0,
        .rb_pool_new = 0x67aa40,
        .rb_pool_init = 0x8eb9dc,
        .random_ctor_lr = 0x922088,
        .random_ctor2_lr = 0x15585a8,
        .ui_text_lr = 0x169aeb4,
        .ui_focus_fn = 0xce8564,
        .sdk_name_pool = 0xb74c26,
        .sdk_name_offsets = 0xb79b90,
        .sdk_pointers = 0xd95168,
        .sdk_last_index = 0x2b9,
        .inits = {{"main", 2866}, {"subsdk0", 4}, {"sdk", 86}},
    },
    {
        .number = 180,
        .display = "1.8.0",
        .nvn_ret = 0xdd07cc,
        .frame_update = 0x69cbb0,
        .frame_slot = 0x367aac0,
        .game_clock = 0x37c8438,
        .zstd_multiframe = 0xe2280,
        .zstd_stream = 0xdeca0,
        .camera_service = 0x378ecb8,   // RELATIVE at main+0x36f4148; 9 functions load it
        .sdk_name_pool = 0xc26452,
        .sdk_name_offsets = 0xc2b870,
        .sdk_pointers = 0xe45aa8,
        .sdk_last_index = 0x2da,
        .inits = {{"main", 2960}, {"subsdk0", 0}, {"subsdk1", 4}, {"sdk", 88}},
    },
};

const RtVersion *rt_version(void) {
    static const RtVersion *cur;
    if (cur) return cur;
    int v = rt_game()->version;
    for (size_t i = 0; i < sizeof versions / sizeof *versions; i++)
        if (versions[i].number == v) return cur = &versions[i];
    rt_fatal("version: the game code is for game version %d, which this runtime has no addresses for", v);
}

uint64_t rt_main_addr(uint64_t off) {
    if (!off) return 0;
    for (int i = 0; i < ldr_count(); i++)
        if (!strcmp(ldr_module(i)->name, "main")) return ldr_module(i)->base + off;
    rt_fatal("version: main is not loaded");
}
