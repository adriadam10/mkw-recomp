// Whether this machine has a GPU the game can run on, by the rules of vk_device.c pick_device, asked before the game:
// the start screen (ui/launcher/launcher.cpp) says it before the setup and Play, instead of the game's window closing
// at its first frame ("no usable Vulkan device"). C and C++.
#pragma once
#include <stdint.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { VK_GPU_REPORT_MAX = 8 };

typedef enum {
    VK_GPU_OK,         // the game can run on it
    VK_GPU_OLD,        // its driver offers Vulkan below 1.3
    VK_GPU_EXT,        // its driver lacks a required extension (ext)
    VK_GPU_QUEUE,      // no queue family does graphics and compute
    VK_GPU_SKIPPED,    // MKW_GPU_INDEX or MKW_GPU_NAME names another
} VkGpuWhy;

typedef struct {
    char name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
    char driver[VK_MAX_DRIVER_NAME_SIZE + VK_MAX_DRIVER_INFO_SIZE + 2];   // "radv Mesa 20.3.5"; empty below Vulkan 1.2
    uint32_t api;                                                          // VK_MAKE_API_VERSION
    VkPhysicalDeviceType type;
    VkGpuWhy why;
    const char *ext;                                                       // VK_GPU_EXT: the first one missing
} VkGpuInfo;

typedef struct {
    int n;      // devices listed (the first VK_GPU_REPORT_MAX); 0: no Vulkan driver at all
    int best;   // the one the game would use, or -1
    VkGpuInfo gpu[VK_GPU_REPORT_MAX];
} VkGpuReport;

// The devices of inst and the one pick_device would take, with MKW_GPU_INDEX and MKW_GPU_NAME as it reads them. The
// window's present support is not asked (no window yet): a device that does graphics and compute and cannot present
// is not known to exist.
void vk_gpu_report(VkInstance inst, VkGpuReport *r);

#ifdef __cplusplus
}
#endif
