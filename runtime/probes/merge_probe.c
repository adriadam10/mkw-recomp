// --probe-merge: the shadow write-back by bytes (gfx/vk/mem/vk_mem_merge.c). merge_back_runs, the default, against
// merge_back_bytes, the byte loop it replaced, on synthetic ranges: the same guest memory after the write-back, the same
// bytes copied and the same bytes counted as the GPU's, with MKW_SHADOW_CPU_WINS off and on, for ranges that start
// and end inside a page and patterns from none changed to all, the way a race's vertex data changes (most bytes, broken
// by ones that happen to stay equal). Then the speed of each on the two shapes that cost a race the most (rpwb-0,
// 18/11/2026: 4-64 KiB ranges the GPU rewrote, and 1-4 MiB ones it left alone). Needs no game.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "platform/platform.h"
#include "vk.h"

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint64_t next(void) {   // xorshift64*
    rng ^= rng >> 12, rng ^= rng << 25, rng ^= rng >> 27;
    return rng * 0x2545F4914F6CDD1Dull;
}

enum { PAT_NONE, PAT_SPARSE, PAT_RUNS, PAT_VERTEX, PAT_ALL, PAT_PAGES, PAT_N };
static const char *const pat_name[PAT_N] = {"none changed", "sparse bytes", "runs", "vertex-like", "all", "whole pages"};

// The GPU's bytes over base (s), and guest memory (g): the base with, when cpu, bytes the CPU wrote after it (some the
// GPU wrote too).
static void make(int pat, uint8_t *base, uint8_t *s, uint8_t *g, uint64_t size, int cpu) {
    for (uint64_t i = 0; i < size; i++) base[i] = (uint8_t)next();
    memcpy(s, base, size);
    for (uint64_t i = 0; i < size; i++) {
        uint64_t r = next();
        switch (pat) {
        case PAT_SPARSE: if (r % 97 == 0) s[i] ^= (uint8_t)(1 + r % 255); break;
        case PAT_RUNS: if (r % 300 == 0) for (uint64_t k = i, e = i + 1 + r % 200; k < e && k < size; k++) s[k] = (uint8_t)~base[k]; break;
        case PAT_VERTEX: s[i] = (uint8_t)(r >> 8); break;   // new bytes: 1 in 256 happens to equal its base
        case PAT_ALL: s[i] = (uint8_t)~base[i]; break;
        case PAT_PAGES: if (((i >> 12) & 3) == 1) s[i] = (uint8_t)~base[i]; break;
        default: break;
        }
    }
    memcpy(g, base, size);
    if (cpu)
        for (uint64_t i = 0; i < size; i++)
            if (next() % 13 == 0) g[i] = (uint8_t)(g[i] + 1 + next() % 254);
}

static int fails, cases;

static void check(int pat, uint64_t size, uint64_t off, int cw) {
    uint8_t *base = malloc(size), *s = malloc(size), *g0 = malloc(size);
    uint8_t *mem = plat_aligned_alloc(2 * 4096 + ((size + 4095) & ~4095ull), 4096);   // page-aligned, room for the offset
    uint8_t *mem2 = plat_aligned_alloc(2 * 4096 + ((size + 4095) & ~4095ull), 4096);
    if (!base || !s || !g0 || !mem || !mem2) { printf("FAIL merge: out of memory\n"); fails++; return; }
    make(pat, base, s, g0, size, cw);
    uint8_t *ga = mem + off, *gb = mem2 + off;   // the range starts off bytes into a page
    memcpy(ga, g0, size), memcpy(gb, g0, size);
    uint64_t gpu_a, gpu_b;
    uint64_t ca = vk_merge_back_probe(0, cw, (uint64_t)(uintptr_t)ga, size, s, base, &gpu_a);
    uint64_t cb = vk_merge_back_probe(1, cw, (uint64_t)(uintptr_t)gb, size, s, base, &gpu_b);
    cases++;
    if (ca != cb || gpu_a != gpu_b || memcmp(ga, gb, size)) {
        uint64_t at = 0;
        while (at < size && ga[at] == gb[at]) at++;
        printf("FAIL merge: %s, %llu bytes at page offset %llu, cpu wins %d: copied %llu/%llu, GPU's %llu/%llu, first "
               "different byte %lld\n", pat_name[pat], (unsigned long long)size, (unsigned long long)off, cw,
               (unsigned long long)ca, (unsigned long long)cb, (unsigned long long)gpu_a, (unsigned long long)gpu_b,
               at < size ? (long long)at : -1);
        fails++;
    }
    free(base), free(s), free(g0), plat_aligned_free(mem), plat_aligned_free(mem2);
}

// ms per call of each implementation over reps write-backs of one range (guest memory reset before each).
static void bench(const char *what, int pat, uint64_t size, int reps) {
    uint8_t *base = malloc(size), *s = malloc(size), *g0 = malloc(size), *g = plat_aligned_alloc(size, 4096);
    if (!base || !s || !g0 || !g) return;
    make(pat, base, s, g0, size, 1);
    double ms[2];
    for (int runs = 0; runs < 2; runs++) {
        uint64_t t = 0, gpu;
        for (int r = 0; r < reps; r++) {
            memcpy(g, g0, size);
            uint64_t t0 = plat_now_ns();
            vk_merge_back_probe(runs, 1, (uint64_t)(uintptr_t)g, size, s, base, &gpu);
            t += plat_now_ns() - t0;
        }
        ms[runs] = t / 1e6 / reps;
    }
    printf("merge: %-40s bytes %.3f ms, runs %.3f ms a range (%.1fx, %.1f GB/s)\n", what, ms[0], ms[1], ms[0] / ms[1],
           size / (ms[1] * 1e6));
    free(base), free(s), free(g0), plat_aligned_free(g);
}

int probe_merge(void) {
    static const uint64_t sizes[] = {1, 7, 63, 64, 65, 127, 4095, 4096, 4097, 8192 + 100, 45 * 1024 + 3, 65536};
    static const uint64_t offs[] = {0, 1, 63, 4032, 4095};
    for (int pat = 0; pat < PAT_N; pat++)
        for (size_t si = 0; si < sizeof sizes / sizeof *sizes; si++)
            for (size_t oi = 0; oi < sizeof offs / sizeof *offs; oi++)
                for (int cw = 0; cw < 2; cw++) check(pat, sizes[si], offs[oi], cw);
    for (int k = 0; k < 40; k++)   // random sizes and offsets
        check((int)(next() % PAT_N), 1 + next() % 200000, next() % 4096, (int)(next() & 1));
    printf("%s merge: %d cases, the two write-backs %s\n", fails ? "FAIL" : "ok  ", cases,
           fails ? "differ" : "write the same bytes");
    bench("44 KiB vertex-like (the GPU rewrote it)", PAT_VERTEX, 44 * 1024, 2000);
    bench("44 KiB, runs here and there", PAT_RUNS, 44 * 1024, 2000);
    bench("4 MiB, none changed", PAT_NONE, 4u << 20, 50);
    printf("probe-merge: %s\n", fails ? "FAILED" : "passed");
    return fails ? 1 : 0;
}
