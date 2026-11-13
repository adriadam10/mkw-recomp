// The Mods page: scan, import, enable, delete. Enabling only writes config (mkw.ini's MODS order and the mod's
// switches); the game reads both at the next start, so nothing here touches the running game. The import runs
// tools/mod/import.py in a child process on a thread, like launcher_setup.cpp runs prepare.py.
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/stat.h>
#endif

#include "imgui.h"
#include "ui/launcher/launcher_setup.h"
#include "ui/launcher/mods.h"
#include "ui/menu/menu_look.h"

extern "C" {
#include "cfg.h"
#include "platform/platform.h"
#include "ui_text.h"
void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
}

static struct {
    Mod mods[MODS_MAX];
    int n;
    char exe_dir[PLAT_PATH_MAX];   // setup tools only; all installation data uses cfg_game_dir()
    // import
    SDL_Thread *thread;
    SDL_Process *proc;
    SDL_Mutex *lock;
    int running, finished;
    char done[64], code[32], error[512];
    char pick[PLAT_PATH_MAX];   // the zip or folder the dialog chose
    int pick_is_zip;
    char name[MOD_NAME_MAX];    // the rename field; empty: import.py takes the mod's own name
    char variant[512];          // the mod chosen from an archive with several (import.py --variant)
    // merge (tools/mod/merge.py, same child-process pattern; reruns after every toggle/import/delete)
    SDL_Thread *mthread;
    SDL_Process *mproc;
    int mrunning, mfinished;
    char mcode[32], merror[512];
    ModConflict conflicts[64];
    int nconflicts;
    // what the import and merge threads leave for the UI thread (mods_poll): G.mods, G.n and the config are only
    // touched there, since the page draws them every frame without the lock
    char pending_enable[64];
    int pending_refresh;
} G = {};

static void set_enabled(const char *name, int on);

static void rebuild_path(char *out, size_t cap) { snprintf(out, cap, "%s/mods/.rebuild", cfg_game_dir()); }

static void mark_rebuild(const char *name) {
    for (int i = 0; i < G.n; i++)
        if (!strcmp(G.mods[i].name, name) && G.mods[i].code) {
            char path[PLAT_PATH_MAX];
            rebuild_path(path, sizeof path);
            plat_file_write_atomic(path, name, strlen(name));
            rt_log("mods: %s changes the game code: it is built again before the next start", name);
        }
}

// The risk of installing a mod, above every install and import: the "I understand" box until the player ticks it once
// (MODS_ACK). True when the install may go on.
int mods_risk_draw(void) {
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetFontSize() * 30);
    ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.35f, 1), "%s", menu_text(UI_MOD_RISK));
    ImGui::PopTextWrapPos();
    const char *ack = cfg_get("MKW_MODS_ACK");
    if (ack && !strcmp(ack, "1")) return 1;
    bool tick = false;
    if (ImGui::Checkbox(menu_text(UI_MOD_RISK_ACK), &tick) && tick) {
        cfg_set("MKW_MODS_ACK", "1");
        cfg_save();
    }
    return tick;
}

// ---- the save, before the first start with a new set of mods: <profile>/save copied to
// <profile>/save-before-mods/<time>/, which the save's own rotation (save-backup, fs_host.c) never touches; the newest
// KEEP_BEFORE_MODS stay
#define KEEP_BEFORE_MODS 10
static void remove_tree(const char *path);
static int remove_backup(const char *root, const char *name);

struct CopyCtx {
    char src[PLAT_PATH_MAX], dst[PLAT_PATH_MAX];
    int files, failed;
};

static int copy_cb(const char *name, int kind, uint64_t, void *ctx) {
    CopyCtx *c = (CopyCtx *)ctx, sub = *c;
    snprintf(sub.src, sizeof sub.src, "%s/%s", c->src, name);
    snprintf(sub.dst, sizeof sub.dst, "%s/%s", c->dst, name);
    sub.files = sub.failed = 0;
    if (kind == PLAT_FS_DIR) {
        plat_file_mkdir_p(sub.dst);
        plat_fs_list(sub.src, copy_cb, &sub);
    } else {
        uint64_t n = 0;
        uint8_t *data = plat_file_read(sub.src, &n);
        if (data && plat_file_write_atomic(sub.dst, data, n)) sub.files++;
        else sub.failed++;
        free(data);
    }
    c->files += sub.files, c->failed += sub.failed;
    return 1;
}

static char g_oldest[64];
static int g_nbackups;
static int oldest_cb(const char *name, int kind, uint64_t, void *) {
    if (kind != PLAT_FS_DIR) return 1;
    g_nbackups++;
    if (!g_oldest[0] || strcmp(name, g_oldest) < 0) snprintf(g_oldest, sizeof g_oldest, "%s", name);
    return 1;
}

void mods_before_play(const char *player_dir) {
    const char *on = cfg_get("MKW_MODS"), *played = cfg_get("MKW_MODS_PLAYED");
    if (!strcmp(on ? on : "", played ? played : "")) return;
    if (on && *on) {
        CopyCtx c = {};
        snprintf(c.src, sizeof c.src, "%s/save", player_dir);
        SDL_Time now;
        SDL_DateTime t;
        SDL_GetCurrentTime(&now);
        SDL_TimeToDateTime(now, &t, true);
        char root[PLAT_PATH_MAX];
        snprintf(root, sizeof root, "%.*s/save-before-mods", (int)(strlen(c.src) - 5), c.src);
        snprintf(c.dst, sizeof c.dst, "%s/%04d%02d%02d-%02d%02d%02d", root, t.year, t.month, t.day, t.hour,
                 t.minute, t.second);
        if (plat_dir_exists(c.src)) {
            plat_file_mkdir_p(c.dst);
            plat_fs_list(c.src, copy_cb, &c);
            rt_log("mods: the save before these mods (%s): %d files in %s%s", on, c.files, c.dst,
                   c.failed ? ", some could not be copied" : "");
            for (;;) {   // the newest ones stay
                g_oldest[0] = 0, g_nbackups = 0;
                plat_fs_list(root, oldest_cb, nullptr);
                if (g_nbackups <= KEEP_BEFORE_MODS || !g_oldest[0]) break;
                if (!remove_backup(root, g_oldest)) break;   // it stays: listed again, it would loop forever
            }
        }
    }
    cfg_set("MKW_MODS_PLAYED", on ? on : "");
    cfg_save();
}

int mods_code_changed(void) {
    char path[PLAT_PATH_MAX];
    rebuild_path(path, sizeof path);
    return plat_file_exists(path);
}

void mods_code_built(void) {
    char path[PLAT_PATH_MAX];
    rebuild_path(path, sizeof path);
    remove(path);
}

void mods_poll(void) {
    if (!G.lock) return;
    char enable[64];
    SDL_LockMutex(G.lock);
    snprintf(enable, sizeof enable, "%s", G.pending_enable);
    int refresh = G.pending_refresh;
    G.pending_enable[0] = 0, G.pending_refresh = 0;
    SDL_UnlockMutex(G.lock);
    if (enable[0] || refresh) mods_refresh(G.exe_dir);   // the new mod is on disk; conflicts.json changed
    if (enable[0]) set_enabled(enable, 1);                 // imported means enabled: its switches apply and it merges
}

ModsState mods_state(void) {
    ModsState s = {0, G.finished, "", "", ""};
    if (G.lock) SDL_LockMutex(G.lock);
    s.running = G.running;
    snprintf(s.done, sizeof s.done, "%s", G.done);
    snprintf(s.code, sizeof s.code, "%s", G.code);
    snprintf(s.error, sizeof s.error, "%s", G.error);
    if (G.lock) SDL_UnlockMutex(G.lock);
    return s;
}

static void mods_dir(char *out, size_t cap) {
    snprintf(out, cap, "%s/mods", cfg_game_dir());
}

// The value of "key" in {"a": 1, "key": value, ...} (mod.json's flat top level): a number, true/false or "string".
static int json_value(const char *json, const char *key, char *out, size_t cap) {
    char want[64];
    snprintf(want, sizeof want, "\"%s\"", key);
    const char *p = strstr(json, want);
    if (!p) return 0;
    p = strchr(p + strlen(want), ':');
    if (!p) return 0;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '"') p++;
    size_t n = 0;
    while (p[n] && p[n] != '"' && p[n] != ',' && p[n] != '}' && p[n] != '\n' && n + 1 < cap) n++;
    while (n && (p[n - 1] == ' ' || p[n - 1] == '\t')) n--;
    memcpy(out, p, n);
    out[n] = 0;
    return 1;
}

// mod.json's "switches": {"MKW_A": "1", ...} into the mod (our own files: flat, MKW_ keys, short values).
static void json_switches(const char *json, Mod *m) {
    const char *p = strstr(json, "\"switches\"");
    if (!p || !(p = strchr(p, '{'))) return;
    for (p++; *p && *p != '}' && m->nswitches < MOD_SWITCHES_MAX; p++) {
        if (*p != '"') continue;
        const char *k = ++p, *ke = strchr(k, '"');
        if (!ke || ke - k < 5 || ke - k >= (int)sizeof m->switches[0].name) break;
        const char *v = strchr(ke, ':');
        if (!v) break;
        v++;
        while (*v == ' ' || *v == '\t' || *v == '"') v++;
        const char *ve = v;
        while (*ve && *ve != '"' && *ve != ',' && *ve != '}') ve++;
        size_t vn = ve - v;
        while (vn && (v[vn - 1] == ' ' || v[vn - 1] == '\t')) vn--;
        if (vn == 0 || vn >= sizeof m->switches[0].value) break;
        ModSwitch *s = &m->switches[m->nswitches++];
        memcpy(s->name, k, (size_t)(ke - k));
        s->name[ke - k] = 0;
        memcpy(s->value, v, vn);
        s->value[vn] = 0;
        p = ve;
    }
}

static int list_cb(const char *name, int kind, uint64_t, void *) {
    if (name[0] == '.') return 1;   // .merged and the like are the menu's own, not mods
    if (kind != PLAT_FS_DIR || G.n >= MODS_MAX) return 1;
    if (strlen(name) >= MOD_NAME_MAX) return 1;   // import.py caps names at 64; not ours
    Mod *m = &G.mods[G.n];
    memset(m, 0, sizeof *m);
    snprintf(m->name, sizeof m->name, "%s", name);
    snprintf(m->title, sizeof m->title, "%s", name);
    char meta[PLAT_PATH_MAX * 2 + 32], json[4096];
    snprintf(meta, sizeof meta, "%s/mods/%s/mod.json", cfg_game_dir(), name);
    PlatFile *f = plat_fs_open(meta, 0, NULL);
    if (f) {
        int64_t n = plat_file_pread(f, json, sizeof json - 1, 0);
        plat_file_close(f);
        if (n < 0) n = 0;
        json[n] = 0;
        char v[96];
        if (json_value(json, "title", v, sizeof v) && *v) snprintf(m->title, sizeof m->title, "%s", v);
        if (json_value(json, "has_romfs", v, sizeof v)) m->has_romfs = !strcmp(v, "true") || !strcmp(v, "1");
        if (json_value(json, "romfs_files", v, sizeof v)) m->romfs_files = atoi(v);
        if (json_value(json, "has_exefs", v, sizeof v)) m->has_exefs = !strcmp(v, "true") || !strcmp(v, "1");
        if (json_value(json, "exefs_match", v, sizeof v)) m->exefs_match = !strcmp(v, "true") || !strcmp(v, "1");
        m->code = strstr(json, "\"image\"") || strstr(json, "\"patches\"");
        const char *warn = strstr(json, "\"warnings\"");   // a list of names; ours, so a search is enough
        const char *end = warn ? strchr(warn, ']') : nullptr;
        if (warn && end) {
            char list[128];
            snprintf(list, sizeof list, "%.*s", (int)(end - warn), warn);
            m->warn_graphics = strstr(list, "\"graphics\"") != nullptr;
            m->warn_shaders = strstr(list, "\"shaders\"") != nullptr;
        }
        const char *b = strstr(json, "\"exefs_builds\"");
        if (b && (b = strchr(b, '[')) && (b = strchr(b, '"'))) {
            size_t n = 0;
            while (b[1 + n] && b[1 + n] != '"' && n < 16) n++;
            memcpy(m->exefs_build, b + 1, n);
            m->exefs_build[n] = 0;
        }
        json_switches(json, m);
    }
    G.n++;
    return 1;
}

static void remove_tree(const char *path);
static void conflicts_load(void);   // .merged/conflicts.json, defined below
// rewrite mods/choices.json, optionally setting one pick or dropping a mod's picks (defined below)
static void rewrite_choices(const char *archive, const char *inner, const char *winner, const char *drop_mod);
void mods_merge_start(const char *exe_dir);        // merge worker, defined below (public: mods.h)
static int mod_enabled(const char *name) {
    const char *mods = cfg_get("MKW_MODS");
    if (!mods || !*mods) return 0;
    char copy[4096];
    snprintf(copy, sizeof copy, "%s", mods);
    for (char *t = strtok(copy, ","); t; t = strtok(NULL, ",")) {
        while (*t == ' ') t++;
        if (!strcmp(t, name)) return 1;
    }
    return 0;
}

void mods_refresh(const char *exe_dir) {
    if (exe_dir != G.exe_dir) snprintf(G.exe_dir, sizeof G.exe_dir, "%s", exe_dir);   // not a self-copy (UB)
    G.n = 0;
    char dir[PLAT_PATH_MAX];
    mods_dir(dir, sizeof dir);
    if (plat_dir_exists(dir)) plat_fs_list(dir, list_cb, nullptr);
    // plat_fs_list order is the filesystem's; keep the page stable
    for (int i = 0; i < G.n; i++)
        for (int j = i + 1; j < G.n; j++)
            if (strcmp(G.mods[i].name, G.mods[j].name) > 0) {
                Mod t = G.mods[i];
                G.mods[i] = G.mods[j];
                G.mods[j] = t;
            }
    for (int i = 0; i < G.n; i++) G.mods[i].enabled = mod_enabled(G.mods[i].name);
    conflicts_load();
}

// Every scanned mod's switches off, then the enabled mods' on, in MODS order (later mods win shared keys).
void mods_apply(void) {
    const char *names[MODS_MAX * MOD_SWITCHES_MAX];
    int nn = 0;
    for (int i = 0; i < G.n; i++)
        for (int s = 0; s < G.mods[i].nswitches; s++) names[nn++] = G.mods[i].switches[s].name;
    if (nn) cfg_reset(names, nn);
    for (int i = 0; i < G.n; i++) {
        if (!G.mods[i].enabled) continue;
        for (int s = 0; s < G.mods[i].nswitches; s++) {
            char var[96];
            snprintf(var, sizeof var, "MKW_%s", G.mods[i].switches[s].name);
            cfg_set(var, G.mods[i].switches[s].value);
        }
    }
    cfg_save();
}

int mods_count(void) { return G.n; }
const Mod *mods_at(int i) { return i >= 0 && i < G.n ? &G.mods[i] : NULL; }

static void remove_tree(const char *path);
static void set_enabled(const char *name, int on) {
    char keep[MODS_MAX][MOD_NAME_MAX], cur[4096];
    int nk = 0;
    const char *mods = cfg_get("MKW_MODS");
    snprintf(cur, sizeof cur, "%s", mods ? mods : "");
    for (char *t = strtok(cur, ","); t && nk < MODS_MAX; t = strtok(NULL, ",")) {
        while (*t == ' ') t++;
        if (*t && strcmp(t, name)) snprintf(keep[nk++], sizeof keep[0], "%s", t);
    }
    // enabling appends: the MODS order is the overlay precedence, the newest mod wins shared files
    if (on && nk < MODS_MAX) snprintf(keep[nk++], sizeof keep[0], "%s", name);
    char out[4096] = "";
    for (int i = 0; i < nk; i++) {
        if (i) strcat(out, ",");
        strcat(out, keep[i]);
    }
    if (!on) rewrite_choices(nullptr, nullptr, nullptr, name);   // a disabled mod's picks go with it
    mark_rebuild(name);
    cfg_set("MKW_MODS", out);
    mods_refresh(G.exe_dir);
    mods_apply();   // saves too
    mods_merge_start(G.exe_dir);   // re-merges the enabled mods' shared archives
}

// the loose RomFS a manifest's original hashes come from: the game's own (set at start), else link mode's
// path, else the copied one
static char g_romfs_override[PLAT_PATH_MAX * 2];
static char g_exefs_override[PLAT_PATH_MAX * 2];

void mods_set_romfs(const char *path) {
    snprintf(g_romfs_override, sizeof g_romfs_override, "%s", path ? path : "");
}

void mods_set_exefs(const char *path) {
    snprintf(g_exefs_override, sizeof g_exefs_override, "%s", path ? path : "");
}

// the game's exefs, for the build id an import checks (the same resolution the game itself uses)
static void exefs_root(char *out, size_t cap) {
    if (g_exefs_override[0]) {
        snprintf(out, cap, "%s", g_exefs_override);
        return;
    }
    const char *e = cfg_get("MKW_EXEFS");
    if (e && *e)
        snprintf(out, cap, "%s", e);
    else
        snprintf(out, cap, "%s/exefs", cfg_game_dir());
}

static void romfs_root(char *out, size_t cap) {
    if (g_romfs_override[0]) {
        snprintf(out, cap, "%s", g_romfs_override);
        return;
    }
    const char *linked = cfg_get("MKW_ROMFS");
    if (linked && *linked)
        snprintf(out, cap, "%s", linked);
    else
        snprintf(out, cap, "%s/romfs", cfg_game_dir());
}

void mods_toggle(const char *name, int on) { set_enabled(name, on); mods_merge_start(G.exe_dir); }

int mods_is_enabled(const char *name) {
    for (int i = 0; i < G.n; i++)
        if (!strcmp(G.mods[i].name, name)) return G.mods[i].enabled;
    return mod_enabled(name);
}

void mods_delete(const char *name) {
    if (!name || !*name || strchr(name, '/') || strchr(name, '\\')) {
        rt_log("mods: refusing to delete '%s' (not a mod folder name)", name ? name : "(null)");
        return;
    }
    char path[PLAT_PATH_MAX * 2];
    snprintf(path, sizeof path, "%s/mods/%s", cfg_game_dir(), name);
    rt_log("mods: delete %s", path);
    mark_rebuild(name);   // before the folder goes: its mod.json says whether it changed the code
    remove_tree(path);
    rewrite_choices(nullptr, nullptr, nullptr, name);
    set_enabled(name, 0);   // drops it from MODS when present; re-applies switches and rescans either way
}

typedef struct {
    char path[PLAT_PATH_MAX];
} RmCtx;

// One of mods_before_play's copies, root/<YYYYMMDD-HHMMSS>: remove_tree refuses anything outside mods/ (on purpose),
// so the copies have their own remover, as narrow: that name only, a real folder (never a link followed). 1 when gone.
static int rm_cb(const char *name, int kind, uint64_t, void *ctx);
static int remove_backup(const char *root, const char *name) {
    int ok = strlen(name) == 15 && name[8] == '-';
    for (int i = 0; ok && i < 15; i++) ok = i == 8 || (name[i] >= '0' && name[i] <= '9');
    char path[PLAT_PATH_MAX];
    snprintf(path, sizeof path, "%s/%s", root, name);
    if (!ok || plat_fs_kind(path) != PLAT_FS_DIR) {
        rt_log("mods: refusing to delete %s (not a save copy of save-before-mods)", path);
        return 0;
    }
    RmCtx c;
    snprintf(c.path, sizeof c.path, "%s", path);
    plat_fs_list(path, rm_cb, &c);
    if (plat_fs_rmdir(path) != PLAT_FS_OK) {
        rt_log("mods: cannot delete the old save copy %s (OS error %d)", path, plat_fs_os_error());
        return 0;
    }
    rt_log("mods: deleted the old save copy %s", path);
    return 1;
}

static int rm_cb(const char *name, int kind, uint64_t, void *ctx) {
    RmCtx *c = (RmCtx *)ctx;
    char full[PLAT_PATH_MAX * 2];
    snprintf(full, sizeof full, "%s/%s", c->path, name);
    if (kind == PLAT_FS_DIR) {
        RmCtx sub;
        snprintf(sub.path, sizeof sub.path, "%s", full);
        plat_fs_list(full, rm_cb, &sub);
        plat_fs_rmdir(full);
    } else {
        plat_fs_unlink(full);
    }
    return 1;
}

// Our own imports only, under <game>/mods: depth-first, then the mod folder itself. Bounded on
// purpose: a bad path here once emptied every mod, so it now refuses anything but one folder
// directly inside mods/ (and logs every refusal).
static void remove_tree(const char *path) {
    char root[PLAT_PATH_MAX * 2 + 16];
    snprintf(root, sizeof root, "%s/mods/", cfg_game_dir());
    // only a folder strictly inside <game>/mods: never the mods dir itself, never the game dir, whatever we were handed
    if (!path || strncmp(path, root, strlen(root)) != 0) {
        rt_log("mods: refusing to delete %s (outside %s)", path ? path : "(null)", root);
        return;
    }
    const char *leaf = path + strlen(root);
    if (!*leaf || strchr(leaf, '/')) {
        rt_log("mods: refusing to delete %s (not one folder of %s)", path, root);
        return;
    }
#ifndef _WIN32
    struct stat st;
    if (lstat(path, &st) || !S_ISDIR(st.st_mode)) {
        rt_log("mods: refusing to delete %s (not a folder)", path);
        return;
    }
#endif
    RmCtx c;
    snprintf(c.path, sizeof c.path, "%s", path);
    plat_fs_list(path, rm_cb, &c);
    plat_fs_rmdir(path);
    rt_log("mods: deleted %s", path);
}

// The wrapper scan for the rename prefill: the single non-hidden subfolder's name, and how many there are.
static char wrap_sub[MOD_NAME_MAX];
static int wrap_n;

static int wrap_cb(const char *name, int kind, uint64_t, void *) {
    if (kind == PLAT_FS_DIR && name[0] != '.') {
        if (wrap_n == 0) snprintf(wrap_sub, sizeof wrap_sub, "%s", name);
        wrap_n++;
    }
    return 1;
}

void mods_pick(const char *path, int is_zip) {
    snprintf(G.pick, sizeof G.pick, "%s", path);
    G.pick_is_zip = is_zip;
    // prefill the rename field: the zip stem, or the folder's name (its single subfolder when the picked folder
    // only wraps the mod, as a zip extraction does); empty means import.py takes the mod's own name
    const char *base = strrchr(path, '/');
#ifdef _WIN32
    const char *bs = strrchr(path, '\\');
    if (bs && (!base || bs > base)) base = bs;
#endif
    base = base ? base + 1 : path;
    snprintf(G.name, sizeof G.name, "%s", base);
    G.variant[0] = 0;
    if (is_zip) {
        char *dot = strrchr(G.name, '.');   // "timer.zip" -> "timer", the same for .rar and .7z
        if (dot && (!SDL_strcasecmp(dot, ".zip") || !SDL_strcasecmp(dot, ".rar") || !SDL_strcasecmp(dot, ".7z")))
            *dot = 0;
    } else {
        char romfs[PLAT_PATH_MAX * 2], exefs[PLAT_PATH_MAX * 2];
        snprintf(romfs, sizeof romfs, "%s/romfs", path);
        snprintf(exefs, sizeof exefs, "%s/exefs", path);
        if (!plat_dir_exists(romfs) && !plat_dir_exists(exefs) && plat_dir_exists(path)) {
            wrap_n = 0;
            wrap_sub[0] = 0;
            plat_fs_list(path, wrap_cb, NULL);
            if (wrap_n == 1)
                snprintf(G.name, sizeof G.name, "%s", wrap_sub);
            else
                G.name[0] = 0;   // import.py decides by its own single-inner rule
        }
    }
    G.finished = 0;
    G.done[0] = G.code[0] = G.error[0] = 0;
}

int mods_has_pick(void) { return G.pick[0] != 0; }
int mods_pick_is_zip(void) { return G.pick_is_zip; }
const char *mods_pick_path(void) { return G.pick; }
char *mods_name_buf(void) { return G.name; }

void mods_cancel(void) {
    if (!G.lock) return;
    SDL_LockMutex(G.lock);
    if (G.proc) SDL_KillProcess(G.proc, true);
    SDL_UnlockMutex(G.lock);
}

void mods_import_variant(const char *exe_dir, const char *variant) {
    snprintf(G.variant, sizeof G.variant, "%s", variant);
    G.finished = 0;
    G.done[0] = G.code[0] = G.error[0] = 0;
    mods_import_start(exe_dir);
}

void mods_clear_pick(void) {
    G.pick[0] = G.name[0] = G.variant[0] = 0;
    G.finished = 0;
    G.done[0] = G.code[0] = G.error[0] = 0;
}

static void set_finished(int how, const char *code, const char *why) {
    SDL_LockMutex(G.lock);
    G.running = 0;
    G.finished = how;
    if (code) snprintf(G.code, sizeof G.code, "%s", code);
    if (why) snprintf(G.error, sizeof G.error, "%s", why);
    SDL_UnlockMutex(G.lock);
}

// One event line of import.py ("done <name> ..." or "error <code> <detail>", like prepare.py's).
static void on_line(char *line) {
    SDL_LockMutex(G.lock);
    char *arg = strchr(line, ' ');
    if (arg) *arg++ = 0;
    if (!strcmp(line, "done") && arg) {
        char *sp = strchr(arg, ' ');
        if (sp) *sp = 0;
        snprintf(G.done, sizeof G.done, "%s", arg);
    } else if (!strcmp(line, "error") && arg) {
        char *why = strchr(arg, ' ');
        if (why) *why++ = 0;
        snprintf(G.code, sizeof G.code, "%s", arg);
        snprintf(G.error, sizeof G.error, "%s", why ? why : "");
    }
    SDL_UnlockMutex(G.lock);
}

static int SDLCALL reader(void *) {
    SDL_IOStream *out = SDL_GetProcessOutput(G.proc);
    char line[2048];
    size_t n = 0;
    char c;
    for (;;) {
        if (SDL_ReadIO(out, &c, 1) != 1) {   // the pipe does not block: no data yet is not the end
            if (SDL_GetIOStatus(out) != SDL_IO_STATUS_NOT_READY) break;
            SDL_Delay(20);
            continue;
        }
        if (c == '\r') continue;
        if (c != '\n') {
            if (n + 1 < sizeof line) line[n++] = c;
            continue;
        }
        line[n] = 0;
        n = 0;
        on_line(line);
    }
    int code = -1;
    SDL_WaitProcess(G.proc, true, &code);
    SDL_DestroyProcess(G.proc);
    G.proc = nullptr;
    ModsState s = mods_state();
    int ok = code == 0 && s.done[0];
    if (ok) {
        SDL_LockMutex(G.lock);
        snprintf(G.pending_enable, sizeof G.pending_enable, "%s", s.done);   // mods_poll, on the UI thread
        SDL_UnlockMutex(G.lock);
    }
    if (!ok && !s.code[0]) set_finished(-1, "stopped", "");
    else set_finished(ok ? 1 : -1, nullptr, nullptr);
    rt_log("mods: import ended with code %d", code);
    return 0;
}

void mods_import_start(const char *exe_dir) {
    if (!G.lock) G.lock = SDL_CreateMutex();
    if (G.running || !G.pick[0]) return;
    char python[1200], script[1200];
    SDL_LockMutex(G.lock);
    G.running = 1;
    G.finished = 0;
    G.done[0] = G.code[0] = G.error[0] = 0;
    SDL_UnlockMutex(G.lock);
    if (!setup_tool(exe_dir, "mod/import.py", python, script, sizeof python)) {
        set_finished(-1, "no_setup", "");
        return;
    }
    // the loose RomFS the manifest's original hashes come from: link mode's path, else the copied one
    char romfs[PLAT_PATH_MAX * 2];
    romfs_root(romfs, sizeof romfs);
    char exefs[PLAT_PATH_MAX * 2];
    exefs_root(exefs, sizeof exefs);
    const char *kind = G.pick_is_zip ? "--zip" : "--folder";
    // the name and the variant are optional: each pair only when it has a value
    const char *args[16] = {python, "-I", script, "--game", cfg_game_dir(), "--romfs", romfs, "--exefs", exefs, kind, G.pick};
    int na = 11;
    if (G.name[0]) args[na++] = "--name", args[na++] = G.name;
    if (G.variant[0]) args[na++] = "--variant", args[na++] = G.variant;
    args[na] = nullptr;
    rt_log("mods: %s %s --game %s %s %s%s%s", python, script, cfg_game_dir(), kind, G.pick,
           G.name[0] ? " --name " : "", G.name[0] ? G.name : "");
    G.proc = SDL_CreateProcess(args, true);
    if (!G.proc) {
        set_finished(-1, "", SDL_GetError());
        return;
    }
    SDL_DetachThread(SDL_CreateThread(reader, "mods-import", nullptr));
}

// ---- archive merge (tools/mod/merge.py): enabled mods sharing one game archive get a merged copy

static void merge_finished(int how, const char *code, const char *why) {
    SDL_LockMutex(G.lock);
    G.mrunning = 0;
    G.mfinished = how;
    if (code) snprintf(G.mcode, sizeof G.mcode, "%s", code);
    if (why) snprintf(G.merror, sizeof G.merror, "%s", why);
    SDL_UnlockMutex(G.lock);
}

// merge.py's event lines: "conflict ..." picks go to conflicts.json (read below); the rest is progress
static void merge_line(char *line) {
    SDL_LockMutex(G.lock);
    char *arg = strchr(line, ' ');
    if (arg) *arg++ = 0;
    if (!strcmp(line, "done") || !strcmp(line, "fresh")) {
        // the merged tree (and conflicts.json) is on disk now
    } else if (!strcmp(line, "error") && arg) {
        char *why = strchr(arg, ' ');
        if (why) *why++ = 0;
        snprintf(G.mcode, sizeof G.mcode, "%s", arg);
        snprintf(G.merror, sizeof G.merror, "%s", why ? why : "");
    }
    SDL_UnlockMutex(G.lock);
}

static int SDLCALL merge_reader(void *) {
    SDL_IOStream *out = SDL_GetProcessOutput(G.mproc);
    char line[2048];
    size_t n = 0;
    char c;
    for (;;) {
        if (SDL_ReadIO(out, &c, 1) != 1) {
            if (SDL_GetIOStatus(out) != SDL_IO_STATUS_NOT_READY) break;
            SDL_Delay(20);
            continue;
        }
        if (c == '\r') continue;
        if (c != '\n') {
            if (n + 1 < sizeof line) line[n++] = c;
            continue;
        }
        line[n] = 0;
        n = 0;
        merge_line(line);
    }
    int code = -1;
    SDL_WaitProcess(G.mproc, true, &code);
    SDL_DestroyProcess(G.mproc);
    G.mproc = nullptr;
    int ok = code == 0;
    if (ok) {
        SDL_LockMutex(G.lock);
        G.pending_refresh = 1;   // conflicts.json changed: mods_poll rescans on the UI thread
        SDL_UnlockMutex(G.lock);
    }
    SDL_LockMutex(G.lock);
    int had_code = G.mcode[0] != 0;
    SDL_UnlockMutex(G.lock);
    if (!ok && !had_code) merge_finished(-1, "stopped", "");
    else merge_finished(ok ? 1 : -1, nullptr, nullptr);
    rt_log("mods: merge ended with code %d", code);
    return 0;
}

void mods_merge_start(const char *exe_dir) {
    if (!G.lock) G.lock = SDL_CreateMutex();
    if (G.mrunning) return;
    char python[1200], script[1200];
    SDL_LockMutex(G.lock);
    G.mrunning = 1;
    G.mfinished = 0;
    G.mcode[0] = G.merror[0] = 0;
    SDL_UnlockMutex(G.lock);
    if (!setup_tool(exe_dir, "mod/merge.py", python, script, sizeof python)) {
        merge_finished(-1, "no_setup", "");
        return;
    }
    char romfs[PLAT_PATH_MAX * 2], mods[4096], choices[PLAT_PATH_MAX * 2];
    romfs_root(romfs, sizeof romfs);
    const char *list = cfg_get("MKW_MODS");
    snprintf(mods, sizeof mods, "%s", list ? list : "");
    snprintf(choices, sizeof choices, "%s/mods/choices.json", cfg_game_dir());
    const char *args[] = {python, "-I", script, "--game", cfg_game_dir(), "--romfs", romfs,
                          "--enable", mods, "--choices", choices, nullptr};
    rt_log("mods: merge %s", mods);
    G.mproc = SDL_CreateProcess(args, true);
    if (!G.mproc) {
        merge_finished(-1, "", SDL_GetError());
        return;
    }
    SDL_DetachThread(SDL_CreateThread(merge_reader, "mods-merge", nullptr));
}

int mods_merge_running(void) {
    int r = 0;
    if (G.lock) SDL_LockMutex(G.lock);
    r = G.mrunning;
    if (G.lock) SDL_UnlockMutex(G.lock);
    return r;
}

void mods_merge_cancel(void) {
    if (!G.lock) return;
    SDL_LockMutex(G.lock);
    if (G.mproc) SDL_KillProcess(G.mproc, true);
    SDL_UnlockMutex(G.lock);
}

// conflicts.json is merge.py's machine output; the page reads it for the picker rows. Values never hold
// quotes (paths and mod-folder names), so a small scanner does instead of a JSON library.
static int json_string(const char **pp, const char *key, char *out, size_t cap) {
    char want[64];
    snprintf(want, sizeof want, "\"%s\"", key);
    const char *p = strstr(*pp, want);
    if (!p) return 0;
    p = strchr(p + strlen(want), ':');
    if (!p) return 0;
    p = strchr(p, '"');
    if (!p) return 0;
    p++;
    size_t n = 0;
    while (*p && *p != '"' && n + 1 < cap) out[n++] = *p++;
    out[n] = 0;
    *pp = p;
    return n > 0;
}

static void conflicts_load(void) {
    G.nconflicts = 0;
    char path[PLAT_PATH_MAX * 2], json[65536];
    snprintf(path, sizeof path, "%s/mods/.merged/conflicts.json", cfg_game_dir());
    PlatFile *f = plat_fs_open(path, 0, NULL);
    if (!f) return;
    int64_t n = plat_file_pread(f, json, sizeof json - 1, 0);
    plat_file_close(f);
    if (n <= 0) return;
    json[n] = 0;
    const char *p = json;
    while (G.nconflicts < 64) {
        ModConflict *c = &G.conflicts[G.nconflicts];
        memset(c, 0, sizeof *c);
        const char *q = p;
        if (!json_string(&q, "archive", c->archive, sizeof c->archive)) break;
        if (!json_string(&q, "inner", c->inner, sizeof c->inner)) break;
        if (!json_string(&q, "winner", c->winner, sizeof c->winner)) break;
        c->resolved = strstr(q, "\"resolved\": true") != NULL;
        // the mods array of this object: names until the closing bracket
        const char *m = strstr(p, "\"mods\"");
        if (!m) break;
        m = strchr(m, '[');
        if (!m) break;
        m++;
        char name[MOD_NAME_MAX];
        while (c->nmods < 8) {
            while (*m == ' ' || *m == '\t' || *m == '\n' || *m == ',') m++;
            if (*m == ']') break;
            if (*m != '"') break;
            m++;
            size_t k = 0;
            while (*m && *m != '"' && k + 1 < sizeof name) name[k++] = *m++;
            name[k] = 0;
            if (*m == '"') m++;
            if (k) snprintf(c->mods[c->nmods++], sizeof c->mods[0], "%s", name);
            else break;
        }
        G.nconflicts++;
        p = q;
    }
    rt_log("mods: conflicts file %s (%d bytes)", path, (int)n);
}

int mods_conflict_count(void) { return G.nconflicts; }

int mods_conflict_unresolved(void) {
    int n = 0;
    for (int i = 0; i < G.nconflicts; i++) n += !G.conflicts[i].resolved;
    return n;
}
const ModConflict *mods_conflict_at(int i) { return i >= 0 && i < G.nconflicts ? &G.conflicts[i] : NULL; }



// Winner picker: mods/choices.json is a flat list of picks
// ([{archive, inner, winner, mods}]) so one mod's entries can be dropped when it is deleted. A pick
// keeps the names of the mods it was made between: if the set changes, the conflict asks again.
// Rewritten from the conflicts list with one change, then re-merged.
static void rewrite_choices(const char *archive, const char *inner, const char *winner, const char *drop_mod) {
    char cpath[PLAT_PATH_MAX * 2], out[8192];
    snprintf(cpath, sizeof cpath, "%s/mods/choices.json", cfg_game_dir());
    int n = snprintf(out, sizeof out, "[\n");
    int first = 1;
    for (int i = 0; i < G.nconflicts && n < (int)sizeof out - 192; i++) {
        const ModConflict *c = &G.conflicts[i];
        const char *win = c->winner;
        if (archive && !strcmp(c->archive, archive) && !strcmp(c->inner, inner)) win = winner;
        if (drop_mod) {   // the deleted mod's picks go with it, so re-adding it asks again
            int mine = !strcmp(win, drop_mod);
            for (int k = 0; k < c->nmods && !mine; k++) mine = !strcmp(c->mods[k], drop_mod);
            if (mine) continue;
        }
        n += snprintf(out + n, sizeof out - (size_t)n, "%s  {\"archive\": \"%s\", \"inner\": \"%s\", \"winner\": \"%s\", \"mods\": [",
                      first ? "" : ",\n", c->archive, c->inner, win);
        for (int k = 0; k < c->nmods && n < (int)sizeof out - 64; k++)
            n += snprintf(out + n, sizeof out - (size_t)n, "%s\"%s\"", k ? ", " : "", c->mods[k]);
        n += snprintf(out + n, sizeof out - (size_t)n, "]}");
        first = 0;
    }
    n += snprintf(out + n, sizeof out - (size_t)n, "\n]\n");
    int err = 0;
    // plat_fs_open is O_RDWR without O_CREAT: the first pick would be dropped while choices.json
    // does not exist yet, and the re-merge would put the default winner back
    PlatFile *f = plat_fs_open(cpath, 1, &err);
    if (!f && err == PLAT_FS_NOT_FOUND) f = plat_fs_create(cpath, &err);
    if (!f) rt_log("mods: cannot write %s (%d)", cpath, err);
    if (f) {
        if (plat_file_set_size(f, 0) == PLAT_FS_OK)
            plat_file_pwrite(f, out, (uint32_t)strlen(out), 0);
        plat_file_fsync(f);
        plat_file_close(f);
    }
}

void mods_conflict_pick(const char *archive, const char *inner) {
    for (int i = 0; i < G.nconflicts; i++) {
        const ModConflict *c = &G.conflicts[i];
        if (strcmp(c->archive, archive) || strcmp(c->inner, inner)) continue;
        int k = 0;   // cycle: mods in order, then vanilla, then back to the first (test hook path)
        while (k < c->nmods && strcmp(c->mods[k], c->winner)) k++;
        const char *win;
        if (k < c->nmods - 1) win = c->mods[k + 1];
        else if (k == c->nmods - 1) win = "vanilla";
        else win = c->nmods ? c->mods[0] : "vanilla";
        rewrite_choices(archive, inner, win, nullptr);
        mods_merge_start(G.exe_dir);
        return;
    }
}

// OK in the modal: save what the rows show, so the default winner becomes an explicit pick too.
void mods_conflict_commit(void) {
    rewrite_choices(nullptr, nullptr, nullptr, nullptr);
    mods_merge_start(G.exe_dir);
}

void mods_conflict_set(const char *archive, const char *inner, const char *winner) {
    rewrite_choices(archive, inner, winner, nullptr);
    mods_merge_start(G.exe_dir);
}

// Blocking merge for the Play button: waits out a running merge, then merges when stale (a no-op
// when fresh, so Play usually waits only for the Python startup). A failed merge still plays.
void mods_merge_sync(const char *exe_dir) {
    mods_poll();   // an import that ended off the page: it enables (and starts a merge) before the wait below
    while (mods_merge_running()) SDL_Delay(50);
    char python[1200], script[1200];
    if (!setup_tool(exe_dir, "mod/merge.py", python, script, sizeof python)) {
        rt_log("mods: merge skipped (no setup tools)");
        return;
    }
    char romfs[PLAT_PATH_MAX * 2], mods[4096], choices[PLAT_PATH_MAX * 2];
    romfs_root(romfs, sizeof romfs);
    const char *list = cfg_get("MKW_MODS");
    snprintf(mods, sizeof mods, "%s", list ? list : "");
    snprintf(choices, sizeof choices, "%s/mods/choices.json", cfg_game_dir());
    const char *args[] = {python, "-I", script, "--game", cfg_game_dir(), "--romfs", romfs,
                          "--enable", mods, "--choices", choices, nullptr};
    rt_log("mods: merge at Play %s", mods);
    SDL_Process *proc = SDL_CreateProcess(args, false);   // output inherits ours: the log shows it
    if (!proc) {
        rt_log("mods: merge at Play failed to start: %s", SDL_GetError());
        return;
    }
    int code = -1;
    SDL_WaitProcess(proc, true, &code);
    SDL_DestroyProcess(proc);
    rt_log("mods: merge at Play ended with code %d", code);
    mods_refresh(exe_dir);
}
