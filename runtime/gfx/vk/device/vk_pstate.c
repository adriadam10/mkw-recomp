// MKW_GPU_CLOCKS (auto|peak|standard|off|ab; default auto: peak on a discrete AMD GPU on Linux, off elsewhere): the
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

static int mode_from_cfg(const char *e, int discrete) {
    if (!e || !*e || !strcmp(e, "auto")) return discrete ? PSTATE_PEAK : PSTATE_NONE;
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
    S.mode = mode_from_cfg(e, discrete);
    if (S.mode == PSTATE_NONE) {
        rt_log("vk: GPU clocks chosen by amdgpu (MKW_GPU_CLOCKS=%s%s)", e && *e ? e : "auto",
               discrete ? "" : ": off on an integrated GPU, whose CPU loses power to it");
        return;
    }
    char slot[32] = "", node[64], level[32];
    if (has_pci_bus_info(phys)) {
        VkPhysicalDevicePCIBusInfoPropertiesEXT pci = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PCI_BUS_INFO_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &pci};
        vkGetPhysicalDeviceProperties2(phys, &p2);
        snprintf(slot, sizeof slot, "%04x:%02x:%02x.%x", pci.pciDomain, pci.pciBus, pci.pciDevice, pci.pciFunction);
    }
    if (!find_node(&p, slot, node)) { rt_log("vk: GPU clocks: no render node for this GPU; amdgpu chooses them"); return; }
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
}
#else
void vk_pstate_init(VkPhysicalDevice phys) { (void)phys; }
void vk_pstate_frame(void) {}
#endif
