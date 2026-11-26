// The internal render resolution (MKW_RESOLUTION, Screen tab): a RomFS overlay written at start, in place of
// tools/romfs/resolution.py, so the player picks it in the settings instead of in the setup (Adrià 06/11/2026).
//
// Default "original": the console's own. 720 is the development machine's (a Radeon 660M; the run scripts set it with
// build/romfs-res).
//
// MKW picks its 3D and screen resolutions from System/GymlArchive/general_format.Product_<version>.sarc.zs (100 in
// 1.0.0, 180 in 1.8.0: the name is looked up in the RomFS, #195) ->
// Gyml/GamePerformanceGraphicsConfigGParam/30_Extendted...bgyml (BYML v7: Resolution{Low,Middle}{For3D,Screen}
// [ForDiv,ForDiv4], 1080/900/1440; it renders at 2560x1440). The integers are patched where they are: every *For3D*
// field to LINES / 0.65 when docked (the scene renders at 65 % of For3D lines whatever the GPU load: 720 -> 1108,
// measured in races 31/10/2026) and to LINES in handheld mode (MKW_OPERATION_MODE=handheld: the scene renders at all of
// For3D's lines, 540 -> 831 drew 1476x828; measured with MKW_PASS_TIMING on 1.8.0 13/11/2026), and every *Screen*
// field to LINES. The archive is compressed again and written with its manifest.tsv to
// <game dir>/cache/resolution-<LINES>/, which romfs.c loads as an overlay (MKW_ROMFS_PATCHES, when that is unset). A
// directory already written for the same original is reused.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zstd.h>

#include "cfg.h"
#include "fs.h"
#include "gfx.h"
#include "host.h"
#include "ov.h"
#include "platform/platform.h"
#include "sha256.h"

#define ARCHIVE_DIR "System/GymlArchive/general_format.Product_"   // + the version and ".sarc.zs"
#define MEMBER "Gyml/GamePerformanceGraphicsConfigGParam/30_Extendted.game__GamePerformanceGraphicsConfigGParam.bgyml"
#define SCENE_SCALE 0.65   // docked; handheld renders all of For3D's lines

// The temporal upscalers (MKW_TEMPORAL, gfx/vk/exec/vk_temporal.c) render the 3D scene at a share of the screen's
// lines (MKW_TEMPORAL_QUALITY) and take the place of the game's own upscale from the scene to the screen; in thousandths,
// 0 when they are off or the engine cannot come up on this PC (vk_tmp_engine_usable). The same table as vk_temporal.c's.
int vk_tmp_engine_usable(void);   // gfx/vk/exec/vk_temporal.c
static int temporal_permille(void) {
    if (!strcmp(cfg_value("MKW_TEMPORAL"), "off")) return 0;
    if (!vk_tmp_engine_usable()) {
        rt_log("temporal: MKW_TEMPORAL=%s is not available on this PC: the 3D scene keeps the game's own resolution",
               cfg_value("MKW_TEMPORAL"));
        return 0;
    }
    static const struct { const char *name; int pm; } q[] = {
        {"native", 1000}, {"quality", 667}, {"balanced", 580}, {"performance", 500}, {"ultra", 333}};
    const char *v = cfg_value("MKW_TEMPORAL_QUALITY");
    for (size_t i = 0; i < sizeof q / sizeof *q; i++)
        if (!strcmp(v, q[i].name)) return q[i].pm;
    return 667;
}

// For3D's value that makes the scene LINES high in the mode the game is told it is in.
static uint32_t lines_3d(int lines) {
    return lines > 0 ? (uint32_t)ceil(lines / (ov_handheld() ? 1.0 : SCENE_SCALE)) : 0;
}

static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t u32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t u24(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16; }

// The member's bytes in a SARC (tools/shaders/extract.py sarc_members): offset into s, 0 when absent.
static size_t sarc_find(const uint8_t *s, size_t n, const char *name, size_t *len) {
    if (n < 0x20 || memcmp(s, "SARC", 4)) return 0;
    uint32_t header = u16(s + 4), data_start = u32(s + 12);
    if (header + 12 > n) return 0;
    uint32_t count = u16(s + header + 6), names = header + 12 + count * 16 + 8;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *e = s + header + 12 + i * 16;
        if (header + 12 + (i + 1) * 16 > n) return 0;
        uint32_t attr = u32(e + 4), lo = u32(e + 8), hi = u32(e + 12), off = names + (attr & 0xffffff) * 4;
        if (off >= n || data_start + hi > n) return 0;
        if (!strncmp((const char *)s + off, name, n - off)) {
            *len = hi - lo;
            return data_start + lo;
        }
    }
    return 0;
}

// Every integer field of the BYML at b whose dotted name contains For3D or Screen (the root dictionary and the
// dictionaries in it, as resolution.py byml_root_fields): set to r3d or screen (0: kept). The float LODDistanceRatio
// (2.0 in 1.0.0: the distance at which objects switch to their simpler models) to lod when lod > 0. Returns how many
// changed.
static int patch_dict(uint8_t *b, size_t n, uint32_t keys_off, uint32_t node, const char *prefix, uint32_t r3d,
                      uint32_t screen, float lod, int depth) {
    if (node + 4 > n || depth > 4) return 0;
    uint32_t count = u24(b + node + 1), nkeys = u24(b + keys_off + 1), changed = 0;
    for (uint32_t k = 0; k < count && node + 4 + 8 * (k + 1) <= n; k++) {
        uint8_t *e = b + node + 4 + 8 * k;
        uint32_t ki = u24(e), type = e[3];
        if (ki >= nkeys) continue;
        const char *key = (const char *)b + keys_off + u32(b + keys_off + 4 + 4 * ki);
        char name[160];
        snprintf(name, sizeof name, "%s%s", prefix, key);
        if (type == 0xC1) {
            char sub[160];
            snprintf(sub, sizeof sub, "%s.", name);
            changed += patch_dict(b, n, keys_off, u32(e + 4), sub, r3d, screen, lod, depth + 1);
        } else if (type == 0xD2 && lod > 0 && !strcmp(name, "LODDistanceRatio")) {   // float
            memcpy(e + 4, &lod, 4);   // little-endian, as the BYML
            changed++;
        } else if (type == 0xD1 || type == 0xD3) {   // int, uint
            uint32_t v = strstr(name, "For3D") ? r3d : strstr(name, "Screen") ? screen : 0;
            if (v) e[4] = (uint8_t)v, e[5] = (uint8_t)(v >> 8), e[6] = (uint8_t)(v >> 16), e[7] = (uint8_t)(v >> 24), changed++;
        }
    }
    return (int)changed;
}

static void hex(const uint8_t d[32], char out[65]) {
    for (int i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", d[i]);
}

// The RomFS path of the general_format archive, whatever version number it carries; NULL when there is none.
static const char *find_archive(void) {
    static char name[128];
    size_t n = strlen(ARCHIVE_DIR);
    for (uint64_t i = 0;; i++) {
        const char *p = romfs_entry(i, NULL, NULL);
        if (!p) return NULL;
        size_t len = strlen(p);
        if (!strncmp(p, ARCHIVE_DIR, n) && len > n + 8 && !strcmp(p + len - 8, ".sarc.zs") && len < sizeof name)
            return memcpy(name, p, len + 1);
    }
}

// 1.0.0 takes the fields but keeps its screen and scene buffers at 2560x1440 (s255e, 13/11/2026), so the 3D never
// goes above 1440 lines there; 1.8.0 makes them 3840x2160 at 2160. #777 picked 2160 with DLAA on 1.0.0: the log said
// "3D 2160" and DLSS got 2560x1440, and the picture looked upscaled. The log says it, as the setting cannot.
static void note_1440_cap(const char *archive, int lines) {
    if (lines > 1440 && strstr(archive, "Product_100"))
        rt_log("resolution: this is MKW 1.0.0, whose 3D stays at 1440 lines at most (its buffers are 2560x1440): "
               "%d lines need a 1.8.0 dump", lines);
}

// The overlay directory for MKW_RESOLUTION (lines, 0: the game's) and the detail distance (lod, 0: the game's), written
// when missing; NULL when it cannot be made.
static const char *write_overlay(int lines, uint32_t r3d, float lod) {
    static char dir[PLAT_PATH_MAX * 2];
    char path[PLAT_PATH_MAX * 3], man[PLAT_PATH_MAX * 3];
    const char *exe = cfg_game_dir();
    const char *archive = find_archive();
    uint64_t zn = 0;
    // over a mod's copy of the archive when one replaces it (romfs.c loads this overlay last), else the game's own
    uint8_t *z = archive ? romfs_read_top(archive, &zn) : NULL;
    char ohex[65];   // the manifest names the RomFS's own original, whatever this one was built from
    if (!z || !romfs_original_sha(archive, ohex)) {
        free(z);
        rt_log("resolution: no %s*.sarc.zs in %s: the game's own resolution", ARCHIVE_DIR, rt_config()->romfs);
        return NULL;
    }
    uint8_t ssha[32];
    char shex[65];   // what it was built from: a mod enabled or changed since writes it again
    sha256(z, zn, ssha);
    hex(ssha, shex);
    const char *mode = ov_handheld() ? "-handheld" : "";   // its own For3D, so its own directory
    char t3d[24] = "";
    if (r3d) snprintf(t3d, sizeof t3d, "-3d%u", (unsigned)r3d);   // the temporal upscalers' own 3D lines
    if (lod > 0) snprintf(dir, sizeof dir, "%s/cache/graphics-r%d%s%s-lod%g", exe, lines, t3d, mode, (double)lod);
    else snprintf(dir, sizeof dir, "%s/cache/resolution-%d%s%s", exe, lines, t3d, mode);
    snprintf(man, sizeof man, "%s/manifest.tsv", dir);
    uint64_t mn = 0;
    char *old = (char *)plat_file_read(man, &mn);
    int fresh = old && strstr(old, ohex) && strstr(old, shex);   // written before from this same source
    free(old);
    if (fresh) {   // the log still names the mode and the overlay used
        free(z);
        rt_log("resolution: %d lines (3D %u, screen %d, %s), detail distance %g: written before in %s", lines,
               (unsigned)(r3d ? r3d : lines_3d(lines)), lines, ov_handheld() ? "handheld" : "docked", (double)lod, dir);
        note_1440_cap(archive, lines);
        return dir;
    }
    unsigned long long rn = ZSTD_getFrameContentSize(z, zn);
    uint8_t *s = rn != ZSTD_CONTENTSIZE_ERROR && rn != ZSTD_CONTENTSIZE_UNKNOWN ? malloc(rn) : NULL;
    size_t got = s ? ZSTD_decompress(s, rn, z, zn) : 0;
    free(z);
    if (!s || ZSTD_isError(got)) { free(s); rt_log("resolution: %s does not decompress", archive); return NULL; }
    size_t mlen = 0, at = sarc_find(s, got, MEMBER, &mlen);
    int changed = 0;
    if (at && mlen > 16 && !memcmp(s + at, "YB", 2)) {
        uint32_t keys_off = u32(s + at + 4), root = u32(s + at + 12);
        uint32_t f3d = r3d ? r3d : lines_3d(lines);
        changed = patch_dict(s + at, mlen, keys_off, root, "", f3d, (uint32_t)(lines > 0 ? lines : 0), lod, 0);
    }
    if (!changed) { free(s); rt_log("resolution: no resolution fields in %s", MEMBER); return NULL; }
    size_t cap = ZSTD_compressBound(got);
    uint8_t *out = malloc(cap);
    size_t on = out ? ZSTD_compress(out, cap, s, got, 9) : 0;
    free(s);
    if (!out || ZSTD_isError(on)) { free(out); return NULL; }
    snprintf(path, sizeof path, "%s/%s", dir, archive);
    char *slash = plat_last_sep(path);
    if (slash) { *slash = 0; plat_file_mkdir_p(path); *slash = '/'; }
    uint8_t nsha[32];
    char nhex[65], line[512];
    sha256(out, on, nsha);
    hex(nsha, nhex);
    snprintf(line, sizeof line, "# from %s\n%s\t%zu\t%s\t%s\n", shex, archive, on, nhex, ohex);
    int ok = plat_file_write_atomic(path, out, on) && plat_file_write_atomic(man, line, strlen(line));
    free(out);
    if (!ok) { rt_log("resolution: cannot write %s", dir); return NULL; }
    rt_log("resolution: %d lines (3D %u, screen %d, %s), detail distance %g: %d fields in %s", lines,
           (unsigned)(r3d ? r3d : lines_3d(lines)), lines, ov_handheld() ? "handheld" : "docked", (double)lod, changed, dir);
    note_1440_cap(archive, lines);
    return dir;
}

static int g_lines;   // romfs_resolution_lines
static int g_scene_pm = 1000;   // romfs_resolution_scene_permille

const char *romfs_resolution_overlay(void) {
    const char *v = cfg_value("MKW_RESOLUTION");
    int lines = strcmp(v, "original") ? cfg_int_in("MKW_RESOLUTION", 0, 4320) : 0;   // the same bounds as patches.c
    if (lines < 0) lines = 0;
    // MKW_DETAIL_DISTANCE (Graphics tab): the game's LODDistanceRatio, 2 in 1.0.0; the
    // console's value writes nothing
    float lod = (float)cfg_double("MKW_DETAIL_DISTANCE");
    if (lod == 2.0f) lod = 0;
    // With a temporal upscaler the screen keeps its lines (the console's 1440 docked, 1080 handheld, when the setting
    // is original) and the 3D scene renders at the quality's share of them: the game upscales from one to the other,
    // and the upscaler takes that pass over.
    uint32_t r3d = 0;
    int pm = temporal_permille();
    if (pm) {
        // the share is of the picture's lines on screen (the window's, or the setting's), the game rendering at most
        // its screen's: a 1820-line window at quality renders 1213 lines, upscaled to the window (vk_temporal.c)
        int out = lines;
        int ww = 0, wh = 0;
        if (!out && gfx_window_shape(&ww, &wh)) out = wh;
        if (!lines) lines = ov_handheld() ? 1080 : 1440;
        if (out < lines) out = lines;
        r3d = (uint32_t)((out * pm + 500) / 1000);
        if (r3d > (uint32_t)lines) r3d = (uint32_t)lines;
    }
    if (!lines && lod <= 0) return NULL;
    const char *dir = write_overlay(lines, r3d, lod);
    if (dir && pm) g_scene_pm = pm;
    if (dir && lines) g_lines = lines;
    return dir;
}

void romfs_resolution_external(void) { g_lines = -1; }
int romfs_resolution_lines(void) { return g_lines; }
// The 3D scene's share of the screen's lines this run, in thousandths (1000: the same, the game's own SMAA writes the
// screen; less: the game's upscale dispatch follows), as the overlay written at start says (vk_temporal.c).
int romfs_resolution_scene_permille(void) { return g_scene_pm; }
