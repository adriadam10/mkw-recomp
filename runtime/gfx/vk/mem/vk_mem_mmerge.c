#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "mirror_base_spv.h"
#include "mirror_merge_spv.h"
#include "vk.h"
#include "vk_exec.h"
#include "vk_mem_internal.h"
#include "platform/platform.h"

// ---------------------------------------------------------------- the mirror's write-back merged
// MKW_MIRROR_MERGE (default 1; 0 the whole-range copy; "ab"). What a submission's GPU work writes into a VRAM mirror
// (vk_mem_mirror.c) goes back to the pool at the submission's end, and a range bound writable is the whole binding, not
// the bytes the shaders store. The CPU keeps writing other bytes of such a range while the GPU runs (each frame's
// indirect draw arguments around the instance counts a culling dispatch adds to, vk_mem_merge.c, lf1 06/11/2026), so
// the copy of the whole range put the mirror's older bytes over them: the game read back, and the next sync uploaded,
// values it had already replaced. The shadow had that fixed by bytes (MKW_SHADOW_MERGE); a pool without a shadow, which
// is every pool in mirror mode on Windows (MKW_SHADOW off there), got the copy: draws with stale arguments, the
// suspected cause of the stretched triangles of #731 #679 #694 #646 (NVIDIA and AMD, all in mirror mode; #279 and
// #595: gone with memory = import, where the GPU writes the pool itself). Shadowed pools keep the copy (merged() in
// vk_mem_mirror.c; MKW_MIRROR_MERGE_ALL=1 merges them too).
// Each range the GPU writes gets a base, a copy of its mirror words taken in the prologue of the part that first writes
// it (after that part's uploads, before any of its commands: runtime/shaders/mem/mirror_base.comp), and at the
// submission's end a compute pass (mirror_merge.comp) writes into the pool only the words where the mirror differs from
// the base (by bytes at a range's edges). Each is one dispatch over every range, the pools bound as arrays of one
// descriptor set: a dispatch and a copy per pool (~120 a frame in a race) cost +1.0 ms of GPU and +0.8 ms of replay a
// frame (wab2, in-run A/B, RX 6700 XT, 20/11/2026). Over PCIe go only the words the GPU changed (0.004-0.14 of 0.3-16
// MiB a frame, wm1 and the tt8/gp397/bridge58 replays), where the copy sent the whole range; GPU time of the write-back
// (MKW_MIRROR_MERGE_TIME): replays 0.28-0.33 ms against the copy's 0.55-0.64, a 1.8.0 race 0.63-0.80 against 0.43-0.45;
// the frame unchanged (in-run A/B and its inverted twin, +0.12 ms mean, -0.03 median, wab5/wab5i; all with every pool
// merged). As with the copy, the GPU writes stamp no page, so the next sync does not upload them again.
// The ranges of one submission are kept disjoint per pool (a new range takes a base only where no earlier one covers
// it: that base is older, taken before the GPU wrote those bytes), so no two jobs write the same byte, and no base copy
// reads bytes an earlier part of the submission wrote: a base-only prologue needs no barrier before its copies.
// MKW_MIRROR_MERGE_CENSUS=1 (diagnosis): the jobs also count the bytes the GPU changed and the bytes the CPU wrote
// during the submission that the GPU left alone, which the whole-range copy reverted (each job reads the pool: PCIe).

#define MAXP 512                   // pools a submission's merges bind (mirror_*.comp MAXP)
#define MAXC 32                    // base chunks (MAXC)
#define CHUNK (8ull << 20)         // base chunk (device-local)
#define TCHUNK (256ull << 10)      // table chunk: host-visible, written by the CPU, and its device-local copy
#define SPARE 8                    // chunks of each kind and descriptor pools kept for reuse
#define DSETS 16                   // sets per descriptor pool
#define CENSUS_SLOTS 64            // census counters by submission, a ring
#define CENSUS_STRIDE 256          // bytes between slots: a flush of one never covers another (nonCoherentAtomSize)

typedef struct { VkBuffer buf; VmaAllocation alloc; uint8_t *map; uint64_t size, used; } MChunk;
typedef struct { MChunk *c; int n, cap; } MArena;
typedef struct { uint32_t src, dst, w0, pc; } MCopy;           // mirror_base.comp's Copy (w0 filled when recorded)
typedef struct { uint32_t lo, hi, boff, w0, pc; } MEntry;      // mirror_merge.comp's Job
// The tables go to VRAM before each dispatch: read from host memory, which AMD's GPU does not cache, every invocation's
// binary search crossed PCIe (wab3: +0.5 ms of GPU a frame).
// A submission's merge state, given back at its fence: base and table chunks, descriptor pools, the pools it binds
// (index PoolRec.mm_idx) and the base copies for the next prologue. It outlives the merges, recorded at the end of the
// last part: that part's prologue is made after its recording (vk_sync.c) and takes the last bases.
typedef struct {
    MArena base, table, dtable;   // kinds 0, 1, 2
    VkDescriptorPool *dp;
    int ndp, cdp;
    PoolRec **pools;
    int np, cpools;
    MCopy *bp;
    uint32_t *bwords;
    int nbp, cbp, cbw;
    uint32_t id;
    uint64_t fence;
} MSub;
// One range of a pool written this submission: [lo, hi) from import_lo; its base, from lo & ~3, at boff of base chunk
// ci (-1: none, the range is copied whole).
typedef struct { PoolRec *r; uint64_t lo, hi, boff; int ci, next; } MJob;

int mmerge_mode = -1;              // MKW_MIRROR_MERGE (rt_knob)
int mmerge_all;                    // MKW_MIRROR_MERGE_ALL=1: shadowed pools too (vk_mem_mirror.c merged)
static int ready, census_on, align_ = 4;
static uint64_t max_range;
static VkDescriptorSetLayout dsl;
static VkPipelineLayout layout;
static VkPipeline pipe_merge, pipe_base;
static VkBuffer cbuf;              // census counters: CENSUS_SLOTS slots of 2 uint32, host-visible
static VmaAllocation calloc_;
static uint8_t *cmap;

static PlatMutex mm_lock = PLAT_MUTEX_INIT;   // after use_lock: everything below
static MSub *cur;                             // the submission being recorded (made at its first note)
static MSub *ended;                           // merged, its last bases not yet in a prologue
static volatile int32_t npending;             // base copies of cur and ended, read without the lock
static uint32_t sub_id = 1;
static MJob *jobs;
static int njobs, cjobs, *order, corder;
static MChunk spare[3][SPARE];                // by kind: base, table, device table
static int nspare[3];
static VkDescriptorPool dspare[SPARE];
static int ndspare;
static VkDescriptorBufferInfo *dinfo;
static int cdinfo;
static VkBufferCopy *regions;
static int cregions;
// since the last summary
static uint64_t s_jobs, s_bytes, s_base_bytes, s_fallback, s_dispatches, s_notes;
static volatile int64_t s_changed, s_kept;     // census, added at the fences
static void tq_log(char *out, size_t n, double f);

static void chunk_free(VkCtx *c, MChunk *k) { vmaDestroyBuffer(c->vma, k->buf, k->alloc); }

static MArena *arena_of(MSub *s, int kind) { return kind == 0 ? &s->base : kind == 1 ? &s->table : &s->dtable; }

static void arena_back(VkCtx *c, MArena *a, int kind, int reuse) {   // mm_lock held
    uint64_t full = kind ? TCHUNK : CHUNK;
    for (int i = 0; i < a->n; i++) {
        if (reuse && a->c[i].size == full && nspare[kind] < SPARE) a->c[i].used = 0, spare[kind][nspare[kind]++] = a->c[i];
        else chunk_free(c, &a->c[i]);
    }
    free(a->c);
}

static void sub_free(VkCtx *c, MSub *s, int reuse) {   // mm_lock held
    for (int k = 0; k < 3; k++) arena_back(c, arena_of(s, k), k, reuse);
    for (int i = 0; i < s->ndp; i++) {
        if (reuse && ndspare < SPARE && vkResetDescriptorPool(c->device, s->dp[i], 0) == VK_SUCCESS) dspare[ndspare++] = s->dp[i];
        else vkDestroyDescriptorPool(c->device, s->dp[i], NULL);
    }
    free(s->dp), free(s->pools), free(s->bp), free(s->bwords), free(s);
}

static void give_back(void *arg) {   // completion thread: the submission's merges ran
    plat_mutex_lock(&mm_lock);
    sub_free(vk_ctx(), arg, 1);
    plat_mutex_unlock(&mm_lock);
}

// mm_lock held: size bytes aligned to align of s's arena of that kind (0 base, 1 table, 2 device table); the chunk's
// index, -1 when none could be made.
static int arena_alloc(MSub *s, int kind, uint64_t size, uint64_t align, uint64_t *off) {
    VkCtx *c = vk_ctx();
    MArena *a = arena_of(s, kind);
    uint64_t full = kind ? TCHUNK : CHUNK;
    MChunk *k = a->n ? &a->c[a->n - 1] : NULL;
    uint64_t at = k ? (k->used + align - 1) & ~(align - 1) : 0;
    if (!k || at + size > k->size) {
        if (!kind && a->n == MAXC) return -1;
        if (a->n == a->cap) vk_mem_grow((void **)&a->c, &a->cap, sizeof *a->c);
        k = &a->c[a->n];
        if (size <= full && nspare[kind]) *k = spare[kind][--nspare[kind]];
        else {
            uint64_t n = size > full ? size : full;
            int host = kind == 1;
            VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = n,
                                     .usage = host ? VK_BUFFER_USAGE_TRANSFER_SRC_BIT
                                                   : VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
            VmaAllocationCreateInfo ai = {.usage = host ? VMA_MEMORY_USAGE_AUTO_PREFER_HOST : VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
                                          .flags = host ? VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                                              VMA_ALLOCATION_CREATE_MAPPED_BIT : 0};
            VmaAllocationInfo info;
            if (vmaCreateBuffer(c->vma, &bi, &ai, &k->buf, &k->alloc, &info) != VK_SUCCESS) return -1;
            k->map = host ? info.pMappedData : NULL;
            k->size = n, k->used = 0;
        }
        a->n++;
        at = 0;
    }
    *off = at;
    k->used = at + size;
    return (int)(k - a->c);
}

// mm_lock held: a descriptor set of s's pools; VK_NULL_HANDLE when none could be made.
static VkDescriptorSet set_alloc(MSub *s) {
    VkCtx *c = vk_ctx();
    for (int tries = 0; tries < 2; tries++) {
        if (s->ndp && !tries) {
            VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                              .descriptorPool = s->dp[s->ndp - 1], .descriptorSetCount = 1,
                                              .pSetLayouts = &dsl};
            VkDescriptorSet ds;
            if (vkAllocateDescriptorSets(c->device, &ai, &ds) == VK_SUCCESS) return ds;
            continue;
        }
        VkDescriptorPool dp = VK_NULL_HANDLE;
        if (ndspare) dp = dspare[--ndspare];
        else {
            VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, DSETS * (3 * MAXP + MAXC + 2)};
            VkDescriptorPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = DSETS,
                                             .poolSizeCount = 1, .pPoolSizes = &ps};
            if (vkCreateDescriptorPool(c->device, &pi, NULL, &dp) != VK_SUCCESS) return VK_NULL_HANDLE;
        }
        if (s->ndp == s->cdp) vk_mem_grow((void **)&s->dp, &s->cdp, sizeof *s->dp);
        s->dp[s->ndp++] = dp;
        VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dp,
                                          .descriptorSetCount = 1, .pSetLayouts = &dsl};
        VkDescriptorSet ds;
        if (vkAllocateDescriptorSets(c->device, &ai, &ds) == VK_SUCCESS) return ds;
    }
    return VK_NULL_HANDLE;
}

// mm_lock held: writes s's pools (mirrors; with dst their storage too), its base chunks, the census slot (merge) and
// the table range into ds.
static void set_write(MSub *s, VkDescriptorSet ds, int dst, uint32_t slot, VkBuffer tb, uint64_t toff, uint64_t tsize) {
    int n = s->np * (dst ? 3 : 1) + s->base.n + 2;
    while (cdinfo < n) vk_mem_grow((void **)&dinfo, &cdinfo, sizeof *dinfo);
    VkWriteDescriptorSet w[6];
    int nw = 0, k = 0;
    for (int b = 0; b < 4; b++) {
        if (b >= 2 && !dst) break;
        int cnt = b == 1 ? s->base.n : s->np;
        if (!cnt) continue;
        w[nw++] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds,
                                         .dstBinding = (uint32_t)b, .descriptorCount = (uint32_t)cnt,
                                         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = dinfo + k};
        for (int i = 0; i < cnt; i++) {
            if (b == 1) dinfo[k++] = (VkDescriptorBufferInfo){s->base.c[i].buf, 0, s->base.c[i].size};
            else {
                const PoolRec *r = s->pools[i];
                dinfo[k++] = (VkDescriptorBufferInfo){b ? r->buf : r->mbuf, 0, r->import_hi - r->import_lo};
            }
        }
    }
    if (dst) {
        dinfo[k] = (VkDescriptorBufferInfo){cbuf, (uint64_t)slot * CENSUS_STRIDE, 8};
        w[nw++] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 4,
                                         .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                         .pBufferInfo = dinfo + k++};
    }
    dinfo[k] = (VkDescriptorBufferInfo){tb, toff, tsize};
    w[nw++] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 5,
                                     .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                     .pBufferInfo = dinfo + k++};
    vkUpdateDescriptorSets(vk_ctx()->device, (uint32_t)nw, w, 0, NULL);
}

static VkPipeline make_pipeline(VkCtx *c, const uint32_t *code, size_t size) {
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = size, .pCode = code};
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(c->device, &mi, NULL, &m) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"},
                                      .layout = layout};
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &ci, NULL, &p) != VK_SUCCESS) p = VK_NULL_HANDLE;
    vkDestroyShaderModule(c->device, m, NULL);
    return p;
}

int mmerge_ready(void) { return ready && RT_KNOB_ON(mmerge_mode); }

void mmerge_init(VkCtx *c) {
    mmerge_mode = rt_knob("MIRROR_MERGE", "1");
    if (!mmerge_mode) return rt_log("mem: mirror write-backs copy whole ranges (MKW_MIRROR_MERGE=0)");
    const char *why = NULL;
    const VkPhysicalDeviceLimits *l = &c->props.limits;
    if (!c->has_8bit_storage) why = "no storageBuffer8BitAccess";
    else if (!c->has_ssbo_nonuniform) why = "no non-uniform storage buffer indexing or partially bound descriptors";
    else if (l->maxPerStageDescriptorStorageBuffers < 3 * MAXP + MAXC + 2 ||
             l->maxDescriptorSetStorageBuffers < 3 * MAXP + MAXC + 2)
        why = "too few storage buffer descriptors per set";
    VkDescriptorSetLayoutBinding b[6];
    VkDescriptorBindingFlags bf[6];
    for (int i = 0; i < 6; i++) {
        uint32_t n = i == 1 ? MAXC : i < 4 ? MAXP : 1;
        b[i] = (VkDescriptorSetLayoutBinding){.binding = (uint32_t)i, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                              .descriptorCount = n, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
        bf[i] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;   // a set binds the submission's pools and chunks only
    }
    VkDescriptorSetLayoutBindingFlagsCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
                                                      .bindingCount = 6, .pBindingFlags = bf};
    VkDescriptorSetLayoutCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .pNext = &fi,
                                           .bindingCount = 6, .pBindings = b};
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 12};
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                     .pSetLayouts = &dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    if (!why && (vkCreateDescriptorSetLayout(c->device, &dci, NULL, &dsl) != VK_SUCCESS ||
                 vkCreatePipelineLayout(c->device, &li, NULL, &layout) != VK_SUCCESS))
        why = "its descriptor set layout could not be made";
    if (!why && (!(pipe_merge = make_pipeline(c, mirror_merge_spv, sizeof mirror_merge_spv)) ||
                 !(pipe_base = make_pipeline(c, mirror_base_spv, sizeof mirror_base_spv))))
        why = "its pipelines could not be made";
    if (!why) {
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = CENSUS_SLOTS * CENSUS_STRIDE,
                                 .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                      .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                                               VMA_ALLOCATION_CREATE_MAPPED_BIT};
        VmaAllocationInfo info;
        if (vmaCreateBuffer(c->vma, &bi, &ai, &cbuf, &calloc_, &info) != VK_SUCCESS) why = "no census buffer";
        else cmap = info.pMappedData, memset(cmap, 0, (size_t)bi.size), vmaFlushAllocation(c->vma, calloc_, 0, VK_WHOLE_SIZE);
    }
    if (why) return rt_log("mem: mirror write-backs copy whole ranges: the merge by bytes is off (%s)", why);
    align_ = l->minStorageBufferOffsetAlignment > 4 ? (int)l->minStorageBufferOffsetAlignment : 4;
    max_range = l->maxStorageBufferRange;
    const char *e = cfg_get("MKW_MIRROR_MERGE_CENSUS");
    census_on = e && *e && strcmp(e, "0") != 0;
    e = cfg_get("MKW_MIRROR_MERGE_ALL");
    mmerge_all = e && *e && strcmp(e, "0") != 0;
    ready = 1;
    rt_log("mem: mirror write-backs of %s merged on the GPU: only what the GPU changed reaches the pool "
           "(MKW_MIRROR_MERGE%s)%s", mmerge_all ? "every pool (MKW_MIRROR_MERGE_ALL)" : "pools without a shadow",
           mmerge_mode == 2 ? "=ab" : "", census_on ? ", with the census" : "");
}

void mmerge_shutdown(VkCtx *c) {
    plat_mutex_lock(&mm_lock);
    if (cur) sub_free(c, cur, 0), cur = NULL;
    if (ended) sub_free(c, ended, 0), ended = NULL;
    for (int k = 0; k < 3; k++) {
        for (int i = 0; i < nspare[k]; i++) chunk_free(c, &spare[k][i]);
        nspare[k] = 0;
    }
    for (int i = 0; i < ndspare; i++) vkDestroyDescriptorPool(c->device, dspare[i], NULL);
    ndspare = njobs = 0;
    npending = 0;
    if (ready) {
        vkDestroyPipeline(c->device, pipe_merge, NULL);
        vkDestroyPipeline(c->device, pipe_base, NULL);
        vkDestroyPipelineLayout(c->device, layout, NULL);
        vkDestroyDescriptorSetLayout(c->device, dsl, NULL);
        vmaDestroyBuffer(c->vma, cbuf, calloc_);
        ready = 0;
    }
    plat_mutex_unlock(&mm_lock);
}

// mm_lock held: [lo, hi) of r as a new job of cur, with its base unless its pool cannot be bound (too large, or past
// MAXP pools) or no base memory is left: then it is copied whole.
static void job_add(PoolRec *r, uint64_t lo, uint64_t hi) {
    uint64_t alo = lo & ~3ull, hi4 = (hi + 3) & ~3ull, boff = 0;
    if (r->mm_sub != cur->id) {   // r's first job in the submission: its place in the sets' arrays
        r->mm_sub = cur->id, r->mm_head = -1, r->mm_idx = -1;
        if (r->import_hi - r->import_lo <= max_range && cur->np < MAXP) {
            if (cur->np == cur->cpools) vk_mem_grow((void **)&cur->pools, &cur->cpools, sizeof *cur->pools);
            r->mm_idx = cur->np, cur->pools[cur->np++] = r;
        }
    }
    int ci = r->mm_idx >= 0 ? arena_alloc(cur, 0, hi4 - alo, 4, &boff) : -1;
    if (ci < 0) s_fallback++;
    if (njobs == cjobs) vk_mem_grow((void **)&jobs, &cjobs, sizeof *jobs);
    int j = njobs++;
    jobs[j] = (MJob){r, lo, hi, boff, ci, r->mm_head};
    r->mm_head = r->mm_hit = j;
    if (ci < 0) return;
    if (cur->nbp == cur->cbp) vk_mem_grow((void **)&cur->bp, &cur->cbp, sizeof *cur->bp);
    while (cur->cbw < cur->cbp) vk_mem_grow((void **)&cur->bwords, &cur->cbw, sizeof *cur->bwords);
    cur->bp[cur->nbp] = (MCopy){(uint32_t)(alo / 4), (uint32_t)(boff / 4), 0, (uint32_t)r->mm_idx | (uint32_t)ci << 16};
    cur->bwords[cur->nbp++] = (uint32_t)((hi4 - alo) / 4);
    __atomic_add_fetch(&npending, 1, __ATOMIC_RELEASE);
    s_base_bytes += hi - lo;
}

// use_lock held (vk_mem_gpu_wrote): the submission's GPU work writes [off, off+size) of r's mirror. The parts of it no
// earlier job of this submission covers become jobs.
void mmerge_note(PoolRec *r, uint64_t off, uint64_t size) {
    uint64_t lim = r->import_hi - r->import_lo, lo = off, hi = off + size < lim ? off + size : lim;
    if (lo >= hi) return;
    plat_mutex_lock(&mm_lock);
    s_notes++;
    if (!cur) {
        if (!(cur = calloc(1, sizeof *cur))) rt_fatal("vk: out of memory (mirror merge)");
        cur->id = sub_id = sub_id + 1 ? sub_id + 1 : 1;
    }
    int mine = r->mm_sub == cur->id;
    // Most notes repeat a range of the submission (a binding used by draw after draw): the job that covered the last.
    if (mine && jobs[r->mm_hit].lo <= lo && jobs[r->mm_hit].hi >= hi) {
        plat_mutex_unlock(&mm_lock);
        return;
    }
    // pieces of [lo, hi) left uncovered; each existing job cuts them (at most one more piece per cut)
    uint64_t pl[64], ph[64];
    int np = 1;
    pl[0] = lo, ph[0] = hi;
    for (int j = mine ? r->mm_head : -1; j >= 0 && np; j = jobs[j].next) {
        uint64_t a = jobs[j].lo, b = jobs[j].hi;
        if (a <= lo && b >= hi) r->mm_hit = j;
        for (int i = 0; i < np; i++) {
            if (b <= pl[i] || a >= ph[i]) continue;
            if (a <= pl[i] && b >= ph[i]) { pl[i] = pl[np - 1], ph[i] = ph[np - 1], np--, i--; continue; }
            if (a > pl[i] && b < ph[i]) {   // j inside the piece: two pieces (past 64, the piece stays whole and
                if (np < 64) pl[np] = b, ph[np] = ph[i], np++, ph[i] = a;   // overlaps j: both write the same bytes)
                continue;
            }
            if (a > pl[i]) ph[i] = a;
            else pl[i] = b;
        }
    }
    for (int i = 0; i < np; i++) job_add(r, pl[i], ph[i]);
    plat_mutex_unlock(&mm_lock);
}

int mmerge_pending(void) { return __atomic_load_n(&npending, __ATOMIC_ACQUIRE); }

static void barrier(VkCommandBuffer cb, VkPipelineStageFlags2 ss, VkAccessFlags2 sa, VkPipelineStageFlags2 ds,
                    VkAccessFlags2 da) {
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = ss, .srcAccessMask = sa,
                           .dstStageMask = ds, .dstAccessMask = da};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
}

// mm_lock held: room for a table of size bytes in host memory (*map) and in VRAM (*dbuf at *doff); 0 when none.
typedef struct { void *map; int ti; uint64_t toff; VkBuffer dbuf; uint64_t doff, size; } MTable;
static int table_alloc(MSub *s, uint64_t size, MTable *t) {
    int di = arena_alloc(s, 2, size, (uint64_t)align_, &t->doff);
    t->ti = di >= 0 ? arena_alloc(s, 1, size, 4, &t->toff) : -1;
    if (t->ti < 0) return 0;
    t->map = s->table.c[t->ti].map + t->toff, t->dbuf = s->dtable.c[di].buf, t->size = size;
    return 1;
}

// The table written: flushed, copied to VRAM in cb, and that copy made visible to the dispatch after it.
static void table_send(VkCommandBuffer cb, MSub *s, const MTable *t) {
    const MChunk *tc = &s->table.c[t->ti];
    vmaFlushAllocation(vk_ctx()->vma, tc->alloc, t->toff, t->size);
    VkBufferCopy r = {t->toff, t->doff, t->size};
    vkCmdCopyBuffer(cb, tc->buf, t->dbuf, 1, &r);
    barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
}

// mm_lock held: s's pending bases as one dispatch into cb; 0 when it could not be recorded (no table or set). The barrier
// after the table's copy also orders the bases after the mirror's uploads, if the prologue made any.
static int bases_record(VkCommandBuffer cb, MSub *s) {
    MTable tb;
    VkDescriptorSet ds = table_alloc(s, (uint64_t)s->nbp * sizeof(MCopy), &tb) ? set_alloc(s) : VK_NULL_HANDLE;
    if (!ds) return 0;
    MCopy *t = tb.map;
    uint32_t words = 0;
    for (int i = 0; i < s->nbp; i++) t[i] = s->bp[i], t[i].w0 = words, words += s->bwords[i];
    table_send(cb, s, &tb);
    set_write(s, ds, 0, 0, tb.dbuf, tb.doff, tb.size);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_base);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &ds, 0, NULL);
    uint32_t pc[3] = {(uint32_t)s->nbp, words, 0};
    vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
    uint32_t groups = (words + 2047) / 2048;   // blocks of 2048 words (the shaders' BLOCK), contiguous per workgroup
    vkCmdDispatch(cb, groups < 1024 ? groups : 1024, 1, 1);
    s_dispatches++;
    return 1;
}

// The prologue (vk_mem_mirror_prologue): the bases noted since the last one, one dispatch. after_uploads: the mirror's
// uploads were just recorded, so a barrier orders the bases after them and the caller's full barrier follows; else only
// the bases are in the prologue, and the part's commands, which may write the bytes they read, wait for them. The
// prologue cb is the submitting thread's own (not the recorder's ring). Returns the commands recorded.
int mmerge_prologue(VkCommandBuffer cb, int after_uploads) {
    plat_mutex_lock(&mm_lock);
    int rec = 0;
    MSub *ss[2] = {ended, cur};
    for (int k = 0; k < 2; k++) {
        MSub *s = ss[k];
        if (!s || !s->nbp) continue;
        uint64_t f = s->fence ? s->fence : vk_exec_recording_fence();   // ended: its own; cur: being recorded
        int tq = f ? mmerge_time_begin(cb) : -1;
        if (!bases_record(cb, s)) rt_fatal("vk: the mirror merge could not record its bases (MKW_MIRROR_MERGE=0 avoids it)");
        mmerge_time_end(cb, tq, f, 2);
        rec += 6;
        __atomic_sub_fetch(&npending, s->nbp, __ATOMIC_RELEASE);
        s->nbp = 0;
    }
    if (rec) {
        barrier(cb, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, after_uploads ? VK_ACCESS_2_MEMORY_READ_BIT : 0), rec++;
    }
    if (ended) vk_completion_add(ended->fence, give_back, ended), ended = NULL;   // its last bases are in
    plat_mutex_unlock(&mm_lock);
    return rec;
}

typedef struct { uint32_t slot; } CensusCb;

static void census_cb(void *arg) {   // completion thread
    CensusCb *cc = arg;
    VmaAllocator vma = vk_ctx()->vma;
    uint64_t at = (uint64_t)cc->slot * CENSUS_STRIDE;
    vmaInvalidateAllocation(vma, calloc_, at, CENSUS_STRIDE);
    volatile uint32_t *s = (volatile uint32_t *)(cmap + at);
    plat_add64(&s_changed, (int64_t)s[0]);
    plat_add64(&s_kept, (int64_t)s[1]);
    s[0] = s[1] = 0;
    vmaFlushAllocation(vma, calloc_, at, CENSUS_STRIDE);
    free(cc);
}

static int job_cmp(const void *x, const void *y) {   // pools in order, then by offset (whole copies grouped per pool)
    const MJob *a = &jobs[*(const int *)x], *b = &jobs[*(const int *)y];
    if (a->r != b->r) return (uintptr_t)a->r < (uintptr_t)b->r ? -1 : 1;
    return a->lo < b->lo ? -1 : a->lo > b->lo;
}

// The submission's end (vk_mem_mirror_end), after its last command: every job with a base merged in one dispatch, the
// others copied whole. Adds the bytes to *bytes; returns the commands recorded.
int mmerge_end(VkCommandBuffer cb, uint64_t fence, uint64_t *bytes) {
    plat_mutex_lock(&mm_lock);
    int rec = 0;
    if (ended) {   // a prologue never took its bases: the merges already ran without them (not expected)
        static int warned;
        if (!warned++) rt_log("mem: mirror merge: a submission's last bases were never recorded");
        __atomic_sub_fetch(&npending, ended->nbp, __ATOMIC_RELEASE);
        vk_completion_add(ended->fence, give_back, ended), ended = NULL;
    }
    MSub *s = cur;
    if (s && njobs) {
        uint32_t slot = s->id % CENSUS_SLOTS;
        while (corder < njobs) vk_mem_grow((void **)&order, &corder, sizeof *order);
        for (int i = 0; i < njobs; i++) order[i] = i;
        qsort(order, (size_t)njobs, sizeof *order, job_cmp);
        barrier(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT |
                    VK_ACCESS_2_TRANSFER_WRITE_BIT);
        rec++;
        int nm = 0;
        for (int i = 0; i < njobs; i++) nm += jobs[i].ci >= 0;
        MTable tb;
        VkDescriptorSet ds = nm && table_alloc(s, (uint64_t)nm * sizeof(MEntry), &tb) ? set_alloc(s) : VK_NULL_HANDLE;
        if (nm && !ds) rt_fatal("vk: the mirror merge could not record its merges (MKW_MIRROR_MERGE=0 avoids it)");
        MEntry *t = ds ? tb.map : NULL;
        uint32_t words = 0;
        int ne = 0;
        for (int i = 0; i < njobs;) {
            const MJob *j0 = &jobs[order[i]];
            int k = 0;
            for (; i < njobs && jobs[order[i]].r == j0->r; i++) {
                const MJob *j = &jobs[order[i]];
                s_jobs++, s_bytes += j->hi - j->lo, *bytes += j->hi - j->lo;
                if (j->ci >= 0) {
                    uint64_t alo = j->lo & ~3ull, hi4 = (j->hi + 3) & ~3ull;
                    t[ne++] = (MEntry){(uint32_t)j->lo, (uint32_t)j->hi, (uint32_t)j->boff, words,
                                       (uint32_t)j->r->mm_idx | (uint32_t)j->ci << 16};
                    words += (uint32_t)((hi4 - alo) / 4);
                    continue;
                }
                if (k == cregions) vk_mem_grow((void **)&regions, &cregions, sizeof *regions);
                regions[k++] = (VkBufferCopy){j->lo, j->lo, j->hi - j->lo};
            }
            if (k) vkCmdCopyBuffer(cb, j0->r->mbuf, j0->r->buf, (uint32_t)k, regions), rec++;
        }
        if (ne) {
            table_send(cb, s, &tb);
            set_write(s, ds, 1, slot, tb.dbuf, tb.doff, tb.size);
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_merge);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &ds, 0, NULL);
            uint32_t pc[3] = {(uint32_t)ne, words, (uint32_t)census_on};
            vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
            uint32_t groups = (words + 2047) / 2048;   // as in bases_record
            vkCmdDispatch(cb, groups < 1024 ? groups : 1024, 1, 1);
            rec += 6, s_dispatches++;
            vk_exec_draw_forget();   // the compute pipeline and set bound here are not the recorder's
            if (census_on) {
                CensusCb *cc = malloc(sizeof *cc);
                if (!cc) rt_fatal("vk: out of memory (mirror merge)");
                cc->slot = slot;
                vk_completion_add(fence, census_cb, cc);
            }
        }
        barrier(cb, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_HOST_BIT | VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                VK_ACCESS_2_HOST_READ_BIT | VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
        rec++;
    }
    njobs = 0;
    if (s) {
        s->fence = fence;
        if (s->nbp) ended = s;   // the last part's prologue takes its last bases, then gives it back
        else vk_completion_add(fence, give_back, s);
        cur = NULL;
    }
    plat_mutex_unlock(&mm_lock);
    return rec;
}

// The mirror summary's part (mirror_log), per frame over f frames.
void mmerge_log(char *out, size_t n, double f) {
    if (n) out[0] = 0;
    if (!ready) return tq_log(out, n, f);
    plat_mutex_lock(&mm_lock);
    uint64_t jb = s_jobs, by = s_bytes, bb = s_base_bytes, fb = s_fallback, nd = s_dispatches, nn = s_notes;
    s_jobs = s_bytes = s_base_bytes = s_fallback = s_dispatches = s_notes = 0;
    plat_mutex_unlock(&mm_lock);
    double M = 1048576.0;
    int k = snprintf(out, n, "; merged write-back: %.1f jobs in %.1f dispatches from %.1f notes, %.3f MiB, bases %.3f MiB, "
                     "%.1f copied whole", jb / f, nd / f, nn / f, by / f / M, bb / f / M, fb / f);
    if (census_on && k > 0 && (size_t)k < n)
        k += snprintf(out + k, n - (size_t)k, ", census: GPU changed %.3f MiB, CPU bytes kept %.1f KiB (MKW_MIRROR_MERGE_CENSUS)",
                 plat_xchg64(&s_changed, 0) / f / M, plat_xchg64(&s_kept, 0) / f / 1024.0);
    tq_log(out, n, f);
}

// ---- MKW_MIRROR_MERGE_TIME=1 (diagnosis): the GPU time of the mirror's write-back at each submission's end, the
// whole-range copy or the merge, and of the merges' bases in the prologues, kept apart so an "ab" run compares them
// without the scene (the frame A/B of a race moved by +-1 ms from its blocks alone, wab4/wab4i 20/11/2026).
#define TQ_SLOTS 128
static int tq_on = -1;
static VkQueryPool tq_pool;
static uint8_t tq_busy[TQ_SLOTS];
static PlatMutex tq_lock = PLAT_MUTEX_INIT;
static volatile int64_t tq_ns[3], tq_n[3];   // 0 whole copy, 1 merge, 2 bases
typedef struct { int slot, kind; } TqCb;

static void tq_cb(void *arg) {   // completion thread: the pair's timestamps are written
    TqCb *t = arg;
    uint64_t v[2];
    VkCtx *c = vk_ctx();
    if (vkGetQueryPoolResults(c->device, tq_pool, (uint32_t)t->slot * 2, 2, sizeof v, v, sizeof v[0],
                              VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && v[1] > v[0]) {
        plat_add64(&tq_ns[t->kind], (int64_t)((double)(v[1] - v[0]) * c->props.limits.timestampPeriod));
        plat_inc64(&tq_n[t->kind]);
    }
    plat_mutex_lock(&tq_lock);
    tq_busy[t->slot] = 0;
    plat_mutex_unlock(&tq_lock);
    free(t);
}

int mmerge_time_begin(VkCommandBuffer cb) {
    if (tq_on < 0) {
        const char *e = cfg_get("MKW_MIRROR_MERGE_TIME");
        tq_on = e && *e && strcmp(e, "0");
        VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                    .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 2 * TQ_SLOTS};
        if (tq_on && vkCreateQueryPool(vk_ctx()->device, &qi, NULL, &tq_pool) != VK_SUCCESS) tq_on = 0;
        if (tq_on) rt_log("mem: MKW_MIRROR_MERGE_TIME: GPU time of the mirror write-backs and the merges' bases");
    }
    if (!tq_on) return -1;
    plat_mutex_lock(&tq_lock);
    int s = 0;
    while (s < TQ_SLOTS && tq_busy[s]) s++;
    if (s < TQ_SLOTS) tq_busy[s] = 1;
    plat_mutex_unlock(&tq_lock);
    if (s == TQ_SLOTS) return -1;
    vkCmdResetQueryPool(cb, tq_pool, (uint32_t)s * 2, 2);
    vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, tq_pool, (uint32_t)s * 2);
    return s;
}

void mmerge_time_end(VkCommandBuffer cb, int slot, uint64_t fence, int kind) {
    if (slot < 0) return;
    vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, tq_pool, (uint32_t)slot * 2 + 1);
    TqCb *t = malloc(sizeof *t);
    if (!t) rt_fatal("vk: out of memory (mirror merge)");
    *t = (TqCb){slot, kind};
    vk_completion_add(fence, tq_cb, t);
}

static void tq_log(char *out, size_t n, double f) {
    if (tq_on <= 0 || !n) return;
    size_t k = strlen(out);
    static const char *const names[3] = {"whole copy", "merge", "bases"};
    for (int i = 0; i < 3 && k < n; i++) {
        int64_t ns = plat_xchg64(&tq_ns[i], 0), c = plat_xchg64(&tq_n[i], 0);
        int w = snprintf(out + k, n - k, "%s%s %.3f ms a frame (%lld)", i ? ", " : "; GPU time: ", names[i], ns / 1e6 / f,
                         (long long)c);
        if (w > 0) k += (size_t)w;
    }
}
