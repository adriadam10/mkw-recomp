// The Mods page's GameBanana tab: Mario Kart World mods listed and searched on GameBanana, a mod's page (author,
// license, description, files) and its install. The network is tools/mod/gamebanana.py in the package's Python, one
// child process at a time whose tab-separated lines a thread of ours reads, as mods.cpp runs import.py. An install
// downloads the file into <game>/mods/.downloads and hands it to the normal import (mods_pick, mods_import_start).
// Nothing is hosted by this project: the mods are their authors', fetched from GameBanana when the player asks. The
// preview pictures too: a second child process (gamebanana.py images) fetches them into the player's cache as the
// page shows them, and they are decoded here (stb_image) into textures of the start screen's renderer.
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "imgui.h"
#include "ui/launcher/launcher_setup.h"
#include "ui/launcher/mods.h"
#include "ui/launcher/mods_browse.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "../../third_party/stb/stb_image.h"

extern "C" {
#include "cfg.h"
#include "platform/platform.h"
#include "ui_text.h"
void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
}

enum { JOB_NONE, JOB_LIST, JOB_INFO, JOB_DOWNLOAD };
enum { MAX_ROWS = 40, MAX_FILES = 16, MAX_SHOTS = 6, MAX_PICS = 160, INFO_CACHE = 24 };

struct Row {
    int id, likes, views;
    char name[128], author[64], category[48], url[128], thumb[200];
};
struct File {
    int id, size, clean;
    char name[128], note[200];
};
struct Info {
    int id;
    char name[128], author[64], license[160], url[128], version[32], text[4096];
    File files[MAX_FILES];
    int nfiles;
    char shots[MAX_SHOTS][200], full[MAX_SHOTS][200];   // 530 pixels wide, and the original for the enlarged view
    int nshots;
};

// A preview picture: wanted (0), asked for (1), on disk (2), shown (3, tex), failed (4).
struct Pic {
    char url[200], path[PLAT_PATH_MAX];
    int state, w, h;
    SDL_Texture *tex;
};
static struct {
    SDL_Process *proc;
    int running;
    Pic pics[MAX_PICS];
    int n;
} P = {};
static Info g_info_cache[INFO_CACHE];   // the pages seen, newest last: a second click shows one at once
static int g_info_n;

static struct {
    SDL_Mutex *lock;
    SDL_Process *proc;
    int job, running;
    // what the thread fills (under lock); the UI copies it out when the job ends
    Row rows[MAX_ROWS];
    int nrows, page, total;
    Info info;
    long long dl_done, dl_size;
    char dl_path[PLAT_PATH_MAX], error[300];
    int finished;   // the last job ended: 1 ok, -1 failed (error)
} G = {};

static char g_query[128], g_last_query[128];
static int g_open_mod, g_open_file;   // a 1-Click link's mod and file (mods_browse_open): its page, then the question

void mods_browse_open(int mod_id, int file_id) { g_open_mod = mod_id, g_open_file = file_id; }
static int g_sort, g_page = 1, g_loaded, g_selected = -1;

static void unescape(char *s) {   // gamebanana.py writes a text's line breaks as \n
    char *w = s;
    for (const char *r = s; *r; r++) {
        if (r[0] == '\\' && r[1] == 'n') *w++ = '\n', r++;
        else *w++ = *r;
    }
    *w = 0;
}

// one tab-separated event line of gamebanana.py (the thread, under lock)
static void on_line(char *line) {
    char *f[9] = {};
    int n = 0;
    for (char *p = line; n < 9;) {
        f[n++] = p;
        char *t = strchr(p, '\t');
        if (!t) break;
        *t = 0, p = t + 1;
    }
    if (!strcmp(f[0], "mod") && n >= 8 && G.nrows < MAX_ROWS) {
        Row &r = G.rows[G.nrows++];
        r.id = atoi(f[1]), r.likes = atoi(f[4]), r.views = atoi(f[5]);
        snprintf(r.name, sizeof r.name, "%s", f[2]);
        snprintf(r.author, sizeof r.author, "%s", f[3]);
        snprintf(r.category, sizeof r.category, "%s", f[6]);
        snprintf(r.url, sizeof r.url, "%s", f[7]);
        snprintf(r.thumb, sizeof r.thumb, "%s", n >= 9 ? f[8] : "");
    } else if (!strcmp(f[0], "page") && n >= 3) {
        G.page = atoi(f[1]), G.total = atoi(f[2]);
    } else if (!strcmp(f[0], "info") && n >= 7) {
        G.info.id = atoi(f[1]);
        snprintf(G.info.name, sizeof G.info.name, "%s", f[2]);
        snprintf(G.info.author, sizeof G.info.author, "%s", f[3]);
        snprintf(G.info.license, sizeof G.info.license, "%s", f[4]);
        snprintf(G.info.url, sizeof G.info.url, "%s", f[5]);
        snprintf(G.info.version, sizeof G.info.version, "%s", f[6]);
    } else if (!strcmp(f[0], "text") && n >= 2) {
        snprintf(G.info.text, sizeof G.info.text, "%s", f[1]);
        unescape(G.info.text);
    } else if (!strcmp(f[0], "file") && n >= 6 && G.info.nfiles < MAX_FILES) {
        File &x = G.info.files[G.info.nfiles++];
        x.id = atoi(f[1]), x.size = atoi(f[3]), x.clean = !strcmp(f[4], "clean");
        snprintf(x.name, sizeof x.name, "%s", f[2]);
        snprintf(x.note, sizeof x.note, "%s", f[5]);
    } else if (!strcmp(f[0], "image") && n >= 3 && G.info.nshots < MAX_SHOTS) {
        snprintf(G.info.full[G.info.nshots], sizeof G.info.full[0], "%s", f[2]);
        snprintf(G.info.shots[G.info.nshots++], sizeof G.info.shots[0], "%s", f[1]);
    } else if (!strcmp(f[0], "progress") && n >= 3) {
        G.dl_done = atoll(f[1]), G.dl_size = atoll(f[2]);
    } else if (!strcmp(f[0], "done") && n >= 2) {
        snprintf(G.dl_path, sizeof G.dl_path, "%s", f[1]);
    } else if (!strcmp(f[0], "error")) {
        snprintf(G.error, sizeof G.error, "%s%s%s", n >= 2 ? f[1] : "", n >= 3 ? ": " : "", n >= 3 ? f[2] : "");
    }
}

static int SDLCALL reader(void *) {
    SDL_IOStream *out = SDL_GetProcessOutput(G.proc);
    char line[8192];
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
        line[n] = 0, n = 0;
        SDL_LockMutex(G.lock);
        on_line(line);
        SDL_UnlockMutex(G.lock);
    }
    int code = -1;
    SDL_WaitProcess(G.proc, true, &code);
    SDL_DestroyProcess(G.proc);
    SDL_LockMutex(G.lock);
    G.proc = nullptr;
    G.running = 0;
    G.finished = code == 0 ? 1 : -1;
    if (code && !G.error[0]) snprintf(G.error, sizeof G.error, "exit %d", code);
    SDL_UnlockMutex(G.lock);
    return 0;
}

// gamebanana.py with args after --cache (one job at a time; a new one is refused while one runs)
static void start(const char *exe_dir, int job, const char *const *rest) {
    if (!G.lock) G.lock = SDL_CreateMutex();
    if (G.running) return;
    char python[1200], script[1200], cache[PLAT_PATH_MAX];
    if (!setup_tool(exe_dir, "mod/gamebanana.py", python, script, sizeof python)) {
        snprintf(G.error, sizeof G.error, "no setup tools");
        G.finished = -1;
        return;
    }
    snprintf(cache, sizeof cache, "%s/mods/.gamebanana", cfg_game_dir());
    const char *args[16] = {python, "-I", script, "--cache", cache};
    int na = 5;
    for (; rest[na - 5] && na < 15; na++) args[na] = rest[na - 5];
    args[na] = nullptr;
    SDL_LockMutex(G.lock);
    G.job = job, G.running = 1, G.finished = 0, G.error[0] = 0;
    if (job == JOB_LIST) G.nrows = 0, G.total = 0;
    if (job == JOB_INFO) G.info = Info{};
    if (job == JOB_DOWNLOAD) G.dl_done = G.dl_size = 0, G.dl_path[0] = 0;
    SDL_UnlockMutex(G.lock);
    G.proc = SDL_CreateProcess(args, true);
    if (!G.proc) {
        SDL_LockMutex(G.lock);
        G.running = 0, G.finished = -1;
        snprintf(G.error, sizeof G.error, "%s", SDL_GetError());
        SDL_UnlockMutex(G.lock);
        return;
    }
    SDL_DetachThread(SDL_CreateThread(reader, "mods-gamebanana", nullptr));
}

// ---- preview pictures: one batch process at a time, its "image URL PATH" lines read by this thread
static int SDLCALL pic_reader(void *) {
    SDL_IOStream *out = SDL_GetProcessOutput(P.proc);
    char line[PLAT_PATH_MAX + 256];
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
        line[n] = 0, n = 0;
        char *url = strchr(line, '\t'), *path = url ? strchr(url + 1, '\t') : nullptr;
        if (strncmp(line, "image\t", 6) || !path) continue;
        *path++ = 0, url++;
        SDL_LockMutex(G.lock);
        for (int i = 0; i < P.n; i++)
            if (P.pics[i].state == 1 && !strcmp(P.pics[i].url, url))
                snprintf(P.pics[i].path, sizeof P.pics[i].path, "%s", path), P.pics[i].state = 2;
        SDL_UnlockMutex(G.lock);
    }
    SDL_WaitProcess(P.proc, true, nullptr);
    SDL_DestroyProcess(P.proc);
    SDL_LockMutex(G.lock);
    P.proc = nullptr, P.running = 0;
    for (int i = 0; i < P.n; i++)
        if (P.pics[i].state == 1) P.pics[i].state = 4;   // asked for and not come: not shown, not asked again
    SDL_UnlockMutex(G.lock);
    return 0;
}

// The picture of url (UI thread): its texture once loaded, else nullptr and it is asked for.
static SDL_Texture *pic(const char *url, int *w, int *h) {
    if (!url[0]) return nullptr;
    SDL_LockMutex(G.lock);
    Pic *p = nullptr;
    for (int i = 0; i < P.n && !p; i++)
        if (!strcmp(P.pics[i].url, url)) p = &P.pics[i];
    if (!p && P.n < MAX_PICS) {
        p = &P.pics[P.n++];
        *p = Pic{};
        snprintf(p->url, sizeof p->url, "%s", url);
    }
    SDL_UnlockMutex(G.lock);
    if (!p || !p->tex) return nullptr;
    *w = p->w, *h = p->h;
    return p->tex;
}

// Once a frame (UI thread): pictures on disk become textures; the wanted ones go to a new batch.
static void pic_tick(const char *exe_dir, SDL_Renderer *ren) {
    for (int i = 0; i < P.n; i++) {
        Pic *p = &P.pics[i];
        if (p->state != 2) continue;
        p->state = 4;
        uint64_t n = 0;
        uint8_t *data = (uint8_t *)plat_file_read(p->path, &n);
        int w, h, comp;
        unsigned char *px = data ? stbi_load_from_memory(data, (int)n, &w, &h, &comp, 4) : nullptr;
        free(data);
        if (!px) continue;
        SDL_Surface *s = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGBA32, px, w * 4);
        p->tex = s && ren ? SDL_CreateTextureFromSurface(ren, s) : nullptr;
        SDL_DestroySurface(s);
        stbi_image_free(px);
        if (p->tex) p->state = 3, p->w = w, p->h = h;
    }
    SDL_LockMutex(G.lock);
    int idle = !P.running;
    SDL_UnlockMutex(G.lock);
    if (!idle) return;
    char python[1200], script[1200], cache[PLAT_PATH_MAX];
    if (!setup_tool(exe_dir, "mod/gamebanana.py", python, script, sizeof python)) return;
    snprintf(cache, sizeof cache, "%s/mods/.gamebanana", cfg_game_dir());
    const char *args[32] = {python, "-I", script, "--cache", cache, "images"};
    int na = 6;
    SDL_LockMutex(G.lock);
    for (int i = 0; i < P.n && na < 30; i++)
        if (P.pics[i].state == 0) args[na++] = P.pics[i].url, P.pics[i].state = 1;
    SDL_UnlockMutex(G.lock);
    if (na == 6) return;
    args[na] = nullptr;
    P.running = 1;
    P.proc = SDL_CreateProcess(args, true);
    if (!P.proc) {
        P.running = 0;
        return;
    }
    SDL_DetachThread(SDL_CreateThread(pic_reader, "mods-pictures", nullptr));
}

static void list_start(const char *exe_dir) {
    static const char *const kSort[] = {"downloads", "new", "likes"};
    char page[16];
    snprintf(page, sizeof page, "%d", g_page);
    snprintf(g_last_query, sizeof g_last_query, "%s", g_query);
    if (g_query[0]) {
        const char *rest[] = {"search", g_query, "--page", page, nullptr};
        start(exe_dir, JOB_LIST, rest);
    } else {
        const char *rest[] = {"browse", "--page", page, "--sort", kSort[g_sort], nullptr};
        start(exe_dir, JOB_LIST, rest);
    }
    g_selected = -1;
}

static void size_text(int bytes, char *out, size_t cap) {
    if (bytes >= 1 << 20) snprintf(out, cap, "%.1f MB", bytes / 1048576.0);
    else snprintf(out, cap, "%d KB", (bytes + 1023) / 1024);
}

static const Info *info_cached(int id) {
    for (int i = 0; i < g_info_n; i++)
        if (g_info_cache[i].id == id) return &g_info_cache[i];
    return nullptr;
}

static void info_keep(const Info &in) {
    if (!in.id || info_cached(in.id)) return;
    if (g_info_n == INFO_CACHE) memmove(g_info_cache, g_info_cache + 1, sizeof g_info_cache[0] * --g_info_n);
    g_info_cache[g_info_n++] = in;
}

// a picture scaled to fit a w x h box, or a blank of that size until it comes; true when clicked
static bool picture(const char *url, float w, float h) {
    int pw = 0, ph = 0;
    SDL_Texture *t = pic(url, &pw, &ph);
    if (!t) {
        ImGui::Dummy(ImVec2(w, h));
        return false;
    }
    float s = w / pw < h / ph ? w / pw : h / ph;
    ImGui::Image((ImTextureID)(intptr_t)t, ImVec2(pw * s, ph * s));
    return ImGui::IsItemClicked();
}

// The enlarged pictures of one mod (the original size, scaled down to the window): < and > (or the arrow keys) go
// through them, the X button or Esc closes.
static struct {
    char small[MAX_SHOTS][200], full[MAX_SHOTS][200];
    int n, at;   // at -1: closed
} Z = {{}, {}, 0, -1};

static void zoom_open(const Info &in, int at) {
    Z.n = in.nshots, Z.at = at;
    memcpy(Z.small, in.shots, sizeof Z.small);
    memcpy(Z.full, in.full, sizeof Z.full);
}

static void zoom_draw(void) {
    if (Z.at < 0) return;
    ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::SetNextWindowFocus();   // above the start screen's own windows while it is open
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0.88f));
    ImGui::Begin("##gbzoom", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                          ImGuiWindowFlags_NoSavedSettings);
    float bw = ImGui::GetFrameHeight() * 1.6f;   // the side and close buttons
    float w = vp->Size.x - bw * 3, h = vp->Size.y * 0.90f;
    int pw = 0, ph = 0;
    SDL_Texture *t = pic(Z.full[Z.at], &pw, &ph);
    if (!t) t = pic(Z.small[Z.at], &pw, &ph);   // the small one until the original has come
    if (t) {
        float s = w / pw < h / ph ? w / pw : h / ph;
        ImGui::SetCursorPos(ImVec2((vp->Size.x - pw * s) / 2, (vp->Size.y - ph * s) / 2));
        ImGui::Image((ImTextureID)(intptr_t)t, ImVec2(pw * s, ph * s));
    }
    float mid = vp->Size.y / 2 - bw / 2;
    ImGui::BeginDisabled(Z.at == 0);
    ImGui::SetCursorPos(ImVec2(bw * 0.4f, mid));
    if (ImGui::Button("<", ImVec2(bw, bw)) || (Z.at > 0 && ImGui::IsKeyPressed(ImGuiKey_LeftArrow))) Z.at--;
    ImGui::EndDisabled();
    ImGui::BeginDisabled(Z.at >= Z.n - 1);
    ImGui::SetCursorPos(ImVec2(vp->Size.x - bw * 1.4f, mid));
    if (ImGui::Button(">", ImVec2(bw, bw)) || (Z.at < Z.n - 1 && ImGui::IsKeyPressed(ImGuiKey_RightArrow))) Z.at++;
    ImGui::EndDisabled();
    ImGui::SetCursorPos(ImVec2(vp->Size.x - bw * 1.4f, bw * 0.4f));
    if (ImGui::Button("X", ImVec2(bw, bw)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) Z.at = -1;
    char count[16];
    snprintf(count, sizeof count, "%d / %d", Z.at + 1, Z.n);
    ImGui::SetCursorPos(ImVec2(vp->Size.x / 2 - ImGui::CalcTextSize(count).x / 2, vp->Size.y - bw));
    if (Z.at >= 0) ImGui::TextUnformatted(count);
    ImGui::End();
    ImGui::PopStyleColor();
}

int mods_browse_draw(const char *exe_dir, SDL_Renderer *ren) {
    if (!G.lock) G.lock = SDL_CreateMutex();
    SDL_LockMutex(G.lock);
    int busy = G.running;
    SDL_UnlockMutex(G.lock);
    // the list once, after a 1-Click link's page has come (its file cleared when its page is shown)
    if (!g_loaded && !g_open_mod && !g_open_file && !busy) {
        int keep = g_selected;   // a 1-Click page stays shown beside the list
        g_loaded = 1, list_start(exe_dir);
        g_selected = keep;
    }
    pic_tick(exe_dir, ren);
    SDL_LockMutex(G.lock);
    int running = G.running, job = G.job, finished = G.finished;
    char error[sizeof G.error], path[sizeof G.dl_path];
    snprintf(error, sizeof error, "%s", G.error);
    snprintf(path, sizeof path, "%s", G.dl_path);
    long long dl_done = G.dl_done, dl_size = G.dl_size;
    SDL_UnlockMutex(G.lock);

    // a finished download goes to the normal import, and the page shows it there
    if (job == JOB_DOWNLOAD && finished > 0 && path[0]) {
        SDL_LockMutex(G.lock);
        G.dl_path[0] = 0, G.finished = 0;
        SDL_UnlockMutex(G.lock);
        mods_pick(path, 1);
        mods_import_start(exe_dir);
        return 1;
    }

    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", menu_text(UI_GB_NOTE));
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.45f);
    bool go = ImGui::InputTextWithHint("##gbq", menu_text(UI_GB_SEARCH), g_query, sizeof g_query,
                                       ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    go |= ImGui::Button(menu_text(UI_GB_SEARCH));
    if (!g_query[0]) {   // no query: the whole list, in an order
        static const UiTextId kSorts[] = {UI_GB_SORT_DOWNLOADS, UI_GB_SORT_NEW, UI_GB_SORT_LIKES};
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
        if (ImGui::BeginCombo("##gbsort", menu_text(kSorts[g_sort]))) {
            for (int i = 0; i < 3; i++)
                if (ImGui::Selectable(menu_text(kSorts[i]), g_sort == i) && g_sort != i) g_sort = i, go = true;
            ImGui::EndCombo();
        }
    }
    if (go && !running) g_page = 1, list_start(exe_dir);

    SDL_LockMutex(G.lock);
    int nrows = job == JOB_LIST && running ? 0 : G.nrows, page = G.page, total = G.total;
    static Row rows[MAX_ROWS];
    memcpy(rows, G.rows, sizeof rows);
    // the page shown stays until the next one has come (no blank frame), and a page seen before comes at once
    static Info info;
    static int wanted, ask_file = -1;
    if (g_open_mod && !running) {   // a 1-Click link: its mod's page (not in the list), then the install asked
        wanted = g_open_mod, g_selected = -2, g_open_mod = 0;
        char id[16];
        snprintf(id, sizeof id, "%d", wanted);
        SDL_UnlockMutex(G.lock);
        const char *rest[] = {"info", id, nullptr};
        start(exe_dir, JOB_INFO, rest);
        SDL_LockMutex(G.lock);
    }
    if (job == JOB_INFO && !running && finished > 0 && G.info.id == wanted && info.id != wanted) {
        info = G.info;
        info_keep(info);
        if (g_open_file) {
            for (int i = 0; i < info.nfiles; i++)
                if (info.files[i].id == g_open_file && info.files[i].clean) ask_file = i;
            g_open_file = 0;
        }
    }
    SDL_UnlockMutex(G.lock);

    float h = ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 2;
    ImGui::BeginChild("##gblist", ImVec2(ImGui::GetContentRegionAvail().x * 0.42f, h), ImGuiChildFlags_Borders);
    if (job == JOB_LIST && running) ImGui::TextDisabled("%s", menu_text(UI_GB_LOADING));
    else if (!nrows && finished > 0) ImGui::TextDisabled("%s", menu_text(UI_GB_NONE));
    float rh = ImGui::GetTextLineHeight() * 2.4f;
    for (int i = 0; i < nrows; i++) {
        ImGui::PushID(i);
        char label[220];
        snprintf(label, sizeof label, "%s\n%s  ·  %s", rows[i].name, rows[i].author, rows[i].category);
        ImVec2 at = ImGui::GetCursorPos();
        bool hit = ImGui::Selectable("##row", g_selected == i, 0, ImVec2(0, rh));
        ImGui::SetCursorPos(at);
        picture(rows[i].thumb, rh * 1.66f, rh);
        ImGui::SameLine();
        ImGui::TextUnformatted(label);
        ImGui::SetCursorPos(ImVec2(at.x, at.y + rh + ImGui::GetStyle().ItemSpacing.y));
        if (hit && !(job == JOB_INFO && running)) {
            g_selected = i, wanted = rows[i].id;
            if (const Info *c = info_cached(wanted)) {
                info = *c;
            } else {
                char id[16];
                snprintf(id, sizeof id, "%d", rows[i].id);
                const char *rest[] = {"info", id, nullptr};
                start(exe_dir, JOB_INFO, rest);
            }
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##gbinfo", ImVec2(0, h), ImGuiChildFlags_Borders);
    static int confirm_file = -1;
    if (job == JOB_INFO && running && !info.id) {
        ImGui::TextDisabled("%s", menu_text(UI_GB_LOADING));
    } else if (info.id && g_selected != -1) {
        ImGui::TextUnformatted(info.name);
        if (job == JOB_INFO && running) {
            ImGui::SameLine();
            ImGui::TextDisabled("  %s", menu_text(UI_GB_LOADING));
        }
        char line[300];
        snprintf(line, sizeof line, menu_text(UI_GB_BY), info.author);
        ImGui::TextDisabled("%s", line);
        snprintf(line, sizeof line, menu_text(UI_GB_LICENSE), info.license);
        ImGui::TextDisabled("%s", line);
        if (ImGui::SmallButton(menu_text(UI_GB_OPEN))) SDL_OpenURL(info.url);
        // the author's screenshots, from GameBanana as the page is shown
        float sw = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2) / 3;
        for (int i = 0; i < info.nshots && i < 3; i++) {
            if (i) ImGui::SameLine();
            if (picture(info.shots[i], sw, sw * 0.6f)) zoom_open(info, i);
            if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }
        ImGui::Separator();
        for (int i = 0; i < info.nfiles; i++) {
            const File &f = info.files[i];
            char size[32], label[220];
            size_text(f.size, size, sizeof size);
            ImGui::PushID(i);
            snprintf(label, sizeof label, menu_text(UI_GB_INSTALL), f.name);
            ImGui::BeginDisabled(!f.clean || running);
            if (ImGui::Button(label) || ask_file == i) confirm_file = i, ask_file = -1, ImGui::OpenPopup("##gbconfirm");
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextDisabled("%s%s%s", size, f.note[0] ? "  ·  " : "", f.note);
            if (!f.clean) ImGui::TextDisabled("%s", menu_text(UI_GB_UNSAFE));
            if (ImGui::BeginPopupModal("##gbconfirm", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                const File &c = info.files[confirm_file >= 0 ? confirm_file : 0];
                char ask[400];
                snprintf(ask, sizeof ask, menu_text(UI_GB_CONFIRM), c.name, info.author);
                ImGui::TextUnformatted(ask);
                ImGui::Spacing();
                int may = mods_risk_draw();
                ImGui::BeginDisabled(!may);
                bool ok = ImGui::Button(menu_text(UI_MOD_OK));
                ImGui::EndDisabled();
                if (ok) {
                    char mid[16], fid[16], dest[PLAT_PATH_MAX];
                    snprintf(mid, sizeof mid, "%d", info.id);
                    snprintf(fid, sizeof fid, "%d", c.id);
                    snprintf(dest, sizeof dest, "%s/mods/.downloads", cfg_game_dir());
                    const char *rest[] = {"download", mid, fid, dest, nullptr};
                    start(exe_dir, JOB_DOWNLOAD, rest);
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button(menu_text(UI_LAUNCH_BACK))) ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
        ImGui::Separator();
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(info.text);
        ImGui::PopTextWrapPos();
    }
    ImGui::EndChild();

    // the status row: a download's progress, the last error, the page
    if (job == JOB_DOWNLOAD && running) {
        char over[64];
        snprintf(over, sizeof over, "%.1f / %.1f MB", dl_done / 1048576.0, dl_size / 1048576.0);
        ImGui::ProgressBar(dl_size ? (float)dl_done / (float)dl_size : 0.0f, ImVec2(-1, 0), over);
    } else if (finished < 0 && error[0]) {
        char msg[400];
        snprintf(msg, sizeof msg, menu_text(UI_GB_ERROR), error);
        ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.35f, 1), "%s", msg);
    } else if (total > 0) {
        int pages = (total + 19) / 20;   // gamebanana.py's PER_PAGE
        ImGui::BeginDisabled(running || page <= 1);
        if (ImGui::Button("<")) g_page = page - 1, list_start(exe_dir);
        ImGui::EndDisabled();
        ImGui::SameLine();
        char p[64];
        snprintf(p, sizeof p, menu_text(UI_GB_PAGE), page, pages);
        ImGui::TextUnformatted(p);
        ImGui::SameLine();
        ImGui::BeginDisabled(running || page >= pages);
        if (ImGui::Button(">")) g_page = page + 1, list_start(exe_dir);
        ImGui::EndDisabled();
    }
    zoom_draw();
    return 0;
}
