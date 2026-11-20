// The runtime's .nup reader (runtime/sdk/fs/nup.c), built alone with the platform layer's file and lock calls stubbed
// on stdio and zstd stubbed (test_nup.py writes host.h and zstd.h for it; its packages store files raw): lists what
// the RomFS layer would see of a package, so the test can hold it against tools/prepare/nup.py.
// Usage: t_nup PACKAGE [count]   Prints "exefs|romfs<TAB>name<TAB>bytes" per file, sorted as nup_entries gives them
// (with count: "exefs N" and "romfs N" only); exit 1 when the package does not open or a file does not read.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nup.h"

struct PlatFile { FILE *f; };

void plat_mutex_lock(PlatMutex *m) { (void)m; }
void plat_mutex_unlock(PlatMutex *m) { (void)m; }
void rt_log(const char *fmt, ...) { (void)fmt; }

int plat_file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f) fclose(f);
    return f != NULL;
}

PlatFile *plat_file_open(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    PlatFile *p = malloc(sizeof *p);
    p->f = f;
    return p;
}

void plat_file_close(PlatFile *p) {
    fclose(p->f);
    free(p);
}

// 64-bit offsets: "count" is also run by hand on a real 23 GB package
#ifdef _WIN32
#define SEEK64 _fseeki64
#define TELL64 _ftelli64
#else
#define SEEK64 fseeko
#define TELL64 ftello
#endif

int64_t plat_file_pread(PlatFile *p, void *buf, uint32_t n, uint64_t off) {
    if (SEEK64(p->f, (int64_t)off, SEEK_SET)) return -1;
    return (int64_t)fread(buf, 1, n, p->f);
}

int plat_file_id(PlatFile *p, PlatFileId *out) {
    memset(out, 0, sizeof *out);
    if (SEEK64(p->f, 0, SEEK_END)) return 0;
    out->size = (uint64_t)TELL64(p->f);
    return 1;
}

int main(int argc, char **argv) {
    if (argc < 2 || argc > 3) return 2;
    Nup *n = nup_open(argv[1]);
    if (!n) { printf("cannot open\n"); return 1; }
    for (int part = 0; part < 2; part++) {
        const PackEntry *e;
        size_t count = nup_entries(n, part, &e);
        if (argc == 3) {   // t_nup PACKAGE count: the files of each part, nothing read
            printf("%s %zu\n", part == NUP_EXEFS ? "exefs" : "romfs", count);
            continue;
        }
        for (size_t i = 0; i < count; i++) {
            uint8_t *data = nup_read(n, &e[i]);
            if (!data) { printf("cannot read %s\n", e[i].name); return 1; }
            printf("%s\t%s\t%.*s\n", part == NUP_EXEFS ? "exefs" : "romfs", e[i].name, (int)e[i].size, (char *)data);
            free(data);
        }
    }
    return 0;
}
