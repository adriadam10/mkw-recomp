// The fixed addresses of each game version the port knows: one row per executable, chosen by RtGame.version (the
// number recomp/versions.py gives the dump the game code was generated from). Every guest address the runtime reads
// or calls by number lives here, so a new version is one more row and the code that uses them stays the same.
// A field at 0 is not known for that version: the feature that needs it is off, with a log line, never a guess.
#pragma once
#include <stdint.h>

#include "gamelib.h"

typedef struct { const char *name; uint64_t entries; } RtInitCount;

typedef struct {
    int number;                // RtGame.version: 100, 180
    const char *display;       // as the game shows it (nn::oe::GetDisplayVersion): "1.0.0"
    // main offsets
    uint64_t nvn_ret;          // return address of the first nvnBootstrapLoader call (boot.c checkpoint)
    uint64_t frame_update;     // the frame object's per-frame virtual (game.c, MKW_FPS)
    uint64_t frame_slot;       // its vtable slot (main's data segment, writable)
    uint64_t game_clock;       // u32, += the present interval every frame
    uint64_t zstd_multiframe;  // ZSTD_decompressMultiFrame (ov_zstd.c, patch set zstd)
    uint64_t zstd_stream;      // the loads' stream function (stream, src, size, ddict)
    uint64_t rb_create;        // gsys RenderBuffer creation (patches.c, set shadow)
    uint64_t rb_alloc_init;    // render-buffer allocator setup
    uint64_t rb_pool_new;      // operator new(size, heap, alignment), as the allocator calls it
    uint64_t rb_pool_init;     // one pool of that allocator
    uint64_t random_ctor_lr;   // sead::Random::Random, after its GetSystemTick call (set seed)
    uint64_t random_ctor2_lr;  // another Random constructor, the same
    uint64_t ui_text_lr;       // return address of the 2D text setup's projection getter call (set aspect)
    uint64_t ui_focus_fn;      // a menu button taking the cursor (seqwatch.c, MKW_UI_FOCUS)
    uint64_t camera_service;   // the global holding the camera service object (patches.c, set camera; 1.8.0)
    // sdk offsets: the NVN2 driver's proc table (tools/gpu/nvn2_procs.py TABLES, the same rows)
    uint64_t sdk_name_pool, sdk_name_offsets, sdk_pointers;
    int sdk_last_index;
    // DT_INIT_ARRAYSZ / 8 of each module, in load order (boot.c): a different count is a different build
    RtInitCount inits[RT_MAX_MODULES];
} RtVersion;

const RtVersion *rt_version(void);                 // the row of the game code in use; fatal for a version not listed
uint64_t rt_main_addr(uint64_t main_offset);       // main's base + offset, 0 for an offset of 0
