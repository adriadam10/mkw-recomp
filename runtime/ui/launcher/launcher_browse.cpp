// The start screen's own file browser (launcher_browse.h). One list per folder: up (..), the folders, then the files
// the request takes; A (Enter, a click) opens a folder or picks a file, B (Backspace) goes up a folder and at the top
// cancels, Escape cancels. A row of places above the list jumps to the home folder, removable media (SteamOS and
// Bazzite mount an SD card or a USB drive under /run/media) and the root; on Windows each drive. Hidden entries
// (a leading dot) are left out, as the system dialogs do by default.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "imgui.h"
#include "ui/launcher/launcher_browse.h"
#include "ui/launcher/launcher_look.h"

extern "C" {
#include "cfg.h"
#include "ui_text.h"
void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
}

#ifdef _WIN32
#define SEP "\\"
#else
#define SEP "/"
#endif

enum { NAME_MAX_ = 256, PATH_MAX_ = 1024, ENTRIES_MAX = 4096 };

typedef struct {
    char name[NAME_MAX_];
    bool dir;
} Entry;

static struct {
    int open, mode;
    char title[256], dir[PATH_MAX_], exts[64];
    SDL_DialogFileCallback cb;
    void *ud;
    Entry *ent;          // the listing of dir, ENTRIES_MAX at most
    int n, unreadable;   // its count; 1 when dir could not be listed
    int truncated;
    char focus[NAME_MAX_];   // the row to put the cursor on after a listing ("" the first)
    int focus_pending;
} B;

// A failed system dialog's request, taken over by the browser on the UI thread (SDL may answer on another thread)
static struct {
    int mode;
    char title[256], start[PATH_MAX_], exts[64], filter_name[128];
    SDL_DialogFileCallback cb;
    void *ud;
} g_req;
static SDL_DialogFileFilter g_filter;   // SDL reads it until the dialog answers
static SDL_AtomicInt g_fallback;   // 1: g_req is waiting for the UI thread
static SDL_AtomicInt g_waiting;    // 1: a system dialog is up with g_req as its request

static bool is_root(const char *p) {
#ifdef _WIN32
    return strlen(p) <= 3 && p[0] && p[1] == ':';   // "C:\"
#else
    return !strcmp(p, "/");
#endif
}

static void join(char *out, size_t cap, const char *dir, const char *name) {
    size_t n = strlen(dir);
    bool sep = n && (dir[n - 1] == '/' || dir[n - 1] == '\\');
    snprintf(out, cap, "%s%s%s", dir, sep ? "" : SEP, name);
}

static bool is_dir(const char *path) {
    SDL_PathInfo info;
    return SDL_GetPathInfo(path, &info) && info.type == SDL_PATHTYPE_DIRECTORY;
}

// name's extension is one of B.exts ("nup;nx"), case aside; any file without a filter
static bool wanted(const char *name) {
    if (!B.exts[0] || !strcmp(B.exts, "*")) return true;
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    size_t n = strlen(dot + 1);
    for (const char *p = B.exts; *p;) {
        const char *end = strchr(p, ';');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len == n && !SDL_strncasecmp(p, dot + 1, n)) return true;
        p += len + (end ? 1 : 0);
    }
    return false;
}

static SDL_EnumerationResult SDLCALL list_one(void *, const char *dirname, const char *fname) {
    if (fname[0] == '.') return SDL_ENUM_CONTINUE;
    if (B.n >= ENTRIES_MAX) {
        B.truncated = 1;
        return SDL_ENUM_SUCCESS;
    }
    char full[PATH_MAX_ + NAME_MAX_];
    join(full, sizeof full, dirname, fname);
    bool dir = is_dir(full);   // through a link too: a linked folder is a folder to open
    if (!dir && (B.mode == BROWSE_FOLDER || !wanted(fname))) return SDL_ENUM_CONTINUE;
    Entry *e = &B.ent[B.n++];
    snprintf(e->name, sizeof e->name, "%s", fname);
    e->dir = dir;
    return SDL_ENUM_CONTINUE;
}

static int by_kind_name(const void *a, const void *b) {
    const Entry *x = (const Entry *)a, *y = (const Entry *)b;
    if (x->dir != y->dir) return x->dir ? -1 : 1;
    return SDL_strcasecmp(x->name, y->name);
}

// B.dir's rows, the cursor on focus ("" the first row)
static void list(const char *focus) {
    rt_log("launcher: browse: in %s", B.dir);   // one line a folder the player opens: what a report needs
    if (!B.ent) B.ent = (Entry *)malloc(sizeof(Entry) * ENTRIES_MAX);
    B.n = B.truncated = 0;
    B.unreadable = !B.ent || !SDL_EnumerateDirectory(B.dir, list_one, nullptr);
    if (B.unreadable) rt_log("launcher: browse: %s cannot be listed: %s", B.dir, SDL_GetError());
    if (B.n > 1) qsort(B.ent, (size_t)B.n, sizeof *B.ent, by_kind_name);
    snprintf(B.focus, sizeof B.focus, "%s", focus ? focus : "");
    B.focus_pending = 1;
}

static void go(const char *dir) {
    snprintf(B.dir, sizeof B.dir, "%s", dir);
    list(nullptr);
}

// One folder up, the cursor on the folder left
static void up(void) {
    if (is_root(B.dir)) return;
    char from[NAME_MAX_] = "";
    size_t n = strlen(B.dir);
    while (n > 1 && (B.dir[n - 1] == '/' || B.dir[n - 1] == '\\')) B.dir[--n] = 0;
    char *sep = nullptr;
    for (char *p = B.dir; *p; p++)
        if (*p == '/' || *p == '\\') sep = p;
    if (!sep) return;
    snprintf(from, sizeof from, "%s", sep + 1);
    if (sep == B.dir) sep[1] = 0;   // "/x" -> "/"
#ifdef _WIN32
    else if (sep == B.dir + 2 && B.dir[1] == ':') sep[1] = 0;   // "C:\x" -> "C:\"
#endif
    else *sep = 0;
    list(from);
}

static void home(char *out, size_t cap) {
    const char *h = SDL_GetUserFolder(SDL_FOLDER_HOME);
    snprintf(out, cap, "%s", h && *h ? h : "/");
    size_t n = strlen(out);   // SDL ends it with a separator; the browser's paths do not
    while (n > 1 && (out[n - 1] == '/' || out[n - 1] == '\\') && !is_root(out)) out[--n] = 0;
}

static void finish(const char *path) {
    rt_log("launcher: browse: %s", path ? path : "cancelled");
    B.open = 0;
    const char *list[2] = {path, nullptr};
    if (B.cb) B.cb(B.ud, list, -1);
}

static void open_at(int mode, const char *title, const char *start, const char *exts, SDL_DialogFileCallback cb,
                    void *ud) {
    B.mode = mode, B.cb = cb, B.ud = ud;
    snprintf(B.title, sizeof B.title, "%s", title ? title : "");
    snprintf(B.exts, sizeof B.exts, "%s", exts ? exts : "");
    if (start && *start && is_dir(start)) snprintf(B.dir, sizeof B.dir, "%s", start);
    else home(B.dir, sizeof B.dir);
    B.open = 1;
    rt_log("launcher: browse: the start screen's own %s browser", mode == BROWSE_FOLDER ? "folder" : "file");
    list(nullptr);
}

// SDL's answer: an error (no portal, no zenity) hands the request to the browser; a pick or a cancel goes on as is
static void SDLCALL relay(void *, const char *const *files, int filter) {
    SDL_SetAtomicInt(&g_waiting, 0);
    if (!files) {
        rt_log("launcher: the system's file dialog did not open (%s): the start screen's own browser instead",
               SDL_GetError());
        SDL_SetAtomicInt(&g_fallback, 1);
        return;
    }
    g_req.cb(g_req.ud, files, filter);
}

// MKW_LAUNCHER_BROWSER=1 (0) always (never) takes the start screen's own; MKW_LAUNCHER_BROWSE, its test hook, too
static bool own_browser(void) {
    const char *e = cfg_get("MKW_LAUNCHER_BROWSER");   // the environment or mkw.ini
    if (e && *e) return strcmp(e, "0") != 0;
    return cfg_steam_console() != 0 || getenv("MKW_LAUNCHER_BROWSE") != nullptr;
}

void browse_dialog(SDL_Window *win, int mode, const char *title, const char *start, const char *filter_name,
                   const char *exts, SDL_DialogFileCallback cb, void *ud) {
    if (B.open || SDL_GetAtomicInt(&g_waiting) || SDL_GetAtomicInt(&g_fallback)) return;   // one at a time
    if (own_browser()) {
        open_at(mode, title, start, exts, cb, ud);
        return;
    }
    g_req.mode = mode, g_req.cb = cb, g_req.ud = ud;
    snprintf(g_req.title, sizeof g_req.title, "%s", title ? title : "");
    snprintf(g_req.start, sizeof g_req.start, "%s", start ? start : "");
    snprintf(g_req.exts, sizeof g_req.exts, "%s", exts ? exts : "");
    snprintf(g_req.filter_name, sizeof g_req.filter_name, "%s", filter_name ? filter_name : "");
    g_filter = {g_req.filter_name, g_req.exts};
    SDL_SetAtomicInt(&g_waiting, 1);
    if (mode == BROWSE_FOLDER) {
        SDL_ShowOpenFolderDialog(relay, nullptr, win, start, false);
    } else {
        bool filtered = g_req.exts[0] != 0;
        SDL_ShowOpenFileDialog(relay, nullptr, win, filtered ? &g_filter : nullptr, filtered ? 1 : 0, start, false);
    }
}

int browse_active(void) {
    if (SDL_CompareAndSwapAtomicInt(&g_fallback, 1, 0))
        open_at(g_req.mode, g_req.title, g_req.start, g_req.exts, g_req.cb, g_req.ud);
    return B.open;
}

// The places row: home, removable media and the root (Linux), or home and each drive (Windows)
static void places(float row_h) {
    char p[PATH_MAX_];
    home(p, sizeof p);
    int k = 0;
    auto place = [&](const char *path) {
        if (!is_dir(path)) return;
        if (k++) ImGui::SameLine();
        ImGui::PushID(path);
        bool here = !strcmp(path, B.dir);
        if (here) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button(path, ImVec2(0, row_h))) go(path);
        if (here) ImGui::PopStyleColor();
        ImGui::PopID();
    };
    place(p);
#ifdef _WIN32
    for (char d = 'A'; d <= 'Z'; d++) {
        char drive[4] = {d, ':', '\\', 0};
        if (SDL_GetPathInfo(drive, nullptr)) place(drive);
    }
#else
    place("/run/media");
    place("/media");
    place("/mnt");
    place("/");
#endif
}

void browse_draw(float w, float h, float scale) {
    if (!B.open) return;
    float pad = 24 * scale, row_h = ImGui::GetFrameHeight() * 1.25f;
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(w, h));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
    ImGui::Begin("##browse", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(LOOK_HEADING), "%s", B.title);
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(B.dir);
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    places(row_h);
    ImGui::Spacing();
    float bottom = row_h + ImGui::GetStyle().ItemSpacing.y * 2 + ImGui::GetStyle().WindowPadding.y;
    ImGui::BeginChild("##browse_list", ImVec2(0, -bottom), ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened);
    const char *pick = nullptr;
    char picked[PATH_MAX_ + NAME_MAX_];
    int enter = -1;   // the folder row chosen: opened after the list is drawn
    bool focus = B.focus_pending;
    B.focus_pending = 0;
    if (!is_root(B.dir)) {
        if (focus && !B.focus[0]) ImGui::SetKeyboardFocusHere();
        if (ImGui::Selectable("..", false, 0, ImVec2(0, row_h))) enter = -2;
        if (focus && !B.focus[0]) focus = false;
    }
    for (int i = 0; i < B.n; i++) {
        const Entry &e = B.ent[i];
        char label[NAME_MAX_ + 16];
        snprintf(label, sizeof label, "%s%s##%d", e.name, e.dir ? "/" : "", i);
        bool here = focus && (!B.focus[0] || !strcmp(B.focus, e.name));
        if (here) ImGui::SetKeyboardFocusHere(), focus = false;
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(e.dir ? LOOK_TEXT : LOOK_HEADING));
        bool hit = ImGui::Selectable(label, false, 0, ImVec2(0, row_h));
        ImGui::PopStyleColor();
        if (here) ImGui::SetScrollHereY(0.4f);
        if (!hit) continue;
        if (e.dir) enter = i;
        else join(picked, sizeof picked, B.dir, e.name), pick = picked;
    }
    if (B.unreadable || !B.n)
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(LOOK_DIM), "%s",
                           menu_text(B.unreadable ? UI_BROWSE_UNREADABLE : UI_BROWSE_EMPTY));
    if (B.truncated) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(LOOK_DIM), "...");
    ImGui::EndChild();
    bool cancel = false;
    ImGui::Spacing();
    if (B.mode == BROWSE_FOLDER) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(LOOK_ACCENT));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::ColorConvertU32ToFloat4(LOOK_ACCENT_HOT));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::ColorConvertU32ToFloat4(LOOK_ACCENT_DOWN));
        if (ImGui::Button(menu_text(UI_BROWSE_USE), ImVec2(0, row_h))) snprintf(picked, sizeof picked, "%s", B.dir),
                                                                        pick = picked;
        ImGui::PopStyleColor(3);
        ImGui::SameLine();
    }
    if (ImGui::Button(menu_text(UI_LAUNCH_CANCEL), ImVec2(0, row_h))) cancel = true;
    // B and Backspace go up a folder (at the top they cancel), Escape cancels; not while a text field has the keys
    bool typing = ImGui::GetIO().WantTextInput;
    bool back = !typing && (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) ||
                            ImGui::IsKeyPressed(ImGuiKey_Backspace, false));
    if (!typing && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) cancel = true;
    ImGui::End();
    if (pick) finish(pick);
    else if (cancel) finish(nullptr);
    else if (enter == -2) up();
    else if (enter >= 0) {
        char to[PATH_MAX_ + NAME_MAX_];
        join(to, sizeof to, B.dir, B.ent[enter].name);
        go(to);
    } else if (back) {
        if (is_root(B.dir)) finish(nullptr);
        else up();
    }
}
