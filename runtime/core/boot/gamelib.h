// The game code's tables: module layout, lifted function tables, the ExeFS hashes the code was generated from and the
// code patch sets. tools/build/build.py writes them (build/obj/rt/modules.c) from the generator's layout.json and
// patches.h. The development build links them into build/mkw (rt_game_builtin); the player's build compiles them into
// the game code library, mkwgame.so / mkwgame.dll (SavageWindRecomp runtime/gamelib.h, MIT).
#pragma once
#include <stdint.h>

#include "cpu.h"

#define RT_GAME_ABI 2   // 2: image_patches
#ifndef RT_GAME_ID
#define RT_GAME_ID "dev"   // -D from build.py: a hash of the headers the game code is compiled against
#endif
#define RT_MAX_MODULES 8
#define RT_MAX_PATCH_SETS 16

typedef struct {
    const char *name;        // its ExeFS file: main, subsdk0, sdk
    uint64_t base, size;     // guest image (recomp/layout.py)
    const uint64_t *addrs;   // <mod>_fn_addrs: guest function starts, strictly ascending
    const GuestFn *fns;      // <mod>_fn_ptrs
    const uint64_t *count;   // <mod>_fn_count
} RtGameModule;

typedef struct { const char *file, *sha256; } RtFileHash;

typedef struct {
    const char *name, *env;  // the set (recomp/patches-mkw.txt) and its MKW_ switch
    int on;                  // generated default, for a set without a row in cfg.c
} RtPatchSet;

// A mod's change to the game's own code (tools/mod/ips.py, from its .ips): the generator lifted the module with these
// bytes in place, and the loader writes them into the module's .text too, so what the code reads of itself agrees.
typedef struct {
    const char *module;
    uint64_t off;            // module offset, inside .text
    const uint8_t *bytes;
    uint32_t n;
} RtImagePatch;

typedef struct {
    uint32_t abi;            // RT_GAME_ABI
    const char *id;          // RT_GAME_ID: a library built against other headers is built again, not loaded
    const RtGameModule *modules;   // layout order, ascending bases
    int nmodules;
    const RtFileHash *exefs_sha256;
    int nexefs;
    const RtPatchSet *patch_sets;
    int npatch_sets;
    uint64_t patch_page;     // 0 without patch sets
    int version;             // the game build: 100 (MKW 1.0.0)
    // The data the generated code reads on every call lives beside it, so a library reaches it without an import.
    volatile int *fnprof;    // g_fnprof (cpu.h FN_ENTER)
    uint8_t *patch_on;       // rt_patch_on[RT_MAX_PATCH_SETS]
    const RtImagePatch *image_patches;   // the enabled code mods' (recomp/gen.py --image), applied at load
    int nimage_patches;
} RtGame;

const RtGame *rt_game(void);           // the tables in use; fatal when there are none
const RtGame *rt_game_builtin(void);   // the linked ones (modules.c), NULL in the player's build (gamelib.c loads them)
int rt_game_present(void);             // linked tables, or a library in the selected game folder built for this runtime
void rt_fnprof_set(int on);            // the function-entry hook of the generated code on or off

#if defined(_WIN32)
#define RT_GAME_LIB "mkwgame.dll"
#define RT_GAME_EXPORT __declspec(dllexport)
#else
#define RT_GAME_LIB "mkwgame.so"
#define RT_GAME_EXPORT __attribute__((visibility("default")))
#endif
#define RT_GAME_ENTRY "mkwgame_tables"   // the library's one export: const RtGame *mkwgame_tables(void)
