// Internal interface of the texture cache (vk_texture.c and vk_tex_*.c): the entry and slot records and the state the
// parts share. Everything else goes through vk.h.
#pragma once

#include <stdint.h>

#include "gfx.h"
#include "nvn_hmap.h"
#include "nvn_tex.h"
#include "tex.h"
#include "vk.h"
#include "platform/platform.h"

typedef struct {
    uint64_t addr;
    nvn_texdesc d;
    uint8_t has_layout, layout[8], pad[7];
} TexKey;

typedef struct Tex {
    VkTexImage pub;               // first: vk_texture_get hands out &pub
    TexKey key;
    tex_t t;                      // the newest texture object seen with this key
    TexVkFormat f;
    VmaAllocation alloc;          // own memory; NULL when bound into pool memory
    VmaAllocation bound_to;       // the pool memory it is bound into
    uint32_t wgen;                // page generation of the last upload check
    uint64_t texpre_ticket;       // vk_texpre.c: the queued cache read that filled it (0 none), checked by make_fresh
    uint32_t checked;             // submit_epoch of the last check from a draw binding
    uint32_t seen;                // submit_epoch of the last lookup of any kind (evict_idle: idle since then)
    int uploaded, released;
    uint32_t n_up;                // uploads of this entry (vk_texture_debug)
    int last_ch;                  // last vk_mem_changed result of make_fresh: 1 written, 0 clean, -1 not tracked
    struct Tex *next_at;          // other entries at the same storage address
    uint8_t gpu_dirty;            // mirrored, and the GPU wrote the image after the last write-back to the storage
    uint32_t gpu_gen;             // page generation when the GPU write that set gpu_dirty was recorded
    uint8_t *gpu_base;            // storage that fills no whole page: its bytes at that GPU write (gpu_base_note)
    uint64_t gpu_seq;             // order of that GPU write among all of them (gpu_writes)
    uint64_t gpu_fence;           // fence of the submission that made that write (0: not recorded in one)
    uint64_t wr_fence;            // any entry: newest submission recorded to write its image (render, copy, clear,
                                  // storage binding); ReadTexels waits for it instead of draining the replay queue
    uint8_t listed;               // in owned[]
    uint32_t wt_n;                // WriteTexels into the storage since the last upload
    struct Tex *rel_prev, *rel_next;   // while released: the list of released entries, oldest first
    uint64_t rel_bytes;           // while released: its own memory, counted in released_bytes
    uint64_t shash;               // XXH3 of the storage at the last upload (same_storage: no write watch)
    uint32_t shash_epoch;         // submit_epoch of the last hash check
    uint8_t sparse;               // a Vulkan sparse image whose tiles follow the game's MapVirtual (vk_sparse.c)
    VkSparseTail sparse_tail;      // privately backed small mips; zeroed at image initialization, without touching shared tiles
    uint64_t use;                 // MKW_GPU_GUARD: newest submission part that looked it up (vk_guard_note)
    uint8_t pf;                   // made by vk_tex_prefetch.c and not looked up by a draw yet
    VkTexImage dtwin;             // vk_tex_twin.c: its D32_SFLOAT copy for depth-compare sampling (image NULL: none)
    VmaAllocation dtwin_alloc;
} Tex;

// A slot keeps what vk_texture_by_id resolved for it, so a draw binding skips the key lookup and the address chain: the
// entry (ce), valid while the generation of its address (agen) is still cgen, the view and has_view handed out with it,
// and the submit_epoch of the last freshness check through the slot (cepoch). Hot fields first: a lookup that hits
// touches one or two cache lines. Slot fields under slots_lock; ce is only used while tex_lock is held too (destroy
// under tex_lock alone is safe: agen bump forces re-resolve before the next dereference).
typedef struct {
    struct Tex *ce;               // NULL: nothing cached (registered since, or never resolved)
    uint32_t cgen;                // agen of the slot's address when ce was resolved
    uint32_t cepoch;
    view_t cview;
    uint8_t chas_view, used, has_view;
    view_t v;
    tex_t t;
} TexSlot;
typedef struct { samp_par p; VkSampler s, sp; uint8_t used; } SampSlot;   // s: vk_sampler_for(&p) once looked up; sp:
                                                                           // its point-sampled form (vk_sampler_point_by_id_locked)

// vk_texture.c: image-cache state under tex_lock; slot-table state under slots_lock (see below).
// Lock order when more than one is held: tex_lock → slots_lock → view_lock (vk_exec_view.c) — never reverse.
// slots_lock alone for register / sampler-slot / token-facing table work; when both tex and slots are needed
// (by_id_locked: resolve + freshness), take tex_lock first then slots_lock (vk_texture_lock does that).
#define AGEN_N 4096
extern PlatRMutex tex_lock;     // by_key / by_addr / Tex lifetime / uploads / release / evict / tex_gen / agen / submit_epoch
extern PlatMutex slots_lock;    // tslots / sslots / slots_gen / samp_gen / reg_serial / sinfo / schk / samplers hashmap
extern PlatOnce tex_once;
extern volatile int32_t tex_ready;
extern nvn_hmap by_key, by_addr, samplers;
extern TexSlot *tslots;
extern int ntslots;
extern SampSlot *sslots;
extern int nsslots, nreleased;
extern uint64_t released_bytes;   // own memory of the released entries (vk_tex_evict.c sweep_released)
extern Tex *rel_head, *rel_tail;
extern uint64_t n_uploads, n_reuploads, ru_n, ru_bytes, ru_ns, ru_wt_n, ru_wt_bytes, ru_wt_ns;
extern int tex_alias;
extern uint64_t n_alias_copied, n_alias_upload, n_alias_uninit;
extern uint32_t submit_epoch, tex_gen, slots_gen, samp_gen, reg_serial;
extern PlatMutex owned_lock;
extern Tex **owned;
extern int nowned, cowned;
extern uint64_t max_owned, gpu_writes, n_checks;
extern uint32_t agen[AGEN_N];
extern uint64_t *sinfo, *schk;
void tex_init(void *p);
uint32_t agen_idx(uint64_t addr);
void tex_gen_bump(uint64_t addr);
void make_key(const tex_t *t, TexKey *k);
int same_but_swizzle(const TexKey *a, const TexKey *b);
int tex_mirrored(const tex_t *t);
void log_limited(volatile int32_t *n, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void barrier(VkCommandBuffer cb, VkPipelineStageFlags2 src, VkAccessFlags2 sa, VkPipelineStageFlags2 dst,
             VkAccessFlags2 da);
#define ALL_RW VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT

static inline void lock(void) {
    if (!__atomic_load_n(&tex_ready, __ATOMIC_ACQUIRE)) plat_once(&tex_once, tex_init, NULL);
    plat_rmutex_lock(&tex_lock);
}
static inline void unlock(void) { plat_rmutex_unlock(&tex_lock); }

static inline void lock_slots(void) {
    if (!__atomic_load_n(&tex_ready, __ATOMIC_ACQUIRE)) plat_once(&tex_once, tex_init, NULL);
    plat_mutex_lock(&slots_lock);
}
static inline void unlock_slots(void) { plat_mutex_unlock(&slots_lock); }

static inline uint32_t umax(uint32_t a, uint32_t b) { return a > b ? a : b; }
static inline uint32_t mip(uint32_t v, unsigned lv) { return umax(v >> lv, 1); }

// vk_tex_prefetch.c: set on its thread, which makes and uploads entries no one else sees yet: create_tex gives up
// instead of freeing memory (released and idle entries are tex_lock's), upload stages without the upload lock and
// sets tex_pf_failed when it cannot
extern _Thread_local int tex_pf_thread, tex_pf_failed;
void tex_pf_first_use(Tex *e, uint32_t uploads_before);   // a draw's first lookup of a prefetched entry (counted)

// MKW_TEX_TIMING=1 (diagnosis): where a frame's texture creation and upload time goes, one line per frame past 5 ms
enum { TT_CREATE, TT_ALPHA, TT_IMAGE, TT_UPLOAD, TT_STAGE, TT_DESW, TT_ASTC, TT_FLUSH, TT_N };
extern int64_t tex_tt[TT_N];
extern int tex_tt_on;
#define TT_LEADS 6
extern int64_t tex_lead_n[TT_LEADS], tex_lead_bytes[TT_LEADS];
extern uint64_t *tex_reg_ns;   // per slot: when the game registered it (MKW_TEX_TIMING)
static inline uint64_t tt_begin(void) { return tex_tt_on && !tex_pf_thread ? plat_now_ns() : 0; }   // the replay's only
static inline void tt_end(int k, uint64_t t0) {
    if (t0) __atomic_add_fetch(&tex_tt[k], (int64_t)(plat_now_ns() - t0), __ATOMIC_RELAXED);
}


// vk_tex_image.c: images
Tex *create_tex(const tex_t *t, const TexKey *k, int defer_init);
void init_image(Tex *e, VkCommandBuffer cb);
VkResult vma_image(VkCtx *c, const VkImageCreateInfo *ci, const VmaAllocationCreateInfo *ai, VkImage *img,
                   VmaAllocation *alloc);   // in the texture pool when it fits

// vk_tex_evict.c: released entries, eviction and the owned list
void destroy_tex(Tex *e);
void destroy_idle(Tex *e);   // one the submission being recorded does not use (released or idle): freed sooner
VkResult evict_idle(VkCtx *c, const VkImageCreateInfo *ci, const VmaAllocationCreateInfo *ai, VkImage *img,
                    VmaAllocation *alloc);
void own_add(Tex *e);
int owned_lower(uint64_t a);
void rel_append(Tex *e);
void rel_unlink(Tex *e);
void sweep_released(void);

// vk_tex_pressure.c: idle recreatable textures out when VMA's blocks near the live VRAM budget (MKW_TEX_PRESSURE)
void tex_pressure(void);   // each submit, the texture lock held

// vk_tex_upload.c: uploads and the freshness checks
void make_fresh(Tex *e);
void make_fresh_snap(Tex *e);
void upload_again(Tex *e);
void upload_first(Tex *e);   // vk_tex_prefetch.c: a new entry's first upload, on the prefetch thread   // uploads a CPU-written entry from its storage now (its depth twin's first fill)
void expand_rgb(int format, const uint8_t *src, uint8_t *dst, size_t n);
void shrink_rgb(int format, const uint8_t *src, uint8_t *dst, size_t n);
int storage_rewritten(const Tex *e, uint32_t since, uint32_t *now);
void gpu_base_note(Tex *e);          // at a GPU write: the bytes of storage that fills no whole page
int gpu_base_kept(const Tex *e);     // those bytes unchanged since: 1; changed: 0; storage with whole pages: -1
int rewritten_since_gpu(const Tex *e);   // the CPU rewrote e's storage after its GPU write: that render is not written back
extern uint64_t trace_addr;
int tracing(uint64_t addr);

// vk_tex_twin.c: depth twins of colour textures sampled with a depth compare
void tex_twin_copy(Tex *e, VkCommandBuffer cb, const uint8_t *stg, VkDeviceSize total, const VkBufferImageCopy *rg, uint32_t n);
void tex_twin_destroy(Tex *e);

// vk_tex_alias.c: aliases (MKW_TEX_ALIAS) and GPU writes into mirrored storage
enum { WB_SMALL = 64 << 10 };
// What goes back to its storage when the CPU waits for a fence (vk_texture_write_back_small): targets up to WB_SMALL
// bytes, and pitch (LINEAR, LINEAR_RENDER_TARGET) textures of any size: that layout is asked for so that the CPU reads
// the texels in place.
static inline int wb_at_fence(const nvn_texdesc *d, uint64_t size) { return size <= WB_SMALL || nvn_tex_is_linear(d); }
extern int small_pending;
Tex *entry_of(const tex_t *t);
Tex *entries_at(uint64_t addr);   // the first entry at that storage address; the rest by next_at

// vk_tex_texel.c: texel reads and writes
typedef struct { unsigned level, layer0; int nz, is3d, bw, bh, bpb; size_t wb, hb, row, img, x0, y0; } Region;
int region_of(const tex_t *t, const view_t *v, const NVNcopyRegion *r, uint64_t row, uint64_t img, Region *g);
int image_texels(Tex *e, const Region *g, const NVNcopyRegion *r, uint8_t *mem, int write);
// A read of an image region in two steps (vk_tex_wb.c): the copy into the open upload batch, then, once that batch is
// done (vk_upload_flush(1)), its rows into mem (NULL: only the staging freed). Nothing between needs the texture lock.
typedef struct { VkBuffer sb; VmaAllocation sa; uint8_t *stg; size_t slice, ib; uint32_t w; int rgb, fmt; } TexRead;
int image_read_record(Tex *e, const Region *g, const NVNcopyRegion *r, TexRead *rd);   // 0: nothing to read
void image_read_rows(const Region *g, TexRead *rd, uint8_t *mem);
int storage_texels(const tex_t *t, const Region *g, const NVNcopyRegion *r, uint8_t *mem, int write);
