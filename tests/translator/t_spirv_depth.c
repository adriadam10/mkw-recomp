// Runs the runtime's -1..1 depth rewrite (runtime/gfx/vk/pipe/vk_spirv_depth.c) on a module: test_spirv_depth.py.
// Usage: t_spirv_depth IN.spv OUT.spv. Exit 0: rewritten to OUT; 1: not rewritten (NULL); 2: I/O; 3: input changed.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vk_spirv_patch.h"

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 2;
    fseek(f, 0, SEEK_END);
    long bytes = ftell(f);
    rewind(f);
    if (bytes < 0 || bytes % 4) return 2;
    uint32_t *w = malloc((size_t)bytes + 4), *before = malloc((size_t)bytes + 4);
    if (!w || !before || fread(w, 1, (size_t)bytes, f) != (size_t)bytes) return 2;
    fclose(f);
    memcpy(before, w, (size_t)bytes);
    uint64_t n = 0;
    uint32_t *out = spirv_depth_minus_w(w, (uint64_t)bytes / 4, &n);
    if (memcmp(before, w, (size_t)bytes)) return 3;
    if (!out) return 1;
    f = fopen(argv[2], "wb");
    if (!f || fwrite(out, 4, (size_t)n, f) != n) return 2;
    fclose(f);
    free(out), free(w), free(before);
    return 0;
}
