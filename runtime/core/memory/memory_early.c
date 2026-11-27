// MKW_EARLY_READ=1 (diagnosis): does the game read memory the GPU has yet to write? With the replay ~3 submits behind
// the game, what the GPU writes (copies into buffers, counters, cleared buffers, storage buffers a shader may write)
// reaches memory later than on the console, and the game decides what it draws with some of it (menus:
// trees and cacti dark for a frame). MKW_SUBMIT_CATCHUP makes every submit wait for its GPU work instead, at ~10 ms a
// frame. Here the destinations a submit lists (nvn_api_cmd.c resolve_deferred) are made inaccessible at the submit, with
// that submit's fence; the first access to such a page puts its protection back and is counted: early when the GPU has
// not finished the submit, late when it has, by a game thread or by the runtime. Nothing waits: it only measures where a
// wait would be needed. Linux and Windows (the watch map, memory.c mem_watch_init). One summary line every 600 frames
// with the first early reads.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "kernel_int.h"
#include "memory_int.h"

#define PAGE 0x1000ull
#define EW_KINDS 6
#define EW_FIRST 12

static const char *const kind_name[EW_KINDS] = {"counter", "copy", "texture to buffer", "clear", "storage buffer", "uniform update"};
int64_t mem_early_pages;
static uint64_t *meta;   // per guest page: fence << 8 | kind << 4 | protection before the watch; mapped lazily
// MKW_EARLY_READ=3: per guest page, the last submit listed as writing it and the one before. On the console a read
// right after submit N finds N-1's results (the GPU has only begun N): the console-like wait is for the previous writer
// when it has not finished, not for N (=2 waited for N: ~1 wait a frame of 7-16 ms, ews5).
uint64_t *mem_lastf, *mem_prevf;
int mem_fence_early;
uint64_t mem_early_rel;
// Per guest page, the 64-byte pieces the two newest recorded writers write (fence, mask): which bytes an unfinished
// writer leaves pending, for mem_early_invalidate. wm_lock.
typedef struct { uint64_t f[2], m[2]; } WMask;
static WMask *wmask;
static PlatMutex wm_lock = PLAT_MUTEX_INIT;
uint64_t (*mem_fence_done)(void), (*mem_fence_queued)(void);
static int (*fence_wait)(uint64_t value, uint64_t timeout_ns);
static int wait_mode;                 // MKW_EARLY_READ=2: an early read of bytes the GPU writes waits for its submit
static int64_t waits, wait_ns;        // since the last log line
static int64_t range_skips;           // frz_lock: watches released by mem_early_invalidate since the last log line
// frz_lock: the early reads that would wait, by whether the writer waited for writes the 64-byte piece read: yes, no
// (not waited for, with MKW_EARLY_READ_RANGE on), unknown (that writer's pieces not reported yet: waited for)
static int64_t piece_cnt[4];   // the waited writer writes the piece read, not, unknown; known from its submit
// The wait the fault on this thread asked for, logged once it ends when it was long: which page, who read it and how
// far behind the GPU was (lwab1: ~6 waits of ~7.5 ms every 600 frames, each a one-frame hitch).
static _Thread_local struct { uint64_t page, pc, done, last, frame, addr; int diff; } wq;
static int nwait_logged;
static int64_t cnt[EW_KINDS][4];   // frz_lock: early read by the game, early write by the game, late by the game, by
                                   // the runtime
static struct { uint64_t page, fence, done, frame, pc; int kind, write; } first[EW_FIRST];
static int nfirst;
uint64_t mem_early_frame;   // the last frame mem_early_log saw
// A bound storage buffer is listed whether or not its shader writes it. What the GPU does write the replay reports
// later (mem_early_gpu_wrote, from vk_mem_gpu_pending), so the game's early reads are kept and told apart at the log:
// of a page the GPU writes (a real early read) or of one it only reads (no wait needed).
#define EW_RING (1u << 16)
static uint64_t *gpu_wrote;   // per guest page, the 64-byte pieces the GPU writes (a bit each); mapped lazily
static struct { uint64_t addr, pc; } ring[EW_RING];   // the byte read: told against the pieces the GPU writes
static uint32_t nring;
static struct { uint64_t pc, n; } rt_pc[8];   // frz_lock: where the runtime's own touches come from (a fault each)

// MKW_EARLY_READ: 3 by default, where with MKW_SHADOW_CPU_WINS it stands for MKW_SUBMIT_CATCHUP (mc1: as few cacti and
// cheaper). Windows has no read traps but has the watches since 27/11/2026 (memory.c mem_watch_init, win32_mem.c
// fault_veh): with them the game's submits no longer wait for their replay and GPU work, and MKW_FENCE_EARLY can let the
// game build its next frame while the GPU finishes the last one (the frame was the three in a row there, 18-19 ms in a
// 24-kart Grand Prix on an RX 6600M). A value outside 0-3 warns and takes the default.
static int early_cfg(void) {
    const char *e = cfg_get("MKW_EARLY_READ");
    int v = e && *e ? atoi(e) : 3;
    if (v < 0 || v > 3) {
        static int warned;
        if (!warned++) rt_log("mem: MKW_EARLY_READ=%s is not 0-3: 3", e);
        v = 3;
    }
    return v;
}

int mem_early_on(void) {
    static int on = -1;
    if (on < 0) {
        on = early_cfg() && mem_watch_init();
        wait_mode = on ? early_cfg() - 1 : 0;   // 1 for N, 2 for the previous writer
        if (wait_mode < 0 || wait_mode > 2) wait_mode = 0;
        // Per guest page over the whole guest range (~6 GiB in all): lazily backed, a few MiB are ever touched
        if (on && (!(meta = plat_mem_alloc_lazy(RT_ADDRESS_LIMIT / PAGE * sizeof *meta)) ||
                   !(gpu_wrote = plat_mem_alloc_lazy(RT_ADDRESS_LIMIT / PAGE * sizeof *gpu_wrote)) ||
                   !(mem_lastf = plat_mem_alloc_lazy(RT_ADDRESS_LIMIT / PAGE * sizeof *mem_lastf)) ||
                   !(mem_prevf = plat_mem_alloc_lazy(RT_ADDRESS_LIMIT / PAGE * sizeof *mem_prevf)) ||
                   !(wmask = plat_mem_alloc_lazy(RT_ADDRESS_LIMIT / PAGE * sizeof *wmask)))) {
            rt_log("mem: early-read watch off: its page tables could not be reserved");
            on = 0;
        }
        if (on) rt_log("mem: early-read watch on (MKW_EARLY_READ=%d): what a submit's GPU work writes is watched until touched%s",
                       wait_mode + 1, wait_mode == 1 ? "; an early read of bytes the GPU writes waits for that submit"
                                      : wait_mode == 2 ? "; an early read waits for the previous submit writing them" : "");
    }
    return on;
}

// The runtime's reads of guest memory that may hold watched pages (the shadow sync, the uniform snapshot): a watched page
// is read through its protection, so the watch stays for the game (ews4: 96 641 faults by shadow_copy in 600 frames,
// each lifting a watch the game's early read would then pass).
void mem_copy_out(void *dst, uint64_t a, uint64_t n) {
    if (mem_early_on() <= 0 || !watched(a, n)) {
        memcpy(dst, (const void *)(uintptr_t)a, n);
        return;
    }
    for (uint64_t x = a, y; x < a + n; x = y) {
        int w = __atomic_load_n(&trapped[x / PAGE], __ATOMIC_RELAXED) == 2;
        for (y = (x & ~(PAGE - 1)) + PAGE; y < a + n && (__atomic_load_n(&trapped[y / PAGE], __ATOMIC_RELAXED) == 2) == w; y += PAGE) {}
        if (y > a + n) y = a + n;
        uint8_t *d = (uint8_t *)dst + (x - a);
        if (!w) memcpy(d, (const void *)(uintptr_t)x, y - x);
        else if (!plat_mem_peek(d, x, y - x)) memcpy(d, (const void *)(uintptr_t)x, y - x);   // falls back to the fault
    }
}

void mem_early_set_done(uint64_t (*done)(void), int (*wait)(uint64_t, uint64_t), uint64_t (*queued)(void)) {
    mem_fence_done = done, fence_wait = wait, mem_fence_queued = queued;
}

// The fault's wait, after frz_lock is released (the GPU's bytes come back under it, on the completion thread, before
// the fence counts as done): the read is then retried and finds them, or a read trap that fills it.
void mem_early_wait(uint64_t fence) {
    static uint8_t before[PAGE], after[PAGE];   // MKW_EARLY_READ_DRY=3: the page without and with the wait
    static PlatMutex diff_lock = PLAT_MUTEX_INIT;
    int diff = wq.diff && plat_mutex_trylock(&diff_lock);
    if (diff) plat_mem_peek(before, wq.page, PAGE);
    uint64_t t0 = plat_now_ns();
    if (fence_wait) fence_wait(fence, 500000000ull);
    uint64_t ns = plat_now_ns() - t0;
    if (diff) {   // the page as the game reads it now: a trap left by the write-back is filled through the fault
        mem_copy_out(after, wq.page, PAGE);
        int nd = 0, lo = -1, hi = -1;
        for (int i = 0; i < (int)PAGE; i++)
            if (before[i] != after[i]) nd++, hi = i, lo = lo < 0 ? i : lo;
        int o = (int)(wq.addr & (PAGE - 1) & ~15ull);
        rt_log("mem: early-read wait at frame %llu changed %d bytes of page %#llx (offsets %#x-%#x); at %#llx without "
               "the wait %02x%02x%02x%02x %02x%02x%02x%02x, with it %02x%02x%02x%02x %02x%02x%02x%02x",
               (unsigned long long)wq.frame, nd, (unsigned long long)wq.page, lo, hi, (unsigned long long)(wq.page + o),
               before[o], before[o + 1], before[o + 2], before[o + 3], before[o + 4], before[o + 5], before[o + 6],
               before[o + 7], after[o], after[o + 1], after[o + 2], after[o + 3], after[o + 4], after[o + 5],
               after[o + 6], after[o + 7]);
        plat_mutex_unlock(&diff_lock);
    }
    __atomic_add_fetch(&waits, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&wait_ns, (int64_t)ns, __ATOMIC_RELAXED);
    // the close criterion: 1 ms; every wait inside a MKW_PROBE window
    if ((ns > 1000000 && __atomic_fetch_add(&nwait_logged, 1, __ATOMIC_RELAXED) < 60) || seqwatch_probe())
        rt_log("mem: early-read wait %.1f ms at frame %llu: page %#llx for fence %llu (GPU at %llu, its newest writer %llu), "
               "by exe+%#llx", (double)ns / 1e6, (unsigned long long)wq.frame, (unsigned long long)wq.page,
               (unsigned long long)fence, (unsigned long long)wq.done, (unsigned long long)wq.last,
               (unsigned long long)(wq.pc - plat_exe_base()));
}

// The 64-byte pieces of page p that [a, end) covers, a bit each.
static uint64_t page_bits(uint64_t p, uint64_t a, uint64_t end) {
    uint64_t lo = p > a ? p : a, hi = p + PAGE < end ? p + PAGE : end;
    unsigned first = (unsigned)(lo % PAGE / 64), last = (unsigned)((hi - 1) % PAGE / 64);
    return (~0ull >> (63 - last)) & (~0ull << first);
}

// One atomic a page, and none for a page already all marked: one a 64-byte piece cost the replay thread ~3 % of its
// samples (def3, 08/11/2026: the 1-4 MiB ranges the shadow binds are 16-64 K pieces each).
void mem_early_gpu_wrote(uint64_t a, uint64_t n, uint64_t fence) {
    if (!n || mem_early_on() <= 0 || a >= RT_ADDRESS_LIMIT) return;
    uint64_t end = a + n < RT_ADDRESS_LIMIT ? a + n : RT_ADDRESS_LIMIT;
    for (uint64_t p = a & ~(PAGE - 1); p < end; p += PAGE) {
        uint64_t bits = page_bits(p, a, end);
        if ((__atomic_load_n(&gpu_wrote[p / PAGE], __ATOMIC_RELAXED) & bits) != bits)
            __atomic_fetch_or(&gpu_wrote[p / PAGE], bits, __ATOMIC_RELAXED);
    }
    if (!fence || !wmask) return;
    plat_mutex_lock(&wm_lock);
    for (uint64_t p = a & ~(PAGE - 1); p < end; p += PAGE) {
        WMask *w = &wmask[p / PAGE];
        uint64_t bits = page_bits(p, a, end);
        if (w->f[0] != fence) {
            if (fence < w->f[0]) continue;   // recorded out of order: left unknown
            w->f[1] = w->f[0], w->m[1] = w->m[0], w->f[0] = fence, w->m[0] = 0;
        }
        w->m[0] |= bits;
    }
    plat_mutex_unlock(&wm_lock);
}

// The game is about to read [a, a+n) of a CPU-cached pool (nvnMemoryPoolInvalidateMappedRange: on the console it
// reads nothing else of it before the next invalidate). A watched page whose pending writer (the one =3 would wait
// for) writes none of those bytes is released without a wait: the wait was for other bytes sharing the page (tdiff1:
// the counters at 0x2a9bdcf00 made the reads of 0x2a9bdc200, written a submission earlier and done, wait 6-13 ms every
// 68 frames). An unknown mask (that writer not recorded yet) keeps the watch. MKW_EARLY_READ_RANGE (default 1; "ab").
// With MKW_FENCE_EARLY a read may wait for the newest writer too, so both pending writers are looked at: the game's
// readbacks (f_7000457da0, f_7000032980, f_700056b520) invalidate one copy of a rotating buffer, written two frames
// before, while the frame before writes the next copy, which shares its edge pages (sim2-ahead35b: ~16 ms of waits a
// frame for those). On the console the GPU has finished the frame before when the game reads (it waits for the fence
// of the frame's submit at the start of the next one, f_70025ec50c), so those bytes are never waited for there.
// The pieces of page p writer f writes, in *m: 2 known from its submit (MKW_EARLY_SUBMASK, memory_early_sub.c; with
// what the replay recorded of it, should it hold more), 1 from the replay's recording alone, 0 unknown. frz_lock held.
static int writer_mask(uint64_t p, uint64_t f, uint64_t *m) {
    uint64_t sb = 0;
    int sk = mem_early_sub_bits(p, f, &sb);
    plat_mutex_lock(&wm_lock);
    const WMask *w = &wmask[p / PAGE];
    int rk = w->f[0] == f || w->f[1] == f;
    uint64_t rm = !rk ? 0 : w->f[0] == f ? w->m[0] : w->m[1];
    plat_mutex_unlock(&wm_lock);
    *m = sb | rm;
    return sk ? 2 : rk;
}

static int pending_writes(uint64_t p, uint64_t f, uint64_t done, uint64_t want) {   // 1 if f writes want or is unknown
    if (!f || f <= done) return 0;
    uint64_t m;
    return !writer_mask(p, f, &m) || (m & want);
}

static int range_on(void) {   // MKW_EARLY_READ_RANGE (default 1; "ab"): wait only for a writer of the bytes read
    static int on = -1;
    if (on < 0) on = rt_knob("EARLY_READ_RANGE", "1");
    return RT_KNOB_ON(on);
}

void mem_early_invalidate(uint64_t a, uint64_t n) {
    if (!n || wait_mode != 2 || !wmask || !range_on() || !watched(a, n) || !mem_fence_done) return;
    uint64_t done = mem_fence_done();
    int early = __atomic_load_n(&mem_fence_early, __ATOMIC_ACQUIRE);
    plat_mutex_lock(&frz_lock);
    for (uint64_t p = a & ~(PAGE - 1); p < a + n; p += PAGE) {
        uint64_t prev = mem_prevf[p / PAGE], last = early ? mem_lastf[p / PAGE] : 0;
        if (trapped[p / PAGE] != 2 || (prev <= done && last <= done)) continue;
        uint64_t lo = p > a ? p : a, hi = p + PAGE < a + n ? p + PAGE : a + n, want = 0;
        for (uint64_t x = lo & ~63ull; x < hi; x += 64) want |= 1ull << (x % PAGE / 64);
        if (pending_writes(p, prev, done, want) || pending_writes(p, last, done, want)) continue;
        mem_early_release_locked(p, 0);
        range_skips++;
    }
    plat_mutex_unlock(&frz_lock);
}

void mem_early_watch(uint64_t a, uint64_t n, uint64_t fence, int kind) {
    if (!n || !mem_early_on() || !watched(a, n)) return;
    static int skip = -1;   // MKW_EARLY_READ_SKIP (default 1; "ab"): the rule below
    if (skip < 0) skip = rt_knob("EARLY_READ_SKIP", "1");
    uint64_t done = wait_mode == 2 && mem_fence_done && RT_KNOB_ON(skip) ? mem_fence_done() : 0;
    plat_mutex_lock(&frz_lock);
    for (uint64_t p = a & ~(PAGE - 1); p < a + n; p += PAGE) {
        if (mem_lastf[p / PAGE] != fence) mem_prevf[p / PAGE] = mem_lastf[p / PAGE], mem_lastf[p / PAGE] = fence;
        // =3 waits only for the previous writer: with it finished, no read of the page can wait, and a watch would only
        // cost faults (cwb3: ~37,000 touches and 12 waits every 600 frames; the kernel went from 13 to 16-19 % of the
        // game thread).
        // (MKW_FENCE_EARLY: this fence's own bytes may be read after its early release)
        if (done && mem_prevf[p / PAGE] <= done && !mem_fence_early) continue;
        uint8_t t = trapped[p / PAGE];
        if (t == 1) continue;   // the GPU's bytes of an earlier fence are still pending: a read trap already
        if (t == 3) mem_base_fill_locked(p), t = 0;   // a base's page: its bytes go to the base, then it is watched
        if (t == 4) mem_spill_fill_locked(p, 2), t = 0;   // a spilled page: back from the file, then it is watched
        // With waits, only pages the GPU has written in earlier frames (a frame repeats the last one): a bound storage
        // buffer the shader only reads costs a fault per touch, by the game and by the shadow sync (ews1: ~270 a frame).
        if (wait_mode && !gpu_wrote[p / PAGE]) continue;
        if (t == 2) {           // watched already: the newer submit's fence
            meta[p / PAGE] = fence << 8 | (meta[p / PAGE] & 0xff);
            continue;
        }
        uint64_t end;
        int st = plat_mem_state(p, p + PAGE, &end);
        if (st != PLAT_PROT_RW && st != PLAT_PROT_READ) continue;   // a hole or a guest protection
        trapped[p / PAGE] = 2;
        meta[p / PAGE] = fence << 8 | (uint64_t)kind << 4 | (uint64_t)st;
        if (!plat_mem_protect(p, PAGE, PLAT_PROT_NONE)) {
            trapped[p / PAGE] = 0;
            continue;
        }
        mem_early_pages++;
    }
    plat_mutex_unlock(&frz_lock);
}

uint64_t mem_early_release_locked(uint64_t a, int access) {
    uint64_t p = a & ~(PAGE - 1);
    uint64_t m = meta[p / PAGE], fence = m >> 8;
    int kind = (int)(m >> 4 & 0xf);
    if (!plat_mem_protect(p, PAGE, (int)(m & 0xf))) rt_fatal("memory: early-read watch: page %#llx kept no access", (unsigned long long)p);
    trapped[p / PAGE] = 0;
    mem_early_pages--;
    if (!access || kind >= EW_KINDS) return 0;
    uint64_t done = mem_fence_done ? mem_fence_done() : 0;
    int early = fence > done, who = !tcur ? 3 : !early ? 2 : plat_fault_write;
    cnt[kind][who]++;
    if (early && tcur && !plat_fault_write && nring < EW_RING) ring[nring].addr = a, ring[nring++].pc = plat_fault_pc;
    // Outside the executable (libc's memcpy, a leaf): its caller, the return address at the stack top.
    uint64_t at = plat_fault_pc - plat_exe_base() < (1ull << 32) ? plat_fault_pc : plat_fault_ret;
    if (!tcur)
        for (int j = 0; j < 8; j++)
            if (!rt_pc[j].n || rt_pc[j].pc == at) {
                rt_pc[j].pc = at, rt_pc[j].n++;
                break;
            }
    if (early && tcur && !plat_fault_write && nfirst < EW_FIRST) {   // the reads, with the code that made them
        first[nfirst].page = p, first[nfirst].fence = fence, first[nfirst].done = done, first[nfirst].kind = kind;
        first[nfirst].frame = mem_early_frame, first[nfirst].pc = plat_fault_pc, first[nfirst].write = 0;
        nfirst++;
    }
    // Waits only where the console's game would find the GPU's bytes: a read by the game, before the GPU finished the
    // submit, of a 64-byte piece the GPU writes (as earlier frames showed: a frame repeats the last one's work).
    if (wait_mode && early && tcur && !plat_fault_write && (gpu_wrote[p / PAGE] >> (a % PAGE / 64) & 1)) {
        wq.page = p, wq.pc = at, wq.done = done, wq.last = mem_lastf[p / PAGE], wq.frame = mem_early_frame;
        uint64_t w = wait_mode == 1 ? fence : mem_prevf[p / PAGE] > done ? mem_prevf[p / PAGE] : 0;
        uint64_t last = mem_lastf[p / PAGE];   // MKW_FENCE_EARLY: a writer whose fence the game was told is done
        if (wait_mode == 2 && mem_fence_early && last > done && last <= __atomic_load_n(&mem_early_rel, __ATOMIC_ACQUIRE))
            w = last;
        // The same rule as mem_early_invalidate at the fault, for reads with no invalidate before them (a pool the
        // CPU does not cache: the game reads it without one, f_7000457da0's GetFlags test): the piece read decides.
        if (w && wmask && wait_mode == 2) {
            uint64_t m;
            int known = writer_mask(p, w, &m), hit = known && (m >> (a % PAGE / 64) & 1);
            piece_cnt[!known ? 2 : hit ? 0 : 1]++;
            if (known == 2) piece_cnt[3]++;
            if (known && !hit && range_on()) return 0;
        }
        static int dry = -1;   // 1 skip (2 in the B blocks), 3 ("diff") wait and log what the wait changed
        if (dry < 0) {
            const char *e = cfg_get("MKW_EARLY_READ_DRY");
            dry = e && !strcmp(e, "diff") ? 3 : rt_knob("EARLY_READ_DRY", "0");
        }
        if (w && dry < 3 && RT_KNOB_ON(dry)) {
            mem_early_ev_note(0, a, w, done, at);
            return 0;
        }
        wq.addr = a, wq.diff = dry == 3;
        return w;
    }
    return 0;
}

void mem_early_log(uint64_t frame) {   // every frame
    mem_early_frame = frame;
    mem_early_ev_log();
    if (mem_early_on() <= 0 || frame % 600) return;
    char b[1024];
    int k = 0;
    plat_mutex_lock(&frz_lock);
    for (int i = 0; i < EW_KINDS && k < (int)sizeof b - 96; i++)
        if (cnt[i][0] || cnt[i][1] || cnt[i][2] || cnt[i][3])
            k += snprintf(b + k, sizeof b - (size_t)k, "; %s %lld early reads %lld early writes %lld late %lld runtime",
                          kind_name[i], (long long)cnt[i][0], (long long)cnt[i][1], (long long)cnt[i][2], (long long)cnt[i][3]);
    char ev[160];
    mem_early_ev_counts(ev, sizeof ev);
    rt_log("mem: early reads at frame %llu (%lld pages watched now)%s; waits %lld, %.1f ms in all; released by an "
           "invalidate of other bytes %lld; the waited writer writes the piece read %lld, not %lld, unknown %lld (known "
           "at its submit %lld); %s",
           (unsigned long long)frame, (long long)mem_early_pages, k ? b : ": none", (long long)waits,
           (double)wait_ns / 1e6, (long long)range_skips, (long long)piece_cnt[0], (long long)piece_cnt[1],
           (long long)piece_cnt[2], (long long)piece_cnt[3], ev);
    waits = wait_ns = range_skips = 0;
    memset(piece_cnt, 0, sizeof piece_cnt);
    // The early reads by whether the GPU writes the page (as far as the replay has reported), with the code making them.
    uint32_t wrote = 0, page_only = 0;
    uint64_t pcs[8] = {0}, pcn[8] = {0};
    for (uint32_t i = 0; i < nring; i++) {
        uint64_t m = gpu_wrote[ring[i].addr / PAGE];
        if (!(m >> (ring[i].addr % PAGE / 64) & 1)) {
            page_only += m != 0;
            continue;
        }
        wrote++;
        for (int j = 0; j < 8; j++)
            if (!pcn[j] || pcs[j] == ring[i].pc) {
                pcs[j] = ring[i].pc, pcn[j]++;
                break;
            }
    }
    char w[512];
    int o = 0;
    for (int j = 0; j < 8 && pcn[j] && o < (int)sizeof w - 48; j++)
        o += snprintf(w + o, sizeof w - (size_t)o, " exe+%#llx x%llu", (unsigned long long)(pcs[j] - plat_exe_base()),
                      (unsigned long long)pcn[j]);
    rt_log("mem: early reads of bytes the GPU writes: %u of %u (%u more of other bytes in pages it writes)%s%s", wrote,
           nring, page_only, wrote ? ", by" : "", o ? w : "");
    nring = 0;
    o = 0;
    for (int j = 0; j < 8 && rt_pc[j].n && o < (int)sizeof w - 48; j++)
        o += snprintf(w + o, sizeof w - (size_t)o, " exe+%#llx x%llu", (unsigned long long)(rt_pc[j].pc - plat_exe_base()),
                      (unsigned long long)rt_pc[j].n);
    if (o) rt_log("mem: early-read watch: the runtime's touches by%s", w);
    memset(rt_pc, 0, sizeof rt_pc);
    for (int i = 0; i < nfirst; i++)
        rt_log("mem: early read: page %#llx (%s) at frame %llu, fence %llu, GPU at %llu, by exe+%#llx", (unsigned long long)first[i].page,
               kind_name[first[i].kind], (unsigned long long)first[i].frame, (unsigned long long)first[i].fence,
               (unsigned long long)first[i].done, (unsigned long long)(first[i].pc - plat_exe_base()));
    memset(cnt, 0, sizeof cnt);
    nfirst = 0;
    plat_mutex_unlock(&frz_lock);
}

int mem_early_waits(void) { return mem_early_on() > 0 && wait_mode; }
