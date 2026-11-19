#include <stdlib.h>
#include <string.h>

#include "vk_mem_internal.h"

// ---------------------------------------------------------------- lazy merge bases (MKW_LAZY_BASE)
// A merge base (vk_mem_merge.c) copied the shadow's bytes of each range a part may write when it was submitted: ~9 MiB
// a frame of 1-4 MiB ranges neither the CPU nor the GPU changes (wbc2), ~1 ms a frame on the replay thread with the
// wait for shadow_lock of whoever copies (ebn3-6: whoever copies, the replay waits for it). Guest memory holds those
// bytes too, so the whole pages inside a big range are base-trapped instead (memory_btrap.c): read-only, and the hook
// below copies a page into the base just before anything writes it. Pages that cannot be trapped (read traps,
// early-read watches, ends of the range) are copied from the shadow as before. A page in the buffer (copied, or
// materialized for base_follow and merge_in, which write the base) is the base; one still trapped is guest memory, and
// stays so while lazy_lock is held: the hook needs it before the page can change.
// MKW_LAZY_BASE (default 1; 0 copies every base; "ab": lazy in the B blocks). lzab1/lzab2 (pairs with their inverted
// twin): the sync's bases ~15 -> ~5 ms and its wait for shadow_lock ~47 -> ~11 ms per 60 frames; the frame with the
// scene balanced -0.34 ms (means), -0.58 (medians) over 40 s. MKW_LAZY_BASE_CHECK=1 (diagnosis) also copies
// every lazy base as before, keeps it with base_follow and merge_in, and counts the bytes where the two differ at the
// write-back: the trap must give the same base (sombra-replay 4).
#define LAZY_MIN (256u << 10)   // smaller ranges: two protection calls cost about what the copy did
#define LAZY_MAX 64             // registered at once; more go eager (a frame takes ~12, wbc3)

struct LazyBase {
    uint64_t lo, hi, plo, phi;   // the range, its whole pages
    uint8_t *buf, *check;        // the base ([lo, hi)); MKW_LAZY_BASE_CHECK's eager copy
    uint64_t *inbuf;             // bit per page of [plo, phi): its base is in buf
};

static PlatMutex lazy_lock = PLAT_MUTEX_INIT;   // leaf: taken under frz_lock (the hook) and under shadow_lock
static LazyBase *reg[LAZY_MAX];                 // lazy_lock
static int nreg;
static int lazy_mode = -1, check_on;
static volatile int64_t s_lazy_n, s_lazy_bytes, s_lazy_eager, s_lazy_filled, s_lazy_cmp, s_check_diff, s_check_bytes;

static int inbuf(const LazyBase *b, uint64_t p) {
    uint64_t k = (p - b->plo) / 4096;
    return (int)(b->inbuf[k / 64] >> (k % 64) & 1);
}

static void set_inbuf(LazyBase *b, uint64_t p) {
    uint64_t k = (p - b->plo) / 4096;
    b->inbuf[k / 64] |= 1ull << (k % 64);
}

// frz_lock held (memory_btrap.c): page p is about to change; its bytes go to the base that stands on it.
static void lazy_hook(uint64_t p) {
    plat_mutex_lock(&lazy_lock);
    for (int i = 0; i < nreg; i++) {
        LazyBase *b = reg[i];
        if (p < b->plo || p >= b->phi || inbuf(b, p)) continue;
        memcpy(b->buf + (p - b->lo), (const void *)(uintptr_t)p, 4096);
        set_inbuf(b, p);
        plat_inc64(&s_lazy_filled);
        break;   // no two lazy bases share a page (lazy_take)
    }
    plat_mutex_unlock(&lazy_lock);
}

int lazy_base_on(void) {
    if (lazy_mode < 0) {
        lazy_mode = rt_knob("LAZY_BASE", "1");
        check_on = rt_knob("LAZY_BASE_CHECK", "0") == 1;
        if (lazy_mode) mem_base_hook(lazy_hook);
        rt_log("mem: lazy merge bases (guest pages read-only until written) %s%s (MKW_LAZY_BASE)",
               lazy_mode == 2 ? "in the B blocks" : lazy_mode ? "on" : "off", check_on ? ", checked" : "");
    }
    return RT_KNOB_ON(lazy_mode);
}

// shadow_lock held: a lazy base of [lo, hi) of r (r's shadow covers it), or NULL to copy it as before.
LazyBase *lazy_take(PoolRec *r, uint64_t lo, uint64_t hi) {
    uint64_t plo = (lo + 4095) & ~4095ull, phi = hi & ~4095ull;
    if (phi <= plo || phi - plo < LAZY_MIN || hi - lo > MERGE_MAX || !r->smap) return NULL;
    LazyBase *b = calloc(1, sizeof *b);
    uint64_t words = ((phi - plo) / 4096 + 63) / 64;
    uint64_t *trapped = calloc(words, sizeof *trapped);
    if (!b || !trapped || !(b->buf = malloc(hi - lo)) || !(b->inbuf = calloc(words, sizeof *b->inbuf))) {
        if (b) free(b->buf), free(b->inbuf);
        free(b), free(trapped);
        return NULL;
    }
    b->lo = lo, b->hi = hi, b->plo = plo, b->phi = phi;
    plat_mutex_lock(&lazy_lock);
    int ok = nreg < LAZY_MAX;
    for (int i = 0; i < nreg && ok; i++) ok = !(plo < reg[i]->phi && phi > reg[i]->plo);
    if (ok) reg[nreg++] = b;
    plat_mutex_unlock(&lazy_lock);
    int traps = ok && mem_base_trap(plo, phi, trapped);
    if (ok && !traps) lazy_mode = 0;   // no trap map (Windows): every base is copied
    if (!traps) {
        if (ok) lazy_free(b);
        else free(b->buf), free(b->inbuf), free(b);
        free(trapped);
        return NULL;
    }
    // The rest from the shadow, as an eager base: the ends and the pages that could not be trapped.
    const uint8_t *s = r->smap + (lo - r->import_lo);
    memcpy(b->buf, s, plo - lo);
    memcpy(b->buf + (phi - lo), s + (phi - lo), hi - phi);
    uint64_t eager = (plo - lo) + (hi - phi);
    plat_mutex_lock(&lazy_lock);
    for (uint64_t p = plo; p < phi; p += 4096) {
        uint64_t k = (p - plo) / 4096;
        if (trapped[k / 64] >> (k % 64) & 1) continue;
        memcpy(b->buf + (p - lo), s + (p - lo), 4096);
        set_inbuf(b, p);
        eager += 4096;
    }
    plat_mutex_unlock(&lazy_lock);
    free(trapped);
    if (check_on && (b->check = malloc(hi - lo))) memcpy(b->check, s, hi - lo);
    plat_inc64(&s_lazy_n), plat_add64(&s_lazy_bytes, (int64_t)(hi - lo)), plat_add64(&s_lazy_eager, (int64_t)eager);
    return b;
}

// [a, b) of the base into its buffer (guest memory for the pages still trapped): before base_follow or merge_in write
// it. The pages stay trapped: a later write copies nothing more (they are in the buffer).
void lazy_materialize(LazyBase *b, uint64_t a, uint64_t e) {
    uint64_t p = (a > b->plo ? a : b->plo) & ~4095ull, end = e < b->phi ? e : b->phi;
    plat_mutex_lock(&lazy_lock);
    for (; p < end; p += 4096)
        if (!inbuf(b, p)) memcpy(b->buf + (p - b->lo), (const void *)(uintptr_t)p, 4096), set_inbuf(b, p);
    plat_mutex_unlock(&lazy_lock);
}

uint8_t *lazy_check(LazyBase *b) { return b->check; }

uint8_t *lazy_buf(LazyBase *b) { return b->buf; }

// The base of [x, x+n) (inside one page of the range) for the write-back: 1 in the buffer; 0 still guest memory and
// the shadow equal to it there (nothing to write back); 2 copied to tmp (the GPU changed something).
int lazy_page_base(LazyBase *b, uint64_t x, const uint8_t *s, uint64_t n, uint8_t *tmp) {
    uint64_t p = x & ~4095ull;
    if (p < b->plo || p >= b->phi) return 1;
    // Shared (MKW_WB_PAR): this only reads inbuf and guest memory; the hook, which changes both, takes the
    // lock exclusively, so the page still cannot change under the compare, and several write-backs compare at once.
    plat_mutex_lock_shared(&lazy_lock);
    int r = 1;
    if (!inbuf(b, p)) {
        r = memcmp(s, (const void *)(uintptr_t)x, n) ? 2 : 0;
        if (r == 2) memcpy(tmp, (const void *)(uintptr_t)x, n);
        plat_inc64(&s_lazy_cmp);
    }
    plat_mutex_unlock_shared(&lazy_lock);
    if (b->check) {   // the base as it would have been, against the one used
        const uint8_t *used = r == 1 ? b->buf + (x - b->lo) : r == 2 ? tmp : s, *want = b->check + (x - b->lo);
        if (memcmp(used, want, n)) {
            uint64_t d = 0;
            for (uint64_t i = 0; i < n; i++) d += used[i] != want[i];
            plat_inc64(&s_check_diff), plat_add64(&s_check_bytes, (int64_t)d);
        }
    }
    return r;
}

// Out of the registry, its pages still trapped back to read-write, and freed.
void lazy_free(LazyBase *b) {
    if (!b) return;
    plat_mutex_lock(&lazy_lock);
    for (int i = 0; i < nreg; i++)
        if (reg[i] == b) { reg[i] = reg[--nreg]; break; }
    plat_mutex_unlock(&lazy_lock);
    mem_base_release(b->plo, b->phi);
    free(b->buf), free(b->check), free(b->inbuf), free(b);
}

void lazy_log(void) {
    if (lazy_mode <= 0) return;
    rt_log("mem: lazy bases since the last line: %lld (%.1f MiB, %.1f copied from the shadow), %lld pages copied "
           "before a write, %lld pages compared with guest memory at the write-back, %lld pages trapped now; check: "
           "%lld pages with %lld bytes different", (long long)plat_xchg64(&s_lazy_n, 0),
           plat_xchg64(&s_lazy_bytes, 0) / 1048576.0, plat_xchg64(&s_lazy_eager, 0) / 1048576.0,
           (long long)plat_xchg64(&s_lazy_filled, 0), (long long)plat_xchg64(&s_lazy_cmp, 0),
           (long long)mem_base_pages(), (long long)plat_xchg64(&s_check_diff, 0),
           (long long)plat_xchg64(&s_check_bytes, 0));
}
