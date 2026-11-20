// --probe-oflow: independent sign/scale check on synthetic translation, without game assets or a window.
// CPU waits deliberately separate upload/luma, optical flow and readback: this is a correctness probe, not a benchmark.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host.h"
#include "cfg.h"
#include "vk.h"
#include "vk_oflow.h"
#include "vk_rec.h"

#define W 256
#define H 128
#define CHECK(x) do { if (!(x)) { printf("probe-oflow: FAILED line %d: %s\n", __LINE__, #x); return 1; } } while (0)

static int submit_wait(VkCtx *c, VkCommandBuffer cb, VkFence fence, VkSemaphore ready, uint64_t value) {
    if (vkEndCommandBuffer(cb) != VK_SUCCESS) return 0;
    VkCommandBufferSubmitInfo ci = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = cb};
    VkSubmitInfo2 si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = 1, .pCommandBufferInfos = &ci};
    VkSemaphoreSubmitInfo wait = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = ready,
        .value = value, .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    if (ready) si.waitSemaphoreInfoCount = 1, si.pWaitSemaphoreInfos = &wait;
    vk_queue_lock();
    VkResult r = vkQueueSubmit2(c->gfx_queue, 1, &si, fence);
    vk_queue_unlock();
    return r == VK_SUCCESS && vkWaitForFences(c->device, 1, &fence, VK_TRUE, 5000000000ull) == VK_SUCCESS;
}

static uint8_t pattern(int x, int y) {
    // Smooth, nonperiodic two-dimensional texture: uncorrelated single-pixel noise aliases the hardware's pyramid.
    double f = 128.0 + 35.0 * sin(x * .13 + y * .031) + 33.0 * cos(y * .19 + x * .017)
                    + 28.0 * sin(x * .071 - y * .11) + 20.0 * cos(x * .23 + y * .29);
    return (uint8_t)f;
}

int probe_oflow(void) {
    CHECK(vk_init());
    VkCtx *c = vk_ctx();
    printf("probe-oflow: Vulkan validation active %d\n", c->validation);
    if (cfg_bool("MKW_VK_VALIDATION")) CHECK(c->validation);
    CHECK(c->has_oflow && c->of_queue);
    CHECK(vk_of_init(c->of_queue, c->of_family));
    int recording = rec_suspend();   // the module pushes descriptors directly, as do temporal SDK calls
    VkBuffer staging;
    VmaAllocation allocation;
    VmaAllocationInfo map;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = W * H * 12,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
        .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT};
    CHECK(vmaCreateBuffer(c->vma, &bi, &ai, &staging, &allocation, &map) == VK_SUCCESS);
    uint8_t *bytes = map.pMappedData;
    for (int i = 0; i < 2; i++) for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        uint8_t v = pattern(x - i * 4, y + i * 2);
        size_t p = ((size_t)i * W * H + y * W + x) * 4;
        bytes[p] = bytes[p+1] = bytes[p+2] = v, bytes[p+3] = 255;
    }
    CHECK(vmaFlushAllocation(c->vma, allocation, 0, W * H * 8) == VK_SUCCESS);
    VkImage images[2]; VkImageView views[2]; VmaAllocation mem[2];
    for (int i = 0; i < 2; i++) {
        VkImageCreateInfo image = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
            .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {W,H,1}, .mipLevels = 1, .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT};
        VmaAllocationCreateInfo device = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        CHECK(vmaCreateImage(c->vma, &image, &device, &images[i], &mem[i], NULL) == VK_SUCCESS);
        VkImageViewCreateInfo view = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = images[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = image.format,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
        CHECK(vkCreateImageView(c->device, &view, NULL, &views[i]) == VK_SUCCESS);
    }
    VkCommandPool pool;
    VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = c->gfx_family};
    CHECK(vkCreateCommandPool(c->device, &pi, NULL, &pool) == VK_SUCCESS);
    VkCommandBuffer cb;
    VkCommandBufferAllocateInfo ca = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    CHECK(vkAllocateCommandBuffers(c->device, &ca, &cb) == VK_SUCCESS);
    VkFence fence;
    VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    CHECK(vkCreateFence(c->device, &fi, NULL, &fence) == VK_SUCCESS);
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    CHECK(vkBeginCommandBuffer(cb, &begin) == VK_SUCCESS);
    for (int i = 0; i < 2; i++) {
        VkImageMemoryBarrier2 barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = images[i], .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
        VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &barrier};
        vkCmdPipelineBarrier2(cb, &di);
        VkBufferImageCopy copy = {.bufferOffset = (VkDeviceSize)i * W * H * 4,
            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}, .imageExtent = {W,H,1}};
        vkCmdCopyBufferToImage(cb, staging, images[i], VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
    }
    VkOfResult result;
    CHECK(vk_of_prepare(cb, views[0], views[1], W, H, 1, &result));
    CHECK(submit_wait(c, cb, fence, VK_NULL_HANDLE, 0));
    CHECK(vk_of_submit(VK_NULL_HANDLE, 0, &result));
    VkSemaphoreWaitInfo wait = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1,
        .pSemaphores = &result.ready, .pValues = &result.value};
    CHECK(vkWaitSemaphores(c->device, &wait, 5000000000ull) == VK_SUCCESS);
    CHECK(vkResetCommandBuffer(cb, 0) == VK_SUCCESS);
    CHECK(vkResetFences(c->device, 1, &fence) == VK_SUCCESS);
    CHECK(vkBeginCommandBuffer(cb, &begin) == VK_SUCCESS);
    VkBufferCopy copy = {.dstOffset = W * H * 8, .size = (VkDeviceSize)result.wblocks * result.hblocks * 4};
    vkCmdCopyBuffer(cb, result.vectors, staging, 1, &copy);
    VkMemoryBarrier2 host = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT, .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT, .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &host};
    vkCmdPipelineBarrier2(cb, &di);
    CHECK(submit_wait(c, cb, fence, result.ready, result.value));
    CHECK(vmaInvalidateAllocation(c->vma, allocation, 0, VK_WHOLE_SIZE) == VK_SUCCESS);
    const int16_t *flow = (const int16_t *)(bytes + W * H * 8);
    int good = 0, total = 0; double sum = 0, worst = 0;
    for (uint32_t y = 0; y < result.hblocks; y++) for (uint32_t x = 0; x < result.wblocks; x++) {
        if (x * result.grid < 16 || x * result.grid >= W - 16 || y * result.grid < 16 || y * result.grid >= H - 16) continue;
        size_t p = ((size_t)y * result.wblocks + x) * 2;
        double e = fmax(fabs(flow[p] / 32.0 + 4.0), fabs(flow[p+1] / 32.0 - 2.0));
        good += e <= 1.0; total++; sum += e; if (e > worst) worst = e;
    }
    printf("probe-oflow: expected (-4,+2), grid %u; %d/%d within 1 px, mean error %.4f max %.4f; validation errors %ld\n",
           result.grid, good, total, total ? sum / total : 0, worst, vk_validation_errors_total());
    int ok = total && good >= total * .95 && !vk_validation_errors_total();
    vk_of_release();
    vkDestroyFence(c->device, fence, NULL); vkDestroyCommandPool(c->device, pool, NULL);
    for (int i = 0; i < 2; i++) { vkDestroyImageView(c->device, views[i], NULL); vmaDestroyImage(c->vma, images[i], mem[i]); }
    vmaDestroyBuffer(c->vma, staging, allocation);
    rec_resume(recording);
    printf("probe-oflow: %s\n", ok ? "passed" : "FAILED");
    return ok ? 0 : 1;   // as fg_probe, no global vk_shutdown/VMA teardown of unrelated runtime pools
}
