// --probe-gpu-clocks: does this GPU's driver lower the clock when the GPU is busy only part of each frame, as the race's
// chain leaves it (record while the GPU waits, then the GPU runs while the game waits; gfx/vk/device/vk_pstate.c)?
// A fixed compute batch of ~MKW_CLK_TARGET_MS (default 8) ms, then the CPU spins a gap of 0 to 50 ms, repeated for
// MKW_CLK_SECS (default 3) s per gap. The batch's work never changes, so its GPU time (timestamps) only changes with the
// clock: a batch slower after a gap is a clock the driver lowered during it. On amdgpu the core clock at the start of
// each batch is read from sysfs too. It runs with the runtime's own device, so MKW_GPU_CLOCKS applies (peak, off):
// the same probe shows what holding the clock does. MKW_CLK_FILL=0.25 keeps the GPU busy that share of each gap with
// ~1 ms batches (whether a filler would hold the clock where no driver op exists, as on Windows).
// Measured 17/11/2026: Radeon 660M (RADV) starts each batch at 400-533 of 1900 MHz after any gap of 8 ms or more,
// +7 to +42 % a batch (median), 8.0 ms flat with MKW_GPU_CLOCKS=peak or MKW_CLK_FILL=0.25 (0.1 is not enough);
// GTX 1050 Ti (Windows) +0 to +14 %.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "vk.h"
#include "platform/platform.h"
#include "../gfx/vk/spv/clk_busy_spv.h"

#define GROUPS 4096
#define MAX_RUNS 20000

static struct {
    VkCtx *c;
    VkCommandBuffer cb;
    VkPipeline pipe;
    VkPipelineLayout layout;
    VkDescriptorSet set;
    VkQueryPool qp;
    VkFence fence;
    double tick_ms;
} P;

#if defined(__linux__)
#include <glob.h>
// amdgpu's current core clock level (the line with '*' of pp_dpm_sclk), -1 elsewhere
static int sclk_mhz(void) {
    static char path[256];
    if (!path[0]) {
        glob_t g;
        if (glob("/sys/class/drm/card*/device/pp_dpm_sclk", 0, NULL, &g) || !g.gl_pathc) { path[0] = 1; return -1; }
        snprintf(path, sizeof path, "%s", g.gl_pathv[0]);
        globfree(&g);
    }
    if (path[0] == 1) return -1;
    FILE *f = fopen(path, "r");
    char line[64];
    int mhz = -1, v;
    while (f && fgets(line, sizeof line, f))
        if (strchr(line, '*') && sscanf(line, "%*d: %dMhz", &v) == 1) mhz = v;
    if (f) fclose(f);
    return mhz;
}
#else
static int sclk_mhz(void) { return -1; }
#endif

static double now_ms(void) { return plat_now_ns() / 1e6; }

static int setup(void) {
    VkCtx *c = P.c;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 4096,
                             .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkBuffer buf;
    VmaAllocation alloc;
    if (vmaCreateBuffer(c->vma, &bi, &ai, &buf, &alloc, NULL) != VK_SUCCESS) return 0;
    VkDescriptorSetLayoutBinding lb = {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                       .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                          .bindingCount = 1, .pBindings = &lb};
    VkDescriptorSetLayout dsl;
    if (vkCreateDescriptorSetLayout(c->device, &li, NULL, &dsl) != VK_SUCCESS) return 0;
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 4};
    VkPipelineLayoutCreateInfo pli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                      .pSetLayouts = &dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    if (vkCreatePipelineLayout(c->device, &pli, NULL, &P.layout) != VK_SUCCESS) return 0;
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof clk_busy_spv,
                                   .pCode = clk_busy_spv};
    VkShaderModule m;
    if (vkCreateShaderModule(c->device, &mi, NULL, &m) != VK_SUCCESS) return 0;
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"},
                                      .layout = P.layout};
    VkResult r = vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &ci, NULL, &P.pipe);
    vkDestroyShaderModule(c->device, m, NULL);
    if (r != VK_SUCCESS) return 0;
    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
    VkDescriptorPoolCreateInfo dpi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1,
                                      .poolSizeCount = 1, .pPoolSizes = &ps};
    VkDescriptorPool dp;
    if (vkCreateDescriptorPool(c->device, &dpi, NULL, &dp) != VK_SUCCESS) return 0;
    VkDescriptorSetAllocateInfo dai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dp,
                                       .descriptorSetCount = 1, .pSetLayouts = &dsl};
    if (vkAllocateDescriptorSets(c->device, &dai, &P.set) != VK_SUCCESS) return 0;
    VkDescriptorBufferInfo dbi = {buf, 0, 4096};
    VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = P.set, .descriptorCount = 1,
                              .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &dbi};
    vkUpdateDescriptorSets(c->device, 1, &w, 0, NULL);
    VkCommandPoolCreateInfo cpi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                   .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                   .queueFamilyIndex = c->gfx_family};
    VkCommandPool cp;
    if (vkCreateCommandPool(c->device, &cpi, NULL, &cp) != VK_SUCCESS) return 0;
    VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cp,
                                       .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    if (vkAllocateCommandBuffers(c->device, &cai, &P.cb) != VK_SUCCESS) return 0;
    VkQueryPoolCreateInfo qpi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                 .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 2};
    if (vkCreateQueryPool(c->device, &qpi, NULL, &P.qp) != VK_SUCCESS) return 0;
    VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vkCreateFence(c->device, &fci, NULL, &P.fence) != VK_SUCCESS) return 0;
    P.tick_ms = c->props.limits.timestampPeriod / 1e6;
    return 1;
}

// One submit of the fixed work, waited for; its GPU time in ms, or -1
static double batch(uint32_t iters) {
    vkResetCommandBuffer(P.cb, 0);
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(P.cb, &bi);
    vkCmdResetQueryPool(P.cb, P.qp, 0, 2);
    vkCmdWriteTimestamp(P.cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, P.qp, 0);
    vkCmdBindPipeline(P.cb, VK_PIPELINE_BIND_POINT_COMPUTE, P.pipe);
    vkCmdBindDescriptorSets(P.cb, VK_PIPELINE_BIND_POINT_COMPUTE, P.layout, 0, 1, &P.set, 0, NULL);
    vkCmdPushConstants(P.cb, P.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &iters);
    vkCmdDispatch(P.cb, GROUPS, 1, 1);
    vkCmdWriteTimestamp(P.cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, P.qp, 1);
    vkEndCommandBuffer(P.cb);
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &P.cb};
    vkResetFences(P.c->device, 1, &P.fence);
    if (vkQueueSubmit(P.c->gfx_queue, 1, &si, P.fence) != VK_SUCCESS) return -1;
    if (vkWaitForFences(P.c->device, 1, &P.fence, VK_TRUE, 5000000000ull) != VK_SUCCESS) return -1;
    uint64_t t[2];
    if (vkGetQueryPoolResults(P.c->device, P.qp, 0, 2, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT))
        return -1;
    return (double)(t[1] - t[0]) * P.tick_ms;
}

static int by_ms(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y); }
static int by_mhz(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

static double cfg_num(const char *name, double def) {
    const char *e = cfg_get(name);
    return e && *e ? atof(e) : def;
}

int probe_gpu_clocks(void) {
    double target = cfg_num("MKW_CLK_TARGET_MS", 8), secs = cfg_num("MKW_CLK_SECS", 3), fill = cfg_num("MKW_CLK_FILL", 0);
    if (!vk_init()) { printf("probe-gpu-clocks: FAIL vk_init\n"); return 1; }
    P.c = vk_ctx();
    if (!setup()) { printf("probe-gpu-clocks: FAIL pipeline\n"); return 1; }
    printf("probe-gpu-clocks: %s (vendor 0x%04x, driver 0x%x), MKW_GPU_CLOCKS=%s, filler %.0f %% of each gap\n",
           P.c->props.deviceName, P.c->props.vendorID, P.c->props.driverVersion,
           cfg_get("MKW_GPU_CLOCKS") ? cfg_get("MKW_GPU_CLOCKS") : "auto", fill * 100);
    // Calibration back to back (2 s, the clock ramps up): iterations for ~target ms
    uint32_t iters = 1024;
    double g = 0;
    for (double t0 = now_ms(); now_ms() - t0 < 2000;) {
        if ((g = batch(iters)) < 0) { printf("probe-gpu-clocks: FAIL submit\n"); return 1; }
        if (g > 0.05) iters = (uint32_t)(iters * (target / g) * 0.5 + iters * 0.5);
        if (iters < 16) iters = 16;
    }
    printf("probe-gpu-clocks: %u iterations, %.2f ms a batch back to back\n", iters, g);
    printf("probe-gpu-clocks: %7s %6s %8s %8s %8s %6s %9s\n", "gap_ms", "n", "gpu_p50", "gpu_p10", "gpu_p90", "busy", "sclk_p50");
    static const double gaps[] = {0, 16, 33, 0, 8, 50, 25, 0};
    static double gs[MAX_RUNS];
    static int ck[MAX_RUNS];
    double base = 0, worst = 0;
    for (size_t k = 0; k < sizeof gaps / sizeof *gaps; k++) {
        int n = 0, nc = 0;
        double t1 = now_ms(), busy = 0;
        while (now_ms() - t1 < secs * 1000 && n < MAX_RUNS) {
            int mhz = sclk_mhz();   // after the gap: the clock this batch starts at
            if (mhz > 0) ck[nc++] = mhz;
            double gt = batch(iters);
            if (gt < 0) { printf("probe-gpu-clocks: FAIL submit\n"); return 1; }
            gs[n++] = gt;
            busy += gt;
            for (double s = now_ms(); now_ms() - s < gaps[k];) {   // the CPU busy, as the recorder would be
                if (fill <= 0) continue;
                batch(iters / 8 + 1);
                for (double f0 = now_ms(); now_ms() - f0 < 1.0 / fill - 1 && now_ms() - s < gaps[k];) {}
            }
        }
        qsort(gs, (size_t)n, sizeof *gs, by_ms);
        qsort(ck, (size_t)nc, sizeof *ck, by_mhz);
        double p50 = gs[n / 2];
        if (gaps[k] == 0) base = base ? (base < p50 ? base : p50) : p50;
        else if (base && p50 / base - 1 > worst) worst = p50 / base - 1;
        printf("probe-gpu-clocks: %7.0f %6d %8.2f %8.2f %8.2f %5.0f%% %9d\n", gaps[k], n, p50, gs[n / 10],
               gs[n * 9 / 10], 100 * busy / (now_ms() - t1), nc ? ck[nc / 2] : -1);
        fflush(stdout);
    }
    printf("probe-gpu-clocks: a batch after a gap is up to %+.0f %% slower (median): %s\n", worst * 100,
           worst > 0.05 ? "the driver lowers the clock while the GPU waits" : "the clock holds");
    // No device teardown, as --probe-framegen: the process ends here
    return 0;
}
