// Transfers of the Vulkan op-stream replay: buffer and texture copies, texture
// clears, event signals, counter reports and conditional rendering. vk_exec.c calls vk_exec_op_xfer for these ops.
//
// Buffers are guest memory: vk_mem_resolve maps an address to the VkBuffer behind its pool, textures are the vk_texture.c images
// (linear texel layout, GENERAL layout; block-linear storage is only converted when a mirrored texture is uploaded). So a
// copy is one Vulkan transfer command and the pitch of a buffer side comes from the payload (OP_COPY_STRIDE or the
// tight row). Every copy ends the open pass and starts with a full barrier. Destinations: an image is marked
// GPU-written at record time; a CPU-visible buffer range is reported to vk_gpu_written once the GPU is done (write
// watch does not see GPU writes, textures stored there re-upload).
//
// Events and counters are CPU-visible side effects. Their values go into guest memory on the completion thread through
// vk_completion_add, in submit order, as gfx_null.c does: event WRITE / ADD, counter {0, timestamp} for every type but
// samples passed. A timestamp report puts 0 in the counter word: TotK gates its clears on two such words being unequal.
//
// Samples passed (NVN_COUNTER_TYPE_SAMPLES_PASSED) is a running count on the queue: RESET_COUNTER zeroes it, every
// draw after it adds the samples that passed the depth and stencil tests, REPORT_COUNTER writes the running value.
// TotK resets and reports it 3 to 8 times a frame in the field and its CPU reads the results back (f_70010c3298); a 0
// there culls what the query tested. The replay counts with Vulkan occlusion queries ("occlusion counts" below).
//
// Conditional rendering is a CPU-side skip, not VK_EXT_conditional_rendering: the extension compares one 32-bit word
// with zero, NVN compares the counter words at addr and addr + 16. The condition is evaluated when the op is recorded
// from guest memory; a counter reported by an earlier submission that is not done yet counts as 0 if it is a timestamp,
// as unknown if it is samples passed. An unknown count renders: the report usually sits in the command buffer being
// recorded, so its value cannot be waited for, and the word in memory still holds an older query's count. Rendering
// costs at most the work the condition would have saved (MacNativeGames nvn_mtl.m decides the same); in the field
// TotK conditions only on timestamp reports. The condition sets VkExec.render_disabled, which vk_exec.c honours for
// clears and vk_exec_draw.c for draws and dispatches. A condition that cannot be read (address outside every pool)
// renders.
// Copies and CLEAR_TEX are not conditional (as in MacNativeGames, nvn_mtl.m).
//
// The copy semantics (region and layer arithmetic, row and image strides, whole-level clears, copy / blit / resolve
// choice) follow MacNativeGames games/totk/third_party/d2r_gfx/gfx/nvn/nvn_mtl.m:10080-10368, 10618-10642, 1700-1725
// (https://github.com/ly05010419/MacNativeGames @ b75be8cfa704df3701e86130c8ced268e79d6b11, MIT, Copyright (c) 2026
// jhaizhou) and LAS Recomp runtime/nvn/vulkan/exec.c:3045-3235 (https://github.com/peiikai/LASRecomp @
// da35b83a9611d046a9a681aae668d7085954d37f, MIT, Copyright (c) 2026 Peiikai).
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "nvn_api.h"
#include "nvn_tex.h"
#include "tex.h"
#include "vk.h"
#include "vk_exec.h"
#include "platform/platform.h"

enum { W_FORMAT, W_REGION, W_BUFFER, W_OVERLAP, W_COPY, W_CLEAR, W_QUERY, W_COUNT };

static void warn_once(int key, const char *fmt, ...) {
    static volatile int32_t seen[W_COUNT];
    if (__atomic_exchange_n(&seen[key], 1, __ATOMIC_RELAXED)) return;
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    rt_log("vk: %s (logged once)", msg);
}

static uint32_t max1(uint32_t v) { return v ? v : 1; }

// ---- completion callbacks (completion thread)

typedef struct { uint64_t addr, size; } Range;

static void guest_written_cb(void *arg) {
    Range *r = arg;
    vk_gpu_written(r->addr, r->size);
    free(r);
}

// The GPU wrote [addr, addr+size) of CPU-visible memory: tell the texture cache once the fence is reached.
static void note_gpu_write(VkExec *ex, uint64_t addr, uint64_t size) {
    vk_usnap_drop(addr, size);
    if (nvn_gpu_only(addr)) return;
    wpage_note(ex, addr, size, "copy, clear or counter");
    Range *r = malloc(sizeof *r);
    if (!r) rt_fatal("vk: out of memory");
    *r = (Range){addr, size};
    vk_mem_gpu_pending(addr, size);
    vk_completion_add(ex->fence, guest_written_cb, r);
}

typedef struct { volatile uint32_t *val; uint32_t mode, value; } EventWrite;

static void event_cb(void *arg) {
    EventWrite *e = arg;
    if (e->mode == NVN_EVENT_SIGNAL_MODE_ADD) __atomic_fetch_add(e->val, e->value, __ATOMIC_RELEASE);
    else __atomic_store_n(e->val, e->value, __ATOMIC_RELEASE);
    free(e);
}

// NVNcounterData = {counter, timestamp}; timestamps are GetSystemTick ticks (RT_TICK_HZ) of the game's clock (rt_ticks:
// with the turbo, ahead of the host's).
static void write_counter(uint64_t addr, uint64_t counter) {
    uint64_t rec[2] = {counter, rt_ticks()};
    memcpy((void *)(uintptr_t)addr, rec, sizeof rec);
}

// MKW_REPORT_GPU_END (default 1; "ab"): the timestamps written at the submit move, at the fence, by the time from the
// fence's last one to the fence's completion, so the last report of a submit reads its GPU end, as the console's GPU
// writes it, and the gaps between reports stay. The game's frame object takes the larger of its CPU time and that end
// (main+0x13e174) to pick the present interval; with the submit's time it saw ~17 of 34 ms, stayed at interval 1 and
// ran the race at half speed (vel3, 10/11/2026). 0: the submit's time only.
typedef struct { uint64_t fence, last; uint32_t refs; } ReportGroup;   // the timestamp reports of one fence
typedef struct { uint64_t addr, ts; ReportGroup *g; } ReportCb;
static ReportGroup *group_cur;   // under pending_lock
static PlatMutex pending_lock;

static int gpu_end_mode(void) {
    static int mode = -1;
    if (mode < 0) {
        mode = rt_knob("REPORT_GPU_END", "1");
        rt_log("vk: timestamp reports moved to their GPU end at the fence %s (MKW_REPORT_GPU_END)",
               mode == 2 ? "in the B blocks" : mode ? "on" : "off");
    }
    return mode;
}

static void counter_cb(void *arg) {
    ReportCb *r = arg;
    if (!nvn_report_at_submit()) write_counter(r->addr, 0);   // written at the submit instead (nvn_api_cmd.c)
    else if (r->g) {
        uint64_t now = rt_ticks(), *ts = (uint64_t *)(uintptr_t)(r->addr + 8);
        plat_mutex_lock(&pending_lock);
        uint64_t last = r->g->last;
        int done = !--r->g->refs;
        if (done && group_cur == r->g) group_cur = NULL;
        plat_mutex_unlock(&pending_lock);
        // Only the value this report wrote: a later submit may have reported to the same address since.
        if (now > last && __atomic_load_n(ts, __ATOMIC_RELAXED) == r->ts && RT_KNOB_ON(gpu_end_mode()))
            __atomic_store_n(ts, r->ts + (now - last), __ATOMIC_RELAXED);
        if (done) free(r->g);
    }
    free(r);
}

// Reports queued by the replay that the GPU has not finished. The condition of RENDER_COND reads a timestamp one as 0
// and a samples-passed one as unknown.
#define PENDING 256
static struct { uint64_t addr, fence; int unknown; } pending[PENDING];
static unsigned pending_head;
static PlatMutex pending_lock = PLAT_MUTEX_INIT;   // also the report groups

// ---- occlusion counts (samples passed)
//
// After a SAMPLES_PASSED reset, the first draw of a pass begins an occlusion query (precise when the device has
// occlusionQueryPrecise); the query ends with the pass, or at the next report or reset of the counter, and the next
// draw begins another. A Vulkan query begun inside a render pass must end inside it. TotK always resets, draws the
// tested proxies, reports, so counting stops at a report and restarts at the next reset: queries cover only the draws
// between the two, not every pass of the frame. Once the game reports twice without a reset in between, counting stays
// on for good (cumulative NVN semantics; that one report misses the draws since the previous one). The queries closed since the last
// report or reset form a segment, handed to the completion thread with that report or reset: in submit order it sums
// the segment's results into the running count (a reset drops the sum and zeroes the count) and writes the count of a
// report into guest memory. Slots come from one ring; the completion thread reads and host-resets them
// (hostQueryReset, Vulkan 1.2), so the replay records no reset commands. A full ring (no slot read back yet) leaves the
// draw uncounted and the segment's count at least 1, as does a query still unavailable after its fence. Without hostQueryReset the counts stay 0.
#define OQ_N 4096
#define LOG_ONCE(...) do { static volatile char once_; if (!__atomic_exchange_n(&once_, 1, __ATOMIC_RELAXED)) rt_log(__VA_ARGS__); } while (0)
static VkQueryPool oq_pool;
static int oq_state;                     // 0 not set up, 1 counting available, -1 unavailable
static VkQueryControlFlags oq_flags;
static volatile uint8_t oq_busy[OQ_N];   // from begin until the completion thread read and reset the slot
static uint32_t oq_next;                 // replay: next ring slot
static int oq_counting;                  // replay: draws are counted (a reset since the last report, or oq_keep)
static int oq_reported;                  // replay: a samples-passed report since the last reset
static int oq_keep;                      // replay: the game reported twice without a reset: count every draw
static int oq_open = -1;                 // replay: the slot whose query is active in the open pass
static uint32_t *oq_seg;                 // replay: slots closed since the last report or reset
static uint32_t oq_nseg, oq_capseg;
static int oq_lost;                      // replay: the segment missed a draw (ring full)
static uint64_t oq_count;                // completion thread: samples since the last reset

typedef struct { uint64_t addr; int reset, lost; uint32_t n; uint32_t q[]; } OqSegment;

static int oq_ready(void) {
    if (oq_state) return oq_state > 0;
    VkCtx *v = vk_ctx();
    // vk_device.c enables every feature the device reports, so supported means enabled.
    VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f12};
    vkGetPhysicalDeviceFeatures2(v->phys, &f2);
    oq_state = -1;
    if (!f12.hostQueryReset) {
        rt_log("vk: no hostQueryReset: samples-passed counters report 0");
        return 0;
    }
    VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_OCCLUSION,
                                .queryCount = OQ_N};
    VkResult r = vkCreateQueryPool(v->device, &qi, NULL, &oq_pool);
    if (r != VK_SUCCESS) {
        rt_log("vk: occlusion query pool creation failed (%d): samples-passed counters report 0", (int)r);
        return 0;
    }
    vkResetQueryPool(v->device, oq_pool, 0, OQ_N);
    oq_flags = f2.features.occlusionQueryPrecise ? VK_QUERY_CONTROL_PRECISE_BIT : 0;
    oq_state = 1;
    return 1;
}

static void oq_close(VkExec *ex) {
    if (oq_open < 0) return;
    vkCmdEndQuery(ex->cb, oq_pool, (uint32_t)oq_open);
    ex->recorded++;
    if (oq_nseg == oq_capseg) {
        oq_capseg = oq_capseg ? oq_capseg * 2 : 64;
        oq_seg = realloc(oq_seg, sizeof *oq_seg * oq_capseg);
        if (!oq_seg) rt_fatal("vk: out of memory");
    }
    oq_seg[oq_nseg++] = (uint32_t)oq_open;
    oq_open = -1;
}

void vk_exec_oq_draw(VkExec *ex) {
    if (!oq_counting || oq_open >= 0) return;
    uint32_t q = oq_next % OQ_N;
    if (__atomic_load_n(&oq_busy[q], __ATOMIC_ACQUIRE)) {
        warn_once(W_QUERY, "occlusion query ring full: draws left uncounted");
        oq_lost = 1;
        return;
    }
    oq_busy[q] = 1;
    oq_next++;
    vkCmdBeginQuery(ex->cb, oq_pool, q, oq_flags);
    ex->recorded++;
    oq_open = (int)q;
}

void vk_exec_oq_pass_end(VkExec *ex) { oq_close(ex); }

int vk_exec_oq_busy(void) { return oq_counting || oq_open >= 0 || oq_nseg; }
int vk_exec_oq_state(void) { return (oq_counting != 0) | (oq_open >= 0) << 1; }

void vk_exec_xfer_shutdown(void) {
    if (oq_state > 0) vkDestroyQueryPool(vk_ctx()->device, oq_pool, NULL);
    oq_pool = VK_NULL_HANDLE, oq_state = 0, oq_next = 0, oq_counting = oq_reported = oq_keep = 0, oq_open = -1;
    oq_nseg = 0, oq_lost = 0, oq_count = 0;
    memset((void *)oq_busy, 0, sizeof oq_busy);
}

static void oq_cb(void *arg) {
    OqSegment *s = arg;
    VkDevice dev = vk_ctx()->device;
    uint64_t sum = 0, v[64 * 2];
    for (uint32_t i = 0; i < s->n;) {
        uint32_t j = i + 1;
        while (j < s->n && j - i < 64 && s->q[j] == s->q[j - 1] + 1) j++;
        // The fence has passed, so a query that is still unavailable will not become available: waiting would block the
        // completion thread, and with it every later callback, for good. It counts as visible (one sample).
        VkResult r = vkGetQueryPoolResults(dev, oq_pool, s->q[i], j - i, sizeof v, v, 2 * sizeof v[0],
                                           VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        if (r != VK_SUCCESS && r != VK_NOT_READY) rt_fatal("vk: occlusion query results failed (%d)", (int)r);
        vkResetQueryPool(dev, oq_pool, s->q[i], j - i);
        for (uint32_t k = i; k < j; k++) {
            if (v[2 * (k - i) + 1]) sum += v[2 * (k - i)];
            else {
                sum += 1;
                LOG_ONCE("vk: occlusion query slot %u not available after its fence, counted as visible", s->q[k]);
            }
            __atomic_store_n(&oq_busy[s->q[k]], 0, __ATOMIC_RELEASE);
        }
        i = j;
    }
    if (s->reset) oq_count = 0;
    else {
        oq_count += sum;
        if (s->lost && !oq_count) oq_count = 1;
        if (s->addr) write_counter(s->addr, oq_count);
    }
    free(s);
}

// Hands the segment to the completion thread with a report (addr; 0: no CPU-visible memory) or a reset.
static void oq_segment(VkExec *ex, uint64_t addr, int reset) {
    oq_close(ex);
    OqSegment *s = malloc(sizeof *s + sizeof *s->q * oq_nseg);
    if (!s) rt_fatal("vk: out of memory");
    s->addr = addr, s->reset = reset, s->lost = oq_lost, s->n = oq_nseg;
    memcpy(s->q, oq_seg, sizeof *s->q * oq_nseg);
    oq_nseg = 0, oq_lost = 0;
    vk_completion_add(ex->fence, oq_cb, s);
}

static void reset_counter(VkExec *ex, const p_reset_counter *p) {
    if (p->type != NVN_COUNTER_TYPE_SAMPLES_PASSED || !oq_ready()) return;
    oq_segment(ex, 0, 1);
    oq_counting = 1, oq_reported = 0;
}

// A samples-passed report: the segment goes with it (addr 0: no CPU-visible memory) and counting stops until the next
// reset, unless reports came without resets before.
static void oq_report(VkExec *ex, uint64_t addr) {
    oq_segment(ex, addr, 0);
    if (oq_reported && !oq_keep) {
        oq_keep = 1;
        rt_log("vk: samples passed reported twice without a reset: counting every draw from now on");
    }
    oq_reported = 1;
    if (!oq_keep) oq_counting = 0;
}

// MKW_REPLAY_PAR: a chunk recorded a second time (vk_exec_xfer_again) leaves its events and counter reports to the
// first recording's callbacks, already queued: signalled twice, an ADD event would count twice.
static _Thread_local int again;
void vk_exec_xfer_again(int on) { again = on; }

static void signal_event(VkExec *ex, const p_signal_event *p) {
    if (again) return;
    EventWrite *e = malloc(sizeof *e);
    if (!e) rt_fatal("vk: out of memory");
    *e = (EventWrite){p->val, p->mode, p->value};
    vk_completion_add(ex->fence, event_cb, e);
}

static void report_counter(VkExec *ex, const p_report_counter *p) {
    if (again) return;
    int samples = p->type == NVN_COUNTER_TYPE_SAMPLES_PASSED && oq_ready();
    if (nvn_gpu_only(p->addr)) {   // CPU-invisible pool: no host memory behind it
        if (samples) oq_report(ex, 0);
        return;
    }
    VkMemSpan m;
    if (!vk_mem_resolve(p->addr, sizeof(NVNcounterData), &m))
        rt_fatal("vk: counter report at %#llx is in no memory pool", (unsigned long long)p->addr);
    vk_mem_count(m.cls, VK_MUSE_GPU_WRITE, sizeof(NVNcounterData));
    if (samples) oq_report(ex, p->addr);
    plat_mutex_lock(&pending_lock);   // chunks record timestamp reports on several threads (MKW_REPLAY_PAR)
    if (!samples) {
        ReportCb *r = malloc(sizeof *r);
        if (!r) rt_fatal("vk: out of memory");
        *r = (ReportCb){p->addr, 0, NULL};
        if (nvn_report_at_submit() && gpu_end_mode()) {   // the value the submit wrote (nvn_api_cmd.c)
            r->ts = __atomic_load_n((const uint64_t *)(uintptr_t)(p->addr + 8), __ATOMIC_RELAXED);
            if (!group_cur || group_cur->fence != ex->fence) {
                group_cur = calloc(1, sizeof *group_cur);
                if (!group_cur) rt_fatal("vk: out of memory");
                group_cur->fence = ex->fence;
            }
            r->g = group_cur;
            r->g->refs++;
            if (r->ts > r->g->last) r->g->last = r->ts;
        }
        vk_completion_add(ex->fence, counter_cb, r);
    }
    pending[pending_head++ % PENDING] = (typeof(pending[0])){p->addr, ex->fence, samples};
    plat_mutex_unlock(&pending_lock);
}

// ---- conditional rendering

// 1: *v read, 0: not readable, 2: a samples-passed report not done yet (unknown)
static int counter_word(uint64_t addr, uint64_t *v) {
    uint64_t done = vk_sync_fence_done();
    for (unsigned k = 1; k <= PENDING; k++) {   // newest first: the last report to addr decides
        unsigned i = (pending_head - k) % PENDING;
        if (pending[i].addr == addr && pending[i].fence > done) return *v = 0, pending[i].unknown ? 2 : 1;
    }
    VkMemSpan m;
    if (nvn_gpu_only(addr) || !vk_mem_resolve(addr, sizeof *v, &m)) return 0;
    memcpy(v, (const void *)(uintptr_t)addr, sizeof *v);
    vk_mem_count(m.cls, VK_MUSE_CPU_READ, sizeof *v);
    return 1;
}

// MKW_COND_TRACE=<lines> (diagnosis, default 0): each condition's counter words and verdict, and the draws it skips
// (vk_exec_ops.c), with the frame. Found for the title's flickering trees (06/11/2026).
int vk_cond_trace = -1;

static int cond_eval(const p_render_cond *c, uint64_t *x, uint64_t *y, int *rx, int *ry) {
    *x = *y = 0;
    *rx = counter_word(c->addr, x), *ry = counter_word(c->addr + 16, y);
    if (!*rx || !*ry) {
        warn_once(W_BUFFER, "conditional render counters at %#llx are not readable: rendering", (unsigned long long)c->addr);
        return 1;
    }
    if (*rx == 2 || *ry == 2) return 1;   // a samples-passed count not done yet
    return c->mode == 0 ? *x == *y : *x != *y;
}

static int cond_enabled(const p_render_cond *c) {
    uint64_t x, y;
    int rx, ry, on = cond_eval(c, &x, &y, &rx, &ry);
    if (vk_cond_trace < 0) {
        const char *e = cfg_get("MKW_COND_TRACE");
        vk_cond_trace = e ? atoi(e) : 0;
    }
    if (vk_cond_trace > 0) {
        vk_cond_trace--;
        rt_log("cond: f%llu addr %#llx mode %d words %d %d x %llu y %llu -> %s", (unsigned long long)vk_exec_presented,
               (unsigned long long)c->addr, c->mode, rx, ry, (unsigned long long)x, (unsigned long long)y,
               on ? "render" : "skip");
    }
    return on;
}

// MKW_REPLAY_PAR: the conditions of a submission decided in op order by the light walk, before any chunk records. A
// report of the same submission earlier in the walk is not recorded yet, so it is taken as the serial replay would see
// it: pending (a timestamp reads 0, samples passed is unknown). The chunks then use the walk's decisions
// (vk_exec_cond_use), whatever thread recorded the reports.
int vk_exec_cond_plan(VkCondPlan *cp, uint32_t op, const void *payload) {
    if (op == OP_REPORT_COUNTER) {
        const p_report_counter *p = payload;
        VkMemSpan m;
        if (nvn_gpu_only(p->addr) || !vk_mem_resolve(p->addr, sizeof(NVNcounterData), &m)) return -1;
        if (cp->n == VK_COND_PLAN_REPORTS) memmove(cp->addr, cp->addr + 1, sizeof cp->addr[0] * --cp->n),
                                           memmove(cp->unknown, cp->unknown + 1, (size_t)cp->n);
        cp->addr[cp->n] = p->addr, cp->unknown[cp->n++] = p->type == NVN_COUNTER_TYPE_SAMPLES_PASSED;
        return -1;
    }
    if (op != OP_RENDER_COND) return -1;
    const p_render_cond *c = payload;
    uint64_t v[2] = {0, 0};
    int r[2];
    for (int k = 0; k < 2; k++) {
        uint64_t a = c->addr + 16 * (uint64_t)k;
        int i = cp->n;
        while (i-- > 0 && cp->addr[i] != a) {}
        r[k] = i >= 0 ? (cp->unknown[i] ? 2 : 1) : counter_word(a, &v[k]);
        if (i >= 0) v[k] = 0;
    }
    if (!r[0] || !r[1] || r[0] == 2 || r[1] == 2) return 1;
    return c->mode == 0 ? v[0] == v[1] : v[0] != v[1];
}

static _Thread_local const VkCondPlanned *cond_use;
static _Thread_local int cond_nuse, cond_at;

void vk_exec_cond_use(const VkCondPlanned *d, int n) { cond_use = d, cond_nuse = n, cond_at = 0; }

// The walk's decision for the condition op at payload; -1 when it has none (serial replay).
static int cond_planned(const void *payload) {
    for (; cond_use && cond_at < cond_nuse; cond_at++)
        if (cond_use[cond_at].op == payload) return cond_use[cond_at++].on;
    return -1;
}

// The render-enable ops' effect on the replay state alone, without the trace: 1 when op is one (the light walk of
// vk_exec.c, MKW_REPLAY_WALK).
int vk_exec_xfer_state(VkExec *ex, uint32_t op, const void *payload) {
    uint64_t x, y;
    int rx, ry;
    if (op == OP_RENDER_ENABLE) ex->render_disabled = !((const p_flag *)payload)->value;
    else if (op == OP_RENDER_COND) {
        ex->render_cond = *(const p_render_cond *)payload;
        ex->has_render_cond = 1;
        ex->render_disabled = !cond_eval(payload, &x, &y, &rx, &ry);
    } else return 0;
    return 1;
}

// ---- textures

typedef struct {
    const VkTexImage *img;
    VkImageAspectFlags aspect;
    uint32_t level, layer0;
    int bpb, bw, bh;
    int32_t x, y, z;           // region origin
    uint32_t w, h, nz;         // region extent; nz: slices (3D) or layers
    int astc_bw, astc_bh;      // ASTC decoded on upload (decoded_ok): its block, 0 otherwise
} Sub;

// Resolves tex, its view and a copy region to image coordinates. 0 (warned) when the copy cannot be replayed.
// decoded_ok: an ASTC texture decoded on upload is taken too (texture to texture: both sides must be decoded alike).
static int sub_resolve(const tex_t *t, const view_t *v, int has_v, const NVNcopyRegion *r, int defaults_to_level,
                       Sub *s, int decoded_ok) {
    memset(s, 0, sizeof *s);
    if (!t) return 0;
    TexVkFormat f;
    int decoded = tex_vk_format(t->d.format, &f) && f.astc_bw && decoded_ok;
    if (!decoded && (!tex_vk_format(t->d.format, &f) || f.vk != f.ideal || f.expand_rgb || f.astc_bw)) {
        warn_once(W_FORMAT, "copy of format %#x texture (decoded or expanded on upload): skipped", t->d.format);
        return 0;
    }
    s->bpb = nvn_fmt_bpb(t->d.format, &s->bw, &s->bh);
    if (decoded) s->astc_bw = f.astc_bw, s->astc_bh = f.astc_bh;
    s->img = vk_texture_get(t);
    if (!s->img || !s->bpb) {
        warn_once(W_FORMAT, "copy of format %#x texture without an image: skipped", t->d.format);
        return 0;
    }
    const VkTexImage *img = s->img;
    if (img->aspect & VK_IMAGE_ASPECT_COLOR_BIT) s->aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    else if (img->aspect == VK_IMAGE_ASPECT_DEPTH_BIT) s->aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    else {
        warn_once(W_FORMAT, "copy of depth-stencil format %#x texture: skipped", t->d.format);
        return 0;
    }
    s->level = has_v && v->set_levels ? v->base_level : 0;
    s->layer0 = has_v && v->set_layers ? v->min_layer : 0;
    if (img->drop) {   // vk_tex_image.c tex_drop: the level is not in the image
        if (s->level < img->drop) return 0;
        s->level -= img->drop;
    }
    uint32_t lw = max1(img->width >> s->level), lh = max1(img->height >> s->level);
    s->x = r->xoffset, s->y = r->yoffset, s->z = r->zoffset;
    s->nz = r->depth > 0 ? (uint32_t)r->depth : 1;
    s->w = r->width > 0 ? (uint32_t)r->width : defaults_to_level ? (s->x < (int32_t)lw ? lw - (uint32_t)s->x : 0) : 0;
    s->h = r->height > 0 ? (uint32_t)r->height : defaults_to_level ? (s->y < (int32_t)lh ? lh - (uint32_t)s->y : 0) : 0;
    int is3d = img->type == VK_IMAGE_TYPE_3D;
    uint32_t layers = is3d ? 1 : (uint32_t)s->z + s->nz, first = is3d ? 0 : s->layer0;
    if (s->level >= img->levels || s->x < 0 || s->y < 0 || s->z < 0 || !s->w || !s->h ||
        (uint64_t)s->x + s->w > lw || (uint64_t)s->y + s->h > lh || first + layers > img->layers ||
        (is3d && (uint64_t)s->z + s->nz > max1(img->depth >> s->level))) {
        warn_once(W_REGION, "copy region %d,%d,%d %ux%ux%u outside level %u of a %ux%u texture: skipped", s->x, s->y,
                  s->z, s->w, s->h, s->nz, s->level, img->width, img->height);
        return 0;
    }
    return 1;
}

static VkImageSubresourceLayers sub_layers(const Sub *s) {
    int is3d = s->img->type == VK_IMAGE_TYPE_3D;
    return (VkImageSubresourceLayers){s->aspect, s->level, is3d ? 0 : s->layer0 + (uint32_t)s->z, is3d ? 1 : s->nz};
}

static VkOffset3D sub_offset(const Sub *s) {
    return (VkOffset3D){s->x, s->y, s->img->type == VK_IMAGE_TYPE_3D ? s->z : 0};
}

static VkExtent3D sub_extent(const Sub *s) {
    return (VkExtent3D){s->w, s->h, s->img->type == VK_IMAGE_TYPE_3D ? s->nz : 1};
}

// A compressed buffer copy covers whole blocks (buffer_side counts them rounded up); Vulkan wants the extent in whole
// blocks too, or reaching the level's edge (VUID 00207/00208): MKW copies 511x511 texels of BC7 into a 512x512 level.
// Not for texture copies: rounding each side against its own level made equal regions differ, and the copy a blit.
static VkExtent3D sub_extent_blocks(const Sub *s) {
    VkExtent3D e = sub_extent(s);
    if (s->astc_bw || (s->bw == 1 && s->bh == 1)) return e;
    uint32_t bw = (uint32_t)s->bw, bh = (uint32_t)s->bh, rw = (s->w + bw - 1) / bw * bw, rh = (s->h + bh - 1) / bh * bh;
    uint32_t lw = s->img->width >> s->level, lh = s->img->height >> s->level;
    lw = lw ? lw : 1, lh = lh ? lh : 1;
    e.width = rw < lw - (uint32_t)s->x ? rw : lw - (uint32_t)s->x;
    e.height = rh < lh - (uint32_t)s->y ? rh : lh - (uint32_t)s->y;
    return e;
}

// Buffer side of a texture <-> buffer copy: bytes covered, pitches, Vulkan buffer span (a VRAM mirror when the pool
// has one).
static int buffer_side(const VkExec *ex, const Sub *s, uint64_t addr, uint64_t *size, VkBufferImageCopy *c,
                       VkMemSpan *span) {
    uint32_t wb = (s->w + (uint32_t)s->bw - 1) / (uint32_t)s->bw, hb = (s->h + (uint32_t)s->bh - 1) / (uint32_t)s->bh;
    uint64_t row = ex->copy_stride > 0 ? (uint64_t)ex->copy_stride : (uint64_t)wb * (uint64_t)s->bpb;
    if (row % (uint64_t)s->bpb || row < (uint64_t)wb * (uint64_t)s->bpb) {
        warn_once(W_REGION, "buffer copy row stride %llu does not fit %u blocks of %d bytes: skipped",
                  (unsigned long long)row, wb, s->bpb);
        return 0;
    }
    uint64_t img = row * hb;
    *size = img * (s->nz - 1) + row * (hb - 1) + (uint64_t)wb * (uint64_t)s->bpb;
    VkMemSpan m;
    if (!addr || !vk_mem_resolve(addr, *size, &m) || m.avail < *size) {
        warn_once(W_BUFFER, "texture copy buffer %#llx (%llu bytes) is not one backed span: skipped",
                  (unsigned long long)addr, (unsigned long long)*size);
        return 0;
    }
    VK_MEM_PICK(m);
    *span = m;
    *c = (VkBufferImageCopy){.bufferOffset = m.offset, .bufferRowLength = (uint32_t)(row / (uint64_t)s->bpb) * (uint32_t)s->bw,
                             .bufferImageHeight = hb * (uint32_t)s->bh, .imageSubresource = sub_layers(s),
                             .imageOffset = sub_offset(s), .imageExtent = sub_extent_blocks(s)};
    return 1;
}

static void copy_buffer_image(VkExec *ex, const p_copy_b2t *p, int to_image) {
    Sub s;
    if (!sub_resolve(p->tex, &p->view, p->has_view, &p->r, 1, &s, 0)) return;
    uint64_t size;
    VkBufferImageCopy c;
    VkMemSpan m;
    if (!buffer_side(ex, &s, p->src, &size, &c, &m)) return;
    vk_mem_count(m.cls, to_image ? VK_MUSE_COPY_SRC : VK_MUSE_GPU_WRITE, size);
    vk_exec_pass_end(ex);
    vk_exec_barrier(ex);
    uint32_t crumb = vk_crumb_op(ex->cb, to_image ? "buffer to image copy" : "image to buffer copy", s.w, s.h);
    if (to_image) vkCmdCopyBufferToImage(ex->cb, m.buffer, s.img->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    else vkCmdCopyImageToBuffer(ex->cb, s.img->image, VK_IMAGE_LAYOUT_GENERAL, m.buffer, 1, &c);
    vk_crumb_end(ex->cb, VK_CRUMB_OP, crumb);
    ex->recorded++;
    if (to_image) vk_texture_gpu_written(p->tex), vk_sparse_written(s.img->image, s.level, s.layer0 + (uint32_t)s.z, s.nz, s.x, s.y, s.w, s.h);
    else vk_mem_gpu_wrote(&m, size), note_gpu_write(ex, p->src, size);
}

// An ASTC texture copied into a sparse texture re-encoded as BC7: MKW fills each layer of its 8192x8192x68 terrain
// arrays, level by level, from an 8192x8192 ASTC texture it uses for nothing else (m19). The blocks are decoded from the
// source's storage straight into the array (vk_texture_astc_copy), so the source never gets an image: 85 MiB of BC7
// each, several alive, and with the heaps full one failed to be created and its layers stayed black (m20).
static int astc_into_sparse(const p_copy_t2t *p) {
    TexVkFormat fs, fd;
    if (!p->src || !p->dst || !(p->dst->d.flags & NVN_TEXTURE_FLAGS_SPARSE) || !tex_vk_format(p->src->d.format, &fs) ||
        !fs.astc_bw || !tex_vk_format(p->dst->d.format, &fd) || fd.astc_bw != fs.astc_bw || fd.astc_bh != fs.astc_bh ||
        p->src->d.target == NVN_TEXTURE_TARGET_3D)
        return 0;
    Sub d;
    if (!sub_resolve(p->dst, &p->dv, p->has_dv, &p->dr, 1, &d, 1) || !vk_sparse_is(d.img->image) ||
        (d.img->format != VK_FORMAT_BC7_SRGB_BLOCK && d.img->format != VK_FORMAT_BC7_UNORM_BLOCK))
        return 0;
    uint32_t level = p->has_sv && p->sv.set_levels ? p->sv.base_level : 0;
    uint32_t layer = (p->has_sv && p->sv.set_layers ? p->sv.min_layer : 0) + (uint32_t)p->sr.zoffset;
    uint32_t lw = max1(p->src->d.width >> level), lh = max1(p->src->d.height >> level);
    uint32_t x = (uint32_t)p->sr.xoffset, y = (uint32_t)p->sr.yoffset;
    uint32_t w = p->sr.width > 0 ? (uint32_t)p->sr.width : lw - x, h = p->sr.height > 0 ? (uint32_t)p->sr.height : lh - y;
    uint32_t n = p->sr.depth > 0 ? (uint32_t)p->sr.depth : 1, dl0 = d.layer0 + (uint32_t)d.z;
    if (level >= p->src->d.levels || x + w > lw || y + h > lh || w != d.w || h != d.h) return 0;
    if (!vk_texture_astc_copy(p->src, level, layer, n, x, y, w, h, d.img, d.aspect, d.level, dl0, d.x, d.y, 1))
        return 0;
    vk_texture_gpu_written(p->dst);
    vk_sparse_written(d.img->image, d.level, dl0, n, d.x, d.y, w, h);
    return 1;
}

static void copy_t2t(VkExec *ex, const p_copy_t2t *p) {
    Sub s, d;
    static int trace = -1;
    static volatile int32_t ntrace;
    if (trace < 0) trace = cfg_get("MKW_SPARSE_TRACE") != NULL;
    if (trace && p->src && p->dst && ((p->src->d.flags | p->dst->d.flags) & NVN_TEXTURE_FLAGS_SPARSE) &&
        plat_inc32(&ntrace) <= 8192)
        rt_log("sparse: copy %ux%ux%u fmt %#x flags %#x lv%u [%d,%d,%d %dx%dx%d] -> %#llx %ux%ux%u fmt %#x [%d,%d,%d "
               "%dx%dx%d] level %d layer %d", p->src->d.width, p->src->d.height, p->src->d.depth, p->src->d.format,
               p->src->d.flags, p->src->d.levels, p->sr.xoffset, p->sr.yoffset, p->sr.zoffset, p->sr.width,
               p->sr.height, p->sr.depth, (unsigned long long)(uintptr_t)p->dst->mem, p->dst->d.width,
               p->dst->d.height, p->dst->d.depth, p->dst->d.format, p->dr.xoffset, p->dr.yoffset, p->dr.zoffset,
               p->dr.width, p->dr.height, p->dr.depth, p->has_dv && p->dv.set_levels ? p->dv.base_level : -1,
               p->has_dv && p->dv.set_layers ? p->dv.min_layer : -1);
    if (astc_into_sparse(p)) return;
    if (!sub_resolve(p->src, &p->sv, p->has_sv, &p->sr, 1, &s, 1)) return;
    if (!sub_resolve(p->dst, &p->dv, p->has_dv, &p->dr, 1, &d, 1)) return;
    // ASTC decoded on upload (MKW fills its sparse ASTC arrays from 8192x8192 ASTC textures, run-sp1): a copy of the
    // decoded texels when both sides are the same ASTC block in the same Vulkan format (BC7 or RGBA8).
    int to_bc7 = d.img->format == VK_FORMAT_BC7_SRGB_BLOCK || d.img->format == VK_FORMAT_BC7_UNORM_BLOCK;
    if (s.astc_bw && s.astc_bw == d.astc_bw && s.astc_bh == d.astc_bh && s.img->format != d.img->format && to_bc7) {
        uint32_t dl0 = d.layer0 + (uint32_t)d.z;
        if (vk_texture_astc_copy(p->src, s.level, s.layer0 + (uint32_t)s.z, s.nz, (uint32_t)s.x, (uint32_t)s.y, s.w, s.h,
                                 d.img, d.aspect, d.level, dl0, d.x, d.y, 1)) {
            vk_texture_gpu_written(p->dst);
            vk_sparse_written(d.img->image, d.level, dl0, s.nz, d.x, d.y, s.w, s.h);
            return;
        }
    }
    if ((s.astc_bw || d.astc_bw) &&
        (s.astc_bw != d.astc_bw || s.astc_bh != d.astc_bh || s.img->format != d.img->format)) {
        warn_once(W_FORMAT, "copy between ASTC textures decoded to VkFormat %d and %d (or ASTC and not): skipped",
                  (int)s.img->format, (int)d.img->format);
        return;
    }
    d.nz = s.nz;
    int same_size = s.w == d.w && s.h == d.h;
    int same_format = s.img->format == d.img->format;
    if (s.img->samples != d.img->samples && !(s.img->samples > 1 && d.img->samples == 1 && same_size && same_format)) {
        warn_once(W_COPY, "texture copy between %u and %u samples: skipped", s.img->samples, d.img->samples);
        return;
    }
    if (s.aspect != d.aspect || (s.aspect != VK_IMAGE_ASPECT_COLOR_BIT && !same_format)) {
        warn_once(W_COPY, "texture copy between formats %#x and %#x: skipped", p->src->d.format, p->dst->d.format);
        return;
    }
    VkImageSubresourceLayers sl = sub_layers(&s), dl = sub_layers(&d);
    // Between a 2D array and a 3D image, the layers of one side are the depth slices of the other: the extent's depth
    // is their count (VUID-VkImageCopy-srcImage-01790..01792). The source's own extent gave depth 1, and only slice 0 of
    // the race's 32x32x32 colour-grading LUTs was filled (all slices alike, no blue: run-race26, 31/10/2026).
    int s3d = s.img->type == VK_IMAGE_TYPE_3D, d3d = d.img->type == VK_IMAGE_TYPE_3D;
    VkExtent3D ext = {s.w, s.h, s3d || d3d ? s.nz : 1};
    vk_exec_pass_end(ex);
    vk_exec_barrier(ex);
    uint32_t crumb = vk_crumb_op(ex->cb, "image copy, blit or resolve", d.w, d.h);
    if (s.img->samples > 1 && d.img->samples == 1) {
        VkImageResolve rv = {sl, sub_offset(&s), dl, sub_offset(&d), ext};
        vkCmdResolveImage(ex->cb, s.img->image, VK_IMAGE_LAYOUT_GENERAL, d.img->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rv);
    } else if (same_size && same_format) {
        VkImageCopy cp = {sl, sub_offset(&s), dl, sub_offset(&d), ext};
        vkCmdCopyImage(ex->cb, s.img->image, VK_IMAGE_LAYOUT_GENERAL, d.img->image, VK_IMAGE_LAYOUT_GENERAL, 1, &cp);
    } else {   // scaled or converting copy: nearest, or linear when asked for (colour only)
        int linear = (p->flags & NVN_COPY_FLAGS_LINEAR_FILTER_BIT) && s.aspect == VK_IMAGE_ASPECT_COLOR_BIT;
        // A blit touching a 3D image takes one layer (VUID-vkCmdBlitImage-srcImage-00240): a 2D array <-> 3D copy goes
        // slice by slice; 3D <-> 3D and 2D <-> 2D in one blit as before.
        uint32_t n = s3d != d3d ? s.nz : 1;
        for (uint32_t k = 0; k < n; k++) {
            VkImageBlit bl = {.srcSubresource = sl, .dstSubresource = dl};
            int32_t sz = s.z + (int32_t)k, dz = d.z + (int32_t)k, depth = n > 1 ? 1 : (int32_t)s.nz;
            if (n > 1) {
                if (!s3d) bl.srcSubresource.baseArrayLayer = s.layer0 + (uint32_t)sz, bl.srcSubresource.layerCount = 1;
                if (!d3d) bl.dstSubresource.baseArrayLayer = d.layer0 + (uint32_t)dz, bl.dstSubresource.layerCount = 1;
            }
            bl.srcOffsets[0] = (VkOffset3D){s.x, s.y, s3d ? sz : 0};
            bl.srcOffsets[1] = (VkOffset3D){s.x + (int32_t)s.w, s.y + (int32_t)s.h, s3d ? sz + depth : 1};
            bl.dstOffsets[0] = (VkOffset3D){d.x, d.y, d3d ? dz : 0};
            bl.dstOffsets[1] = (VkOffset3D){d.x + (int32_t)d.w, d.y + (int32_t)d.h, d3d ? dz + depth : 1};
            vkCmdBlitImage(ex->cb, s.img->image, VK_IMAGE_LAYOUT_GENERAL, d.img->image, VK_IMAGE_LAYOUT_GENERAL, 1, &bl,
                           linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
        }
    }
    vk_crumb_end(ex->cb, VK_CRUMB_OP, crumb);
    ex->recorded++;
    vk_texture_gpu_written(p->dst);
    vk_sparse_written(d.img->image, d.level, d.layer0 + (d3d ? 0 : (uint32_t)d.z), d3d ? 1 : d.nz, d.x, d.y, d.w, d.h);
}

// ---- buffer to buffer

static void copy_b2b(VkExec *ex, const p_copy_b2b *p) {
    uint64_t src = p->src, dst = p->dst, left = p->size;
    int started = 0;
    while (left) {
        VkMemSpan s, d;
        if (!vk_mem_resolve(src, 1, &s) || !vk_mem_resolve(dst, 1, &d)) {
            warn_once(W_BUFFER, "buffer copy %#llx -> %#llx (%llu bytes) leaves backed memory: rest skipped",
                      (unsigned long long)p->src, (unsigned long long)p->dst, (unsigned long long)p->size);
            break;
        }
        uint64_t n = left < s.avail ? left : s.avail;
        if (n > d.avail) n = d.avail;
        VK_MEM_PICK(s);
        VK_MEM_PICK(d);
        if (s.buffer == d.buffer && s.offset < d.offset + n && d.offset < s.offset + n) {
            warn_once(W_OVERLAP, "buffer copy %#llx -> %#llx overlaps within one buffer: skipped",
                      (unsigned long long)p->src, (unsigned long long)p->dst);
            break;
        }
        if (!started) vk_exec_pass_end(ex), vk_exec_barrier(ex), started = 1;
        VkBufferCopy bc = {s.offset, d.offset, n};
        vkCmdCopyBuffer(ex->cb, s.buffer, d.buffer, 1, &bc);
        vk_mem_gpu_wrote(&d, n);
        ex->recorded++;
        vk_mem_count(s.cls, VK_MUSE_COPY_SRC, n);
        vk_mem_count(d.cls, VK_MUSE_GPU_WRITE, n);
        src += n, dst += n, left -= n;
    }
    if (started) note_gpu_write(ex, p->dst, p->size - left);
}

// ---- buffer fills (ClearBuffer, and ReportValue as a 4-byte one): vkCmdFillBuffer in GPU order after a full
// barrier, so the value lands after earlier work, as NVN's semaphore release does; CPU-visible ranges go to
// vk_guest_written at the fence, as copies do. Works in CPU-invisible pools.

static void clear_buf(VkExec *ex, const p_clear_buf *p) {
    if ((p->addr | p->size) & 3) {
        warn_once(W_BUFFER, "buffer fill at %#llx of %llu bytes is not 4-byte aligned: skipped", (unsigned long long)p->addr,
                  (unsigned long long)p->size);
        return;
    }
    uint64_t dst = p->addr, left = p->size;
    int started = 0;
    while (left) {
        VkMemSpan d;
        if (!vk_mem_resolve(dst, 1, &d) || d.offset % 4) {
            warn_once(W_BUFFER, "buffer fill at %#llx (%llu bytes) leaves backed memory: rest skipped",
                      (unsigned long long)p->addr, (unsigned long long)p->size);
            break;
        }
        uint64_t n = left < d.avail ? left : d.avail & ~3ull;
        if (!n) break;
        if (!started) vk_exec_pass_end(ex), vk_exec_barrier(ex), started = 1;
        VK_MEM_PICK(d);
        vkCmdFillBuffer(ex->cb, d.buffer, d.offset, n, p->value);
        vk_mem_gpu_wrote(&d, n);
        ex->recorded++;
        vk_mem_count(d.cls, VK_MUSE_GPU_WRITE, n);
        dst += n, left -= n;
    }
    if (started) note_gpu_write(ex, p->addr, p->size - left);
}

// ---- texture clears

// Whole levels only (vkCmdClear*Image has no rectangle); partial regions, channel masks and a view that reinterprets
// the format are skipped (MacNativeGames does not replay them either, nvn_mtl.m).
static void clear_tex(VkExec *ex, const p_clear_tex *p) {
    const tex_t *t = p->tex;
    if (!t) return;
    Sub s;
    if (!sub_resolve(t, &p->view, p->has_view, &p->r, 1, &s, 0)) return;
    const VkTexImage *img = s.img;
    int is3d = img->type == VK_IMAGE_TYPE_3D;
    uint32_t lw = max1(img->width >> s.level), lh = max1(img->height >> s.level);
    int reformat = p->has_view && p->view.set_format && p->view.format != t->d.format;
    if (s.x || s.y || s.w != lw || s.h != lh || (p->mask & 0xf) != 0xf || reformat ||
        (is3d && (s.z || s.nz != max1(img->depth >> s.level)))) {
        warn_once(W_CLEAR, "texture clear of region %d,%d %ux%u mask %#x (level %ux%u, view format change %d): skipped",
                  s.x, s.y, s.w, s.h, p->mask, lw, lh, reformat);
        return;
    }
    VkImageSubresourceRange rng = {img->aspect, s.level, 1, is3d ? 0 : s.layer0 + (uint32_t)s.z, is3d ? 1 : s.nz};
    if (img->aspect & VK_IMAGE_ASPECT_COLOR_BIT) {
        rng.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        VkClearColorValue cv;
        if ((vk_format_numeric(img->format) != GFX_TYPE_FLOAT) != (p->is_uint != 0)) {
            warn_once(W_CLEAR, "texture clear value kind (uint %d) does not match format %d: skipped", p->is_uint, (int)img->format);
            return;
        }
        memcpy(&cv, &p->c, sizeof cv);
        vk_exec_pass_end(ex);
        vk_exec_barrier(ex);
        vkCmdClearColorImage(ex->cb, img->image, VK_IMAGE_LAYOUT_GENERAL, &cv, 1, &rng);
    } else {
        int both = (img->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) && (img->aspect & VK_IMAGE_ASPECT_STENCIL_BIT);
        VkClearDepthStencilValue dv = {0, 0};
        if (img->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) dv.depth = p->c.f[0];
        if (img->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) dv.stencil = both ? p->c.u[1] : p->c.u[0];
        vk_exec_pass_end(ex);
        vk_exec_barrier(ex);
        vkCmdClearDepthStencilImage(ex->cb, img->image, VK_IMAGE_LAYOUT_GENERAL, &dv, 1, &rng);
    }
    ex->recorded++;
    vk_texture_gpu_written(t);
}

void vk_exec_op_xfer(VkExec *ex, uint32_t op, const void *payload) {
    switch (op) {
    case OP_COPY_B2B: copy_b2b(ex, payload); break;
    case OP_COPY_B2T: copy_buffer_image(ex, payload, 1); break;
    case OP_COPY_T2B: copy_buffer_image(ex, payload, 0); break;   // p_copy_b2t: src holds the destination address
    case OP_COPY_T2T: copy_t2t(ex, payload); break;
    case OP_CLEAR_TEX: clear_tex(ex, payload); break;
    case OP_SIGNAL_EVENT: signal_event(ex, payload); break;
    case OP_REPORT_COUNTER: report_counter(ex, payload); break;
    case OP_RESET_COUNTER: reset_counter(ex, payload); break;
    case OP_CLEAR_BUF: clear_buf(ex, payload); break;
    case OP_RENDER_ENABLE: ex->render_disabled = !((const p_flag *)payload)->value; break;
    case OP_RENDER_COND:
        ex->render_cond = *(const p_render_cond *)payload;
        ex->has_render_cond = 1;
        int on = cond_planned(payload);
        ex->render_disabled = !(on >= 0 ? on : cond_enabled(payload));
        break;
    default: rt_fatal("vk: op %s is not a transfer", nvn_op_name(op));
    }
}
