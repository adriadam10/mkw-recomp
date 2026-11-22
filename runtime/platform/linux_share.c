// Linux: guest pages the GPU reads and writes where they are (PlatShared, platform.h). A memfd holds the bytes, udmabuf
// (/dev/udmabuf) makes a dma-buf of its pages for the driver to import, and the same pages are mapped over the guest
// range. The pages stay pinned while the dma-buf lives, so page protections over the guest range (the write watch, read
// traps, frozen regions) never reach the GPU, unlike a userptr import: amdgpu invalidates one at each change of
// protection and refuses the submit when the range holds an inaccessible page ("The CS has been rejected (-1)", a
// four-player race with MKW_SHADOW=0 on an RX 6700 XT, 21/11/2026).
// /dev/udmabuf is open to the user at the seat since systemd 257 (udev's uaccess tag); where it is not, nothing here works
// and the caller keeps its copies.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <linux/udmabuf.h>

#include "platform.h"
#include "linux_int.h"

#define PG 4096ull
#ifndef MFD_ALLOW_SEALING
#define MFD_ALLOW_SEALING 2u
#endif
#ifndef F_ADD_SEALS
#define F_ADD_SEALS 1033
#define F_SEAL_SHRINK 2
#endif

static uint8_t **at;          // per guest page its view (PlatShared.map), NULL when the page is the reservation's own;
                              // mapped lazily: only the pages touched cost memory
static int64_t nshared;       // pages shared now: the poke's and decommit's fast way out

int plat_shared_open(uint64_t size, PlatShared *s, const char **why) {
    static int dev = -2;
    if (dev == -2) dev = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
    if (dev < 0) {
        *why = "/dev/udmabuf cannot be opened";
        return 0;
    }
    if (!at) {
        void *p = plat_mem_alloc(PS_LIMIT / PG * sizeof *at);
        if (!p) return *why = "no address space for the page table", 0;
        at = p;
    }
    *s = (PlatShared){.memfd = -1, .dmabuf = -1, .size = size};
    s->memfd = (int)syscall(SYS_memfd_create, "mkw guest shared", (unsigned)(MFD_CLOEXEC | MFD_ALLOW_SEALING));
    if (s->memfd < 0 || ftruncate(s->memfd, (off_t)size) || fcntl(s->memfd, F_ADD_SEALS, F_SEAL_SHRINK)) {
        *why = "memfd refused";
        plat_shared_close(s);
        return 0;
    }
    struct udmabuf_create c = {.memfd = (__u32)s->memfd, .flags = UDMABUF_FLAGS_CLOEXEC, .size = size};
    s->dmabuf = ioctl(dev, UDMABUF_CREATE, &c);   // allocates and pins every page
    if (s->dmabuf < 0) {
        *why = errno == EINVAL ? "udmabuf refused the size" : "udmabuf refused";
        plat_shared_close(s);
        return 0;
    }
    void *m = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, s->memfd, 0);
    if (m == MAP_FAILED) {
        *why = "the view could not be mapped";
        plat_shared_close(s);
        return 0;
    }
    s->map = m;
    return 1;
}

void plat_shared_close(PlatShared *s) {
    if (s->map) munmap(s->map, s->size);
    if (s->dmabuf >= 0) close(s->dmabuf);
    if (s->memfd >= 0) close(s->memfd);
    *s = (PlatShared){.memfd = -1, .dmabuf = -1};
}

uint8_t *plat_mem_shared_at(uint64_t addr) {
    return at && addr < PS_LIMIT ? __atomic_load_n(&at[addr / PG], __ATOMIC_ACQUIRE) : NULL;
}

int plat_mem_any_shared(uint64_t addr, uint64_t n) {
    if (!__atomic_load_n(&nshared, __ATOMIC_ACQUIRE)) return 0;
    for (uint64_t p = addr & ~(PG - 1); p < addr + n; p += PG)
        if (plat_mem_shared_at(p)) return 1;
    return 0;
}

void share_forget(uint64_t addr, uint64_t n) {
    if (!__atomic_load_n(&nshared, __ATOMIC_ACQUIRE)) return;
    for (uint64_t p = addr & ~(PG - 1); p < addr + n && p < PS_LIMIT; p += PG)
        if (at[p / PG]) {
            __atomic_store_n(&at[p / PG], NULL, __ATOMIC_RELEASE);
            __atomic_sub_fetch(&nshared, 1, __ATOMIC_ACQ_REL);
        }
}

int plat_mem_unshare(uint64_t addr, uint64_t n) {
    if (!__atomic_load_n(&nshared, __ATOMIC_ACQUIRE)) return 1;
    for (uint64_t a = addr, b; a < addr + n; a = b) {   // runs of shared pages
        while (a < addr + n && !plat_mem_shared_at(a)) a += PG;
        b = a;
        while (b < addr + n && plat_mem_shared_at(b)) b += PG;
        if (a == b) break;
        void *p = mmap((void *)(uintptr_t)a, b - a, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0);
        if (p != (void *)(uintptr_t)a) return 0;
        share_forget(a, b - a);
        madvise(p, b - a, MADV_NOHUGEPAGE);   // as the reservation's own pages (plat_mem_reserve)
        ww_register(a, b - a);
    }
    return 1;
}

int plat_mem_share(uint64_t addr, uint64_t n, const PlatShared *s, uint64_t off) {
    if (!at || addr + n > PS_LIMIT || off + n > s->size) return 0;
    // Read-only and present first, and their marks cleared: a page a read maps later would count as written.
    void *p = mmap((void *)(uintptr_t)addr, n, PROT_READ, MAP_SHARED | MAP_FIXED | MAP_POPULATE, s->memfd, (off_t)off);
    if (p != (void *)(uintptr_t)addr) return -1;
    madvise(p, n, MADV_NOHUGEPAGE);
    ww_register(addr, n);
    for (uint64_t x = addr, pages[256], k; x < addr + n; x = k < 256 ? addr + n : pages[255] + PG)
        if ((k = plat_mem_written(x, addr + n - x, pages, 256, 1)) == UINT64_MAX) break;   // no write watch here
    for (uint64_t k = 0; k < n; k += PG) {
        if (!at[(addr + k) / PG]) __atomic_add_fetch(&nshared, 1, __ATOMIC_ACQ_REL);
        __atomic_store_n(&at[(addr + k) / PG], s->map + off + k, __ATOMIC_RELEASE);
    }
    madvise(s->map + off, n, MADV_DONTNEED);   // the view lets go of them: they would count twice in the process's memory
    if (mprotect(p, n, PROT_READ | PROT_WRITE)) return -1;
    ps_set(addr, n, PS_RW);
    return 1;
}
