// spirv/shaders.pack (tools/shaders/pack.py): every program's .spv, .res.json and .cs.spv in one file, which the boot
// prewarm reads instead of 23 124 loose files. With the file cache cold (a start after a reboot) the Windows PC took
// 24.0 s for the loose files and its loading threads did not help; the same bytes as one file read cold in 0.68 s
// (prep-pack, 18/11/2026). The pack is used only while its folder was not written after it; else, or for a program it
// lacks, gfx_shader.c reads the loose file as before.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx_shader_internal.h"
#include "host.h"
#include "platform/platform.h"

#define PACK_ENTRY 72   // key[32], then offset u64 and length u32 of each of the three files

static struct {
    PlatFile *f;
    uint32_t n;
    uint8_t *index;   // n entries sorted by key
} PK;

void gfx_shader_pack_open(const char *dir) {
    char p[PATH_CAP + 32];
    snprintf(p, sizeof p, "%s/shaders.pack", dir);
    uint64_t pack_time = plat_path_write_time(p), dir_time = plat_path_write_time(dir);
    if (!pack_time) return;
    if (dir_time > pack_time) {
        rt_log("gfx_shader: %s is older than its folder: the loose files are read", p);
        return;
    }
    PlatFile *f = plat_file_open(p);
    uint8_t head[16];
    uint32_t version = 0, n = 0;
    if (f && plat_file_pread(f, head, sizeof head, 0) == sizeof head && !memcmp(head, "MKWSPK1", 8))
        memcpy(&version, head + 8, 4), memcpy(&n, head + 12, 4);
    uint8_t *index = version == 1 && n && n < (1u << 24) ? malloc((size_t)n * PACK_ENTRY) : NULL;
    if (!index || plat_file_pread(f, index, n * PACK_ENTRY, sizeof head) != (int64_t)n * PACK_ENTRY) {
        rt_log("gfx_shader: %s unreadable or of another format (version %u): the loose files are read", p, version);
        free(index);
        plat_file_close(f);
        return;
    }
    PK.f = f, PK.n = n, PK.index = index;
    rt_log("gfx_shader: %u programs in %s", n, p);
}

static int entry_cmp(const void *key, const void *entry) { return memcmp(key, entry, 32); }

// File kind (0 .spv, 1 .res.json, 2 .cs.spv) of the program, NUL-terminated as plat_file_read's. 0 when the pack is not
// in use or has no such program (the caller reads the loose file); 1 with *out NULL when the program has no such file.
int gfx_shader_pack_read(const uint8_t key[32], int kind, uint8_t **out, uint64_t *size) {
    *out = NULL;
    const uint8_t *e = PK.index ? bsearch(key, PK.index, PK.n, PACK_ENTRY, entry_cmp) : NULL;
    if (!e) return 0;
    uint64_t off;
    uint32_t len;
    memcpy(&off, e + 32 + 12 * kind, 8), memcpy(&len, e + 40 + 12 * kind, 4);
    if (!len) return 1;
    uint8_t *b = malloc((size_t)len + 1);
    if (!b || plat_file_pread(PK.f, b, len, off) != len) {
        rt_log("gfx_shader: shaders.pack: read of %u bytes at %llu failed; the loose file is read", len,
               (unsigned long long)off);
        free(b);
        return 0;
    }
    b[len] = 0;
    *out = b, *size = len;
    return 1;
}

size_t gfx_shader_pack_keys(uint8_t (**keys)[32]) {
    *keys = PK.index ? malloc((size_t)PK.n * 32) : NULL;
    if (!*keys) return 0;
    for (uint32_t i = 0; i < PK.n; i++) memcpy((*keys)[i], PK.index + (size_t)i * PACK_ENTRY, 32);
    return PK.n;
}
