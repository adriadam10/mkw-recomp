// Clears of the op-stream replay (vk_exec_internal.h): colour and depth-stencil clears, as image clears outside a
// pass when they cover the whole target, else as attachment clears in one; MKW_DEPTH_CLEAR_ALL's layers cleared ahead.
#include <math.h>
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

#include "vk_exec_internal.h"
#include "vk_passhz.h"

// ---- clears

// Outside a pass, a clear of a whole target (no narrower scissor, the target as large as the render area) clears the
// image directly instead of opening a pass for it.
static int whole_target(const VkExec *ex, const VkExecTarget *t) {
    // vkCmdClear*Image addresses a 3D level whole, not one depth plane: slices of a 3D target clear in a pass; it
    // cannot clear a compressed image: views of BC blocks clear in a pass too
    if (ex->in_pass || t->is3d || t->blocks || t->width != ex->pass_w || t->height != ex->pass_h) return 0;
    if (!ex->has_scissor) return 1;
    const p_scissor *s = &ex->scissor;
    return s->x <= 0 && s->y <= 0 && (int64_t)s->x + s->w >= t->width && (int64_t)s->y + s->h >= t->height;
}

// The barrier before a whole-image clear, except inside a run of clears: right after another image clear with nothing
// recorded between, and of a subresource no clear of the run touched, it orders nothing the run's first barrier did
// not. The race clears the 5 layers of a 128x128 target one by one and 19 targets a frame, each behind a barrier that
// drains the GPU (cc1); RADV's own clears need none between them. MKW_CLEAR_RUN=0 puts each barrier back; =ab.
static void clear_barrier(VkExec *ex, VkImage img, uint32_t level, uint32_t layer0, uint32_t layers, int bound) {
    static int mode = -1;
    if (mode < 0) {
        mode = rt_knob("CLEAR_RUN", "1");
        if (mode != 1) rt_log("vk: a barrier before every image clear%s (MKW_CLEAR_RUN)", mode == 2 ? " in the A blocks" : "");
    }
    int run = RT_KNOB_ON(mode) && ex->barrier_at >= 0 && ex->clear_run_at == ex->recorded && ex->nclr > 0 &&
              ex->nclr < (int)(sizeof ex->clr / sizeof ex->clr[0]);
    for (int i = 0; run && i < ex->nclr; i++)
        if (ex->clr[i].img == img && ex->clr[i].level == level && layer0 < ex->clr[i].layer0 + ex->clr[i].layers &&
            ex->clr[i].layer0 < layer0 + layers)
            run = 0;
    // MKW_PASS_HAZARDS follows the clears of the run and the passes before it alike, and decides alone: the run's own list
    // starts at its first clear, whose barrier may have been left out.
    int ph = ph_clear(ex, img, level, layer0, layers, bound);
    if (ph >= 0) run = ph;
    if (!run) vk_exec_barrier(ex), ex->nclr = 0;
    if (ex->nclr < (int)(sizeof ex->clr / sizeof ex->clr[0])) ex->clr[ex->nclr++] = (typeof(ex->clr[0])){img, level, layer0, layers};
    ex->clear_run_at = ex->recorded + 1;   // the clear the caller records next
}

static void clear_attachment(VkExec *ex, const VkClearAttachment *a) {
    vk_exec_pass_begin(ex);
    VkClearRect cr = {vk_exec_scissor(ex), 0, ex->pass_layers};
    if (!cr.rect.extent.width || !cr.rect.extent.height) return;
    vkCmdClearAttachments(ex->cb, 1, a, 1, &cr);
    ex->recorded++;
    ex->fb_dirty |= a->aspectMask & VK_IMAGE_ASPECT_COLOR_BIT ? 1u << a->colorAttachment : 0x100u;
}

// MKW_CLEAR_CENSUS=1 (a measurement, frame-carrera paso 5): one line per colour clear (the first 2000), with what decides
// whether RADV fast-clears it: size (no DCC at 512x512 or less), format, layers against the image's, whole or partial.
// The 0.7 ms of colour clears a race frame (rt-y2) come from some of these; MKW_PASS_TIMING says which passes.
static void clear_census(const VkExec *ex, const VkExecTarget *t, int whole) {
    static int on = -1, n;
    if (on < 0) on = cfg_get("MKW_CLEAR_CENSUS") && atoi(cfg_get("MKW_CLEAR_CENSUS"));
    if (!on || n++ >= 2000) return;
    VkRect2D r = vk_exec_scissor(ex);
    rt_log("clear: f%llu %ux%u level %u layers %u+%u vk fmt %d nvn %#x %s, rect %ux%u%s",
           (unsigned long long)vk_exec_presented, t->width, t->height, t->level, t->base_layer, t->layers,
           (int)t->format, t->nvn_format, whole ? "image clear" : "attachment clear", r.extent.width, r.extent.height,
           t->width <= 512 && t->height <= 512 ? " (small: no DCC)" : "");
}

void clear_color(VkExec *ex, const p_clear_color *c) {
    vk_exec_frame.clears++;
    if (ex->render_disabled) {
        if (vk_cond_trace > 0) vk_cond_trace--, rt_log("cond: f%llu skipped colour clear", (unsigned long long)vk_exec_presented);
        return;
    }
    resolve_rt(ex);
    int i = c->index;
    if (i < 0 || i >= (int)ex->pass_ncolor || !ex->color[i].image) {
        exec_warn_once(W_CLEAR_TARGET, "colour clear of target %d, which is not bound or not renderable: skipped", i);
        return;
    }
    uint32_t mask = (uint32_t)c->mask & 0xf;
    if (!mask) return;
    if (mask != 0xf) {   // vkCmdClearAttachments ignores write masks; a masked clear needs a draw (vk_exec_draw.c)
        exec_warn_once(W_CLEAR_MASK, "colour clear with channel mask %#x is not replayed: skipped", mask);
        return;
    }
    const VkExecTarget *t = &ex->color[i];
    VkClearColorValue cv;
    memcpy(cv.float32, c->rgba, sizeof cv.float32);   // integer targets: the payload's bits as the front end stored them
    int whole = whole_target(ex, t);
    clear_census(ex, t, whole);
    if (whole) {
        clear_barrier(ex, t->image, t->level, t->base_layer, t->layers, t->img && t->img->bound);
        VkImageSubresourceRange r = {VK_IMAGE_ASPECT_COLOR_BIT, t->level, 1, t->base_layer, t->layers};
        uint32_t crumb = vk_crumb_op(ex->cb, "colour clear", t->width, t->height);
        vkCmdClearColorImage(ex->cb, t->image, VK_IMAGE_LAYOUT_GENERAL, &cv, 1, &r);
        vk_crumb_end(ex->cb, VK_CRUMB_OP, crumb);
        ex->recorded++;
        ph_cleared(ex, t->image, t->level, t->base_layer, t->layers, t->img && t->img->bound);
        vk_texture_gpu_written(t->tex);
        return;
    }
    VkClearAttachment a = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .colorAttachment = (uint32_t)i};
    a.clearValue.color = cv;
    clear_attachment(ex, &a);
}

// Layered depth clears (MKW_DEPTH_CLEAR_ALL, default 1; "ab" in the B blocks): race GPU 22.71 -> 19.91 ms, frame
// -4.9 %, no sampling of a layer cleared ahead (rt-z5). MKW clears its 3-layer 3072x3072 D16 shadow
// target one layer at a time, to 1.0, before rendering each (rt-z4); RADV fast-clears depth (HTILE only) only when the
// clear covers every layer of the image (radv_meta_clear.c radv_is_clear_rect_full), so each took ~0.8 ms of GPU,
// 2.4 per frame (rt-z1). The first such clear clears every layer; a later clear of another layer to the same value
// is skipped while nothing wrote that layer since (a pass on it, or any copy or transfer op, or a new submission).
// The cost: a read of such a layer between the two clears sees the cleared value, not the old contents; DC.reads
// counts the samplings of the image while layers are pending, logged with MKW_PASS_TIMING's line.
// Every layer is cleared only where a sequence starts: at a layer whose clear already came since the image's last
// clear of every layer (DC.seq), or the image's first. Pending layers are dropped at each new submission, copy, report
// or queue switch, and the clear of the next layer then cleared every layer again, wiping the ones drawn before it:
// the kart's shadow missing or the near cascade dark for one frame, ~1 in 2,400 (tclose1, tclose4, 08/11/2026). eden-nx2
// clears only the layer asked for (vk_rasterizer.cpp:728).
int dc_mode = -1;
static uint64_t dc_mid;   // clears of one layer in the middle of a sequence (logged)

static uint32_t *dc_seq(VkImage img) {
    int i = 0;
    while (i < 3 && DC.seq[i].img != img) i++;
    if (DC.seq[i].img != img) DC.seq[i].img = img, DC.seq[i].done = 0;   // the last entry is the one recycled
    if (i) {   // most recent first
        __typeof__(DC.seq[0]) e = DC.seq[i];
        memmove(&DC.seq[1], &DC.seq[0], (size_t)i * sizeof e);
        DC.seq[0] = e;
    }
    return &DC.seq[0].done;
}

void vk_exec_dc_read(VkImage img) {
    if (DC.pending && img == DC.img) __atomic_add_fetch(&dc_cnt[0], 1, __ATOMIC_RELAXED);
}

void vk_exec_dc_stats(uint64_t *reads, uint64_t *skipped, uint64_t *ahead, uint64_t *by_layer) {
    *reads = __atomic_exchange_n(&dc_cnt[0], 0, __ATOMIC_RELAXED);
    *skipped = __atomic_exchange_n(&dc_cnt[1], 0, __ATOMIC_RELAXED);
    *ahead = __atomic_exchange_n(&dc_cnt[2], 0, __ATOMIC_RELAXED);
    *by_layer = __atomic_exchange_n(&dc_cnt[3], 0, __ATOMIC_RELAXED);
}

// The whole-image path of a depth-only clear of one layer: 1 when done here (skipped, or every layer cleared).
static int dc_layered(VkExec *ex, const VkExecTarget *t, VkImageAspectFlags asp, const VkClearDepthStencilValue *v) {
    if (dc_mode < 0) {
        dc_mode = rt_knob("DEPTH_CLEAR_ALL", "1");
        if (dc_mode != 1) rt_log("vk: layered depth clears clear every layer at once %s (MKW_DEPTH_CLEAR_ALL)",
                                 dc_mode == 2 ? "in the B blocks" : "off");
    }
    if (!dc_mode || !RT_KNOB_ON(dc_mode) || !t->img || asp != VK_IMAGE_ASPECT_DEPTH_BIT || t->layers != 1 ||
        t->img->levels != 1 || t->img->layers < 2 || t->img->layers > 32 || (v->depth != 0.0f && v->depth != 1.0f))
        return 0;
    if (dc_seen(t->image)) return __atomic_add_fetch(&dc_cnt[3], 1, __ATOMIC_RELAXED), 0;
    uint32_t bit = 1u << t->base_layer, *done = dc_seq(t->image);
    if (DC.img == t->image && DC.depth == v->depth && (DC.pending & bit)) {
        DC.pending &= ~bit, *done |= bit;
        __atomic_add_fetch(&dc_cnt[1], 1, __ATOMIC_RELAXED);
        return 1;
    }
    if (*done && !(*done & bit)) {   // the sequence goes on with its pending layers dropped: this layer only
        uint64_t n = __atomic_add_fetch(&dc_mid, 1, __ATOMIC_RELAXED);
        if (n <= 32 || !(n & 63))
            rt_log("vk: layered depth clear of layer %u in the middle of a sequence (layers %#x cleared, pending dropped "
                   "by %s): this layer only, f%llu (%llu so far)", t->base_layer, *done,
                   DC.img != t->image ? "another image's clear" : DC.depth != v->depth ? "another depth"
                   : DC.dropped_by ? DC.dropped_by : "?", (unsigned long long)vk_exec_presented,
                   (unsigned long long)n);
        *done |= bit;
        return 0;
    }
    vk_exec_barrier(ex);
    VkImageSubresourceRange r = {asp, 0, 1, 0, t->img->layers};
    uint32_t crumb = vk_crumb_op(ex->cb, "depth clear", t->width, t->height);
    vkCmdClearDepthStencilImage(ex->cb, t->image, VK_IMAGE_LAYOUT_GENERAL, v, 1, &r);
    vk_crumb_end(ex->cb, VK_CRUMB_OP, crumb);
    ex->recorded++;
    ph_cleared(ex, t->image, 0, 0, t->img->layers, t->img->bound);
    vk_texture_gpu_written(t->tex);
    DC.img = t->image, DC.depth = v->depth, DC.pending = ((t->img->layers == 32 ? 0u : 1u << t->img->layers) - 1u) & ~bit;
    DC.dropped_by = NULL, *done = bit;
    __atomic_add_fetch(&dc_cnt[2], 1, __ATOMIC_RELAXED);
    return 1;
}

void clear_ds(VkExec *ex, const p_clear_ds *c) {
    if (ex->render_disabled) {
        if (vk_cond_trace > 0) vk_cond_trace--, rt_log("cond: f%llu skipped depth clear", (unsigned long long)vk_exec_presented);
        return;
    }
    resolve_rt(ex);
    const VkExecTarget *t = &ex->depth;
    if (!t->image) return;
    VkImageAspectFlags asp = 0;
    if (c->depth_mask && (t->aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) asp |= VK_IMAGE_ASPECT_DEPTH_BIT;
    uint32_t sm = (uint32_t)c->stencil_mask & 0xff;
    if (sm && (t->aspect & VK_IMAGE_ASPECT_STENCIL_BIT)) {
        if (sm == 0xff) asp |= VK_IMAGE_ASPECT_STENCIL_BIT;
        else exec_warn_once(W_STENCIL_MASK, "stencil clear with mask %#x is not replayed: skipped", sm);
    }
    if (!asp) return;
    float d = c->depth < 0.0f ? 0.0f : c->depth > 1.0f ? 1.0f : c->depth;
    VkClearDepthStencilValue v = {d, (uint32_t)c->stencil & 0xff};
    static int n_dslog;   // MKW_PASS_TIMING: the first depth clears of large targets, for the fast-clear conditions
    if (pt_on > 0 && t->width >= 2048 && n_dslog < 12 && t->img)
        n_dslog++, rt_log("vk: depth clear %ux%u layers %u+%u of %u, level %u of %u, depth %g, stencil %s, whole %d",
                          t->width, t->height, t->base_layer, t->layers, t->img->layers, t->level, t->img->levels, d,
                          asp & VK_IMAGE_ASPECT_STENCIL_BIT ? "yes" : "no", whole_target(ex, t));
    if (pt_on > 0 && !ex->in_pass)   // MKW_PASS_TIMING: by path (bit 28: the whole image) and target size
        vk_pt_mark(ex->cb, 1u << 29 | (uint32_t)whole_target(ex, t) << 28 | (t->width >> 4 & 0x3fff) << 14 |
                               (t->height >> 4 & 0x3fff));
    if (whole_target(ex, t)) {
        if (dc_layered(ex, t, asp, &v)) return;
        clear_barrier(ex, t->image, t->level, t->base_layer, t->layers, t->img && t->img->bound);
        VkImageSubresourceRange r = {asp, t->level, 1, t->base_layer, t->layers};
        uint32_t crumb = vk_crumb_op(ex->cb, "depth clear", t->width, t->height);
        vkCmdClearDepthStencilImage(ex->cb, t->image, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &r);
        vk_crumb_end(ex->cb, VK_CRUMB_OP, crumb);
        ex->recorded++;
        ph_cleared(ex, t->image, t->level, t->base_layer, t->layers, t->img && t->img->bound);
        vk_texture_gpu_written(t->tex);
        return;
    }
    VkClearAttachment a = {.aspectMask = asp};
    a.clearValue.depthStencil = v;
    clear_attachment(ex, &a);
}

