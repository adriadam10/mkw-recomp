// Isolated settings and installation discovery for test_game_dir.py. Single-thread platform stubs use real files;
// no game code, SDL window, GPU or guest address space is needed.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cfg.h"
#include "gamelib.h"
#include "platform/platform.h"

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
int plat_file_exists(const char *path) { struct stat s; return !stat(path, &s) && S_ISREG(s.st_mode); }
int64_t plat_file_size(const char *path) { struct stat s; return !stat(path, &s) ? s.st_size : -1; }
uint8_t *plat_file_read(const char *path, uint64_t *size) {
    FILE *f = fopen(path, "rb");
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
    strcpy(strrchr(selector, '/') + 1, "mkw.ini");
    if (fail_selector && !strcmp(selector, path)) return 0;
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    int ok = fwrite(data, 1, size, f) == size;
    return fclose(f) == 0 && ok;
}
const RtGame *rt_game_builtin(void) { return NULL; }
void *plat_lib_open(const char *path) { (void)path; return NULL; }
void *plat_lib_sym(void *lib, const char *name) { (void)lib; (void)name; return NULL; }

int main(int argc, char **argv) {
    exe = argv[0];
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
        if (fail_selector) {
            cfg_set("MKW_VOLUME", "37");
            return cfg_save() ? 0 : 1;
        }
    }
    return 0;
}
