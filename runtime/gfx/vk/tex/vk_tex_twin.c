// Depth twins: a D32_SFLOAT copy of a colour texture that a shader samples with a depth compare. Maxwell compares
// against any single-channel float texture; Vulkan only against formats with SAMPLED_IMAGE_DEPTH_COMPARISON, which
// no colour format has (VUID-vkCmdDraw-None-06479). MKW's minimap shader (fs e58f1f90bbda8c9f, free roam, every frame)
// binds a 4x4 R32_SFLOAT texture the CPU fills to its shadow slot: RADV compares it anyway, another driver may return
// anything (validation, val-free2, 21/11/2026). The twin holds the same texels and is refreshed by every upload of its
// entry (vk_tex_upload.c upload calls tex_twin_copy), so a descriptor memo that skips the lookup still reads current
// data. Only R32_SFLOAT 2D textures the CPU writes: one the GPU writes would need a copy after each of those writes,
// and none is seen in MKW (logged once if one shows up; the colour image is then bound as before).
#include <math.h>
#include <string.h>

#include "vk_tex_internal.h"

static int twin_supported(void) {
    static int ok = -1;
    if (ok >= 0) return ok;
    VkFormatProperties3 p3 = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3};
    VkFormatProperties2 p = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, .pNext = &p3};
    vkGetPhysicalDeviceFormatProperties2(vk_ctx()->phys, VK_FORMAT_D32_SFLOAT, &p);
    VkFormatFeatureFlags2 need = VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT |
                                 VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_DEPTH_COMPARISON_BIT;
    ok = (p3.optimalTilingFeatures & need) == need;
    if (!ok) rt_log("vk: D32_SFLOAT lacks sampling with a compare: colour textures in shadow slots stay colour");
    return ok;
}

// The twin of img for a shadow slot, made on first use (and filled by an upload of its entry); NULL to bind img as it is.
// Texture lock held (tex_info_full).
const VkTexImage *vk_texture_depth_twin_locked(const VkTexImage *img) {
    static volatile int32_t n_gpu, n_fmt, n_fail;
    Tex *e = (Tex *)img;   // vk_texture_by_id_locked hands out &e->pub
    if (img->format != VK_FORMAT_R32_SFLOAT || img->type != VK_IMAGE_TYPE_2D || img->samples != 1) {
        log_limited(&n_fmt, "texture %#llx (VkFormat %d) sampled with a depth compare: no depth twin for that format",
                    (unsigned long long)img->addr, (int)img->format);
        return NULL;
    }
    if (img->gpu_owned || e->wr_fence || !img->mirrored || !e->t.mem) {
        log_limited(&n_gpu, "texture %#llx (R32_SFLOAT %ux%u) sampled with a depth compare is written by the GPU: "
                            "bound as colour (a twin would be stale)", (unsigned long long)img->addr, img->width,
                    img->height);
        return NULL;
    }
    if (e->dtwin.image) return &e->dtwin;
    if (!twin_supported()) return NULL;
    VkCtx *c = vk_ctx();
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                            .format = VK_FORMAT_D32_SFLOAT, .extent = {img->width, img->height, 1},
                            .mipLevels = img->levels, .arrayLayers = img->layers, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    if (img->view_type == VK_IMAGE_VIEW_TYPE_CUBE || img->view_type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY)
        ci.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkImage im;
    VmaAllocation al;
    if (vmaCreateImage(c->vma, &ci, &ai, &im, &al, NULL) != VK_SUCCESS) {
        log_limited(&n_fail, "texture %#llx: depth twin (%ux%u, %u levels, %u layers) could not be created",
                    (unsigned long long)img->addr, img->width, img->height, img->levels, img->layers);
        return NULL;
    }
    VkTexImage tw = *img;   // shape, swizzle and address as the entry's, for views and logs
    tw.image = im, tw.format = VK_FORMAT_D32_SFLOAT, tw.aspect = VK_IMAGE_ASPECT_DEPTH_BIT, tw.usage = ci.usage;
    tw.alpha_one = 0, memset(tw.chan, 0, sizeof tw.chan);
    tw.block_views = tw.block_ca_skipped = tw.fb_usage = tw.listed = tw.fb_learned = 0, tw.twin = VK_FORMAT_UNDEFINED;
    VkCommandBuffer cb = vk_upload_begin();
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                               .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT, .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                               .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .image = im, .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, img->levels, 0, img->layers}};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &di);
    vk_upload_end();
    e->dtwin = tw, e->dtwin_alloc = al;
    rt_log("vk: texture %#llx (R32_SFLOAT %ux%u, %u layers) is sampled with a depth compare: a D32_SFLOAT twin is bound "
           "in its place", (unsigned long long)img->addr, img->width, img->height, img->layers);
    upload_again(e);   // fills both from the storage, in the batch that made the twin GENERAL
    return &e->dtwin;
}

// From upload(), in its open batch, after the colour copy: the same regions from a copy of the staging texels, each
// clamped to [0, 1] (a D32_SFLOAT copy from a buffer is undefined outside it without VK_EXT_depth_range_unrestricted;
// NaN to 0). The caller's closing barrier covers this copy too.
void tex_twin_copy(Tex *e, VkCommandBuffer cb, const uint8_t *stg, VkDeviceSize total, const VkBufferImageCopy *rg, uint32_t n) {
    static volatile int32_t n_clamped;
    VkBuffer tb;
    float *dst = vk_staging(total, &tb, NULL);
    const float *src = (const float *)stg;
    uint64_t clamped = 0;
    for (VkDeviceSize i = 0; i < total / 4; i++) {
        float v = src[i], k = isnan(v) ? 0.0f : v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
        clamped += k != v || isnan(v);
        dst[i] = k;
    }
    if (clamped)
        log_limited(&n_clamped, "texture %#llx: %llu texels outside [0, 1] clamped in its depth twin (a compare against "
                                "them may differ from the console's)", (unsigned long long)e->pub.addr,
                    (unsigned long long)clamped);
    VkBufferImageCopy r[64];
    for (uint32_t i = 0; i < n; i += 64) {
        uint32_t m = n - i < 64 ? n - i : 64;
        for (uint32_t j = 0; j < m; j++) r[j] = rg[i + j], r[j].imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        vkCmdCopyBufferToImage(cb, tb, e->dtwin.image, VK_IMAGE_LAYOUT_GENERAL, m, r);
    }
}

// destroy_tex, texture lock held, inside its vk_mem_defer_use bracket
void tex_twin_destroy(Tex *e) {
    if (!e->dtwin.image) return;
    vk_exec_image_gone(e->dtwin.image);
    vk_defer_image(e->dtwin.image, e->dtwin_alloc);
    e->dtwin.image = VK_NULL_HANDLE, e->dtwin_alloc = NULL;
}
