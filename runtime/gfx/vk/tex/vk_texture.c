// Vulkan textures of the graphics layer: a VkImage cache keyed by storage
// GPU address plus descriptor, uploads through staging buffers with block-linear deswizzle and ASTC decode (vk_astc.c),
// re-uploads when page write generations (vk_memory.c) show the storage written, ReadTexels / WriteTexels, the texture
// and sampler descriptor slots, and VkSamplers. Image-cache state lives here under tex_lock; slot tables under
// slots_lock (vk_tex_slots.c). Lock order: tex_lock → slots_lock → view_lock. The parts, sharing vk_tex_internal.h:
// vk_tex_image.c (images, released entries), vk_tex_upload.c, vk_tex_alias.c (aliases, GPU writes into mirrored
// storage), vk_tex_slots.c (descriptor slots, samplers) and vk_tex_texel.c (ReadTexels / WriteTexels, CPU reads).
//
// Behaviour ported from MacNativeGames games/totk/third_party/d2r_gfx/gfx/nvn @ b75be8cf (MIT, Copyright (c) 2026
// jhaizhou; origin d2r-mobile): which textures mirror their pool memory (nvn_mtl.m tex_mirrored, 3347-3362), the
// "GPU-owned" rule that stops re-uploads once the GPU rendered into a mirrored texture (nvn_mtl.m tex_mtl, 3366-3372),
// the live texture at a storage address that a slot of a finalized texture follows (hle_nvn.c:373-393), and
// ReadTexels / WriteTexels (nvn_mtl.m:10105-10195). Image creation, upload regions and samplers are ported from LAS
// Recomp runtime/nvn/vulkan/textures.c:235-330, 778-844 @ da35b83a (MIT, Copyright (c) 2026 Peiikai).
//
// Every image lives in VK_IMAGE_LAYOUT_GENERAL. Uploads are recorded into the upload batch of vk_memory.c; the replay
// submits it (vk_upload_flush) before a submission that samples the textures.
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "gfx.h"
#include "host.h"
#include "nvn_api.h"
#include "nvn_hmap.h"
#include "nvn_tex.h"
#include "tex.h"
#include "vk.h"
#include "vk_exec.h"
#include "vk_tex_internal.h"
#include "platform/platform.h"

PlatRMutex tex_lock;   // before slots_lock and view_lock when those are held too
PlatMutex slots_lock = PLAT_MUTEX_INIT;   // after tex_lock, before view_lock
PlatOnce tex_once = PLAT_ONCE_INIT;
nvn_hmap by_key = {.klen = sizeof(TexKey)};
nvn_hmap by_addr = {.klen = 8};
TexSlot *tslots;   // under slots_lock
int ntslots;
SampSlot *sslots;   // under slots_lock
int nsslots;
nvn_hmap samplers = {.klen = sizeof(samp_par)};   // under slots_lock (VkSampler cache; not Tex lifetime)
int nreleased;
uint64_t released_bytes;
Tex *rel_head, *rel_tail;   // released entries in release order (sweep_released drops from the head)
uint64_t n_uploads, n_reuploads;
// Re-uploads since the last perf line (tex_lock): all, and those of textures a WriteTexels wrote since their last upload.
uint64_t ru_n, ru_bytes, ru_ns, ru_wt_n, ru_wt_bytes, ru_wt_ns;
// MKW_TEX_ALIAS (default on; =0: a new image over live storage starts empty): a new descriptor over the storage of a
// live image that the GPU wrote gets that image's content (alias_copy). Counts since the last log line.
int tex_alias = -1;
uint64_t n_alias_copied, n_alias_upload, n_alias_uninit;
uint32_t submit_epoch = 1;   // bumped per queue submit: draw bindings check a texture's storage once per submit
// Mirrored textures the GPU wrote into, sorted by address (vk_texture_cpu_stale, vk_texture_write_back). Changed under
// tex_lock and owned_lock; read under either.
PlatMutex owned_lock = PLAT_MUTEX_INIT;
Tex **owned;
int nowned, cowned;
uint64_t max_owned;   // largest storage size in owned[]
uint64_t gpu_writes;  // GPU writes into mirrored textures so far: Tex.gpu_seq
// Bumped whenever the entry a slot resolves to can change: an entry created or destroyed, released or taken back into
// use (the slot lookup and the follow of a finalized slot read only by_key, by_addr and the released flags).
uint32_t tex_gen = 1;
// Bumped with tex_gen and whenever a slot is registered: what vk_texture_by_id returns for any slot may have changed.
// Atomic: TEX_MEMO / slot_still read it unlocked, and tex_gen_bump (tex_lock held, slots may already be held) must not
// take slots_lock.
uint32_t slots_gen = 1;
uint32_t samp_gen = 1;   // bumped whenever a sampler slot is registered: vk_sampler_by_id may answer otherwise
uint64_t n_checks;    // draw-binding storage checks (make_fresh_snap) since the last perf line
// tex_gen per storage address (hashed): a slot resolves through the entries at its own address only (slot_resolve), so
// a slot's resolution stays valid while its address's generation does. TotK finalizes and initializes transient
// textures every frame: about 90 tex_gen and 160 slots_gen bumps per frame in the monsters fight, one every 16 draws,
// which emptied every cache keyed on the global generations.
uint32_t agen[AGEN_N];
// Per slot id, for vk_texture_slot_token without the lock: the registration serial (bits 16-47; 0: never registered)
// and the address generation index (bits 0-15), one word so a reader gets a consistent pair. Fixed-size (slot ids are
// 20 bits), unlike tslots, which moves when it grows. Written under slots_lock; read unlocked.
uint64_t *sinfo;
// Per slot id, for vk_texture_checked without the lock: the submit_epoch of the slot's last freshness check (bits
// 32-63) and the agen its entry was resolved under (bits 0-31); 0 after a registration. Fixed-size like sinfo.
uint64_t *schk;
uint32_t reg_serial;   // under slots_lock; readers of tokens use the value published in sinfo

uint32_t agen_idx(uint64_t addr) { return (uint32_t)(addr * 0x9E3779B97F4A7C15ull >> 52); }

void tex_gen_bump(uint64_t addr) {
    if (!++tex_gen) tex_gen = 1;
    __atomic_fetch_add(&slots_gen, 1, __ATOMIC_RELEASE);
    uint32_t *g = &agen[agen_idx(addr)];
    __atomic_store_n(g, *g + 1, __ATOMIC_RELEASE);
}

volatile int32_t tex_ready;
void tex_init(void *p) {
    (void)p;
    __atomic_store_n(&tex_ready, 1, __ATOMIC_RELEASE);
    return;
}


// Flags that do not change where the texels are: two descriptors of one storage that differ only in them are one image.
// MKW writes from compute through an IMAGE descriptor and samples the same storage through another without the flag;
// as two images, the readers never saw the writes (the race's wave simulation and 3D LUTs, run-race33, 31/10/2026).
#define KEY_FREE_FLAGS (NVN_TEXTURE_FLAGS_IMAGE | NVN_TEXTURE_FLAGS_COMPRESSIBLE | NVN_TEXTURE_FLAGS_ADAPTIVE_ZCULL | \
                        NVN_TEXTURE_FLAGS_ZCULL_SUPPORT_STENCIL)

void make_key(const tex_t *t, TexKey *k) {
    memset(k, 0, sizeof *k);
    k->addr = (uintptr_t)t->mem;
    k->d = t->d;
    k->d.flags &= ~KEY_FREE_FLAGS;
    k->has_layout = t->has_layout;
    memcpy(k->layout, t->layout, 8);
}

// Keys equal apart from the descriptor's swizzle selectors: the same storage, format and geometry.
int same_but_swizzle(const TexKey *a, const TexKey *b) {
    TexKey x = *a, y = *b;
    x.d.swizzle_r = x.d.swizzle_g = x.d.swizzle_b = x.d.swizzle_a = 0;
    y.d.swizzle_r = y.d.swizzle_g = y.d.swizzle_b = y.d.swizzle_a = 0;
    return !memcmp(&x, &y, sizeof x);
}

// MNG nvn_mtl.m tex_mirrored: textures whose content the CPU provides in pool memory. Display, compressible, zcull,
// depth, multisample and GPU-only storage stays GPU-owned.
int tex_mirrored(const tex_t *t) {
    if (!t->mem || nvn_gpu_only((uintptr_t)t->mem)) return 0;
    if (t->has_layout) return 1;
    const pool_t *p = t->pool;
    if (!p || !(p->flags & (NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_CPU_CACHED))) return 0;
    if (t->d.flags & (NVN_TEXTURE_FLAGS_DISPLAY | NVN_TEXTURE_FLAGS_COMPRESSIBLE | NVN_TEXTURE_FLAGS_ADAPTIVE_ZCULL |
                      NVN_TEXTURE_FLAGS_DEPTH16_PREFER_FAST_CLEAR | NVN_TEXTURE_FLAGS_ZCULL_SUPPORT_STENCIL))
        return 0;
    if (t->d.samples > 1 || t->d.target == NVN_TEXTURE_TARGET_BUFFER) return 0;
    if (t->d.format >= 0x31 && t->d.format <= 0x36) return 0;   // stencil / depth formats
    return 1;
}

void log_limited(volatile int32_t *n, const char *fmt, ...) {
    if (plat_inc32(n) > 16) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    rt_log("vk: %s", buf);
}

void barrier(VkCommandBuffer cb, VkPipelineStageFlags2 src, VkAccessFlags2 sa, VkPipelineStageFlags2 dst,
                    VkAccessFlags2 da) {
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = src, .srcAccessMask = sa,
                           .dstStageMask = dst, .dstAccessMask = da};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
}
// ---------------------------------------------------------------- statistics and shutdown
void vk_texture_stats(uint64_t *images, uint64_t *uploads, uint64_t *reuploads) {
    lock();
    if (images) *images = by_key.n;
    if (uploads) *uploads = n_uploads;
    if (reuploads) *reuploads = n_reuploads;
    unlock();
}

void vk_texture_census(VkTexCensus *s) {
    VkCtx *c = vk_ctx();
    memset(s, 0, sizeof *s);
    lock();
    s->epoch = submit_epoch;
    for (uint32_t i = 0; i < by_key.cap; i++) {
        const Tex *e = by_key.vals[i];
        if (!e) continue;
        uint64_t bytes = 0;
        if (e->alloc && c) {
            VmaAllocationInfo ai;
            vmaGetAllocationInfo(c->vma, e->alloc, &ai);
            bytes = ai.size;
        }
        if (e->released) s->released++, s->own_released += e->alloc != NULL, s->bytes_released += bytes;
        else {
            s->live++, s->own_live += e->alloc != NULL, s->bytes_live += bytes;
            int fmt = e->t.d.format;
            if (fmt >= 0x79 && fmt <= 0x94) {
                s->astc_bytes += bytes, s->astc_n += bytes != 0;
                if (!e->f.astc_bc7) s->astc_rgba_bytes += bytes;
            }
            else if (e->pub.usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))
                s->target_bytes += bytes, s->target_n += bytes != 0;
            if (bytes && !(e->pub.usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))) {
                VkFormat vf = e->f.vk;
                int k = vf >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && vf <= VK_FORMAT_BC1_RGBA_SRGB_BLOCK ? 0
                        : vf == VK_FORMAT_BC7_UNORM_BLOCK || vf == VK_FORMAT_BC7_SRGB_BLOCK ? 1
                        : vf >= VK_FORMAT_BC2_UNORM_BLOCK && vf <= VK_FORMAT_BC6H_SFLOAT_BLOCK ? 2 : 3;
                s->cls_bytes[k] += bytes, s->cls_n[k]++;
                uint32_t side = e->t.d.width > e->t.d.height ? e->t.d.width : e->t.d.height;
                int z = side < 512 ? 0 : side < 1024 ? 1 : 2;
                s->size_bytes[z] += bytes, s->size_n[z]++;
                uint32_t idle = submit_epoch - e->seen;
                for (int j = 0; j < 3; j++)
                    if (idle > (64u << (3 * j))) s->idle_bytes[j] += bytes, s->idle_n[j]++;
            }
            if (bytes > (64ull << 20)) s->big_bytes += bytes;
        }
    }
    unlock();
}

void vk_texture_shutdown(void) {
    VkCtx *c = vk_ctx();
    lock();
    lock_slots();   // tex then slots: freers of slot tables and the sampler hashmap
    Tex **list = malloc(sizeof *list * (size_t)(by_key.n + 1));
    int n = 0;
    for (uint32_t i = 0; i < by_key.cap; i++)
        if (by_key.vals[i]) list[n++] = by_key.vals[i];
    for (int i = 0; i < n; i++) destroy_tex(list[i]);
    free(list);
    for (uint32_t i = 0; i < samplers.cap; i++)
        if (samplers.vals[i] && c) vkDestroySampler(c->device, (VkSampler)samplers.vals[i], NULL);
    free(by_key.keys), free(by_key.vals), free(by_addr.keys), free(by_addr.vals), free(samplers.keys), free(samplers.vals);
    by_key = (nvn_hmap){.klen = sizeof(TexKey)};
    by_addr = (nvn_hmap){.klen = 8};
    samplers = (nvn_hmap){.klen = sizeof(samp_par)};
    free(tslots), free(sslots);
    tslots = NULL, sslots = NULL, ntslots = nsslots = 0, nreleased = 0, released_bytes = 0;
    rel_head = rel_tail = NULL;
    if (sinfo) memset(sinfo, 0, sizeof *sinfo << 20);   // no slot is registered: tokens 0
    if (schk) memset(schk, 0, sizeof *schk << 20);
    unlock_slots();
    unlock();
}
