// Native Vulkan optical-flow backend for frame generation's realFSR motion.
//
// The pictures are converted to R8 on the graphics queue because VK_NV_optical_flow consumes an
// optical-flow input image, then the hardware engine runs on its queue and copies its
// R16G16_SFIXED5_NV result into a storage buffer. No CPU readback is on this path.
#include "vk.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "cfg.h"
#include "of_luma_spv.h"
#include "vk_oflow.h"

#define OF_RING 4
#define OF_FENCE_TIMEOUT 5000000000ull

typedef struct {
    VkImage image;
    VmaAllocation alloc;
    VkImageView view;
} OfImage;

typedef struct {
    VkDevice device;
    VkPhysicalDevice phys;
    VmaAllocator vma;
    VkQueue queue;
    uint32_t gfx_family, of_family;

    PFN_vkGetPhysicalDeviceOpticalFlowImageFormatsNV get_formats;
    PFN_vkCreateOpticalFlowSessionNV create_session;
    PFN_vkDestroyOpticalFlowSessionNV destroy_session_fn;
    PFN_vkBindOpticalFlowSessionImageNV bind_image;
    PFN_vkCmdOpticalFlowExecuteNV execute;
    PFN_vkCmdPushDescriptorSetKHR push;

    uint32_t min_width, min_height, max_width, max_height;
    VkOpticalFlowGridSizeFlagsNV output_grids;
    VkOpticalFlowGridSizeFlagsNV grid_bit;

    VkSampler sampler;
    VkDescriptorSetLayout set_layout;
    VkPipelineLayout pipeline_layout;
    VkPipeline luma_pipeline;
    VkCommandPool pool;
    VkCommandBuffer cb[OF_RING];
    VkFence fence[OF_RING];
    VkSemaphore timeline;
    uint64_t next_value;

    VkOpticalFlowSessionNV session;
    OfImage luma[2];            // 0: previous/reference, 1: current/input
    OfImage flow;
    VkBuffer vectors;
    VmaAllocation vectors_alloc;
    uint32_t width, height, wblocks, hblocks, grid;
    int luma_layout, flow_layout;

    unsigned slot_next;
    int pending, pending_slot, pending_reset;
    int ok, failed;
} OfState;

static OfState O;

static int sharing_families(uint32_t family[2]) {
    family[0] = O.gfx_family;
    if (O.of_family == O.gfx_family) return 1;
    family[1] = O.of_family;
    return 2;
}

static void destroy_image(OfImage *image) {
    if (image->view && O.device) vkDestroyImageView(O.device, image->view, NULL);
    if (image->image && O.vma) vmaDestroyImage(O.vma, image->image, image->alloc);
    memset(image, 0, sizeof *image);
}

static void destroy_session_resources(void) {
    if (O.session && O.destroy_session_fn) O.destroy_session_fn(O.device, O.session, NULL);
    O.session = VK_NULL_HANDLE;
    destroy_image(&O.luma[0]);
    destroy_image(&O.luma[1]);
    destroy_image(&O.flow);
    if (O.vectors && O.vma) vmaDestroyBuffer(O.vma, O.vectors, O.vectors_alloc);
    O.vectors = VK_NULL_HANDLE;
    O.vectors_alloc = NULL;
    O.width = O.height = O.wblocks = O.hblocks = 0;
    O.luma_layout = O.flow_layout = 0;
}

static int format_list_has(VkFormat wanted, VkOpticalFlowUsageFlagsNV usage) {
    VkOpticalFlowImageFormatInfoNV info = {
        .sType = VK_STRUCTURE_TYPE_OPTICAL_FLOW_IMAGE_FORMAT_INFO_NV,
        .usage = usage,
    };
    uint32_t count = 0;
    VkResult r = O.get_formats(O.phys, &info, &count, NULL);
    if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || !count) return 0;
    VkOpticalFlowImageFormatPropertiesNV *formats = calloc(count, sizeof *formats);
    if (!formats) return 0;
    for (uint32_t i = 0; i < count; i++)
        formats[i].sType = VK_STRUCTURE_TYPE_OPTICAL_FLOW_IMAGE_FORMAT_PROPERTIES_NV;
    r = O.get_formats(O.phys, &info, &count, formats);
    int found = 0;
    if (r == VK_SUCCESS || r == VK_INCOMPLETE) {
        for (uint32_t i = 0; i < count; i++)
            if (formats[i].format == wanted) found = 1;
    }
    free(formats);
    return found;
}

static int format_has(VkFormat format, VkFormatFeatureFlags2 need) {
    VkFormatProperties3 p3 = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3};
    VkFormatProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, .pNext = &p3};
    vkGetPhysicalDeviceFormatProperties2(O.phys, format, &p2);
    return (p3.optimalTilingFeatures & need) == need;
}

static int of_image_supported(VkFormat format, VkOpticalFlowUsageFlagsNV of_usage, VkImageUsageFlags usage,
                              uint32_t width, uint32_t height) {
    VkOpticalFlowImageFormatInfoNV of = {
        .sType = VK_STRUCTURE_TYPE_OPTICAL_FLOW_IMAGE_FORMAT_INFO_NV,
        .usage = of_usage,
    };
    VkPhysicalDeviceImageFormatInfo2 info = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
        .pNext = &of,
        .format = format,
        .type = VK_IMAGE_TYPE_2D,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
    };
    VkImageFormatProperties2 props = {.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
    VkResult r = vkGetPhysicalDeviceImageFormatProperties2(O.phys, &info, &props);
    return r == VK_SUCCESS && (!width || width <= props.imageFormatProperties.maxExtent.width) &&
           (!height || height <= props.imageFormatProperties.maxExtent.height);
}

static int queue_family_usable(void) {
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(O.phys, &count, NULL);
    if (O.of_family >= count) return 0;
    VkQueueFamilyProperties *props = calloc(count, sizeof *props);
    if (!props) return 0;
    vkGetPhysicalDeviceQueueFamilyProperties(O.phys, &count, props);
    VkQueueFlags flags = O.of_family < count ? props[O.of_family].queueFlags : 0;
    uint32_t queues = O.of_family < count ? props[O.of_family].queueCount : 0;
    free(props);
    return queues && (flags & VK_QUEUE_OPTICAL_FLOW_BIT_NV) && (flags & VK_QUEUE_TRANSFER_BIT);
}

static int create_pipeline(void) {
    VkDescriptorSetLayoutBinding binding[4];
    for (uint32_t i = 0; i < 4; i++) {
        binding[i] = (VkDescriptorSetLayoutBinding){
            .binding = i,
            .descriptorCount = 1,
            .descriptorType = i < 2 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        };
    }
    VkDescriptorSetLayoutCreateInfo sl = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
        .bindingCount = 4,
        .pBindings = binding,
    };
    if (vkCreateDescriptorSetLayout(O.device, &sl, NULL, &O.set_layout) != VK_SUCCESS) return 0;
    VkPipelineLayoutCreateInfo pl = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &O.set_layout,
    };
    if (vkCreatePipelineLayout(O.device, &pl, NULL, &O.pipeline_layout) != VK_SUCCESS) return 0;

    VkShaderModuleCreateInfo sm = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof of_luma_spv,
        .pCode = of_luma_spv,
    };
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(O.device, &sm, NULL, &module) != VK_SUCCESS) return 0;
    VkComputePipelineCreateInfo cp = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = module,
            .pName = "main",
        },
        .layout = O.pipeline_layout,
    };
    VkResult r = vkCreateComputePipelines(O.device, VK_NULL_HANDLE, 1, &cp, NULL, &O.luma_pipeline);
    vkDestroyShaderModule(O.device, module, NULL);
    return r == VK_SUCCESS;
}

static int create_ring(void) {
    VkCommandPoolCreateInfo pc = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = O.of_family,
    };
    if (vkCreateCommandPool(O.device, &pc, NULL, &O.pool) != VK_SUCCESS) return 0;
    VkCommandBufferAllocateInfo cb = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = O.pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = OF_RING,
    };
    if (vkAllocateCommandBuffers(O.device, &cb, O.cb) != VK_SUCCESS) return 0;
    VkFenceCreateInfo fi = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT,
    };
    for (unsigned i = 0; i < OF_RING; i++)
        if (vkCreateFence(O.device, &fi, NULL, &O.fence[i]) != VK_SUCCESS) return 0;

    VkSemaphoreTypeCreateInfo st = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
        .initialValue = 0,
    };
    VkSemaphoreCreateInfo sc = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &st};
    return vkCreateSemaphore(O.device, &sc, NULL, &O.timeline) == VK_SUCCESS;
}

int vk_of_init(VkQueue of_queue, uint32_t of_family) {
    if (O.ok) return 1;
    if (O.failed) return 0;
    VkCtx *c = vk_ctx();
    if (!c || !c->device || !c->phys || !c->vma || !of_queue) return 0;

    memset(&O, 0, sizeof O);
    O.device = c->device;
    O.phys = c->phys;
    O.vma = c->vma;
    O.queue = of_queue;
    O.gfx_family = c->gfx_family;
    O.of_family = of_family;
    O.failed = 1;

    O.get_formats = (PFN_vkGetPhysicalDeviceOpticalFlowImageFormatsNV)vkGetInstanceProcAddr(
        c->instance, "vkGetPhysicalDeviceOpticalFlowImageFormatsNV");
    O.create_session = (PFN_vkCreateOpticalFlowSessionNV)vkGetDeviceProcAddr(O.device, "vkCreateOpticalFlowSessionNV");
    O.destroy_session_fn =
        (PFN_vkDestroyOpticalFlowSessionNV)vkGetDeviceProcAddr(O.device, "vkDestroyOpticalFlowSessionNV");
    O.bind_image = (PFN_vkBindOpticalFlowSessionImageNV)vkGetDeviceProcAddr(O.device, "vkBindOpticalFlowSessionImageNV");
    O.execute = (PFN_vkCmdOpticalFlowExecuteNV)vkGetDeviceProcAddr(O.device, "vkCmdOpticalFlowExecuteNV");
    O.push = (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(O.device, "vkCmdPushDescriptorSetKHR");
    if (!O.get_formats || !O.create_session || !O.destroy_session_fn || !O.bind_image || !O.execute || !O.push) {
        rt_log("vk: hardware optical flow off: VK_NV_optical_flow entry points are incomplete");
        return 0;
    }
    if (!queue_family_usable()) {
        rt_log("vk: hardware optical flow off: family %u lacks optical-flow or transfer support", of_family);
        return 0;
    }

    VkPhysicalDeviceOpticalFlowPropertiesNV np = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPTICAL_FLOW_PROPERTIES_NV,
    };
    VkPhysicalDeviceProperties2 p = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &np};
    vkGetPhysicalDeviceProperties2(O.phys, &p);
    O.min_width = np.minWidth;
    O.min_height = np.minHeight;
    O.max_width = np.maxWidth;
    O.max_height = np.maxHeight;
    O.output_grids = np.supportedOutputGridSizes;

    if (!format_list_has(VK_FORMAT_R8_UNORM, VK_OPTICAL_FLOW_USAGE_INPUT_BIT_NV) ||
        !format_list_has(VK_FORMAT_R16G16_SFIXED5_NV, VK_OPTICAL_FLOW_USAGE_OUTPUT_BIT_NV)) {
        rt_log("vk: hardware optical flow off: required R8 input or SFIXED5 output format is unsupported");
        return 0;
    }
    if (!format_has(VK_FORMAT_R8_UNORM,
                    VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_2_OPTICAL_FLOW_IMAGE_BIT_NV)) {
        rt_log("vk: hardware optical flow off: R8_UNORM cannot be a compute-written optical-flow input");
        return 0;
    }
    if (!format_has(VK_FORMAT_R16G16_SFIXED5_NV,
                    VK_FORMAT_FEATURE_2_OPTICAL_FLOW_VECTOR_BIT_NV | VK_FORMAT_FEATURE_2_TRANSFER_SRC_BIT |
                    VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT)) {
        rt_log("vk: hardware optical flow off: SFIXED5 cannot be copied from optical-flow output to storage");
        return 0;
    }
    rt_log("vk: hardware optical-flow output view support: sampled %d storage %d",
           format_has(VK_FORMAT_R16G16_SFIXED5_NV, VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT),
           format_has(VK_FORMAT_R16G16_SFIXED5_NV, VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT));
    const char *requested_grid = cfg_get("MKW_FG_NVOF_GRID");
    unsigned requested = requested_grid ? (unsigned)strtoul(requested_grid, NULL, 10) : 4u;
    VkOpticalFlowGridSizeFlagsNV requested_bit = requested == 1 ? VK_OPTICAL_FLOW_GRID_SIZE_1X1_BIT_NV
        : requested == 2 ? VK_OPTICAL_FLOW_GRID_SIZE_2X2_BIT_NV : VK_OPTICAL_FLOW_GRID_SIZE_4X4_BIT_NV;
    if ((requested == 1 || requested == 2 || requested == 4) && (O.output_grids & requested_bit))
        O.grid = requested, O.grid_bit = requested_bit;
    else if (O.output_grids & VK_OPTICAL_FLOW_GRID_SIZE_4X4_BIT_NV)
        O.grid = 4, O.grid_bit = VK_OPTICAL_FLOW_GRID_SIZE_4X4_BIT_NV;
    else if (O.output_grids & VK_OPTICAL_FLOW_GRID_SIZE_2X2_BIT_NV)
        O.grid = 2, O.grid_bit = VK_OPTICAL_FLOW_GRID_SIZE_2X2_BIT_NV;
    else if (O.output_grids & VK_OPTICAL_FLOW_GRID_SIZE_1X1_BIT_NV)
        O.grid = 1, O.grid_bit = VK_OPTICAL_FLOW_GRID_SIZE_1X1_BIT_NV;
    else {
        rt_log("vk: hardware optical flow off: no 4x4, 2x2 or 1x1 output grid");
        return 0;
    }

    VkSamplerCreateInfo sci = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_NEAREST,
        .minFilter = VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .maxLod = 0,
    };
    if (vkCreateSampler(O.device, &sci, NULL, &O.sampler) != VK_SUCCESS || !create_pipeline() || !create_ring()) {
        rt_log("vk: hardware optical flow off: module objects could not be created");
        return 0;
    }
    rt_log("vk: hardware optical flow ready: queue family %u, %ux%u grid, packed SFIXED5 vectors", O.of_family, O.grid,
           O.grid);
    O.failed = 0;
    O.ok = 1;
    return 1;
}

static int create_image(OfImage *dst, VkFormat format, uint32_t width, uint32_t height, VkImageUsageFlags usage,
                        VkOpticalFlowUsageFlagsNV of_usage) {
    uint32_t families[2];
    int nf = sharing_families(families);
    VkOpticalFlowImageFormatInfoNV of = {
        .sType = VK_STRUCTURE_TYPE_OPTICAL_FLOW_IMAGE_FORMAT_INFO_NV,
        .usage = of_usage,
    };
    VkImageCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &of,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
        .sharingMode = nf == 1 ? VK_SHARING_MODE_EXCLUSIVE : VK_SHARING_MODE_CONCURRENT,
        .queueFamilyIndexCount = nf == 1 ? 0 : (uint32_t)nf,
        .pQueueFamilyIndices = nf == 1 ? NULL : families,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(O.vma, &ci, &ai, &dst->image, &dst->alloc, NULL) != VK_SUCCESS) return 0;
    VkImageViewCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = dst->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    return vkCreateImageView(O.device, &vi, NULL, &dst->view) == VK_SUCCESS;
}

static int create_buffer(uint32_t wblocks, uint32_t hblocks) {
    if (!wblocks || !hblocks || (size_t)wblocks > SIZE_MAX / (size_t)hblocks / sizeof(uint32_t)) return 0;
    VkDeviceSize size = (VkDeviceSize)wblocks * hblocks * sizeof(uint32_t);
    uint32_t families[2];
    int nf = sharing_families(families);
    VkBufferCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .sharingMode = nf == 1 ? VK_SHARING_MODE_EXCLUSIVE : VK_SHARING_MODE_CONCURRENT,
        .queueFamilyIndexCount = nf == 1 ? 0 : (uint32_t)nf,
        .pQueueFamilyIndices = nf == 1 ? NULL : families,
    };
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    return vmaCreateBuffer(O.vma, &ci, &ai, &O.vectors, &O.vectors_alloc, NULL) == VK_SUCCESS;
}

static int ensure_resources(uint32_t width, uint32_t height) {
    if (O.session && O.width == width && O.height == height) return 1;

    vk_queue_lock();
    vkDeviceWaitIdle(O.device);
    vk_queue_unlock();
    destroy_session_resources();

    if (width < O.min_width || height < O.min_height || width > O.max_width || height > O.max_height) {
        rt_log("vk: hardware optical flow off for %ux%u: supported size is %ux%u to %ux%u", width, height, O.min_width,
               O.min_height, O.max_width, O.max_height);
        return 0;
    }
    const VkFormat input_format = VK_FORMAT_R8_UNORM;
    const VkFormat flow_format = VK_FORMAT_R16G16_SFIXED5_NV;
    uint32_t wb = (width + O.grid - 1) / O.grid, hb = (height + O.grid - 1) / O.grid;
    if (!of_image_supported(input_format, VK_OPTICAL_FLOW_USAGE_INPUT_BIT_NV, VK_IMAGE_USAGE_STORAGE_BIT, width, height) ||
        !of_image_supported(flow_format, VK_OPTICAL_FLOW_USAGE_OUTPUT_BIT_NV,
                            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, wb, hb)) {
        rt_log("vk: hardware optical flow off for %ux%u: optical-flow image usage is unsupported", width, height);
        return 0;
    }

    O.width = width;
    O.height = height;
    O.wblocks = wb;
    O.hblocks = hb;
    if (!create_image(&O.luma[0], input_format, width, height, VK_IMAGE_USAGE_STORAGE_BIT,
                      VK_OPTICAL_FLOW_USAGE_INPUT_BIT_NV) ||
        !create_image(&O.luma[1], input_format, width, height, VK_IMAGE_USAGE_STORAGE_BIT,
                      VK_OPTICAL_FLOW_USAGE_INPUT_BIT_NV) ||
        // The optical-flow session binds a view even though the port copies the output instead of sampling it.
        // Transfer-only images cannot have a view (VUID-VkImageViewCreateInfo-image-04441).
        !create_image(&O.flow, flow_format, O.wblocks, O.hblocks, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                      VK_OPTICAL_FLOW_USAGE_OUTPUT_BIT_NV) ||
        !create_buffer(O.wblocks, O.hblocks)) {
        rt_log("vk: hardware optical flow off for %ux%u: no memory for its images or vector buffer", width, height);
        destroy_session_resources();
        return 0;
    }

    VkOpticalFlowSessionCreateInfoNV sc = {
        .sType = VK_STRUCTURE_TYPE_OPTICAL_FLOW_SESSION_CREATE_INFO_NV,
        .width = width,
        .height = height,
        .imageFormat = input_format,
        .flowVectorFormat = flow_format,
        .costFormat = VK_FORMAT_UNDEFINED,
        .outputGridSize = O.grid_bit,
        .hintGridSize = 0,
        .performanceLevel = VK_OPTICAL_FLOW_PERFORMANCE_LEVEL_SLOW_NV,
        .flags = 0,
    };
    VkResult r = O.create_session(O.device, &sc, NULL, &O.session);
    if (r == VK_SUCCESS)
        r = O.bind_image(O.device, O.session, VK_OPTICAL_FLOW_SESSION_BINDING_POINT_REFERENCE_NV, O.luma[0].view,
                         VK_IMAGE_LAYOUT_GENERAL);
    if (r == VK_SUCCESS)
        r = O.bind_image(O.device, O.session, VK_OPTICAL_FLOW_SESSION_BINDING_POINT_INPUT_NV, O.luma[1].view,
                         VK_IMAGE_LAYOUT_GENERAL);
    if (r == VK_SUCCESS)
        r = O.bind_image(O.device, O.session, VK_OPTICAL_FLOW_SESSION_BINDING_POINT_FLOW_VECTOR_NV, O.flow.view,
                         VK_IMAGE_LAYOUT_GENERAL);
    if (r != VK_SUCCESS) {
        rt_log("vk: hardware optical flow off for %ux%u: session creation or binding failed (%d)", width, height, (int)r);
        destroy_session_resources();
        return 0;
    }
    rt_log("vk: hardware optical flow session %ux%u: %ux%u blocks, %u-pixel current->previous grid", width, height,
           O.wblocks, O.hblocks, O.grid);
    return 1;
}

static int record_luma(VkCommandBuffer cb, VkImageView previous, VkImageView current) {
    VkImageMemoryBarrier2 luma[2];
    memset(luma, 0, sizeof luma);
    for (unsigned i = 0; i < 2; i++) {
        luma[i] = (VkImageMemoryBarrier2){
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
            .srcAccessMask = 0,
            .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            .oldLayout = O.luma_layout ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .image = O.luma[i].image,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };
    }
    VkMemoryBarrier2 source = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
    };
    VkDependencyInfo dep = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &source,
        .imageMemoryBarrierCount = 2,
        .pImageMemoryBarriers = luma,
    };
    vkCmdPipelineBarrier2(cb, &dep);

    VkDescriptorImageInfo image[4] = {
        {.sampler = O.sampler, .imageView = previous, .imageLayout = VK_IMAGE_LAYOUT_GENERAL},
        {.sampler = O.sampler, .imageView = current, .imageLayout = VK_IMAGE_LAYOUT_GENERAL},
        {.imageView = O.luma[0].view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL},
        {.imageView = O.luma[1].view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL},
    };
    VkWriteDescriptorSet write[4];
    memset(write, 0, sizeof write);
    for (uint32_t i = 0; i < 4; i++) {
        write[i] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstBinding = i,
            .descriptorCount = 1,
            .descriptorType = i < 2 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .pImageInfo = &image[i],
        };
    }
    O.push(cb, VK_PIPELINE_BIND_POINT_COMPUTE, O.pipeline_layout, 0, 4, write);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, O.luma_pipeline);
    vkCmdDispatch(cb, (O.width + 7) / 8, (O.height + 7) / 8, 1);

    VkMemoryBarrier2 done_barrier = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
    };
    VkDependencyInfo done = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &done_barrier,
    };
    vkCmdPipelineBarrier2(cb, &done);
    O.luma_layout = 1;
    return 1;
}

static void record_optical_flow(VkCommandBuffer cb, int reset) {
    VkImageMemoryBarrier2 flow = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = O.flow_layout ? VK_PIPELINE_STAGE_2_TRANSFER_BIT : VK_PIPELINE_STAGE_2_NONE,
        .dstStageMask = VK_PIPELINE_STAGE_2_OPTICAL_FLOW_BIT_NV,
        .dstAccessMask = VK_ACCESS_2_OPTICAL_FLOW_WRITE_BIT_NV,
        .oldLayout = O.flow_layout ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .image = O.flow.image,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    VkMemoryBarrier2 ready = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_OPTICAL_FLOW_BIT_NV,
        .dstAccessMask = VK_ACCESS_2_OPTICAL_FLOW_READ_BIT_NV | VK_ACCESS_2_OPTICAL_FLOW_WRITE_BIT_NV,
    };
    VkDependencyInfo start = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &ready,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &flow,
    };
    vkCmdPipelineBarrier2(cb, &start);

    VkOpticalFlowExecuteInfoNV execute = {
        .sType = VK_STRUCTURE_TYPE_OPTICAL_FLOW_EXECUTE_INFO_NV,
        .flags = reset ? VK_OPTICAL_FLOW_EXECUTE_DISABLE_TEMPORAL_HINTS_BIT_NV : 0,
    };
    O.execute(cb, O.session, &execute);

    VkImageMemoryBarrier2 to_transfer = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_OPTICAL_FLOW_BIT_NV,
        .srcAccessMask = VK_ACCESS_2_OPTICAL_FLOW_WRITE_BIT_NV,
        .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
        .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .image = O.flow.image,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    VkDependencyInfo copy_ready = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &to_transfer,
    };
    vkCmdPipelineBarrier2(cb, &copy_ready);
    VkBufferImageCopy region = {
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageExtent = {O.wblocks, O.hblocks, 1},
    };
    vkCmdCopyImageToBuffer(cb, O.flow.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, O.vectors, 1, &region);
}

int vk_of_prepare(VkCommandBuffer graphics_copies, VkImageView previous, VkImageView current, uint32_t width,
                  uint32_t height, int reset, VkOfResult *out) {
    if (out) memset(out, 0, sizeof *out);
    if (!O.ok || !graphics_copies || !previous || !current || !out || O.pending || !width || !height) return 0;
    if (!ensure_resources(width, height)) return 0;

    int slot = (int)(O.slot_next++ % OF_RING);
    VkResult r = vkWaitForFences(O.device, 1, &O.fence[slot], VK_TRUE, OF_FENCE_TIMEOUT);
    if (r != VK_SUCCESS) {
        rt_log("vk: hardware optical flow: command ring fence wait failed (%d)", (int)r);
        return 0;
    }
    vkResetCommandBuffer(O.cb[slot], 0);

    if (!record_luma(graphics_copies, previous, current)) return 0;
    VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    r = vkBeginCommandBuffer(O.cb[slot], &begin);
    if (r != VK_SUCCESS) {
        rt_log("vk: hardware optical flow: command buffer begin failed (%d)", (int)r);
        return 0;
    }
    record_optical_flow(O.cb[slot], reset);
    r = vkEndCommandBuffer(O.cb[slot]);
    if (r != VK_SUCCESS) {
        rt_log("vk: hardware optical flow: command buffer end failed (%d)", (int)r);
        return 0;
    }

    O.pending = 1;
    O.pending_slot = slot;
    O.pending_reset = reset != 0;
    out->vectors = O.vectors;
    out->wblocks = O.wblocks;
    out->hblocks = O.hblocks;
    out->grid = O.grid;
    out->ready = O.timeline;
    out->value = 0;
    return 1;
}

static int recreate_fence(int slot) {
    if (O.fence[slot]) vkDestroyFence(O.device, O.fence[slot], NULL);
    O.fence[slot] = VK_NULL_HANDLE;
    VkFenceCreateInfo fi = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT,
    };
    return vkCreateFence(O.device, &fi, NULL, &O.fence[slot]) == VK_SUCCESS;
}

int vk_of_submit(VkSemaphore graphics_ready, uint64_t ready_value, VkOfResult *out) {
    if (!O.ok || !O.pending || !out || O.next_value == UINT64_MAX) return 0;
    int slot = O.pending_slot;
    uint64_t value = O.next_value + 1;
    VkCommandBufferSubmitInfo cb = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .commandBuffer = O.cb[slot],
    };
    VkSemaphoreSubmitInfo wait = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = graphics_ready,
        .value = ready_value,
        .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
    };
    VkSemaphoreSubmitInfo signal = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = O.timeline,
        .value = value,
        .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
    };
    VkSubmitInfo2 submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .waitSemaphoreInfoCount = graphics_ready ? 1u : 0u,
        .pWaitSemaphoreInfos = graphics_ready ? &wait : NULL,
        .commandBufferInfoCount = 1,
        .pCommandBufferInfos = &cb,
        .signalSemaphoreInfoCount = 1,
        .pSignalSemaphoreInfos = &signal,
    };

    vk_queue_lock();
    VkResult r = vkResetFences(O.device, 1, &O.fence[slot]);
    if (r == VK_SUCCESS) r = vkQueueSubmit2(O.queue, 1, &submit, O.fence[slot]);
    if (r != VK_SUCCESS) recreate_fence(slot);
    vk_queue_unlock();
    if (r != VK_SUCCESS) {
        rt_log("vk: hardware optical flow: submit failed (%d)", (int)r);
        O.pending = 0;
        return 0;
    }

    O.next_value = value;
    O.flow_layout = 1;
    O.pending = 0;
    out->vectors = O.vectors;
    out->wblocks = O.wblocks;
    out->hblocks = O.hblocks;
    out->grid = O.grid;
    out->ready = O.timeline;
    out->value = value;
    return 1;
}

void vk_of_release(void) {
    if (!O.device) return;
    vk_queue_lock();
    vkDeviceWaitIdle(O.device);
    vk_queue_unlock();

    destroy_session_resources();
    if (O.luma_pipeline) vkDestroyPipeline(O.device, O.luma_pipeline, NULL);
    if (O.pipeline_layout) vkDestroyPipelineLayout(O.device, O.pipeline_layout, NULL);
    if (O.set_layout) vkDestroyDescriptorSetLayout(O.device, O.set_layout, NULL);
    if (O.sampler) vkDestroySampler(O.device, O.sampler, NULL);
    if (O.timeline) vkDestroySemaphore(O.device, O.timeline, NULL);
    for (unsigned i = 0; i < OF_RING; i++)
        if (O.fence[i]) vkDestroyFence(O.device, O.fence[i], NULL);
    if (O.pool) vkDestroyCommandPool(O.device, O.pool, NULL);
    memset(&O, 0, sizeof O);
}
