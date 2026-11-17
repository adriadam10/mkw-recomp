// Isolated settings and installation discovery for test_game_dir*.py. Single-thread platform stubs use real files;
// no game code, SDL window, GPU or guest address space is needed.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#endif

#include "cfg.h"
#include "gamelib.h"
#include "platform/platform.h"
#include "sysinfo.h"

static const char *exe;
static int fail_selector;

void rt_log(const char *fmt, ...) { (void)fmt; }
void rt_fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    exit(2);
}
int plat_exe_path(char *out, size_t cap) { return snprintf(out, cap, "%s", exe) < (int)cap; }
void plat_mutex_lock(PlatMutex *m) { (void)m; }
void plat_mutex_unlock(PlatMutex *m) { (void)m; }
#ifdef _WIN32
static int wide_path(const char *path, wchar_t *out) {
    return MultiByteToWideChar(CP_UTF8, 0, path, -1, out, PLAT_PATH_MAX * 3);
}
static FILE *open_file(const char *path, const char *mode) {
    wchar_t w[PLAT_PATH_MAX * 3], m[8];
    if (!wide_path(path, w) || !MultiByteToWideChar(CP_UTF8, 0, mode, -1, m, 8)) return NULL;
    return _wfopen(w, m);
}
int plat_file_exists(const char *path) {
    wchar_t w[PLAT_PATH_MAX * 3];
    if (!wide_path(path, w)) return 0;
    DWORD a = GetFileAttributesW(w);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
int64_t plat_file_size(const char *path) {
    wchar_t w[PLAT_PATH_MAX * 3];
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!wide_path(path, w) || !GetFileAttributesExW(w, GetFileExInfoStandard, &d)) return -1;
    return (int64_t)(((uint64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow);
}
#else
#define open_file fopen
int plat_file_exists(const char *path) { struct stat s; return !stat(path, &s) && S_ISREG(s.st_mode); }
int64_t plat_file_size(const char *path) { struct stat s; return !stat(path, &s) ? s.st_size : -1; }
#endif
uint8_t *plat_file_read(const char *path, uint64_t *size) {
    FILE *f = open_file(path, "rb");
    if (!f) return NULL;
    *size = plat_file_size(path);
    uint8_t *buf = calloc(1, *size + 1);
    if (fread(buf, 1, *size, f) != *size) { free(buf); buf = NULL; }
    fclose(f);
    return buf;
}
int plat_file_write_atomic(const char *path, const void *data, size_t size) {
    char selector[8192];
    snprintf(selector, sizeof selector, "%s", exe);
    char *cut = plat_last_sep(selector);
    if (!cut) return 0;
    strcpy(cut + 1, "mkw.ini");
    if (fail_selector && !strcmp(selector, path)) return 0;
    FILE *f = open_file(path, "wb");
    if (!f) return 0;
    int ok = fwrite(data, 1, size, f) == size;
    return fclose(f) == 0 && ok;
}
const RtGame *rt_game_builtin(void) { return NULL; }
void *plat_lib_open(const char *path) { (void)path; return NULL; }
void *plat_lib_sym(void *lib, const char *name) { (void)lib; (void)name; return NULL; }
const char *rt_log_path(void) { return getenv("TEST_LOG_PATH"); }
int plat_stderr_path(char *out, size_t cap) {
    const char *path = getenv("TEST_STDERR_PATH");
    return path && snprintf(out, cap, "%s", path) < (int)cap;
}

static int run(int argc, char **argv) {
    exe = argv[0];
    if (argc > 1 && !strcmp(argv[1], "limit")) {
        printf("limit=%d\n", CFG_GAME_DIR_MAX);
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "logs")) {
        char folder[PLAT_PATH_MAX * 3] = "";
        size_t cap = argc > 2 ? strtoul(argv[2], NULL, 10) : sizeof folder;
        if (cap > sizeof folder) return 2;
        printf("ok=%d\n", sysinfo_log_folder(folder, cap));
        printf("folder=%s\n", folder);
        return 0;
    }
    // Discovery happens before mem_init/cfg_init in a player's build.
    printf("ready=%d\n", rt_game_present());
    printf("dir=%s\n", cfg_game_dir());
    printf("romfs=%s\n", cfg_get("MKW_ROMFS") ? cfg_get("MKW_ROMFS") : "");
    printf("volume=%s\n", cfg_value("MKW_VOLUME"));
    if (argc > 2 && !strcmp(argv[1], "save")) {
        cfg_set("MKW_VOLUME", argv[2]);
        return cfg_save() ? 0 : 1;
    }
    if (argc > 2) {
        fail_selector = !strcmp(argv[1], "fail");
        printf("selected=%d\n", cfg_select_game_dir(argv[2]));
        printf("invalid=%d\n", cfg_game_dir_invalid());
        printf("value=%s\n", cfg_get("MKW_GAME_DIR") ? cfg_get("MKW_GAME_DIR") : "");
        if (fail_selector) {
            cfg_set("MKW_VOLUME", "37");
            return cfg_save() ? 0 : 1;
        }
    }
    return 0;
}

#ifdef _WIN32
int wmain(int argc, wchar_t **wide) {
    char **args = calloc((size_t)argc + 1, sizeof *args);
    for (int i = 0; i < argc; i++) {
        int n = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, NULL, 0, NULL, NULL);
        if (!n) return 2;
        args[i] = malloc((size_t)n);
        if (!WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, args[i], n, NULL, NULL)) return 2;
    }
    int result = run(argc, args);
    for (int i = 0; i < argc; i++) free(args[i]);
    free(args);
    return result;
}
#else
int main(int argc, char **argv) { return run(argc, argv); }
#endif
