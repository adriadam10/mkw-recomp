// --probe-share (MKW's own; zero-copy pools, MKW_UDMABUF, gfx/vk/mem/vk_mem_zc.c): guest pages swapped for a PlatShared's on
// 8 MiB of the runtime's own guest memory, without a GPU. Checked: every byte kept; no page marked as written by the swap
// nor by reading it, exactly one by a write; the bytes written on one side seen on the other; a poke and a peek through a
// no-access page; a read trap over shared pages; a file read in flight refuses the swap; a spill takes them and gives
// them back; four threads writing while the pages are swapped lose no write.
#include <stdio.h>
#include <string.h>

#include "host.h"
#include "platform/platform.h"

#define PAGE 4096ull
#define SIZE (8ull << 20)

static int fails;

static void expect(int ok, const char *what) {
    printf("%s share: %s\n", ok ? "ok  " : "FAIL", what);
    fails += !ok;
}

static uint8_t want(uint64_t o) { return (uint8_t)(o / PAGE * 37 + o * 11 + 5); }

static uint64_t bad(const uint8_t *p) {
    uint64_t n = 0;
    for (uint64_t o = 0; o < SIZE; o++) n += p[o] != want(o);
    return n;
}

static uint64_t marks(uint64_t a) {
    static uint64_t pages[SIZE / PAGE];
    return mem_written_pages(a, SIZE, pages, SIZE / PAGE);
}

// Each thread counts up in its own word of every page until told to stop; a lost write leaves a word behind.
typedef struct { uint64_t a; int id; volatile int32_t *stop; uint32_t laps; } Writer;
static void writer(void *p) {
    Writer *w = p;
    while (!__atomic_load_n(w->stop, __ATOMIC_ACQUIRE)) {
        w->laps++;
        for (uint64_t o = 0; o < SIZE; o += PAGE) ((volatile uint32_t *)(uintptr_t)(w->a + o))[w->id]++;
    }
}

int probe_share(void) {
    PlatShared s, s2;
    const char *why = "";
    if (!plat_shared_open(SIZE, &s, &why)) return printf("probe-share: not available here (%s)\n", why), 1;
    uint64_t a = mem_runtime_alloc(SIZE, "share probe");
    if (!a) return printf("probe-share: no guest memory\n"), 1;
    uint8_t *g = (uint8_t *)(uintptr_t)a;
    for (uint64_t o = 0; o < SIZE; o++) g[o] = want(o);

    mem_io_begin(a + 5 * PAGE, 100);
    expect(!mem_share(a, a + SIZE, &s, 0) && !plat_mem_shared_at(a), "refused under a file read in flight");
    mem_io_end(a + 5 * PAGE, 100);
    expect(mem_share(a, a + SIZE, &s, 0) == 1, "the pages swapped");
    expect(!bad(g) && !bad(s.map), "every byte kept, on both sides");
    uint64_t n = marks(a);
    expect(n == 0, "no page marked as written by the swap or the reads");
    if (n) printf("     %llu pages\n", (unsigned long long)n);
    g[7 * PAGE + 3] ^= 0xff, n = marks(a);
    expect(n == 1 && s.map[7 * PAGE + 3] == (uint8_t)~want(7 * PAGE + 3), "a write marks its page and reaches the other side");
    s.map[7 * PAGE + 3] = want(7 * PAGE + 3);
    expect(g[7 * PAGE + 3] == want(7 * PAGE + 3) && marks(a) == 0, "a write on the other side is seen here, unmarked (the GPU's)");

    uint8_t b[8], v[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    plat_mem_protect(a, PAGE, PLAT_PROT_NONE);
    int ok = plat_mem_poke(a + 16, v, 8) && plat_mem_peek(b, a + 16, 8) && !memcmp(b, v, 8);
    plat_mem_protect(a, PAGE, PLAT_PROT_RW);
    expect(ok && !memcmp(g + 16, v, 8), "a poke and a peek through a no-access page");
    for (int i = 0; i < 8; i++) g[16 + i] = want(16 + (uint64_t)i);
    static uint8_t src[2 * PAGE];
    for (uint64_t o = 0; o < sizeof src; o++) src[o] = want(64 * PAGE + o);
    memset(g + 64 * PAGE, 0, sizeof src);
    if (mem_trap(a + 64 * PAGE, sizeof src, src)) expect(!bad(g), "a read trap over shared pages fills them");
    mem_untrap(a + 64 * PAGE, sizeof src);

    if (mem_spill_on()) {
        while (marks(a)) {}
        expect(mem_spill(a, a + SIZE) == SIZE && !plat_mem_shared_at(a + SIZE - PAGE), "a spill takes them off the GPU's pages");
        expect(!bad(g), "and gives every byte back");
    } else printf("     (no spill here: not checked)\n");

    // Swapped back and forth between two sets of pages with the writers running: every increment must survive.
    if (!plat_shared_open(SIZE, &s2, &why)) return printf("probe-share: second set refused (%s)\n", why), 1;
    memset(g, 0, SIZE);
    volatile int32_t stop = 0;
    Writer w[4];
    PlatThread *t[4];
    for (int i = 0; i < 4; i++) w[i] = (Writer){a, i, &stop, 0}, t[i] = plat_thread_new(writer, &w[i], "share probe");
    int swaps = 0;
    for (int i = 0; i < 200; i++) swaps += mem_share(a, a + SIZE, i & 1 ? &s2 : &s, 0) == 1;
    __atomic_store_n(&stop, 1, __ATOMIC_RELEASE);
    for (int i = 0; i < 4; i++) plat_thread_join(t[i]);
    uint64_t lost = 0;
    for (uint64_t o = 0; o < SIZE; o += PAGE)
        for (int i = 0; i < 4; i++) lost += ((uint32_t *)(g + o))[i] != w[i].laps;
    expect(swaps == 200 && !lost, "200 swaps under four writing threads lose no write");
    if (lost) printf("     %llu words behind\n", (unsigned long long)lost);
    printf("     %u laps of %llu pages by the first writer meanwhile\n", w[0].laps, SIZE / PAGE);
    expect(plat_mem_decommit(a, PAGE) && plat_mem_commit(a, PAGE) && !g[0] && !g[PAGE - 1] && !plat_mem_shared_at(a),
           "a page given back and committed again reads zero, its own");
    printf("probe-share: %s\n", fails ? "FAILED" : "passed");
    return fails != 0;
}
