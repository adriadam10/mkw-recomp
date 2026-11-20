// --probe-fsr-fg: FSR integration with synthetic geometry or raw captured depth and motion.
// A textured plane translates beneath a camera-attached rectangle and its fixed shadow. Geometry vectors belong
// to the plane under the shadow; image vectors do not. Compare both with MKW_FSR_PROBE_MV=geometry|image|zero.
// MKW_FSR_PROBE_INPUT_PREFIX selects raw mode. MKW_FSR_PROBE_FRAMES is then a suffix or a comma-separated suffix list,
// loading vN.bin and zN.bin from that directory. MKW_FSR_PROBE_VECTOR=x,y replaces raw motion with a constant field.
// Explicit CPU waits are intentional here: this tests the SDK contract, never production frame pacing.
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cfg.h"
#include "host.h"
#include "vk.h"
#include "vk_rec.h"
#include "vk_temporal.h"

static int probe_width = 256, probe_height = 128;
#define W probe_width
#define H probe_height
#define BYTES (W * H * 4)
#define CHECK(x) do { if (!(x)) { printf("probe-fsr-fg: FAILED line %d: %s\n", __LINE__, #x); return 1; } } while (0)

static float motion_x = -4, motion_y = 2;
static float vector_x, vector_y;
static int constant_vector;
static int opaque_shadow;
static int still_images;
static uint16_t half(float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    uint32_t sign = bits >> 16 & 0x8000, exponent = bits >> 23 & 255, mantissa = bits & 0x7fffff;
    if (exponent == 255) return (uint16_t)(sign | 0x7c00 | (mantissa ? 0x200 | mantissa >> 13 : 0));
    int half_exp = (int)exponent - 112;
    if (half_exp >= 31) return (uint16_t)(sign | 0x7c00);
    if (half_exp <= 0) {
        if (half_exp < -10) return (uint16_t)sign;
        mantissa |= 0x800000;
        uint32_t shift = (uint32_t)(14 - half_exp);
        mantissa = (mantissa + ((1u << (shift - 1)) - 1) + (mantissa >> shift & 1)) >> shift;
        return (uint16_t)(sign | mantissa);
    }
    mantissa = (mantissa + 0xfff + (mantissa >> 13 & 1)) >> 13;
    if (mantissa >= 1024) {
        mantissa = 0;
        if (++half_exp >= 31) return (uint16_t)(sign | 0x7c00);
    }
    return (uint16_t)(sign | (uint32_t)half_exp << 10 | mantissa);
}
static int parse_pair(const char *text, float *x, float *y) {
    char tail;
    return sscanf(text, "%f,%f %c", x, y, &tail) == 2;
}
static char *raw_path(const char *prefix, char kind, unsigned frame) {
    size_t n = strlen(prefix);
    int slash = n && (prefix[n - 1] == '/' || prefix[n - 1] == '\\');
    size_t cap = n + (slash ? 0 : 1) + 32;
    char *path = malloc(cap);
    if (!path) return NULL;
    snprintf(path, cap, "%s%s%c%u.bin", prefix, slash ? "" : "/", kind, frame);
    return path;
}
static int read_raw(const char *prefix, char kind, unsigned frame, uint32_t width, uint32_t height,
                    void *field, size_t bytes) {
    char *path = raw_path(prefix, kind, frame);
    if (!path) {
        printf("probe-fsr-fg: raw input path for %c%u.bin is too long\n", kind, frame);
        return 0;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("probe-fsr-fg: raw input missing or unreadable: %s\n", path);
        free(path);
        return 0;
    }
    uint32_t header[2];
    if (fread(header, sizeof header, 1, f) != 1) {
        printf("probe-fsr-fg: raw input %s is short: missing u32 width,height header\n", path);
        fclose(f), free(path);
        return 0;
    }
    if (header[0] != width || header[1] != height) {
        printf("probe-fsr-fg: raw input %s is malformed: header %ux%u, expected %ux%u\n",
               path, header[0], header[1], width, height);
        fclose(f), free(path);
        return 0;
    }
    size_t got = fread(field, 1, bytes, f);
    if (got != bytes) {
        printf("probe-fsr-fg: raw input %s is short: %zu field bytes, expected %zu\n", path, got, bytes);
        fclose(f), free(path);
        return 0;
    }
    int extra = fgetc(f);
    if (extra != EOF) {
        printf("probe-fsr-fg: raw input %s is malformed: extra data after %zu field bytes\n", path, bytes);
        fclose(f), free(path);
        return 0;
    }
    if (ferror(f)) {
        printf("probe-fsr-fg: raw input %s failed while checking its length\n", path);
        fclose(f), free(path);
        return 0;
    }
    if (fclose(f)) {
        printf("probe-fsr-fg: raw input %s could not be closed after reading\n", path);
        free(path);
        return 0;
    }
    free(path);
    return 1;
}
static int parse_raw_frames(const char *text, int **frames, int *count) {
    static const int maximum = 4096;
    if (!text || !*text) {
        printf("probe-fsr-fg: raw mode needs MKW_FSR_PROBE_FRAMES (a suffix or comma-separated suffixes)\n");
        return 0;
    }
    size_t cap = strlen(text) + 1;
    char *copy = malloc(cap), *p = copy;
    int *ids = malloc((size_t)maximum * sizeof *ids);
    if (!copy || !ids) {
        free(copy), free(ids);
        printf("probe-fsr-fg: raw mode could not allocate its frame list\n");
        return 0;
    }
    memcpy(copy, text, cap);
    int n = 0;
    for (;;) {
        while (*p == ' ' || *p == '\t') p++;
        if (*p < '0' || *p > '9') {
            printf("probe-fsr-fg: malformed raw frame suffix list '%s' at '%s'\n", text, p);
            break;
        }
        errno = 0;
        char *end;
        unsigned long value = strtoul(p, &end, 10);
        if (errno || end == p || value > INT_MAX) {
            printf("probe-fsr-fg: raw frame suffix '%s' is not a nonnegative decimal integer\n", p);
            break;
        }
        if (n == maximum) {
            printf("probe-fsr-fg: raw frame suffix list exceeds %d entries\n", maximum);
            break;
        }
        ids[n++] = (int)value;
        p = end;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) {
            free(copy);
            *frames = ids, *count = n;
            return 1;
        }
        if (*p != ',') {
            printf("probe-fsr-fg: malformed raw frame suffix list '%s' at '%s'\n", text, p);
            break;
        }
        p++;
    }
    free(copy), free(ids);
    return 0;
}
static int queue_timestamps(VkCtx *c) {
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(c->phys, &count, NULL);
    VkQueueFamilyProperties *props = calloc(count ? count : 1, sizeof *props);
    if (!props) return 0;
    vkGetPhysicalDeviceQueueFamilyProperties(c->phys, &count, props);
    int supported = c->gfx_family < count && props[c->gfx_family].timestampValidBits &&
                    c->props.limits.timestampPeriod > 0;
    free(props);
    return supported;
}
static int body(int x, int y) { return x >= 104 && x < 152 && y >= 28 && y < 78; }
static int shadow(int x, int y) { return x >= 88 && x < 168 && y >= 76 && y < 98; }
static uint8_t channel(int x, int y, double t, int k) {
    if (still_images) t = 0;
    if (body(x, y)) return (uint8_t)(60 + ((x * (k + 3) + y * (k + 5)) % 160));
    if (opaque_shadow && shadow(x,y)) return 32;
    double xx = x + motion_x * t, yy = y + motion_y * t;
    double p = 128 + 35 * sin(xx * .13 + yy * .031 + k) + 33 * cos(yy * .19 + xx * .017 + k * .7)
                   + 28 * sin(xx * .071 - yy * .11);
    return (uint8_t)(p * (shadow(x, y) ? .25 : 1.0));
}
static void barrier(VkCommandBuffer cb) {
    VkMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &d);
}
static int finish(VkCtx *c, VkCommandBuffer cb, VkFence fence) {
    if (vkEndCommandBuffer(cb) != VK_SUCCESS) return 0;
    VkCommandBufferSubmitInfo ci = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = cb};
    VkSubmitInfo2 si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = 1, .pCommandBufferInfos = &ci};
    vk_queue_lock();
    VkResult r = vkQueueSubmit2(c->gfx_queue, 1, &si, fence);
    vk_queue_unlock();
    return r == VK_SUCCESS && vkWaitForFences(c->device, 1, &fence, VK_TRUE, 10000000000ull) == VK_SUCCESS;
}

int probe_fsr_fg(void) {
    const char *raw_prefix = cfg_get("MKW_FSR_PROBE_INPUT_PREFIX");
    int raw_mode = raw_prefix && *raw_prefix;
    const char *size = cfg_get("MKW_FSR_PROBE_SIZE");
    if (size) {
        CHECK(sscanf(size,"%dx%d",&probe_width,&probe_height) == 2);
        CHECK(W >= 256 && H >= 128 && W <= 4096 && H <= 2160);
    } else if (raw_mode) {
        probe_width = 2560, probe_height = 1440;
    }
    const char *frames = cfg_get("MKW_FSR_PROBE_FRAMES");
    int last_frame = 20, raw_count = 0, *raw_frames = NULL;
    if (raw_mode) {
        CHECK(W == 2560 && H == 1440);
        CHECK(parse_raw_frames(frames, &raw_frames, &raw_count));
    } else if (frames) {
        last_frame = atoi(frames);
        CHECK(last_frame >= 2 && last_frame <= 60);
    }
    CHECK(vk_init());
    VkCtx *c = vk_ctx();
    printf("probe-fsr-fg: Vulkan validation active %d\n", c->validation);
    if (cfg_bool("MKW_VK_VALIDATION")) CHECK(c->validation);
    CHECK(vk_tmp_ffx_load());
    int recording = rec_suspend();
    const char *mode = cfg_get("MKW_FSR_PROBE_MV");
    if (!mode) mode = "geometry";
    CHECK(!strcmp(mode, "geometry") || !strcmp(mode, "image") || !strcmp(mode, "zero"));
    const char *opaque = cfg_get("MKW_FSR_PROBE_OPAQUE");
    opaque_shadow = opaque && strcmp(opaque,"0");
    const char *still = cfg_get("MKW_FSR_PROBE_STILL");
    still_images = still && strcmp(still,"0");
    const char *shift = cfg_get("MKW_FSR_PROBE_SHIFT");
    if (shift) {
        CHECK(sscanf(shift,"%f,%f",&motion_x,&motion_y) == 2);
        CHECK(isfinite(motion_x) && isfinite(motion_y) && fabsf(motion_x) <= 128 && fabsf(motion_y) <= 128 &&
              motion_x == roundf(motion_x) && motion_y == roundf(motion_y));
    }
    const char *vector = cfg_get("MKW_FSR_PROBE_VECTOR");
    if (vector) {
        CHECK(parse_pair(vector, &vector_x, &vector_y));
        CHECK(isfinite(vector_x) && isfinite(vector_y) &&
              fabsf(vector_x) <= 65504 && fabsf(vector_y) <= 65504);
        constant_vector = 1;
    }
    if (raw_mode) printf("probe-fsr-fg: raw capture inputs from %s (%d frame%s)\n",
                         raw_prefix, raw_count, raw_count == 1 ? "" : "s");
    VkBuffer staging; VmaAllocation memory; VmaAllocationInfo map;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = BYTES * 4,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo host = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
        .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT};
    CHECK(vmaCreateBuffer(c->vma, &bi, &host, &staging, &memory, &map) == VK_SUCCESS);
    uint8_t *bytes = map.pMappedData;
    VkFormat formats[4] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R32_SFLOAT, VK_FORMAT_R16G16_SFLOAT,
                          VK_FORMAT_R8G8B8A8_UNORM};
    VkImage images[4]; VkImageView views[4]; VmaAllocation allocations[4];
    for (int i = 0; i < 4; i++) {
        VkImageCreateInfo im = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
            .format = formats[i], .extent = {W,H,1}, .mipLevels = 1, .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT};
        VmaAllocationCreateInfo device = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        CHECK(vmaCreateImage(c->vma, &im, &device, &images[i], &allocations[i], NULL) == VK_SUCCESS);
        VkImageViewCreateInfo v = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = images[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = formats[i],
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
        CHECK(vkCreateImageView(c->device, &v, NULL, &views[i]) == VK_SUCCESS);
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
    const char *output_path = cfg_get("MKW_FSR_PROBE_OUT");
    VkQueryPool timing_pool = VK_NULL_HANDLE;
    if (raw_mode && queue_timestamps(c)) {
        VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 2};
        CHECK(vkCreateQueryPool(c->device, &qi, NULL, &timing_pool) == VK_SUCCESS);
    }
    int raw_n = raw_mode ? raw_count : last_frame + 1;
    double prepare_sum = 0, record_sum = 0, submit_sum = 0, gpu_sum = 0;
    double prepare_min = 0, record_min = 0, submit_min = 0, gpu_min = 0;
    double prepare_max = 0, record_max = 0, submit_max = 0, gpu_max = 0;
    int timed_gpu = 0;
    for (int slot = 0; slot < raw_n; slot++) {
        uint64_t prepare_start = raw_mode ? perf_begin() : 0;
        int frame = raw_mode ? raw_frames[slot] : slot;
        if (raw_mode) {
            CHECK(read_raw(raw_prefix, 'z', (unsigned)frame, W, H, bytes, BYTES));
            CHECK(read_raw(raw_prefix, 'v', (unsigned)frame, W, H, bytes + BYTES, BYTES));
            if (constant_vector) {
                uint16_t *mv = (uint16_t *)(bytes + BYTES);
                uint16_t x16 = half(vector_x), y16 = half(vector_y);
                for (int p = 0; p < W * H; p++) mv[p * 2] = x16, mv[p * 2 + 1] = y16;
            }
            if (!slot) {
                for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
                    int p = y * W + x;
                    float depth;
                    memcpy(&depth, bytes + (size_t)p * sizeof depth, sizeof depth);
                    int shade = (int)lrintf(fminf(fmaxf(depth, 0), 1) * 255);
                    uint8_t *px = bytes + BYTES * 2 + p * 4;
                    px[0] = (uint8_t)(x * 255u / (W - 1));
                    px[1] = (uint8_t)(y * 255u / (H - 1));
                    px[2] = (uint8_t)shade;
                    px[3] = 255;
                }
            }
        } else {
            for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
                int p = y * W + x;
                for (int k = 0; k < 3; k++) bytes[p * 4 + k] = channel(x, y, frame, k);
                bytes[p * 4 + 3] = 255;
                // Finite perspective near=1, far=100, body at 2 units, road at 4.
                ((float *)(bytes + BYTES))[p] = (100.f - 100.f / (body(x,y) ? 2.f : 4.f)) / 99.f;
                int still = body(x,y) || !strcmp(mode, "zero") || (!strcmp(mode,"image") && shadow(x,y));
                uint16_t *mv = (uint16_t *)(bytes + BYTES * 2) + p * 2;
                mv[0] = still ? 0 : half(constant_vector ? vector_x : motion_x);
                mv[1] = still ? 0 : half(constant_vector ? vector_y : motion_y);
            }
        }
        CHECK(vmaFlushAllocation(c->vma, memory, 0, BYTES * 3) == VK_SUCCESS);
        if (slot) CHECK(vkResetFences(c->device, 1, &fence) == VK_SUCCESS);
        CHECK(vkResetCommandBuffer(cb, 0) == VK_SUCCESS);
        VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        CHECK(vkBeginCommandBuffer(cb, &begin) == VK_SUCCESS);
        if (!slot) for (int i = 0; i < 4; i++) {
            VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = images[i], .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
            VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1,
                .pImageMemoryBarriers = &b};
            vkCmdPipelineBarrier2(cb, &d);
        }
        barrier(cb);
        int first_upload = raw_mode && slot ? 1 : 0;
        for (int i = first_upload; i < 3; i++) {
            VkBufferImageCopy cp = {.bufferOffset = (VkDeviceSize)i * BYTES,
                .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}, .imageExtent = {W,H,1}};
            vkCmdCopyBufferToImage(cb, staging, images[i], VK_IMAGE_LAYOUT_GENERAL, 1, &cp);
        }
        barrier(cb);
        VkTmpFgInputs in = {.depth = images[1], .mv = images[2], .depth_view = views[1], .mv_view = views[2],
            .depth_fmt = VK_FORMAT_R32_SFLOAT, .w = W, .h = H, .reset = !slot,
            .near_ = 1, .far_ = 100, .fov_y = 1.04719755f, .aspect = raw_mode ? (float)W / H : 2,
            .frame_ms = 1000.f/60, .frame = (uint64_t)slot+1};
        in.cam[0] = in.cam[5] = in.cam[10] = 1;
        VkTmpPic color = {images[0],views[0],formats[0],W,H}, out = {images[3],views[3],formats[3],W,H};
        if (timing_pool) {
            vkCmdResetQueryPool(cb, timing_pool, 0, 2);
            vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timing_pool, 0);
        }
        uint64_t record_start = raw_mode ? perf_begin() : 0;
        CHECK(vk_tmp_ffx_fg(cb, &in, &color, NULL, &out));
        double record_ms = raw_mode ? (perf_begin() - record_start) / 1e6 : 0;
        if (timing_pool) vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timing_pool, 1);
        // Experimental provider pipelines must pass shader/descriptor validation before any of their work is submitted.
        CHECK(!vk_validation_errors_total());
        barrier(cb);
        int read_output = !raw_mode || (output_path && slot == raw_n - 1);
        if (read_output) {
            VkBufferImageCopy cp = {.bufferOffset = BYTES * 3,
                .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}, .imageExtent = {W,H,1}};
            vkCmdCopyImageToBuffer(cb, images[3], VK_IMAGE_LAYOUT_GENERAL, staging, 1, &cp);
            VkMemoryBarrier2 read = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT, .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT, .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
            VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1,
                .pMemoryBarriers = &read};
            vkCmdPipelineBarrier2(cb, &di);
        }
        uint64_t submit_start = raw_mode ? perf_begin() : 0;
        CHECK(finish(c, cb, fence));
        double prepare_ms = raw_mode ? (record_start - prepare_start) / 1e6 : 0;
        double submit_ms = raw_mode ? (perf_begin() - submit_start) / 1e6 : 0;
        if (raw_mode) {
            if (!slot || prepare_ms < prepare_min) prepare_min = prepare_ms;
            if (!slot || record_ms < record_min) record_min = record_ms;
            if (!slot || submit_ms < submit_min) submit_min = submit_ms;
            prepare_sum += prepare_ms, record_sum += record_ms, submit_sum += submit_ms;
            if (prepare_ms > prepare_max) prepare_max = prepare_ms;
            if (record_ms > record_max) record_max = record_ms;
            if (submit_ms > submit_max) submit_max = submit_ms;
        }
        if (timing_pool) {
            uint64_t ticks[2];
            VkResult result = vkGetQueryPoolResults(c->device, timing_pool, 0, 2, sizeof ticks, ticks,
                                                    sizeof ticks[0], VK_QUERY_RESULT_64_BIT);
            CHECK(result == VK_SUCCESS);
            if (ticks[1] >= ticks[0]) {
                double gpu_ms = (double)(ticks[1] - ticks[0]) * c->props.limits.timestampPeriod / 1e6;
                if (!timed_gpu || gpu_ms < gpu_min) gpu_min = gpu_ms;
                if (gpu_ms > gpu_max) gpu_max = gpu_ms;
                gpu_sum += gpu_ms, timed_gpu++;
            }
        }
    }
    CHECK(vmaInvalidateAllocation(c->vma, memory, 0, VK_WHOLE_SIZE) == VK_SUCCESS);
    double error[3] = {0}; int count[3] = {0}, bad[3] = {0};
    if (!raw_mode) for (int y = 16; y < H-16; y++) for (int x = 16; x < W-16; x++) {
        int group = body(x,y) ? 0 : shadow(x,y) ? 1 : 2;
        int worst = 0;
        for (int k = 0; k < 3; k++) {
            int e = abs((int)bytes[BYTES*3 + (y*W+x)*4+k] - channel(x,y,last_frame-0.5,k));
            error[group] += e;
            if (e > worst) worst = e;
        }
        count[group]++; bad[group] += worst > 24;
    }
    if (!raw_mode) {
        printf("probe-fsr-fg: %s vectors (%.3f,%.3f), %s shadow, %dx%d analytical midpoint after %d frames; validation errors %ld\n",
            mode,motion_x,motion_y,opaque_shadow?"opaque":"textured",W,H,last_frame,vk_validation_errors_total());
        if (still_images) printf("probe-fsr-fg: identical endpoint images; injected nonzero world vectors intentionally inconsistent\n");
        const char *names[3] = {"body", "shadow", "road"};
        for (int i = 0; i < 3; i++) printf("probe-fsr-fg: %s mean RGB error %.3f /255, >24 max-RGB %d/%d\n",
            names[i], error[i]/(3*count[i]), bad[i], count[i]);
    }
    if (output_path) {
        FILE *f = fopen(output_path, "wb");
        CHECK(f);
        fprintf(f,"P6\n%d %d\n255\n",W,H);
        for (int p = 0; p < W*H; p++) fwrite(bytes+BYTES*3+p*4,1,3,f);
        CHECK(!fclose(f));
    }
    int ok = !vk_validation_errors_total() && (raw_mode || error[2]/(3*count[2]) < 8);
    const char *limit = cfg_get("MKW_FSR_PROBE_SHADOW_LIMIT");
    if (limit && !raw_mode) {
        float maximum = strtof(limit,NULL);
        CHECK(isfinite(maximum) && maximum > 0);
        int quality = error[1]/(3*count[1]) <= maximum && error[0]/(3*count[0]) <= 2;
        printf("probe-fsr-fg: analytical shadow quality gate <= %.3f /255, body <=2: %s\n",maximum,quality?"passed":"FAILED");
        ok = ok && quality;
    } else if (limit) {
        printf("probe-fsr-fg: raw mode ignores MKW_FSR_PROBE_SHADOW_LIMIT; no analytical image-quality comparison\n");
    }
    if (raw_mode) {
        printf("probe-fsr-fg: raw timing mean/min/max: prepare %.3f/%.3f/%.3f ms, "
               "SDK record %.3f/%.3f/%.3f ms, submit+fence %.3f/%.3f/%.3f ms\n",
               prepare_sum/raw_n, prepare_min, prepare_max, record_sum/raw_n, record_min, record_max,
               submit_sum/raw_n, submit_min, submit_max);
        if (timed_gpu)
            printf("probe-fsr-fg: raw provider GPU mean/min/max %.3f/%.3f/%.3f ms; "
                   "%dx%d, %d capture%s, constant vector %s\n",
                   gpu_sum/timed_gpu, gpu_min, gpu_max, W, H, raw_n, raw_n == 1 ? "" : "s",
                   constant_vector ? "enabled" : "disabled");
        else
            printf("probe-fsr-fg: raw provider GPU timing unavailable on this queue; %dx%d, %d capture%s, "
                   "constant vector %s\n", W, H, raw_n, raw_n == 1 ? "" : "s",
                   constant_vector ? "enabled" : "disabled");
        if (constant_vector)
            printf("probe-fsr-fg: raw constant vector values (%.6g,%.6g)\n", vector_x, vector_y);
        printf("probe-fsr-fg: raw integration validation errors %ld; raw captures are timing/integration inputs only\n",
               vk_validation_errors_total());
    }
    if (timing_pool) vkDestroyQueryPool(c->device, timing_pool, NULL);
    vk_tmp_ffx_shutdown();
    vkDestroyFence(c->device,fence,NULL); vkDestroyCommandPool(c->device,pool,NULL);
    for (int i = 0; i < 4; i++) { vkDestroyImageView(c->device,views[i],NULL); vmaDestroyImage(c->vma,images[i],allocations[i]); }
    vmaDestroyBuffer(c->vma,staging,memory);
    rec_resume(recording);
    // The standalone validation harness disables the game's preallocated pools and requests full device teardown.
    // Leaving a live device for validation DLL detach can stall process exit after the measured result is printed.
    const char *shutdown = cfg_get("MKW_FSR_PROBE_SHUTDOWN");
    if (shutdown && strcmp(shutdown,"0")) {
        vk_shutdown();
        if (vk_validation_errors_total()) ok = 0;
    }
    if (raw_mode)
        printf("probe-fsr-fg: raw integration %s (no analytical image-quality claim)\n",ok?"passed":"FAILED");
    else
        printf("probe-fsr-fg: %s (quality metrics above remain separate from this integration check)\n",ok?"passed":"FAILED");
    free(raw_frames);
    return ok ? 0 : 1;
}
