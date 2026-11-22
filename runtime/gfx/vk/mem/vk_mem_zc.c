// Zero-copy pools (MKW_UDMABUF, Linux): the pages of a CPU-visible pool are the GPU buffer's memory, as on the console.
// The pool's guest pages are swapped at its first GPU use for pages of an arena, a memfd the driver imported through
// udmabuf (platform/linux_share.c), and its buffer is bound to the arena's memory there. No shadow copy of the pool
// (which doubles it in RAM, is synced before each submit and written back at each fence), and no userptr import, which
// amdgpu invalidates at every change of page protection (vk_mem_shadow.c) and refuses over an inaccessible page.
// What the rest of the layer sees is an imported pool without a shadow: smap is NULL, and mem too, since the memory is
// the arena's and nothing has to wait for the GPU when the pool goes.
// The guest range keeps the arena's pages after its pool is finalized: the next pool made there finds them and binds to
// them without a copy. A page of an arena is free again when no pool record holds it and the guest range it stood for
// was given back or got other pages.
// Pages the spill takes (vk_mem_spill.c, only where no pool record holds them) leave the arena the same way.
// Pools this does not take (too large, sharing pages with another live pool, a file read in flight over them, a kernel
// without udmabuf, a driver that refuses it) get a shadow copy as before.
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "gfx.h"
#include "host.h"
#include "nvn_api.h"
#include "nvn_pool_index.h"
#include "vk.h"
#include "vk_mem_internal.h"
#include "platform/platform.h"

#define PG 0x1000ull
#define ARENA_BYTES (64ull << 20)
#define ZC_MAX (256ull << 20)   // a larger pool keeps a shadow: its arena would be pinned whole
#define ARENAS 128

typedef struct {
    PlatShared s;
    VkDeviceMemory mem;
    uint64_t *guest;     // per page: the guest page it was last put at, 0 never
    uint16_t *refs;      // per page: the pool records bound to it
    uint32_t npages, next;   // next: where the last allocation ended
} Arena;

static PlatMutex z_lock = PLAT_MUTEX_INIT;   // after pool_lock; a leaf besides gen_lock's stamp
static Arena arenas[ARENAS];
static int narenas;
static int dead;                        // an arena could not be made: no further tries
static int64_t live_n, live_bytes, swapped_n, reused_n, refused_n;
static PFN_vkGetMemoryFdPropertiesKHR p_fd_props;

static int free_page(const Arena *a, uint32_t i) {
    if (a->refs[i]) return 0;
    return !a->guest[i] || plat_mem_shared_at(a->guest[i]) != a->s.map + i * PG;
}

static VkBuffer zc_buffer(VkCtx *c, uint64_t n) {
    VkExternalMemoryBufferCreateInfo eb = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
                                           .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &eb, .size = n, .usage = pool_usage(c),
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkBuffer b = VK_NULL_HANDLE;
    return vkCreateBuffer(c->device, &bi, NULL, &b) == VK_SUCCESS ? b : VK_NULL_HANDLE;
}

// z_lock held. A new arena of at least n bytes, or NULL (said once).
static Arena *arena_new(VkCtx *c, uint64_t n) {
    const char *why = "too many arenas";
    if (dead || narenas == ARENAS) return NULL;
    Arena *a = &arenas[narenas];
    *a = (Arena){0};
    uint64_t size = n > ARENA_BYTES ? n : ARENA_BYTES;
    VkBuffer probe = VK_NULL_HANDLE;
    if (!plat_shared_open(size, &a->s, &why)) goto fail;
    a->npages = (uint32_t)(size / PG);
    a->guest = calloc(a->npages, sizeof *a->guest);
    a->refs = calloc(a->npages, sizeof *a->refs);
    if (!a->guest || !a->refs) rt_fatal("vk: out of memory for a shared arena");
    if (!p_fd_props)
        p_fd_props = (PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(c->device, "vkGetMemoryFdPropertiesKHR");
    VkMemoryFdPropertiesKHR fp = {.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
    VkMemoryRequirements req;
    why = "the driver does not know the dma-buf";
    if (!p_fd_props || p_fd_props(c->device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, a->s.dmabuf, &fp) != VK_SUCCESS)
        goto fail;
    why = "no buffer over it";
    if (!(probe = zc_buffer(c, size))) goto fail;
    vkGetBufferMemoryRequirements(c->device, probe, &req);
    vkDestroyBuffer(c->device, probe, NULL);
    // Cached on the CPU (the game reads what the GPU writes) and in the GPU's caches: RADV also offers its
    // device-coherent type, uncached on the GPU.
    int type = -1;
    uint32_t bits = fp.memoryTypeBits & req.memoryTypeBits;
    for (uint32_t i = 0; i < c->mem_props.memoryTypeCount && type < 0; i++) {
        VkMemoryPropertyFlags f = c->mem_props.memoryTypes[i].propertyFlags;
        if ((bits >> i & 1) && (f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) && (f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) &&
            !(f & (VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD | VK_MEMORY_PROPERTY_DEVICE_UNCACHED_BIT_AMD)))
            type = (int)i;
    }
    why = "no cached coherent memory type for it";
    if (type < 0 || req.alignment > PG || req.size > size) goto fail;
    VkImportMemoryFdInfoKHR im = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
                                  .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, .fd = a->s.dmabuf};
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &im, .allocationSize = size,
                               .memoryTypeIndex = (uint32_t)type};
    why = "the driver refused the import";
    if (vkAllocateMemory(c->device, &ai, NULL, &a->mem) != VK_SUCCESS) goto fail;
    a->s.dmabuf = -1;   // the driver's now
    narenas++;
    if (narenas <= 2 || !(narenas & (narenas - 1)))
        rt_log("mem: zero-copy pools: %d arenas (newest %.0f MiB, memory type %d)", narenas, size / 1048576.0, type);
    return a;
fail:
    plat_shared_close(&a->s);
    free(a->guest), free(a->refs);
    *a = (Arena){0};
    if (n > ARENA_BYTES) {   // udmabuf's size limit (64 MiB by default) or a refusal of this one: the rest go on
        static int said;
        if (said++ < 4) rt_log("mem: zero-copy: no arena of %.0f MiB (%s): a shadow copy of that pool", size / 1048576.0, why);
        return NULL;
    }
    rt_log("mem: zero-copy pools stop here (%s): shadow copies from now on (%d arenas)", why, narenas);
    dead = 1;
    return NULL;
}

// z_lock held: n contiguous free pages of an arena, from where its last allocation ended.
static int fit(Arena *a, uint32_t n, uint32_t *at) {
    for (int lap = 0; lap < 2; lap++) {
        uint32_t i = lap ? 0 : a->next, end = lap && a->next + n < a->npages ? a->next + n : a->npages;
        for (uint32_t run = 0; i < end; i++) {
            run = free_page(a, i) ? run + 1 : 0;
            if (run < n) continue;
            *at = i + 1 - n;
            a->next = i + 1 == a->npages ? 0 : i + 1;
            return 1;
        }
    }
    return 0;
}

static Arena *arena_alloc(VkCtx *c, uint64_t bytes, uint32_t *at) {
    uint32_t n = (uint32_t)(bytes / PG);
    for (int i = 0; i < narenas; i++)
        if (arenas[i].npages >= n && fit(&arenas[i], n, at)) return &arenas[i];
    Arena *a = arena_new(c, bytes);
    return a && fit(a, n, at) ? a : NULL;
}

// The pages [lo, hi) are one run of an arena already, from an earlier pool there.
static Arena *already(uint64_t lo, uint64_t hi, uint32_t *at) {
    const uint8_t *v = plat_mem_shared_at(lo);
    if (!v) return NULL;
    for (uint64_t p = lo; p < hi; p += PG)
        if (plat_mem_shared_at(p) != v + (p - lo)) return NULL;
    for (int i = 0; i < narenas; i++)
        if (v >= arenas[i].s.map && v + (hi - lo) <= arenas[i].s.map + arenas[i].s.size) {
            *at = (uint32_t)((uint64_t)(v - arenas[i].s.map) / PG);
            return &arenas[i];
        }
    return NULL;
}

static int zc_on(const VkCtx *c) {
    static int on = -1;
    if (on < 0) {
        const char *e = cfg_get("MKW_UDMABUF");
        int want = e && !strcmp(e, "1");
        if (e && *e && !want && strcmp(e, "0")) rt_log("mem: MKW_UDMABUF=%s is not 0 or 1: 0", e);
        on = want && c->has_dma_buf;
        if (want && !on) rt_log("mem: MKW_UDMABUF: the driver has no VK_EXT_external_memory_dma_buf");
        if (on) rt_log("mem: zero-copy pools on (MKW_UDMABUF): the GPU uses the game's own pages");
    }
    return on;
}

// pool_lock held exclusively (lazy_back).
int zc_pool(VkCtx *c, PoolRec *r, uint64_t addr, uint64_t size) {
    if (!zc_on(c) || !r->lazy) return 0;
    uint64_t lo = addr & ~(PG - 1), hi = (addr + size + PG - 1) & ~(PG - 1), n = hi - lo;
    if (n > ZC_MAX) goto refuse;
    for (int i = 0; i < pool_index.n; i++) {   // a page has one place: no other pool with storage may hold it
        const PoolRec *o = pool_index.e[i].buf;
        if (o->key != r->key && o->kind == POOL_IMPORT && o->buf && o->import_lo < hi && o->import_hi > lo) goto refuse;
    }
    VkBuffer buf = zc_buffer(c, n);
    if (!buf) goto refuse;
    VkMemoryRequirements req;   // not asked for its memory type again: every pool's buffer has the arena probe's usage
    vkGetBufferMemoryRequirements(c->device, buf, &req);
    plat_mutex_lock(&z_lock);
    uint32_t at = 0;
    Arena *a = already(lo, hi, &at);
    int reuse = a != NULL;
    if (!a) a = req.size <= n ? arena_alloc(c, n, &at) : NULL;
    int ok = a && vkBindBufferMemory(c->device, buf, a->mem, at * PG) == VK_SUCCESS &&
             (reuse || mem_share(lo, hi, &a->s, at * PG));
    if (ok) {
        for (uint32_t k = 0; k < n / PG; k++) a->refs[at + k]++, a->guest[at + k] = lo + k * PG;
        live_n++, live_bytes += (int64_t)n;
        if (reuse) reused_n++;
        else swapped_n++;
    }
    plat_mutex_unlock(&z_lock);
    if (!ok) {
        vkDestroyBuffer(c->device, buf, NULL);
        goto refuse;
    }
    if (!reuse) vk_guest_written(lo, n);   // the marks of writes not yet collected went with the old pages
    r->buf = buf, r->import_lo = lo, r->import_hi = hi;
    r->zc = (int)(a - arenas) + 1, r->zc_at = at;
    plat_inc32(&n_imports);
    return 1;
refuse:
    refused_n++;
    return 0;
}

void zc_release(PoolRec *r) {   // its buffer is gone
    plat_mutex_lock(&z_lock);
    Arena *a = &arenas[r->zc - 1];
    for (uint64_t k = 0; k < (r->import_hi - r->import_lo) / PG; k++) a->refs[r->zc_at + k]--;
    live_n--, live_bytes -= (int64_t)(r->import_hi - r->import_lo);
    plat_mutex_unlock(&z_lock);
    r->zc = 0;
}

int zc_in_use(uint64_t lo, uint64_t hi) {
    int hit = 0;
    plat_mutex_lock(&z_lock);
    for (uint64_t p = lo & ~(PG - 1); narenas && p < hi && !hit; p += PG) {
        const uint8_t *v = plat_mem_shared_at(p);
        for (int i = 0; v && i < narenas && !hit; i++)
            if (v >= arenas[i].s.map && v < arenas[i].s.map + arenas[i].s.size)
                hit = arenas[i].refs[(uint64_t)(v - arenas[i].s.map) / PG] != 0;
    }
    plat_mutex_unlock(&z_lock);
    return hit;
}

void zc_log(uint64_t frame) {
    if (!narenas) return;
    plat_mutex_lock(&z_lock);
    uint64_t bytes = 0;
    for (int i = 0; i < narenas; i++) bytes += arenas[i].s.size;
    rt_log("mem: zero-copy at frame %llu: %lld pools %.1f MiB in %d arenas of %.0f MiB; pools swapped in %lld, on pages of an "
           "earlier one %lld, refused %lld", (unsigned long long)frame, (long long)live_n, live_bytes / 1048576.0, narenas,
           bytes / 1048576.0, (long long)swapped_n, (long long)reused_n, (long long)refused_n);
    plat_mutex_unlock(&z_lock);
}

void zc_shutdown(VkCtx *c) {   // the device goes: every pool's buffer is gone
    for (int i = 0; i < narenas; i++)
        if (arenas[i].mem) vkFreeMemory(c->device, arenas[i].mem, NULL), arenas[i].mem = VK_NULL_HANDLE;
    narenas = 0, dead = 1;   // the guest keeps the pages; a device made again gets shadow copies
}
