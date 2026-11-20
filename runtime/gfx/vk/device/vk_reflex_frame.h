// NVIDIA Reflex for MKW (vk_reflex_frame.c): the runtime's side of vk_reflex.c. MKW_REFLEX=off|on|boost, live.
#pragma once
#include <stdint.h>
#include <vulkan/vulkan.h>

void vk_rfx_init(uint32_t spec_version);   // after the device, with VK_NV_low_latency2's enabled revision (0: none)
int vk_rfx_available(void);
int vk_rfx_menu_state(void);   // the menu's note under MKW_REFLEX: 0 none, 1 after a restart, 2 not available
const void *vk_rfx_swapchain_pnext(VkSwapchainLatencyCreateInfoNV *storage, const void *next);
void vk_rfx_swapchain_created(VkSwapchainKHR sc);
void vk_rfx_swapchain_destroying(VkSwapchainKHR sc);   // before vkDestroySwapchainKHR
void vk_rfx_frame_start(uint64_t id);   // game thread, before the frame update: the sleep and the simulation's start
void vk_rfx_frame_end(void);            // game thread, after it
void vk_rfx_submitted(uint64_t id);     // replay thread: the frame's work is recorded
uint64_t vk_rfx_present_start(uint64_t id);   // present thread: the VkPresentIdKHR to present the game frame with, 0 none
void vk_rfx_present_end(uint64_t id);
uint64_t vk_rfx_oob_present_start(uint64_t id);   // before a generated present: marker id, or 0 if inactive
void vk_rfx_oob_present_end(uint64_t id);     // immediately after it
void vk_rfx_shutdown(void);   // returns only after admitted driver calls leave; call before vkDestroyDevice
uint64_t vk_be_next_frame_id(void);     // vk_backend.c: the id of the frame the game thread is about to simulate
