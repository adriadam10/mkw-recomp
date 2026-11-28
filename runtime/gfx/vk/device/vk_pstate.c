// MKW_GPU_CLOCKS (auto|peak|standard|off|ab; default auto: peak on a discrete AMD GPU on Linux, standard on the Steam
// Deck, off elsewhere): the
// clocks of an AMD GPU held at a stable pstate for the whole run, through the kernel's context op
// AMDGPU_CTX_OP_SET_STABLE_PSTATE on a context of our own (libdrm_amdgpu, loaded at run time; the op the profilers
// use, no root needed). The kernel drops it when the process ends.
//
// Why: amdgpu's automatic clock choice judges a GPU by how busy it is, and the race frame keeps it half idle (the
// replay records while the GPU waits, then the GPU runs while the game waits: a chain, docs/project/roadmap.md section 1). On an
// RX 6700 XT (RADV, game 1.8.0 at 2560x1440, race at a fixed 60 fps) the core sat at its lowest clock, 500 of 2855 MHz,
// with the GPU 50 % busy, and the race took 42.6 ms a frame (tl-now, dpm1); held at the peak pstate (2700 MHz) the
// same race took 16.8 ms, its GPU busy 5.3 ms a frame instead of 19, and the replay's CPU 4.4 ms instead of 8 (pk1,
// 17/11/2026); at the standard one (1825 MHz) 18.5 ms (pk-std2). The same machine raced at 19-22 ms whenever
// something else kept the GPU's clock up, which made runs of one hour twice as slow as the next.
// Issue #496 (7900 XT: 60 fps with MKW_QUEUE_WAITS=0, 50 with its default 1) is the same clock: the queue waits hold
// work a little longer, the GPU idles a little more and amdgpu lowers its clock further. In-run A/B of MKW_QUEUE_WAITS
// and its inverted twin, one player: +15.3 ms a frame (medians) with amdgpu choosing the clock (qw-auto, qw-autoi:
// 18-19 against 33-34 ms), +0.01 ms with the clock held (qw-pk, qw-pki: 16.7-16.8 both), 17/11/2026.
//
// Off by default on integrated GPUs (a Steam Deck, Ryzen APUs): their GPU shares the chip's power budget with the CPU,
// and the peak GPU clock takes it from the game's threads. Radeon 660M (Ryzen 5 6600H, RADV, 1280x720), in-run A/B
// with MKW_FPS=60 and its inverted twin over 40 s of race: +1.9 ms a frame with the peak held (scene balanced), the
// GPU 2.2-3.1 ms faster and the recording CPU 1.1-1.4 ms slower (gc-ab3, gc-ab3i, 17/11/2026); at the game's own
// pace the race stayed at 15 fps either way (gc-ab1). "ab" holds the peak pstate in the B blocks only (MKW_AB).
// The Steam Deck (Van Gogh, RADV names it VANGOGH: LCD and OLED) is the exception, at the standard pstate: amdgpu's
// automatic choice kept its GPU at 200-1040 MHz in split screen, the standard pstate holds 1100 MHz (of 1600, the peak,
// which costs the CPU its clock within the 15 W). 2 players handheld at MKW_FPS=60, game 1.8.0 at 720 lines, separate
// runs in turn (auto, standard, auto, standard; 19/11/2026): 38.5 / 38.4 against 36.4 / 36.2 ms a frame (means),
// 37.8 / 37.1 against 35.5 / 35.0 (medians). A SteamOS per-game GPU clock (a manual level) is left alone, as above.
//
// Only from amdgpu's automatic level: the kernel counts a manual or high level (sysfs power_dpm_force_performance_level,
// set by LACT, CoreCtrl or SteamOS's GPU clock slider) as no stable pstate, and when our context ends it puts back
// what it counted, so the GPU would go back to auto and lose the player's own setting (amdgpu_ctx.c,
// amdgpu_get_stable_pstate and amdgpu_ctx_fini).
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "vk.h"

#if defined(__linux__)
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

enum { PSTATE_NONE = 0, PSTATE_STANDARD = 1, PSTATE_PEAK = 4 };   // AMDGPU_CTX_STABLE_PSTATE_* (amdgpu_drm.h)
enum { OP_SET_STABLE_PSTATE = 6 };                                // AMDGPU_CTX_OP_SET_STABLE_PSTATE

static struct {
    int mode;                 // PSTATE_* held, or -1 (ab: switched each block)
    int set;                  // the pstate set now
    void *ctx;                // amdgpu_context_handle
    int (*op)(void *ctx, uint32_t op, uint32_t flags, uint32_t *out);
} S = {.mode = PSTATE_NONE};

// The render node of the Vulkan device: /sys/class/drm/renderD*/device by its vendor, device id and, when Vulkan
// gives it (VK_EXT_pci_bus_info), its PCI slot. Its name goes to node[] (sized 64).
static int find_node(const VkPhysicalDeviceProperties *p, const char *slot, char *node) {
    DIR *d = opendir("/sys/class/drm");
    if (!d) return 0;
    int found = 0;
    for (struct dirent *e; !found && (e = readdir(d));) {
        if (strncmp(e->d_name, "renderD", 7)) continue;
        char path[300], line[128];
        unsigned vendor = 0, device = 0;
        int slot_ok = !slot[0];
        snprintf(path, sizeof path, "/sys/class/drm/%s/device/uevent", e->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        while (fgets(line, sizeof line, f)) {
            sscanf(line, "PCI_ID=%x:%x", &vendor, &device);
            if (slot[0] && !strncmp(line, "PCI_SLOT_NAME=", 14) && !strncmp(line + 14, slot, strlen(slot))) slot_ok = 1;
        }
        fclose(f);
        if (vendor != p->vendorID || device != p->deviceID || !slot_ok) continue;
        snprintf(node, 64, "%s", e->d_name);
        found = 1;
    }
    closedir(d);
    return found;
}

// amdgpu's performance level of that node ("auto", "manual", "high"...); "" when sysfs does not say
static void dpm_level(const char *node, char *level, size_t n) {
    char path[300];
    snprintf(path, sizeof path, "/sys/class/drm/%s/device/power_dpm_force_performance_level", node);
    level[0] = 0;
    FILE *f = fopen(path, "r");
    if (!f) return;
    if (fgets(level, (int)n, f)) level[strcspn(level, "\n")] = 0;
    fclose(f);
}

// The power caps of the GPU's hwmon (power1_cap and power2_cap, in microwatts) against their defaults. On the Steam
// Deck they are the APU's slow and fast PPT, which Steam's TDP limit lowers: at 8 W instead of 15 W races ran at
// 0.34-0.82x and nothing in the log said why (#753). Logged at start and when a cap changes, read every 300 frames
// (two small sysfs files, so the present thread does not notice).
static struct {
    char dir[300];   // the hwmon directory, "" when there is none
    long cap[2];
    unsigned frames;
} P = {.cap = {-2, -2}};

static long sys_long(const char *dir, const char *name) {
    char path[400];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    long v = -1;
    if (fscanf(f, "%ld", &v) != 1) v = -1;
    fclose(f);
    return v;
}

static void power_check(void) {
    long cap[2] = {sys_long(P.dir, "power1_cap"), sys_long(P.dir, "power2_cap")};
    if (cap[0] == P.cap[0] && cap[1] == P.cap[1]) return;
    P.cap[0] = cap[0], P.cap[1] = cap[1];
    if (cap[0] < 0 && cap[1] < 0) return;
    char text[200] = "", one[96];
    int lowered = 0;
    for (int i = 0; i < 2; i++) {
        if (cap[i] < 0) continue;
        char name[32];
        snprintf(name, sizeof name, "power%d_cap_default", i + 1);
        long def = sys_long(P.dir, name);
        lowered |= def > 0 && cap[i] < def;
        const char *sep = text[0] ? ", " : "";
        if (def > 0) snprintf(one, sizeof one, "%spower%d %.1f W (default %.1f W)", sep, i + 1, cap[i] / 1e6, def / 1e6);
        else snprintf(one, sizeof one, "%spower%d %.1f W", sep, i + 1, cap[i] / 1e6);
        strncat(text, one, sizeof text - strlen(text) - 1);
    }
    rt_log("vk: GPU power caps: %s%s", text,
           lowered ? ": below the default, the GPU and the CPU run slower (a TDP limit such as Steam's)" : "");
}

static void power_init(const char *node) {
    char base[200];
    snprintf(base, sizeof base, "/sys/class/drm/%s/device/hwmon", node);
    DIR *d = opendir(base);
    if (!d) return;
    for (struct dirent *e; (e = readdir(d));)
        if (!strncmp(e->d_name, "hwmon", 5)) {
            snprintf(P.dir, sizeof P.dir, "%s/%s", base, e->d_name);
            break;
        }
    closedir(d);
    if (P.dir[0]) power_check();
}

static void apply(int want) {
    if (!S.ctx || want == S.set) return;
    uint32_t out = 0;
    int r = S.op(S.ctx, OP_SET_STABLE_PSTATE, (uint32_t)want, &out);
    if (r) {
        rt_log("vk: GPU clocks: the stable pstate %d was refused (%d%s); amdgpu chooses them", want, r,
               r == -16 ? ": another process holds one" : "");
        S.ctx = NULL;   // EBUSY: another process holds one; EINVAL: thermal limit or an old kernel. Leave it
        return;
    }
    S.set = want;
}

static int mode_from_cfg(const char *e, int discrete, int deck) {
    if (!e || !*e || !strcmp(e, "auto")) return discrete ? PSTATE_PEAK : deck ? PSTATE_STANDARD : PSTATE_NONE;
    if (!strcmp(e, "peak") || !strcmp(e, "1")) return PSTATE_PEAK;
    if (!strcmp(e, "standard")) return PSTATE_STANDARD;
    if (!strcmp(e, "ab")) return -1;
    return PSTATE_NONE;
}

static int has_pci_bus_info(VkPhysicalDevice phys) {
    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(phys, NULL, &ne, NULL);
    VkExtensionProperties *ep = calloc(ne ? ne : 1, sizeof *ep);
    int has = 0;
    if (ep && !vkEnumerateDeviceExtensionProperties(phys, NULL, &ne, ep))
        for (uint32_t i = 0; i < ne; i++) has |= !strcmp(ep[i].extensionName, VK_EXT_PCI_BUS_INFO_EXTENSION_NAME);
    free(ep);
    return has;
}

// Our own amdgpu context on that node; 0 with the reason in *why
static int open_ctx(const char *node, const char **why) {
    void *lib = dlopen("libdrm_amdgpu.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!lib) { *why = "no libdrm_amdgpu.so.1"; return 0; }
    int (*init)(int, uint32_t *, uint32_t *, void **) = (int (*)(int, uint32_t *, uint32_t *, void **))dlsym(lib, "amdgpu_device_initialize");
    int (*mk)(void *, void **) = (int (*)(void *, void **))dlsym(lib, "amdgpu_cs_ctx_create");
    S.op = (int (*)(void *, uint32_t, uint32_t, uint32_t *))dlsym(lib, "amdgpu_cs_ctx_stable_pstate");
    if (!init || !mk || !S.op) { *why = "libdrm_amdgpu without stable pstates (before 2.4.110)"; return 0; }
    char path[300];
    snprintf(path, sizeof path, "/dev/dri/%s", node);
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) { *why = "the render node did not open"; return 0; }
    uint32_t major, minor;
    void *dev = NULL;
    int r = init(fd, &major, &minor, &dev);
    close(fd);   // amdgpu_device_initialize keeps its own duplicate (libdrm amdgpu_device.c)
    if (r) { *why = "amdgpu_device_initialize failed"; return 0; }
    if (mk(dev, &S.ctx)) { S.ctx = NULL; *why = "amdgpu_cs_ctx_create failed"; return 0; }
    return 1;
}

void vk_pstate_init(VkPhysicalDevice phys) {
    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(phys, &p);
    if (p.vendorID != 0x1002) return;
    const char *e = cfg_get("MKW_GPU_CLOCKS");
    int discrete = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
    int deck = strstr(p.deviceName, "VANGOGH") != NULL;   // the Steam Deck's APU, as RADV names it
    S.mode = mode_from_cfg(e, discrete, deck);
    char slot[32] = "", node[64], level[32];
    if (has_pci_bus_info(phys)) {
        VkPhysicalDevicePCIBusInfoPropertiesEXT pci = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PCI_BUS_INFO_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &pci};
        vkGetPhysicalDeviceProperties2(phys, &p2);
        snprintf(slot, sizeof slot, "%04x:%02x:%02x.%x", pci.pciDomain, pci.pciBus, pci.pciDevice, pci.pciFunction);
    }
    int have_node = find_node(&p, slot, node);
    if (have_node) power_init(node);
    if (S.mode == PSTATE_NONE) {
        rt_log("vk: GPU clocks chosen by amdgpu (MKW_GPU_CLOCKS=%s%s)", e && *e ? e : "auto",
               discrete ? "" : ": off on an integrated GPU, whose CPU loses power to it");
        return;
    }
    if (!have_node) { rt_log("vk: GPU clocks: no render node for this GPU; amdgpu chooses them"); return; }
    dpm_level(node, level, sizeof level);
    if (level[0] && strcmp(level, "auto")) {
        rt_log("vk: GPU clocks left at the level set on this PC (%s: LACT, CoreCtrl, SteamOS or by hand), not held",
               level);
        return;
    }
    const char *why = "";
    if (!open_ctx(node, &why)) { rt_log("vk: GPU clocks: no amdgpu context of our own (%s); amdgpu chooses them", why); return; }
    if (S.mode > 0) apply(S.mode);
    if (S.ctx)
        rt_log("vk: GPU clocks held at the %s stable pstate%s (MKW_GPU_CLOCKS: amdgpu left a GPU this frame chain keeps "
               "half busy at its lowest clock)", S.mode == PSTATE_STANDARD ? "standard" : "peak",
               S.mode < 0 ? " in the B blocks" : "");
}

void vk_pstate_frame(void) {
    if (S.mode < 0 && S.ctx) apply(rt_ab() ? PSTATE_PEAK : PSTATE_NONE);
    if (P.dir[0] && ++P.frames % 300 == 0) power_check();
}

void vk_pstate_shutdown(void) {}
void vk_pstate_activity(void) {}
#elif defined(_WIN32)
#include <stdio.h>
#include "platform/platform.h"
#include "../spv/clk_busy_spv.h"

#define WIN_CLK_GROUPS 2048u

static struct {
    int mode;                    // 0 off, 1 hold/fill, -1 ab (active in B blocks)
    volatile int active, stop;
    volatile uint64_t last_act_ns;
    uint32_t pulse_us, idle_us, gap_us, iters;
    VkDevice device;
    VmaAllocator vma;
    VkQueue queue;
    VkBuffer buf;
    VmaAllocation alloc;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkPipeline pipe;
    VkDescriptorPool dp;
    VkDescriptorSet set;
    VkCommandPool cp;
    VkCommandBuffer cb;
    VkFence fence;
    PlatThread *thr;
} W;

static uint32_t u32_cfg(const char *name, uint32_t def, uint32_t lo, uint32_t hi) {
    const char *e = cfg_get(name);
    if (!e || !*e) return def;
    long long v = atoll(e);
    if (v < (long long)lo) return lo;
    if (v > (long long)hi) return hi;
    return (uint32_t)v;
}

static int win_mode_from_cfg(const char *e, int discrete) {
    if (!e || !*e || !strcmp(e, "auto")) return discrete ? 1 : 0;
    if (!strcmp(e, "peak") || !strcmp(e, "fill") || !strcmp(e, "standard") || !strcmp(e, "1")) return 1;
    if (!strcmp(e, "ab")) return -1;
    return 0;
}

static int submit_pulse(uint32_t iters, double *out_ms) {
    uint64_t t0 = plat_now_ns();
    vkResetCommandBuffer(W.cb, 0);
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    if (vkBeginCommandBuffer(W.cb, &bi) != VK_SUCCESS) return 0;
    vkCmdBindPipeline(W.cb, VK_PIPELINE_BIND_POINT_COMPUTE, W.pipe);
    vkCmdBindDescriptorSets(W.cb, VK_PIPELINE_BIND_POINT_COMPUTE, W.layout, 0, 1, &W.set, 0, NULL);
    vkCmdPushConstants(W.cb, W.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &iters);
    vkCmdDispatch(W.cb, WIN_CLK_GROUPS, 1, 1);
    if (vkEndCommandBuffer(W.cb) != VK_SUCCESS) return 0;
    vkResetFences(W.device, 1, &W.fence);
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &W.cb};
    if (vkQueueSubmit(W.queue, 1, &si, W.fence) != VK_SUCCESS) return 0;
    if (vkWaitForFences(W.device, 1, &W.fence, VK_TRUE, 200000000ull) != VK_SUCCESS) return 0;
    if (out_ms) *out_ms = (double)(plat_now_ns() - t0) / 1e6;
    return 1;
}

static void pstate_worker(void *arg) {
    (void)arg;
    plat_thread_priority(-1);   // below normal: never preempt game or replay threads
    uint32_t iters = 2048;
    double target_ms = (double)W.pulse_us / 1000.0;
    for (int k = 0; k < 4 && !__atomic_load_n(&W.stop, __ATOMIC_ACQUIRE); k++) {
        double ms = 0;
        if (!submit_pulse(iters, &ms)) break;
        if (ms > 0.05) {
            double next = (double)iters * (target_ms / ms) * 0.6 + (double)iters * 0.4;
            iters = next < 64.0 ? 64u : next > 65536.0 ? 65536u : (uint32_t)next;
        }
    }
    W.iters = iters;
    int err_logged = 0;
    while (!__atomic_load_n(&W.stop, __ATOMIC_ACQUIRE)) {
        if (!__atomic_load_n(&W.active, __ATOMIC_ACQUIRE)) {
            plat_wait_ns(5000000ull);
            continue;
        }
        uint64_t last = __atomic_load_n(&W.last_act_ns, __ATOMIC_RELAXED);
        uint64_t now = plat_now_ns();
        if (last && now >= last && (now - last) < (uint64_t)W.idle_us * 1000ull) {
            plat_wait_ns(400000ull);
            continue;
        }
        if (!submit_pulse(W.iters, NULL)) {
            if (!err_logged++) rt_log("vk: GPU clock keeper stopped after async compute submit/wait error");
            break;
        }
        if (W.gap_us) plat_wait_ns((uint64_t)W.gap_us * 1000ull);
    }
}

static void destroy_win_objects(void) {
    if (!W.device) return;
    if (W.fence) vkDestroyFence(W.device, W.fence, NULL);
    if (W.cp) vkDestroyCommandPool(W.device, W.cp, NULL);
    if (W.dp) vkDestroyDescriptorPool(W.device, W.dp, NULL);
    if (W.pipe) vkDestroyPipeline(W.device, W.pipe, NULL);
    if (W.layout) vkDestroyPipelineLayout(W.device, W.layout, NULL);
    if (W.dsl) vkDestroyDescriptorSetLayout(W.device, W.dsl, NULL);
    if (W.buf && W.vma) vmaDestroyBuffer(W.vma, W.buf, W.alloc);
    memset(&W, 0, sizeof W);
}

void vk_pstate_init(VkPhysicalDevice phys) {
    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(phys, &p);
    if (p.vendorID != 0x1002) return;
    const char *e = cfg_get("MKW_GPU_CLOCKS");
    int discrete = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
    W.mode = win_mode_from_cfg(e, discrete);
    if (!W.mode) {
        rt_log("vk: GPU clocks chosen by AMD driver (MKW_GPU_CLOCKS=%s%s)", e && *e ? e : "auto",
               discrete ? "" : ": off on an integrated GPU");
        return;
    }
    VkCtx *c = vk_ctx();
    if (!c || c->clk_queue == VK_NULL_HANDLE || c->clk_family == VK_QUEUE_FAMILY_IGNORED) {
        rt_log("vk: GPU clocks: no dedicated async compute queue; AMD driver chooses them");
        return;
    }
    W.device = c->device, W.vma = c->vma, W.queue = c->clk_queue;
    W.pulse_us = u32_cfg("MKW_CLK_PULSE_US", 500, 100, 5000);
    W.idle_us = u32_cfg("MKW_CLK_IDLE_US", 1500, 100, 20000);
    W.gap_us = W.pulse_us / 2;

    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 4096,
                             .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateBuffer(W.vma, &bi, &ai, &W.buf, &W.alloc, NULL) != VK_SUCCESS) { destroy_win_objects(); return; }
    VkDescriptorSetLayoutBinding lb = {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                       .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                          .bindingCount = 1, .pBindings = &lb};
    if (vkCreateDescriptorSetLayout(W.device, &li, NULL, &W.dsl) != VK_SUCCESS) { destroy_win_objects(); return; }
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 4};
    VkPipelineLayoutCreateInfo pli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                      .pSetLayouts = &W.dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    if (vkCreatePipelineLayout(W.device, &pli, NULL, &W.layout) != VK_SUCCESS) { destroy_win_objects(); return; }
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof clk_busy_spv,
                                   .pCode = clk_busy_spv};
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(W.device, &mi, NULL, &m) != VK_SUCCESS) { destroy_win_objects(); return; }
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"},
                                      .layout = W.layout};
    VkResult r = vkCreateComputePipelines(W.device, VK_NULL_HANDLE, 1, &ci, NULL, &W.pipe);
    vkDestroyShaderModule(W.device, m, NULL);
    if (r != VK_SUCCESS) { destroy_win_objects(); return; }
    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
    VkDescriptorPoolCreateInfo dpi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1,
                                      .poolSizeCount = 1, .pPoolSizes = &ps};
    if (vkCreateDescriptorPool(W.device, &dpi, NULL, &W.dp) != VK_SUCCESS) { destroy_win_objects(); return; }
    VkDescriptorSetAllocateInfo dai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = W.dp,
                                       .descriptorSetCount = 1, .pSetLayouts = &W.dsl};
    if (vkAllocateDescriptorSets(W.device, &dai, &W.set) != VK_SUCCESS) { destroy_win_objects(); return; }
    VkDescriptorBufferInfo dbi = {W.buf, 0, 4096};
    VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = W.set, .descriptorCount = 1,
                              .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &dbi};
    vkUpdateDescriptorSets(W.device, 1, &w, 0, NULL);
    VkCommandPoolCreateInfo cpi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                   .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                   .queueFamilyIndex = c->clk_family};
    if (vkCreateCommandPool(W.device, &cpi, NULL, &W.cp) != VK_SUCCESS) { destroy_win_objects(); return; }
    VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = W.cp,
                                       .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    if (vkAllocateCommandBuffers(W.device, &cai, &W.cb) != VK_SUCCESS) { destroy_win_objects(); return; }
    VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vkCreateFence(W.device, &fci, NULL, &W.fence) != VK_SUCCESS) { destroy_win_objects(); return; }

    W.stop = 0;
    W.active = W.mode > 0 ? 1 : 0;
    W.thr = plat_thread_new(pstate_worker, NULL, "vk pstate");
    if (!W.thr) { destroy_win_objects(); return; }
    rt_log("vk: GPU clock keeper active on async compute family %u (pulse %u us, idle %u us%s; MKW_GPU_CLOCKS)",
           c->clk_family, W.pulse_us, W.idle_us, W.mode < 0 ? ", in the B blocks" : "");
}

void vk_pstate_frame(void) {
    if (W.mode < 0 && W.thr) __atomic_store_n(&W.active, rt_ab() ? 1 : 0, __ATOMIC_RELEASE);
}

void vk_pstate_activity(void) {
    if (W.thr) __atomic_store_n(&W.last_act_ns, plat_now_ns(), __ATOMIC_RELAXED);
}

void vk_pstate_shutdown(void) {
    if (W.thr) {
        __atomic_store_n(&W.stop, 1, __ATOMIC_RELEASE);
        plat_thread_join(W.thr);
        W.thr = NULL;
    }
    destroy_win_objects();
}
#else
void vk_pstate_init(VkPhysicalDevice phys) { (void)phys; }
void vk_pstate_frame(void) {}
void vk_pstate_shutdown(void) {}
void vk_pstate_activity(void) {}
#endif
