// Device-local scratch of task/mesh as compute: the task records, the vertices and
// the triangles a mesh draw writes in its compute passes and reads in its draw, by GPU address (push constants) and as
// the index buffer. In chunks of MESH_CHUNK bytes (or one larger chunk for a larger draw) held by the submission that
// used them and given back when its fence completes (vk_completion_add), as the replay's host scratch is
// (vk_exec_cb.c scratch); MESH_SPARE chunks are kept for reuse. In VRAM: the GPU writes and reads them in the same
// frame, ~75 MB a frame of race on a measured upper bound (meshn1), which over PCIe would cost more than the draws.

#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "vk.h"
#include "platform/platform.h"
#include "mesh_compact_spv.h"

#define MESH_CHUNK (64ull << 20)
#define MESH_SPARE 4

typedef struct {
    VkBuffer buf;
    VmaAllocation alloc;
    VkDeviceAddress addr;
    VkDeviceSize size, used;
} MeshChunk;

typedef struct {
    MeshChunk *c;
    int n, cap;
} MeshHeld;

static PlatMutex lock = PLAT_MUTEX_INIT;   // spare[]
static MeshChunk spare[MESH_SPARE];
static int nspare;
// The chunks of the submission being recorded, per recording thread (MKW_REPLAY_PAR records chunks on several).
static _Thread_local MeshHeld *cur;
static _Thread_local uint64_t cur_fence;

static void give_back(void *arg) {   // completion thread
    MeshHeld *h = arg;
    for (int i = 0; i < h->n; i++) {
        plat_mutex_lock(&lock);
        int kept = h->c[i].size == MESH_CHUNK && nspare < MESH_SPARE;
        if (kept) spare[nspare] = h->c[i], spare[nspare++].used = 0;
        plat_mutex_unlock(&lock);
        if (!kept) vmaDestroyBuffer(vk_ctx()->vma, h->c[i].buf, h->c[i].alloc);
    }
    free(h->c), free(h);
}

static int new_chunk(VkDeviceSize size, MeshChunk *out) {
    plat_mutex_lock(&lock);
    if (size <= MESH_CHUNK && nspare) {
        *out = spare[--nspare];
        plat_mutex_unlock(&lock);
        return 1;
    }
    plat_mutex_unlock(&lock);
    VkCtx *c = vk_ctx();
    VkDeviceSize n = size > MESH_CHUNK ? size : MESH_CHUNK;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = n,
                             .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                                      VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VmaAllocationInfo info;
    if (vmaCreateBuffer(c->vma, &bi, &ai, &out->buf, &out->alloc, &info) != VK_SUCCESS) return 0;
    // Where the chunk landed: with VRAM full AUTO_PREFER_DEVICE may give host memory (wbc1 on a GTX 1050 Ti: the mesh
    // draws 0.2 -> 5-6 ms of GPU a frame with ASTC as BC1, not on the 660M; 08/11/2026).
    static volatile int32_t n_chunks, n_host;
    uint32_t heap = c->mem_props.memoryTypes[info.memoryType].heapIndex;
    int host = !(c->mem_props.memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT);
    int32_t k = plat_inc32(&n_chunks), h = host ? plat_inc32(&n_host) : n_host;
    if (host && h <= 8) rt_log("vk: mesh scratch chunk %d of %llu KiB in host memory (type %u)", k,
                               (unsigned long long)(n >> 10), info.memoryType);
    else if ((k & (k - 1)) == 0) rt_log("vk: mesh scratch: %d chunks, %d in host memory", k, h);
    VkBufferDeviceAddressInfo di = {.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = out->buf};
    out->addr = vkGetBufferDeviceAddress(c->device, &di);
    out->size = n, out->used = 0;
    return 1;
}

int vk_mesh_cs_alloc(uint64_t fence, VkDeviceSize size, VkBuffer *buf, VkDeviceSize *off, VkDeviceAddress *addr) {
    size = (size + 255) & ~255ull;
    if (!cur || cur_fence != fence) {
        if (!(cur = calloc(1, sizeof *cur))) rt_fatal("vk: out of memory (mesh scratch)");
        cur_fence = fence;
        vk_completion_add(fence, give_back, cur);
    }
    MeshChunk *c = cur->n ? &cur->c[cur->n - 1] : NULL;
    if (!c || c->used + size > c->size) {
        if (cur->n == cur->cap) {
            cur->cap = cur->cap ? cur->cap * 2 : 4;
            if (!(cur->c = realloc(cur->c, sizeof *cur->c * (size_t)cur->cap))) rt_fatal("vk: out of memory (mesh scratch)");
        }
        if (!new_chunk(size, &cur->c[cur->n])) return 0;
        c = &cur->c[cur->n++];
    }
    *buf = c->buf, *off = c->used, *addr = c->addr + c->used;
    c->used += size;
    return 1;
}

// Task/mesh draws go through the compute passes on a device without VK_EXT_mesh_shader, and with MKW_MESH_COMPUTE=1
// on one with it (to compare both forms of the same frame); MKW_MESH_COMPUTE=0 leaves them undrawn there.
// AMD's own driver also takes the compute passes by default: on Windows 26.10.x (RX 7600 to 9070 XT, issues #88 #91
// #67 #76 #83) the device is lost ~5 s into the title, Radeon GPU Detective showing a DispatchMesh that never launched
// its workgroups and no page fault; MKW_MESH_COMPUTE=1 runs past it (#88, 11/11/2026). RADV too: the 8 logs with the
// mesh shaders native (#4 RX 7900 GRE, #43 RX 6650 XT, #50 890M, #225 RX 9070; Mesa 25.2.8 to 26.2.6) lose the device
// at the title ~70 log lines after the first task/mesh pipeline, the kernel naming a read fault of the command
// processor (CPG) in RADV's 32-bit window after a barrier; with MKW_MESH_COMPUTE=1 they all run past it (13/11/2026).
int vk_mesh_cs_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = cfg_get("MKW_MESH_COMPUTE");
        on = e && *e ? atoi(e) != 0
                     : !vk_ctx()->has_mesh || vk_caps.driver_id == VK_DRIVER_ID_AMD_PROPRIETARY ||
                           vk_caps.driver_id == VK_DRIVER_ID_MESA_RADV;
        on = on && vk_ctx()->has_buffer_device_address;
        rt_log("vk: task/mesh draws %s", on ? "as compute passes and an indexed draw (MKW_MESH_COMPUTE)"
                                             : vk_ctx()->has_mesh ? "as mesh shaders" : "not drawn (no mesh shaders, no buffer addresses)");
    }
    return on;
}

// The pass that packs the mesh workgroups the task workgroups asked for (runtime/shaders/mesh/mesh_compact.comp): made once,
// its layout only push constants (three addresses and the task count). VK_NULL_HANDLE when it could not be made.
static VkPipeline compact_pipe;
static VkPipelineLayout compact_layout;

static void compact_make(void *arg) {
    (void)arg;
    VkCtx *c = vk_ctx();
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 32};
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .pushConstantRangeCount = 1,
                                     .pPushConstantRanges = &pr};
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof mesh_compact_spv,
                                   .pCode = mesh_compact_spv};
    VkShaderModule mod = VK_NULL_HANDLE;
    if (vkCreatePipelineLayout(c->device, &li, NULL, &compact_layout) != VK_SUCCESS ||
        vkCreateShaderModule(c->device, &mi, NULL, &mod) != VK_SUCCESS) {
        rt_log("vk: task/mesh compaction pipeline not made");
        return;
    }
    VkComputePipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = compact_layout,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = mod, .pName = "main"}};
    if (vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &pi, NULL, &compact_pipe) != VK_SUCCESS) {
        compact_pipe = VK_NULL_HANDLE;
        rt_log("vk: task/mesh compaction pipeline not made");
    }
    vkDestroyShaderModule(c->device, mod, NULL);
}

VkPipeline vk_mesh_cs_compact_pipeline(VkPipelineLayout *layout) {
    static PlatOnce once = PLAT_ONCE_INIT;
    plat_once(&once, compact_make, NULL);
    *layout = compact_layout;
    return compact_pipe;
}
