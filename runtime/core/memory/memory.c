// Adapted from MacNativeGames games/totk/runtime/services.c and loader.c @ b75be8c (MIT, Copyright (c) 2026
// MacNativeGames contributors), and LAS Recomp runtime/main.c and platform/windows.c @ da35b83 (MIT, Copyright (c)
// 2026 Peiikai).
// Guest address space and the memory SVCs: heap, module data permissions, stack moves,
// QueryMemory, physical memory in the alias region, GetInfo memory ids, and the kernel-region allocator.
//
// Every region is a plain reservation; pages are committed and decommitted at 4 KiB granularity. Decommitted pages
// come back zeroed, so no stale-page bookkeeping is needed. The stack and heap block and the alias region are
// reserved write-watched (platform.h plat_mem_reserve): mem_written_pages reports the pages the CPU wrote, which
// the Vulkan back end uploads again.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "gamelib.h"
#include "platform/platform.h"
#include "memory_int.h"

#define PAGE 0x1000ull
#define GiB (1ull << 30)
#define LOW_SEARCH_FROM 0x100000000ull
#define LOW_SIZE (14 * GiB)        // stack + heap block: the heap of a Switch 2 title nears 8 GiB
#define STACK_OFF 0x10000000ull
#define STACK_SIZE 0x10000000ull
#define HEAP_OFF (STACK_OFF + STACK_SIZE + 0x10000000ull)
#define HEAP_CAP (12 * GiB)
#define ALIAS_SIZE (64 * GiB)
#define ALIAS_ALIGN (4 * GiB)
#define ALIAS_PAGES (ALIAS_SIZE / PAGE)
// Application pool of a 12 GB Switch 2: 9809 MiB, as eden-nx2 sets it (k_system_control.cpp, MemoryArrangement_12GB;
// not measured on a console). MKW's nninitStartup sizes its heap as pool - used - 0x60400000 (main+0xc388c8).
#define DEFAULT_POOL (9809ull << 20)
#define SYSTEM_RESOURCE_BUDGET (32ull << 20)
#define MAX_STACK_MAPS 4096
#define REEXEC_LIMIT 20

// Horizon memory states, permissions, attributes and results used here.
enum { ST_FREE = 0, ST_CODE = 3, ST_CODEDATA = 4, ST_NORMAL = 5, ST_STACK = 0xB, ST_THREADLOCAL = 0xC,
       ST_INACCESSIBLE = 0x10 };
enum { PERM_NONE = 0, PERM_R = 1, PERM_RW = 3, PERM_RX = 5 };
enum { ATTR_PERMISSION_LOCKED = 16 };
#define R_OK 0u
#define R_INVALID_ADDRESS 0xcc01u   // misaligned address
#define R_INVALID_SIZE 0xca01u
#define R_INVALID_NEW_PERM 0xd801u
#define R_INVALID_STATE 0xd401u     // InvalidCurrentMemory
#define R_INVALID_REGION 0xdc01u
#define R_LIMIT_REACHED 0x10801u

static RtGuestLayout L;
static uint64_t mods_lo, mods_hi;   // fixed module window
static PlatMutex lock = PLAT_MUTEX_INIT;
static uint64_t heap_size, alias_bytes;
static uint8_t *alias_pages;        // 1 per mapped 4 KiB page of the alias region
typedef struct { uint64_t dst, src, size; } StackMap;
static StackMap stack_maps[MAX_STACK_MAPS];
static unsigned nstack_maps;
static uint8_t *module_perm[RT_MAX_MODULES];   // per data page of each module: perm | ATTR_PERMISSION_LOCKED

static uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

static int reserve_at(uint64_t at, uint64_t size, int watch) { return plat_mem_reserve(at, size, watch); }

// ---- frozen regions
// A 2 MiB region of the watched reservations that nothing wrote for a while can be made read-only (mem_freeze), so the
// back end's write-watch queries skip it: no page of a frozen region changes before a thaw. The first write faults and
// thaw_handler makes the region writable again before the write retries. A kernel write into guest memory (a file
// read) fails on a read-only page instead of faulting, so file reads thaw their buffer first (mem_thaw), and so does
// every commit, decommit and protection change here. A region stays thawed for FRZ_HOLD_MS after a mem_thaw: a file
// read still in flight writes its buffer later. frz_lock is a leaf lock: nothing writes guest memory under it.
#define FRZ_SHIFT 21
#define FRZ_SIZE (1ull << FRZ_SHIFT)
#define FRZ_HOLD_MS 5000u
static volatile uint8_t frz[RT_ADDRESS_LIMIT >> FRZ_SHIFT];
static uint32_t thaw_ms[RT_ADDRESS_LIMIT >> FRZ_SHIFT];   // frz_lock: GetTickCount of the last thaw request, 0 never
static volatile int32_t nfrz;
PlatMutex frz_lock = PLAT_MUTEX_INIT;   // memory_int.h
static volatile int64_t frz_count[4];   // freezes; thaws by a write fault, by mem_thaw, by a mapping change

int watched(uint64_t addr, uint64_t size) {   // memory_int.h
    const RtGuestLayout *g = &L;
    return (addr >= g->aslr.base && size <= LOW_SIZE && addr - g->aslr.base <= LOW_SIZE - size) ||
           (addr >= g->alias.base && size <= ALIAS_SIZE && addr - g->alias.base <= ALIAS_SIZE - size);
}

// frz_lock held exclusively. why: 1 a write fault, 2 mem_thaw (holds the region thawed), 3 a mapping change here.
static void thaw_locked(uint64_t r, int why) {
    if (why == 2) thaw_ms[r] = (uint32_t)plat_now_ms() | 1;
    if (!frz[r]) return;
    // The read-only runs only: freeze_locked left reserved holes (heap pages given back) as they were. Every committed
    // page of a frozen region was read-write at the freeze, and mapping changes thaw before they run, so these are all
    // ours.
    uint64_t a = r << FRZ_SHIFT, end = a + FRZ_SIZE, b;
    for (; a < end; a = b)
        if (plat_mem_state(a, end, &b) == PLAT_PROT_READ && !plat_mem_protect(a, b - a, PLAT_PROT_RW))
            rt_fatal("memory: thawing %#llx+%#llx failed", (unsigned long long)a, (unsigned long long)(b - a));
    __atomic_store_n(&frz[r], 0, __ATOMIC_RELEASE);
    plat_dec32(&nfrz);
    plat_inc64(&frz_count[why]);
}

static void untrap_range_locked(uint64_t a, uint64_t n);

void mem_thaw_regions_locked(uint64_t a, uint64_t n) {   // memory_int.h
    for (uint64_t r = a >> FRZ_SHIFT; n && r <= (a + n - 1) >> FRZ_SHIFT; r++) thaw_locked(r, 2);
}

// frz_lock held exclusively.
static void thaw_range_locked(uint64_t a, uint64_t n, int why) {
    if (!n || a >= RT_ADDRESS_LIMIT) return;
    untrap_range_locked(a, n);
    uint64_t r0 = a >> FRZ_SHIFT, r1 = (a + n - 1 < RT_ADDRESS_LIMIT ? a + n - 1 : RT_ADDRESS_LIMIT - 1) >> FRZ_SHIFT;
    for (uint64_t r = r0; r <= r1; r++) thaw_locked(r, why);
}

void mem_thaw(uint64_t a, uint64_t n) {
    mem_commit_range(a, n);   // a kernel write does not fault into lazy_fault
    plat_mutex_lock(&frz_lock);   // also when nothing is frozen: the hold keeps a read's buffer writable
    thaw_range_locked(a, n, 2);
    plat_mutex_unlock(&frz_lock);
}

// frz_lock held exclusively. Makes the committed pages of region r read-only when every one of them is read-write;
// reserved holes (heap pages given back) stay as they are, and thaw_locked finds the read-only runs again.
static int freeze_locked(uint64_t r) {
    uint64_t a = r << FRZ_SHIFT, end = a + FRZ_SIZE, run[16][2];
    int n = 0;
    for (uint64_t x = a, y; x < end; x = y) {
        int st = plat_mem_state(x, end, &y);
        if (st >= PLAT_PROT_NONE) {
            if (st != PLAT_PROT_RW || n == 16) return 0;
            run[n][0] = x, run[n][1] = y, n++;
        } else if (st != PLAT_MEM_RESERVED) return 0;
    }
    for (int i = 0; i < n; i++)
        if (!plat_mem_protect(run[i][0], run[i][1] - run[i][0], PLAT_PROT_READ)) {
            while (i--) plat_mem_protect(run[i][0], run[i][1] - run[i][0], PLAT_PROT_RW);
            return 0;
        }
    return n > 0;
}

int mem_freeze(uint64_t a) {
    if ((a & (FRZ_SIZE - 1)) || !watched(a, FRZ_SIZE)) return 0;
    uint64_t r = a >> FRZ_SHIFT;
    int ok = 0;
    plat_mutex_lock(&frz_lock);
    if (frz[r]) ok = 1;
    else if (thaw_ms[r] && (int32_t)((uint32_t)plat_now_ms() - thaw_ms[r]) < (int32_t)FRZ_HOLD_MS) ok = 0;   // |1 may be ahead
    else if (freeze_locked(r)) {
        __atomic_store_n(&frz[r], 1, __ATOMIC_RELEASE);
        plat_inc32(&nfrz);
        plat_inc64(&frz_count[0]);
        ok = 1;
    }
    plat_mutex_unlock(&frz_lock);
    return ok;
}

int mem_frozen(uint64_t a) { return a < RT_ADDRESS_LIMIT && __atomic_load_n(&frz[a >> FRZ_SHIFT], __ATOMIC_ACQUIRE); }

void mem_freeze_stats(uint64_t *frozen, uint64_t *freezes, uint64_t *fault_thaws, uint64_t *other_thaws) {
    *frozen = (uint64_t)__atomic_load_n(&nfrz, __ATOMIC_RELAXED);
    *freezes = (uint64_t)plat_xchg64(&frz_count[0], 0);
    *fault_thaws = (uint64_t)plat_xchg64(&frz_count[1], 0);
    *other_thaws = (uint64_t)plat_xchg64(&frz_count[2], 0) + (uint64_t)plat_xchg64(&frz_count[3], 0);
}

// ---- read traps
// Bytes the GPU wrote into a shadowed pool (vk_memory.c) reach guest memory on demand instead of at the fence: the
// pages are made inaccessible (mem_trap) with where their bytes are, and the first access to one, a read or a write,
// fills it (plat_mem_poke writes through the protection, so no other thread sees the page before it holds them) and
// makes it read-write again. The fence copied ~95 MB a frame back, most of it pages the game never reads (live35).
// Every path that thaws (file reads, commits, decommits, protection changes) fills the trapped pages it covers first,
// and frozen regions never hold one: freeze_locked refuses a region with an inaccessible page. Under frz_lock.
typedef struct { uint64_t lo, hi; const uint8_t *src; } TrapSrc;
uint8_t *trapped;                     // one byte per guest page, mapped lazily; frz_lock for writes; 1 a read trap,
                                      // 2 MKW_EARLY_READ's watch (memory_early.c)
static TrapSrc *tsrc;                 // frz_lock: where trapped pages take their bytes from
static int ntsrc, ctsrc;
static int64_t ntrapped;              // frz_lock: pages trapped now
static volatile int64_t trap_count[3];   // pages trapped, filled by an access, filled by a thaw or mem_untrap
static int trap_ok = -1;              // plat_mem_poke works; -1 not known yet

// frz_lock held: fills the trapped page of a from its source and gives it back to the guest; why 1: a's access.
static void fill_locked(uint64_t a, int why) {
    uint64_t p = a & ~(PAGE - 1);
    const uint8_t *src = NULL;
    for (int i = ntsrc; i-- && !src;)
        if (p >= tsrc[i].lo && p < tsrc[i].hi) src = tsrc[i].src + (p - tsrc[i].lo);
    if (!src || !plat_mem_poke(p, src, PAGE) || !plat_mem_protect(p, PAGE, PLAT_PROT_RW))
        rt_fatal("memory: filling the trapped page %#llx failed", (unsigned long long)p);
    trapped[p / PAGE] = 0;
    ntrapped--;
    plat_inc64(&trap_count[why]);
    mem_early_filled_locked(a, why == 1);   // MKW_EARLY_READ_DRY: a writer of the page running now
}

static void untrap_range_locked(uint64_t a, uint64_t n) {
    if (!ntrapped && !mem_early_pages && !mem_base_pages() && !mem_spill_pages()) return;
    uint64_t end = a + n < RT_ADDRESS_LIMIT ? a + n : RT_ADDRESS_LIMIT;
    for (uint64_t p = a & ~(PAGE - 1);
         p < end && (ntrapped || mem_early_pages || mem_base_pages() || mem_spill_pages()); p += PAGE)
        if (trapped[p / PAGE] == 2) mem_early_release_locked(p, 0);
        else if (trapped[p / PAGE] == 3) mem_base_fill_locked(p);
        else if (trapped[p / PAGE] == 4) mem_spill_fill_locked(p, 2);
        else if (trapped[p / PAGE]) fill_locked(p, 2);
}

int mem_trap_init(void) {   // memory_int.h
    if (trap_ok < 0) {
        uint8_t probe;
        trapped = plat_mem_alloc(RT_ADDRESS_LIMIT / PAGE);
        trap_ok = trapped && plat_mem_poke((uint64_t)(uintptr_t)&probe, &probe, 1);
        rt_log("mem: read traps %s", trap_ok ? "on: GPU writes reach guest memory when the game touches them"
                                             : "unavailable: GPU writes are copied back at the fence");
    }
    return trap_ok > 0;
}

int mem_trap(uint64_t a, uint64_t n, const void *src) {
    if (!n || !watched(a, n)) return 0;
    if (!mem_trap_init()) return 0;
    const uint8_t *s = src;
    uint64_t lo = (a + PAGE - 1) & ~(PAGE - 1), hi = (a + n) & ~(PAGE - 1);
    plat_mutex_lock(&frz_lock);
    // No region over the range stays frozen. Pages still trapped from an earlier fence stay so: their source is the same
    // shadow bytes (one live shadow per guest page), and filling them again at each fence copied it all back anyway
    // (rt-a1: 1 727 of 1 739 MiB trapped were filled that way).
    for (uint64_t r = a >> FRZ_SHIFT; r <= (a + n - 1) >> FRZ_SHIFT; r++) thaw_locked(r, 2);
    // A page MKW_EARLY_READ watches gets its own protection back first: the trap below then takes the GPU's bytes.
    // A base-trapped one gives its bytes to its base first: the pokes below write through its protection.
    // A spilled one gets its bytes back from the file first.
    if (mem_early_pages || mem_base_pages() || mem_spill_pages())
        for (uint64_t q = a & ~(PAGE - 1); q < a + n; q += PAGE) {
            if (trapped[q / PAGE] == 2) mem_early_release_locked(q, 0);
            else if (trapped[q / PAGE] == 3) mem_base_fill_locked(q);
            else if (trapped[q / PAGE] == 4) mem_spill_fill_locked(q, 2);
        }
    // The partial pages at the ends hold guest bytes besides the GPU's: copied now.
    int ok = 1;
    if (lo >= hi) ok = plat_mem_poke(a, s, n);
    else {
        if (a < lo) ok &= plat_mem_poke(a, s, lo - a);
        if (a + n > hi) ok &= plat_mem_poke(hi, s + (hi - a), a + n - hi);
    }
    // Then the read-write runs of whole pages; anything else (holes, guest protections) gets the bytes now.
    for (uint64_t x = lo, y; ok && x < hi; x = y) {
        int st = plat_mem_state(x, hi, &y);
        if (st == PLAT_PROT_RW) {
            memset(trapped + x / PAGE, 1, (y - x) / PAGE);   // before the protection: a fault in between finds it
            if (plat_mem_protect(x, y - x, PLAT_PROT_NONE)) {
                ntrapped += (int64_t)((y - x) / PAGE);
                plat_add64(&trap_count[0], (int64_t)((y - x) / PAGE));
            } else {
                memset(trapped + x / PAGE, 0, (y - x) / PAGE);
                ok = plat_mem_poke(x, s + (x - a), y - x);
            }
        } else if (st == PLAT_PROT_NONE) {   // trapped already, or a guest no-access page that gets the bytes now
            for (uint64_t q = x; ok && q < y; q += PAGE)
                if (!trapped[q / PAGE]) ok = plat_mem_poke(q, s + (q - a), PAGE);
        } else if (st >= PLAT_PROT_NONE) ok = plat_mem_poke(x, s + (x - a), y - x);
    }
    if (ok && lo < hi) {
        int i = 0;
        while (i < ntsrc && !(tsrc[i].lo == lo && tsrc[i].hi == hi && tsrc[i].src == s + (lo - a))) i++;
        if (i == ntsrc && ntsrc == ctsrc && ntsrc >= 64)   // sources no trapped page needs any more go first
            for (int k = 0; k < ntsrc;) {
                uint64_t p = tsrc[k].lo;
                while (p < tsrc[k].hi && !trapped[p / PAGE]) p += PAGE;
                if (p == tsrc[k].hi) tsrc[k] = tsrc[--ntsrc];
                else k++;
            }
        i = 0;
        while (i < ntsrc && !(tsrc[i].lo == lo && tsrc[i].hi == hi && tsrc[i].src == s + (lo - a))) i++;
        if (i == ntsrc) {
            if (ntsrc == ctsrc) {
                ctsrc = ctsrc ? ctsrc * 2 : 16;
                tsrc = realloc(tsrc, (size_t)ctsrc * sizeof *tsrc);
                if (!tsrc) rt_fatal("memory: out of memory");
            }
            tsrc[ntsrc++] = (TrapSrc){lo, hi, s + (lo - a)};
        }
    }
    plat_mutex_unlock(&frz_lock);
    if (!ok) rt_fatal("memory: copying %#llx+%#llx back failed", (unsigned long long)a, (unsigned long long)n);
    return 1;
}

void mem_untrap(uint64_t a, uint64_t n) {
    if (trap_ok <= 0 || !n) return;
    plat_mutex_lock(&frz_lock);
    untrap_range_locked(a, n);
    for (int i = 0; i < ntsrc;)   // sources inside the range go, and those no trapped page needs any more
        if (tsrc[i].lo >= a && tsrc[i].hi <= a + n) tsrc[i] = tsrc[--ntsrc];
        else i++;
    plat_mutex_unlock(&frz_lock);
}

// ---- pages the GPU shares (MKW_UDMABUF, gfx/vk/mem/vk_mem_zc.c)
// A file read in flight writes its buffer from the kernel, at any time until it returns: a range it covers is not
// swapped for another mapping meanwhile, its bytes would go to the pages that left. Under frz_lock.
#define IO_N 64
static struct { uint64_t lo, hi; } io[IO_N];
static int io_lost;   // reads in flight past IO_N: no range is swapped while there are any

void mem_io_begin(uint64_t a, uint64_t n) {
    mem_commit_range(a, n);
    plat_mutex_lock(&frz_lock);
    thaw_range_locked(a, n, 2);
    int i = 0;
    while (i < IO_N && io[i].hi) i++;
    if (i < IO_N) io[i].lo = a, io[i].hi = a + n;
    else io_lost++;
    plat_mutex_unlock(&frz_lock);
}

void mem_io_end(uint64_t a, uint64_t n) {
    plat_mutex_lock(&frz_lock);
    int i = 0;
    while (i < IO_N && !(io[i].lo == a && io[i].hi == a + n)) i++;
    if (i < IO_N) io[i].lo = io[i].hi = 0;
    else io_lost--;
    plat_mutex_unlock(&frz_lock);
}

int mem_share(uint64_t lo, uint64_t hi, const PlatShared *s, uint64_t off) {
    if (!watched(lo, hi - lo)) return 0;
    int ok = 0;
    plat_mutex_lock(&frz_lock);
    for (int i = 0; i < IO_N; i++)
        if (io_lost || (io[i].hi > lo && io[i].lo < hi)) goto out;
    thaw_range_locked(lo, hi - lo, 2);
    uint64_t end;
    if (plat_mem_state(lo, hi, &end) != PLAT_PROT_RW || end < hi) goto out;   // a hole or a guest protection
    // Read-only while the bytes are copied: a write waits in thaw_fault on frz_lock, finds the page read-write and retries.
    if (!plat_mem_protect(lo, hi - lo, PLAT_PROT_READ)) goto out;
    memcpy(s->map + off, (const void *)(uintptr_t)lo, hi - lo);
    ok = plat_mem_share(lo, hi - lo, s, off);
    if (ok < 0) rt_fatal("memory: sharing %#llx+%#llx with the GPU lost the range", (unsigned long long)lo,
                         (unsigned long long)(hi - lo));
    if (!ok && !plat_mem_protect(lo, hi - lo, PLAT_PROT_RW))
        rt_fatal("memory: %#llx+%#llx kept read-only", (unsigned long long)lo, (unsigned long long)(hi - lo));
out:
    plat_mutex_unlock(&frz_lock);
    return ok;
}

void mem_hold_layout(void) { plat_mutex_lock(&frz_lock); }
void mem_release_layout(void) { plat_mutex_unlock(&frz_lock); }

int mem_trapped(uint64_t a) { return trapped && a < RT_ADDRESS_LIMIT && __atomic_load_n(&trapped[a / PAGE], __ATOMIC_RELAXED) == 1; }

void mem_trap_stats(uint64_t *now, uint64_t *trapped_pages, uint64_t *access_fills, uint64_t *other_fills) {
    *now = (uint64_t)__atomic_load_n(&ntrapped, __ATOMIC_RELAXED);
    *trapped_pages = (uint64_t)plat_xchg64(&trap_count[0], 0);
    *access_fills = (uint64_t)plat_xchg64(&trap_count[1], 0);
    *other_fills = (uint64_t)plat_xchg64(&trap_count[2], 0);
}

// The first write into a frozen region thaws it and retries. A fault in a region another thread thawed meanwhile
// retries too; an access to a trapped page fills it and retries; any other fault goes on to the crash handler.
// The platform's fault hook (plat_mem_fault_hook): 1 retries the access.
static int thaw_fault(uint64_t a) {
    if (a >= RT_ADDRESS_LIMIT || !watched(a, 1)) return 0;
    if (trapped && __atomic_load_n(&trapped[a / PAGE], __ATOMIC_RELAXED)) {
        uint64_t wait = 0;   // MKW_EARLY_READ=2: the submit an early read waits for
        plat_mutex_lock(&frz_lock);
        if (trapped[a / PAGE] == 2) wait = mem_early_release_locked(a, 1);   // the byte touched
        else if (trapped[a / PAGE] == 3) mem_base_fill_locked(a);            // a write into a base's page
        else if (trapped[a / PAGE] == 4) mem_spill_fill_locked(a, 1);        // a spilled page: its bytes from the file
        else if (trapped[a / PAGE]) fill_locked(a, 1);
        plat_mutex_unlock(&frz_lock);
        if (wait) mem_early_wait(wait);
        return 1;
    }
    plat_mutex_lock(&frz_lock);
    int was = frz[a >> FRZ_SHIFT];
    thaw_locked(a >> FRZ_SHIFT, 1);
    plat_mutex_unlock(&frz_lock);
    if (was) return 1;
    uint64_t end;
    int st = plat_mem_state(a & ~(PAGE - 1), (a & ~(PAGE - 1)) + PAGE, &end);
    if (st != PLAT_PROT_RW && trap_ok > 0)   // the crash report follows: say whether a trap was involved
        rt_log("memory: fault at %#llx: page state %d, not trapped (%lld trapped now)", (unsigned long long)a, st,
               (long long)ntrapped);
    return st == PLAT_PROT_RW;
}

// Mapping changes thaw the regions they touch and run under frz_lock, so no freeze sees them half done.
static void commit(uint64_t a, uint64_t n, const char *what) {
    plat_mutex_lock(&frz_lock);
    thaw_range_locked(a, n, 3);
    int ok = !n || plat_mem_commit(a, n);
    plat_mutex_unlock(&frz_lock);
    if (!ok)   // Windows: RAM plus the page file used up (#467); the wiki's Troubleshooting says what to turn on
        rt_fatal("memory: commit %s %#llx+%#llx failed (the system is out of memory: turn on the Windows page file or close "
                 "other programs)", what, (unsigned long long)a, (unsigned long long)n);
}

// Windows (#198): the game sets an 8 GiB heap at boot and maps physical memory it touches later, and Windows charges
// every committed page against RAM plus the page file whether it is touched or not (Linux only counts touched pages):
// the process ran at about three times its resident memory in private bytes (#198: 13.4 GB private, 3.4 resident) and
// a 16 GB machine refused the next commit. There the heap and the mapped physical pages stay reserved and are
// committed 2 MiB at a time on their first access (lazy_fault); anything that reaches guest memory outside our CPU
// threads (a kernel write into a file read's buffer, a driver import) commits its range first (mem_thaw,
// mem_commit_range). MKW_LAZY_COMMIT=0 commits as before.
#define LAZY_CHUNK (2ull << 20)
static int lazy;

// lock held: commits the still-reserved pages of [a, a+n) that the game owns (its heap below heap_size, the mapped
// pages of the alias region).
static void owned_commit_locked(uint64_t a, uint64_t n) {
    uint64_t e = a + n, lo, hi, end;
    lo = a > L.heap.base ? a : L.heap.base, hi = e < L.heap.base + heap_size ? e : L.heap.base + heap_size;
    for (uint64_t x = lo; x < hi; x = end)
        if (plat_mem_state(x, hi, &end) == PLAT_MEM_RESERVED) commit(x, end - x, "heap");
    lo = a > L.alias.base ? a : L.alias.base, hi = e < L.alias.base + ALIAS_SIZE ? e : L.alias.base + ALIAS_SIZE;
    for (uint64_t p = lo & ~(PAGE - 1); p < hi;) {
        if (!alias_pages[(p - L.alias.base) / PAGE]) { p += PAGE; continue; }
        uint64_t q = p;
        while (q < hi && alias_pages[(q - L.alias.base) / PAGE]) q += PAGE;
        for (uint64_t x = p; x < q; x = end)
            if (plat_mem_state(x, q, &end) == PLAT_MEM_RESERVED) commit(x, end - x, "physical memory");
        p = q;
    }
}

// The platform's lazy hook (plat_mem_lazy_hook), for reads and writes: 1 when the page at a was owned and is now
// committed, so the access is retried.
static int lazy_fault(uint64_t a) {
    if (!lazy || !watched(a, 1)) return 0;
    uint64_t end, p = a & ~(PAGE - 1);
    // only a page still reserved: a committed one faulted for another reason (a frozen region's write, a guard)
    if (plat_mem_state(p, p + PAGE, &end) != PLAT_MEM_RESERVED) return 0;
    plat_mutex_lock(&lock);
    owned_commit_locked(a & ~(LAZY_CHUNK - 1), LAZY_CHUNK);
    int st = plat_mem_state(p, p + PAGE, &end);
    plat_mutex_unlock(&lock);
    return st >= PLAT_PROT_NONE;   // not the game's page: still reserved, the fault goes on to the crash handler
}

void mem_commit_range(uint64_t a, uint64_t n) {
    if (!lazy || !n) return;
    plat_mutex_lock(&lock);
    owned_commit_locked(a, n);
    plat_mutex_unlock(&lock);
}

// ---- the runtime's own regions (mem_runtime_alloc)
// Memory the runtime gives the game beyond what the game sized itself (the render-buffer pool for larger targets,
// patches.c): at the top of the heap block, above the application pool the game's heap can reach (SetHeapSize refuses
// more), inside the same watched reservation, so the back end's write-watch works there as on the heap. Committed
// now, also on Windows: the lazy commit only serves the game's own pages (owned_commit_locked). QueryMemory reports
// it free: the game never asks there.
static uint64_t runtime_top;

uint64_t mem_runtime_alloc(uint64_t size, const char *what) {
    size = align_up(size, LAZY_CHUNK);
    plat_mutex_lock(&lock);
    if (!runtime_top) runtime_top = L.heap.base + HEAP_CAP;
    uint64_t a = runtime_top - size;
    if (size > runtime_top || a < L.heap.base + L.pool) {
        plat_mutex_unlock(&lock);
        rt_log("mem: no room above the game's heap for %s (%#llx bytes)", what, (unsigned long long)size);
        return 0;
    }
    commit(a, size, what);
    char name[64];
    snprintf(name, sizeof name, "mkw rt %s", what);
    plat_mem_name((void *)(uintptr_t)a, size, name);
    runtime_top = a;
    plat_mutex_unlock(&lock);
    rt_log("mem: %s: %#llx bytes of the runtime's own at %#llx, above the game's heap", what,
           (unsigned long long)size, (unsigned long long)a);
    return a;
}

static void decommit(uint64_t a, uint64_t n, const char *what) {
    plat_mutex_lock(&frz_lock);
    thaw_range_locked(a, n, 3);
    int ok = !n || plat_mem_decommit(a, n);
    plat_mutex_unlock(&frz_lock);
    if (!ok) rt_fatal("memory: decommit %s %#llx+%#llx failed", what, (unsigned long long)a, (unsigned long long)n);
}

static void protect(uint64_t a, uint64_t n, int prot, const char *what) {
    plat_mutex_lock(&frz_lock);
    thaw_range_locked(a, n, 3);
    int ok = !n || plat_mem_protect(a, n, prot);
    plat_mutex_unlock(&frz_lock);
    if (!ok) rt_fatal("memory: protect %s %#llx+%#llx failed", what, (unsigned long long)a, (unsigned long long)n);
}

// The process is re-run when a region is taken: Windows places DLLs, heaps and thread stacks at random addresses,
// and a fresh process gets a fresh layout.
static void reexec_or_fail(const char *what) {
    const char *e = cfg_get("MKW_REEXEC");
    int n = e ? atoi(e) : 0;
    if (n >= REEXEC_LIMIT) rt_fatal("memory: %s is not free after %d attempts", what, n);
    char buf[16];
    snprintf(buf, sizeof buf, "%d", n + 1);
    plat_setenv("MKW_REEXEC", buf);
    plat_reexec();
}

void mem_init(void) {
    if (L.pool) return;
    // Fixed window: modules, kernel region, host-function addresses.
    const RtGame *g = rt_game();
    mods_lo = g->modules[0].base;
    mods_hi = align_up(g->modules[g->nmodules - 1].base + g->modules[g->nmodules - 1].size, 0x10000);
    if (mods_hi > RT_KERNEL_BASE) rt_fatal("memory: modules end at %#llx, inside the kernel region",
                                           (unsigned long long)mods_hi);
    if (!reserve_at(mods_lo, mods_hi - mods_lo, 0)) reexec_or_fail("the module window");
    if (!reserve_at(RT_KERNEL_BASE, RT_KERNEL_SIZE, 0)) reexec_or_fail("the kernel region");
    if (!reserve_at(RT_HOSTFN_BASE, RT_HOSTFN_SIZE, 0)) reexec_or_fail("the host-function window");

    // Stack and heap block, then the alias region above it, both searched below the module window.
    uint64_t low = 0, alias = 0;
    for (uint64_t at = LOW_SEARCH_FROM; at + LOW_SIZE <= mods_lo && !low; at += GiB)
        if (reserve_at(at, LOW_SIZE, 1)) low = at;
    if (!low) reexec_or_fail("a 6 GiB stack and heap block below 2^39");
    for (uint64_t at = align_up(low + LOW_SIZE, ALIAS_ALIGN); at + ALIAS_SIZE <= mods_lo && !alias; at += ALIAS_ALIGN)
        if (reserve_at(at, ALIAS_SIZE, 1)) alias = at;
    if (!alias) reexec_or_fail("a 64 GiB alias region below 2^39");

    plat_mem_fault_hook(thaw_fault);   // before the crash handler
    alias_pages = calloc(ALIAS_PAGES, 1);
    if (!alias_pages) rt_fatal("memory: cannot allocate the alias page map");
    // mkw.ini is read here, once the address space is claimed and before the first setting: the pool size below
    // depends on MKW_APP_POOL.
    cfg_init();
    const char *pool = cfg_get("MKW_APP_POOL");
    L = (RtGuestLayout){
        .aslr = {low, RT_ADDRESS_LIMIT - low},
        .stack = {low + STACK_OFF, STACK_SIZE},
        .heap = {low + HEAP_OFF, HEAP_CAP},
        .alias = {alias, ALIAS_SIZE},
        .kernel = {RT_KERNEL_BASE, RT_KERNEL_SIZE},
        .hostfn = {RT_HOSTFN_BASE, RT_HOSTFN_SIZE},
        .pool = pool ? strtoull(pool, NULL, 0) : DEFAULT_POOL,
    };
    if (!L.pool) rt_fatal("memory: MKW_APP_POOL is 0");
    // Labels for the memory census (plat_mem_name): the block first, its stack and heap over it.
    static const char *const names[] = {"modules", "kernel", "hostfn", "other", "stack", "heap", "alias"};
    const RtRange where[] = {{mods_lo, mods_hi - mods_lo}, L.kernel, L.hostfn, {low, LOW_SIZE}, L.stack, L.heap, L.alias};
    for (int i = 0; i < 7; i++) {
        char name[32];
        snprintf(name, sizeof name, "mkw guest %s", names[i]);
        plat_mem_name((void *)(uintptr_t)where[i].base, where[i].size, name);
    }
#ifdef _WIN32
    const char *lz = cfg_get("MKW_LAZY_COMMIT");
    lazy = !lz || atoi(lz) != 0;
#endif
    plat_mem_lazy_hook(lazy_fault);   // a no-op on Linux
}

const RtGuestLayout *mem_layout(void) { return &L; }

// A range inside one watched reservation (stack and heap block, or alias region) only.
static uint64_t written_pages(uint64_t addr, uint64_t size, uint64_t *pages, uint64_t max, int reset) {
    if (!max || !size) return 0;
    if (!watched(addr, size)) return UINT64_MAX;
    return plat_mem_written(addr, size, pages, max, reset);
}

uint64_t mem_written_pages(uint64_t addr, uint64_t size, uint64_t *pages, uint64_t max) {
    return written_pages(addr, size, pages, max, 1);
}

uint64_t mem_written_peek(uint64_t addr, uint64_t size, uint64_t *pages, uint64_t max) {
    return written_pages(addr, size, pages, max, 0);
}

static uint64_t image_bytes(void) {
    uint64_t n = 0;
    for (int i = 0; i < ldr_count(); i++) n += ldr_module(i)->size;
    return align_up(n, PAGE);
}

static int overlaps(uint64_t a, uint64_t n, uint64_t b, uint64_t m) { return a < b + m && b < a + n; }
static int inside(uint64_t a, uint64_t n, RtRange r) { return a >= r.base && n <= r.size && a - r.base <= r.size - n; }

// ---- 0x01 SetHeapSize
uint32_t mem_set_heap_size(uint64_t size, uint64_t *addr) {
    plat_mutex_lock(&lock);
    if (size > HEAP_CAP || size % 0x200000 || size + image_bytes() > L.pool) {
        plat_mutex_unlock(&lock);
        return R_INVALID_STATE;
    }
    if (size > heap_size && !lazy) commit(L.heap.base + heap_size, size - heap_size, "heap");
    if (size < heap_size) decommit(L.heap.base + size, heap_size - size, "heap");
    if (size != heap_size) rt_log("mem: heap %#llx bytes at %#llx", (unsigned long long)size, (unsigned long long)L.heap.base);
    heap_size = size;
    plat_mutex_unlock(&lock);
    if (addr) *addr = L.heap.base;
    return R_OK;
}

// ---- module data permissions (0x02, 0x03) and module pages for QueryMemory
static int module_index(uint64_t a) {
    for (int i = 0; i < ldr_count(); i++) {
        const RtLoaded *m = ldr_module(i);
        if (a - m->base < m->size) return i;
    }
    return -1;
}

static uint8_t *perm_table(int i) {   // under lock
    if (!module_perm[i]) {
        const RtLoaded *m = ldr_module(i);
        size_t n = (m->size - m->data_off) / PAGE;
        module_perm[i] = malloc(n);
        if (!module_perm[i]) rt_fatal("memory: module permission table");
        memset(module_perm[i], PERM_RW, n);
    }
    return module_perm[i];
}

static unsigned module_page_perm(int i, uint64_t off) {   // under lock
    const RtLoaded *m = ldr_module(i);
    if (off < m->data_off) return off < align_up(m->text_end, PAGE) ? PERM_RX : PERM_R;
    return module_perm[i] ? module_perm[i][(off - m->data_off) / PAGE] : PERM_RW;
}

// The module whose data segment holds [a, a+n), or -1.
static int module_data(uint64_t a, uint64_t n) {
    int i = module_index(a);
    if (i < 0) return -1;
    const RtLoaded *m = ldr_module(i);
    uint64_t off = a - m->base;
    return off >= m->data_off && n <= m->size - off ? i : -1;
}

uint32_t mem_set_permission(uint64_t a, uint64_t n, uint32_t perm) {
    if (a & (PAGE - 1)) return R_INVALID_ADDRESS;
    if (!n || (n & (PAGE - 1))) return R_INVALID_SIZE;
    if (perm != PERM_NONE && perm != PERM_R && perm != PERM_RW) return R_INVALID_NEW_PERM;
    int i = module_data(a, n);
    if (i < 0) return R_INVALID_STATE;
    const RtLoaded *m = ldr_module(i);
    plat_mutex_lock(&lock);
    uint8_t *t = perm_table(i) + (a - m->base - m->data_off) / PAGE;
    for (uint64_t p = 0; p < n / PAGE; p++)
        if (t[p] & ATTR_PERMISSION_LOCKED) {
            plat_mutex_unlock(&lock);
            return R_INVALID_STATE;
        }
    memset(t, (int)perm, n / PAGE);
    protect(a, n, perm == PERM_RW ? PLAT_PROT_RW : perm == PERM_R ? PLAT_PROT_READ : PLAT_PROT_NONE, "module data");
    plat_mutex_unlock(&lock);
    rt_log("%s data permission +%#llx size %#llx perm %u", m->name, (unsigned long long)(a - m->base),
           (unsigned long long)n, perm);
    return R_OK;
}

// The only module attribute the SDK sets is the irreversible RELRO lock.
uint32_t mem_set_attribute(uint64_t a, uint64_t n, uint32_t mask, uint32_t at) {
    if (a & (PAGE - 1)) return R_INVALID_ADDRESS;
    if (!n || (n & (PAGE - 1))) return R_INVALID_SIZE;
    if (mask != ATTR_PERMISSION_LOCKED || at != ATTR_PERMISSION_LOCKED) return R_INVALID_NEW_PERM;
    int i = module_data(a, n);
    if (i < 0) return R_INVALID_STATE;
    const RtLoaded *m = ldr_module(i);
    plat_mutex_lock(&lock);
    uint8_t *t = perm_table(i) + (a - m->base - m->data_off) / PAGE;
    for (uint64_t p = 0; p < n / PAGE; p++) t[p] |= ATTR_PERMISSION_LOCKED;
    plat_mutex_unlock(&lock);
    return R_OK;
}

// ---- 0x04/0x05 MapMemory, UnmapMemory: thread stacks move from heap or alias into the stack region by copy; the
// source is inaccessible while moved and gets the contents back on unmap.
static int alias_all_mapped(uint64_t a, uint64_t n) {   // under lock
    uint64_t first = (a - L.alias.base) / PAGE;
    for (uint64_t i = 0; i < n / PAGE; i++)
        if (!alias_pages[first + i]) return 0;
    return 1;
}

static uint32_t stack_move(int unmap, uint64_t dst, uint64_t src, uint64_t n) {
    if (!n || ((dst | src | n) & (PAGE - 1))) return R_INVALID_ADDRESS;
    if (!inside(dst, n, L.stack)) return R_INVALID_REGION;
    int in_heap = inside(src, n, (RtRange){L.heap.base, heap_size});
    int in_alias = inside(src, n, L.alias);
    if (!in_heap && !in_alias) return R_INVALID_REGION;
    plat_mutex_lock(&lock);
    uint32_t r = R_OK;
    if (in_alias && !alias_all_mapped(src, n)) {
        r = R_INVALID_REGION;
        goto out;
    }
    // the copies and protections below run under lock, where a lazy fault would wait for it: commit the source first
    if (lazy) owned_commit_locked(src, n);
    unsigned found = nstack_maps;
    for (unsigned i = 0; i < nstack_maps; i++)
        if (stack_maps[i].dst == dst && stack_maps[i].src == src && stack_maps[i].size == n) found = i;
    if (unmap) {
        if (found == nstack_maps) {
            r = R_INVALID_STATE;
            goto out;
        }
        protect(src, n, PLAT_PROT_RW, "moved stack source");
        memcpy((void *)(uintptr_t)src, (const void *)(uintptr_t)dst, n);
        decommit(dst, n, "moved stack");
        stack_maps[found] = stack_maps[--nstack_maps];
    } else {
        if (nstack_maps == MAX_STACK_MAPS) {
            r = R_INVALID_STATE;
            goto out;
        }
        for (unsigned i = 0; i < nstack_maps; i++)
            if (overlaps(dst, n, stack_maps[i].dst, stack_maps[i].size) ||
                overlaps(src, n, stack_maps[i].src, stack_maps[i].size)) {
                r = R_INVALID_REGION;
                goto out;
            }
        commit(dst, n, "moved stack");
        memcpy((void *)(uintptr_t)dst, (const void *)(uintptr_t)src, n);
        protect(src, n, PLAT_PROT_NONE, "moved stack source");
        stack_maps[nstack_maps++] = (StackMap){dst, src, n};
    }
out:
    plat_mutex_unlock(&lock);
    return r;
}

uint32_t mem_map(uint64_t dst, uint64_t src, uint64_t n) { return stack_move(0, dst, src, n); }
uint32_t mem_unmap(uint64_t dst, uint64_t src, uint64_t n) { return stack_move(1, dst, src, n); }

// ---- 0x2C/0x2D MapPhysicalMemory, UnmapPhysicalMemory. Horizon maps only the still-unmapped pages of the range and
// leaves mapped pages and their contents alone; TotK's heaps grow by re-mapping ranges they partly own.
static void alias_runs(uint64_t first, uint64_t count, uint8_t want, int do_commit) {   // under lock
    uint64_t i = 0;
    while (i < count) {
        if (alias_pages[first + i] != want) { i++; continue; }
        uint64_t j = i;
        while (j < count && alias_pages[first + j] == want) j++;
        uint64_t a = L.alias.base + (first + i) * PAGE, n = (j - i) * PAGE;
        if (do_commit) {
            if (!lazy) commit(a, n, "physical memory");
        }
        else decommit(a, n, "physical memory");
        memset(alias_pages + first + i, !want, j - i);
        i = j;
    }
}

static uint32_t alias_map(int unmap, uint64_t a, uint64_t n) {
    if (!n || ((a | n) & (PAGE - 1))) return R_INVALID_ADDRESS;
    if (!inside(a, n, L.alias)) return R_INVALID_REGION;
    uint64_t first = (a - L.alias.base) / PAGE, count = n / PAGE;
    plat_mutex_lock(&lock);
    uint32_t r = R_OK;
    if (unmap) {
        for (unsigned i = 0; i < nstack_maps; i++)
            if (overlaps(a, n, stack_maps[i].src, stack_maps[i].size)) {
                r = R_INVALID_REGION;
                goto out;
            }
        uint64_t released = 0;
        for (uint64_t i = 0; i < count; i++) released += alias_pages[first + i];
        alias_runs(first, count, 1, 0);
        alias_bytes -= released * PAGE;
    } else {
        uint64_t fresh = 0;
        for (uint64_t i = 0; i < count; i++) fresh += !alias_pages[first + i];
        if (fresh * PAGE + alias_bytes + heap_size + image_bytes() > L.pool) {
            r = R_LIMIT_REACHED;   // the SDK maps LimitReached to its recoverable out-of-memory result
            goto out;
        }
        alias_runs(first, count, 0, 1);
        alias_bytes += fresh * PAGE;
    }
out:
    plat_mutex_unlock(&lock);
    if (r) rt_log("%sMapPhysicalMemory %#llx+%#llx failed %#x (alias %#llx used, heap %#llx)", unmap ? "Un" : "",
                  (unsigned long long)a, (unsigned long long)n, r, (unsigned long long)alias_bytes,
                  (unsigned long long)heap_size);
    return r;
}

uint32_t mem_map_physical(uint64_t a, uint64_t n) { return alias_map(0, a, n); }
uint32_t mem_unmap_physical(uint64_t a, uint64_t n) { return alias_map(1, a, n); }

// ---- 0x06 QueryMemory
// The region around a that no known region covers: everything else below 2^39 belongs to the host and is reported
// as taken (Code, RX), so the SDK never picks it.
static void taken_gap(uint64_t a, uint64_t *base, uint64_t *size) {
    uint64_t lo = 0, hi = RT_ADDRESS_LIMIT;
    RtRange known[3 + RT_MAX_MODULES + 2];
    int n = 0;
    known[n++] = (RtRange){L.aslr.base, LOW_SIZE};
    known[n++] = L.alias;
    known[n++] = L.kernel;
    known[n++] = L.hostfn;
    for (int i = 0; i < ldr_count(); i++) known[n++] = (RtRange){ldr_module(i)->base, ldr_module(i)->size};
    for (int i = 0; i < n; i++) {
        uint64_t e = known[i].base + known[i].size;
        if (e <= a && e > lo) lo = e;
        if (known[i].base > a && known[i].base < hi) hi = known[i].base;
    }
    *base = lo;
    *size = hi - lo;
}

static uint64_t run_end(const uint8_t *p, uint64_t i, uint64_t n, uint8_t v) {
    while (i < n && (i & 7)) { if (p[i] != v) return i; i++; }
    uint64_t w = v ? 0x0101010101010101ull : 0;
    while (i + 8 <= n && *(const uint64_t *)(p + i) == w) i += 8;
    while (i < n && p[i] == v) i++;
    return i;
}

static uint64_t run_start(const uint8_t *p, uint64_t i, uint8_t v) {   // first index of the run that ends at i
    while (i > 0 && (i & 7)) { if (p[i - 1] != v) return i; i--; }
    uint64_t w = v ? 0x0101010101010101ull : 0;
    while (i >= 8 && *(const uint64_t *)(p + i - 8) == w) i -= 8;
    while (i > 0 && p[i - 1] == v) i--;
    return i;
}

uint32_t mem_query(uint64_t a, RtMemoryInfo *out) {
    RtMemoryInfo r = {.base = a & ~(PAGE - 1), .size = PAGE};
    uint64_t low = L.aslr.base;
    plat_mutex_lock_shared(&lock);
    int mi;
    if (a >= RT_ADDRESS_LIMIT) {
        r.base = RT_ADDRESS_LIMIT;
        r.size = 0 - RT_ADDRESS_LIMIT;
        r.state = ST_INACCESSIBLE;
    } else if (a < low) {
        r.base = 0;
        r.size = low;
        r.state = ST_FREE;
    } else if (a < low + LOW_SIZE) {
        uint64_t heap = L.heap.base;
        if (a >= heap && a < heap + heap_size) {
            r = (RtMemoryInfo){.base = heap, .size = heap_size, .state = ST_NORMAL, .perm = PERM_RW};
        } else if (a < heap) {
            r = (RtMemoryInfo){.base = low, .size = heap - low, .state = ST_FREE};
        } else {
            r = (RtMemoryInfo){.base = heap + heap_size, .size = low + LOW_SIZE - heap - heap_size, .state = ST_FREE};
        }
    } else if (a - L.alias.base < L.alias.size) {
        uint64_t p = (a - L.alias.base) / PAGE;
        uint8_t v = alias_pages[p];
        uint64_t s = run_start(alias_pages, p, v), e = run_end(alias_pages, p, ALIAS_PAGES, v);
        r = (RtMemoryInfo){.base = L.alias.base + s * PAGE, .size = (e - s) * PAGE, .state = v ? ST_NORMAL : ST_FREE,
                           .perm = v ? PERM_RW : PERM_NONE};
    } else if ((mi = module_index(a)) >= 0) {
        const RtLoaded *m = ldr_module(mi);
        uint64_t off = (a - m->base) & ~(PAGE - 1), lo = off, hi = off + PAGE;
        unsigned perm = module_page_perm(mi, off);
        int data = off >= m->data_off;
        uint64_t limit = data ? m->data_off : 0, end = data ? m->size : m->data_off;
        while (lo > limit && module_page_perm(mi, lo - PAGE) == perm) lo -= PAGE;
        while (hi < end && module_page_perm(mi, hi) == perm) hi += PAGE;
        r = (RtMemoryInfo){.base = m->base + lo, .size = hi - lo, .state = data ? ST_CODEDATA : ST_CODE,
                           .perm = perm & 7, .attr = perm & ATTR_PERMISSION_LOCKED};
    } else if (a - L.kernel.base < L.kernel.size) {
        r = (RtMemoryInfo){.base = L.kernel.base, .size = L.kernel.size, .state = ST_THREADLOCAL, .perm = PERM_RW};
    } else if (a - L.hostfn.base < L.hostfn.size) {
        r = (RtMemoryInfo){.base = L.hostfn.base, .size = L.hostfn.size, .state = ST_INACCESSIBLE};
    } else {
        taken_gap(a, &r.base, &r.size);
        r.state = ST_CODE;
        r.perm = PERM_RX;
    }
    // Moved stacks: the destination is Stack, the source Normal without access; free and normal regions around them
    // are clipped (MNG services.c).
    for (unsigned i = 0; i < nstack_maps; i++) {
        const StackMap *m = &stack_maps[i];
        uint64_t end = m->dst + m->size, send = m->src + m->size;
        if (a >= m->dst && a < end) {
            r = (RtMemoryInfo){.base = m->dst, .size = m->size, .state = ST_STACK, .perm = PERM_RW};
            break;
        }
        if (a >= m->src && a < send) {
            r = (RtMemoryInfo){.base = m->src, .size = m->size, .state = ST_NORMAL, .perm = PERM_NONE, .attr = 4};
            break;
        }
        uint64_t lo = r.base, hi = r.base + r.size;
        uint64_t b = r.state == ST_FREE ? m->dst : r.state == ST_NORMAL ? m->src : 0;
        uint64_t e = b + m->size;
        if (!b) continue;
        if (e <= a && e > lo) lo = e;
        if (b > a && b < hi) hi = b;
        r.base = lo;
        r.size = hi - lo;
    }
    plat_mutex_unlock_shared(&lock);
    if (out) *out = r;
    return R_OK;
}

// ---- GetInfo memory ids
int mem_info(uint32_t id, uint64_t *out) {
    uint64_t v;
    switch (id) {
    case 2: v = L.alias.base; break;
    case 3: v = L.alias.size; break;
    case 4: v = L.heap.base; break;
    case 5: v = L.heap.size; break;
    case 6: case 21: v = L.pool; break;
    case 7: case 22:
        plat_mutex_lock_shared(&lock);
        v = image_bytes() + heap_size + alias_bytes;
        plat_mutex_unlock_shared(&lock);
        break;
    case 12: v = L.aslr.base; break;
    case 13: v = L.aslr.size; break;
    case 14: v = L.stack.base; break;
    case 15: v = L.stack.size; break;
    case 16: v = SYSTEM_RESOURCE_BUDGET; break;
    case 17: v = sizeof stack_maps + ALIAS_PAGES; break;   // system resource in use: our bookkeeping
    case 28: v = 0; break;   // AliasRegionExtraSize: no kernel-owned extension
    // Switch 2 ids (eden-nx2 NX2_NOTES.md rows 7, 8, 16): 29/30 a region between the alias region and ASLR, left
    // empty right after the alias region; 31/32 remote memory for the graphics server, none here; 33 the page
    // granule nn::os's VAMM manager checks module ranges against.
    case 29: v = L.alias.base + L.alias.size; break;
    case 30: v = 0; break;
    case 31: case 32: v = 0; break;
    case 33: v = 0x1000; break;
    default: return 0;
    }
    *out = v;
    return 1;
}

// ---- kernel region: power-of-two blocks, a free list per size, bump allocation with lazy commit.
#define KCLASSES 29          // 64 B .. 256 MiB
#define KMIN_SHIFT 6
#define KCOMMIT 0x10000ull
typedef struct KFree { struct KFree *next; } KFree;
static KFree *kfree_list[KCLASSES];
static uint64_t kbump, kcommitted;
static PlatMutex klock = PLAT_MUTEX_INIT;

static int kclass(uint64_t size) {
    int s = KMIN_SHIFT;
    while ((1ull << s) < size) s++;
    return s - KMIN_SHIFT;
}

void *mem_kernel_alloc(uint64_t size, uint64_t align) {
    if (!L.pool) rt_fatal("mem_kernel_alloc before mem_init");
    if (align > KCOMMIT || (align & (align - 1))) rt_fatal("mem_kernel_alloc: alignment %#llx", (unsigned long long)align);
    uint64_t want = size > align ? size : align;
    int k = kclass(want ? want : 1);
    if (k >= KCLASSES) rt_fatal("mem_kernel_alloc: %#llx bytes", (unsigned long long)size);
    uint64_t bytes = 1ull << (k + KMIN_SHIFT), al = bytes < KCOMMIT ? bytes : KCOMMIT;
    plat_mutex_lock(&klock);
    KFree *f = kfree_list[k];
    void *p;
    if (f) {
        kfree_list[k] = f->next;
        p = f;
    } else {
        uint64_t a = align_up(kbump ? kbump : RT_KERNEL_BASE, al);
        if (a + bytes > RT_KERNEL_BASE + RT_KERNEL_SIZE) rt_fatal("kernel region exhausted (%#llx bytes)",
                                                                  (unsigned long long)bytes);
        uint64_t need = align_up(a + bytes, KCOMMIT);
        if (!kcommitted) kcommitted = RT_KERNEL_BASE;
        if (need > kcommitted) {
            commit(kcommitted, need - kcommitted, "kernel region");
            kcommitted = need;
        }
        kbump = a + bytes;
        p = (void *)(uintptr_t)a;
    }
    plat_mutex_unlock(&klock);
    memset(p, 0, size ? size : 1);
    return p;
}

void mem_kernel_free(void *p, uint64_t size) {
    if (!p) return;
    uint64_t a = (uintptr_t)p;
    if (a - RT_KERNEL_BASE >= RT_KERNEL_SIZE) rt_fatal("mem_kernel_free(%p): not in the kernel region", p);
    int k = kclass(size ? size : 1);
    // A block allocated with a larger alignment sits in a larger class; freeing it into the smaller class only
    // wastes the rest.
    plat_mutex_lock(&klock);
    KFree *f = p;
    f->next = kfree_list[k];
    kfree_list[k] = f;
    plat_mutex_unlock(&klock);
}
