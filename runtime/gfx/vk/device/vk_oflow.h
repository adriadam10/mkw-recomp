// Native Vulkan optical-flow module for frame generation (VK_NV_optical_flow).
// The caller owns the queue lock protocol and keeps one game-frame pair in flight at a time.
#pragma once
#include <vulkan/vulkan.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VkOfResult {
    VkBuffer vectors;       // packed 2x16 signed fixed-point vectors, row-major, 4 bytes per block
    uint32_t wblocks;
    uint32_t hblocks;
    uint32_t grid;          // pixels per flow block (prefers 4, then 2, then 1)
    VkSemaphore ready;      // private timeline semaphore, signalled after the copy to vectors
    uint64_t value;         // timeline value; zero until vk_of_submit succeeds
} VkOfResult;

// Initializes the device-side functions, format/grid support, luma pipeline and command ring.
// of_queue must belong to of_family and include optical-flow and transfer queue bits. Luma compute
// stays on the graphics queue.
int vk_of_init(VkQueue of_queue, uint32_t of_family);

// Records, but does not submit, the R8 luma conversion for previous/current into graphics_copies.
// Both source views must be final resolved RGBA pictures in GENERAL layout when XC executes.
// previous is REFERENCE and current is INPUT, producing current->previous flow. reset disables the
// session's temporal hints for this pair. A successful prepare must have its XC and OF submission
// ordered as described for vk_of_submit; do not call prepare again before submit or failure cleanup.
int vk_of_prepare(VkCommandBuffer graphics_copies, VkImageView previous, VkImageView current, uint32_t w, uint32_t h,
                  int reset, VkOfResult *out);

// Submits the prepared OF work after graphics_copies has been submitted. The OF submission waits
// graphics_ready >= ready_value, executes NV optical flow, copies its SFIXED5 image to vectors, and
// signals out->ready at out->value. This function never waits for or reads back GPU results.
int vk_of_submit(VkSemaphore graphics_ready, uint64_t ready_value, VkOfResult *out);

// Waits for the device while holding the shared queue lock, then destroys all module objects.
void vk_of_release(void);

#ifdef __cplusplus
}
#endif
