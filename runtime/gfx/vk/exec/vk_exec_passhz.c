// MKW_PASS_HAZARDS (default 0; 1, "ab", "census"): no full barrier before a pass that has nothing to do with what was
// recorded since the last one. The replay puts one before every pass (715 a frame in a four-player race), besides the
// game's own; each empties the GPU and its caches. Without any (MKW_PASS_NOBARRIER, a measurement) the GPU took 2.0 ms a
// frame less there (RX 6700 XT). Left out only when, since the last full barrier, nothing but passes and image clears
// were recorded in this command buffer, none of their draws stored into a buffer or an image, and the pass's targets
// meet neither what they rendered or cleared nor what they sampled. Then every texture its draws sample is checked
// against what was rendered without a barrier: at the first that meets (or that stores anywhere), the pass ends and begins again behind the
// barrier (as a feedback loop does), and the targets are remembered, so the next pass on them gets its barrier at once.
// Four players, in-run A/B (plain and inverted, RX 6700 XT, RADV): GPU -1.6 ms a frame.
// Images over a physical pool may share memory: any two different ones count as meeting. "census" keeps every barrier
// and counts what would have happened.
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "vk.h"
#include "vk_exec.h"
#include "vk_exec_internal.h"
#include "vk_passhz.h"
#include "platform/platform.h"

#define PH_W 48
#define PH_R 384
#define PH_LEARN 4096

static _Thread_local struct {
    const VkExec *ex;
    VkCommandBuffer cb;
    int at;                          // ex->recorded after the last command followed
    uint8_t dirty;                   // a draw stored somewhere, or a list is full: the next pass gets the barrier
    uint8_t force;                   // the pass beginning was ended for the barrier
    uint8_t elided;                  // the open pass has no barrier before it
    uint8_t drew;                    // a draw of the open pass was checked
    uint8_t bound_w, bound_r;        // some image over a physical pool was written, sampled
    int nw, nr, w0;                  // w0: the first write of the open pass
    uint64_t key;
    PhRange w[PH_W], r[PH_R];
    uint64_t learn[PH_LEARN];
} *P;

_Thread_local uint32_t ph_gen = 1;   // bumped at every command buffer this thread begins (ph_new_cb)

enum { C_FRESH, C_CHAIN, C_DIRTY, C_TARGET_W, C_TARGET_R, C_BOUND, C_LEARNED, C_LEFT_OUT, C_RESTART_FIRST, C_RESTART_LATER,
       C_CLEAR_KEPT, C_CLEAR_OUT, C_FORCED, C_N };
static const char *const c_name[C_N] = {
    "right after a barrier", "after something else", "after a store", "its target was written", "its target was sampled",
    "physical pool memory", "learned", "LEFT OUT", "restarted at the first draw", "restarted later", "clears kept",
    "clears LEFT OUT", "forced"};
static int64_t cnt[C_N];

static void count(int c) { __atomic_add_fetch(&cnt[c], 1, __ATOMIC_RELAXED); }

int ph_mode(void) {   // 0 off, 1 on, 2 in the B blocks, 3 census
    static int mode = -1;
    int m = __atomic_load_n(&mode, __ATOMIC_RELAXED);
    if (m < 0) {
        const char *e = cfg_get("MKW_PASS_HAZARDS");
        m = e && !strcmp(e, "census") ? 3 : rt_knob("PASS_HAZARDS", "0");
        if (m) rt_log("vk: no barrier before passes independent of what came before%s (MKW_PASS_HAZARDS)",
                      m == 2 ? " in the B blocks" : m == 3 ? ": counted only" : "");
        __atomic_store_n(&mode, m, __ATOMIC_RELAXED);
    }
    return m;
}

static int on(void) {
    int m = ph_mode();
    return m == 3 ? 1 : RT_KNOB_ON(m);
}

void ph_log(void) {
    if (!ph_mode()) return;
    char b[512];
    int k = 0;
    for (int i = 0; i < C_N; i++)
        k += snprintf(b + k, sizeof b - (size_t)k, "%s%s %lld", i ? ", " : "", c_name[i],
                      (long long)__atomic_exchange_n(&cnt[i], 0, __ATOMIC_RELAXED));
    rt_log("vk: pass barriers since the last line: %s (MKW_PASS_HAZARDS)", b);
}

static int meets(const PhRange *a, const PhRange *b) {
    return a->image == b->image && a->l0 < b->l1 && b->l0 < a->l1 && a->a0 < b->a1 && b->a0 < a->a1;
}

static PhRange target(const VkExecTarget *t) {
    PhRange r = {t->image, t->level, t->level + 1, t->base_layer, t->base_layer + t->layers, t->img && t->img->bound};
    if (t->is3d) r.a0 = 0, r.a1 = ~0u;   // views of a 3D level have one layer
    return r;
}

static void reset(const VkExec *ex) {
    P->ex = ex, P->cb = ex->cb, P->at = ex->recorded;
    P->nw = P->nr = P->w0 = 0, P->dirty = 0, P->bound_w = P->bound_r = 0;
}

// A command buffer begins: nothing of it is followed yet. Handles are recycled and recorded starts at 0 again, so the last
// one's state could pass for this one's; a chunk copies the walk's serials, so the draw memo (ph_draw) could as well.
void ph_new_cb(void) {
    ph_gen++;
    if (P) P->ex = NULL, P->cb = VK_NULL_HANDLE, P->force = P->elided = 0;
}

void ph_barrier(const VkExec *ex) {   // a full barrier was recorded
    if (!ph_mode()) return;
    if (!P && !(P = calloc(1, sizeof *P))) rt_fatal("vk: out of memory (pass hazards)");
    reset(ex);
}

static int followed(const VkExec *ex) { return P && P->ex == ex && P->cb == ex->cb && P->at == ex->recorded; }

// What writing t would meet: 0 nothing, or why (C_*).
static int write_meets(const PhRange *t) {
    for (int i = 0; i < P->nw; i++)
        if (meets(t, &P->w[i])) return C_TARGET_W;
    for (int i = 0; i < P->nr; i++)
        if (meets(t, &P->r[i])) return C_TARGET_R;
    if (t->bound && (P->bound_w || P->bound_r)) return C_BOUND;   // another image over the same memory, perhaps
    return 0;
}

static void add_write(const PhRange *t) {
    if (P->nw == PH_W) P->dirty = 1;
    else P->w[P->nw++] = *t;
    P->bound_w |= t->bound;
}

// The pass about to begin, its targets resolved: 1 when it needs no barrier. fresh: nothing was recorded since one.
int ph_pass_begin(VkExec *ex, int fresh) {
    if (!ph_mode()) return 0;
    if (!P && !(P = calloc(1, sizeof *P))) rt_fatal("vk: out of memory (pass hazards)");
    int why = -1;
    uint64_t key = 0x9E3779B97F4A7C15ull;
    PhRange t[9];
    int n = 0;
    for (uint32_t i = 0; i < 9; i++) {
        const VkExecTarget *v = i < 8 ? &ex->color[i] : &ex->depth;
        if ((i < 8 && i >= ex->pass_ncolor) || !v->image) continue;
        t[n++] = target(v);
        key = (key ^ (uint64_t)(uintptr_t)v->image ^ (uint64_t)v->level << 40 ^ (uint64_t)v->base_layer << 48) * 0x9E3779B97F4A7C15ull;
    }
    P->key = key | 1;
    if (fresh && !P->force) why = C_FRESH;   // the state was reset at that barrier
    else if (P->force) why = C_FORCED;
    else if (!on()) why = C_CHAIN;
    else if (!followed(ex)) why = C_CHAIN;
    else if (P->dirty) why = C_DIRTY;
    for (int i = 0; i < n && why < 0; i++) {
        int w = write_meets(&t[i]);
        if (w) why = w;
    }
    if (why < 0 && P->learn[(key >> 40) % PH_LEARN] == P->key) why = C_LEARNED;
    int out = why < 0;
    count(out ? C_LEFT_OUT : why);
    if (!out) reset(ex);
    P->force = 0, P->elided = (uint8_t)out, P->drew = 0;
    P->w0 = P->nw;
    for (int i = 0; i < n; i++) add_write(&t[i]);
    P->ex = ex, P->cb = ex->cb;
    return out && ph_mode() != 3;
}

void ph_pass_end(VkExec *ex) {   // right after vkCmdEndRendering was counted
    if (P && P->ex == ex && P->cb == ex->cb) P->at = ex->recorded;
}

int ph_following(void) { return P && ph_mode(); }

// A draw of the open pass samples s: 1 when it was rendered or cleared since the last barrier, outside this pass.
int ph_sample(const PhRange *s) {
    if (!s->image) return 0;
    for (int i = 0; i < P->w0; i++)
        if (meets(s, &P->w[i])) return 1;
    if (s->bound && P->bound_w) {
        for (int i = 0; i < P->w0; i++)
            if (P->w[i].bound && P->w[i].image != s->image) return 1;
    }
    for (int i = P->nr; i--;)   // the same texture again, most likely among the last ones
        if (P->r[i].image == s->image && P->r[i].l0 <= s->l0 && P->r[i].l1 >= s->l1 && P->r[i].a0 <= s->a0 && P->r[i].a1 >= s->a1)
            return 0;
    if (P->nr == PH_R) P->dirty = 1;
    else P->r[P->nr++] = *s;
    P->bound_r |= s->bound;
    return 0;
}

// A draw of the open pass stores into a buffer or an image, or touches what is not followed (mesh stages, storage
// images): the next pass gets its barrier, and 1 when this one had none, as what it stores into may be in use by a pass
// before it.
int ph_stores(void) {
    P->dirty = 1;
    return P->elided;
}

// The draw samples what was rendered without a barrier: the pass's targets are remembered. 1: end the pass and begin it
// again (ph_pass_begin then asks for the barrier).
int ph_conflict(void) {
    count(P->drew ? C_RESTART_LATER : C_RESTART_FIRST);
    P->learn[((P->key & ~1ull) >> 40) % PH_LEARN] = P->key;
    if (ph_mode() == 3) {   // counted only: as if the barrier had come
        P->elided = 0;
        memmove(P->w, P->w + P->w0, (size_t)(P->nw - P->w0) * sizeof *P->w);   // the open pass's own targets stay
        P->nw -= P->w0, P->w0 = P->nr = 0, P->bound_w = P->bound_r = 0;
        for (int i = 0; i < P->nw; i++) P->bound_w |= (uint8_t)P->w[i].bound;
        return 0;
    }
    P->force = 1;
    return 1;
}

void ph_drew(void) { P->drew = 1; }

// An image clear of [level, layers): 1 when it needs no barrier before it, 0 when it does, -1 when that is not known here
// (not followed, or off).
int ph_clear(VkExec *ex, VkImage img, uint32_t level, uint32_t layer0, uint32_t layers, int bound) {
    if (!P || !ph_mode()) return -1;
    if (!on() || !followed(ex) || ex->in_pass) return count(C_CLEAR_KEPT), -1;
    PhRange t = {img, level, level + 1, layer0, layer0 + layers, (uint32_t)bound};
    int out = !P->dirty && !write_meets(&t);
    count(out ? C_CLEAR_OUT : C_CLEAR_KEPT);
    return ph_mode() == 3 ? -1 : out;
}

void ph_cleared(VkExec *ex, VkImage img, uint32_t level, uint32_t layer0, uint32_t layers, int bound) {   // it is recorded
    if (!P || !ph_mode()) return;
    PhRange t = {img, level, level + 1, layer0, layer0 + layers, (uint32_t)bound};
    if (P->ex != ex || P->cb != ex->cb || P->at != ex->recorded - 1) return;   // not followed: a barrier comes next
    add_write(&t);
    P->at = ex->recorded;
}
