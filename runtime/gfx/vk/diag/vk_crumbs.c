// GPU breadcrumbs for a device loss (vk.h vk_crumbs_*). VK_AMD_buffer_marker has the command stream write a 32-bit
// value into host memory: at TOP_OF_PIPE once the command processor reaches it, at BOTTOM_OF_PIPE once every command
// before it has finished. Each mark writes its start at the top and its end at the bottom into two words of one
// host-coherent buffer, so after a loss one word names the newest mark the GPU started and the other the newest it
// had finished: the hang lies between them. Words 0 and 1: upload batches (vk_mem_upload.c); words 2 and 3: the
// replay's render passes and dispatches (vk_exec.c, vk_exec_ops.c). A ring of records says what each mark was.
//
// Why: issues #4 and #5 (AMD discrete GPUs, v0.1.2) logged only "upload submit failed (-4)": the queue the loss was
// noticed on, not the work that hung. VK_EXT_device_fault (vk_exec_dstat.c) names an address only for a page fault;
// these marks also cover a timeout. Cost: two small writes per pass or dispatch, none per draw; a barrier already
// precedes each pass and dispatch, so the end-of-pipe write adds no wait. MKW_GPU_CRUMBS=0 turns them off.
// Where the driver has no buffer marker but VK_NV_device_diagnostic_checkpoints (older NVIDIA drivers; current ones
// have both), the same marks are checkpoints, read from the queue after the loss.
// The design follows totk-pc-recomp's vk_crumbs.c (MIT, 710b7f5) and Savage Wind Recomp's GPU checkpoints; the code
// is ours.
#include <stdio.h>
#include <string.h>

#include "host.h"
#include "nvn_api.h"
#include "vk.h"
#include "vk_rec.h"

enum { RING = 4096 };   // records kept: a loss is noticed within a few frames of the hang
typedef struct {
    uint32_t id;
    uint8_t kind, ncolor;
    uint64_t frame;
    uint32_t w, h, d;   // pass: render area and layers; dispatch: groups (0 indirect); upload: w its batch number
    VkFormat color[8], depth;
    const char *what;   // VK_CRUMB_OP
    uint8_t key[2][32], has_fs;   // VK_CRUMB_DRAW: the program's vertex and fragment keys
    uint64_t ind, ind_count;      // VK_CRUMB_DRAW, indirect: guest addresses of the arguments and of the count (0 none)
} Mark;

static PFN_vkCmdWriteBufferMarkerAMD write_marker;
static PFN_vkCmdSetCheckpointNV set_checkpoint;   // NVIDIA: a checkpoint (id << 1 | end) per mark instead of a write
static int active;                                // marks are recorded (either way)
static VkBuffer buf;
static VmaAllocation alloc;
static volatile uint32_t *words;
static Mark ring[RING];
static uint32_t seq;   // marks made; several replay threads (MKW_REPLAY_PAR) take ids at once
static int mode;       // rt_knob: 1 on, 2 "ab" (on in the B blocks, for its A/B)
static int draws;      // MKW_GPU_CRUMBS_DRAW

void vk_crumbs_init(void) {
    VkCtx *c = vk_ctx();
    mode = rt_knob("GPU_CRUMBS", "1");
    draws = rt_knob("GPU_CRUMBS_DRAW", "0");
    if (!c || !mode) return;
    if (c->has_nv_checkpoints) {
        set_checkpoint = (PFN_vkCmdSetCheckpointNV)vkGetDeviceProcAddr(c->device, "vkCmdSetCheckpointNV");
        active = set_checkpoint != NULL;
        rt_log("vk: GPU breadcrumbs %s%s: upload batches, render passes and dispatches as NVIDIA checkpoints "
               "(MKW_GPU_CRUMBS=0 turns them off)", active ? "on" : "unavailable",
               active && mode == 2 ? " in the B blocks (MKW_AB)" : "");
        return;
    }
    if (!c->has_buffer_marker) {
        rt_log("vk: GPU breadcrumbs unavailable (no VK_AMD_buffer_marker or VK_NV_device_diagnostic_checkpoints)");
        return;
    }
    write_marker = (PFN_vkCmdWriteBufferMarkerAMD)vkGetDeviceProcAddr(c->device, "vkCmdWriteBufferMarkerAMD");
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 16,
                             .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                  .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                  .requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT};
    VmaAllocationInfo info;
    if (!write_marker || vmaCreateBuffer(c->vma, &bi, &ai, &buf, &alloc, &info) != VK_SUCCESS) {
        rt_log("vk: GPU breadcrumbs unavailable (no marker buffer)");
        write_marker = NULL;
        return;
    }
    words = info.pMappedData;
    words[0] = words[1] = words[2] = words[3] = 0;
    active = 1;
    rt_log("vk: GPU breadcrumbs on%s: upload batches, render passes, dispatches%s (MKW_GPU_CRUMBS=0 turns them off)",
           mode == 2 ? " in the B blocks (MKW_AB)" : "", draws ? " and draws (MKW_GPU_CRUMBS_DRAW)" : "");
}

void vk_crumbs_release(void) {
    VkCtx *c = vk_ctx();
    if (buf && c) vmaDestroyBuffer(c->vma, buf, alloc);
    buf = VK_NULL_HANDLE, alloc = NULL, words = NULL, write_marker = NULL, set_checkpoint = NULL, active = 0;
}

static void mark(VkCommandBuffer cb, uint32_t word, uint32_t id) {
    if (set_checkpoint) rec_checkpoint(set_checkpoint, cb, (const void *)(uintptr_t)((uint64_t)id << 1 | (word & 1)));
    else rec_marker(write_marker, cb, word & 1 ? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                    buf, 4 * word, id);
}

uint32_t vk_crumb_begin(VkCommandBuffer cb, int kind, uint32_t w, uint32_t h, uint32_t d, uint32_t ncolor,
                        const VkFormat *color, VkFormat depth) {
    if (!active || !cb || !RT_KNOB_ON(mode)) return 0;
    uint32_t id = __atomic_add_fetch(&seq, 1, __ATOMIC_RELAXED);
    if (!id) id = __atomic_add_fetch(&seq, 1, __ATOMIC_RELAXED);   // 0 means no mark
    Mark *m = &ring[id % RING];
    *m = (Mark){.id = id, .kind = (uint8_t)kind, .ncolor = (uint8_t)(ncolor > 8 ? 8 : ncolor), .w = w, .h = h, .d = d,
                .depth = depth, .frame = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED)};
    if (color) memcpy(m->color, color, sizeof *color * m->ncolor);
    mark(cb, kind == VK_CRUMB_UPLOAD ? 0 : 2, id);
    return id;
}

// Why: issue #43 (RX 6650 XT, RADV) hung after the last marked pass of a frame had finished, in commands no mark
// covered (clears, copies, blits, the present's scaling): four runs, four frames, the same place.
uint32_t vk_crumb_op(VkCommandBuffer cb, const char *what, uint32_t w, uint32_t h) {
    uint32_t id = vk_crumb_begin(cb, VK_CRUMB_OP, w, h, 0, 0, NULL, 0);
    if (id) ring[id % RING].what = what;
    return id;
}

// Why: issues #22, #33, #34, #37 and #40 (NVIDIA RTX, 617-621 drivers) lost the device with "instruction pointer
// invalid" inside the frame's main 3D pass, which holds hundreds of draws. A draw's end is marked at the next draw
// recorded into the same command buffer (or never, if the pass ends first): the last finished mark lags one draw.
static _Thread_local uint32_t prev;   // this thread's last draw mark

// The last draw mark of this thread was an indirect draw: where its arguments are and how many at most. Two hangs
// (13/11/2026, Xid 109) stopped in a run of draws of one program without loops: the suspect is the work the draw is
// given, which for an indirect draw only the argument buffer says.
void vk_crumb_draw_ind(uint64_t ind, uint64_t count, uint32_t max) {
    if (!draws || !prev) return;
    Mark *m = &ring[prev % RING];
    if (m->id == prev) m->ind = ind, m->ind_count = count, m->h = max;
}

void vk_crumb_draw(VkCommandBuffer cb, const uint8_t *vs, const uint8_t *fs, uint32_t count) {
    static _Thread_local VkCommandBuffer prev_cb;
    if (!draws) return;
    if (prev && prev_cb == cb) vk_crumb_end(cb, VK_CRUMB_DRAW, prev);
    uint32_t id = vk_crumb_begin(cb, VK_CRUMB_DRAW, 0, 0, 0, 0, NULL, 0);
    if (id) {
        Mark *m = &ring[id % RING];
        memcpy(m->key[0], vs, 32);
        if ((m->has_fs = fs != NULL)) memcpy(m->key[1], fs, 32);
        m->w = count;   // vertices; 0x7fffffff: indirect
    }
    prev = id, prev_cb = cb;
}

// The compute program of a dispatch mark: a hang inside a dispatch names its shader as a draw's does.
void vk_crumb_cs(uint32_t id, const uint8_t *cs) {
    if (id && cs) memcpy(ring[id % RING].key[0], cs, 32), ring[id % RING].has_fs = 1;
}

void vk_crumb_end(VkCommandBuffer cb, int kind, uint32_t id) {
    if (active && cb && id) mark(cb, kind == VK_CRUMB_UPLOAD ? 1 : 3, id);
}

static void describe(uint32_t id, char *o, size_t size) {
    const Mark *m = &ring[id % RING];
    if (!id) {
        snprintf(o, size, "nothing yet");
        return;
    }
    if (m->id != id) {
        snprintf(o, size, "%u (no record: a later mark took its slot)", id);
        return;
    }
    if (m->kind == VK_CRUMB_UPLOAD) {
        snprintf(o, size, "%u, upload batch %u in frame %llu", id, m->w, (unsigned long long)m->frame);
    } else if (m->kind == VK_CRUMB_DRAW) {
        char a[65], b[65] = "none";
        for (int k = 0; k < 32; k++) snprintf(a + 2 * k, 3, "%02x", m->key[0][k]);
        if (m->has_fs)
            for (int k = 0; k < 32; k++) snprintf(b + 2 * k, 3, "%02x", m->key[1][k]);
        size_t n = (size_t)snprintf(o, size, "%u, draw in frame %llu: vs %.16s fs %.16s, ", id, (unsigned long long)m->frame, a, b);
        if (n >= size) return;
        if (m->ind) snprintf(o + n, size - n, "indirect, arguments at %#llx, count at %#llx, at most %u",
                             (unsigned long long)m->ind, (unsigned long long)m->ind_count, m->h);
        else snprintf(o + n, size - n, "%u vertices%s", m->w, m->w == 0x7fffffff ? " (indirect, no note)" : "");
    } else if (m->kind == VK_CRUMB_OP) {
        snprintf(o, size, "%u, %s in frame %llu: %ux%u", id, m->what ? m->what : "?", (unsigned long long)m->frame, m->w,
                 m->h);
    } else if (m->kind == VK_CRUMB_DISPATCH) {
        char a[65] = "?";
        if (m->has_fs)
            for (int k = 0; k < 32; k++) snprintf(a + 2 * k, 3, "%02x", m->key[0][k]);
        snprintf(o, size, "%u, dispatch in frame %llu: %ux%ux%u groups%s, cs %s", id, (unsigned long long)m->frame, m->w,
                 m->h, m->d, m->w ? "" : " (indirect)", a);
    } else {
        size_t n = (size_t)snprintf(o, size, "%u, render pass in frame %llu: %ux%u, %u layers, colour formats", id,
                                    (unsigned long long)m->frame, m->w, m->h, m->d);
        for (int i = 0; i < m->ncolor && n < size; i++) n += (size_t)snprintf(o + n, size - n, "%s%d", i ? "," : " ", m->color[i]);
        if (!m->ncolor && n < size) n += (size_t)snprintf(o + n, size - n, " none");
        if (n < size) snprintf(o + n, size - n, ", depth format %d", m->depth);
    }
}

// NVIDIA: the last checkpoint each pipeline stage of the graphics queue (uploads and the replay both) executed. A
// checkpoint is a mark's id times 2, plus 1 at its end: the top-of-pipe one is the newest point the GPU reached, the
// bottom-of-pipe one the newest before which everything had finished.
static void nv_report(void) {
    VkCtx *c = vk_ctx();
    PFN_vkGetQueueCheckpointDataNV get =
        c ? (PFN_vkGetQueueCheckpointDataNV)vkGetDeviceProcAddr(c->device, "vkGetQueueCheckpointDataNV") : NULL;
    if (!get) return;
    VkCheckpointDataNV d[32];
    uint32_t n = 0;
    get(c->gfx_queue, &n, NULL);
    if (n > 32) n = 32;
    for (uint32_t i = 0; i < n; i++) d[i] = (VkCheckpointDataNV){.sType = VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV};
    get(c->gfx_queue, &n, d);
    rt_log("vk: GPU breadcrumbs (%u marks made, NVIDIA checkpoints): %u stages reported", __atomic_load_n(&seq, __ATOMIC_RELAXED),
           n);
    for (uint32_t i = 0; i < n; i++) {
        uint64_t v = (uint64_t)(uintptr_t)d[i].pCheckpointMarker;
        char a[256];
        describe((uint32_t)(v >> 1), a, sizeof a);
        rt_log("vk:   stage %#x last ran the %s of mark %s", (unsigned)d[i].stage, v & 1 ? "end" : "start", a);
    }
}

static void marker_report(void);

void vk_crumbs_report(void) {
    static int done;
    if (!active || __atomic_exchange_n(&done, 1, __ATOMIC_ACQ_REL)) return;
    if (set_checkpoint) nv_report();
    else marker_report();
}

// A hang without a device loss (vk_sync.c's completion thread): the marker words are host memory and can be read at
// any time; NVIDIA's checkpoints only once the device is lost (vkGetQueueCheckpointDataNV), so they are left for then.
// Does not use up vk_crumbs_report: a loss reported later still gets its own.
void vk_crumbs_report_hang(void) {
    if (!active) return;
    if (set_checkpoint) rt_log("vk: GPU breadcrumbs are NVIDIA checkpoints here: readable only after a device loss");
    else marker_report();
}

// Both rings' newest started mark had also finished, and some mark was made: no marked work was running.
static int crumbs_idle(const volatile uint32_t *w) { return w[0] == w[1] && w[2] == w[3] && (w[0] || w[2]); }

static void marker_report(void) {
    static const char *const what[2] = {"upload batches", "replay passes and dispatches"};
    rt_log("vk: GPU breadcrumbs (%u marks made): the hang lies after the last finished mark and up to the last started",
           __atomic_load_n(&seq, __ATOMIC_RELAXED));
    for (int k = 0; k < 2; k++) {
        char a[256], b[256];
        describe(words[2 * k], a, sizeof a);
        describe(words[2 * k + 1], b, sizeof b);
        rt_log("vk:   %s: last started %s", what[k], a);
        rt_log("vk:   %s: last finished %s", what[k], b);
        // every mark between them: the first not finished is the suspect (13/11/2026: 79 marks apart, only the two
        // ends were named)
        uint32_t fin = words[2 * k + 1], st = words[2 * k];
        for (uint32_t id = fin + 1; fin && id < st && id - fin <= 200; id++) {
            describe(id, a, sizeof a);
            rt_log("vk:     between: %s", a);
        }
    }
    // Why: issue #557 (GTX 1080) lost the device with both rings at started == finished, so the two lines above named
    // finished work as if it were the hang. Queue work without marks is left: sparse binds, the prologue, the present.
    if (!crumbs_idle(words)) return;
    rt_log("vk:   every marked upload batch, pass and dispatch had finished: the hang is in queue work without marks "
           "(sparse binds, prologues, presents)");
    vk_sparse_last_bind_log();
}
