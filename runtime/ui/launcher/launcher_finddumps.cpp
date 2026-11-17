// The copies found (launcher_finddumps.h): tools/prepare/finddumps.py in the package's Python, its lines read by a
// thread of ours when it ends (it stops itself within a few seconds), as launcher_lastrun.cpp reads lastrun.py.
#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

#include "ui/launcher/launcher_finddumps.h"
#include "ui/launcher/launcher_setup.h"

extern "C" {
void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
}

static SDL_Mutex *g_lock;
static FoundDump g_found[FOUND_MAX];
static int g_n, g_running;
static SDL_Process *g_proc;

// "dump<TAB>kind<TAB>version<TAB>path"
static void on_line(char *line) {
    if (strncmp(line, "dump\t", 5)) return;
    char *kind = line + 5, *version = strchr(kind, '\t'), *path = version ? strchr(version + 1, '\t') : nullptr;
    if (!path) return;
    *version++ = 0, *path++ = 0;
    SDL_LockMutex(g_lock);
    if (g_n < FOUND_MAX) {
        FoundDump *d = &g_found[g_n++];
        snprintf(d->kind, sizeof d->kind, "%s", kind);
        snprintf(d->version, sizeof d->version, "%s", version);
        snprintf(d->path, sizeof d->path, "%s", path);
    }
    SDL_UnlockMutex(g_lock);
    rt_log("launcher: copy found: %s %s %s", kind, version, path);
}

static int SDLCALL reader(void *) {
    size_t n = 0;
    char *out = (char *)SDL_ReadProcess(g_proc, &n, nullptr);
    for (char *p = out; p && *p;) {
        char *e = p + strcspn(p, "\r\n"), c = *e;
        *e = 0;
        on_line(p);
        if (!c) break;
        p = e + 1;
    }
    SDL_free(out);
    SDL_DestroyProcess(g_proc);
    g_proc = nullptr;
    SDL_LockMutex(g_lock);
    g_running = 0;
    SDL_UnlockMutex(g_lock);
    return 0;
}

void finddumps_start(const char *exe_dir, const char *game_dir) {
    if (g_lock) return;   // once a start
    g_lock = SDL_CreateMutex();
    char python[1200], script[1200];
    if (!setup_tool(exe_dir, "prepare/finddumps.py", python, script, sizeof python)) return;
    const char *args[] = {python, "-I", script, "--game", game_dir && *game_dir ? game_dir : ".", nullptr};
    g_proc = SDL_CreateProcess(args, true);
    if (!g_proc) {
        rt_log("launcher: cannot look for the game's copies: %s", SDL_GetError());
        return;
    }
    g_running = 1;
    SDL_Thread *t = SDL_CreateThread(reader, "finddumps", nullptr);
    if (t) SDL_DetachThread(t);
    else g_running = 0;
}

int finddumps_list(FoundDump *out, int max, int *running) {
    if (!g_lock) {
        if (running) *running = 0;
        return 0;
    }
    SDL_LockMutex(g_lock);
    int n = g_n < max ? g_n : max;
    memcpy(out, g_found, (size_t)n * sizeof *out);
    if (running) *running = g_running;
    SDL_UnlockMutex(g_lock);
    return n;
}
