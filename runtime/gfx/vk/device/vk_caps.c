// What the runtime branches on, logged once at device creation, and the fallbacks for GPUs other than the RX 9070.
//
// Fallbacks (each also forced on any GPU by an environment switch, to test it where the GPU does not need it):
//   - no shaderFloat64 (MKW_VK_NO_FLOAT64=1 clears the feature before vkCreateDevice): the translator lowers FP64 to
//     FP32 ("softfloat64"), so no shader declares Float64.
//   - minStorageBufferOffsetAlignment above 4 (MKW_VK_SSBO_ALIGN=<power of two> raises it): the translator computes
//     storage offsets from the guest address rounded down to that alignment (Ryujinx's unaligned storage path), and
//     vk_exec_bind.c ssbo_info binds the rounded-down address, so shader writes reach guest memory instead of a copy.
//   - no graphics pipeline library with fast linking (MKW_GPL=0 forces it): vk_caps.gpl = 0, the extension stays
//     disabled, and pipelines are built whole.
//   - no 32-lane pin (no subgroupSizeControl, 32 outside min..max, or MKW_VK_SUBGROUP_PIN=0): stages run at the
//     device's subgroupSize (SPIR-V 1.3 without a required size) while the shaders still assume 32 lanes (the setup
//     translates them once). At 64 on RADV the race differs in 1 846 pixels of 3.7 M (sg-0, 10/11/2026).
//     MKW_VK_SUBGROUP=<n> pins n instead of 32 where the driver allows it. MKW_VK_SUBGROUP_PIN_STAGES=<mask> pins only
//     on those of the driver's stages: 0xe0 leaves vertex and fragment unpinned as Intel ANV does (issue #26), so a
//     GPU that can pin them shows what those drivers draw.
// The defaults (Float64, no storage rounding, 32 everywhere) leave the shader cache keys as they were (gfx_shader.c).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "gfx.h"
#include "host.h"
#include "vk.h"

VkCaps vk_caps = {.float64 = 1, .sg_size = 32};
static int f64_supported;
static VkBool32 gpl_lib, gpl_fast;   // the driver's graphicsPipelineLibrary (feature), ...FastLinking (property)

static uint32_t env_u32(const char *name, uint32_t def) {
    const char *e = cfg_get(name);
    return e && *e ? (uint32_t)strtoul(e, NULL, 0) : def;
}

void vk_caps_features(VkPhysicalDeviceFeatures *f) {
    f64_supported = f->shaderFloat64;
    if (env_u32("MKW_VK_NO_FLOAT64", 0)) f->shaderFloat64 = VK_FALSE;
    vk_caps.float64 = f->shaderFloat64;
}

int vk_caps_gpl(const VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT *f,
                const VkPhysicalDeviceGraphicsPipelineLibraryPropertiesEXT *p) {
    gpl_lib = f->graphicsPipelineLibrary;
    gpl_fast = p->graphicsPipelineLibraryFastLinking;
    vk_caps.gpl = gpl_lib && gpl_fast && cfg_bool("MKW_GPL");
    return vk_caps.gpl;
}

static int has_ext(const VkExtensionProperties *ep, uint32_t n, const char *name) {
    for (uint32_t i = 0; i < n; i++)
        if (!strcmp(ep[i].extensionName, name)) return 1;
    return 0;
}

static char depth_ok(VkPhysicalDevice p, VkFormat f) {
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(p, f, &fp);
    return fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT ? 'y' : 'n';
}

static void driver_version(char *o, size_t n, uint32_t vendor, uint32_t v) {
    if (vendor == 0x10de) snprintf(o, n, "%u.%u.%u.%u", v >> 22, v >> 14 & 0xff, v >> 6 & 0xff, v & 0x3f);
    else if (vendor == 0x8086) snprintf(o, n, "%u.%u", v >> 14, v & 0x3fff);
    else snprintf(o, n, "%u.%u.%u", VK_API_VERSION_MAJOR(v), VK_API_VERSION_MINOR(v), VK_API_VERSION_PATCH(v));
}

void vk_caps_init(VkCtx *c) {
    VkPhysicalDeviceLimits *l = &c->props.limits;
    VkPhysicalDeviceDriverProperties drv = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceSubgroupProperties sg = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES, .pNext = &drv};
    VkPhysicalDeviceVulkan13Properties p13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES, .pNext = &sg};
    VkPhysicalDevicePushDescriptorPropertiesKHR pd = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR,
                                                      .pNext = &p13};
    VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &pd};
    vkGetPhysicalDeviceProperties2(c->phys, &p2);
    VkPhysicalDeviceProvokingVertexFeaturesEXT pvf = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT};
    VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                            .pNext = c->has_provoking_vertex ? &pvf : NULL};
    VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f13};
    vkGetPhysicalDeviceFeatures2(c->phys, &f2);
    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(c->phys, NULL, &ne, NULL);
    VkExtensionProperties *ep = calloc(ne ? ne : 1, sizeof *ep);
    vkEnumerateDeviceExtensionProperties(c->phys, NULL, &ne, ep);

    uint32_t real_align = (uint32_t)l->minStorageBufferOffsetAlignment, a = env_u32("MKW_VK_SSBO_ALIGN", 0);
    if (a && !(a & (a - 1)) && a > real_align) l->minStorageBufferOffsetAlignment = a;
    vk_caps.sbuf_align = l->minStorageBufferOffsetAlignment > 4 ? (uint32_t)l->minStorageBufferOffsetAlignment : 0;

    uint32_t want = env_u32("MKW_VK_SUBGROUP", 32);
    int pin = f13.subgroupSizeControl && cfg_bool("MKW_VK_SUBGROUP_PIN") && p13.minSubgroupSize <= want &&
              want <= p13.maxSubgroupSize;
    vk_caps.sg_size = want;
    vk_caps.sg_pin_stages = pin ? p13.requiredSubgroupSizeStages & env_u32("MKW_VK_SUBGROUP_PIN_STAGES", ~0u) : 0;
    // Adreno (Turnip): a compute shader with a workgroup barrier must fit all its waves at once, and deep branching
    // (the branch stack) limits the waves an SP holds; the compiler then ends the process (ir3.c
    // ir3_get_reg_independent_max_waves: "impossible to have enough concurrent waves", exit(1)), which MKW's translated
    // shaders hit at the first race load. The widest wave halves the waves of a workgroup. MKW_VK_CS_SUBGROUP=<n>
    // sets it on any device, 0 never.
    uint32_t wide = env_u32("MKW_VK_CS_SUBGROUP", c->props.vendorID == 0x5143 ? p13.maxSubgroupSize : 0);
    vk_caps.cs_wide = wide && f13.subgroupSizeControl && (p13.requiredSubgroupSizeStages & VK_SHADER_STAGE_COMPUTE_BIT) &&
                      p13.minSubgroupSize <= wide && wide <= p13.maxSubgroupSize ? wide : 0;
    if (vk_caps.cs_wide) rt_log("vk caps: compute shaders at subgroup size %u (MKW_VK_CS_SUBGROUP)", vk_caps.cs_wide);

    vk_caps.driver_id = drv.driverID;
    char dv[32];
    driver_version(dv, sizeof dv, c->props.vendorID, c->props.driverVersion);
    uint32_t v = c->props.apiVersion;
    VkPhysicalDevice p = c->phys;
    rt_log("vk caps: %s, vendor %#06x device %#06x, driver %s \"%s\" (id %d, version %s, raw %#x), api %u.%u.%u",
           c->props.deviceName, c->props.vendorID, c->props.deviceID, drv.driverName, drv.driverInfo, (int)drv.driverID, dv,
           c->props.driverVersion, VK_API_VERSION_MAJOR(v), VK_API_VERSION_MINOR(v), VK_API_VERSION_PATCH(v));
    rt_log("vk caps: subgroup size %u, stages %#x, ops %#x; size control %d (%u-%u, required stages %#x)", sg.subgroupSize,
           (unsigned)sg.supportedStages, (unsigned)sg.supportedOperations, (int)f13.subgroupSizeControl, p13.minSubgroupSize,
           p13.maxSubgroupSize, (unsigned)p13.requiredSubgroupSizeStages);
    rt_log("vk caps: shaderFloat64 %d (enabled %d), storage buffer offset alignment %u (device %u), uniform %llu, "
           "maxPushDescriptors %u, provoking vertex ext %d last %d",
           f64_supported, vk_caps.float64, (unsigned)l->minStorageBufferOffsetAlignment, real_align,
           (unsigned long long)l->minUniformBufferOffsetAlignment, pd.maxPushDescriptors, c->has_provoking_vertex,
           (int)pvf.provokingVertexLast);
    rt_log("vk caps: ext index_type_uint8 khr %d ext %d, attachment_feedback_loop_layout %d dynamic_state %d, "
           "graphics_pipeline_library %d; depth attachment D16 %c D24S8 %c X8D24 %c D32 %c D32S8 %c",
           has_ext(ep, ne, "VK_KHR_index_type_uint8"), has_ext(ep, ne, "VK_EXT_index_type_uint8"),
           has_ext(ep, ne, "VK_EXT_attachment_feedback_loop_layout"), has_ext(ep, ne, "VK_EXT_attachment_feedback_loop_dynamic_state"),
           has_ext(ep, ne, "VK_EXT_graphics_pipeline_library"), depth_ok(p, VK_FORMAT_D16_UNORM),
           depth_ok(p, VK_FORMAT_D24_UNORM_S8_UINT), depth_ok(p, VK_FORMAT_X8_D24_UNORM_PACK32), depth_ok(p, VK_FORMAT_D32_SFLOAT),
           depth_ok(p, VK_FORMAT_D32_SFLOAT_S8_UINT));
    rt_log("vk caps: gpl %s fast-link %s (graphicsPipelineLibrary %u%s)", vk_caps.gpl ? "yes" : "no",
           gpl_fast ? "yes" : "no", (unsigned)gpl_lib, gpl_lib && gpl_fast && !vk_caps.gpl ? ", off by MKW_GPL=0" : "");
    rt_log("vk caps: fallbacks: float64 %s, storage buffers %s %u, subgroup pin %u on stages %#x, device subgroup %u",
           vk_caps.float64 ? "native" : "softfloat64", vk_caps.sbuf_align ? "rounded down to" : "exact, alignment",
           (unsigned)l->minStorageBufferOffsetAlignment, want, (unsigned)vk_caps.sg_pin_stages, sg.subgroupSize);
    free(ep);
}
