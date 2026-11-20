// Native NVIDIA Reflex support through VK_NV_low_latency2.
//
// This module owns only Reflex calls and the timeline semaphore used by Reflex sleep. The Vulkan back end still
// creates the device and swapchain, enables VK_NV_low_latency2 and the Vulkan timelineSemaphore feature, and
// destroys every swapchain only after vk_reflex_swapchain_detach returns.
#pragma once
#include <stdint.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VkReflex VkReflex;
typedef struct VkReflexSwapchain VkReflexSwapchain;

typedef enum VkReflexMode {
    VK_REFLEX_OFF = 0,
    VK_REFLEX_ON = 1,
    VK_REFLEX_BOOST = 2,
} VkReflexMode;

typedef enum VkReflexSleepResult {
    VK_REFLEX_SLEEP_OK = 0,
    VK_REFLEX_SLEEP_UNAVAILABLE,
    VK_REFLEX_SLEEP_BUSY,
    VK_REFLEX_SLEEP_TIMEOUT,
    VK_REFLEX_SLEEP_ERROR,
} VkReflexSleepResult;

// enabled_spec_version is the VkExtensionProperties::specVersion selected and enabled with the device. This module
// never enables an extension. NULL means no usable backend; create logs the missing entry points and revision.
VkReflex *vk_reflex_create(VkDevice device, uint32_t enabled_spec_version);
void vk_reflex_destroy(VkReflex *reflex);
int vk_reflex_available(const VkReflex *reflex);

// Before vkCreateSwapchainKHR, pass its current pNext chain as next. When Reflex is available and allow_low_latency is
// nonzero, this fills storage and returns it for the create-info pNext; otherwise it returns next unchanged. Pass true
// whenever the extension is available if the mode may later change from off to on, even when it starts off.
const void *vk_reflex_swapchain_pnext(VkReflex *reflex, VkSwapchainLatencyCreateInfoNV *storage, const void *next,
                                      int allow_low_latency);

// Attach after vkCreateSwapchainKHR succeeds. The returned handle is stable through reflex shutdown. All calls that
// take it become safe no-ops after detach starts. detach waits for calls already in flight, then disables low latency
// and releases the sleep semaphore before it returns, so it MUST precede vkDestroySwapchainKHR.
VkReflexSwapchain *vk_reflex_swapchain_attach(VkReflex *reflex, VkSwapchainKHR swapchain);
void vk_reflex_swapchain_detach(VkReflex *reflex, VkReflexSwapchain *swapchain);

// Switches the live swapchain between off, low-latency, and boost modes. minimum_interval_us is passed to the
// driver and may pace presents; zero asks the driver for its latency mode without an added rate limit.
VkResult vk_reflex_set_mode(VkReflex *reflex, VkReflexSwapchain *swapchain, VkReflexMode mode,
                            uint32_t minimum_interval_us);

// Call exactly once per application-rendered frame, after the previous present and before input sampling. Invoke this
// only from the one game/simulation thread. The module gives up its virtual core only around vkWaitSemaphores and
// requests a 100 ms Vulkan timeout. A timeout is a normal result after which this frame continues without more delay.
VkReflexSleepResult vk_reflex_sleep(VkReflex *reflex, VkReflexSwapchain *swapchain);

// app_frame_id identifies the application-rendered frame. It is deliberately unrelated to VkPresentIdKHR and must
// remain the same across simulation, all source submissions/markers, and present. Generated frames inherit the ID of
// the source frame and never get their own application-frame ID.
void vk_reflex_marker(VkReflex *reflex, VkReflexSwapchain *swapchain, VkLatencyMarkerNV marker,
                      uint64_t app_frame_id);

// Prepends VkLatencySubmissionPresentIdNV to a vkQueueSubmit/vkQueueSubmit2 pNext chain only when
// VK_NV_low_latency2 revision 3+ is enabled. storage must outlive the submit call. Capture app_frame_id when work is
// enqueued; do not read a mutable latest-game-frame value on the replay thread. Nonzero IDs must be monotonic on each
// queue. Once explicit IDs are used, tag every later queue submission on the device so it cannot inherit a stale ID.
int vk_reflex_chain_submission(VkReflex *reflex, uint64_t app_frame_id, VkLatencySubmissionPresentIdNV *storage,
                               const void **p_next);

// The lock of whoever acquires and presents on the swapchain: every call that names it holds it (external sync)
void vk_reflex_set_external_lock(void (*lock)(void), void (*unlock)(void));

#ifdef __cplusplus
}
#endif
