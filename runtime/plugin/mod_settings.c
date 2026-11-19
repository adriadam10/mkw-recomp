// A mod's settings, per player: <player folder>/mod-settings/<mod>.ini, "key=value" lines (mod_settings.h). The start
// screen's options page (mod_page.cpp) writes them, the plugin reads them in the game (plugin.c); both go through here.
//
// One file is cached at a time (the page reads its widgets' values every frame): the cache is dropped when another
// file is asked for, and rewritten with the file on every set. A file another process changed meanwhile is seen at the
// next change of file; the start screen and the game do not run together.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mod_settings.h"
#include "platform/platform.h"

static PlatMutex lock = PLAT_MUTEX_INIT;
static char cached_path[PLAT_PATH_MAX * 2 + 96];
static char *cached;   // the file's text, NUL-terminated; NULL: not loaded (or missing: "")

int mod_settings_key_ok(const char *key) {
    if (!key || !*key) return 0;
    size_t n = 0;
    for (const char *k = key; *k; k++, n++) {
        char c = *k;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
              c == '-'))
            return 0;
    }
    return n <= MOD_SETTING_KEY_MAX;
}

static int value_ok(const char *v) {
    if (!v) return 1;
    size_t n = strlen(v);
    return n <= MOD_SETTING_VALUE_MAX && !strchr(v, '\n') && !strchr(v, '\r');
}

static int path_of(const char *player_dir, const char *mod, char *out, size_t cap) {
    if (!player_dir || !*player_dir || !mod || !*mod || strchr(mod, '/') || strchr(mod, '\\') || strstr(mod, ".."))
        return 0;
    return snprintf(out, cap, "%s/mod-settings/%s.ini", player_dir, mod) < (int)cap;
}

// The cache for path; lock held. 1 when it holds that file (an empty text when it is missing).
static int load(const char *path) {
    if (cached && !strcmp(cached_path, path)) return 1;
    free(cached);
    cached = NULL;
    uint64_t size = 0;
    uint8_t *data = plat_file_read(path, &size);   // NULL when missing or empty
    cached = data ? (char *)data : calloc(1, 1);
    if (!cached) return 0;
    snprintf(cached_path, sizeof cached_path, "%s", path);
    return 1;
}

// The value of key in text: its start and length, or NULL.
static const char *find(const char *text, const char *key, size_t *len) {
    size_t kn = strlen(key);
    for (const char *line = text; *line;) {
        const char *end = strchr(line, '\n');
        size_t n = end ? (size_t)(end - line) : strlen(line);
        if (n > kn && !strncmp(line, key, kn) && line[kn] == '=') {
            *len = n - kn - 1;
            if (*len && line[kn + 1 + *len - 1] == '\r') (*len)--;
            return line + kn + 1;
        }
        line = end ? end + 1 : line + n;
    }
    return NULL;
}

int mod_settings_get(const char *player_dir, const char *mod, const char *key, char *out, size_t cap) {
    char path[sizeof cached_path];
    if (!out || !cap || !mod_settings_key_ok(key) || !path_of(player_dir, mod, path, sizeof path)) return 0;
    plat_mutex_lock(&lock);
    size_t len = 0;
    const char *v = load(path) ? find(cached, key, &len) : NULL;
    if (v) {
        size_t n = len < cap - 1 ? len : cap - 1;
        memcpy(out, v, n);
        out[n] = 0;
    }
    plat_mutex_unlock(&lock);
    return v != NULL;
}

int mod_settings_set(const char *player_dir, const char *mod, const char *key, const char *value) {
    char path[sizeof cached_path];
    if (!mod_settings_key_ok(key) || !value_ok(value) || !path_of(player_dir, mod, path, sizeof path)) return 0;
    plat_mutex_lock(&lock);
    int ok = 0;
    if (load(path)) {
        // the other lines as they are, this key's dropped, then this key's new line at the end
        size_t old = strlen(cached), kn = strlen(key), vn = value ? strlen(value) : 0;
        char *text = malloc(old + kn + vn + 3);
        if (text) {
            size_t n = 0;
            for (const char *line = cached; *line;) {
                const char *end = strchr(line, '\n');
                size_t ln = end ? (size_t)(end - line) : strlen(line);
                int mine = ln > kn && !strncmp(line, key, kn) && line[kn] == '=';
                if (!mine && ln) {
                    memcpy(text + n, line, ln);
                    n += ln;
                    text[n++] = '\n';
                }
                line = end ? end + 1 : line + ln;
            }
            if (value) {
                memcpy(text + n, key, kn);
                n += kn;
                text[n++] = '=';
                memcpy(text + n, value, vn);
                n += vn;
                text[n++] = '\n';
            }
            text[n] = 0;
            char dir[sizeof path];
            snprintf(dir, sizeof dir, "%s/mod-settings", player_dir);
            plat_file_mkdir_p(dir);
            ok = plat_file_write_atomic(path, text, n);
            if (ok) {
                free(cached);
                cached = text;
            } else {
                free(text);
            }
        }
    }
    plat_mutex_unlock(&lock);
    return ok;
}
