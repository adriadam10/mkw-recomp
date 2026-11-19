// A mod's settings per player (runtime/plugin/mod_settings.c), built alone with the platform layer's file and lock
// calls stubbed on stdio: values round-trip, a set replaces only its key, NULL removes it, the file survives a re-read
// from another cache, invalid keys / values / mod names are refused, and two mods or two players never share a file.
// Usage: t_mod_settings <empty directory>   Prints "ok" and exits 0, else the failed check.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "mod_settings.h"
#include "platform/platform.h"

void plat_mutex_lock(PlatMutex *m) { (void)m; }
void plat_mutex_unlock(PlatMutex *m) { (void)m; }
uint8_t *plat_file_read(const char *path, uint64_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
    uint8_t *b = malloc((size_t)n + 1);
    size_t got = fread(b, 1, (size_t)n, f);
    fclose(f);
    b[got] = 0;
    *size = got;
    return b;
}
void plat_file_mkdir_p(const char *path) { mkdir(path, 0755); }
int plat_file_write_atomic(const char *path, const void *data, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    int ok = fwrite(data, 1, n, f) == n;
    return fclose(f) == 0 && ok;
}

#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) { printf("failed: %s (line %d)\n", #c, __LINE__); return 1; }                                       \
    } while (0)

static char *slurp(const char *path) {
    uint64_t n;
    return (char *)plat_file_read(path, &n);
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    char p1[512], p2[512], v[64], file[600];
    snprintf(p1, sizeof p1, "%s/p1", argv[1]);
    snprintf(p2, sizeof p2, "%s/p2", argv[1]);
    mkdir(p1, 0755);
    mkdir(p2, 0755);

    CHECK(!mod_settings_get(p1, "mkwmp", "server", v, sizeof v));   // nothing stored yet, no file
    CHECK(mod_settings_set(p1, "mkwmp", "server", "https://example.org"));
    CHECK(mod_settings_set(p1, "mkwmp", "voice", "1"));
    CHECK(mod_settings_get(p1, "mkwmp", "server", v, sizeof v) && !strcmp(v, "https://example.org"));
    CHECK(mod_settings_set(p1, "mkwmp", "server", "http://localhost:8900"));   // replaced, voice kept
    CHECK(mod_settings_get(p1, "mkwmp", "server", v, sizeof v) && !strcmp(v, "http://localhost:8900"));
    CHECK(mod_settings_get(p1, "mkwmp", "voice", v, sizeof v) && !strcmp(v, "1"));
    snprintf(file, sizeof file, "%s/mod-settings/mkwmp.ini", p1);
    char *text = slurp(file);
    CHECK(text && !strcmp(text, "voice=1\nserver=http://localhost:8900\n"));
    free(text);

    // another mod's file (the cache moves to it), then back: read from the disk again
    CHECK(mod_settings_set(p1, "other", "server", "x"));
    CHECK(mod_settings_get(p1, "mkwmp", "server", v, sizeof v) && !strcmp(v, "http://localhost:8900"));
    CHECK(mod_settings_get(p1, "other", "server", v, sizeof v) && !strcmp(v, "x"));
    // another player: their own file
    CHECK(!mod_settings_get(p2, "mkwmp", "server", v, sizeof v));
    CHECK(mod_settings_set(p2, "mkwmp", "server", "p2"));
    CHECK(mod_settings_get(p1, "mkwmp", "server", v, sizeof v) && !strcmp(v, "http://localhost:8900"));

    // a key that is a prefix of another is its own key; an empty value is a value
    CHECK(mod_settings_set(p1, "mkwmp", "serv", ""));
    CHECK(mod_settings_get(p1, "mkwmp", "serv", v, sizeof v) && !strcmp(v, ""));
    CHECK(mod_settings_get(p1, "mkwmp", "server", v, sizeof v) && !strcmp(v, "http://localhost:8900"));
    // NULL removes
    CHECK(mod_settings_set(p1, "mkwmp", "voice", NULL));
    CHECK(!mod_settings_get(p1, "mkwmp", "voice", v, sizeof v));
    // cut at cap, NUL-terminated
    CHECK(mod_settings_get(p1, "mkwmp", "server", v, 5) && !strcmp(v, "http"));

    // refused: keys with other characters or too long, values on two lines or too long, mod names that leave the folder
    CHECK(!mod_settings_set(p1, "mkwmp", "a b", "1") && !mod_settings_set(p1, "mkwmp", "a=b", "1"));
    CHECK(!mod_settings_set(p1, "mkwmp", "", "1"));
    char long_key[80];
    memset(long_key, 'k', 65);
    long_key[65] = 0;
    CHECK(!mod_settings_set(p1, "mkwmp", long_key, "1"));
    CHECK(!mod_settings_set(p1, "mkwmp", "k", "a\nb") && !mod_settings_set(p1, "mkwmp", "k", "a\rb"));
    char big[1100];
    memset(big, 'v', 1025);
    big[1025] = 0;
    CHECK(!mod_settings_set(p1, "mkwmp", "k", big));
    big[1024] = 0;
    CHECK(mod_settings_set(p1, "mkwmp", "k", big));
    CHECK(!mod_settings_set(p1, "../x", "k", "1") && !mod_settings_set(p1, "a/b", "k", "1"));
    CHECK(!mod_settings_set("", "mkwmp", "k", "1") && !mod_settings_get(NULL, "mkwmp", "k", v, sizeof v));
    printf("ok\n");
    return 0;
}
