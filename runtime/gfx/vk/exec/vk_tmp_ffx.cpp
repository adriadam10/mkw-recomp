// AMD FSR 3.1 upscaling for vk_temporal.c through AMD's FidelityFX API library, loaded at run time from the
// executable's folder: amd_fidelityfx_loader_vk.dll (FidelityFX SDK 2.x, with its upscaler provider
// amd_fidelityfx_upscaler_vk.dll beside it), else the older single amd_fidelityfx_vk.dll. The API's headers are in
// third_party/ffx_sdk (MIT, AMD). One context per render and output size; FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION lets
// the game's dynamic resolution change the render size below the context's without a new one.
//
// The inputs are vk_temporal.c's: the tonemapped scene (perceptual colour, so FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB
// unless the game draws HDR), the D32 depth (0 near, 1 far), and the motion vectors in render pixels from this frame to
// the last. Every image of the replay stays in VK_IMAGE_LAYOUT_GENERAL, which FFX calls the common state.
#ifdef _WIN32
#include <windows.h>
#include "ffx_game_field_spv.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ffx_api.h"
#include "ffx_upscale.h"
#include "ffx_framegeneration.h"
#include "vk/ffx_api_vk.h"

extern "C" {
#include "cfg.h"
#include "gfx.h"
#include "host.h"
#include "vk.h"
#include "vk_temporal.h"
}

namespace {

struct FfxApi {
    void *lib = nullptr;
    PfnFfxCreateContext create = nullptr;
    PfnFfxDestroyContext destroy = nullptr;
    PfnFfxConfigure configure = nullptr;
    PfnFfxQuery query = nullptr;
    PfnFfxDispatch dispatch = nullptr;
    int tried = 0, ok = 0, legacy = 0;   // legacy: amd_fidelityfx_vk.dll, which takes the deprecated backend descriptor
    char path[1024] = {};
};
FfxApi A;   // the upscaler provider, and the standard frame-generation provider
FfxApi B;   // opt-in MKW_FFX_FG_LIBRARY provider for frame generation only

// a few contexts at once, as vk_temporal.c keeps DLSS features: the game's upscale writes the output texture itself in
// some frames, of another size than the screen target
struct Ctx {
    ffxContext ctx = nullptr;
    uint32_t mw = 0, mh = 0, ow = 0, oh = 0;
    int hdr = -1;
    uint64_t used = 0;
};
struct {
    Ctx c[3];
    int cur = 0, failed = 0;
    uint64_t tick = 0;
    ffxContext ctx = nullptr;   // the current one's
} U;

void message(uint32_t type, const wchar_t *msg) {
    char s[512];
    size_t n = 0;
    for (; msg && msg[n] && n < sizeof s - 1; n++) s[n] = msg[n] < 128 ? (char)msg[n] : '?';
    s[n] = 0;
    static int told;
    if (told++ < 40) rt_log("ffx: %s %s", type == FFX_API_MESSAGE_TYPE_ERROR ? "error:" : "warning:", s);
}

uint32_t surface(VkFormat f) {
    switch (f) {
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32: return FFX_API_SURFACE_FORMAT_R11G11B10_FLOAT;
    case VK_FORMAT_R16G16B16A16_SFLOAT: return FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT;
    case VK_FORMAT_R16G16_SFLOAT: return FFX_API_SURFACE_FORMAT_R16G16_FLOAT;
    case VK_FORMAT_R32_SFLOAT:   // the depth as plain R32F (vk_temporal.c R.zs)
    case VK_FORMAT_D32_SFLOAT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT: return FFX_API_SURFACE_FORMAT_R32_FLOAT;
    case VK_FORMAT_R8G8B8A8_UNORM: return FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_R8G8B8A8_SRGB: return FFX_API_SURFACE_FORMAT_R8G8B8A8_SRGB;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return FFX_API_SURFACE_FORMAT_R10G10B10A2_UNORM;
    default: return FFX_API_SURFACE_FORMAT_UNKNOWN;
    }
}

FfxApiResource res(const VkTmpRes *r, int write) {
    FfxApiResourceDescription d = {};
    d.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    d.format = surface(r->format);
    d.width = r->w, d.height = r->h, d.depth = 1, d.mipCount = 1;
    d.flags = FFX_API_RESOURCE_FLAGS_NONE;
    d.usage = write ? FFX_API_RESOURCE_USAGE_UAV : FFX_API_RESOURCE_USAGE_READ_ONLY;
    return ffxApiGetResourceVK((void *)r->image, d, FFX_API_RESOURCE_STATE_COMMON);
}

void ctx_destroy(void *ctx) {   // completion thread
    ffxContext c = (ffxContext)ctx;
    A.destroy(&c, nullptr);
}

void destroy_ctx() {
    vkDeviceWaitIdle(vk_ctx()->device);
    for (auto &c : U.c)
        if (c.ctx) A.destroy(&c.ctx, nullptr), c.ctx = nullptr;
    U.ctx = nullptr;
}

int context_for(uint32_t mw, uint32_t mh, uint32_t ow, uint32_t oh, int hdr, int *changed) {
    U.tick++;
    for (int i = 0; i < 3; i++) {
        Ctx &x = U.c[i];
        if (x.ctx && x.ow == ow && x.oh == oh && mw <= x.mw && mh <= x.mh && x.hdr == hdr) {
            *changed = i != U.cur;
            U.cur = i, x.used = U.tick, U.ctx = x.ctx;
            return 1;
        }
    }
    int pick = 0;
    for (int i = 0; i < 3; i++) {
        if (!U.c[i].ctx) { pick = i; break; }
        if (U.c[i].used < U.c[pick].used) pick = i;
    }
    if (U.c[pick].ctx) {   // the least used one goes once the GPU is done with the submission being recorded
        vk_tmp_defer(ctx_destroy, (void *)U.c[pick].ctx);
        U.c[pick].ctx = nullptr;
    }
    U.cur = pick, *changed = 1;
    const VkCtx *c = vk_ctx();
    ffxCreateContextDescUpscale d = {};
    d.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    d.flags = FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION | (hdr ? FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE : 0);
    d.maxRenderSize = {mw, mh};
    d.maxUpscaleSize = {ow, oh};
    d.fpMessage = message;
    ffxCreateBackendVKDesc be = {};
    ffxCreateBackendVKDesc_DEPRECATED old = {};
    if (A.legacy) {
        old.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK_DEPRECATED;
        old.vkDevice = c->device, old.vkPhysicalDevice = c->phys, old.vkDeviceProcAddr = vkGetDeviceProcAddr;
        d.header.pNext = &old.header;
    } else {
        be.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
        be.deviceContext.vkDevice = c->device, be.deviceContext.vkPhysicalDevice = c->phys;
        be.deviceContext.vkDeviceProcAddr = vkGetDeviceProcAddr;
        d.header.pNext = &be.header;
    }
    Ctx &x = U.c[pick];
    ffxReturnCode_t r = A.create(&x.ctx, &d.header, nullptr);
    if (r != FFX_API_RETURN_OK) {
        x.ctx = nullptr, U.ctx = nullptr;
        if (U.failed++ < 4) rt_log("ffx: upscaling context %ux%u -> %ux%u could not be created (%u)", mw, mh, ow, oh, r);
        return 0;
    }
    x.mw = mw, x.mh = mh, x.ow = ow, x.oh = oh, x.hdr = hdr, x.used = U.tick;
    U.ctx = x.ctx;
    rt_log("ffx: FSR upscaling context %ux%u -> %ux%u%s", mw, mh, ow, oh, hdr ? ", HDR" : "");
    return 1;
}

#ifdef _WIN32
// Diagnostic source-shader substitution on the standard provider. Exact byte matching keeps another SDK
// version or permutation from receiving a shader with incompatible descriptors or constant-buffer layout.
struct ShaderReplacement {
    uint32_t *original = nullptr;
    const uint32_t *replacement = nullptr;
    size_t original_size = 0, replacement_size = 0;
    int tried = 0, packaged = 0;
} shader_replacement;

uint32_t *read_sdk_shader(const char *path, size_t *size) {
    if (!path || !*path) return nullptr;
    FILE *file = fopen(path, "rb");
    if (!file) return nullptr;
    fseek(file, 0, SEEK_END);
    long length = ftell(file);
    rewind(file);
    uint32_t *data = length >= 20 && length <= 2097152 && !(length % 4)
                         ? (uint32_t *)malloc((size_t)length) : nullptr;
    int ok = data && fread(data, 1, (size_t)length, file) == (size_t)length && data[0] == 0x07230203;
    fclose(file);
    if (!ok) { free(data); return nullptr; }
    *size = (size_t)length;
    return data;
}

VKAPI_ATTR VkResult VKAPI_CALL dump_fg_shader(VkDevice device, const VkShaderModuleCreateInfo *info,
                                             const VkAllocationCallbacks *allocation, VkShaderModule *module) {
    const char *directory = cfg_get("MKW_FFX_SPIRV_DUMP");
    if (directory && *directory && info && info->pCode) {
        static unsigned number;
        char path[1400];
        snprintf(path, sizeof path, "%s/ffx-%03u.spv", directory, number++);
        FILE *file = fopen(path,"wb");
        if (file) {
            size_t written = fwrite(info->pCode,1,info->codeSize,file);
            fclose(file);
            rt_log("ffx[fg]: SDK shader %s, %zu/%zu bytes",path,written,info->codeSize);
        } else rt_log("ffx[fg]: could not write diagnostic SDK shader %s",path);
    }
    auto &patch = shader_replacement;
    if (!patch.tried) {
        patch.tried = 1;
        const char *original = cfg_get("MKW_FFX_REPLACE_ORIGINAL");
        const char *replacement = cfg_get("MKW_FFX_REPLACE_SPIRV");
        patch.packaged = !original && !replacement;
        if (patch.packaged) {
            patch.replacement = ffx_game_field_spv;
            patch.replacement_size = sizeof ffx_game_field_spv;
        } else {
            patch.original = read_sdk_shader(original, &patch.original_size);
            patch.replacement = read_sdk_shader(replacement, &patch.replacement_size);
        }
        if (patch.packaged && patch.replacement) {
            // Reproducible tools/ffx output from the pinned source, patch and compiler. Reject another payload
            // even if it happens to have the same descriptor layout as the original SDK stage.
            static const uint8_t expected[32] = {
                0x1d,0x7e,0x3e,0x8d,0x3b,0x70,0x46,0x76,0x4a,0x2a,0x9f,0x98,0xf5,0xe0,0x41,0x47,
                0xbe,0xfa,0xf4,0xb0,0xea,0x4f,0x9f,0xa0,0x17,0x24,0x2f,0x98,0x24,0xaf,0x5c,0x6e};
            uint8_t digest[32];
            rt_sha256(patch.replacement, patch.replacement_size, digest);
            if (memcmp(digest, expected, sizeof digest)) {
                rt_log("ffx[fg]: packaged game-vector shader does not match the verified source build");
                patch.replacement = nullptr;
            }
        }
        if (!patch.packaged && (!patch.original || !patch.replacement))
            rt_log("ffx[fg]: source-shader substitution unavailable (both valid SPIR-V paths required)");
    }
    int match = info && patch.original && info->codeSize == patch.original_size &&
                !memcmp(info->pCode, patch.original, patch.original_size);
    if (info && patch.packaged && patch.replacement && info->codeSize == 14952) {
        // MIT SDK 7539ae8b, official 3.1.6 FP32 game-vector field: original CB256/UAV128..129 layout.
        // A different shader permutation/version remains untouched; a byte length alone is never sufficient.
        static const uint8_t expected[32] = {
            0x1e,0xc9,0xa6,0x37,0x13,0x64,0x9e,0xa0,0x50,0x35,0x06,0xf8,0x44,0xfa,0xc7,0x64,
            0x98,0x76,0x9d,0xf0,0x0c,0x45,0x2d,0x0b,0x2a,0x62,0xb0,0x7a,0xae,0x15,0x46,0xa0};
        uint8_t digest[32];
        rt_sha256(info->pCode, info->codeSize, digest);
        match = !memcmp(digest, expected, sizeof digest);
    }
    if (match && patch.replacement) {
        VkShaderModuleCreateInfo replaced = *info;
        replaced.pCode = patch.replacement, replaced.codeSize = patch.replacement_size;
        rt_log("ffx[fg]: exact source-shader substitution %zu -> %zu bytes%s", info->codeSize,
               patch.replacement_size, patch.packaged ? " (embedded still-motion priority fix)" : "");
        return vkCreateShaderModule(device, &replaced, allocation, module);
    }
    return vkCreateShaderModule(device,info,allocation,module);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL fg_device_proc(VkDevice device, const char *name) {
    if (!strcmp(name,"vkCreateShaderModule")) return (PFN_vkVoidFunction)dump_fg_shader;
    return vkGetDeviceProcAddr(device,name);
}
#endif

PFN_vkGetDeviceProcAddr fg_proc_address() {
#ifdef _WIN32
    const char *fix = cfg_get("MKW_FFX_STILL_TIE");
    if (!(fix && !strcmp(fix, "0"))) return fg_device_proc;
#endif
    return vkGetDeviceProcAddr;
}

#ifdef _WIN32
void load_api_exports(FfxApi &api, HMODULE lib) {
    api.create = (PfnFfxCreateContext)(void *)GetProcAddress(lib, "ffxCreateContext");
    api.destroy = (PfnFfxDestroyContext)(void *)GetProcAddress(lib, "ffxDestroyContext");
    api.configure = (PfnFfxConfigure)(void *)GetProcAddress(lib, "ffxConfigure");
    api.query = (PfnFfxQuery)(void *)GetProcAddress(lib, "ffxQuery");
    api.dispatch = (PfnFfxDispatch)(void *)GetProcAddress(lib, "ffxDispatch");
    api.ok = api.create && api.destroy && api.configure && api.query && api.dispatch;
}

int absolute_library_path(const char *path) {
    int drive = ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) &&
                path[1] == ':' && (path[2] == '\\' || path[2] == '/');
    int unc = (path[0] == '\\' || path[0] == '/') && (path[1] == '\\' || path[1] == '/');
    return drive || unc;
}

FfxApi *load_explicit_fg_api(const char *override) {
    if (B.tried) return B.ok ? &B : nullptr;
    B.tried = 1;
    snprintf(B.path, sizeof B.path, "%s", override);
    if (!absolute_library_path(B.path)) {
        rt_log("ffx[fg]: explicit library %s is not an absolute path; frame generation is unavailable", B.path);
        return nullptr;
    }
    HMODULE m = LoadLibraryA(B.path);
    if (!m) {
        rt_log("ffx[fg]: explicit library %s could not be loaded (%lu); frame generation is unavailable",
               B.path, GetLastError());
        return nullptr;
    }
    B.lib = m;
    B.legacy = 0;
    load_api_exports(B, m);
    rt_log("ffx[fg]: %s %s", B.path,
           B.ok ? "loaded" : "lacks one or more of the five API entry points: frame generation is unavailable");
    return B.ok ? &B : nullptr;
}
#endif

FfxApi *fg_api() {
    const char *override = cfg_get("MKW_FFX_FG_LIBRARY");
    if (override && *override) {
#ifdef _WIN32
        return load_explicit_fg_api(override);
#else
        return nullptr;
#endif
    }
    if (!vk_tmp_ffx_load()) return nullptr;
    static int told;
    if (!told++)
        rt_log("ffx[fg]: using the upscaler API selection from %s (standard provider)", A.path);
    return &A;
}

}  // namespace

extern "C" int vk_tmp_ffx_load(void) {
    if (A.tried) return A.ok;
    A.tried = 1;
#ifdef _WIN32
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
    while (n && path[n - 1] != '\\') n--;
    static const char *const names[] = {"amd_fidelityfx_loader_vk.dll", "amd_fidelityfx_vk.dll"};
    HMODULE m = nullptr;
    const char *override = cfg_get("MKW_FFX_LIBRARY");
    if (override && *override) {
        // Isolated source-provider experiments must not replace the player's signed DLLs. Explicit override uses
        // the current backend descriptor; an invalid path fails visibly instead of silently testing the stock DLL.
        snprintf(path, sizeof path, "%s", override);
        m = LoadLibraryA(path);
        A.legacy = 0;
        if (!m) rt_log("ffx: explicit diagnostic library %s could not be loaded (%lu)", path, GetLastError());
    } else {
        for (int i = 0; i < 2 && !m; i++) {
            snprintf(path + n, MAX_PATH - n, "%s", names[i]);
            m = LoadLibraryA(path);
            A.legacy = i == 1;
        }
    }
    if (!m) {
        rt_log("ffx: neither amd_fidelityfx_loader_vk.dll nor amd_fidelityfx_vk.dll next to mkw.exe: FSR 3.1 unavailable");
        return 0;
    }
    A.lib = m;
    snprintf(A.path, sizeof A.path, "%s", path);
    load_api_exports(A, m);
    rt_log("ffx[sr]: %s %s", A.path,
           A.ok ? "loaded" : "lacks one or more of the five API entry points: FSR 3.1 unavailable");
#else
    rt_log("ffx: FSR 3.1 is only wired on Windows in this build");
#endif
    return A.ok;
}

extern "C" int vk_tmp_ffx_upscale(VkCommandBuffer cb, const VkTmpRes *color, const VkTmpRes *depth, const VkTmpRes *mv,
                                  const VkTmpRes *out, float jx, float jy, int reset, float frame_ms, float znear,
                                  float zfar, float fov_y, int hdr, float sharpness) {
    if (!A.ok) return 0;
    int changed = 0;
    if (!context_for(color->w, color->h, out->w, out->h, hdr, &changed)) return 0;
    if (changed) reset = 1;
    ffxDispatchDescUpscale d = {};
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    d.commandList = (void *)cb;
    d.color = res(color, 0);
    d.depth = res(depth, 0);
    d.motionVectors = res(mv, 0);
    d.output = res(out, 1);
    d.jitterOffset = {jx, jy};
    d.motionVectorScale = {1.0f, 1.0f};
    d.renderSize = {color->w, color->h};
    d.upscaleSize = {out->w, out->h};
    d.enableSharpening = sharpness > 0;
    d.sharpness = sharpness > 1 ? 1.f : sharpness;
    d.frameTimeDelta = frame_ms;
    d.preExposure = 1.0f;
    d.reset = reset != 0;
    d.cameraNear = znear;
    d.cameraFar = zfar;
    d.cameraFovAngleVertical = fov_y;
    // Same metre-scale camera/depth units as frame generation below (Mario's body spans about 1.74 units).
    d.viewSpaceToMetersFactor = 1.0f;
    d.flags = hdr ? 0 : FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB;
    ffxReturnCode_t r = A.dispatch(&U.c[U.cur].ctx, &d.header);
    if (r != FFX_API_RETURN_OK) {
        if (U.failed++ < 4) rt_log("ffx: upscale dispatch failed (%u)", r);
        return 0;
    }
    return 1;
}

// ---- frame generation: FSR 3.1's interpolation without its swapchain (FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY):
// vk_framegen.c's present thread presents the frame like its own made ones. Each game frame: the prepare pass (depth,
// motion vectors, camera) and the generation, both into the present thread's command buffer.
namespace {
struct {
    ffxContext ctx = nullptr;
    FfxApi *api = nullptr;   // the API that created ctx, and must destroy it
    uint32_t w = 0, h = 0, rw = 0, rh = 0;
    VkFormat fmt = VK_FORMAT_UNDEFINED;
    VkImage flat_depth = VK_NULL_HANDLE;
    VmaAllocation flat_alloc = VK_NULL_HANDLE;
    uint32_t flat_log_w = 0, flat_log_h = 0;
    int flat_layout = 0;
    uint64_t id = 0, last_frame = 0, evals = 0;
    int failed = 0;
} FG;

void fg_destroy() {
    if (!FG.ctx && !FG.flat_depth) return;
    FfxApi *api = FG.api;
    vk_queue_lock();   // the present thread, as vk_framegen.c ensure_images
    vkDeviceWaitIdle(vk_ctx()->device);
    if (FG.flat_depth) vmaDestroyImage(vk_ctx()->vma, FG.flat_depth, FG.flat_alloc);
    vk_queue_unlock();
    if (FG.ctx && api) api->destroy(&FG.ctx, nullptr);
    FG.ctx = nullptr, FG.api = nullptr, FG.flat_depth = VK_NULL_HANDLE, FG.flat_alloc = VK_NULL_HANDLE;
    FG.flat_log_w = FG.flat_log_h = 0, FG.flat_layout = 0;
}

int flat_depth_create(const VkCtx *c, uint32_t w, uint32_t h) {
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                            .format = VK_FORMAT_R32_SFLOAT, .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(c->vma, &ci, &ai, &FG.flat_depth, &FG.flat_alloc, NULL) != VK_SUCCESS) {
        FG.flat_depth = VK_NULL_HANDLE, FG.flat_alloc = VK_NULL_HANDLE;
        rt_log("ffx: diagnostic flat FG depth %ux%u could not be allocated", w, h);
        return 0;
    }
    return 1;
}

void flat_depth_prepare(VkCommandBuffer cb, uint32_t w, uint32_t h) {
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageMemoryBarrier2 b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = FG.flat_layout ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_2_NONE,
        .srcAccessMask = FG.flat_layout ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT : VK_ACCESS_2_NONE,
        .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT, .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .oldLayout = FG.flat_layout ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = FG.flat_depth, .subresourceRange = range};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1,
                          .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &d);
    VkClearColorValue clear = {.float32 = {0.75f, 0.f, 0.f, 0.f}};
    vkCmdClearColorImage(cb, FG.flat_depth, VK_IMAGE_LAYOUT_GENERAL, &clear, 1, &range);
    b.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT, b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    b.oldLayout = b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    vkCmdPipelineBarrier2(cb, &d);
    FG.flat_layout = 1;
    if (FG.flat_log_w != w || FG.flat_log_h != h)
        rt_log("ffx: diagnostic flat FG depth %ux%u cleared to 0.75", w, h), FG.flat_log_w = w, FG.flat_log_h = h;
}

FfxApiResource pic(const VkTmpPic *p, int write) {
    VkTmpRes r = {p->img, p->fmt, p->w, p->h};
    return res(&r, write);
}
}  // namespace

extern "C" int vk_tmp_ffx_fg(VkCommandBuffer cb, const VkTmpFgInputs *in, const VkTmpPic *bb, const VkTmpPic *hl,
                             const VkTmpPic *out) {
    const char *full_mv = cfg_get("MKW_FG_DISPLAY_MV");
    int display_mv = full_mv && strcmp(full_mv, "0") && cfg_get("MKW_FG_NVOF") && strcmp(cfg_get("MKW_FG_NVOF"), "0");
    const char *flat = cfg_get("MKW_FSR_FG_FLAT_DEPTH");
    int use_flat_depth = flat && strcmp(flat, "0");
    if (FG.failed) return 0;
    FfxApi *api = fg_api();
    if (!api) {
        FG.failed = 1;
        return 0;
    }
    const VkCtx *c = vk_ctx();
    if (FG.ctx && (FG.w != bb->w || FG.h != bb->h || FG.fmt != bb->fmt || in->w > FG.rw || in->h > FG.rh)) fg_destroy();
    if (!FG.ctx) {
        ffxCreateContextDescFrameGeneration d = {};
        d.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
        ffxCreateContextDescFrameGenerationVersion api_version = {};
        api_version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
        api_version.version = FFX_FRAMEGENERATION_VERSION;
        int hdr = bb->fmt != VK_FORMAT_R8G8B8A8_UNORM && bb->fmt != VK_FORMAT_R8G8B8A8_SRGB;
        d.flags = hdr ? FFX_FRAMEGENERATION_ENABLE_HIGH_DYNAMIC_RANGE : 0;
        if (display_mv) d.flags |= FFX_FRAMEGENERATION_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS;
        d.displaySize = {bb->w, bb->h};
        // the depth and motion vectors can be larger than the picture shown: DLSS native renders the game's 1440 lines
        // also for a 1080-line screen. A context made for the picture's size alone was made again every frame
        // (in->w > FG.rw), 470 times in a 20 s race at 0.50x game speed (inst-39-own, 20/11/2026).
        uint32_t rw = in->w > bb->w ? in->w : bb->w, rh = in->h > bb->h ? in->h : bb->h;
        d.maxRenderSize = {rw, rh};
        d.backBufferFormat = surface(bb->fmt);
        ffxCreateBackendVKDesc be = {};
        ffxCreateBackendVKDesc_DEPRECATED old = {};
        if (api->legacy) {
            old.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK_DEPRECATED;
            old.vkDevice = c->device, old.vkPhysicalDevice = c->phys, old.vkDeviceProcAddr = fg_proc_address();
            api_version.header.pNext = &old.header;
        } else {
            be.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
            be.deviceContext.vkDevice = c->device, be.deviceContext.vkPhysicalDevice = c->phys;
            be.deviceContext.vkDeviceProcAddr = fg_proc_address();
            api_version.header.pNext = &be.header;
        }
        // SDK 2.1+ requires the application API version in the creation chain for new frame-generation APIs.
        // Keep the backend chained after it so PrepareV2's camera data is interpreted under the declared contract.
        d.header.pNext = &api_version.header;
        // under the queue lock, and finished before it is let go: FFX's backend may submit its own setup work to the
        // device's queue while it makes the context (two runs faulted on a write to address 0 at the first frame)
        vk_queue_lock();
        ffxReturnCode_t r = api->create(&FG.ctx, &d.header, nullptr);
        vkDeviceWaitIdle(c->device);
        if (r == FFX_API_RETURN_OK && use_flat_depth) flat_depth_create(c, bb->w, bb->h);
        vk_queue_unlock();
        if (r != FFX_API_RETURN_OK) {
            FG.ctx = nullptr, FG.failed = 1;
            rt_log("ffx: frame generation context %ux%u could not be created (%u): the port's own frame generation",
                   bb->w, bb->h, r);
            return 0;
        }
        FG.w = bb->w, FG.h = bb->h, FG.rw = rw, FG.rh = rh, FG.fmt = bb->fmt, FG.id = 0;
        FG.api = api;
        rt_log("ffx[fg]: FSR 3.1 frame generation context %ux%u (render up to %ux%u) via %s", bb->w, bb->h, rw, rh,
               api->path);
        if (api->query) {
            ffxQueryGetProviderVersion version = {};
            version.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
            if (api->query(&FG.ctx, &version.header) == FFX_API_RETURN_OK)
                rt_log("ffx[fg]: selected provider %s (id %llu)",
                       version.versionName ? version.versionName : "unnamed", (unsigned long long)version.versionId);
        }
    }
    int reset = in->reset || (FG.last_frame && in->frame != FG.last_frame + 1);
    FG.id++;
    ffxConfigureDescFrameGeneration cf = {};
    cf.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    cf.swapChain = nullptr;
    cf.frameGenerationEnabled = true;
    cf.allowAsyncWorkloads = false;
    cf.flags = FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY;
    // Opt-in SDK diagnostics expose its motion/occlusion decisions in the generated picture. Keep NO_SWAPCHAIN:
    // this port owns presentation even when inspecting the provider's debug view.
    const char *debug = cfg_get("MKW_FSR_FG_DEBUG");
    if (debug && strcmp(debug, "0")) cf.flags |= FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_VIEW;
    cf.generationRect = {0, 0, (int32_t)bb->w, (int32_t)bb->h};
    cf.frameID = FG.id;
    // the picture without the HUD (vk_tmp_fg_hudless; bb's format and size): FSR 3.1 takes the HUD from the
    // difference and keeps it out of the interpolation
    if (hl) cf.HUDLessColor = pic(hl, 0);
    ffxReturnCode_t r = api->configure(&FG.ctx, &cf.header);
    if (r != FFX_API_RETURN_OK) {
        FG.failed = 1;
        rt_log("ffx: frame generation configure failed (%u): the port's own frame generation", r);
        return 0;
    }
    ffxDispatchDescFrameGenerationPrepareV2 pr = {};
    pr.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
    pr.frameID = FG.id;
    pr.commandList = (void *)cb;
    pr.renderSize = {in->w, in->h};
    pr.jitterOffset = {in->jx, in->jy};
    pr.motionVectorScale = {1.0f, 1.0f};
    pr.frameTimeDelta = in->frame_ms;
    pr.reset = reset != 0;
    pr.cameraNear = in->near_;
    pr.cameraFar = in->far_;
    pr.cameraFovAngleVertical = in->fov_y;
    // Game assets use metre-scale positions (Mario's body bounds are about 1.74 units, unit bone scale), matching
    // the race depth's ~4.2-unit camera-to-driver distance. A centimetre conversion shrinks the SDK's scene by 100x.
    pr.viewSpaceToMetersFactor = 1.0f;
    // Diagnostic calibration override for comparing the former centimetre assumption against the measured units.
    const char *meters = cfg_get("MKW_FG_METERS");
    if (meters) {
        float scale = strtof(meters, nullptr);
        if (scale >= 0.0001f && scale <= 100.f) pr.viewSpaceToMetersFactor = scale;
    }
    VkTmpRes rz = {in->depth, VK_FORMAT_R32_SFLOAT, in->w, in->h},
             rm = {in->mv, VK_FORMAT_R16G16_SFLOAT, display_mv ? bb->w : in->w, display_mv ? bb->h : in->h};
    if (use_flat_depth && FG.flat_depth) flat_depth_prepare(cb, in->w, in->h), rz.image = FG.flat_depth;
    pr.depth = res(&rz, 0);
    pr.motionVectors = res(&rm, 0);
    const float *v = in->cam;
    for (int i = 0; i < 3; i++) {
        pr.cameraRight[i] = v[i], pr.cameraUp[i] = v[4 + i], pr.cameraForward[i] = -v[8 + i];
        pr.cameraPosition[i] = -(v[i] * v[3] + v[4 + i] * v[7] + v[8 + i] * v[11]);
    }
    r = api->dispatch(&FG.ctx, &pr.header);
    if (r != FFX_API_RETURN_OK) {
        if (FG.evals < 4) rt_log("ffx: frame generation prepare failed (%u)", r);
        return 0;
    }
    ffxDispatchDescFrameGeneration g = {};
    g.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION;
    g.commandList = (void *)cb;
    g.presentColor = pic(bb, 0);
    g.outputs[0] = pic(out, 1);
    g.numGeneratedFrames = 1;
    g.reset = reset != 0;
    int hdr = bb->fmt != VK_FORMAT_R8G8B8A8_UNORM && bb->fmt != VK_FORMAT_R8G8B8A8_SRGB;
    uint16_t peak = 0, white = 0;
    if (hdr) gfx_hdr_output(&peak, &white);
    g.backbufferTransferFunction = !hdr ? FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB
                                 : bb->fmt == VK_FORMAT_R16G16B16A16_SFLOAT ? FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SCRGB
                                                                          : FFX_API_BACKBUFFER_TRANSFER_FUNCTION_PQ;
    g.minMaxLuminance[0] = hdr ? 0.001f : 0.f, g.minMaxLuminance[1] = hdr ? (peak ? (float)peak : 1000.f) : 1.f;
    g.generationRect = {0, 0, (int32_t)bb->w, (int32_t)bb->h};
    g.frameID = FG.id;
    r = api->dispatch(&FG.ctx, &g.header);
    if (r != FFX_API_RETURN_OK) {
        if (FG.evals < 4) rt_log("ffx: frame generation dispatch failed (%u)", r);
        return 0;
    }
    FG.last_frame = in->frame;
    if (++FG.evals == 1 || FG.evals % 3600 == 0)
        rt_log("ffx: FSR 3.1 frame generation: %llu game frames", (unsigned long long)FG.evals);
    return 1;
}

extern "C" void vk_tmp_ffx_shutdown(void) {
    if (A.ok) destroy_ctx();
    fg_destroy();
}
