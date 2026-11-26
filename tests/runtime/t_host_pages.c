// Guest pages (4 KiB) on larger host pages (runtime/platform/linux_mem.c, built alone), run by test_host_pages.py
// with the kernel's pages and with MKW_HOST_PAGE_KIB=16 and 64, which take host pages as that large on a 4 KiB kernel
// (Linux on Apple silicon has 16 KiB ones: 0.1.18 died there protecting subsdk0's +0xbe000 size 0x2000). Checks, with
// a fault probe for what the host really allows:
// - a guest protection on part of a host page never fails and never takes access from a neighbour; a whole host page
//   gets exactly what was asked; plat_mem_loose_pages counts the host pages left more accessible than asked
// - a decommitted guest page reads zero when committed again, and its neighbours in the host page keep their bytes
// - the write watch reports every guest page of a written host page, a one-page peek finds its page, and a range that
//   covers a host page only in part does not take away the written marks of the rest (two pools in one 16 KiB page)
// Prints "ok" last.
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"

// What linux_mem.c takes from the rest of the runtime: one thread here, no fault handler of its own installed
void plat_mutex_lock(PlatMutex *m) { (void)m; }
void plat_mutex_unlock(PlatMutex *m) { (void)m; }
void alt_stack(void) {}

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL: " __VA_ARGS__); putchar('\n'); } } while (0)

static sigjmp_buf probe_jb;
static void on_fault(int sig) { siglongjmp(probe_jb, sig); }

// 1 when a byte at a can be written (it is written with its own value), 0 when the write faults
static int writable(uint64_t a) {
    volatile uint8_t *p = (volatile uint8_t *)(uintptr_t)a;
    if (!plat_mem_readable(a, 1)) return 0;   // a read fault outside the probe below would jump to a stale frame
    if (sigsetjmp(probe_jb, 1)) return 0;
    *p = *p;
    return 1;
}

#define PG 0x1000ull
#define BASE 0x5000000000ull   // below 2^39, 64 KiB aligned
#define SIZE (64ull << 20)

static void protections(uint64_t hp) {
    const uint64_t x = BASE;
    volatile uint8_t *b = (volatile uint8_t *)(uintptr_t)x;
    uint64_t end;
    CHECK(plat_mem_commit(x, 4 * hp), "commit of 4 host pages");
    memset((void *)(uintptr_t)x, 0xAB, 4 * hp);
    // one guest page read-only inside host page 0
    uint64_t g = x + (hp > PG ? PG : 0);
    CHECK(plat_mem_protect(g, PG, PLAT_PROT_READ), "a read-only guest page inside a host page");
    CHECK(plat_mem_state(g, x + hp, &end) == PLAT_PROT_READ, "the guest page's own state is not read-only");
    if (hp > PG) CHECK(writable(x) && writable(x + 2 * PG), "a neighbour of the read-only guest page lost its write access");
    else CHECK(!writable(g), "a read-only 4 KiB host page can be written");
    CHECK(plat_mem_loose_pages(x, hp) == (hp > PG), "loose host pages: %llu", (unsigned long long)plat_mem_loose_pages(x, hp));
    // a whole host page read-only, then inaccessible, then read-write again
    CHECK(plat_mem_protect(x + hp, hp, PLAT_PROT_READ) && !writable(x + hp) && !writable(x + 2 * hp - 1) &&
              plat_mem_readable(x + hp, hp) && plat_mem_loose_pages(x + hp, hp) == 0,
          "a whole host page made read-only is not exactly that");
    CHECK(plat_mem_protect(x + hp, hp, PLAT_PROT_NONE) && !plat_mem_readable(x + hp, 1), "a whole host page made inaccessible");
    CHECK(plat_mem_protect(x + hp, hp, PLAT_PROT_RW) && writable(x + hp) && b[hp] == 0xAB, "a whole host page back to read-write");
    // subsdk0's data permission: 2 guest pages ending a host page, aligned to 4 KiB only
    CHECK(plat_mem_protect(x + 3 * hp - 2 * PG, 2 * PG, PLAT_PROT_READ), "guest pages ending a host page made read-only");
    if (hp > 2 * PG) CHECK(writable(x + 2 * hp) && writable(x + 3 * hp - 3 * PG), "their neighbours lost write access");
    CHECK(plat_mem_protect(x + 3 * hp - 2 * PG, 2 * PG, PLAT_PROT_RW) && writable(x + 3 * hp - 1), "and back");
    // a decommitted guest page in a host page that keeps committed ones
    if (hp > PG) b[3 * hp + PG] = 0x11;   // a neighbour in the same host page (with 4 KiB ones there is none)
    CHECK(plat_mem_decommit(x + 3 * hp, PG) && plat_mem_state(x + 3 * hp, x + 4 * hp, &end) == PLAT_MEM_RESERVED,
          "decommit of one guest page");
    if (hp > PG) CHECK(b[3 * hp + PG] == 0x11 && b[3 * hp + 2 * PG] == 0xAB && writable(x + 3 * hp + PG),
                       "a decommit took a neighbour's bytes or access");
    CHECK(plat_mem_commit(x + 3 * hp, PG) && b[3 * hp] == 0 && b[3 * hp + PG - 1] == 0,
          "a decommitted guest page does not read zero when committed again");
    CHECK(plat_mem_decommit(x, 4 * hp) && !plat_mem_readable(x, 1) && !plat_mem_readable(x + 4 * hp - 1, 1),
          "host pages decommitted whole stay readable");
    CHECK(plat_mem_commit(x, 4 * hp) && b[0] == 0 && b[2 * hp] == 0 && b[4 * hp - 1] == 0,
          "host pages decommitted whole do not read zero when committed again");
    CHECK(plat_mem_decommit(x, 4 * hp), "final decommit");
}

static uint64_t written(uint64_t a, uint64_t n, uint64_t *pages, uint64_t max, int reset) {
    return plat_mem_written(a, n, pages, max, reset);
}

static void write_watch(uint64_t hp) {
    const uint64_t x = BASE + (16ull << 20);
    static uint64_t pages[4096];
    CHECK(plat_mem_commit(x, 8 * hp), "commit for the write watch");
    memset((void *)(uintptr_t)x, 1, 8 * hp);
    if (written(x, 8 * hp, pages, 4096, 1) == UINT64_MAX) {
        printf("write watch: not on this kernel, skipped\n");
        return;
    }
    CHECK(written(x, 8 * hp, pages, 4096, 1) == 0, "pages still written after a reset");
    // one guest page written in host page 1: every guest page of it is reported when host pages are larger
    uint64_t h = x + hp, g = h + (hp > PG ? 2 * PG : 0);
    *(volatile uint8_t *)(uintptr_t)g = 2;
    uint64_t n = written(g, PG, pages, 1, 0);
    CHECK(n == 1 && pages[0] == g, "a one-page peek of the written page: %llu", (unsigned long long)n);
    n = written(x, 8 * hp, pages, 4096, 0);
    int has = 0;
    for (uint64_t i = 0; i < n; i++) has |= pages[i] == g;
    CHECK(has && n >= 1 && n <= hp / PG, "a peek of the range: %llu pages", (unsigned long long)n);
    if (hp > PG) {
        // pool A is the first guest page of host page 1, pool B the rest: A's scan with a reset leaves B's mark
        n = written(h, PG, pages, 16, 1);
        CHECK(n <= 1, "pool A: %llu pages", (unsigned long long)n);
        n = written(h + PG, hp - PG, pages, 16, 1);
        has = 0;
        for (uint64_t i = 0; i < n; i++) has |= pages[i] == g;
        CHECK(has, "pool B lost its written page to pool A's reset (%llu pages)", (unsigned long long)n);
    }
    // a scan of the whole range with a reset takes it all: nothing written after
    written(x, 8 * hp, pages, 4096, 1);
    CHECK(written(x, 8 * hp, pages, 4096, 1) == 0, "written pages after a whole reset");
    // a reset scan whose room ends inside a host page leaves that host page for the next scan
    for (uint64_t i = 0; i < 8; i++) *(volatile uint8_t *)(uintptr_t)(x + i * hp) = 3;
    uint64_t total = 0, k, at = x;
    for (int i = 0; i < 64 && (k = written(at, x + 8 * hp - at, pages, 3, 1)) && k != UINT64_MAX; i++) {
        total += k;
        at = pages[k - 1] + PG;
    }
    CHECK(total >= 8, "written pages lost when the room ran out: %llu of at least 8", (unsigned long long)total);
    plat_mem_decommit(x, 8 * hp);
}

int main(void) {
    signal(SIGSEGV, on_fault);
    signal(SIGBUS, on_fault);
    uint64_t hp = plat_page_size();
    printf("host pages: %llu KiB\n", (unsigned long long)(hp >> 10));
    if (!plat_mem_reserve(BASE, SIZE, 1)) {
        printf("FAIL: reserve %#llx\n", (unsigned long long)BASE);
        return 1;
    }
    protections(hp);
    write_watch(hp);
    // memory outside the guest's reservations (the dispatch table, sealed read-only): protected as asked
    uint8_t *t = plat_mem_alloc(3 * hp + 100);
    CHECK(t && (t[0] = 7) && plat_mem_protect((uint64_t)(uintptr_t)t, 3 * hp + 100, PLAT_PROT_READ) &&
              plat_mem_readable((uint64_t)(uintptr_t)t, 3 * hp + 100) && t[0] == 7 && !writable((uint64_t)(uintptr_t)t),
          "memory of plat_mem_alloc sealed read-only is not exactly that");
    if (failures) return 1;
    printf("ok\n");
    return 0;
}
