// Frame generation (MKW_FRAMEGEN=off|auto|2|3|4|120, off by default: the console does not do it): frames made between
// the game's, from its last two, so a game at 30 fps is shown at 60 (2x) and one at 60 at 120 on a fast display.
// MKW hands out no motion vectors, so the motion comes from the pictures (optical flow), on the present thread and the
// GPU only: nothing on the game thread or the replay thread (docs/project/roadmap.md).
//
// Per game frame (vk_fg_frame, the present thread, before the game frame's timeline value is signalled):
//   1. the crop rectangle of the game's picture is copied into a picture of our own (the game draws over its own next
//      frame) and reduced to a luma pyramid: full size, a quarter, a sixteenth;
//   2. block matching, one vector per 16x16 pixels (32x32 at first): at the sixteenth within +-8 texels (+-128
//      pixels), then at quarter size within +-3 texels around that, then on the picture within +-3 pixels with the
//      fraction of a pixel, each followed by a 3x3 median (shaders/fg_flow.comp, fg_median.comp);
//   3. each made frame at t = k / n (shaders/fg_synth.comp).
// The caller blits every frame (the made ones, then the game's) into a swapchain image of its own at once and then
// presents each at its time, spread evenly over the game's recent frame interval (an average of the pacer's display
// times): the blits never queue behind the game's next frame on the GPU (Hookshot issue #1702: made frames shown 5.7
// and 11 ms apart instead of 8.3). The game's frame is shown (n - 1) / n of a frame later than without: the latency.
// Frames over 100 ms apart (a loading hitch) are not blended. Auto makes as many frames as fit the monitor's refresh
// rate at the game's frame rate (60 Hz at 30 fps: 2x; 144 Hz at 60 fps: 2x; 60 Hz at 60 fps: none).
//
// The algorithm and the display schedule follow SkyboundTotk's rx_framegen.cpp (a64-kit/gpu/render/rx_framegen.cpp,
// https://github.com/NotRexed/SkyboundTotk @ a4495f95b5e5ab6562d603058dcaa3457d0c8027, MIT, Copyright (c) 2026
// NotRexed), ported from WebGPU; the three-level search and the synthesis guards are ours.
#include "vk.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "fg_flow_spv.h"
#include "fg_fsr_mv_spv.h"
#include "of_mv_spv.h"
#include "vk_oflow.h"
#include "fg_luma_spv.h"
#include "fg_median_spv.h"
#include "fg_hud_spv.h"
#include "fg_synth_spv.h"
#include "hacks.h"
#include "vk_temporal.h"
#include "host.h"
#include "platform/platform.h"

#define SLOTS 4
#define GAP_NS 100000000ull

typedef struct {
    VkImage img;
    VmaAllocation alloc;
    VkImageView view;
} Img;

static struct {
    int tried, ok;
    PFN_vkCmdPushDescriptorSetKHR push;
    VkDescriptorSetLayout dsl, fsr_dsl;
    VkPipelineLayout layout, fsr_layout;
    VkPipeline luma, coarse, fine, full, median, synth, fsr_mvp;
    VkDescriptorSetLayout of_dsl;
    VkPipelineLayout of_layout;
    VkPipeline of_mvp;
    VkOfResult of_result;
    uint64_t of_last_frame;
    int of_active, of_hudless;
    VkTmpFgInputs of_prev;                // the previous present-thread inputs, when replay metadata has moved on
    VkTmpFgInputs sdk_inputs;             // exact inputs selected for the current real FSR dispatch (diagnostic)
    int of_prev_ok;
    VkSampler sampler;
    VkCommandPool pool;
    VkCommandBuffer cb[SLOTS];
    VkFence fence[SLOTS];
    // Two queues (VkCtx.fg_queue not the game's): each game frame's copies of its pictures go to the game's queue
    // (xcb, so the game's next frame cannot write them first) and signal xsem; the generation, the HUD, the dump, and
    // after them the blits and presents (vk_backend.c) go to fg_queue, which waits for that and signals fsem. The next
    // copies wait for fsem: they write the images the last frame's generation read (pic, hl, hpic take turns).
    VkCommandBuffer xcb[SLOTS], rec_x;   // rec_x: where vk_fg_record puts the copies (its cb when 0)
    VkSemaphore xsem, fsem;
    uint64_t xval, fval, bypass_waited;
    int split;
    int luma_ok[2];                      // pic[i]'s luma levels were made (only the optical flow reads them)
    unsigned n;
    // the images, for a w x h picture: the game's last two (rgba8), their luma at full size (lf), quarter size (lq) and
    // a sixteenth (le), the vectors of each level and their medians (coarse, fine, full size) and the made frames
    Img pic[2], lf[2], lq[2], le[2], fl[6], made[VK_FG_MAX - 1];
    // FSR frame generation's dense motion: the native flow with its object motion kept from crossing depth and picture
    // edges, in FSR's current-to-previous direction and without the render jitter
    Img fsr_mv;
    // DLSS-G and FSR 3.1 on an HDR10 picture: the game's picture and the made frames in its own 10-bit format (the
    // optical flow's images above are 8-bit), made at first use
    Img hpic, hmade[VK_FG_MAX - 1];
    // the game frame's picture without the HUD (vk_tmp_fg_hudless), cropped as the picture: rgba8, and the HDR10 one
    Img hl, hhl;
    int hl_fresh, hhl_fresh;
    // the last game frame's: its picture without the HUD and (HDR10) its picture, swapped with the above after each
    // frame; hl_okp: that frame had its picture without the HUD (fg_hud.comp measures the HUD's coverage from both)
    Img hlp, hhlp, hpicp;
    int hl_okp, hp_okp;
    VkImage hl_dump;   // this frame's picture without the HUD (the dump reads it after the swap)
    // fg_hud.comp: the game frame's HUD over the made frames (MKW_FG_HUD=own, the default)
    VkDescriptorSetLayout hdsl;
    VkPipelineLayout hlayout;
    VkPipeline hud;
    int hud_tried;
    uint64_t hl_given, hl_missing, hud_frames;
    int hl_last;   // the last frame had one (MKW_FG_DUMP writes it)
    int hw, hh;
    VkFormat hfmt;
    int mv_w, mv_h;
    int w, h, fresh, cur, have_prev;
    VkImage compare_native;   // diagnostic pair for this call's dump: FSR made frame plus native synthesis
    int nomem_w, nomem_h;   // the size whose images did not fit: not tried again until the size changes
    float lambda, bias;
    float last_jx, last_jy;
    int have_jitter;
    // pacing (the present thread)
    uint64_t last_at, interval, frames, made_total;
    int logged_mult, auto_m;   // auto_m: MKW_FRAMEGEN=auto's factor now (mult_now)
    int ui_paused, ui_logged;  // vk_fg_frame: a menu frame shown as it is (vk_tmp_fg_ui_frame), the lines logged
    uint64_t ui_frames;
    // the GPU time of each slot's command buffer (two timestamps), read when the slot comes round again: the made
    // frames are flipped on the CPU's schedule, so a generation longer than a step shows them late
    VkQueryPool qp;
    double ts_ns;              // nanoseconds per timestamp tick
    int q_mult[SLOTS];   // total pictures recorded in each slot's timed submission, or zero if unused
    double gpu_sum, gpu_max;
    unsigned gpu_n, gpu_over;  // over: longer than the step between the shown frames
} F;

static VkPipeline make_pipeline(VkCtx *c, VkPipelineLayout layout, const uint32_t *code, size_t size, const int *spec,
                                int nspec) {
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = size, .pCode = code};
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(c->device, &mi, NULL, &m) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkSpecializationMapEntry me[7];
    for (int i = 0; i < nspec; i++) me[i] = (VkSpecializationMapEntry){(uint32_t)i, (uint32_t)(i * 4), 4};
    VkSpecializationInfo si = {(uint32_t)nspec, me, (size_t)nspec * 4, spec};
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main",
                                                .pSpecializationInfo = nspec ? &si : NULL},
                                      .layout = layout};
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &ci, NULL, &p) != VK_SUCCESS) p = VK_NULL_HANDLE;
    vkDestroyShaderModule(c->device, m, NULL);
    return p;
}

// MKW_FG_LAMBDA, MKW_FG_BIAS (tuning, with --probe-framegen): the search's cost per pixel of motion and for leaving
// zero, in mean luma difference per patch texel
static float knob(const char *name, float def) {
    const char *e = cfg_get(name);
    return e ? (float)atof(e) : def;
}

static int init(VkCtx *c) {
    __atomic_store_n(&F.tried, 1, __ATOMIC_RELEASE);   // read by vk_fg_retire on other threads
    F.push = (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(c->device, "vkCmdPushDescriptorSetKHR");
    if (!F.push) {
        rt_log("vk: frame generation off: no VK_KHR_push_descriptor");
        return 0;
    }
    // the luma levels and the vectors are written as storage images in formats Vulkan does not require for that
    static const VkFormat need[] = {VK_FORMAT_R16_SFLOAT, VK_FORMAT_R16G16_SFLOAT};
    for (int i = 0; i < 2; i++) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(c->phys, need[i], &fp);
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) {
            rt_log("vk: frame generation off: no storage images of format %d on this GPU", (int)need[i]);
            return 0;
        }
    }
    VkDescriptorSetLayoutBinding b[4];
    for (int i = 0; i < 4; i++)
        b[i] = (VkDescriptorSetLayoutBinding){.binding = (uint32_t)i, .descriptorCount = 1,
                                              .descriptorType = i < 3 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                                                      : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                              .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                                           .bindingCount = 4, .pBindings = b};
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                     .pSetLayouts = &F.dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    // fsr_mv is the only pass with this larger binding layout. Keeping it separate means the opt-in experiment cannot
    // invalidate the luma, native-flow, median or synthesis pipelines when it is off.
    VkDescriptorSetLayoutBinding fb[6];
    for (int i = 0; i < 6; i++)
        fb[i] = (VkDescriptorSetLayoutBinding){.binding = (uint32_t)i, .descriptorCount = 1,
                                               .descriptorType = i < 5 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                                                       : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                               .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo fdci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                            .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                                            .bindingCount = 6, .pBindings = fb};
    VkPushConstantRange fpr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 32};
    VkPipelineLayoutCreateInfo fli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                      .pSetLayouts = &F.fsr_dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &fpr};
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR,
                              .minFilter = VK_FILTER_LINEAR, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                              .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0};
    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                   .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                   .queueFamilyIndex = c->gfx_family};
    // fg_flow.comp's levels {CELL, HALF, RADIUS, START, STEP, SUBPIX, SPARSE}: one vector per 32x32 pixels from an 8x8
    // patch at a sixteenth (128x128 pixels: a 4x4 one, 64x64, matched blurred text and flat ground anywhere), +-8
    // texels (128 pixels) from zero; 8x8 at quarter size (32x32 pixels), +-3 texels around that; 8x8 samples two pixels
    // apart at full size (the 16x16 block), +-2 pixels around that (+-3 matched more wrongly), with the fraction
    static const int coarse[7] = {2, 4, 8, 0, 1, 0, 0}, fine[7] = {4, 4, 3, 1, 1, 0, 1}, full[7] = {16, 4, 2, 1, 2, 1, 0};
    if (vkCreateDescriptorSetLayout(c->device, &dci, NULL, &F.dsl) != VK_SUCCESS ||
        vkCreatePipelineLayout(c->device, &li, NULL, &F.layout) != VK_SUCCESS ||
        vkCreateDescriptorSetLayout(c->device, &fdci, NULL, &F.fsr_dsl) != VK_SUCCESS ||
        vkCreatePipelineLayout(c->device, &fli, NULL, &F.fsr_layout) != VK_SUCCESS ||
        vkCreateSampler(c->device, &si, NULL, &F.sampler) != VK_SUCCESS ||
        vkCreateCommandPool(c->device, &pci, NULL, &F.pool) != VK_SUCCESS ||
        !(F.luma = make_pipeline(c, F.layout, fg_luma_spv, sizeof fg_luma_spv, NULL, 0)) ||
        !(F.coarse = make_pipeline(c, F.layout, fg_flow_spv, sizeof fg_flow_spv, coarse, 7)) ||
        !(F.fine = make_pipeline(c, F.layout, fg_flow_spv, sizeof fg_flow_spv, fine, 7)) ||
        !(F.full = make_pipeline(c, F.layout, fg_flow_spv, sizeof fg_flow_spv, full, 7)) ||
        !(F.median = make_pipeline(c, F.layout, fg_median_spv, sizeof fg_median_spv, NULL, 0)) ||
        !(F.synth = make_pipeline(c, F.layout, fg_synth_spv, sizeof fg_synth_spv, NULL, 0))) {
        rt_log("vk: frame generation off: its pipelines could not be created");
        return 0;
    }
    // FSR 3.1's motion from our flow: only that engine's; without it the flow engine still works (PR 462 review: in
    // the chain above its failure turned all frame generation off)
    if (!(F.fsr_mvp = make_pipeline(c, F.fsr_layout, fg_fsr_mv_spv, sizeof fg_fsr_mv_spv, NULL, 0)))
        rt_log("vk: frame generation: FSR 3.1's motion pipeline could not be created; its frames use the game's vectors");
    VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = F.pool,
                                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = SLOTS};
    VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT};
    if (vkAllocateCommandBuffers(c->device, &ai, F.cb) != VK_SUCCESS) return 0;
    if (c->fg_queue && c->fg_queue != c->gfx_queue && vkAllocateCommandBuffers(c->device, &ai, F.xcb) == VK_SUCCESS) {
        VkSemaphoreTypeCreateInfo st = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE};
        VkSemaphoreCreateInfo sc = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &st};
        F.split = vkCreateSemaphore(c->device, &sc, NULL, &F.xsem) == VK_SUCCESS &&
                  vkCreateSemaphore(c->device, &sc, NULL, &F.fsem) == VK_SUCCESS;
        rt_log("vk: frame generation on a queue of its own%s", F.split ? "" : ": no semaphores, on the game's");
    }
    {
        VkPhysicalDeviceProperties pp;
        vkGetPhysicalDeviceProperties(c->phys, &pp);
        F.ts_ns = pp.limits.timestampPeriod;
        VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP,
                                    .queryCount = 2 * SLOTS};
        if (!pp.limits.timestampComputeAndGraphics || vkCreateQueryPool(c->device, &qi, NULL, &F.qp) != VK_SUCCESS)
            F.qp = VK_NULL_HANDLE;
    }
    for (int i = 0; i < SLOTS; i++)
        if (vkCreateFence(c->device, &fi, NULL, &F.fence[i]) != VK_SUCCESS) return 0;
    F.lambda = knob("MKW_FG_LAMBDA", 0.0001f);
    F.bias = knob("MKW_FG_BIAS", 0.003f);
    F.interval = 1000000000ull / 60;
    return F.ok = 1;
}

static void free_img(VkCtx *c, Img *m) {
    if (m->view) vkDestroyImageView(c->device, m->view, NULL);
    if (m->img) vmaDestroyImage(c->vma, m->img, m->alloc);
    memset(m, 0, sizeof *m);
}

static void free_images(VkCtx *c) {
    for (int i = 0; i < 2; i++) free_img(c, &F.pic[i]), free_img(c, &F.lf[i]), free_img(c, &F.lq[i]), free_img(c, &F.le[i]);
    for (int i = 0; i < 6; i++) free_img(c, &F.fl[i]);
    for (int i = 0; i < VK_FG_MAX - 1; i++) free_img(c, &F.made[i]), free_img(c, &F.hmade[i]);
    free_img(c, &F.fsr_mv);
    free_img(c, &F.hpic);
    free_img(c, &F.hl), free_img(c, &F.hhl), free_img(c, &F.hlp), free_img(c, &F.hhlp), free_img(c, &F.hpicp);
    F.hl_okp = F.hp_okp = 0;
    F.hw = F.hh = 0, F.hfmt = VK_FORMAT_UNDEFINED;
    F.mv_w = F.mv_h = 0;
    F.w = F.h = 0;
    F.have_prev = 0, F.have_jitter = 0;
}

static int make_img(VkCtx *c, Img *m, uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags extra) {
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = fmt,
                            .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | extra};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(c->vma, &ci, &ai, &m->img, &m->alloc, NULL) != VK_SUCCESS) {
        m->img = VK_NULL_HANDLE;
        return 0;
    }
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = m->img,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    return vkCreateImageView(c->device, &vi, NULL, &m->view) == VK_SUCCESS;
}

// The pictures without the HUD for DLSS-G's and FSR 3.1's frames (hl, hlp; ~66 MiB at 4K): made the first time one of
// those engines runs at this size, never for the flow engine (PR 462 review). Freed with the other images.
static int ensure_hudless(VkCtx *c) {
    if (F.hl.img) return 1;
    if (!F.w || !make_img(c, &F.hl, (uint32_t)F.w, (uint32_t)F.h, VK_FORMAT_R8G8B8A8_UNORM,
                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT) ||
        !make_img(c, &F.hlp, (uint32_t)F.w, (uint32_t)F.h, VK_FORMAT_R8G8B8A8_UNORM,
                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        free_img(c, &F.hl), free_img(c, &F.hlp);
        return 0;
    }
    F.hl_fresh = 1, F.hl_okp = 0;
    return 1;
}

// The vector grid: one per 16x16 pixels, a texel of the sixteenth level
static uint32_t grid(int n) { return ((uint32_t)n + 15) / 16; }

static int ensure_images(VkCtx *c, int w, int h) {
    if (F.pic[0].img && F.w == w && F.h == h) return 1;
    // A failed size is not tried every present: each try waits for the whole device (below) and frees what it made
    if (F.nomem_w == w && F.nomem_h == h) return 0;
    // A size change: earlier presents may still read the old images (vkDeviceWaitIdle holds the queue lock, as
    // vk_fsr.c ensure_images)
    vk_queue_lock();
    vkDeviceWaitIdle(c->device);
    vk_queue_unlock();
    free_images(c);
    const VkImageUsageFlags XS = VK_IMAGE_USAGE_TRANSFER_SRC_BIT, XD = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    uint32_t qw = ((uint32_t)w + 3) / 4, qh = ((uint32_t)h + 3) / 4, gw = grid(w), gh = grid(h);
    int ok = 1;
    for (int i = 0; i < 2; i++)
        ok = ok && make_img(c, &F.pic[i], (uint32_t)w, (uint32_t)h, VK_FORMAT_R8G8B8A8_UNORM, XS | XD) &&
             make_img(c, &F.lf[i], (uint32_t)w, (uint32_t)h, VK_FORMAT_R16_SFLOAT, 0) &&
             make_img(c, &F.lq[i], qw, qh, VK_FORMAT_R16_SFLOAT, 0) &&
             make_img(c, &F.le[i], gw, gh, VK_FORMAT_R16_SFLOAT, 0);
    for (int i = 0; i < 6; i++)   // (the coarse level's two: one vector per 2x2 of the others)
        ok = ok && make_img(c, &F.fl[i], i < 2 ? (gw + 1) / 2 : gw, i < 2 ? (gh + 1) / 2 : gh, VK_FORMAT_R16G16_SFLOAT, 0);
    for (int i = 0; i < VK_FG_MAX - 1; i++)
        ok = ok && make_img(c, &F.made[i], (uint32_t)w, (uint32_t)h, VK_FORMAT_R8G8B8A8_UNORM, XS);
    // (the pictures without the HUD, hl and hlp, only for DLSS-G's and FSR 3.1's: ensure_hudless)
    if (!ok) {
        rt_log("vk: frame generation: no memory for its %dx%d images; off until the size changes", w, h);
        free_images(c);
        F.nomem_w = w, F.nomem_h = h;
        return 0;
    }
    F.w = w, F.h = h, F.fresh = 1, F.cur = 0;
    rt_log("vk: frame generation: %dx%d pictures, %ux%u vectors", w, h, gw, gh);
    return 1;
}

// FSR describes motion vectors at its render size, which is smaller than the displayed picture with DLSS/FSR quality.
static int ensure_fsr_motion(VkCtx *c, int w, int h) {
    if (F.fsr_mv.img && F.mv_w == w && F.mv_h == h) return 1;
    vk_queue_lock();
    vkDeviceWaitIdle(c->device);
    vk_queue_unlock();
    free_img(c, &F.fsr_mv);
    F.mv_w = F.mv_h = 0;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    const char *dump_inputs = cfg_get("MKW_FG_DUMP_INPUTS");
    if (dump_inputs && strcmp(dump_inputs,"0")) usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (!make_img(c, &F.fsr_mv, (uint32_t)w, (uint32_t)h, VK_FORMAT_R16G16_SFLOAT, usage)) return 0;
    F.mv_w = w, F.mv_h = h;
    return 1;
}

static void mem_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &d);
}

static void img_barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkPipelineStageFlags2 ss, VkAccessFlags2 sa,
                        VkPipelineStageFlags2 ds, VkAccessFlags2 da) {
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = ss, .srcAccessMask = sa,
                               .dstStageMask = ds, .dstAccessMask = da, .oldLayout = from,
                               .newLayout = VK_IMAGE_LAYOUT_GENERAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img,
                               .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1,
                          .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &d);
}

// One pass: inputs a, b, c (b and c may repeat a) into out, over w x h invocations, then a barrier
static void pass(VkCommandBuffer cb, VkPipeline p, const Img *a, const Img *b, const Img *c, const Img *out,
                 const void *pc, uint32_t w, uint32_t h) {
    VkDescriptorImageInfo ii[4] = {{F.sampler, a->view, VK_IMAGE_LAYOUT_GENERAL},
                                   {F.sampler, (b ? b : a)->view, VK_IMAGE_LAYOUT_GENERAL},
                                   {F.sampler, (c ? c : a)->view, VK_IMAGE_LAYOUT_GENERAL},
                                   {VK_NULL_HANDLE, out->view, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet wr[4];
    for (int i = 0; i < 4; i++)
        wr[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = (uint32_t)i,
                                       .descriptorCount = 1,
                                       .descriptorType = i < 3 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                                               : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                       .pImageInfo = &ii[i]};
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p);
    rec_push(F.push, cb, VK_PIPELINE_BIND_POINT_COMPUTE, F.layout, 0, 4, wr);
    vkCmdPushConstants(cb, F.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 16, pc);
    vkCmdDispatch(cb, (w + 7) / 8, (h + 7) / 8, 1);
    mem_barrier(cb);
}

// Dense FSR motion: the pictures compare and gate the nine coarse-cell candidates, while depth keeps a kart's vector
// off the road behind it. camera is the per-pixel fallback already in FSR's direction.
static void fsr_motion_pass(VkCommandBuffer cb, const VkTmpFgInputs *in, const Img *cur, const Img *prev, const Img *flow,
                            const Img *out, const float pc[8], uint32_t w, uint32_t h) {
    VkDescriptorImageInfo ii[6] = {{F.sampler, cur->view, VK_IMAGE_LAYOUT_GENERAL},
                                   {F.sampler, prev->view, VK_IMAGE_LAYOUT_GENERAL},
                                   {F.sampler, in->depth_view, VK_IMAGE_LAYOUT_GENERAL},
                                   {F.sampler, in->mv_view, VK_IMAGE_LAYOUT_GENERAL},
                                   {F.sampler, flow->view, VK_IMAGE_LAYOUT_GENERAL},
                                   {VK_NULL_HANDLE, out->view, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet wr[6];
    for (int i = 0; i < 6; i++)
        wr[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = (uint32_t)i,
                                       .descriptorCount = 1,
                                       .descriptorType = i < 5 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                                               : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                       .pImageInfo = &ii[i]};
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, F.fsr_mvp);
    rec_push(F.push, cb, VK_PIPELINE_BIND_POINT_COMPUTE, F.fsr_layout, 0, 6, wr);
    vkCmdPushConstants(cb, F.fsr_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 32, pc);
    vkCmdDispatch(cb, (w + 7) / 8, (h + 7) / 8, 1);
    mem_barrier(cb);
}

static int of_motion_pass(VkCommandBuffer cb, const VkTmpFgInputs *in, const VkTmpFgInputs *cached_prev,
                          VkImageView current_color, VkImageView previous_color,
                          VkBuffer vectors, uint32_t gw, uint32_t gh, uint32_t grid, const Img *out, uint32_t rw,
                          uint32_t rh, uint32_t ow, uint32_t oh, float jx, float jy) {
    VkCtx *c = vk_ctx();
    VkTmpFgInputs prev = {0};
    static int geometry_mode = -1;
    if (geometry_mode < 0) geometry_mode = rt_knob("FG_NVOF_GEOMETRY", "0");
    static int refine_mode = -1;
    if (refine_mode < 0) refine_mode = rt_knob("FG_NVOF_REFINE", "0");
    static int camera_mode = -1;
    if (camera_mode < 0) camera_mode = rt_knob("FG_NVOF_CAMERA", "0");
    int camera_agreement = RT_KNOB_ON(camera_mode);
    static int last_camera = -1;
    if (last_camera != camera_agreement) last_camera = camera_agreement,
        rt_log("vk: FSR hardware motion camera agreement %s (2 output-pixel tolerance)", camera_agreement ? "on" : "off");
    int refine = RT_KNOB_ON(refine_mode);
    static int last_refine = -1;
    if (last_refine != refine) last_refine = refine,
        rt_log("vk: FSR hardware motion endpoint refinement %s", refine ? "on" : "off");
    int knob_on = RT_KNOB_ON(geometry_mode), prev_ok = 0, prev_source = 0;
    if (knob_on && !in->reset) {
        if (in->frame > 1 && vk_tmp_fg_inputs(in->frame - 1, &prev))
            prev_ok = prev.depth_view && prev.w == in->w && prev.h == in->h && !prev.reset, prev_source = 1;
        if (!prev_ok && cached_prev && cached_prev->frame + 1 == in->frame) {
            prev = *cached_prev, prev_source = 2;
            prev_ok = prev.depth_view && prev.w == in->w && prev.h == in->h && !prev.reset;
        }
        if (!prev_ok) prev_source = 0;
    }
    static int logged_geometry = -1;
    int geometry_state = prev_ok ? prev_source : 0;
    if (logged_geometry != geometry_state) {
        logged_geometry = geometry_state;
        if (prev_ok)
            rt_log("vk: FSR motion depth geometry active: frame %llu/%llu, %ux%u previous depth from %s; 2%% gate",
                   (unsigned long long)in->frame, (unsigned long long)prev.frame, in->w, in->h,
                   prev_source == 1 ? "replay metadata" : "present cache");
        else
            rt_log("vk: FSR motion depth geometry off at frame %llu: knob %s, previous depth %s",
                   (unsigned long long)in->frame, knob_on ? "on" : "off", knob_on ? "unavailable or reset" : "not queried");
    }
    if (!F.of_mvp) {
        VkDescriptorSetLayoutBinding b[6] = {
            {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
            {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
            {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
            {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
            {4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
            {5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
        VkDescriptorSetLayoutCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR, .bindingCount = 6, .pBindings = b};
        VkPushConstantRange pc = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 128};
        if (vkCreateDescriptorSetLayout(c->device, &di, NULL, &F.of_dsl) != VK_SUCCESS) return 0;
        VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 1, .pSetLayouts = &F.of_dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pc};
        if (vkCreatePipelineLayout(c->device, &li, NULL, &F.of_layout) != VK_SUCCESS) return 0;
        F.of_mvp = make_pipeline(c, F.of_layout, of_mv_spv, sizeof of_mv_spv, NULL, 0);
        if (!F.of_mvp) return 0;
    }
    VkDescriptorBufferInfo bi = {vectors, 0, (VkDeviceSize)gw * gh * 4};
    VkDescriptorImageInfo ii[5] = {
        {VK_NULL_HANDLE, out->view, VK_IMAGE_LAYOUT_GENERAL},
        {F.sampler, in->depth_view, VK_IMAGE_LAYOUT_GENERAL},
        {F.sampler, prev_ok ? prev.depth_view : in->depth_view, VK_IMAGE_LAYOUT_GENERAL},
        {F.sampler, current_color, VK_IMAGE_LAYOUT_GENERAL},
        {F.sampler, previous_color, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet wr[6] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 1, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &ii[0]},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 2, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii[1]},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 3, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii[2]},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 4, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii[3]},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 5, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii[4]}};
    struct { float size[4]; int grid[4]; float jitter[4]; float reproj[16]; float geometry[4]; } pc = {
        {(float)rw,(float)rh,(float)ow,(float)oh},{(int)gw,(int)gh,(int)grid,refine},{jx,jy,0.f,0.f},{0},
        {camera_agreement ? 2.f : prev_ok ? 1.f : 0.f,in->near_,in->far_,0.02f}};
    for (int r = 0; r < 4; r++)
        for (int col = 0; col < 4; col++) pc.reproj[col * 4 + r] = (float)in->m[r * 4 + col];
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, F.of_mvp);
    rec_push(F.push, cb, VK_PIPELINE_BIND_POINT_COMPUTE, F.of_layout, 0, 6, wr);
    vkCmdPushConstants(cb, F.of_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
    vkCmdDispatch(cb, (rw + 7) / 8, (rh + 7) / 8, 1);
    mem_barrier(cb);
    return 1;
}

// fg_hud.comp's pipeline: five pictures sampled, the made frame written in place (each pixel reads only its own)
static int hud_init(VkCtx *c) {
    if (F.hud_tried) return F.hud != VK_NULL_HANDLE;
    F.hud_tried = 1;
    VkDescriptorSetLayoutBinding b[6];
    for (int i = 0; i < 6; i++)
        b[i] = (VkDescriptorSetLayoutBinding){.binding = (uint32_t)i, .descriptorCount = 1,
                                              .descriptorType = i < 5 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                                                      : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                              .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                                           .bindingCount = 6, .pBindings = b};
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof fg_hud_spv,
                                   .pCode = fg_hud_spv};
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateDescriptorSetLayout(c->device, &dci, NULL, &F.hdsl) != VK_SUCCESS) return 0;
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                     .pSetLayouts = &F.hdsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    if (vkCreatePipelineLayout(c->device, &li, NULL, &F.hlayout) != VK_SUCCESS ||
        vkCreateShaderModule(c->device, &mi, NULL, &m) != VK_SUCCESS) {
        rt_log("vk: frame generation: the HUD pass could not be set up: the engine takes the HUD apart itself");
        return 0;
    }
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"},
                                      .layout = F.hlayout};
    if (vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &ci, NULL, &F.hud) != VK_SUCCESS) F.hud = VK_NULL_HANDLE;
    vkDestroyShaderModule(c->device, m, NULL);
    rt_log("vk: frame generation: the game's HUD over the made frames %s", F.hud ? "(fg_hud.comp)" : "could not be set up");
    return F.hud != VK_NULL_HANDLE;
}

static void hud_pass(VkCommandBuffer cb, const Img *fc, const Img *sc, const Img *fp, const Img *sp, const Img *g, int mode,
                     int prev_ok, int w, int h) {
    const Img *in[5] = {fc, sc, fp, sp, g};
    VkDescriptorImageInfo ii[6];
    VkWriteDescriptorSet wr[6];
    for (int i = 0; i < 6; i++) {
        ii[i] = (VkDescriptorImageInfo){i < 5 ? F.sampler : VK_NULL_HANDLE, (i < 5 ? in[i] : g)->view,
                                        VK_IMAGE_LAYOUT_GENERAL};
        wr[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = (uint32_t)i,
                                       .descriptorCount = 1,
                                       .descriptorType = i < 5 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                                               : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                       .pImageInfo = &ii[i]};
    }
    const int pc[4] = {mode, prev_ok, w, h};
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, F.hud);
    rec_push(F.push, cb, VK_PIPELINE_BIND_POINT_COMPUTE, F.hlayout, 0, 6, wr);
    vkCmdPushConstants(cb, F.hlayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 16, pc);
    vkCmdDispatch(cb, ((uint32_t)w + 7) / 8, ((uint32_t)h + 7) / 8, 1);
    mem_barrier(cb);
}

static int provider_made(int got, int fallback_made);   // defined with the selector for its asset-free test

static int fg_engine(void);

static int fg_record(VkCommandBuffer cb, VkImage src, int sw, int sh, const int crop[4], int made, int fallback_made,
                     VkImage out[VK_FG_MAX], int *ow, int *oh) {
    VkCtx *c = vk_ctx();
    if (!F.tried) init(c);
    if (!F.ok) return 0;
    F.compare_native = VK_NULL_HANDLE;
    F.of_active = 0;
    int cx = 0, cy = 0, cw = sw, ch = sh;
    if (crop && crop[2] > 0 && crop[3] > 0) cx = crop[0], cy = crop[1], cw = crop[2], ch = crop[3];
    if (cx < 0) cx = 0;
    if (cy < 0) cy = 0;
    if (cx + cw > sw) cw = sw - cx;
    if (cy + ch > sh) ch = sh - cy;
    if (cw <= 0 || ch <= 0 || !ensure_images(c, cw, ch)) return 0;
    if (made > VK_FG_MAX - 1) made = VK_FG_MAX - 1;
    if (fallback_made > VK_FG_MAX - 1) fallback_made = VK_FG_MAX - 1;
    if (!F.have_prev) made = fallback_made = 0;
    F.hl_last = 0;
    const int cur = F.cur, prev = cur ^ 1;
    const VkPipelineStageFlags2 ALL = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, XF = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    VkCommandBuffer xc = F.rec_x ? F.rec_x : cb;   // the copies, and the layouts of new images (before them)
    if (F.fresh) F.luma_ok[0] = F.luma_ok[1] = 0;
    if (F.fresh) {
        Img *all[] = {&F.pic[0], &F.pic[1], &F.lf[0], &F.lf[1], &F.lq[0], &F.lq[1], &F.le[0],   &F.le[1],   &F.fl[0],
                      &F.fl[1],  &F.fl[2],  &F.fl[3], &F.fl[4], &F.fl[5], &F.made[0], &F.made[1], &F.made[2]};
        for (size_t i = 0; i < sizeof all / sizeof *all; i++)
            img_barrier(xc, all[i]->img, VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_NONE, 0, ALL,
                        VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
        F.fresh = 0;
    }
    // earlier reads of our images (the last frame's presents) finish before this frame writes them; the game's writes
    // to its picture become visible to the copy, and its later writes wait for the copy
    mem_barrier(xc);
    img_barrier(xc, src, VK_IMAGE_LAYOUT_GENERAL, ALL, VK_ACCESS_2_MEMORY_WRITE_BIT, XF, VK_ACCESS_2_TRANSFER_READ_BIT);
    VkImageBlit region = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                          .srcOffsets = {{cx, cy, 0}, {cx + cw, cy + ch, 1}},
                          .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                          .dstOffsets = {{0, 0, 0}, {cw, ch, 1}}};
    vkCmdBlitImage(xc, src, VK_IMAGE_LAYOUT_GENERAL, F.pic[cur].img, VK_IMAGE_LAYOUT_GENERAL, 1, &region, VK_FILTER_NEAREST);
    img_barrier(xc, src, VK_IMAGE_LAYOUT_GENERAL, XF, 0, ALL, 0);
    mem_barrier(xc);
    mem_barrier(cb);
    F.luma_ok[cur] = 0;
    const uint32_t qw = ((uint32_t)cw + 3) / 4, qh = ((uint32_t)ch + 3) / 4, gw = grid(cw), gh = grid(ch);
    const int colour[4] = {1, 1, 0, 0}, level[4] = {0, 4, 0, 0};
    // DLSS Frame Generation or FSR 3.1 frame generation (MKW_FG_ENGINE, vk_temporal.c) in the place of the optical flow,
    // from the same game frame's depth, motion vectors and camera; the flow below when they cannot
    int eng = fg_engine();
    // MKW_FG_TEST=copy (measuring): the made frames are copies of the game's, so the present cadence is frame
    // generation's with next to no GPU work
    static int copy_test = -1;
    if (copy_test < 0) copy_test = cfg_get("MKW_FG_TEST") && !strcmp(cfg_get("MKW_FG_TEST"), "copy");
    if (copy_test && made) {
        mem_barrier(cb);
        VkImageCopy ic = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0},
                          {(uint32_t)cw, (uint32_t)ch, 1}};
        for (int k = 0; k < made; k++) vkCmdCopyImage(cb, F.pic[cur].img, VK_IMAGE_LAYOUT_GENERAL, F.made[k].img, VK_IMAGE_LAYOUT_GENERAL, 1, &ic);
        mem_barrier(cb);
        for (int k = 0; k < made; k++) out[k] = F.made[k].img;
        out[made] = F.pic[cur].img;
        *ow = cw, *oh = ch;
        F.have_prev = 1, F.cur = prev, F.hl_okp = F.hp_okp = 0;
        return made + 1;
    }
    // an HDR10 picture (10-bit PQ) goes to them in its own format
    int hdr = vk_fg_src_format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 || vk_fg_src_format == VK_FORMAT_R16G16B16A16_SFLOAT;
    // SDR-only diagnostic: the native synthesis of exactly the two source pictures used by the real FSR generation.
    // It is read back beside the FSR frame and is never included in out[] or presented.
    static int compare_native_mode = -1;
    if (compare_native_mode < 0) compare_native_mode = rt_knob("FG_COMPARE_NATIVE", "0");
    int compare_native = eng == 2 && !hdr && RT_KNOB_ON(compare_native_mode);
    // FSR interpolation can use the native flow's object motion too. MKW_FG_FLOW_MV is the A/B switch while its
    // confidence gates are tuned; off by default until the hardware comparison.
    static int flow_mv_mode = -1;
    if (flow_mv_mode < 0) flow_mv_mode = rt_knob("FG_FLOW_MV", "0");
    int flow_mv = eng == 2 && RT_KNOB_ON(flow_mv_mode);
    static int zero_mv_mode = -1;
    if (zero_mv_mode < 0) zero_mv_mode = rt_knob("FG_ZERO_MV", "0");
    int zero_mv = eng == 2 && RT_KNOB_ON(zero_mv_mode);   // diagnostic: isolate game vectors from SDK optical flow
    static int of_mode = -1;
    if (of_mode < 0) of_mode = rt_knob("FG_NVOF", "1");
    int of_wanted = eng == 2 && RT_KNOB_ON(of_mode) && c->has_oflow && F.split;
    static int display_mv_mode = -1;
    if (display_mv_mode < 0) display_mv_mode = rt_knob("FG_DISPLAY_MV", "0");
    int display_mv = of_wanted && RT_KNOB_ON(display_mv_mode);
    VkTmpFgInputs fgin = {0};
    int have_fgin = made && (flow_mv || zero_mv || of_wanted) && vk_tmp_fg_inputs(vk_fg_game_frame, &fgin);
    int mv_ready = 0;
    if (have_fgin && (!fgin.reset || zero_mv) && ensure_fsr_motion(c, display_mv ? cw : (int)fgin.w, display_mv ? ch : (int)fgin.h)) {
        img_barrier(cb, F.fsr_mv.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_NONE, 0,
                    VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                    VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
        mem_barrier(cb);
        mv_ready = 1;
    }
    int flow_ready = 0;
    if (made && (eng == 0 || (mv_ready && !zero_mv && !of_wanted) || compare_native)) {
        // The SDK-only path avoids luma work, but FSR's optional object vectors and the readback reference need it
        // before their flow passes too. Both images may be stale after toggling the motion experiment in-run.
        for (int i = 0; i < 2; i++) {
            int k = i ? prev : cur;
            if (F.luma_ok[k]) continue;
            pass(cb, F.luma, &F.pic[k], NULL, NULL, &F.lf[k], colour, (uint32_t)cw, (uint32_t)ch);
            pass(cb, F.luma, &F.lf[k], NULL, NULL, &F.lq[k], level, qw, qh);
            pass(cb, F.luma, &F.lq[k], NULL, NULL, &F.le[k], level, gw, gh);
            F.luma_ok[k] = 1;
        }
        const float pc16[4] = {16, F.lambda, F.bias, 0}, pc4[4] = {4, F.lambda, F.bias, 0}, pc1[4] = {1, F.lambda, F.bias, 0};
        pass(cb, F.coarse, &F.le[prev], &F.le[cur], NULL, &F.fl[0], pc16, (gw + 1) / 2, (gh + 1) / 2);
        pass(cb, F.median, &F.fl[0], NULL, NULL, &F.fl[1], level, (gw + 1) / 2, (gh + 1) / 2);
        pass(cb, F.fine, &F.lq[prev], &F.lq[cur], &F.fl[1], &F.fl[2], pc4, gw, gh);
        pass(cb, F.median, &F.fl[2], NULL, NULL, &F.fl[3], level, gw, gh);
        pass(cb, F.full, &F.lf[prev], &F.lf[cur], &F.fl[3], &F.fl[4], pc1, gw, gh);
        pass(cb, F.median, &F.fl[4], NULL, NULL, &F.fl[5], level, gw, gh);
        flow_ready = 1;
    }
    // an HDR10 picture (10-bit PQ) goes to them in its own format
    if (made && eng && hdr && (F.hw != cw || F.hh != ch || F.hfmt != vk_fg_src_format)) {
        vk_queue_lock();
        vkDeviceWaitIdle(c->device);
        vk_queue_unlock();
        free_img(c, &F.hpic), free_img(c, &F.hhl), free_img(c, &F.hpicp), free_img(c, &F.hhlp);
        F.hp_okp = F.hl_okp = 0;
        for (int i = 0; i < VK_FG_MAX - 1; i++) free_img(c, &F.hmade[i]);
        const VkImageUsageFlags X = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        int ok = make_img(c, &F.hpic, (uint32_t)cw, (uint32_t)ch, vk_fg_src_format, X) &&
                 make_img(c, &F.hhl, (uint32_t)cw, (uint32_t)ch, vk_fg_src_format, X) &&
                 make_img(c, &F.hpicp, (uint32_t)cw, (uint32_t)ch, vk_fg_src_format, X) &&
                 make_img(c, &F.hhlp, (uint32_t)cw, (uint32_t)ch, vk_fg_src_format, X);
        F.hhl_fresh = 1;
        for (int i = 0; i < VK_FG_MAX - 1; i++) ok = ok && make_img(c, &F.hmade[i], (uint32_t)cw, (uint32_t)ch, vk_fg_src_format, X);
        if (ok) {
            F.hw = cw, F.hh = ch, F.hfmt = vk_fg_src_format;
            Img *all[] = {&F.hpic, &F.hmade[0], &F.hmade[1], &F.hmade[2], &F.hhl, &F.hpicp, &F.hhlp};
            for (int i = 0; i < 7; i++)
                img_barrier(xc, all[i]->img, VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_NONE, 0, ALL,
                            VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
        } else {
            F.hw = F.hh = 0;
        }
    }
    int use_h = hdr && F.hw == cw && F.hh == ch;
    if (made && eng && use_h) {   // the picture again, bit for bit in its own format
        img_barrier(xc, src, VK_IMAGE_LAYOUT_GENERAL, ALL, VK_ACCESS_2_MEMORY_WRITE_BIT, XF, VK_ACCESS_2_TRANSFER_READ_BIT);
        vkCmdBlitImage(xc, src, VK_IMAGE_LAYOUT_GENERAL, F.hpic.img, VK_IMAGE_LAYOUT_GENERAL, 1, &region, VK_FILTER_NEAREST);
        mem_barrier(xc);
    }
    vk_fg_out_format = VK_FORMAT_R8G8B8A8_UNORM;
    if (made && eng) {
        VkFormat pf = use_h ? F.hfmt : VK_FORMAT_R8G8B8A8_UNORM;
        const Img *P = use_h ? &F.hpic : &F.pic[cur], *M = use_h ? F.hmade : F.made;
        VkTmpPic bb = {P->img, P->view, pf, (uint32_t)cw, (uint32_t)ch};
        // the same frame's picture without the HUD (vk_temporal.c), cropped the same way into an image of bb's format
        VkTmpPic hls, hl;
        const VkTmpPic *hlp = NULL;
        if (!use_h) ensure_hudless(c);   // (without them: no picture without the HUD, as when the frame has none)
        Img *H = use_h ? &F.hhl : &F.hl;
        if (H->img && vk_tmp_fg_hudless(vk_fg_game_frame, &hls) && (int)hls.w == sw && (int)hls.h == sh &&
            hls.fmt == vk_fg_src_format) {
            if (!use_h && F.hl_fresh) {
                img_barrier(xc, F.hl.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_NONE, 0, ALL,
                            VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
                img_barrier(xc, F.hlp.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_NONE, 0, ALL,
                            VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
                F.hl_fresh = 0;
            }
            mem_barrier(xc);
            vkCmdBlitImage(xc, hls.img, VK_IMAGE_LAYOUT_GENERAL, H->img, VK_IMAGE_LAYOUT_GENERAL, 1, &region, VK_FILTER_NEAREST);
            mem_barrier(xc);
            hl = (VkTmpPic){H->img, H->view, pf, (uint32_t)cw, (uint32_t)ch};
            hlp = &hl;
            F.hl_given++;
        } else {
            F.hl_missing++;
        }
        F.hl_last = hlp != NULL;
        F.hl_dump = hlp ? H->img : VK_NULL_HANDLE;
        // MKW_FG_HUD=own (default): the engine interpolates the pictures without the HUD and fg_hud.comp puts the
        // game frame's HUD back over its frames; =engine: the picture with the HUD and the one without it go to the
        // engine (DLSSG.HUDLess, HUDLessColor), which takes the HUD apart itself
        int own = hlp && strcmp(cfg_value("MKW_FG_HUD"), "engine") != 0 && hud_init(c);
        const VkTmpPic *in_pic = own ? &hl : &bb, *in_hl = own ? NULL : hlp;
        if (mv_ready && of_wanted && !fgin.reset && vk_of_init(c->of_queue, c->of_family)) {
            // The motion must describe the scene that FSR interpolates. Searching the full picture makes the static
            // timer/minimap overwrite the road's vectors underneath the separately composited translucent HUD.
            int clean = own && F.hl_okp && (!use_h || F.hp_okp);
            const Img *previous = clean ? (use_h ? &F.hhlp : &F.hlp) : &F.pic[prev];
            const Img *current = clean ? H : &F.pic[cur];
            int reset = !F.of_last_frame || F.of_last_frame + 1 != fgin.frame || clean != F.of_hudless;
            F.of_active = vk_of_prepare(xc, previous->view, current->view, (uint32_t)cw, (uint32_t)ch,
                                        reset, &F.of_result);
            if (F.of_active) F.of_last_frame = fgin.frame, F.of_hudless = clean;
        }
        VkTmpPic outs[VK_FG_MAX - 1];
        for (int k = 0; k < VK_FG_MAX - 1; k++)
            outs[k] = (VkTmpPic){M[k].img, M[k].view, pf, (uint32_t)cw, (uint32_t)ch};
        int got = 0;
        if (eng == 1) {
            got = vk_tmp_dlssg(cb, vk_fg_game_frame, in_pic, in_hl, outs, made);
        } else {
            VkTmpFgInputs in;
            if (have_fgin || vk_tmp_fg_inputs(vk_fg_game_frame, &in)) {
                if (have_fgin) in = fgin;
                VkTmpFgInputs current_in = in;
                const VkTmpFgInputs *cached_prev =
                    F.of_prev_ok && F.of_prev.frame + 1 == in.frame ? &F.of_prev : NULL;
                int full_mv_ready = 0;
                static int of_jitter = -1;
                if (of_jitter < 0) of_jitter = rt_knob("FG_NVOF_JITTER", "1");
                // The zero-vector diagnostic must override hardware flow, including reset frames.
                if (mv_ready && zero_mv) {
                    VkClearColorValue zero = {.float32 = {0.f, 0.f, 0.f, 0.f}};
                    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                    vkCmdClearColorImage(cb, F.fsr_mv.img, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
                    mem_barrier(cb);
                    in.mv = F.fsr_mv.img, in.mv_view = F.fsr_mv.view;
                } else if (F.of_active && of_motion_pass(cb, &in, cached_prev,
                                                  F.of_hudless ? H->view : F.pic[cur].view,
                                                  F.of_hudless ? (use_h ? F.hhlp.view : F.hlp.view) : F.pic[prev].view,
                                                  F.of_result.vectors, F.of_result.wblocks,
                                                  F.of_result.hblocks, F.of_result.grid, &F.fsr_mv,
                                                  display_mv ? (uint32_t)cw : in.w, display_mv ? (uint32_t)ch : in.h,
                                                  (uint32_t)cw, (uint32_t)ch,
                                                  !display_mv && RT_KNOB_ON(of_jitter) ? in.jx : 0.f,
                                                  !display_mv && RT_KNOB_ON(of_jitter) ? in.jy : 0.f)) {
                    in.mv = F.fsr_mv.img, in.mv_view = F.fsr_mv.view;
                    full_mv_ready = display_mv;
                    static int told;
                    if (!told++) rt_log("vk: FSR motion from hardware optical flow, %ux%u cells of %u pixels",
                                        F.of_result.wblocks, F.of_result.hblocks, F.of_result.grid);
                } else if (mv_ready && flow_ready && !in.reset && F.fsr_mvp) {
                    static int still_mode = -1;
                    if (still_mode < 0) still_mode = rt_knob("FG_STILL_MV", "0");
                    const float pc[8] = {(float)in.w, (float)in.h, (float)cw, (float)ch, (float)gw, (float)gh,
                                         RT_KNOB_ON(still_mode) ? 1.f : 0.f, 0.f};
                    fsr_motion_pass(cb, &in, &F.pic[cur], &F.pic[prev], &F.fl[5], &F.fsr_mv, pc, in.w, in.h);
                    in.mv = F.fsr_mv.img, in.mv_view = F.fsr_mv.view;
                }
                // The display-vector experiment describes a full-size motion resource to FSR. A failed OF setup
                // must not expose the smaller camera-vector image under that description.
                if (!display_mv || full_mv_ready) {
                    F.sdk_inputs = in;
                    got = vk_tmp_ffx_fg(cb, &in, in_pic, in_hl, &outs[0]);
                }
                // Readback-only twin at the FSR frame's midpoint. FSR uses M[0]; this spare image never enters out[].
                if (got == 1 && compare_native && !use_h && flow_ready) {
                    const float pc[4] = {0.5f, 1.0f / (16.0f * (float)gw), 1.0f / (16.0f * (float)gh), 0};
                    pass(cb, F.synth, &F.pic[prev], &F.pic[cur], &F.fl[5], &F.made[1], pc, (uint32_t)cw, (uint32_t)ch);
                    F.compare_native = F.made[1].img;
                }
                F.of_prev = current_in, F.of_prev_ok = 1;
            }
        }
        made = provider_made(got, fallback_made);
        if (got) {
            static int last_eng = -1;
            if (last_eng != eng) last_eng = eng, rt_log("vk: frame generation by %s", eng == 1 ? "DLSS Frame Generation" : "FSR 3.1");
            made = got;
            mem_barrier(cb);
            if (own) {   // the game frame's HUD over each made frame
                const Img *PP = use_h ? &F.hpicp : &F.pic[prev], *HP = use_h ? &F.hhlp : &F.hlp;
                int prev_ok = F.hl_okp && F.have_prev && (!use_h || F.hp_okp);
                int mode = !use_h ? 0 : pf == VK_FORMAT_R16G16B16A16_SFLOAT ? 2 : 1;
                for (int k = 0; k < made; k++) hud_pass(cb, P, H, PP, HP, &M[k], mode, prev_ok, cw, ch);
                F.hud_frames++;
            }
            for (int k = 0; k < made; k++) out[k] = M[k].img;
            out[made] = P->img;
            vk_fg_out_format = pf;
            // this frame's pictures become the last frame's (the images handed out above stay as they are)
            Img t;
            if (use_h) t = F.hpic, F.hpic = F.hpicp, F.hpicp = t, t = F.hhl, F.hhl = F.hhlp, F.hhlp = t;
            else t = F.hl, F.hl = F.hlp, F.hlp = t;
            F.hl_okp = hlp != NULL, F.hp_okp = use_h;
            *ow = cw, *oh = ch;
            if (have_fgin) F.last_jx = fgin.jx, F.last_jy = fgin.jy, F.have_jitter = 1;
            F.have_prev = 1;
            F.cur = prev;
            return made + 1;
        }
        // A provider's failure keeps the existing native fallback. The provider cap must not also cap that path.
        if (made) {
            static int told;
            if (!told++)
                rt_log("vk: %s made no frame; the native flow falls back to %dx",
                       eng == 1 ? "DLSS Frame Generation" : "FSR 3.1", made + 1);
        }
    }
    // the luma levels the flow searches: this frame's, and the last frame's when it went to an engine (none made then)
    for (int i = 0; i < 2 && (made || !eng); i++) {
        int k = i ? prev : cur;
        if (F.luma_ok[k] || (i && !made)) continue;
        pass(cb, F.luma, &F.pic[k], NULL, NULL, &F.lf[k], colour, (uint32_t)cw, (uint32_t)ch);
        pass(cb, F.luma, &F.lf[k], NULL, NULL, &F.lq[k], level, qw, qh);
        pass(cb, F.luma, &F.lq[k], NULL, NULL, &F.le[k], level, gw, gh);
        F.luma_ok[k] = 1;
    }
    if (made) {
        if (!flow_ready) {
            const float pc16[4] = {16, F.lambda, F.bias, 0}, pc4[4] = {4, F.lambda, F.bias, 0}, pc1[4] = {1, F.lambda, F.bias, 0};
            pass(cb, F.coarse, &F.le[prev], &F.le[cur], NULL, &F.fl[0], pc16, (gw + 1) / 2, (gh + 1) / 2);
            pass(cb, F.median, &F.fl[0], NULL, NULL, &F.fl[1], level, (gw + 1) / 2, (gh + 1) / 2);
            pass(cb, F.fine, &F.lq[prev], &F.lq[cur], &F.fl[1], &F.fl[2], pc4, gw, gh);
            pass(cb, F.median, &F.fl[2], NULL, NULL, &F.fl[3], level, gw, gh);
            pass(cb, F.full, &F.lf[prev], &F.lf[cur], &F.fl[3], &F.fl[4], pc1, gw, gh);
            pass(cb, F.median, &F.fl[4], NULL, NULL, &F.fl[5], level, gw, gh);
        }
        for (int k = 0; k < made; k++) {
            const float pc[4] = {(float)(k + 1) / (float)(made + 1), 1.0f / (16.0f * (float)gw), 1.0f / (16.0f * (float)gh), 0};
            pass(cb, F.synth, &F.pic[prev], &F.pic[cur], &F.fl[5], &F.made[k], pc, (uint32_t)cw, (uint32_t)ch);
        }
    }
    for (int k = 0; k < made; k++) out[k] = F.made[k].img;
    out[made] = F.pic[cur].img;
    *ow = cw, *oh = ch;
    F.have_prev = 1;
    F.cur = prev;
    F.hl_okp = F.hp_okp = 0;
    return made + 1;
}

int vk_fg_record(VkCommandBuffer cb, VkImage src, int sw, int sh, const int crop[4], int made, VkImage out[VK_FG_MAX],
                 int *ow, int *oh) {
    return fg_record(cb, src, sw, sh, crop, made, made, out, ow, oh);
}

// The frames per game frame the setting asks for now: 0 off (also under the turbo, whose frames are dropped anyway,
// and while a full-screen movie plays: it goes at the game's own 30 or 60 and a made frame between two of its pictures
// only repeats or smears them; vis3, 15/11/2026: "duplicado cuando no hace falta"), else 1..VK_FG_MAX, at most what
// the swapchain has room for
// Whether m frames per game frame split the screen's refreshes evenly: every frame shown for the same whole number of
// refreshes. The game runs at 60 / iv fps (its present interval, from the measured frame interval); m frames of it
// at hz fit when hz * iv / (60 * m) is a whole number. 20 fps x2 on 60 Hz is 40 fps, frames held 1, 2, 1, 2
// refreshes: judder (Adrià 15/11/2026); 20 x3 and 15 x4 are 60, 15 x2 is 30. hz unknown: taken as fitting.
// The game's interval in vsync ticks of MKW_LOGIC_HZ (1/60 s by default; game.c frame_logic_hz): what it asked NVN for
// (game.c), not a measure. lhz120tt2: read as 1/60 s at 120 Hz, interval 6 (20 fps) looked like 10 fps and got 4x
// on a 60 Hz screen, whose GPU time pushed the interval up again. The average in F.interval lags ~8
// frames behind a change (fga2: 41.9 ms at 20 fps), and steps between display times grow when a frame is late: a
// median of the last three read 5 at 15 fps, no factor fit and frame generation paused (fga3, frames 1670-1675).
static int game_ticks(void) {
    int iv = frame_present_interval();
    return iv < 1 ? 1 : iv;
}

static int fits(int m) {
    double hz = vk_present_refresh_hz();
    if (hz <= 0) return 1;
    double k = hz * game_ticks() / ((double)frame_logic_hz() * m);
    return k >= 0.97 && fabs(k - round(k)) < 0.03 * round(k);
}

// A partial provider result remains the result: vendor and native frames have different temporal positions. Only a
// provider that made none retains the existing native-flow fallback at its full requested factor.
static int provider_made(int got, int fallback_made) { return got > 0 ? got : fallback_made; }

static int vrr(void) { return cfg_bool("MKW_VRR"); }

static int fg_engine(void) {
    const char *v = cfg_value("MKW_FG_ENGINE");
    return !strcmp(v, "dlss") ? 1 : !strcmp(v, "fsr3") ? 2 : 0;
}

// The selected provider's total pictures per game frame (generated + source). DLSS's maximum is a device capability.
// FSR 3.1's frame-generation API returns one generated picture. If its later evaluation fails, fg_record runs the native
// path with the full factor from mult_choice's fallback.
static int provider_mult(void) {
    int eng = fg_engine();
    // The vendors' frame generation consumes the temporal engine's depth and motion vectors across vendors. Without
    // any temporal engine, the native flow keeps the requested refresh-fitted factor.
    if (vk_tmp_engine() == VK_TMP_OFF) return VK_FG_MAX;
    // FSR's frame-generation API has a fixed one-generated-frame limit. Do not load its DLLs from this selector: it runs
    // on the menu thread as well. An unavailable provider returns no picture at evaluation and fg_record falls back to
    // the full native factor.
    if (eng == 2) return 2;
    if (eng == 1) {
        int generated = vk_tmp_ngx_fg_max();
        if (generated >= VK_FG_MAX - 1) return VK_FG_MAX;
        if (generated > 0) return generated + 1;
    }
    return VK_FG_MAX;
}

// Choose from at most max_mult pictures before testing refresh fit. Auto must do this rather than choosing a native
// factor first and clamping it afterwards: a provider cap can turn an evenly fitting candidate into judder.
static int choose_mult(const char *v, int max_mult) {
    int room = vk_present_ahead_max();
    if (max_mult > room) max_mult = room;
    if (max_mult > VK_FG_MAX) max_mult = VK_FG_MAX;
    if (max_mult < 1) max_mult = 1;
    int m;
    if (!strcmp(v, "120")) {
        // Target 120 shown fps without speeding up the game: 60 x2 or 30 x4. Unlike Auto this does not ask a
        // 240 Hz screen for 240 fps. Below 30 game fps, the existing four-frame limit takes precedence.
        int iv = game_ticks(), n = frame_logic_hz();
        m = (120 * iv + n / 2) / n;
        double hz = vk_present_refresh_hz();
        // Do not synthesize pictures the display cannot show; tolerate fractional rates such as 119.88 Hz.
        int refresh_room = hz > 0 ? (int)(hz * iv / n + 0.03) : m;
        if (m > refresh_room) m = refresh_room;
    } else if (!strcmp(v, "auto") && !vrr()) {
        // The most pictures that split the refreshes evenly within the selected provider's limit. If none fit,
        // show the game's own frames rather than silently replacing an even native candidate with an uneven one.
        m = 1;
        for (int k = max_mult; k >= 2 && m == 1; k--)
            if (fits(k)) m = k;
        return m;
    } else if (!strcmp(v, "auto")) {   // a variable refresh screen shows any rate evenly: as many as fit in time
        // The refreshes of the monitor that fit in one game frame, rounded down: rounded up a quarter early, a game
        // frame of 46 ms at 60 Hz (2.76 refreshes) asked for 3x, made 2 frames where 1.76 fit, so the GPU made frames
        // never shown (fg3-auto: 1.1 made per game frame), and it switched between 2x and 3x every few seconds. Kept
        // within a band: up only 0.1 past the next whole number, down only 0.05 under its own.
        double hz = vk_present_refresh_hz(), r = hz * (double)F.interval / 1e9;
        if (hz <= 0) F.auto_m = 2;
        else if (!F.auto_m || r < F.auto_m - 0.05) F.auto_m = (int)r;
        else if (r >= F.auto_m + 1.1) F.auto_m = (int)(r - 0.1);
        m = F.auto_m;
    } else {
        m = atoi(v);
    }
    if (m > max_mult) m = max_mult;
    return m < 1 ? 1 : m;
}

static int mult_choice(int *fallback) {
    // Diagnostic same-run comparison: A presents source frames alone, B includes the selected generation engine.
    // The simulation stays at the configured rate; framegen counters never stand in for base-frame throughput.
    static int bench = -1;
    if (bench < 0) bench = rt_knob("FG_BENCH", "1");
    *fallback = 0;
    if (!RT_KNOB_ON(bench)) return 0;
    const char *v = cfg_value("MKW_FRAMEGEN");
    if (!strcmp(v, "off") || turbo_speed() > 1 || ov_movie_playing()) return 0;
    // MKW_FG_AB=ab (measuring): frame generation off in the MKW_AB blocks A, on in B, within one race
    static int ab = -1;
    if (ab < 0) ab = cfg_get("MKW_FG_AB") && !strcmp(cfg_get("MKW_FG_AB"), "ab");
    if (ab && !rt_ab()) return 0;
    int cap = provider_mult();
    *fallback = choose_mult(v, VK_FG_MAX);
    return cap == VK_FG_MAX ? *fallback : choose_mult(v, cap);
}

static int mult_now(void) { int fallback; return mult_choice(&fallback); }

int vk_fg_wanted(void) { return strcmp(cfg_value("MKW_FRAMEGEN"), "off") != 0; }

// The menu's note: the selected factor does not split this screen's refreshes evenly at the game's rate now.
int vk_fg_uneven(void) {
    const char *v = cfg_value("MKW_FRAMEGEN");
    if (!strcmp(v, "off") || !strcmp(v, "auto") || vrr() || !F.frames) return 0;
    int m = mult_now();
    return m >= 2 && !fits(m);
}

// MKW_FG_DUMP=<counter> (checking, with MKW_FG_DUMP_DIR, default "."): from that generation counter onward,
// four in a row as g<n>.png and the first frame made before each as m<n>.png, read back on the present thread (it
// waits for the GPU then): tools/imagen/fg_check.py takes three g in a row (the made frame between the first and the
// third against the second). MKW_FG_COMPARE_NATIVE=1 in an SDR FSR run also writes r<n>.png, the diagnostic native
// synthesis at t=0.5 from the same two source pictures; it is never presented. MKW_SHOTS reads the game's pictures,
// black on some machines; these are what is shown. The counter can include pre-race pictures: the capture log gives
// its source game frame, reset and hardware-flow state. The host readback pages are touched 120 frames beforehand.
// MKW_FG_DUMP_AFTER=<seconds> instead uses MKW_SEQ_UNTIL's watched event, with a two-second readback warmup.
uint64_t vk_fg_game_frame;   // the game frame vk_fg_frame is called for (vk_backend.c present_main)
VkFormat vk_fg_src_format = VK_FORMAT_R8G8B8A8_UNORM;   // that frame's picture format (vk_backend.c)
VkFormat vk_fg_out_format = VK_FORMAT_R8G8B8A8_UNORM;   // the format of the frames vk_fg_frame handed out

// The four frames go into one buffer, each its own part, and are written once the last is done: a wait for the GPU and
// the PNG encoding at each dumped frame held the present thread long enough to reset frame generation at the next one
// (m == g, 15/11/2026), so the frames dumped were not those of a steady run.
#define DUMP_N 4
static struct {
    uint64_t at;
    int initialized;
    int64_t after_us, prewarmed_us;
    VkBuffer buf;
    VmaAllocation alloc;
    void *map;
    VkDeviceSize size, stride;
    int n[DUMP_N], hl[DUMP_N], ref[DUMP_N], depth[DUMP_N], w, h;
    float near_[DUMP_N], far_[DUMP_N];
    VkTmpFgInputs inputs[DUMP_N];
    VkTmpFgInputs sdk[DUMP_N];
    int raw[DUMP_N];
    int nv[DUMP_N];
    uint32_t grid[DUMP_N][3];
    VkFormat fmt[DUMP_N];   // each frame's exact encoding, including a bypass or HDR transition
} D;

static int dump_record(VkCommandBuffer cb, const VkImage *out, int n, int w, int h, VkImage native_ref) {
    if (!D.initialized) {
        D.initialized = 1;
        const char *e = cfg_get("MKW_FG_DUMP");
        D.at = e && atoll(e) > 0 ? (uint64_t)atoll(e) : UINT64_MAX;
        const char *after = cfg_get("MKW_FG_DUMP_AFTER");
        char *end;
        double seconds = after ? strtod(after, &end) : 0;
        if (after && end != after && !*end && seconds >= .001 && seconds <= 3600) {
            D.after_us = (int64_t)(seconds * 1000000.0);
            D.at = UINT64_MAX;
        }
    }
    const char *raw_env = cfg_get("MKW_FG_DUMP_INPUTS");
    int raw_inputs = raw_env && strcmp(raw_env,"0");
    VkDeviceSize part = (VkDeviceSize)w * h * (raw_inputs ? 28 : 20) + 64;
    int awaiting_after = D.after_us && D.at == UINT64_MAX;
    if (awaiting_after && seqwatch_since_us() < D.after_us - 2000000) return 0;
    uint64_t warm_at = awaiting_after ? F.frames : D.at > 120 ? D.at - 120 : 0;
    if (F.frames < warm_at || (!awaiting_after && F.frames >= D.at + DUMP_N) ||
        (D.buf && (D.w != w || D.h != h))) return 0;
    if (vk_fg_out_format != VK_FORMAT_R8G8B8A8_UNORM && vk_fg_out_format != VK_FORMAT_A2B10G10R10_UNORM_PACK32) return 0;
    VkCtx *c = vk_ctx();
    if (!D.buf) {
        D.size = part * DUMP_N, D.stride = part, D.w = w, D.h = h;
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = D.size,
                                 .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                      .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
        VmaAllocationInfo info;
        if (vmaCreateBuffer(c->vma, &bi, &ai, &D.buf, &D.alloc, &info) != VK_SUCCESS) {
            D.after_us = 0;
            return D.at = UINT64_MAX, 0;
        }
        D.map = info.pMappedData;
        // A 4K batch maps over 500 MiB. Fault its host pages before the requested sequence, so allocation does not
        // reset temporal history between the captured frames. This path exists only with MKW_FG_DUMP enabled.
        memset(D.map, 0, (size_t)D.size);
        vmaFlushAllocation(c->vma, D.alloc, 0, VK_WHOLE_SIZE);
        D.prewarmed_us = seqwatch_since_us();
        rt_log("fg capture: readback prewarmed at counter %llu for %llu", (unsigned long long)F.frames,
               (unsigned long long)D.at);
    }
    if (awaiting_after) {
        int64_t since = seqwatch_since_us();
        if (since < D.after_us || since - D.prewarmed_us < 2000000 || n <= 1) return 0;
        D.at = F.frames;
        rt_log("fg capture: armed %.3f seconds after watched phase, counter %llu source frame %llu",
               since / 1000000.0, (unsigned long long)D.at, (unsigned long long)vk_fg_game_frame);
    }
    if (F.frames < D.at) return 0;
    int k = (int)(F.frames - D.at);
    D.fmt[k] = vk_fg_out_format;
    VkDeviceSize base = part * (VkDeviceSize)k;
    D.n[k] = n;
    D.hl[k] = F.hl_last && F.hl_dump;
    VkBufferImageCopy bc = {.bufferOffset = base, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                            .imageExtent = {(uint32_t)w, (uint32_t)h, 1}};
    vkCmdCopyImageToBuffer(cb, out[n - 1], VK_IMAGE_LAYOUT_GENERAL, D.buf, 1, &bc);
    bc.bufferOffset = base + (VkDeviceSize)w * h * 4;
    if (n > 1) vkCmdCopyImageToBuffer(cb, out[0], VK_IMAGE_LAYOUT_GENERAL, D.buf, 1, &bc);
    bc.bufferOffset = base + (VkDeviceSize)w * h * 8;   // the picture without the HUD (8-bit only)
    if (D.hl[k]) vkCmdCopyImageToBuffer(cb, F.hl_dump, VK_IMAGE_LAYOUT_GENERAL, D.buf, 1, &bc);
    D.ref[k] = native_ref != VK_NULL_HANDLE;
    bc.bufferOffset = base + (VkDeviceSize)w * h * 12;
    if (D.ref[k]) vkCmdCopyImageToBuffer(cb, native_ref, VK_IMAGE_LAYOUT_GENERAL, D.buf, 1, &bc);
    VkTmpFgInputs in;
    D.depth[k] = vk_tmp_fg_inputs(vk_fg_game_frame, &in);
    if (D.depth[k]) {
        D.inputs[k] = in;
        D.near_[k] = in.near_, D.far_[k] = in.far_;
        // Sparse raw depth samples distinguish an incorrect foreground depth from a bad motion vector. No image
        // conversion: the SDK's actual R32 values, in a fifth readback plane used only by this diagnostic.
        const float xy[9][2] = {{.5f,.3f},{.5f,.55f},{.5f,.7f},{.4f,.8f},{.6f,.8f},{.5f,.9f},{.25f,.8f},{.75f,.8f},{.5f,.98f}};
        VkBufferImageCopy samples[9];
        for (int j = 0; j < 9; j++) {
            samples[j] = (VkBufferImageCopy){.bufferOffset = base + (VkDeviceSize)w * h * 16 + (VkDeviceSize)j * 4,
                .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                .imageOffset = {(int32_t)(xy[j][0] * in.w), (int32_t)(xy[j][1] * in.h), 0}, .imageExtent = {1,1,1}};
        }
        vkCmdCopyImageToBuffer(cb, in.depth, VK_IMAGE_LAYOUT_GENERAL, D.buf, 9, samples);
    }
    rt_log("fg capture: counter %llu source frame %llu generated %d reset %d hardware %d format %d",
           (unsigned long long)F.frames, (unsigned long long)vk_fg_game_frame, n - 1,
           D.depth[k] ? D.inputs[k].reset : -1, F.of_active, (int)D.fmt[k]);
    VkDeviceSize nv_bytes = (VkDeviceSize)F.of_result.wblocks * F.of_result.hblocks * 4;
    D.nv[k] = F.of_active && nv_bytes <= (VkDeviceSize)w * h * 4;
    if (D.nv[k]) {
        D.grid[k][0] = F.of_result.wblocks, D.grid[k][1] = F.of_result.hblocks, D.grid[k][2] = F.of_result.grid;
        VkBufferCopy copy = {.srcOffset = 0, .dstOffset = base + (VkDeviceSize)w * h * 16 + 64, .size = nv_bytes};
        vkCmdCopyBuffer(cb, F.of_result.vectors, D.buf, 1, &copy);
    }
    D.raw[k] = raw_inputs && F.sdk_inputs.frame == vk_fg_game_frame &&
               (VkDeviceSize)F.sdk_inputs.w * F.sdk_inputs.h <= (VkDeviceSize)w * h;
    if (D.raw[k]) {
        D.sdk[k] = F.sdk_inputs;
        VkBufferImageCopy copy = {.bufferOffset = base + (VkDeviceSize)w*h*20 + 64,
            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
            .imageExtent = {D.sdk[k].w,D.sdk[k].h,1}};
        vkCmdCopyImageToBuffer(cb,D.sdk[k].depth,VK_IMAGE_LAYOUT_GENERAL,D.buf,1,&copy);
        copy.bufferOffset = base + (VkDeviceSize)w*h*24 + 64;
        vkCmdCopyImageToBuffer(cb,D.sdk[k].mv,VK_IMAGE_LAYOUT_GENERAL,D.buf,1,&copy);
    }
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                           .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                           .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &d);
    return k == DUMP_N - 1;   // the last: written once this submission is done
}

static void dump_write(VkFence fence) {
    VkCtx *c = vk_ctx();
    vkWaitForFences(c->device, 1, &fence, VK_TRUE, 5000000000ull);
    vmaInvalidateAllocation(c->vma, D.alloc, 0, VK_WHOLE_SIZE);
    const char *dir = cfg_get("MKW_FG_DUMP_DIR") ? cfg_get("MKW_FG_DUMP_DIR") : ".";
    char path[1024];
    size_t px = (size_t)D.w * D.h * 4;
    for (int f = 0; f < DUMP_N; f++)
        for (int k = 0; k < 4; k++) {
            if (!D.n[f] || (k == 1 && D.n[f] < 2) || (k == 2 && !D.hl[f]) || (k == 3 && !D.ref[f])) continue;
            snprintf(path, sizeof path, "%s/%c%llu.png", dir, "gmhr"[k], (unsigned long long)(D.at + (uint64_t)f));
            if (D.fmt[f] == VK_FORMAT_A2B10G10R10_UNORM_PACK32) {   // 10 bits a channel to 8, in place
                uint32_t *q = (uint32_t *)((uint8_t *)D.map + (size_t)D.stride * f + px * (size_t)k);
                for (size_t i = 0; i < (size_t)D.w * D.h; i++) {
                    uint32_t v = q[i];
                    q[i] = (v >> 2 & 0xff) | (v >> 12 & 0xff) << 8 | (v >> 22 & 0xff) << 16 | 0xffu << 24;
                }
            }
            rt_write_png(path, (const uint8_t *)D.map + (size_t)D.stride * f + px * (size_t)k, (uint32_t)D.w, (uint32_t)D.h);
        }
    for (int f = 0; f < DUMP_N; f++) if (D.depth[f]) {
        const float *z = (const float *)((const uint8_t *)D.map + (size_t)D.stride * f + px * 4);
        rt_log("fg depth: frame %llu near %.8g far %.8g, samples %.8g %.8g %.8g %.8g %.8g %.8g %.8g %.8g %.8g",
               (unsigned long long)(D.at + f), D.near_[f], D.far_[f], z[0],z[1],z[2],z[3],z[4],z[5],z[6],z[7],z[8]);
        if (D.nv[f]) {
            const VkTmpFgInputs *in = &D.inputs[f];
            const float xy[9][2] = {{.5f,.3f},{.5f,.55f},{.5f,.7f},{.4f,.8f},{.6f,.8f},{.5f,.9f},{.25f,.8f},{.75f,.8f},{.5f,.98f}};
            const int16_t *nv = (const int16_t *)((const uint8_t *)D.map + (size_t)D.stride * f + px * 4 + 64);
            for (int i = 0; i < 9; i++) {
                int rx = (int)(xy[i][0] * in->w), ry = (int)(xy[i][1] * in->h);
                double ux = (rx + .5) / in->w, uy = (ry + .5) / in->h;
                double clip[4] = {ux*2-1,1-uy*2,z[i]*2-1,1}, previous[4] = {0};
                for (int r = 0; r < 4; r++) for (int col = 0; col < 4; col++) previous[r] += in->m[r*4+col] * clip[col];
                if (previous[3] < 1e-7) continue;
                double cx = ((previous[0]/previous[3]*.5+.5)-ux)*D.w;
                double cy = ((.5-previous[1]/previous[3]*.5)-uy)*D.h;
                int gx = (int)((rx+.5-in->jx)*D.w/in->w) / (int)D.grid[f][2];
                int gy = (int)((ry+.5-in->jy)*D.h/in->h) / (int)D.grid[f][2];
                if (gx < 0 || gy < 0 || gx >= (int)D.grid[f][0] || gy >= (int)D.grid[f][1]) continue;
                size_t p = ((size_t)gy*D.grid[f][0]+gx)*2;
                rt_log("fg motion: frame %llu sample %d uv %.3f %.3f camera %.3f %.3f hardware %.3f %.3f (output pixels)",
                    (unsigned long long)(D.at+f),i,ux,uy,cx,cy,nv[p]/32.f,nv[p+1]/32.f);
            }
        }
    }
    for (int f = 0; f < DUMP_N; f++) if (D.nv[f]) {
        snprintf(path, sizeof path, "%s/o%llu.bin", dir, (unsigned long long)(D.at + f));
        FILE *file = fopen(path, "wb");
        if (file) {
            fwrite(D.grid[f], sizeof(uint32_t), 3, file);
            fwrite((const uint8_t *)D.map + (size_t)D.stride * f + px * 4 + 64, 4,
                   (size_t)D.grid[f][0] * D.grid[f][1], file);
            fclose(file);
        }
    }
    for (int f = 0; f < DUMP_N; f++) if (D.raw[f]) {
        const VkTmpFgInputs *in = &D.sdk[f];
        uint32_t size[2] = {in->w,in->h};
        for (int type = 0; type < 2; type++) {
            snprintf(path,sizeof path,"%s/%c%llu.bin",dir,type ? 'v' : 'z',(unsigned long long)(D.at+f));
            FILE *file = fopen(path,"wb");
            if (file) {
                fwrite(size,sizeof(uint32_t),2,file);
                fwrite((const uint8_t*)D.map+(size_t)D.stride*f+px*(size_t)(type ? 6 : 5)+64,4,
                       (size_t)in->w*in->h,file);
                fclose(file);
            }
        }
        rt_log("fg sdk inputs: capture %llu game %llu %ux%u near %.8g far %.8g jitter %.8g %.8g reset %d",
               (unsigned long long)(D.at+f),(unsigned long long)in->frame,in->w,in->h,in->near_,in->far_,in->jx,in->jy,in->reset);
    }
    rt_log("vk: frame generation: game frames %llu-%llu written to %s (interval %.1f ms)", (unsigned long long)D.at,
           (unsigned long long)(D.at + DUMP_N - 1), dir, F.interval / 1e6);
}

// Active generation's xcb chain protects the temporal input ring. Bypass frames have no xcb, so the first one must
// leave a device-side wait before later graphics frames can wrap that ring. This adds no wait to steady generation.
static void bypass_order(void) {
    if (!F.split || !F.fsem || F.fval <= F.bypass_waited) return;
    VkSemaphoreSubmitInfo wait = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = F.fsem,
        .value = F.fval, .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    VkSubmitInfo2 si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .waitSemaphoreInfoCount = 1, .pWaitSemaphoreInfos = &wait};
    vk_queue_lock();
    VkResult r = vkQueueSubmit2(vk_ctx()->gfx_queue, 1, &si, VK_NULL_HANDLE);
    vk_queue_unlock();
    if (r == VK_SUCCESS) F.bypass_waited = F.fval;
    else rt_log("vk: frame generation bypass ordering failed (%d)", (int)r);
}

// Images of the upscaler that DLSS-G and FSR 3.1 read here on the present thread (a frame's motion vectors and depth,
// the picture without the HUD), retired by the replay (a size change, the upscaler turned off): the replay's fences
// say only that its own work is done, and the present thread makes frames for a game frame after the replay has gone
// on (PR 462 review: HL.out and FS[] read without a lock). An image retired at game frame r may be read by the
// generation of the frames before r: once the present thread is at r (vk_fg_game_frame) it is marked with the
// submissions made so far (F.n), and freed once each of those has had its fence waited (SLOTS submissions later,
// vk_fg_frame), or at once when no generation is in flight.
typedef struct Retired {
    void (*fn)(void *);
    void *arg;
    uint64_t frame, mark;
    int marked;
    struct Retired *next;
} Retired;
static PlatMutex retire_lk = PLAT_MUTEX_INIT;
static Retired *retired;

void vk_fg_retire(uint64_t frame, void (*fn)(void *), void *arg) {   // any thread
    plat_mutex_lock(&retire_lk);
    if (!__atomic_load_n(&F.tried, __ATOMIC_ACQUIRE)) {   // frame generation never ran: nothing here reads it
        plat_mutex_unlock(&retire_lk);
        fn(arg);
        return;
    }
    Retired *e = malloc(sizeof *e);
    if (!e) rt_fatal("vk: out of memory");
    *e = (Retired){fn, arg, frame, 0, 0, retired};
    retired = e;
    plat_mutex_unlock(&retire_lk);
}

// The present thread: after the slot's fence wait in vk_fg_frame (idle 0), or with no generation in flight (idle 1;
// a later generation is of a frame at or past the mark, which reads the new images). force: the device's end.
static void retire_drain(int idle, int force) {
    Retired *done = NULL;
    plat_mutex_lock(&retire_lk);
    for (Retired **p = &retired; *p;) {
        Retired *e = *p;
        if (!e->marked && (force || vk_fg_game_frame >= e->frame)) e->marked = 1, e->mark = F.n;
        if (e->marked && (force || idle || F.n >= e->mark + SLOTS)) {
            *p = e->next;
            e->next = done, done = e;
        } else {
            p = &e->next;
        }
    }
    plat_mutex_unlock(&retire_lk);
    int n = 0;
    while (done) {
        Retired *e = done;
        done = e->next;
        e->fn(e->arg);
        free(e);
        n++;
    }
    static int logged;
    if (n && logged < 10)
        logged++, rt_log("vk: frame generation: %d retired images of the upscaler freed at game frame %llu%s", n,
                         (unsigned long long)vk_fg_game_frame, idle ? " (no generation in flight)" : "");
}

// No generation in flight: every slot's fence is signalled (the present thread owns them)
static int fg_idle(void) {
    const VkCtx *c = vk_ctx();
    if (!F.ok || !c) return 1;
    for (int i = 0; i < SLOTS; i++)
        if (F.fence[i] && vkGetFenceStatus(c->device, F.fence[i]) != VK_SUCCESS) return 0;
    return 1;
}

int vk_fg_frame(VkImage src, int sw, int sh, const int crop[4], uint64_t at, VkImage out[VK_FG_MAX], int *w, int *h,
                uint64_t *step) {
    // the upscaler's retired images: before this call's submission, F.n >= mark + SLOTS means every submission made
    // before the mark had its fence waited by an earlier call (each waits the one SLOTS before it)
    if (__atomic_load_n(&retired, __ATOMIC_RELAXED)) retire_drain(fg_idle(), 0);
    // the game's frame interval: an average of the pacer's display times, a hitch left out
    int gap = !F.last_at || at <= F.last_at || at - F.last_at > GAP_NS;
    if (!gap) {
        uint64_t d = at - F.last_at;
        // no cap under GAP_NS: a 50 ms one held 15 fps (66.7 ms) at 50, so at 2x the made frame and the game's went
        // out 25 and 41.7 ms apart instead of 33 and 33 (vis2: "game frame every 50.0 ms")
        if (d < 4000000) d = 4000000;
        F.interval = F.interval ? (F.interval * 7 + d) / 8 : d;
    }
    F.last_at = at;
    int fallback_m;
    int m = mult_choice(&fallback_m);
    if (m < 2) {   // off, or auto with no room between two refreshes: the caller presents the game's picture
        bypass_order();
        if (F.logged_mult >= 2) rt_log("vk: frame generation paused (%s)", ov_movie_playing() ? "a movie plays" : "off");
        F.logged_mult = m;
        F.have_prev = 0;
        return 0;
    }
    // A menu over the upscaled scene (vk_temporal.c vk_tmp_fg_ui_frame): its picture shown for every refresh of the
    // game frame, as a movie's. The next frame with a picture without the HUD starts over (have_prev 0; DLSS-G resets
    // on the skipped frame numbers).
    if (vk_tmp_fg_ui_frame(vk_fg_game_frame)) {
        bypass_order();
        if (!F.ui_paused && F.ui_logged < 20)
            F.ui_logged++, rt_log("vk: frame generation paused at game frame %llu (a menu: no picture without the HUD)",
                                  (unsigned long long)vk_fg_game_frame);
        F.ui_paused = 1, F.ui_frames++;
        F.have_prev = 0;
        return 0;
    }
    if (F.ui_paused && F.ui_logged <= 20)
        rt_log("vk: frame generation back at game frame %llu (%llu menu frames shown as they are)",
               (unsigned long long)vk_fg_game_frame, (unsigned long long)F.ui_frames);
    F.ui_paused = 0;
    VkCtx *c = vk_ctx();
    if (!F.tried) init(c);
    if (!F.ok) return 0;
    if (gap) F.have_prev = 0;
    unsigned slot = F.n++ % SLOTS;
    if (vkWaitForFences(c->device, 1, &F.fence[slot], VK_TRUE, 5000000000ull) != VK_SUCCESS) {
        rt_log("vk: frame generation: fence wait timed out");
        bypass_order();
        return 0;
    }
    VkCommandBuffer cb = F.cb[slot];
    int pictures = F.q_mult[slot];
    if (F.qp && pictures) {   // the last use of this slot: its fence was waited for above
        uint64_t t[2];
        if (vkGetQueryPoolResults(c->device, F.qp, 2 * slot, 2, sizeof t, t, sizeof t[0], VK_QUERY_RESULT_64_BIT) ==
            VK_SUCCESS && t[1] > t[0]) {
            double ms = (double)(t[1] - t[0]) * F.ts_ns / 1e6;
            F.gpu_sum += ms, F.gpu_n++;
            if (ms > F.gpu_max) F.gpu_max = ms;
            if (F.interval && pictures > 1 && ms * 1e6 > (double)F.interval / pictures) F.gpu_over++;
        }
        F.q_mult[slot] = 0;
    }
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(cb, &bi);
    if (F.qp) {
        vkCmdResetQueryPool(cb, F.qp, 2 * slot, 2);
        vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, F.qp, 2 * slot);
    }
    VkCommandBuffer xcb = F.split ? F.xcb[slot] : VK_NULL_HANDLE;
    if (xcb) vkResetCommandBuffer(xcb, 0), vkBeginCommandBuffer(xcb, &bi);
    F.rec_x = xcb;
    // MKW_FG_OCCUPANCY="priority,throttling" (VK_NV_compute_occupancy_priority; "off"): the generation's compute
    // takes fewer of the GPU's SMs beside the game's work. The game waits for its GPU work inside its frame, and a
    // generation of 2.5 ms beside it pushed game frames past 20 ms in proportion to its GPU time (a synthetic load
    // of the same length did too: flow-load3, 15/11/2026)
    if (c->has_occupancy) {
        static PFN_vkCmdSetComputeOccupancyPriorityNV setocc;
        static float op = -1, ot;
        static int ab;   // "ab:priority,throttling": in the MKW_AB blocks B only
        if (op < 0) {
            const char *e = cfg_get("MKW_FG_OCCUPANCY");
            op = 0.25f, ot = 0.0f;
            if (e && !strncmp(e, "ab:", 3)) ab = 1, e += 3;
            if (e && !strcmp(e, "off")) op = 0;
            else if (e) sscanf(e, "%f,%f", &op, &ot);
            setocc = (PFN_vkCmdSetComputeOccupancyPriorityNV)vkGetDeviceProcAddr(c->device, "vkCmdSetComputeOccupancyPriorityNV");
            rt_log("vk: frame generation's compute occupancy priority %.2f, throttling %.2f%s", op, ot, setocc ? "" : " (no entry point)");
        }
        if (setocc && op > 0 && (!ab || rt_ab())) {
            VkComputeOccupancyPriorityParametersNV pp = {.sType = VK_STRUCTURE_TYPE_COMPUTE_OCCUPANCY_PRIORITY_PARAMETERS_NV,
                                                         .occupancyPriority = op, .occupancyThrottling = ot};
            setocc(cb, &pp);
        }
    }
    int n = fg_record(cb, src, sw, sh, crop, m - 1, fallback_m - 1, out, w, h);
    F.rec_x = VK_NULL_HANDLE;
    if (xcb) vkEndCommandBuffer(xcb);
    if (F.qp) {
        vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, F.qp, 2 * slot + 1);
        F.q_mult[slot] = n;
    }
    int dump = n && dump_record(cb, out, n, *w, *h, F.compare_native);
    vkEndCommandBuffer(cb);
    if (!n) {   // recorded nothing; preserve ordering when normal graphics resumes without a copy submission
        bypass_order();
        return 0;
    }
    vkResetFences(c->device, 1, &F.fence[slot]);
    VkCommandBufferSubmitInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = cb};
    VkSubmitInfo2 si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = 1, .pCommandBufferInfos = &cbi};
    VkResult r;
    if (xcb) {   // the copies on the game's queue after the last generation; the rest on fg_queue after the copies
        VkCommandBufferSubmitInfo xi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = xcb};
        VkSemaphoreSubmitInfo xw = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = F.fsem, .value = F.fval,
                                    .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT},
                              xs = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = F.xsem,
                                    .value = F.xval + 1, .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT},
                              fw = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = F.xsem,
                                    .value = F.xval + 1, .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT},
                              fs = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = F.fsem,
                                    .value = F.fval + 1, .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
        static int xwait = -1;   // MKW_FG_XWAIT=0 (measuring only, unsafe): the copies do not wait for the last generation
        if (xwait < 0) xwait = !(cfg_get("MKW_FG_XWAIT") && !strcmp(cfg_get("MKW_FG_XWAIT"), "0"));
        VkSubmitInfo2 xsi = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .waitSemaphoreInfoCount = F.fval && xwait ? 1u : 0u,
                             .pWaitSemaphoreInfos = &xw, .commandBufferInfoCount = 1, .pCommandBufferInfos = &xi,
                             .signalSemaphoreInfoCount = 1, .pSignalSemaphoreInfos = &xs};
        VkSemaphoreSubmitInfo waits[2] = {fw};
        si.waitSemaphoreInfoCount = 1, si.pWaitSemaphoreInfos = waits;
        si.signalSemaphoreInfoCount = 1, si.pSignalSemaphoreInfos = &fs;
        vk_queue_lock();
        r = vkQueueSubmit2(c->gfx_queue, 1, &xsi, VK_NULL_HANDLE);
        if (r == VK_SUCCESS) {
            F.xval++;
            if (F.of_active) {
                if (!vk_of_submit(F.xsem, F.xval, &F.of_result)) rt_fatal("vk: hardware optical-flow submit failed");
                waits[1] = (VkSemaphoreSubmitInfo){.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                    .semaphore = F.of_result.ready, .value = F.of_result.value,
                    .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
                si.waitSemaphoreInfoCount = 2;
            }
            r = vkQueueSubmit2(c->fg_queue, 1, &si, F.fence[slot]);
        }
        if (r == VK_SUCCESS) F.fval++;
        vk_queue_unlock();
    } else {
        vk_queue_lock();
        r = vkQueueSubmit2(c->gfx_queue, 1, &si, F.fence[slot]);
        vk_queue_unlock();
    }
    if (r != VK_SUCCESS) {
        rt_log("vk: frame generation: submit failed (%d)", (int)r);
        F.have_prev = 0;
        return 0;
    }
    if (dump) dump_write(F.fence[slot]);
    // the step from the game's interval in whole vsync ticks (the average lags ~8 frames behind a change of rate;
    // under the turbo frame generation is off)
    *step = (uint64_t)(game_ticks() * (1e9 / frame_logic_hz())) / (uint64_t)n;
    F.frames++, F.made_total += (uint64_t)(n - 1);
    if (n != F.logged_mult || F.frames % (cfg_get("MKW_FG_LOG_EVERY") && atoll(cfg_get("MKW_FG_LOG_EVERY")) > 0 ? (uint64_t)atoll(cfg_get("MKW_FG_LOG_EVERY")) : 1800) == 0) {   // ~1 min at 30 fps
        F.logged_mult = n;
        rt_log("vk: frame generation %dx: game frame every %.1f ms, %llu frames made over %llu game frames; GPU per game "
               "frame %.2f ms mean, %.2f max, %u longer than a step (%.1f ms); without the HUD %llu, HUD seen %llu", n,
               F.interval / 1e6, (unsigned long long)F.made_total, (unsigned long long)F.frames,
               F.gpu_n ? F.gpu_sum / F.gpu_n : 0.0, F.gpu_max, F.gpu_over, n ? F.interval / 1e6 / n : 0.0,
               (unsigned long long)F.hl_given, (unsigned long long)F.hl_missing);
        if (F.hud_frames)
            rt_log("vk: frame generation: the game's HUD put over the made frames of %llu game frames",
                   (unsigned long long)F.hud_frames);
        F.gpu_sum = F.gpu_max = 0, F.gpu_n = F.gpu_over = 0;
    }
    return n;
}

VkQueue vk_fg_queue(void) { return F.split ? vk_ctx()->fg_queue : VK_NULL_HANDLE; }

VkImage vk_fg_flow(int level, int *gw, int *gh) {
    *gw = (int)grid(F.w), *gh = (int)grid(F.h);
    if (level < 2) *gw = (*gw + 1) / 2, *gh = (*gh + 1) / 2;
    return level >= 0 && level < 6 ? F.fl[level].img : VK_NULL_HANDLE;
}

void vk_fg_release(void) {
    VkCtx *c = vk_ctx();
    retire_drain(1, 1);   // at the device's end, after its wait for idle
    if (!c || !F.tried) return;
    vk_of_release();
    if (D.buf) vmaDestroyBuffer(c->vma, D.buf, D.alloc);
    memset(&D, 0, sizeof D);
    free_images(c);
    VkPipeline ps[] = {F.luma, F.coarse, F.fine, F.full, F.median, F.synth, F.fsr_mvp, F.of_mvp};
    for (size_t i = 0; i < sizeof ps / sizeof *ps; i++)
        if (ps[i]) vkDestroyPipeline(c->device, ps[i], NULL);
    for (int i = 0; i < SLOTS; i++)
        if (F.fence[i]) vkDestroyFence(c->device, F.fence[i], NULL);
    if (F.qp) vkDestroyQueryPool(c->device, F.qp, NULL);
    if (F.xsem) vkDestroySemaphore(c->device, F.xsem, NULL);
    if (F.fsem) vkDestroySemaphore(c->device, F.fsem, NULL);
    if (F.hud) vkDestroyPipeline(c->device, F.hud, NULL);
    if (F.hlayout) vkDestroyPipelineLayout(c->device, F.hlayout, NULL);
    if (F.hdsl) vkDestroyDescriptorSetLayout(c->device, F.hdsl, NULL);
    if (F.pool) vkDestroyCommandPool(c->device, F.pool, NULL);
    if (F.sampler) vkDestroySampler(c->device, F.sampler, NULL);
    if (F.fsr_layout) vkDestroyPipelineLayout(c->device, F.fsr_layout, NULL);
    if (F.of_layout) vkDestroyPipelineLayout(c->device, F.of_layout, NULL);
    if (F.layout) vkDestroyPipelineLayout(c->device, F.layout, NULL);
    if (F.fsr_dsl) vkDestroyDescriptorSetLayout(c->device, F.fsr_dsl, NULL);
    if (F.of_dsl) vkDestroyDescriptorSetLayout(c->device, F.of_dsl, NULL);
    if (F.dsl) vkDestroyDescriptorSetLayout(c->device, F.dsl, NULL);
    memset(&F, 0, sizeof F);
}
