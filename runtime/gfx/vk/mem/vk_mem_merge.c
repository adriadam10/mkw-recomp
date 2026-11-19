#include <stdlib.h>
#include <string.h>
#ifdef __AVX2__
#include <immintrin.h>
#endif

#include "vk_mem_internal.h"

// ---------------------------------------------------------------- shadow merge by bytes (MKW_SHADOW_MERGE)
// A range a submission binds writable is the whole binding, not the bytes the GPU writes: the CPU keeps writing other
// bytes of it (each frame's indirect arguments around the instance counts a culling dispatch adds to). On the console
// both write one memory; here the sync skipped the whole range while it was pending and the write-back copied all of
// it, so the CPU's bytes never reached the GPU and were reverted in guest memory (MKW_SHADOW_LOST, lf1 06/11/2026:
// in a third of the race frames; things drawn by indirect draws appeared or vanished for one frame).
// With a base, the shadow's bytes when the part that writes the range was submitted:
// - sync (merge_in): a CPU-written byte the GPU has not changed (shadow == base) goes into the shadow and the base;
// - write-back (merge_back): only the bytes the GPU changed (shadow != base) go to guest memory.
// Ranges recorded in the part being synced are not skipped at all: the GPU has not run it.
// MKW_SHADOW_CPU_WINS (default 1 when MKW_EARLY_READ waits, as on Linux, else 0; "ab"): a byte both changed goes to the later writer, as on the console, where the GPU
// runs a submission right after it and the CPU then writes the next frame's values: guest memory away from the base
// means the CPU wrote after the part was recorded, so the sync takes it and the write-back leaves it. Without it the
// GPU wins: its write-back reaches guest memory at the fence, late, over the CPU's next values. Measured in the race
// without MKW_SUBMIT_CATCHUP; on build/cap/race it was not measurable, the black
// bush there came from the replay's memory restore.

static int cpu_wins(void) {
    static int m = -1;
    if (m < 0) {
        m = rt_knob("SHADOW_CPU_WINS", mem_early_waits() ? "1" : "0");
        if (m) rt_log("mem: a byte the CPU and the GPU both wrote goes to the later writer%s (MKW_SHADOW_CPU_WINS)",
                      m == 2 ? " in the B blocks" : "");
    }
    return RT_KNOB_ON(m);
}

// The bytes of [lo, hi) of r's shadow, or NULL past MERGE_MAX (s_nobase counts them).
uint8_t *merge_base(const PoolRec *r, uint64_t lo, uint64_t hi) {
    if (!r || !r->smap || lo < r->import_lo || hi > r->import_hi || hi - lo > MERGE_MAX) return NULL;
    uint8_t *b = malloc(hi - lo);
    if (b) memcpy(b, r->smap + (lo - r->import_lo), hi - lo);
    return b;
}

// shadow_lock held: [a, b) of r into the shadow where every base over it still equals the shadow (bases[k] holds
// [blo[k], bhi[k]) and covers [a, b)), updating the bases; the bytes come from src (a submit-time capture,
// vk_mem_capture.c) or, NULL, from guest memory. Returns the bytes copied; merge_over counts those the GPU had changed.
uint64_t merge_over;
uint64_t merge_in(PoolRec *r, uint64_t a, uint64_t b, uint8_t *const *bases, const uint64_t *blo, int n,
                  const uint8_t *src) {
    uint8_t *s = r->smap + (a - r->import_lo);
    uint64_t copied = 0;
    for (uint64_t x = a, y; x < b; x = y) {   // page by page: a trapped page holds the shadow's bytes already
        y = (x & ~0xfffull) + 0x1000;
        if (y > b) y = b;
        if (!src && mem_trapped(x)) continue;
        const uint8_t *g = src ? src + (x - a) : (const uint8_t *)(uintptr_t)x;
        uint8_t *sp = s + (x - a);
        if (!memcmp(g, sp, y - x)) continue;
        for (uint64_t i = 0; i < y - x; i++) {
            if (g[i] == sp[i]) continue;
            int gpu = 0, cpu_after = 1;   // the GPU changed it; the CPU wrote it after the base (it differs from every base)
            for (int k = 0; k < n; k++) {
                uint8_t bk = bases[k][x + i - blo[k]];
                gpu |= bk != sp[i], cpu_after &= g[i] != bk;
            }
            if (gpu && !(cpu_after && cpu_wins())) continue;
            merge_over += (uint64_t)gpu;
            sp[i] = g[i];
            for (int k = 0; k < n; k++) bases[k][x + i - blo[k]] = g[i];
            copied++;
        }
    }
    return copied;
}

// MKW_WB_CENSUS=1 (diagnosis, with MKW_TRACE=perf): merged write-backs by range size, with the span the GPU changed
// (first to last changed byte), the bytes the CPU had changed since the base, the time, and how many were the same
// range as an earlier one of the window: whether bases of whole bindings are needed (sombra-replay 4 and 5).
enum { WBC_BUCKETS = 5, WBC_HASH = 4096 };
static const uint64_t wbc_lim[WBC_BUCKETS] = {4096, 65536, 262144, 1048576, ~0ull};
static struct { uint64_t n, bytes, span, changed, cpu, ns, zero, repeat; } wbc[WBC_BUCKETS];
static uint64_t wbc_seen[WBC_HASH];
static int wbc_on = -1;
static PlatMutex wbc_lock = PLAT_MUTEX_INIT;

static void wbc_note(uint64_t addr, uint64_t size, uint64_t lo, uint64_t hi, uint64_t changed, uint64_t cpu,
                     uint64_t ns) {
    int b = 0;
    while (size > wbc_lim[b]) b++;
    uint64_t key = addr ^ size << 40, h = (key * 0x9E3779B97F4A7C15ull) >> 52;
    plat_mutex_lock(&wbc_lock);
    int rep = 0;
    for (uint64_t k = h;; k = (k + 1) & (WBC_HASH - 1)) {
        if (wbc_seen[k] == key) { rep = 1; break; }
        if (!wbc_seen[k]) { wbc_seen[k] = key; break; }
        if (((k + 1) & (WBC_HASH - 1)) == h) break;
    }
    wbc[b].n++, wbc[b].bytes += size, wbc[b].span += hi > lo ? hi - lo : 0, wbc[b].changed += changed;
    wbc[b].cpu += cpu, wbc[b].ns += ns, wbc[b].zero += !changed, wbc[b].repeat += (uint64_t)rep;
    plat_mutex_unlock(&wbc_lock);
}

void merge_census_log(void) {
    if (wbc_on <= 0) return;
    plat_mutex_lock(&wbc_lock);
    for (int b = 0; b < WBC_BUCKETS; b++)
        if (wbc[b].n)
            rt_log("mem: merged write-backs up to %llu KiB since the last line: %llu (%llu nothing changed, %llu the same "
                   "range again), %.1f MiB, GPU span %.1f MiB, GPU changed %.1f KiB, CPU changed %.1f KiB, %.2f ms",
                   (unsigned long long)(wbc_lim[b] == ~0ull ? MERGE_MAX : wbc_lim[b]) >> 10, (unsigned long long)wbc[b].n,
                   (unsigned long long)wbc[b].zero, (unsigned long long)wbc[b].repeat, wbc[b].bytes / 1048576.0,
                   wbc[b].span / 1048576.0, wbc[b].changed / 1024.0, wbc[b].cpu / 1024.0, wbc[b].ns / 1e6);
    memset(wbc, 0, sizeof wbc);
    memset(wbc_seen, 0, sizeof wbc_seen);
    plat_mutex_unlock(&wbc_lock);
}

// What a write-back did, for the census (MKW_WB_CENSUS): the first and last+1 offsets the GPU changed, the bytes it
// changed and the bytes copied (the difference is the CPU's, with cpu_wins).
typedef struct { uint64_t lo, hi, gpu, copied; } MergeStat;

// Write-back of [addr, addr+size) from shadow s: the runs the GPU changed against base, a byte at a time (MKW_MERGE_SIMD=0,
// and the reference --probe-merge checks merge_back_runs against).
// With cw (MKW_SHADOW_CPU_WINS) a byte the CPU changed since the base (guest memory away from it) stays.
// Guest memory is read only over the runs the GPU changed (watched pages through their protection), a chunk at a time:
// a copy of the whole range for each of ~200 ranges a frame (~44 KiB each, ~100 KiB of them changed) held the fence's
// waiters on the race frame's critical path (tl-cb1: ~1 ms a value in cb_done).
// With a lazy base (vk_mem_lazy.c) each guest page takes its base from lazy_page_base: a page still trapped has none
// copied, and when the shadow equals guest memory there the GPU changed nothing in it.
static void merge_back_bytes(uint64_t addr, uint64_t size, const uint8_t *s, const uint8_t *base, LazyBase *lazy, int cw,
                             MergeStat *st) {
    uint64_t copied = 0, lo = ~0ull, hi = 0, gpu = 0;
    uint64_t thawed = addr;   // guest memory below it is thawed already
    uint8_t g[512];   // guest memory over [k, k + m) of a run, with cpu_wins
    uint8_t tmp[4096];
    const uint8_t *base0 = base;
    uint64_t pend = 0;   // the end of the page of i, as an offset (lazy)
    for (uint64_t i = 0; i < size;) {
        if (lazy && i >= pend) {   // a new guest page: where its base is
            pend = ((addr + i) & ~0xfffull) + 0x1000 - addr;
            if (pend > size) pend = size;
            int r = lazy_page_base(lazy, addr + i, s + i, pend - i, tmp);
            if (!r) { i = pend; continue; }
            base = r == 2 ? (const uint8_t *)((uintptr_t)tmp - i) : base0;   // base[i..pend) is the page's
        }
        // A page at a time first: ranges of 1-4 MiB the GPU binds and never changes are ~460 MiB a 60-frame window of
        // the race (wbc2), and one memcmp call per 64 bytes cost its call each time.
        if (!(i & 4095) && size - i >= 4096 && (!lazy || pend - i >= 4096) && !memcmp(s + i, base + i, 4096)) {
            i += 4096;
            continue;
        }
        if (!(i & 63) && size - i >= 64 && (!lazy || pend - i >= 64) && !memcmp(s + i, base + i, 64)) { i += 64; continue; }
        if (s[i] == base[i]) { i++; continue; }
        uint64_t j = i + 1, jmax = lazy ? pend : size;   // [i, j): the GPU changed every byte
        while (j < jmax && s[j] != base[j]) j++;
        if (i < lo) lo = i;
        hi = j, gpu += j - i;
        for (uint64_t k = i; k < j;) {
            uint64_t m = j - k < sizeof g ? j - k : sizeof g;
            if (cw) mem_copy_out(g, addr + k, m);
            for (uint64_t a = 0; a < m;) {   // runs the CPU left alone (guest memory still at the base)
                if (cw && g[a] != base[k + a]) { a++; continue; }
                uint64_t b = a + 1;
                while (b < m && !(cw && g[b] != base[k + b])) b++;
                // Once a page, not once a run: a range the GPU rewrites (vertex data, ~45 KiB) has thousands of runs
                // broken by bytes that happen to stay equal, and mem_thaw takes frz_lock (wbc2: 154 ms per 60 frames
                // for 277 such ranges, 3.9 MiB changed).
                uint64_t x = addr + k + a, y = x + (b - a);
                if (y > thawed) {
                    uint64_t from = x > thawed ? x & ~0xfffull : thawed;
                    thawed = (y + 0xfff) & ~0xfffull;
                    mem_thaw(from, thawed - from);
                }
                memcpy((void *)(uintptr_t)x, s + k + a, b - a);
                copied += b - a;
                a = b;
            }
            k += m;
        }
        i = j;
    }
    *st = (MergeStat){lo, hi, gpu, copied};
}

// Bit i set: a[i] != b[i], for n <= 64 bytes. With AVX2 (the x86-64 build is x86-64-v3, tools/build/build.py) two
// compares a 64-byte block; elsewhere the compiler's own loop.
static inline uint64_t ne_mask(const uint8_t *a, const uint8_t *b, uint64_t n) {
#ifdef __AVX2__
    if (n == 64) {
        __m256i e0 = _mm256_cmpeq_epi8(_mm256_loadu_si256((const __m256i *)a), _mm256_loadu_si256((const __m256i *)b));
        __m256i e1 = _mm256_cmpeq_epi8(_mm256_loadu_si256((const __m256i *)(a + 32)),
                                       _mm256_loadu_si256((const __m256i *)(b + 32)));
        return ~((uint64_t)(uint32_t)_mm256_movemask_epi8(e0) | (uint64_t)(uint32_t)_mm256_movemask_epi8(e1) << 32);
    }
#endif
    uint64_t m = 0;
    for (uint64_t i = 0; i < n; i++) m |= (uint64_t)(a[i] != b[i]) << i;
    return m;
}

// The run being gathered: [rs, re) as offsets, copied to guest memory when the next one does not follow it.
typedef struct { uint64_t addr, thawed, rs, re, copied; const uint8_t *s; } RunOut;

static void run_flush(RunOut *o) {
    if (o->rs == o->re) return;
    uint64_t x = o->addr + o->rs, y = o->addr + o->re;
    if (y > o->thawed) {   // once a page, as merge_back_bytes: mem_thaw takes frz_lock
        uint64_t from = x > o->thawed ? x & ~0xfffull : o->thawed;
        o->thawed = (y + 0xfff) & ~0xfffull;
        mem_thaw(from, o->thawed - from);
    }
    memcpy((void *)(uintptr_t)x, o->s + o->rs, o->re - o->rs);
    o->copied += o->re - o->rs;
    o->rs = o->re;
}

static inline void run_add(RunOut *o, uint64_t a, uint64_t b) {
    if (o->re != a) run_flush(o), o->rs = a;
    o->re = b;
}

// The same write-back as merge_back_bytes, guest page by guest page: a page equal to its base is one memcmp; in one
// that is not, a mask of the bytes the GPU changed per 64-byte block (and, with cw, of those where guest memory is still
// at the base, read once for the rest of the page) gives the runs to copy, gathered across blocks and pages into one
// memcpy each. merge_back_bytes walked such pages a byte at a time, twice with cw, and called memcpy for every run
// between bytes that stayed equal: in a 2-player race, ranges of 4-64 KiB were 92 of the 159 ms per 60 frames of
// merges, at ~2 GB/s, on the completion thread before the fence's waiters are released (rpwb-0, MKW_WB_CENSUS,
// i7-14700F, 18/11/2026). The bytes written are the same set: those the GPU changed, less the CPU's with cw
// (--probe-merge). In-run A/B with its inverted twin, the same race, serial replay (ms2p): merges 2.55-2.62 -> 1.53-1.68
// ms a frame, the 4-64 KiB ones 88-90 -> 38-45 ms per 60 frames; the fence's waiters released 1.02 ms sooner after the
// GPU, the frame -0.52 ms (there at the 60 fps cap: B's median 16.8-16.9 ms). Left: 1-4 MiB ranges the GPU did not
// change, ~30 ms per 60 frames of memcmp at memory speed.
static void merge_back_runs(uint64_t addr, uint64_t size, const uint8_t *s, const uint8_t *base, LazyBase *lazy, int cw,
                            MergeStat *st) {
    RunOut o = {.addr = addr, .thawed = addr, .s = s};
    uint64_t lo = ~0ull, hi = 0, gpu = 0;
    uint8_t g[4096];   // guest memory over [gx, pend), with cw
    uint8_t tmp[4096];
    for (uint64_t i = 0; i < size;) {
        uint64_t pend = ((addr + i) & ~0xfffull) + 0x1000 - addr;
        if (pend > size) pend = size;
        const uint8_t *b = base;   // b[i..pend) is the page's base
        if (lazy) {
            int r = lazy_page_base(lazy, addr + i, s + i, pend - i, tmp);
            if (!r) { i = pend; continue; }
            if (r == 2) b = (const uint8_t *)((uintptr_t)tmp - i);
        }
        if (!memcmp(s + i, b + i, pend - i)) { i = pend; continue; }
        uint64_t gx = pend;   // guest memory read from gx (none yet)
        for (uint64_t x = i; x < pend; x += 64) {
            uint64_t n = pend - x < 64 ? pend - x : 64;
            uint64_t m = ne_mask(s + x, b + x, n);
            if (!m) continue;
            if (x + (uint64_t)__builtin_ctzll(m) < lo) lo = x + (uint64_t)__builtin_ctzll(m);
            hi = x + 64 - (uint64_t)__builtin_clzll(m), gpu += (uint64_t)__builtin_popcountll(m);
            if (cw) {
                if (gx == pend) gx = x, mem_copy_out(g, addr + x, pend - x);
                m &= ~ne_mask(g + (x - gx), b + x, n);   // the CPU's bytes stay
            }
            while (m) {
                unsigned a = (unsigned)__builtin_ctzll(m);
                uint64_t rest = ~(m >> a);   // a < 64; zero only when every bit from 0 on is set
                unsigned len = rest ? (unsigned)__builtin_ctzll(rest) : 64;
                run_add(&o, x + a, x + a + len);
                m = a + len >= 64 ? 0 : m & ~0ull << (a + len);
            }
        }
        i = pend;
    }
    run_flush(&o);
    *st = (MergeStat){lo, hi, gpu, o.copied};
}

// MKW_WB_PAR (vk_wb_par.c): a large range in slices cut at multiples of SLICE of the guest address, each
// the same write-back of its own bytes (merge_back_runs works guest page by guest page and keeps nothing across pages
// but the run being gathered, which a slice end flushes).
#define SLICE (128ull << 10)
#define SLICE_MIN (512ull << 10)
typedef struct { uint64_t addr, size; const uint8_t *s, *base; LazyBase *lazy; int cw; uint64_t off[66]; MergeStat st[65]; } SliceJob;

static void slice_one(void *ctx, int i) {
    SliceJob *j = ctx;
    uint64_t o = j->off[i];
    merge_back_runs(j->addr + o, j->off[i + 1] - o, j->s + o, j->base ? j->base + o : NULL, j->lazy, j->cw, &j->st[i]);
}

static int merge_back_sliced(uint64_t addr, uint64_t size, const uint8_t *s, const uint8_t *base, LazyBase *lazy, int cw,
                             MergeStat *st) {
    if (size < SLICE_MIN || size > 64 * SLICE || vk_wb_par_inside() || vk_wb_par_threads() <= 0) return 0;
    static _Thread_local SliceJob j;   // ~2.6 KiB; one caller at a time per thread
    j.addr = addr, j.size = size, j.s = s, j.base = base, j.lazy = lazy, j.cw = cw;
    int n = 0;
    for (uint64_t o = 0; o < size;) {
        j.off[n++] = o;
        o = ((addr + o) / SLICE + 1) * SLICE - addr;
    }
    j.off[n] = size;
    vk_wb_par(n, slice_one, &j);
    *st = (MergeStat){~0ull, 0, 0, 0};
    for (int i = 0; i < n; i++) {
        if (j.st[i].gpu && j.off[i] + j.st[i].lo < st->lo) st->lo = j.off[i] + j.st[i].lo;
        if (j.st[i].gpu && j.off[i] + j.st[i].hi > st->hi) st->hi = j.off[i] + j.st[i].hi;
        st->gpu += j.st[i].gpu, st->copied += j.st[i].copied;
    }
    return 1;
}

// MKW_MERGE_SIMD (default 1; "ab"): merge_back_runs; 0: merge_back_bytes. Returns the bytes copied.
uint64_t merge_back(uint64_t addr, uint64_t size, const uint8_t *s, const uint8_t *base, LazyBase *lazy) {
    static int simd = -1;
    if (simd < 0) simd = rt_knob("MERGE_SIMD", "1");
    if (wbc_on < 0) wbc_on = rt_knob("WB_CENSUS", "0");
    uint64_t t0 = wbc_on > 0 ? qpc() : 0;
    MergeStat st;
    if (RT_KNOB_ON(simd) && merge_back_sliced(addr, size, s, base, lazy, cpu_wins(), &st)) {}
    else if (RT_KNOB_ON(simd)) merge_back_runs(addr, size, s, base, lazy, cpu_wins(), &st);
    else merge_back_bytes(addr, size, s, base, lazy, cpu_wins(), &st);
    if (wbc_on > 0) wbc_note(addr, size, st.lo, st.hi, st.gpu, st.gpu - st.copied, qpc() - t0);
    return st.copied;
}

// --probe-merge (merge_probe.c): one write-back of host memory at addr (above RT_ADDRESS_LIMIT, where mem_thaw does
// nothing) by either implementation, without a lazy base; gpu: the bytes the GPU changed.
uint64_t vk_merge_back_probe(int runs, int cw, uint64_t addr, uint64_t size, const uint8_t *s, const uint8_t *base,
                             uint64_t *gpu) {
    MergeStat st;
    if (runs) merge_back_runs(addr, size, s, base, NULL, cw, &st);
    else merge_back_bytes(addr, size, s, base, NULL, cw, &st);
    *gpu = st.gpu;
    return st.copied;
}
