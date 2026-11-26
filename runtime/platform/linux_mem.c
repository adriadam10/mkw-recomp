// Linux implementation of platform.h, virtual memory, the write watch and the fault handler (the parts: linux_int.h).
#ifndef _GNU_SOURCE   // build.py passes it on Linux
#define _GNU_SOURCE
#endif
#if defined(__aarch64__)
#include <asm/sigcontext.h>
#endif
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/fs.h>
#include <linux/futex.h>
#include <linux/userfaultfd.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <spawn.h>
#include <execinfo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/random.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "platform.h"
#include "linux_int.h"


// The write-watch ABI of Linux 6.7 (uapi linux/fs.h and linux/userfaultfd.h), for headers older than it: the release
// builds against the Steam Runtime's 5.10 headers and runs on newer kernels. The kernel refuses what it lacks at run time.
#ifndef PAGEMAP_SCAN
struct page_region {
    __u64 start;
    __u64 end;
    __u64 categories;
};
struct pm_scan_arg {
    __u64 size;
    __u64 flags;
    __u64 start;
    __u64 end;
    __u64 walk_end;
    __u64 vec;
    __u64 vec_len;
    __u64 max_pages;
    __u64 category_inverted;
    __u64 category_mask;
    __u64 category_anyof_mask;
    __u64 return_mask;
};
#define PAGEMAP_SCAN _IOWR('f', 16, struct pm_scan_arg)
#define PAGE_IS_WRITTEN (1 << 1)
#define PAGE_IS_PRESENT (1 << 3)
#define PAGE_IS_SWAPPED (1 << 4)
#define PAGE_IS_PFNZERO (1 << 5)
#define PM_SCAN_WP_MATCHING (1 << 0)
#define PM_SCAN_CHECK_WPASYNC (1 << 1)
#endif
#ifndef UFFD_FEATURE_WP_ASYNC
#define UFFD_FEATURE_WP_UNPOPULATED (1 << 13)
#define UFFD_FEATURE_WP_ASYNC (1 << 15)
#endif
#ifndef UFFD_USER_MODE_ONLY
#define UFFD_USER_MODE_ONLY 1
#endif

// ---- virtual memory. Reservations are PROT_NONE MAP_NORESERVE mappings; commit is mprotect read-write, decommit
// MADV_DONTNEED (private anonymous pages read zero afterwards) and PROT_NONE. Linux keeps no committed/reserved
// distinction, so a byte per guest page records it (pstate, mapped lazily: only the pages touched cost memory).
//
// Write watch: a userfaultfd in asynchronous write-protect mode (Linux 6.7) over the watched reservations. The kernel
// clears a page's protection on the first write without a round trip to us, and PAGEMAP_SCAN lists the pages whose
// protection was cleared and protects them again. Without it (an older kernel), plat_mem_written reports the ranges as
// untracked and the back end uploads them in full.
//
// Host pages larger than the guest's 4 KiB (Apple silicon under Linux: 16 KiB). pstate stays per guest page and holds
// what the guest asked for; mprotect takes whole host pages, so each host page gets the most permissive protection of
// its guest pages (host_apply): a guest page can be more accessible than asked, never less, and nothing a neighbour
// still needs is taken away (0.1.18 on an M4 Max under Arch Linux ARM died at subsdk0's data permission, 0x70038be000
// being 4 KiB but not 16 KiB aligned). Only the guest's reservations take this path (guest_large). A decommit drops a
// host page's memory only when none of its guest pages is committed; otherwise it zeroes the guest pages it covers,
// which read zero when committed again as they would after MADV_DONTNEED. The written-page
// scan reports whole host pages, each as its guest pages. Traps that rely on a single guest page faulting (memory.c
// read traps and what builds on them) are off on such a host (mem_trap_init). With 4 KiB host pages none of this runs.

#define PG 4096ull
static uint8_t *pstate;
static uint64_t host_pg;   // plat_page_size, set once
static uint64_t kernel_page_size(void);
static int ww_fd = -1, pm_fd = -1;   // userfaultfd and /proc/self/pagemap; -1 when write watch is unavailable
static PlatMutex vm_lock = PLAT_MUTEX_INIT;

void ps_set(uint64_t addr, uint64_t n, uint8_t v) {   // linux_int.h
    if (addr < PS_LIMIT) memset(pstate + addr / PG, v, (size_t)((n + PG - 1) / PG));
}

static void ww_open(void) {
    int fd = (int)syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY);
    struct uffdio_api api = {.api = UFFD_API,
                             .features = UFFD_FEATURE_WP_ASYNC | UFFD_FEATURE_WP_UNPOPULATED};
    if (fd < 0 || ioctl(fd, UFFDIO_API, &api)) {
        fprintf(stderr, "platform: no asynchronous userfaultfd write protection (%s): written memory is not tracked\n",
                strerror(errno));
        if (fd >= 0) close(fd);
        return;
    }
    pm_fd = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
    if (pm_fd < 0) {
        close(fd);
        return;
    }
    ww_fd = fd;
}

void ww_register(uint64_t addr, uint64_t n) {   // linux_int.h: the write watch over a fresh mapping of a reservation
    struct uffdio_register reg = {.range = {addr, n}, .mode = UFFDIO_REGISTER_MODE_WP};
    if (ww_fd >= 0) ioctl(ww_fd, UFFDIO_REGISTER, &reg);
}

int plat_mem_reserve(uint64_t addr, uint64_t n, int watch) {
    plat_mutex_lock(&vm_lock);
    if (!pstate) {
        void *p = mmap(NULL, PS_LIMIT / PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        pstate = p == MAP_FAILED ? NULL : p;
    }
    if (watch && ww_fd < 0 && pm_fd < 0) ww_open();
    void *p = pstate && addr + n <= PS_LIMIT
                  ? mmap((void *)(uintptr_t)addr, n, PROT_NONE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0)
                  : MAP_FAILED;
    int ok = p == (void *)(uintptr_t)addr;
    if (p != MAP_FAILED && !ok) munmap(p, n);   // a kernel without MAP_FIXED_NOREPLACE put it elsewhere
    if (ok) {
        ps_set(addr, n, PS_RESERVED);
        // Transparent huge pages would populate (and count as written) 2 MiB at the first write of one page; writes are
        // tracked per 4 KiB page, as on Windows.
        if (watch) madvise(p, n, MADV_NOHUGEPAGE);
        struct uffdio_register reg = {.range = {addr, n}, .mode = UFFDIO_REGISTER_MODE_WP};
        if (watch && ww_fd >= 0 && ioctl(ww_fd, UFFDIO_REGISTER, &reg))
            fprintf(stderr, "platform: write watch over %#llx+%#llx failed (%s)\n", (unsigned long long)addr,
                    (unsigned long long)n, strerror(errno));
    }
    plat_mutex_unlock(&vm_lock);
    return ok;
}

void plat_mem_name(const void *p, size_t n, const char *name) {
#ifndef PR_SET_VMA
#define PR_SET_VMA 0x53564d41
#define PR_SET_VMA_ANON_NAME 0
#endif
    // The kernel copies the name; a kernel without CONFIG_ANON_VMA_NAME refuses it and the range stays "anon".
    if (p && n) prctl(PR_SET_VMA, PR_SET_VMA_ANON_NAME, (unsigned long)(uintptr_t)p, (unsigned long)n, name);
}

int64_t plat_mem_resident(const void *p, size_t n) {
    unsigned char v[4096];   // one byte per host page: 16 MiB per call with 4 KiB pages
    const size_t hp = (size_t)kernel_page_size();   // the kernel's, which mincore counts in (not MKW_HOST_PAGE_KIB)
    // mincore wants a start on one of them: a 4 KiB-aligned range on a 16 KiB kernel counts its whole first page
    size_t head = (uintptr_t)p & (hp - 1);
    p = (const char *)p - head, n += head;
    int64_t in = 0;
    for (size_t off = 0; off < n; off += sizeof v * hp) {
        size_t len = n - off < sizeof v * hp ? n - off : sizeof v * hp;
        if (mincore((char *)p + off, len, v)) return -1;
        for (size_t i = 0; i < (len + hp - 1) / hp; i++) in += (int64_t)((v[i] & 1) * hp);
    }
    return in;
}

static uint64_t kernel_page_size(void) {
    long s = sysconf(_SC_PAGESIZE);
    // a power of two from 4 KiB up; anything else (no answer) is taken as 4 KiB, the size the runtime was made for
    return s >= (long)PG && !((uint64_t)s & ((uint64_t)s - 1)) ? (uint64_t)s : PG;
}

// MKW_HOST_PAGE_KIB=16 (tests): host pages taken as 16 KiB (or 8..64, a power of two, not below the kernel's) on a
// 4 KiB kernel, so the large-page paths run on any PC (--selftest; a game run, as on Apple silicon). Anything else
// warns and keeps the kernel's size.
uint64_t plat_page_size(void) {
    uint64_t v = __atomic_load_n(&host_pg, __ATOMIC_RELAXED);
    if (!v) {
        v = kernel_page_size();
        const char *e = getenv("MKW_HOST_PAGE_KIB");
        if (e && *e) {
            char *end;
            unsigned long k = strtoul(e, &end, 10);
            uint64_t want = (uint64_t)k << 10;
            if (*end || k < 4 || k > 64 || (k & (k - 1)) || want < v)
                fprintf(stderr, "[host] platform: MKW_HOST_PAGE_KIB=%s is not 4..64 KiB, a power of two, at least the "
                                "kernel's %llu KiB: ignored\n", e, (unsigned long long)(v >> 10));
            else v = want;
        }
        __atomic_store_n(&host_pg, v, __ATOMIC_RELAXED);
    }
    return v;
}

// The protection the host page at h (aligned) needs: the most permissive one its guest pages ask for. *mixed: they ask
// for different ones. vm_lock is not needed: pstate is written before the protection that follows it.
static int host_prot(uint64_t h, int *mixed) {
    uint64_t hp = plat_page_size();
    int best = PROT_NONE, first = -1;
    *mixed = 0;
    for (uint64_t g = h; g < h + hp; g += PG) {
        uint8_t v = g < PS_LIMIT ? pstate[g / PG] : PS_FREE;
        int p = v == PS_RW ? PROT_READ | PROT_WRITE : v == PS_READ ? PROT_READ : PROT_NONE;
        if (first < 0) first = p;
        else if (p != first) *mixed = 1;
        if (p == (PROT_READ | PROT_WRITE)) best = p;
        else if (p == PROT_READ && best == PROT_NONE) best = p;
    }
    return best;
}

// The host pages over [addr, addr + n) get the protections pstate asks for, a run of equal ones per mprotect.
static int host_apply(uint64_t addr, uint64_t n) {
    uint64_t hp = plat_page_size(), lo = addr & ~(hp - 1), hi = (addr + n + hp - 1) & ~(hp - 1);
    int mixed;
    for (uint64_t x = lo, y; x < hi; x = y) {
        int p = host_prot(x, &mixed);
        for (y = x + hp; y < hi && host_prot(y, &mixed) == p;) y += hp;
        if (mprotect((void *)(uintptr_t)x, y - x, p)) return 0;
    }
    return 1;
}

// Larger host pages and a range of the guest's reservations (pstate knows it): the per-host-page path. Anything else
// (plat_mem_alloc's memory, the dispatch table sealed read-only) is protected as asked, as it always was: pstate holds
// nothing there, and host_prot would make it inaccessible.
static int guest_large(uint64_t addr, uint64_t n) {
    return plat_page_size() != PG && pstate && addr + n <= PS_LIMIT && pstate[addr / PG] != PS_FREE;
}

uint64_t plat_mem_loose_pages(uint64_t addr, uint64_t n) {
    uint64_t hp = plat_page_size(), got = 0;
    if (hp == PG || !pstate || !n) return 0;
    int mixed;
    for (uint64_t x = addr & ~(hp - 1); x < addr + n; x += hp) got += (host_prot(x, &mixed), mixed);
    return got;
}

void *plat_mem_alloc(size_t n) {
    void *p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

int plat_mem_commit(uint64_t addr, size_t n) {
    if (guest_large(addr, n)) {   // larger host pages: what the guest asks, then the union per host page
        plat_mutex_lock(&vm_lock);
        ps_set(addr, n, PS_RW);
        int ok = host_apply(addr, n);
        plat_mutex_unlock(&vm_lock);
        return ok;
    }
    if (mprotect((void *)(uintptr_t)addr, n, PROT_READ | PROT_WRITE)) return 0;
    ps_set(addr, n, PS_RW);
    return 1;
}

// Larger host pages: a host page none of whose guest pages stays committed gives its memory up; one that keeps a
// committed guest page keeps its memory and has the decommitted guest pages zeroed (made writable for that while
// vm_lock is held: a neighbour can only gain access meanwhile), so they read zero when committed again.
static int decommit_large(uint64_t addr, uint64_t n) {
    uint64_t hp = plat_page_size(), lo = addr & ~(hp - 1), hi = (addr + n + hp - 1) & ~(hp - 1);
    ps_set(addr, n, PS_RESERVED);
    for (uint64_t x = lo; x < hi; x += hp) {
        int kept = 0;
        for (uint64_t g = x; g < x + hp && !kept; g += PG) kept = g < PS_LIMIT && pstate[g / PG] >= PS_NONE;
        if (!kept) {
            if (madvise((void *)(uintptr_t)x, hp, MADV_DONTNEED)) return 0;
            continue;
        }
        uint64_t a = x > addr ? x : addr, b = x + hp < addr + n ? x + hp : addr + n;
        if (mprotect((void *)(uintptr_t)x, hp, PROT_READ | PROT_WRITE)) return 0;
        memset((void *)(uintptr_t)a, 0, b - a);
    }
    return host_apply(addr, n);
}

// Shared pages (linux_share.c) give their memory up by leaving it: MADV_DONTNEED would keep their bytes.
int plat_mem_decommit(uint64_t addr, uint64_t n) {
    if (!plat_mem_unshare(addr, n)) return 0;
    if (guest_large(addr, n)) {
        plat_mutex_lock(&vm_lock);
        int ok = decommit_large(addr, n);
        plat_mutex_unlock(&vm_lock);
        return ok;
    }
    if (madvise((void *)(uintptr_t)addr, n, MADV_DONTNEED) || mprotect((void *)(uintptr_t)addr, n, PROT_NONE)) return 0;
    ps_set(addr, n, PS_RESERVED);
    return 1;
}

// Shared pages leave it: only inaccessible ranges are dropped. Larger host pages: only the host pages inside the range,
// whose bytes are all the caller's; the ends keep their memory.
int plat_mem_drop(uint64_t addr, uint64_t n) {
    if (!plat_mem_unshare(addr, n)) return 0;
    uint64_t hp = plat_page_size(), lo = (addr + hp - 1) & ~(hp - 1), hi = (addr + n) & ~(hp - 1);
    if (hp == PG) lo = addr, hi = addr + n;
    return lo >= hi || madvise((void *)(uintptr_t)lo, hi - lo, MADV_DONTNEED) == 0;
}

int plat_mem_protect(uint64_t addr, uint64_t n, int prot) {
    if (guest_large(addr, n)) {
        plat_mutex_lock(&vm_lock);
        ps_set(addr, n, (uint8_t)(PS_NONE + prot));
        int ok = host_apply(addr, n);
        plat_mutex_unlock(&vm_lock);
        return ok;
    }
    int p = prot == PLAT_PROT_RW ? PROT_READ | PROT_WRITE : prot == PLAT_PROT_READ ? PROT_READ : PROT_NONE;
    if (mprotect((void *)(uintptr_t)addr, n, p)) return 0;
    ps_set(addr, n, (uint8_t)(PS_NONE + prot));
    return 1;
}

// /proc/self/mem writes with FOLL_FORCE, so inaccessible pages of a private mapping take the bytes. A kernel built
// without that (proc_mem.force_override=never) refuses: checked once on a page of our own.
static int self_mem(void);

int plat_mem_poke(uint64_t addr, const void *src, size_t n) {
    if (plat_mem_any_shared(addr, n)) {   // the kernel refuses a forced write into a shared mapping: through its view
        for (uint64_t x = addr, y; x < addr + n; x = y) {
            y = (x & ~(PG - 1)) + PG < addr + n ? (x & ~(PG - 1)) + PG : addr + n;
            const uint8_t *s = (const uint8_t *)src + (x - addr);
            uint8_t *v = plat_mem_shared_at(x);
            int fd;
            if (v) memcpy(v + (x & (PG - 1)), s, y - x);
            else if ((fd = self_mem()) < 0 || pwrite(fd, s, y - x, (off_t)x) != (ssize_t)(y - x)) return 0;
        }
        return 1;
    }
    int fd = self_mem();
    return fd >= 0 && pwrite(fd, src, n, (off_t)addr) == (ssize_t)n;
}

// Reads through the page protection, as plat_mem_poke writes: the runtime copying a page MKW_EARLY_READ watches.
int plat_mem_peek(void *dst, uint64_t addr, size_t n) {
    int fd = self_mem();
    return fd >= 0 && pread(fd, dst, n, (off_t)addr) == (ssize_t)n;
}

static int self_mem(void) {   // /proc/self/mem, once: -1 when it cannot write through a no-access page
    static int fd = -2;
    if (fd == -2) {
        int f = open("/proc/self/mem", O_RDWR | O_CLOEXEC);
        char *t = mmap(NULL, PG, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        int ok = f >= 0 && t != MAP_FAILED && pwrite(f, "x", 1, (off_t)(uintptr_t)t) == 1;
        if (t != MAP_FAILED) munmap(t, PG);
        if (!ok && f >= 0) close(f);
        __atomic_store_n(&fd, ok ? f : -1, __ATOMIC_RELEASE);
    }
    return fd;
}

int plat_mem_state(uint64_t addr, uint64_t limit, uint64_t *end) {
    if (!pstate || addr >= PS_LIMIT) {
        *end = limit;
        return PLAT_MEM_FREE;
    }
    uint64_t lim = limit < PS_LIMIT ? limit : PS_LIMIT, i = addr / PG;
    uint8_t v = pstate[i];
    while ((i + 1) * PG < lim && pstate[i + 1] == v) i++;
    *end = (i + 1) * PG < lim ? (i + 1) * PG : lim;
    return v == PS_FREE ? PLAT_MEM_FREE : v == PS_RESERVED ? PLAT_MEM_RESERVED : v - PS_NONE;
}

// Scans [at, end) (host-page aligned) for written pages, reporting the guest pages inside [lo, hi) after got in pages.
// reset: the host pages reported become unwritten again, so each must fit whole (a host page cut short here would be
// protected again with guest pages left unreported); with room for less than one, it is peeked instead and stays
// written for the next scan. A peek may stop inside a host page. -1 when the scan fails.
static int64_t scan_written(uint64_t at, uint64_t end, uint64_t lo, uint64_t hi, uint64_t *pages, uint64_t max,
                            uint64_t got, int reset) {
    struct page_region r[256];
    const uint64_t per = plat_page_size() / PG, kp = kernel_page_size();
    // the kernel's own pages bound the scan: with MKW_HOST_PAGE_KIB on a 4 KiB kernel the first written page of a host
    // page may be a neighbour outside [lo, hi), and a peek of one page stopped there
    if (at < (lo & ~(kp - 1))) at = lo & ~(kp - 1);
    if (hi < UINT64_MAX - kp && end > ((hi + kp - 1) & ~(kp - 1))) end = (hi + kp - 1) & ~(kp - 1);
    while (at < end && got < max) {
        uint64_t room = (max - got) / per;
        int rs = reset && room;
        // Written means: write-protection cleared, on a populated page (present or swapped out) that is not the shared
        // zero page. A page never touched since its commit has no protection marker either and counts as written to
        // the kernel; a read maps the zero page there. Neither is a write, as on Windows.
        struct pm_scan_arg arg = {.size = sizeof arg,
                                  .flags = rs ? PM_SCAN_WP_MATCHING | PM_SCAN_CHECK_WPASYNC : 0,
                                  .start = at, .end = end, .vec = (uint64_t)(uintptr_t)r, .vec_len = 256,
                                  .max_pages = room ? room : 1,
                                  .category_inverted = PAGE_IS_PFNZERO,
                                  .category_mask = PAGE_IS_WRITTEN | PAGE_IS_PFNZERO,
                                  .category_anyof_mask = PAGE_IS_PRESENT | PAGE_IS_SWAPPED,
                                  .return_mask = PAGE_IS_WRITTEN};
        long k = ioctl(pm_fd, PAGEMAP_SCAN, &arg);
        if (k < 0) return -1;
        for (long i = 0; i < k; i++)
            for (uint64_t p = r[i].start > lo ? r[i].start : lo; p < r[i].end && p < hi && got < max; p += PG)
                pages[got++] = p;
        if (arg.walk_end <= at) break;
        at = arg.walk_end;
    }
    return (int64_t)got;
}

// Larger host pages (plat_page_size): the kernel reports and protects whole host pages, each reported here as its guest
// pages. A host page the range covers only in part is peeked, never reset: its other guest pages may belong to another
// caller's range (two pools sharing a 16 KiB page), whose written marks a reset here would take away. It is reported
// again until a range that covers it whole resets it: an upload too many at worst, never a write missed.
uint64_t plat_mem_written(uint64_t addr, uint64_t n, uint64_t *pages, uint64_t max, int reset) {
    if (!max || !n) return 0;
    if (ww_fd < 0) return UINT64_MAX;
    const uint64_t hp = plat_page_size(), end = addr + n;
    const uint64_t a0 = addr & ~(hp - 1), a1 = (addr + hp - 1) & ~(hp - 1);   // the head's host page, the body's start
    const uint64_t b1 = (end + hp - 1) & ~(hp - 1), b0 = end & ~(hp - 1);      // the tail's host page, the body's end
    int64_t got = 0;
    if (hp == PG) got = scan_written(addr, end, 0, UINT64_MAX, pages, max, 0, reset);   // as it always was
    else if (a1 >= b0)   // no whole host page inside: peeked only, unless the range is exactly its host pages
        got = scan_written(a0, b1, addr, end, pages, max, 0, reset && a0 == addr && b1 == end);
    if (hp == PG || a1 >= b0) return got < 0 ? UINT64_MAX : (uint64_t)got;
    if (a0 < a1 && (got = scan_written(a0, a1, addr, a1, pages, max, (uint64_t)got, 0)) < 0) return UINT64_MAX;
    if ((got = scan_written(a1, b0, a1, b0, pages, max, (uint64_t)got, reset)) < 0) return UINT64_MAX;
    if (b0 < b1 && (got = scan_written(b0, b1, b0, end, pages, max, (uint64_t)got, 0)) < 0) return UINT64_MAX;
    return (uint64_t)got;
}

uint64_t plat_mem_populated(uint64_t addr, uint64_t n, uint64_t (*runs)[2], uint64_t max) {
    if (!max || !n) return 0;
    static int own = -1;   // not pm_fd: that one being open means write watch is on (plat_mem_reserve)
    if (pm_fd < 0 && own < 0) own = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
    int fd = pm_fd >= 0 ? pm_fd : own;
    if (fd < 0) return UINT64_MAX;
    struct page_region r[256];
    uint64_t got = 0;
    // the scan starts on a kernel page (EINVAL otherwise: a 4 KiB start on a 16 KiB kernel); runs are cut to the range
    for (uint64_t at = addr & ~(kernel_page_size() - 1), end = addr + n; at < end && got < max;) {
        struct pm_scan_arg arg = {.size = sizeof arg, .start = at, .end = end, .vec = (uint64_t)(uintptr_t)r,
                                  .vec_len = 256, .category_inverted = PAGE_IS_PFNZERO,
                                  .category_mask = PAGE_IS_PFNZERO,
                                  .category_anyof_mask = PAGE_IS_PRESENT | PAGE_IS_SWAPPED};
        long k = ioctl(fd, PAGEMAP_SCAN, &arg);
        if (k < 0) return UINT64_MAX;
        for (long i = 0; i < k && got < max; i++) {
            uint64_t s0 = r[i].start > addr ? r[i].start : addr, s1 = r[i].end < end ? r[i].end : end;
            if (s0 >= s1) continue;
            if (got && runs[got - 1][1] == s0) runs[got - 1][1] = s1;
            else runs[got][0] = s0, runs[got][1] = s1, got++;
        }
        if (arg.walk_end <= at) break;
        at = arg.walk_end;
    }
    return got;
}

// One handler for the synchronous faults. A SIGSEGV goes to the fault hook first (memory.c thaws a frozen region on a
// write, fills a read-trapped page on any access); a fault in this executable's code then goes to the crash
// hook (main.c); anything else gets the default action. Runs on the thread's signal stack.
static int (*fault_hook)(uint64_t addr);
static int (*watch_fn)(uint64_t addr, uint64_t rip);   // plat_mem_watch_hook: write watches set for diagnostics
static void (*crash_fn)(const PlatCrash *c);

void plat_mem_watch_hook(int (*fn)(uint64_t addr, uint64_t rip)) { watch_fn = fn; }
void plat_mem_lazy_hook(int (*fn)(uint64_t addr, int write)) { (void)fn; }   // Linux commits on touch

_Thread_local int plat_fault_write;        // platform.h
_Thread_local uint64_t plat_fault_pc, plat_fault_ret;

#if defined(__aarch64__)
// x86's page fault error code from the ESR the kernel puts in the signal frame (an esr_context record): 2 a write
// (data abort with WnR), 16 an instruction fetch (instruction abort). A leaf's caller and a wild call's return address
// are in x30 there, not on the stack.
static long fault_err(const ucontext_t *u) {
    const unsigned char *p = u->uc_mcontext.__reserved, *end = p + sizeof u->uc_mcontext.__reserved;
    while (p + sizeof(struct _aarch64_ctx) <= end) {
        const struct _aarch64_ctx *h = (const struct _aarch64_ctx *)p;
        if (!h->magic || !h->size) break;
        if (h->magic == ESR_MAGIC) {
            uint64_t esr = ((const struct esr_context *)h)->esr, ec = esr >> 26;
            if (ec == 0x20 || ec == 0x21) return 16;
            if ((ec == 0x24 || ec == 0x25) && (esr & (1u << 6))) return 2;
            return 0;
        }
        p += h->size;
    }
    return 0;
}
#define UC_PC(u) ((u)->uc_mcontext.pc)
#define UC_SP(u) ((u)->uc_mcontext.sp)
#define UC_RET(u) ((u)->uc_mcontext.regs[30])
#else
static long fault_err(const ucontext_t *u) { return u->uc_mcontext.gregs[REG_ERR]; }
#define UC_PC(u) ((u)->uc_mcontext.gregs[REG_RIP])
#define UC_SP(u) ((u)->uc_mcontext.gregs[REG_RSP])
#define UC_RET(u) (*(const uint64_t *)(uintptr_t)(u)->uc_mcontext.gregs[REG_RSP])
#endif

static void fault_handler(int sig, siginfo_t *si, void *uc) {
    ucontext_t *u = uc;
    long err = fault_err(u);
    if (sig == SIGSEGV && watch_fn && (err & 2) &&
        watch_fn((uint64_t)(uintptr_t)si->si_addr, (uint64_t)UC_PC(u)))
        return;
    plat_fault_write = (err & 2) != 0, plat_fault_pc = (uint64_t)UC_PC(u);   // for the hook
    plat_fault_ret = (uint64_t)UC_RET(u);   // a leaf (libc memcpy): its caller
    if (sig == SIGSEGV && fault_hook && fault_hook((uint64_t)(uintptr_t)si->si_addr)) return;   // reads too: traps
    uintptr_t rip = (uintptr_t)UC_PC(u);
    Dl_info at = {0}, self = {0};
    // Ours: this executable, the game code library (mkwgame.so), or no module at all (a jump to a wild address)
    int in_module = dladdr((void *)rip, &at) != 0, ours_exe = 0, ours_game = 0;
    if (in_module && dladdr((void *)plat_crash_hook, &self)) {
        ours_exe = at.dli_fbase == self.dli_fbase;
        const char *base = at.dli_fname ? strrchr(at.dli_fname, '/') : NULL;
        ours_game = base && !strcmp(base + 1, "mkwgame.so");
    }
    if (crash_fn && (!in_module || ours_exe || ours_game)) {
        PlatCrash c = {sig == SIGSEGV ? "access violation" : sig == SIGBUS ? "bus error"
                       : sig == SIGILL ? "illegal instruction" : "arithmetic exception",
                       (uint32_t)sig, rip, in_module ? rip - (uintptr_t)at.dli_fbase : rip, -1, 0,
                       !in_module ? "no module" : ours_exe ? "mkw" : "mkwgame", 0};
#if defined(__aarch64__)
        if (!in_module) c.ret = (uintptr_t)UC_RET(u);   // a wild call left its return address in x30
#else
        uintptr_t sp = (uintptr_t)UC_SP(u);   // a wild call left its return address there
        if (!in_module && sp && !(sp & 7) && plat_mem_readable(sp, 8)) c.ret = *(const uintptr_t *)sp;
#endif
        if (sig == SIGSEGV || sig == SIGBUS)
            c.access = (err & 16) ? 8 : (err & 2) ? 1 : 0, c.addr = (uint64_t)(uintptr_t)si->si_addr;
        crash_fn(&c);
    } else {
        // Outside this executable (a driver or system library): no crash report, but say where before dying.
        Dl_info lib = {0};
        dladdr((void *)rip, &lib);
        char line[512];
        int n = snprintf(line, sizeof line, "[host] fatal signal %d at %#lx (%s+%#lx, %s) addr %p err %#lx\n", sig,
                         (unsigned long)rip, lib.dli_fname ? lib.dli_fname : "?",
                         (unsigned long)(rip - (uintptr_t)lib.dli_fbase), lib.dli_sname ? lib.dli_sname : "?",
                         si->si_addr, (unsigned long)err);
        if (n > 0) (void)!write(2, line, (size_t)n < sizeof line ? (size_t)n : sizeof line - 1);
        void *frames[32];
        backtrace_symbols_fd(frames, backtrace(frames, 32), 2);   // the host call into the library
    }
    signal(sig, SIG_DFL);   // returns into the faulting instruction, which faults again with the default action
}

static void fault_install(void) {
    static int done;
    if (done) return;
    done = 1;
    alt_stack();
    struct sigaction sa = {.sa_sigaction = fault_handler, .sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER};
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
}

void plat_mem_fault_hook(int (*fn)(uint64_t addr)) {
    fault_hook = fn;
    fault_install();
}

void plat_crash_hook(void (*fn)(const PlatCrash *c)) {
    crash_fn = fn;
    fault_install();
}

int plat_shadow_stack(unsigned *flags) { (void)flags; return -1; }

void plat_reexec(void) {
    static char args[16384];
    char *argv[256];
    int argc = 0;
    FILE *f = fopen("/proc/self/cmdline", "rb");
    size_t n = f ? fread(args, 1, sizeof args - 1, f) : 0;
    if (f) fclose(f);
    args[n] = 0;
    for (size_t i = 0; i < n && argc < 255; i += strlen(args + i) + 1) argv[argc++] = args + i;
    argv[argc] = NULL;
    fflush(stdout);
    fflush(stderr);
    execv("/proc/self/exe", argv);
    fprintf(stderr, "FATAL: re-running the process failed (%s)\n", strerror(errno));
    _exit(70);
}

int plat_mem_read(uint64_t addr, void *out, size_t n) {
    struct iovec local = {out, n}, remote = {(void *)(uintptr_t)addr, n};
    return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == (ssize_t)n;
}

int plat_mem_readable(uint64_t addr, uint64_t n) {
    uint8_t b;
    uint64_t len = n ? n : 1;
    // A range up to or past 2^64 (no user memory up there): its end wrapped, the loop read nothing and this said yes
    // to a frame walk's 16 bytes at 0xfffffffffffffff8.
    if (len > UINT64_MAX - addr) return 0;
    uint64_t end = addr + len;
    for (uint64_t a = addr; a < end; a = (a | 4095) + 1)
        if (!plat_mem_read(a, &b, 1)) return 0;
    return 1;
}

void *plat_aligned_alloc(size_t size, size_t align) {
    void *p = NULL;
    return posix_memalign(&p, align < sizeof(void *) ? sizeof(void *) : align, size) ? NULL : p;
}

void plat_aligned_free(void *p) { free(p); }

// Fresh read-write anonymous pages over [addr, addr + n) of a reservation (a placed map undone, vk_memory.c), back
// under the write watch like the pages they replace.
int plat_mem_replace_anon(uint64_t addr, uint64_t n) {
    // a fresh mapping replaces whole host pages: a range that cuts one would take a neighbour's bytes along
    if ((addr | n) & (plat_page_size() - 1)) return 0;
    share_forget(addr, n);
    void *p = mmap((void *)(uintptr_t)addr, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED,
                   -1, 0);
    if (p != (void *)(uintptr_t)addr) return 0;
    madvise(p, n, MADV_NOHUGEPAGE);
    ww_register(addr, n);
    ps_set(addr, n, PS_RW);
    return 1;
}
