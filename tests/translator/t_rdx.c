// Runs the production RenoDX constant patch (gfx_rdx.c gfx_rdx_patch) on a module file (test_rdx.py):
// t_rdx IN OUT ID VALUE writes the patched words to OUT and exits with the patch's result (1 found, 0 not).
#include <stdio.h>
#include <stdlib.h>
#include "gfx.h"

int main(int argc, char **argv) {
    if (argc != 5) return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 2;
    fseek(f, 0, SEEK_END);
    long bytes = ftell(f);
    rewind(f);
    if (bytes < 0 || bytes % 4) return 2;
    uint32_t *w = malloc((size_t)bytes + 4);
    if (!w || fread(w, 1, (size_t)bytes, f) != (size_t)bytes) return 2;
    fclose(f);
    int r = gfx_rdx_patch(w, (uint64_t)bytes / 4, (uint32_t)strtoul(argv[3], NULL, 0), strtof(argv[4], NULL));
    f = fopen(argv[2], "wb");
    if (!f || fwrite(w, 1, (size_t)bytes, f) != (size_t)bytes) return 2;
    fclose(f);
    free(w);
    return r;
}
