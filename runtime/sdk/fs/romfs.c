// Adapted from MacNativeGames games/totk/runtime/romfs.c @ b75be8cf (MIT, Copyright (c) 2026 MacNativeGames
// contributors).
// Read-only game RomFS: the dump's loose romfs/ directory (rt_config()->romfs), walked at fs_init into an in-memory
// index of paths and sizes; each file is opened by its host path when the game opens it. Or, when rt_config()->romfs
// is a packed dump (pack.h: .nup or .nx), the index comes from its table of contents: a .nup raw file is read in place at
// its offset in the package (one shared handle), a .nup zstd file is decompressed whole into memory when the game opens
// it (one frame per file: no way to start in the middle), a .nx file is read at any offset through its blocks. Reads are
// positional (platform.h plat_file_pread), so threads read in parallel without a file-pointer lock. totk-pc-recomp's packed
// romfs.bin (its index, SHA-256 proof and mapping) has no MKW counterpart.
//
// Overlays: files listed in a directory's manifest.tsv are served in place of the originals. Each line names a RomFS
// path, the overlay's size and SHA-256 and the original's SHA-256 ("-" adds a file the RomFS lacks; it is numbered
// after the RomFS's own files, file_count + k in path order). The directories, in this order: MKW_ROMFS_PATCHES and
// MKW_ROMFS_MODS; unset or "0" = none; then <game dir>/lang/<code>/ for a language the game does not ship
// (ov_text_language: its texts rebuilt by the setup, tools/i18n/game_texts.py). A missing path, a size or hash mismatch
// or a changed original stops the start.
// An overlay file is opened when the game opens it; its identity (volume, file id, size, last write) is kept from the
// start check, and an open that finds another file there stops the game.
#include <stdio.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "fs.h"
#include "host.h"
#include "pack.h"
#include "ov.h"
#include "romfs_path.h"
#include "platform/platform.h"
#include "sha256.h"

#define NH 65536                     // RomFS handle slots
#define READ_CHUNK (64u << 20)       // largest single read
#define HASH_CHUNK (8u << 20)
#define ADDED UINT64_MAX             // an added overlay file's index until overlay_init numbers it

typedef struct { uint64_t offset, size; uint32_t name, len; } Entry;
_Static_assert(sizeof(Entry) == 24, "index entry layout");

typedef struct { uint32_t volume, pad; uint64_t file_id, size, write_time, create_time; } Identity;   // no padding
typedef struct { uint64_t index, size; char *path, *name; Identity id; unsigned ord; } Overlay;

static uint8_t *index_bytes;
static size_t index_size;
static uint64_t file_count, strings_offset;
static char loose_root[2048];   // the loose folder or the package (load_loose_index, load_package_index)
static char mount_name[32];
static PlatMutex lock = PLAT_MUTEX_INIT;
static uint64_t handles[NH];         // file index + 1; 0 is free
static uint32_t handle_ovl[NH];      // overlay slot + 1 of an open handle, 0 = original
static PlatFile *handle_file[NH];    // the handle's open file: an overlay's, a loose file's or the shared package
static uint64_t handle_base[NH];     // where the file starts in handle_file (a package's raw file), else 0
static uint8_t *handle_mem[NH];      // a .nup zstd file, decompressed at open (handle_file NULL)
static Pack *package;                // the RomFS is a packed dump; its rows are package_rows, in index order
static const PackEntry *package_rows;
static Overlay *overlays;            // sorted by RomFS index after overlay_init
static size_t noverlay, overlay_cap;
static unsigned load_seq;            // overlay_load call count: later dirs win shared files
static char **added;                 // RomFS paths of the added files, sorted; added[k] is index file_count + k
static size_t nadded;

static void need(int c, const char *m) {
    if (!c) rt_fatal("RomFS: %s", m);
}

static Entry entry(uint64_t i) {
    Entry e;
    memcpy(&e, index_bytes + 24 + i * 24, 24);
    return e;
}
static const char *entry_name(Entry e) { return (const char *)index_bytes + strings_offset + e.name; }

static double now_s(void) { return (double)plat_now_ns() / 1e9; }

// ---- positional reads
// Bytes read at off (0 at or past the end of the file), -1 on an I/O error.
static int64_t read_at(PlatFile *f, void *buf, uint32_t n, uint64_t off) {
    mem_thaw((uintptr_t)buf, n);   // the kernel's write would fail on a frozen page instead of faulting
    int64_t got = plat_file_pread(f, buf, n, off);
    if (got == -2)   // a read-only page in the buffer: a frozen region missed its thaw
        rt_log("RomFS: read of %u bytes into %p refused: the buffer is not writable", n, buf);
    return got < 0 ? -1 : got;
}

static PlatFile *open_read(const char *path) { return plat_file_open(path); }

static Identity identity_of(PlatFile *f) {
    PlatFileId i;
    need(plat_file_id(f, &i), "file information");
    return (Identity){(uint32_t)i.volume, 0, i.file_id, i.size, i.write_time, i.create_time};
}

// ---- SHA-256 over a file range, streaming
static void hex32(const uint8_t d[32], char out[65]) {
    for (int i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", d[i]);
}

static void sha_range(PlatFile *f, uint64_t offset, uint64_t size, char hex[65], int progress) {
    Sha256 h;
    sha256_init(&h);
    uint8_t *buf = malloc(HASH_CHUNK);
    need(buf != NULL, "hash buffer");
    double t0 = now_s();
    for (uint64_t done = 0; done < size;) {
        uint32_t want = size - done > HASH_CHUNK ? HASH_CHUNK : (uint32_t)(size - done);
        int64_t n = read_at(f, buf, want, offset + done);
        need(n > 0, "read of hash input");
        sha256_update(&h, buf, (size_t)n);
        done += (uint64_t)n;
        if (progress && (done % (2ull << 30) < (uint64_t)n || done == size))
            rt_log("RomFS: hashed %.1f of %.1f GiB (%.0f MiB/s)", done / 1073741824.0, size / 1073741824.0,
                   done / 1048576.0 / (now_s() - t0 + 1e-9));
    }
    uint8_t d[32];
    sha256_final(&h, d);
    free(buf);
    hex32(d, hex);
}

static PlatFile *row_open(uint64_t i, uint64_t *base, uint8_t **mem);

// SHA-256 of RomFS row i's original bytes.
static void sha_row(uint64_t i, char hex[65]) {
    uint64_t base;
    uint8_t *mem = NULL;
    PlatFile *f = row_open(i, &base, &mem);
    if (!f && !mem && package) mem = pack_read_whole(package, &package_rows[i]);   // a .nx row
    if (!f && !mem) rt_fatal("RomFS: %s unreadable", entry_name(entry(i)));
    if (mem) {
        uint8_t d[32];
        sha256(mem, entry(i).size, d);
        hex32(d, hex);
        free(mem);
        return;
    }
    sha_range(f, base, entry(i).size, hex, 0);
    if (!package) plat_file_close(f);
}

// ---- index lookup
static uint64_t lower(const char *name) {
    uint64_t lo = 0, hi = file_count;
    while (lo < hi) {
        uint64_t m = lo + (hi - lo) / 2;
        if (strcmp(entry_name(entry(m)), name) < 0) lo = m + 1;
        else hi = m;
    }
    return lo;
}

static size_t added_lower(const char *p) {
    size_t lo = 0, hi = nadded;
    while (lo < hi) {
        size_t m = lo + (hi - lo) / 2;
        if (strcmp(added[m], p) < 0) lo = m + 1;
        else hi = m;
    }
    return lo;
}

// The index of RomFS path p (an added file: file_count + k), or UINT64_MAX.
static uint64_t file_index(const char *p) {
    uint64_t i = lower(p);
    if (i < file_count && !strcmp(p, entry_name(entry(i)))) return i;
    size_t k = added_lower(p);
    return k < nadded && !strcmp(added[k], p) ? file_count + k : UINT64_MAX;
}

// 1 when a RomFS file or an added one lies under folder (a path ending in '/').
static int folder_exists(const char *folder) {
    size_t n = strlen(folder);
    uint64_t i = lower(folder);
    if (i < file_count && !strncmp(entry_name(entry(i)), folder, n)) return 1;
    size_t k = added_lower(folder);
    return k < nadded && !strncmp(added[k], folder, n);
}

// The overlay slot + 1 that replaces RomFS file i, or 0.
static uint32_t overlay_slot(uint64_t i) {
    size_t lo = 0, hi = noverlay;
    while (lo < hi) {
        size_t m = lo + (hi - lo) / 2;
        if (overlays[m].index < i) lo = m + 1;
        else hi = m;
    }
    return lo < noverlay && overlays[lo].index == i ? (uint32_t)(lo + 1) : 0;
}

// ---- overlays
static void overlay_load(const char *dir) {
    char path[2100];
    need(snprintf(path, sizeof path, "%s/manifest.tsv", dir) < (int)sizeof path, "overlay path");
    FILE *m = fopen(path, "rb");
    if (!m) {
        rt_log("RomFS overlay: none (%s)", path);
        return;
    }
    char line[8192];
    size_t first = noverlay;
    uint64_t hashed = 0;
    double t0 = now_s();
    while (fgets(line, sizeof line, m)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        char name[4096], sha[65], orig[65], hex[65];
        unsigned long long size = 0;
        need(sscanf(line, "%4095[^\t]\t%llu\t%64[0-9a-f]\t%64[0-9a-f-]", name, &size, sha, orig) == 4 &&
             strlen(sha) == 64 && (strlen(orig) == 64 || !strcmp(orig, "-")), "overlay manifest line");
        need(romfs_path_valid_relative(name), "overlay path");
        uint64_t i = lower(name);
        int known = i < file_count && !strcmp(entry_name(entry(i)), name), add = !strcmp(orig, "-");
        if (!known && !add)
            rt_fatal("RomFS overlay: %s/manifest.tsv names %s, which the RomFS lacks; a new file has \"-\" as its "
                     "original's SHA-256", dir, name);
        if (known && add)
            rt_fatal("RomFS overlay: %s/manifest.tsv adds %s, which the RomFS already has; give its original's SHA-256",
                     dir, name);
        Entry e = {0};
        if (known) {
            e = entry(i);
            sha_row(i, hex);
            if (strcmp(hex, orig)) rt_fatal("RomFS overlay: the original of %s has SHA-256 %s, %s/manifest.tsv says %s",
                                            name, hex, dir, orig);
        } else {
            i = ADDED;
        }
        char file[8200];
        need(snprintf(file, sizeof file, "%s/%s", dir, name) < (int)sizeof file, "overlay file path");
        PlatFile *f = open_read(file);
        if (!f) rt_fatal("RomFS overlay: %s missing", file);
        Identity id = identity_of(f);
        if (id.size != size) rt_fatal("RomFS overlay: %s has %llu bytes, its manifest says %llu", file,
                                      (unsigned long long)id.size, size);
        sha_range(f, 0, size, hex, 0);
        plat_file_close(f);
        if (strcmp(hex, sha)) rt_fatal("RomFS overlay: %s has SHA-256 %s, its manifest says %s", file, hex, sha);
        hashed += e.size + size;
        if (noverlay == overlay_cap) {
            overlay_cap = overlay_cap ? overlay_cap * 2 : 16;
            overlays = realloc(overlays, overlay_cap * sizeof *overlays);
            need(overlays != NULL, "overlay table");
        }
        char *copy = strdup(file), *rel = strdup(name);
        need(copy && rel, "overlay path copy");
        overlays[noverlay++] = (Overlay){i, size, copy, rel, id, load_seq};
        if (noverlay - first <= 16)
            rt_log("RomFS overlay: %s (%llu bytes) %s", name, size, add ? "is added" : "replaces the original");
    }
    fclose(m);
    if (noverlay - first > 16) rt_log("RomFS overlay: %zu files from %s (%zu not listed)", noverlay - first, dir,
                                      noverlay - first - 16);
    if (noverlay > first) rt_log("RomFS overlay: %s checked, %zu files, %.1f MiB hashed in %.2f s", dir,
                                 noverlay - first, hashed / 1048576.0, now_s() - t0);
}

static int overlay_order(const void *a, const void *b) {
    uint64_t x = ((const Overlay *)a)->index, y = ((const Overlay *)b)->index;
    // added files all share ADDED until added_init numbers them: never equal, keep load order among them
    if (x == ADDED || y == ADDED) return (x > y) - (x < y);
    if (x != y) return (x > y) - (x < y);
    unsigned ox = ((const Overlay *)a)->ord, oy = ((const Overlay *)b)->ord;
    return (ox > oy) - (ox < oy);
}

static int added_order(const void *a, const void *b) {
    int c = strcmp((*(Overlay *const *)a)->name, (*(Overlay *const *)b)->name);
    if (c) return c;
    unsigned x = (*(Overlay *const *)a)->ord, y = (*(Overlay *const *)b)->ord;
    return (x > y) - (x < y);
}

// Number the added files in path order after the RomFS's own (file_count + k); two overlays adding one path stop.
static void added_init(void) {
    Overlay **list = NULL;
    for (size_t k = 0; k < noverlay; k++)
        if (overlays[k].index == ADDED) {
            list = realloc(list, (nadded + 1) * sizeof *list);
            need(list != NULL, "added files");
            list[nadded++] = &overlays[k];
        }
    if (!nadded) return;
    qsort(list, nadded, sizeof *list, added_order);
    // two mods adding one path: the later mod wins, like replacements (the MODS order is the precedence)
    size_t kept = 0;
    for (size_t k = 0; k < nadded; k++) {
        if (kept && !strcmp(list[k]->name, list[kept - 1]->name)) {
            rt_log("RomFS overlay: %s loses added %s to %s", list[kept - 1]->path, list[k]->name, list[k]->path);
            free(list[kept - 1]->path);
            free(list[kept - 1]->name);
            list[kept - 1] = list[k];
        } else {
            list[kept++] = list[k];
        }
    }
    nadded = kept;
    added = malloc(nadded * sizeof *added);
    need(added != NULL, "added files");
    for (size_t k = 0; k < nadded; k++) {
        list[k]->index = file_count + k;
        added[k] = list[k]->name;
    }
    free(list);
    rt_log("RomFS overlay: %zu files added to the RomFS", nadded);
}

// <game dir>/lang/<code> for a rebuilt game language, NULL for one the game ships.
static const char *lang_overlay(void) {
    static char dir[PLAT_PATH_MAX * 2];
    const char *code = ov_text_language();
    if (!code) return NULL;
    snprintf(dir, sizeof dir, "%s/lang/%s", cfg_game_dir(), code);
    return dir;
}

// The bytes a read of rel gets from the overlays loaded so far (the last one that replaces it), else the RomFS's own.
uint8_t *romfs_read_top(const char *rel, uint64_t *size) {
    const Overlay *top = NULL;
    for (size_t k = 0; k < noverlay; k++)
        if (overlays[k].index != ADDED && !strcmp(overlays[k].name, rel) && (!top || overlays[k].ord >= top->ord))
            top = &overlays[k];
    if (!top) return romfs_read_whole(rel, size);
    return (uint8_t *)plat_file_read(top->path, size);
}

int romfs_original_sha(const char *rel, char hex[65]) {
    uint64_t i = lower(rel);
    if (i >= file_count || strcmp(entry_name(entry(i)), rel)) return 0;
    sha_row(i, hex);
    return 1;
}

static void overlay_init(void) {
    // MKW_ROMFS_PATCHES (the scripts' build/romfs-res) goes first, as before the mods; else the resolution setting
    // writes its own last, over whatever the mods left of its archive (a mod that ships general_format, as Classic
    // GP does, replaced the resolution's copy and the setting did nothing)
    const char *patches = cfg_get("MKW_ROMFS_PATCHES");
    if (patches && *patches && strcmp(patches, "0")) {
        romfs_resolution_external();
        load_seq++;
        overlay_load(patches);
    }
    const char *legacy = cfg_get("MKW_ROMFS_MODS");
    if (legacy && *legacy && strcmp(legacy, "0")) {
        load_seq++;
        overlay_load(legacy);
    }
    // the Mods page's enabled mods, in MODS order: later mods win shared files
    const char *game = cfg_game_dir();
    char mods[4096], seen[64][64];
    size_t nseen = 0;
    const char *list = cfg_get("MKW_MODS");
    // a bug report's log says which mods were on (tools/github/classify.py mods_enabled asks for a run without them)
    if (list && *list) rt_log("mods: enabled %s", list);
    if (list && *list) {
        snprintf(mods, sizeof mods, "%s", list);
        for (char *t = strtok(mods, ","); t; t = strtok(NULL, ",")) {
            while (*t == ' ') t++;
            char *e = t + strlen(t);
            while (e > t && (e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
            if (!*t || strlen(t) >= sizeof seen[0] || nseen >= 64) continue;
            size_t k = 0;
            while (k < nseen && strcmp(seen[k], t)) k++;
            if (k < nseen) continue;
            snprintf(seen[nseen++], sizeof seen[0], "%s", t);
            char dir[PLAT_PATH_MAX * 2 + 16];
            snprintf(dir, sizeof dir, "%s/mods/%s/romfs", game, t);
            load_seq++;
            overlay_load(dir);
        }
    }
    // the Mods page's merge of the enabled mods' shared archives (tools/mod/merge.py); missing when
    // nothing is shared, and then this is a no-op log line like a missing legacy dir. Gated on MODS so a
    // stale .merged from an earlier enablement never applies after the last mod is toggled off.
    const char *mods_on = cfg_get("MKW_MODS");
    if (mods_on && *mods_on) {
        char merged[PLAT_PATH_MAX * 2 + 16];
        snprintf(merged, sizeof merged, "%s/mods/.merged/romfs", game);
        load_seq++;
        overlay_load(merged);
    }
    // the language after the mods: a mod's copy of a text archive would leave a translated menu half in Spanish
    const char *lang = lang_overlay();
    if (lang && *lang) {
        load_seq++;
        overlay_load(lang);
    }
    const char *res = patches && *patches ? NULL : romfs_resolution_overlay();
    if (res) {
        load_seq++;
        overlay_load(res);
    }
    added_init();
    if (noverlay) qsort(overlays, noverlay, sizeof *overlays, overlay_order);
    // two dirs replacing one file: the later dir wins (MODS order); the loser's strings are freed
    size_t kept = 0;
    for (size_t k = 0; k < noverlay; k++) {
        if (kept && overlays[k].index != ADDED && overlays[k].index == overlays[kept - 1].index) {
            rt_log("RomFS overlay: %s loses %s to %s", overlays[kept - 1].path, overlays[k].name, overlays[k].path);
            free(overlays[kept - 1].path);
            free(overlays[kept - 1].name);
            overlays[kept - 1] = overlays[k];
        } else {
            overlays[kept++] = overlays[k];
        }
    }
    noverlay = kept;
}

// ---- start
#ifndef _WIN32
#include <ftw.h>
#include <sys/stat.h>
#endif

static size_t loose_root_len;
typedef struct { char *name; uint64_t size; } LooseFile;
static LooseFile *loose_files;
static size_t loose_n, loose_cap;

static void loose_add(const char *rel, uint64_t size) {
    if (loose_n == loose_cap) {
        loose_cap = loose_cap ? loose_cap * 2 : 65536;
        loose_files = realloc(loose_files, loose_cap * sizeof *loose_files);
        need(loose_files != NULL, "loose index memory");
    }
    loose_files[loose_n].name = strdup(rel);
    loose_files[loose_n].size = size;
    need(loose_files[loose_n].name != NULL, "loose index memory");
    loose_n++;
}

#ifdef _WIN32
// mingw-w64's nftw stats every file (19-41 s for the 205 780 files); a directory listing already holds each size
// (plat_fs_list: FindFirstFileExW with large fetches). Links are not entered, as with FTW_PHYS.
typedef struct { char path[4096]; size_t len, rel; } LooseWalk;   // rel: where the RomFS path starts in path

static int loose_entry(const char *name, int kind, uint64_t size, void *ctx) {
    LooseWalk *w = ctx;
    size_t n = strlen(name), len = w->len;
    if (len < w->rel && *name == '.') return 1;   // the dump tool's .listing files are not game files
    need(len + 1 + n < sizeof w->path, "RomFS path");
    w->path[len] = '/';
    memcpy(w->path + len + 1, name, n + 1);
    if (kind == PLAT_FS_FILE) loose_add(w->path + w->rel, size);
    else if (kind == PLAT_FS_DIR) {
        w->len = len + 1 + n;
        if (plat_fs_list(w->path, loose_entry, w) != PLAT_FS_OK) rt_fatal("RomFS: cannot list %s", w->path);
    }
    w->len = len;
    w->path[len] = 0;
    return 1;
}

static int loose_walk(void) {
    static LooseWalk w;
    w.len = loose_root_len;
    while (w.len && (loose_root[w.len - 1] == '/' || loose_root[w.len - 1] == '\\')) w.len--;
    memcpy(w.path, loose_root, w.len);
    w.path[w.len] = 0;
    w.rel = w.len + 1;
    return plat_fs_list(w.path, loose_entry, &w);
}
#else
static int loose_visit(const char *path, const struct stat *st, int type, struct FTW *ftw) {
    (void)ftw;
    if (type != FTW_F) return 0;
    const char *rel = path + loose_root_len;
    while (*rel == '/') rel++;
    if (*rel == '.') return 0;   // the dump tool's .listing files are not game files
    loose_add(rel, (uint64_t)st->st_size);
    return 0;
}

static int loose_walk(void) { return nftw(loose_root, loose_visit, 64, FTW_PHYS); }
#endif

static int loose_order(const void *a, const void *b) {
    return strcmp(((const LooseFile *)a)->name, ((const LooseFile *)b)->name);
}

// The index (MKW_RFS1 layout: header, sorted entries, names), built from a walk of the loose directory.
static void load_loose_index(const char *dir) {
    need(snprintf(loose_root, sizeof loose_root, "%s", dir) < (int)sizeof loose_root, "RomFS path");
#ifndef _WIN32
    // FTW_PHYS does not enter a symbolic link given as the root: a romfs/ link next to the executable to the copy
    char real[PATH_MAX];
    if (realpath(dir, real) && strlen(real) < sizeof loose_root) strcpy(loose_root, real);
#endif
    loose_root_len = strlen(loose_root);
    if (loose_walk() != 0 || !loose_n)
        rt_fatal("RomFS directory %s missing or empty (set it with --romfs)", loose_root);
    qsort(loose_files, loose_n, sizeof *loose_files, loose_order);
    size_t strings = 0;
    for (size_t i = 0; i < loose_n; i++) strings += strlen(loose_files[i].name) + 1;
    file_count = loose_n;
    strings_offset = 24 + file_count * 24;
    index_size = (size_t)strings_offset + strings;
    index_bytes = malloc(index_size);
    need(index_bytes != NULL, "loose index memory");
    memcpy(index_bytes, "MKW_RFS1", 8);
    memcpy(index_bytes + 8, &file_count, 8);
    memcpy(index_bytes + 16, &strings_offset, 8);
    uint32_t at = 0;
    for (size_t i = 0; i < loose_n; i++) {
        uint32_t len = (uint32_t)strlen(loose_files[i].name) + 1;
        Entry e = {0, loose_files[i].size, at, len};
        memcpy(index_bytes + 24 + i * 24, &e, sizeof e);
        memcpy(index_bytes + strings_offset + at, loose_files[i].name, len);
        at += len;
        free(loose_files[i].name);
    }
    free(loose_files);
    loose_files = NULL;
}

// The index from a packed dump's RomFS rows (sorted by name as the loose walk's).
static void load_package_index(const char *path) {
    need(snprintf(loose_root, sizeof loose_root, "%s", path) < (int)sizeof loose_root, "RomFS path");
    package = pack_open(path);
    if (!package) rt_fatal("RomFS package %s unreadable (see the log)", path);
    file_count = pack_entries(package, PACK_ROMFS, &package_rows);
    if (!file_count) rt_fatal("RomFS package %s has no RomFS files", path);
    size_t strings = 0;
    for (uint64_t i = 0; i < file_count; i++) strings += strlen(package_rows[i].name) + 1;
    strings_offset = 24 + file_count * 24;
    index_size = (size_t)strings_offset + strings;
    index_bytes = malloc(index_size);
    need(index_bytes != NULL, "package index memory");
    memcpy(index_bytes, "MKW_RFS1", 8);
    memcpy(index_bytes + 8, &file_count, 8);
    memcpy(index_bytes + 16, &strings_offset, 8);
    uint32_t at = 0;
    for (uint64_t i = 0; i < file_count; i++) {
        uint32_t len = (uint32_t)strlen(package_rows[i].name) + 1;
        Entry e = {package_rows[i].offset, package_rows[i].size, at, len};
        memcpy(index_bytes + 24 + i * 24, &e, sizeof e);
        memcpy(index_bytes + strings_offset + at, package_rows[i].name, len);
        at += len;
    }
}

// RomFS row i's bytes: an open file and where they start in it (a loose file: the caller closes it; the package: shared,
// never closed), or for a .nup zstd file, the whole file decompressed in *mem (malloc'd) and NULL returned; a .nx file
// gives neither (NULL, *mem NULL): its reads go through pack_read_at.
static PlatFile *row_open(uint64_t i, uint64_t *base, uint8_t **mem) {
    *base = 0;
    *mem = NULL;
    if (package) {
        const PackEntry *e = &package_rows[i];
        if (e->kind == PACK_RAW) {
            *base = e->offset;
            return pack_file(package);
        }
        if (e->kind == PACK_READ) return NULL;
        double t0 = now_s();
        *mem = pack_read_whole(package, e);
        if (!*mem) rt_fatal("RomFS: %s in %s does not decompress", e->name, loose_root);
        double ms = (now_s() - t0) * 1e3;
        if (ms >= 20)   // the game's open waits for it (on the thread that opens; #167 review)
            rt_log("RomFS: %s (%.1f MB) decompressed in %.0f ms", e->name, e->size / 1e6, ms);
        return NULL;
    }
    char path[4096];
    need(snprintf(path, sizeof path, "%s/%s", loose_root, entry_name(entry(i))) < (int)sizeof path, "RomFS path");
    return open_read(path);
}

uint8_t *romfs_read_whole(const char *rel, uint64_t *size) {
    uint64_t i = lower(rel), base;
    if (i >= file_count || strcmp(entry_name(entry(i)), rel)) return NULL;
    uint8_t *mem;
    PlatFile *f = row_open(i, &base, &mem);
    *size = entry(i).size;
    if (!f && !mem && package) return pack_read_whole(package, &package_rows[i]);   // a .nx row
    if (mem || !f) return mem;
    mem = malloc(*size ? *size : 1);
    for (uint64_t done = 0; mem && done < *size;) {
        uint32_t want = *size - done > READ_CHUNK ? READ_CHUNK : (uint32_t)(*size - done);
        int64_t n = plat_file_pread(f, mem + done, want, base + done);
        if (n <= 0) free(mem), mem = NULL;
        else done += (uint64_t)n;
    }
    if (!package) plat_file_close(f);
    return mem;
}

static void init_once(void *param) {
    (void)param;
    double t0 = now_s();
    const char *src = rt_config()->romfs;
    if (pack_is(src)) load_package_index(src);
    else load_loose_index(src);
    overlay_init();
    rt_log("RomFS ready: %llu %s %s, %zu overlays, %zu added, %.2f s", (unsigned long long)file_count,
           package ? (strcmp(pack_format(package), "nx") ? "files in the .nup package" : "files in the .nx package")
                   : "loose files under",
           loose_root, noverlay, nadded, now_s() - t0);
}

void fs_init(void) {
    static PlatOnce once = PLAT_ONCE_INIT;
    plat_once(&once, init_once, NULL);
}

// ---- the RomFS interface (fs.h)
void fs_trace(const char *what, const char *path) {
    static double t0;
    if (!(rt_trace & RT_TRACE_FS)) return;
    if (!t0) t0 = now_s();
    rt_log("fs %8.3f %s %s", now_s() - t0, what, path ? path : "(null)");
}

uint64_t romfs_file_count(void) { return file_count; }

// Mals/<region><lang>/...: the game's own texts of a language (USen, EUpl, a Thai folder since 1.7.0), by its two
// lowercase letters; the index is sorted, so the folder run starts at the first "Mals/" row.
int romfs_has_language(const char *lang) {
    for (uint64_t i = lower("Mals/"); i < file_count; i++) {
        const char *n = entry_name(entry(i));
        if (strncmp(n, "Mals/", 5)) break;
        if (strlen(n) > 10 && n[9] == '/' && !strncmp(n + 7, lang, 2)) return 1;
    }
    return 0;
}

const char *romfs_entry(uint64_t i, uint64_t *offset, uint64_t *size) {
    if (i >= file_count) return NULL;
    Entry e = entry(i);
    if (offset) *offset = e.offset;
    if (size) *size = e.size;
    return entry_name(e);
}

const char *romfs_source_path(uint64_t i, char *buf, size_t n) {
    if (i >= file_count || (package && package_rows[i].kind != PACK_RAW)) return NULL;
    if (package) return snprintf(buf, n, "%s", loose_root) < (int)n ? buf : NULL;
    return snprintf(buf, n, "%s/%s", loose_root, entry_name(entry(i))) < (int)n ? buf : NULL;
}

size_t romfs_overlay_count(void) { return noverlay; }

const char *romfs_overlay(size_t k, const char **file, uint64_t *size, int *is_added) {
    if (k >= noverlay) return NULL;
    *file = overlays[k].path;
    *size = overlays[k].size;
    *is_added = overlays[k].index >= file_count;
    return overlays[k].name;
}

int romfs_owns(const char *path) {
    size_t n = strlen(mount_name);
    return n && path && !strncmp(path, mount_name, n) && path[n] == ':';
}

int romfs_handle(uint64_t h) { return h > ROMFS_HANDLE_BASE && h < ROMFS_HANDLE_BASE + NH; }

uint32_t romfs_mount(const char *name) {
    fs_init();
    if (!name || !romfs_path_valid_relative(name) || strlen(name) >= sizeof mount_name || strchr(name, '/') ||
        strchr(name, ':'))
        return FS_R_INVALID;
    plat_mutex_lock(&lock);
    if (*mount_name && strcmp(name, mount_name)) rt_fatal("second RomFS mount %s (%s is mounted)", name, mount_name);
    strcpy(mount_name, name);
    plat_mutex_unlock(&lock);
    rt_log("fs mount %s: -> RomFS", name);
    return 0;
}

uint32_t romfs_open(uint64_t *out, const char *path, int mode) {
    fs_trace("rom", path);
    if (!out || mode != FS_OPEN_READ) return FS_R_UNSUPPORTED_OPEN;
    const char *p = romfs_path_relative(mount_name, path);
    if (!p) return FS_R_PATH_NOT_FOUND;
    uint64_t i = file_index(p);
    if (i == UINT64_MAX) return FS_R_PATH_NOT_FOUND;
    uint32_t ov = overlay_slot(i);
    PlatFile *f = NULL;
    uint64_t base = 0;
    uint8_t *mem = NULL;
    if (!ov && i < file_count) {
        f = row_open(i, &base, &mem);
        if (!f && !mem && !(package && package_rows[i].kind == PACK_READ)) return FS_R_PATH_NOT_FOUND;
    }
    if (ov) {
        const Overlay *o = &overlays[ov - 1];
        f = open_read(o->path);
        Identity id;
        if (f) id = identity_of(f);
        if (!f || memcmp(&id, &o->id, sizeof id))
            rt_fatal("RomFS overlay %s is missing or changed since the start check (restart the game)", o->path);
    }
    plat_mutex_lock(&lock);
    for (unsigned j = 1; j < NH; j++)
        if (!handles[j]) {
            handles[j] = i + 1;
            handle_ovl[j] = ov;
            handle_file[j] = f;
            handle_base[j] = base;
            handle_mem[j] = mem;
            *out = ROMFS_HANDLE_BASE + j;
            plat_mutex_unlock(&lock);
            return 0;
        }
    plat_mutex_unlock(&lock);
    rt_fatal("RomFS file handle table full (%d)", NH);
}

// The handle's open file (the overlay's or the original's), where its bytes start, or its bytes in memory, and its size.
static int handle_source(uint64_t h, PlatFile **f, uint64_t *base, uint8_t **mem, uint64_t *size) {
    if (!romfs_handle(h)) return 0;
    uint64_t j = h - ROMFS_HANDLE_BASE;
    plat_mutex_lock_shared(&lock);
    uint64_t i = handles[j];
    uint32_t ov = handle_ovl[j];
    PlatFile *of = handle_file[j];
    *base = handle_base[j];
    *mem = handle_mem[j];
    plat_mutex_unlock_shared(&lock);
    if (!i) return 0;
    *f = of;
    *size = ov ? overlays[ov - 1].size : entry(i - 1).size;
    return 1;
}

uint32_t romfs_read(uint64_t *out, uint64_t h, int64_t offset, void *buffer, uint64_t size) {
    PlatFile *f;
    uint64_t fsize, base;
    uint8_t *mem;
    if (!out || offset < 0 || !handle_source(h, &f, &base, &mem, &fsize)) return FS_R_INVALID;
    if ((uint64_t)offset > fsize || (!buffer && size)) return FS_R_INVALID;
    uint64_t n = size < fsize - (uint64_t)offset ? size : fsize - (uint64_t)offset, done = 0;
    if (mem) {
        mem_thaw((uintptr_t)buffer, n);
        memcpy(buffer, mem + offset, n);
        *out = n;
        return 0;
    }
    if (!f) {   // a .nx row: only the blocks this range touches are read and decoded
        uint64_t i = handles[h - ROMFS_HANDLE_BASE] - 1;
        mem_thaw((uintptr_t)buffer, n);
        if (!pack_read_at(package, &package_rows[i], (uint64_t)offset, buffer, n))
            rt_fatal("RomFS: read of %s at %#llx failed (see the log)", package_rows[i].name, (unsigned long long)offset);
        *out = n;
        return 0;
    }
    while (done < n) {
        uint32_t chunk = n - done > READ_CHUNK ? READ_CHUNK : (uint32_t)(n - done);
        int64_t r = read_at(f, (uint8_t *)buffer + done, chunk, base + (uint64_t)offset + done);
        if (r <= 0) rt_fatal("RomFS read failure at %#llx", (unsigned long long)(offset + done));
        done += (uint64_t)r;
    }
    *out = done;
    return 0;
}

uint32_t romfs_size(int64_t *out, uint64_t h) {
    PlatFile *f;
    uint64_t size, base;
    uint8_t *mem;
    if (!out || !handle_source(h, &f, &base, &mem, &size)) return FS_R_INVALID;
    *out = (int64_t)size;
    return 0;
}

void romfs_close(uint64_t h) {
    PlatFile *f;
    uint64_t size, base;
    uint8_t *mem;
    if (!handle_source(h, &f, &base, &mem, &size))
        rt_fatal("RomFS: close of invalid file handle %#llx", (unsigned long long)h);
    uint64_t j = h - ROMFS_HANDLE_BASE;
    plat_mutex_lock(&lock);
    PlatFile *of = handle_file[j];   // an overlay's, a loose RomFS file's or the shared package
    int shared = !handle_ovl[j] && package;
    handles[j] = 0;
    handle_ovl[j] = 0;
    handle_file[j] = NULL;
    handle_mem[j] = NULL;
    plat_mutex_unlock(&lock);
    if (of && !shared) plat_file_close(of);
    free(mem);
}

uint32_t romfs_entry_type(int *out, const char *path) {
    if (!out) return FS_R_INVALID;
    const char *p = romfs_path_relative(mount_name, path);
    if (!p) return FS_R_PATH_NOT_FOUND;
    if (file_index(p) != UINT64_MAX) {
        *out = FS_ENTRY_FILE;
        return 0;
    }
    char dir[4096];
    if (snprintf(dir, sizeof dir, "%s/", p) >= (int)sizeof dir) return FS_R_INVALID;
    if (folder_exists(dir)) {
        *out = FS_ENTRY_DIRECTORY;
        return 0;
    }
    return FS_R_PATH_NOT_FOUND;
}

// The index holds file paths only, sorted: a folder's entries are the paths that start with "folder/"; the part up to
// the next '/' names a child folder (consecutive paths share it) or, without one, a file. Added files follow.
static int list_from(const char *prefix, size_t n, const char *name, uint64_t size, char *last, size_t cap,
                     int (*add)(const char *, int, uint64_t, void *), void *ctx, int *found) {
    if (strncmp(name, prefix, n)) return -1;   // past the folder
    const char *rest = name + n, *slash = strchr(rest, '/');
    *found = 1;
    if (!slash) return add(rest, 0, size, ctx);
    size_t k = (size_t)(slash - rest);
    if (k >= cap || (!strncmp(last, rest, k) && !last[k])) return 0;   // the same child folder as the last path
    memcpy(last, rest, k), last[k] = 0;
    return add(last, 1, 0, ctx);
}

uint32_t romfs_list(const char *path, int (*add)(const char *name, int is_dir, uint64_t size, void *ctx), void *ctx) {
    const char *p = romfs_path_relative(mount_name, path);
    if (!p) return FS_R_PATH_NOT_FOUND;
    char prefix[4096], last[1024] = "";
    if (snprintf(prefix, sizeof prefix, *p ? "%s/" : "%s", p) >= (int)sizeof prefix) return FS_R_INVALID;
    size_t n = strlen(prefix);
    int found = 0, r = 0;
    for (uint64_t i = lower(prefix); i < file_count && r == 0; i++) {
        Entry e = entry(i);
        r = list_from(prefix, n, entry_name(e), e.size, last, sizeof last, add, ctx, &found);
    }
    last[0] = 0;
    for (size_t k = added_lower(prefix); k < nadded && r >= 0; k++)
        if ((r = list_from(prefix, n, added[k], 0, last, sizeof last, add, ctx, &found)) > 0) break;
    return found || !*p ? 0 : FS_R_PATH_NOT_FOUND;
}

// ---- overrides
// nn::Result nn::fs::MountRom(const char *name, void *cache, size_t cache_size)
static void ov_mount_rom(Cpu *c) { ret_x(c, romfs_mount(arg_p(c, 0))); }
// nn::Result nn::fs::QueryMountRomCacheSize(size_t *out): no cache needed, the host reads the files directly
static void ov_query_cache(Cpu *c) {
    uint64_t *out = arg_p(c, 0);
    if (out) *out = 0;
    ret_x(c, out ? 0 : FS_R_INVALID);
}
// void nn::fs::InitializeForApplication(): SDK-internal; opens the index instead of the fs service session
static void ov_initialize(Cpu *c) {
    (void)c;
    fs_init();
}

HOST_OVERRIDE("_ZN2nn2fs8MountRomEPKcPvm", ov_mount_rom);
HOST_OVERRIDE("_ZN2nn2fs22QueryMountRomCacheSizeEPm", ov_query_cache);
HOST_OVERRIDE("_ZN2nn2fs24InitializeForApplicationEv", ov_initialize);
