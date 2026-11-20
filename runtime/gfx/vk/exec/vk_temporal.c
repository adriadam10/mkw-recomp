// Temporal upscaling: NVIDIA DLSS Super Resolution (MKW_TEMPORAL=dlss) and AMD FSR 3.1 (=fsr3) in the place of the
// game's own upscale pass.
//
// Where it runs (measured with MKW_DUMP in races, 15/11/2026). MKW renders its 3D scene and its screen (HUD, menus) at
// two resolutions of its own, from GamePerformanceGraphicsConfigGParam (romfs_res.c): with the 3D lines below the
// screen's, the frame is the scene into an R11G11B10F light target with a D32FS8 depth, bloom, the tonemap pass, SMAA
// (agl's three passes: edges, weights, blend, fs 9e672c00f0cd8eb1 / 68b69c76ad2d7905 / 138f5b8eb8a6b9e0), then one
// compute dispatch (cs 5d920a71d444c084: tex slot 0 the anti-aliased scene, img slot 1 the screen-size target) scales
// the scene into the screen target, and the HUD is drawn over it. With the 3D lines equal to the screen's there is no
// dispatch: the SMAA blend writes the screen target itself. The upscaler takes the place of that dispatch, or of the
// SMAA blend (native: DLAA, FSR 3.1 native AA), with the tonemapped scene before SMAA as its colour (the SMAA blend's
// tex slot 0), so the HUD stays sharp and is never jittered or upscaled.
//
// Jitter: the draws that read the scene's camera block (below) and draw into last frame's scene depth get a sub-pixel
// offset of their viewport (vk_tmp_viewport), a Halton (2, 3) sequence of 8 x the area ratio phases.
//
// Motion vectors: the scene's camera block (vertex c4, 0x980 bytes) holds this frame's and the previous frame's
// matrices: rows 0-2 view, 3-6 view-projection, 7-10 projection, 11-13 inverse view, 14 near and far, 18 the render
// size, and rows 21-31 the previous frame's view, view-projection and projection. shaders/temporal/tmp_mv.comp reprojects every
// depth pixel through prev = VP_prev * inverse(VP_this) (built here in double precision). The camera of the player's
// kart moves with it: MKW_TEMPORAL_KART (default on) keeps the pixels of the kart (close to the camera, bottom middle)
// still instead of moving them like the ground.
//
// Every DLSS or FFX call records into the replay's command buffer after the recorder thread has made every call queued
// before it (rec_drain): they record with their own function pointers, not through the ring. The replay's memo of what
// is bound is dropped afterwards (vk_exec_draw_forget).
//
// NGX is the driver's own (_nvngx.dll, found through the registry), the DLSS model nvngx_dlss.dll next to the
// executable; no NVIDIA SDK code or header is compiled in: the few NGX types used are declared here from its ABI, and
// its parameter object's virtual functions are called through their vtable slots (MSVC's order of the overloads),
// checked with a set-then-get round trip at start.
#ifdef _WIN32
#include <windows.h>
#endif
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "cfg.h"
#include "host.h"
#include "nvn_api.h"
#include "platform/platform.h"
#include "tmp_cas_spv.h"
#include "tmp_mv_spv.h"
#include "vk.h"
#include "vk_draw_internal.h"
#include "vk_exec.h"
#include "vk_rec.h"
#include "vk_temporal.h"

// ---------------------------------------------------------------- settings

static const struct { const char *name; int permille; } qualities[] = {
    {"native", 1000}, {"quality", 667}, {"balanced", 580}, {"performance", 500}, {"ultra", 333},
};

int romfs_resolution_scene_permille(void);   // sdk/fs/romfs_res.c

int vk_tmp_engine(void) {
    const char *v = cfg_value("MKW_TEMPORAL");
    return !strcmp(v, "dlss") ? VK_TMP_DLSS : !strcmp(v, "fsr3") ? VK_TMP_FSR3 : VK_TMP_OFF;
}

int vk_tmp_scene_permille(void) {
    if (vk_tmp_engine() == VK_TMP_OFF) return 0;
    const char *q = cfg_value("MKW_TEMPORAL_QUALITY");
    for (size_t i = 0; i < sizeof qualities / sizeof *qualities; i++)
        if (!strcmp(q, qualities[i].name)) return qualities[i].permille;
    return 667;
}

// ---------------------------------------------------------------- NGX ABI
typedef int NgxResult;   // NVSDK_NGX_Result: 1 success, 0xBAD00000 | n failures
#define NGX_OK(r) (((unsigned)(r) & 0xFFF00000u) != 0xBAD00000u)
#define NGX_VERSION_API 0x15
#define NGX_FEATURE_SUPERSAMPLING 1
#define NGX_APP_ID 231313132ull   // the id NVIDIA's samples use for builds without an issued one
typedef struct NgxParam NgxParam;
typedef struct NgxHandle NgxHandle;
typedef struct { void *slot[17]; } NgxParamVtbl;
struct NgxParam { const NgxParamVtbl *vt; };
typedef struct {   // NVSDK_NGX_ImageViewInfo_VK
    VkImageView view;
    VkImage image;
    VkImageSubresourceRange range;
    VkFormat format;
    unsigned width, height;
} NgxImageView;
typedef struct {   // NVSDK_NGX_Resource_VK
    union { NgxImageView img; struct { VkBuffer buffer; unsigned bytes; } buf; } r;
    int type;      // 0 image view, 1 buffer
    _Bool rw;
} NgxResourceVK;
typedef NgxResult (*PFN_NgxInit)(unsigned long long app, const wchar_t *path, VkInstance, VkPhysicalDevice, VkDevice,
                                 PFN_vkGetInstanceProcAddr, PFN_vkGetDeviceProcAddr, int version, const void *info);
typedef NgxResult (*PFN_NgxReqExt)(unsigned *ni, const char ***ie, unsigned *nd, const char ***de);
typedef NgxResult (*PFN_NgxParams)(NgxParam **);
typedef NgxResult (*PFN_NgxDestroyParams)(NgxParam *);
typedef NgxResult (*PFN_NgxCreate)(VkCommandBuffer, int feature, NgxParam *, NgxHandle **);
typedef NgxResult (*PFN_NgxEvaluate)(VkCommandBuffer, const NgxHandle *, const NgxParam *, void *callback);
typedef NgxResult (*PFN_NgxRelease)(NgxHandle *);
typedef NgxResult (*PFN_NgxShutdown1)(VkDevice);

// The parameter object's vtable, MSVC's layout: each group of overloads in reverse declaration order (the SDK declares
// Set(u64), Set(float), Set(double), Set(u32), Set(int), Set(ID3D11Resource *), Set(ID3D12Resource *), Set(void *),
// then the Get overloads in the same order).
enum { S_P = 0, S_D12, S_D11, S_I, S_U32, S_D, S_F, S_U64, G_P, G_D12, G_D11, G_I, G_U32, G_D, G_F, G_U64 };
typedef void (*SetI)(NgxParam *, const char *, int);
typedef void (*SetU)(NgxParam *, const char *, unsigned);
typedef void (*SetF)(NgxParam *, const char *, float);
typedef void (*SetP)(NgxParam *, const char *, void *);
typedef void (*SetU64)(NgxParam *, const char *, unsigned long long);
typedef NgxResult (*GetI)(const NgxParam *, const char *, int *);
typedef NgxResult (*GetU)(const NgxParam *, const char *, unsigned *);
typedef NgxResult (*GetF)(const NgxParam *, const char *, float *);
typedef NgxResult (*GetP)(const NgxParam *, const char *, void **);
static void p_seti(NgxParam *p, const char *n, int v) { ((SetI)p->vt->slot[S_I])(p, n, v); }
static void p_setu(NgxParam *p, const char *n, unsigned v) { ((SetU)p->vt->slot[S_U32])(p, n, v); }
static void p_setf(NgxParam *p, const char *n, float v) { ((SetF)p->vt->slot[S_F])(p, n, v); }
static void p_setp(NgxParam *p, const char *n, void *v) { ((SetP)p->vt->slot[S_P])(p, n, v); }
static void p_setu64(NgxParam *p, const char *n, unsigned long long v) { ((SetU64)p->vt->slot[S_U64])(p, n, v); }
static int p_geti(const NgxParam *p, const char *n, int *v) { return NGX_OK(((GetI)p->vt->slot[G_I])(p, n, v)); }
static int p_getu(const NgxParam *p, const char *n, unsigned *v) { return NGX_OK(((GetU)p->vt->slot[G_U32])(p, n, v)); }
static int p_getf(const NgxParam *p, const char *n, float *v) { return NGX_OK(((GetF)p->vt->slot[G_F])(p, n, v)); }
static int p_getp(const NgxParam *p, const char *n, void **v) { return NGX_OK(((GetP)p->vt->slot[G_P])(p, n, v)); }

static struct {
    void *core;
    PFN_NgxInit init;
    PFN_NgxReqExt req_ext;
    PFN_NgxParams caps, alloc_params;
    PFN_NgxDestroyParams destroy_params;
    PFN_NgxCreate create;
    PFN_NgxEvaluate evaluate;
    PFN_NgxRelease release;
    PFN_NgxShutdown1 shutdown1;
    int tried, loaded, up, dlss;   // up: NGX initialised on the device; dlss: DLSS available
    int wanted_at_start;           // ngx_wanted said yes (the menu: a missing NGX is not a restart away)
    int fg_max;                    // DLSS Frame Generation: generated frames per rendered one this GPU allows, 0 none
    NgxParam *capsp;
} N;

int vk_tmp_ngx_fg_max(void) { return N.up ? N.fg_max : 0; }

#ifdef _WIN32
// The NGX core next to the driver's Vulkan ICD, when the registry's NGXCore key is missing.
static int ngx_dir_from_adapter(char *dir, DWORD size) {
    HKEY cls;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}",
                      0, KEY_READ, &cls) != ERROR_SUCCESS)
        return 0;
    int found = 0;
    for (DWORD i = 0; !found; i++) {
        char sub[64], val[MAX_PATH + 1] = {0}, probe[MAX_PATH + 32];
        DWORD sn = sizeof sub, vn = MAX_PATH;
        if (RegEnumKeyExA(cls, i, sub, &sn, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
        if (RegGetValueA(cls, sub, "VulkanDriverName", RRF_RT_REG_SZ | RRF_RT_REG_MULTI_SZ, NULL, val, &vn) != ERROR_SUCCESS)
            continue;
        char *slash = strrchr(val, '\\');
        if (!slash) continue;
        *slash = 0;
        snprintf(probe, sizeof probe, "%s\\_nvngx.dll", val);
        if (GetFileAttributesA(probe) == INVALID_FILE_ATTRIBUTES) continue;
        snprintf(dir, size, "%s", val);
        found = 1;
    }
    RegCloseKey(cls);
    return found;
}
#endif

static int ngx_wanted(void) {
    // NGX (NVIDIA's _nvngx.dll in the process, initialised on the device, with instance and device extensions of its
    // own) only when DLSS is asked for at start: MKW_TEMPORAL=dlss, or frame generation with MKW_FG_ENGINE=dlss. Every
    // other player keeps main's process and device (PR 462 review); DLSS picked later in the menu comes after a
    // restart, and the menu says so (vk_tmp_menu_state). MKW_TEMPORAL_NGX=1 forces it, =0 never.
    static int w = -1;
    if (w < 0) {
        const char *v = cfg_get("MKW_TEMPORAL_NGX");
        w = v && *v ? strcmp(v, "0") != 0
                    : !strcmp(cfg_value("MKW_TEMPORAL"), "dlss") ||
                      (vk_fg_wanted() && !strcmp(cfg_value("MKW_FG_ENGINE"), "dlss"));
        if (!w) rt_log("temporal: NGX not loaded: DLSS is not asked for at start");
    }
    return w;
}

static int core_load(void) {
    if (N.tried) return N.loaded;
    N.tried = 1;
    if (!ngx_wanted()) return 0;
    N.wanted_at_start = 1;
#ifdef _WIN32
    char dir[MAX_PATH] = {0}, path[MAX_PATH + 32];
    DWORD n = sizeof dir;
    if (RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore", "FullPath", RRF_RT_REG_SZ,
                     NULL, dir, &n) != ERROR_SUCCESS && !ngx_dir_from_adapter(dir, sizeof dir)) {
        rt_log("temporal: no NVIDIA NGX core on this system (not an NVIDIA driver): DLSS unavailable");
        return 0;
    }
    snprintf(path, sizeof path, "%s\\_nvngx.dll", dir);
    N.core = LoadLibraryA(path);
    if (!N.core) {
        rt_log("temporal: cannot load %s (error %lu): DLSS unavailable", path, GetLastError());
        return 0;
    }
#define FN(f, name) N.f = (void *)GetProcAddress((HMODULE)N.core, name)
    FN(init, "NVSDK_NGX_VULKAN_Init_Ext2");
    FN(req_ext, "NVSDK_NGX_VULKAN_RequiredExtensions");
    FN(caps, "NVSDK_NGX_VULKAN_GetCapabilityParameters");
    FN(alloc_params, "NVSDK_NGX_VULKAN_AllocateParameters");
    FN(destroy_params, "NVSDK_NGX_VULKAN_DestroyParameters");
    FN(create, "NVSDK_NGX_VULKAN_CreateFeature");
    FN(evaluate, "NVSDK_NGX_VULKAN_EvaluateFeature");
    FN(release, "NVSDK_NGX_VULKAN_ReleaseFeature");
    FN(shutdown1, "NVSDK_NGX_VULKAN_Shutdown1");
#undef FN
    if (!N.init || !N.req_ext || !N.caps || !N.alloc_params || !N.create || !N.evaluate || !N.release) {
        rt_log("temporal: the NGX core %s lacks Vulkan entry points: DLSS unavailable", path);
        return 0;
    }
    N.loaded = 1;
    rt_log("temporal: NGX core %s", path);
    return 1;
#else
    rt_log("temporal: DLSS is only wired on Windows in this build");
    return 0;
#endif
}

// Before the device (romfs_res.c, when it writes the game's 3D resolution at start): whether the engine MKW_TEMPORAL
// asks for can come up on this PC. Without one the scene keeps the game's own resolution: scaled down for an engine
// that is not there, the game's bilinear upscale stretched it with nothing said (PR 462 review: all of Linux, AMD
// and Intel with dlss, no DLL next to mkw.exe). DLSS: the NGX core and nvngx_dlss.dll next to mkw.exe; FSR 3.1: AMD's
// DLL loads. A DLSS that still fails on the device is logged by read_cfg.
int vk_tmp_engine_usable(void) {
    int e = vk_tmp_engine();
    if (e == VK_TMP_FSR3) return vk_tmp_ffx_load();
    if (e != VK_TMP_DLSS || !core_load()) return 0;
#ifdef _WIN32
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
    while (n && path[n - 1] != '\\') n--;
    snprintf(path + n, MAX_PATH - n, "nvngx_dlss.dll");
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) return 1;
    rt_log("temporal: no %s: DLSS unavailable", path);
#endif
    return 0;
}

// The menu's note under MKW_TEMPORAL or MKW_FG_ENGINE (key) for its value now: 0 none, 1 after a restart (NGX is
// loaded only when DLSS is asked for at start), 2 not available on this PC.
int vk_tmp_menu_state(const char *key) {
    const char *v = cfg_value(key);
    int fg = !strcmp(key, "MKW_FG_ENGINE");
    if (!strcmp(v, "fsr3")) return vk_tmp_ffx_load() ? 0 : 2;
    if (strcmp(v, "dlss")) return 0;
#ifdef _WIN32
    const VkCtx *c = vk_ctx();
    if (c && c->props.vendorID != 0x10DE) return 2;   // not an NVIDIA GPU
    if (!N.loaded) return N.wanted_at_start ? 2 : 1;
    if (!N.up) return 2;
    return (fg ? N.fg_max : N.dlss) ? 0 : 2;
#else
    (void)fg;
    return 2;
#endif
}

static int listed(const char *const *list, uint32_t n, const char *s) {
    for (uint32_t i = 0; i < n; i++)
        if (!strcmp(list[i], s)) return 1;
    return 0;
}

// NVSDK_NGX_FeatureDiscoveryInfo with an application id (the SDK struct's 64-bit layout), for the instance extensions
// DLSS Frame Generation wants
typedef struct {
    int sdk, feature;
    struct { int type; union { struct { const char *id; int engine; const char *ver; } project; unsigned long long app; } v; } ident;
    const wchar_t *path;
    const void *info;
} NgxDiscovery;
typedef NgxResult (*PFN_FeatInstReq)(const NgxDiscovery *, uint32_t *, VkExtensionProperties **);
typedef NgxResult (*PFN_FeatDevReq)(VkInstance, VkPhysicalDevice, const NgxDiscovery *, uint32_t *, VkExtensionProperties **);
static char fg_names[16][VK_MAX_EXTENSION_NAME_SIZE];   // kept: NGX's list may not outlive the call
static int fg_nnames;

void vk_tmp_instance_exts(const char **ext, uint32_t *n, uint32_t max) {
    unsigned ni = 0, nd = 0;
    const char **ie = NULL, **de = NULL;
    if (!core_load() || !NGX_OK(N.req_ext(&ni, &ie, &nd, &de))) return;
    for (unsigned i = 0; i < ni; i++)
        if (!listed(ext, *n, ie[i]) && *n < max) ext[(*n)++] = ie[i], rt_log("temporal: NGX instance extension %s", ie[i]);
#ifdef _WIN32
    PFN_FeatInstReq req = (PFN_FeatInstReq)(void *)GetProcAddress((HMODULE)N.core,
                                                                  "NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements");
    if (!req) return;
    static const wchar_t here[] = L".";
    NgxDiscovery d;
    memset(&d, 0, sizeof d);
    d.sdk = NGX_VERSION_API, d.feature = 11 /* frame generation */, d.ident.type = 0, d.ident.v.app = NGX_APP_ID, d.path = here;
    uint32_t k = 0;
    VkExtensionProperties *pr = NULL;
    if (NGX_OK(req(&d, &k, &pr)) && pr)
        for (uint32_t i = 0; i < k && fg_nnames < 16; i++) {
            snprintf(fg_names[fg_nnames], sizeof fg_names[0], "%s", pr[i].extensionName);
            const char *nm = fg_names[fg_nnames++];
            if (!listed(ext, *n, nm) && *n < max) ext[(*n)++] = nm, rt_log("temporal: DLSS-G instance extension %s", nm);
        }
#endif
}

static char fgd_names[16][VK_MAX_EXTENSION_NAME_SIZE];
void vk_tmp_device_exts(VkInstance inst, VkPhysicalDevice phys, const VkExtensionProperties *have, uint32_t nhave,
                        const char **ext, uint32_t *n, uint32_t max) {
    unsigned ni = 0, nd = 0;
    const char **ie = NULL, **de = NULL;
    if (!core_load() || !NGX_OK(N.req_ext(&ni, &ie, &nd, &de))) return;
    const char *want[48];
    unsigned nw = 0;
    for (unsigned i = 0; i < nd && nw < 32; i++) want[nw++] = de[i];
#ifdef _WIN32
    // DLSS Frame Generation's own (VK_NV_optical_flow and the like): without them it can only blend the two pictures
    PFN_FeatDevReq req = (PFN_FeatDevReq)(void *)GetProcAddress((HMODULE)N.core,
                                                                "NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements");
    if (req) {
        static const wchar_t here[] = L".";
        NgxDiscovery d;
        memset(&d, 0, sizeof d);
        d.sdk = NGX_VERSION_API, d.feature = 11, d.ident.type = 0, d.ident.v.app = NGX_APP_ID, d.path = here;
        uint32_t k = 0;
        VkExtensionProperties *pr = NULL;
        NgxResult r = req(inst, phys, &d, &k, &pr);
        rt_log("temporal: DLSS-G device extension query %08x: %u", (unsigned)r, k);
        if (NGX_OK(r) && pr)
            for (uint32_t i = 0; i < k && i < 16 && nw < 48; i++) {
                snprintf(fgd_names[i], sizeof fgd_names[0], "%s", pr[i].extensionName);
                want[nw++] = fgd_names[i];
                rt_log("temporal: DLSS-G device extension %s", fgd_names[i]);
            }
    }
#endif
    for (unsigned i = 0; i < nw; i++) {
        int ok = 0;
        for (uint32_t k = 0; k < nhave && !ok; k++) ok = !strcmp(have[k].extensionName, want[i]);
        if (!ok) rt_log("temporal: the device lacks %s, which NGX asks for", want[i]);
        else if (!listed(ext, *n, want[i]) && *n < max) ext[(*n)++] = want[i];
    }
}

static int vtable_ok(void) {
    NgxParam *p = NULL;
    if (!NGX_OK(N.alloc_params(&p)) || !p) return 0;
    int i = 0;
    unsigned u = 0;
    float f = 0;
    void *v = NULL;
    p_seti(p, "MKW.Test.I", -12345);
    p_setu(p, "MKW.Test.U", 4000000000u);
    p_setf(p, "MKW.Test.F", 1.25f);
    p_setp(p, "MKW.Test.P", (void *)p);
    int ok = p_geti(p, "MKW.Test.I", &i) && i == -12345 && p_getu(p, "MKW.Test.U", &u) && u == 4000000000u &&
             p_getf(p, "MKW.Test.F", &f) && f == 1.25f && p_getp(p, "MKW.Test.P", &v) && v == (void *)p;
    if (N.destroy_params) N.destroy_params(p);
    if (!ok) rt_log("temporal: NGX parameter layout check failed (i=%d u=%u f=%g p=%p): DLSS unavailable", i, u, f, v);
    return ok;
}

// NVSDK_NGX_FeatureCommonInfo: MKW_NGX_LOG=1 (or 2, verbose) hands NGX's log lines (DLSS, DLSS-G warnings) to ours
typedef void (*NgxLogFn)(const char *msg, int level, int feature);
typedef struct {
    const wchar_t *const *paths;
    unsigned npaths;
    void *internal;
    NgxLogFn log;
    int min_level;
    _Bool other_sinks_off;
} NgxCommonInfo;
// The NVIDIA App's DLSS Frame Generation override: NGX then loads the App's model, whose made frames come out as an
// even blend of the two game frames (docs/project/upscaling.md). NGX says so in its log, which is always listened to (no files).
static volatile int ngx_fg_override, ngx_log_print;
static void ngx_log(const char *msg, int level, int feature) {
    if (strstr(msg, "Feature dlssg override enabled")) ngx_fg_override = 1;
    if (!ngx_log_print) return;
    size_t n = strlen(msg);
    while (n && (msg[n - 1] == '\n' || msg[n - 1] == '\r')) n--;
    rt_log("ngx[%d/%d]: %.*s", feature, level, (int)n, msg);
}

static void ngx_init(void) {
    if (!core_load()) return;
#ifdef _WIN32
    wchar_t wdir[MAX_PATH];
    DWORD len = GetModuleFileNameW(NULL, wdir, MAX_PATH);
    while (len && wdir[len - 1] != L'\\') len--;
    wdir[len] = 0;
    const VkCtx *c = vk_ctx();
    static NgxCommonInfo ci;
    const char *lg = cfg_get("MKW_NGX_LOG");
    ci.log = ngx_log, ci.min_level = 1, ci.other_sinks_off = 1;
    if (lg && atoi(lg) > 0) ngx_log_print = 1, ci.min_level = atoi(lg) > 1 ? 2 : 1, ci.other_sinks_off = 0;
    NgxResult r = N.init(NGX_APP_ID, wdir, c->instance, c->phys, c->device, vkGetInstanceProcAddr, vkGetDeviceProcAddr,
                         NGX_VERSION_API, &ci);
    if (!NGX_OK(r)) {
        rt_log("temporal: NGX init failed (%08x): DLSS unavailable", (unsigned)r);
        return;
    }
    if (!vtable_ok()) return;
    NgxParam *caps = NULL;
    int avail = 0, driver = 0, fga = 0;
    unsigned mx = 0;
    if (NGX_OK(N.caps(&caps)) && caps) {
        p_geti(caps, "SuperSampling.Available", &avail);
        p_geti(caps, "SuperSampling.NeedsUpdatedDriver", &driver);
        p_geti(caps, "FrameGeneration.Available", &fga);
        if (!p_getu(caps, "DLSSG.MultiFrameCountMax", &mx)) mx = 1;
    }
    N.capsp = caps;
    N.up = 1;
    N.dlss = avail != 0;
    N.fg_max = fga ? (mx < 1 ? 1 : (int)mx) : 0;
    rt_log("temporal: NGX up; DLSS Super Resolution %s%s; DLSS Frame Generation %s (up to %dx)",
           avail ? "available" : "NOT available (is nvngx_dlss.dll next to mkw.exe?)",
           driver ? " (the driver needs an update)" : "", fga ? "available" : "not available", N.fg_max + 1);
#endif
}

// ---------------------------------------------------------------- the frame's state

#define CAM_BYTES 0x980u   // the scene camera block's size (vertex c4)
#define CAM_ROWS 32
// The SMAA passes (1.0.0), by the first 8 bytes of their program key (spirv/<key>.spv): the key comes from the game's
// program, so it holds whatever the translator makes of it. Their SPIR-V's FNV-1a (the translator of main bd367ee;
// the edge pass was a1becfadc5da81ad before it) changed with translate.py's glslc -O0 (b5244df): the blend became
// 87ba66678d075862 and the upscaler never ran on a set-up made with it, so the SPIR-V is only a fallback.
#define KEY_SMAA_BLEND 0x41cee8c6ac1280dfull
#define KEY_SMAA_EDGE 0x6f97a82553fc9820ull
#define KEY_SMAA_WEIGHT 0x8dc1048ac84eb282ull
#define SHADER_SMAA_BLEND 0x138f5b8eb8a6b9e0ull
#define SHADER_SMAA_EDGE 0x9e672c00f0cd8eb1ull
#define SHADER_SMAA_WEIGHT 0x68b69c76ad2d7905ull

typedef struct {
    VkImage image;
    VkFormat format;
    VkImageAspectFlags aspect;
    VkImageUsageFlags usage;
    uint32_t w, h;
    const VkTexImage *tex;   // valid only under the texture lock in the frame it was seen
} Img;

static PlatMutex tlock = PLAT_MUTEX_INIT;   // the camera capture: MKW_REPLAY_PAR records draws on several threads
static struct {
    int engine;                  // this frame's engine (read at the present before it)
    int kart;                    // MKW_TEMPORAL_KART
    int debug;                   // MKW_TEMPORAL_DEBUG: 1 motion vectors, 2 depth, 3 jitter off
    float jsign[2];              // MKW_TEMPORAL_JSIGN: the jitter handed to the upscaler, x and y signs
    float sharp;                 // MKW_TEMPORAL_SHARPNESS / 100
    float cas;                   // MKW_CAS on: MKW_CAS_SHARPNESS / 100, else -1
    int preset;                  // MKW_DLSS_PRESET: 0 NVIDIA's default, else the letter's index + 1
    unsigned cfg_seen;
    int warned_scene;   // read_cfg: the scene scaled down for an engine that is not there, logged once
    // this frame
    int cam_ok;
    float cam[CAM_ROWS * 4];
    uint64_t cam_addr;
    Img depth;                   // the scene's depth (passes with the camera block)
    Img pre;                     // the tonemapped scene before SMAA (the SMAA blend's tex slot 0)
    VkImage smaa_out;            // the SMAA blend's target: what the game's upscale dispatch reads
    uint64_t upscale_frame;      // the last frame with the game's upscale dispatch
    int done;                    // the upscaler ran this frame
    uint64_t frame;              // the game frame being recorded (presents so far)
    // jitter: decided at the present before the frame
    uint32_t jidx;
    float jx, jy;                // this frame's offset in render pixels (x right, y down)
    VkImage jdepth;              // last frame's scene depth: draws into it with the camera take the jitter
    uint32_t jw, jh;
    int draw_jit;                // the draw being recorded takes the jitter (vk_tmp_viewport)
    uint64_t jit_draws;
    // camera history
    double last_vp[16];
    int last_vp_ok;
    int reset;                   // the next evaluation starts a new history
    uint64_t use_fence;          // the last replay submission that used the upscaler's images (release_images)
    uint64_t last_eval_ns;
    // stats, logged every 600 frames
    uint64_t evals, cams_miss, prev_match, prev_differ, fails, plain;
    uint64_t log_frame;
} T = {.jsign = {1, 1}, .cfg_seen = ~0u, .reset = 1};

// The FNV-1a of a program's SPIR-V, of the generic shader for a variant (traces; the SHADER_* fallback). Asked on
// every draw: gfx_shader_spirv_fnv keeps it on the shader.
static uint64_t spv_hash(const GfxShader *s) {
    if (!s) return 0;
    const GfxShader *g = gfx_shader_variant_of(s);
    return gfx_shader_spirv_fnv(g ? g : s);
}

// The first 8 bytes of a program's key, of the generic shader for a variant (variants have keys of their own): the
// KEY_* constants.
static uint64_t prog_key(const GfxShader *s) {
    if (!s) return 0;
    const GfxShader *g = gfx_shader_variant_of(s);
    const uint8_t *k = gfx_shader_key(g ? g : s);
    uint64_t v = 0;
    for (int i = 0; k && i < 8; i++) v = v << 8 | k[i];
    return v;
}

// 0 not an SMAA pass, else 1 the edges, 2 the weights, 3 the blend
static int smaa_pass(const GfxShader *fs) {
    uint64_t k = prog_key(fs);
    if (k == KEY_SMAA_BLEND) return 3;
    if (k == KEY_SMAA_WEIGHT) return 2;
    if (k == KEY_SMAA_EDGE) return 1;
    uint64_t h = spv_hash(fs);
    return h == SHADER_SMAA_BLEND ? 3 : h == SHADER_SMAA_WEIGHT ? 2 : h == SHADER_SMAA_EDGE ? 1 : 0;
}

static void read_cfg(void) {
    unsigned v = cfg_version();
    if (v == T.cfg_seen) return;
    T.cfg_seen = v;
    int asked = vk_tmp_engine();
    T.engine = asked;
    if (T.engine == VK_TMP_DLSS && !N.dlss) T.engine = VK_TMP_OFF;
    if (T.engine == VK_TMP_FSR3 && !vk_tmp_ffx_load()) T.engine = VK_TMP_OFF;
    // the camera block from each submission's own copy (vk_usnap_peek): a host copy of every uniform snapshot, off in
    // main, so only once an engine runs (it stays on after)
    if (T.engine != VK_TMP_OFF) vk_usnap_host_wanted = 1;
    if (asked != VK_TMP_OFF && T.engine == VK_TMP_OFF && romfs_resolution_scene_permille() && !T.warned_scene) {
        // the scene was made smaller at start for an engine that did not come up: the game's own upscale stretches it
        T.warned_scene = 1;
        rt_log("temporal: WARNING: the 3D scene renders at %d/1000 of the screen's lines for an upscaler that is not "
               "available; the game's own upscale stretches it (set MKW_TEMPORAL=off and restart)",
               romfs_resolution_scene_permille());
    }
    T.kart = cfg_bool("MKW_TEMPORAL_KART");
    const char *d = cfg_get("MKW_TEMPORAL_DEBUG");
    T.debug = !d ? 0 : !strcmp(d, "mv") ? 1 : !strcmp(d, "depth") ? 2 : !strcmp(d, "nojitter") ? 3 : 0;
    const char *js = cfg_get("MKW_TEMPORAL_JSIGN");   // "xy", "-xy", "x-y", "-x-y"
    T.jsign[0] = js && js[0] == '-' ? -1.f : 1.f;
    T.jsign[1] = js && strstr(js, "-y") ? -1.f : 1.f;
    T.sharp = (float)cfg_int("MKW_TEMPORAL_SHARPNESS") / 100.f;
    T.cas = !strcmp(cfg_value("MKW_CAS"), "on") ? (float)cfg_int("MKW_CAS_SHARPNESS") / 100.f : -1.f;
    const char *p = cfg_value("MKW_DLSS_PRESET");
    T.preset = p[0] >= 'a' && p[0] <= 'z' && !p[1] ? p[0] - 'a' + 1 : p[0] >= 'A' && p[0] <= 'Z' && !p[1] ? p[0] - 'A' + 1 : 0;
    rt_log("temporal: %s (MKW_TEMPORAL %s, quality %s, 3D at %d/1000 of the screen's lines)%s%s",
           T.engine == VK_TMP_DLSS ? "DLSS" : T.engine == VK_TMP_FSR3 ? "FSR 3.1" : "off", cfg_value("MKW_TEMPORAL"),
           cfg_value("MKW_TEMPORAL_QUALITY"), vk_tmp_scene_permille(), T.kart ? ", kart kept still" : "",
           T.debug ? ", debug view" : "");
}

// ---------------------------------------------------------------- jitter

static float halton(uint32_t i, uint32_t b) {
    float f = 1, r = 0;
    for (uint32_t k = i; k > 0; k /= b) f /= (float)b, r += f * (float)(k % b);
    return r;
}

int vk_tmp_in_order(void) { return __atomic_load_n(&T.engine, __ATOMIC_RELAXED) != VK_TMP_OFF; }

void vk_tmp_viewport(const VkExec *ex, VkViewport *v) {
    (void)ex;
    if (!T.draw_jit) return;
    v->x += T.jx;
    v->y += T.jy;   // the y-flipped viewport (record_fixed): its origin is the bottom row; moving it down moves the picture down
}

static void next_jitter(uint32_t rw, uint32_t ow) {
    float ratio = rw ? (float)ow / (float)rw : 1.f;
    uint32_t phases = (uint32_t)ceilf(8.f * ratio * ratio);
    if (phases < 8) phases = 8;
    if (phases > 64) phases = 64;
    T.jidx = T.jidx % phases + 1;
    T.jx = halton(T.jidx, 2) - 0.5f;
    T.jy = halton(T.jidx, 3) - 0.5f;
}

// ---------------------------------------------------------------- per draw

static int img_of(VkExec *ex, uint64_t h, Img *out) {
    if (!h) return 0;
    view_t v;
    int hv = 0;
    vk_texture_lock();
    const VkTexImage *t = vk_texture_by_id_locked(NVN_HANDLE_TEX(h), &v, &hv);
    if (t) *out = (Img){t->image, t->format, t->aspect, t->usage, t->width, t->height, t};
    vk_texture_unlock();
    (void)ex;
    return t != NULL;
}

static int scene_pass(const VkExec *ex) {
    return ex->depth.image && (ex->depth.format == VK_FORMAT_D32_SFLOAT_S8_UINT || ex->depth.format == VK_FORMAT_D32_SFLOAT) &&
           ex->pass_w >= 320 && ex->pass_h >= 180;
}

static void capture_camera(VkExec *ex) {
    const p_bind_buf *u = &ex->ubo[0][1];   // vertex c4
    if (u->size != CAM_BYTES || !u->addr) return;
    if (T.cam_ok && T.cam_addr == u->addr && (T.depth.image == ex->depth.image || ex->pass_w * ex->pass_h <= T.depth.w * T.depth.h))
        return;   // once per frame: the first pass of the largest size
    const uint8_t *b = vk_usnap_peek(u->addr, CAM_ROWS * 16, 256);
    if (!b) b = (const uint8_t *)(uintptr_t)u->addr;   // guest memory: what the game wrote last
    plat_mutex_lock(&tlock);
    // the largest scene of the frame wins: reflections and the like draw smaller
    if (!T.cam_ok || ex->pass_w * ex->pass_h > T.depth.w * T.depth.h) {
        memcpy(T.cam, b, sizeof T.cam);
        T.cam_ok = 1, T.cam_addr = u->addr;
        T.depth = (Img){ex->depth.image, ex->depth.format, VK_IMAGE_ASPECT_DEPTH_BIT,
                        ex->depth.img ? ex->depth.img->usage : 0, ex->depth.width, ex->depth.height, NULL};
    }
    plat_mutex_unlock(&tlock);
}

static int run_upscaler(VkExec *ex, const Img *color, const Img *out, int via_draw);
static void trace_hash(const char *what, uint64_t h, const VkExec *ex);

int vk_tmp_draw(VkExec *ex, const GfxShader *vs, const GfxShader *fs) {
    int jit = 0;
    if (T.engine != VK_TMP_OFF && vs) {
        const p_bind_buf *u = &ex->ubo[0][1];
        int cam = u->size == CAM_BYTES && u->addr;
        if (cam && scene_pass(ex)) capture_camera(ex);
        // the scene's depth alternates between two images from frame to frame (traced 15/11/2026): a draw with the
        // camera into a depth of last frame's scene size is the scene's
        jit = cam && T.debug != 3 && T.jdepth && scene_pass(ex) && ex->pass_w == T.jw && ex->pass_h == T.jh;
    }
    {
        static int on = -1, n;
        if (on < 0) on = cfg_get("MKW_TEMPORAL_TRACE") ? atoi(cfg_get("MKW_TEMPORAL_TRACE")) : 0;
        if (on && n < 300 && T.frame >= (uint64_t)on && T.frame < (uint64_t)on + 2 && scene_pass(ex) && vs) {
            n++;
            rt_log("temporal: draw f%llu pass %ux%u depth %p jdepth %p vs %016llx ubo sizes %llx %llx %llx %llx %llx jit %d",
                   (unsigned long long)T.frame, ex->pass_w, ex->pass_h, (void *)ex->depth.image, (void *)T.jdepth,
                   (unsigned long long)spv_hash(vs), (unsigned long long)ex->ubo[0][0].size,
                   (unsigned long long)ex->ubo[0][1].size, (unsigned long long)ex->ubo[0][2].size,
                   (unsigned long long)ex->ubo[0][3].size, (unsigned long long)ex->ubo[0][4].size, jit);
        }
    }
    if (jit != T.draw_jit) T.draw_jit = jit, ex->dirty |= VK_EXEC_DIRTY_VIEWPORT;
    if (jit) T.jit_draws++;
    // the SMAA blend is followed also with the upscaler off: the game's own upscale after it draws black in this port
    // (vanilla runs with 3D lines below the screen's, 15/11/2026), and vk_tmp_dispatch then scales it itself
    if (!fs || T.done || (T.engine == VK_TMP_OFF && romfs_resolution_scene_permille() >= 1000)) return 1;
    int smaa = smaa_pass(fs);
    if (smaa) trace_hash("fs-smaa", prog_key(fs), ex);
    if (smaa != 3) return 1;
    // The SMAA blend: its tex slot 0 is the tonemapped scene. With the screen at the scene's size (native) it writes
    // the screen target, and the upscaler takes its place; else the upscale dispatch comes after it.
    Img pre;
    if (!img_of(ex, ex->tex[1][0], &pre)) return 1;
    T.pre = pre;
    T.smaa_out = ex->color[0].image;   // the upscale dispatch reads it (vk_tmp_dispatch)
    if (T.engine == VK_TMP_OFF) return 1;
    if (!ex->color[0].image || ex->color[0].width != pre.w || ex->color[0].height != pre.h) return 1;
    if (!T.cam_ok) return 1;
    // native: is this the screen target? The screen target is the one the HUD draws into, of the screen's size; the
    // blend into a target of the scene's size followed by the upscale dispatch is told apart by MKW_TEMPORAL_QUALITY.
    // with the game's upscale dispatch in the recent frames, the blend writes the scene's own target and the dispatch
    // follows (vk_tmp_dispatch); else the blend writes the screen target and the upscaler goes here
    if (!T.upscale_frame && romfs_resolution_scene_permille() < 1000) T.upscale_frame = T.frame;   // the likely path
    if (T.upscale_frame && T.frame - T.upscale_frame < 30) return 1;
    Img out = {ex->color[0].image, ex->color[0].format, VK_IMAGE_ASPECT_COLOR_BIT,
               ex->color[0].img ? ex->color[0].img->usage : 0, ex->color[0].width, ex->color[0].height, NULL};
    return run_upscaler(ex, &pre, &out, 1) ? 0 : 1;
}

static void trace_hash(const char *what, uint64_t h, const VkExec *ex) {
    static int on = -1, n;
    if (on < 0) on = cfg_get("MKW_TEMPORAL_TRACE") ? atoi(cfg_get("MKW_TEMPORAL_TRACE")) : 0;
    if (!on || n >= 1500 || T.frame < (uint64_t)on) return;
    n++;
    rt_log("temporal: trace frame %llu %s %016llx pass %ux%u", (unsigned long long)T.frame, what, (unsigned long long)h,
           ex->pass_w, ex->pass_h);
}

// The game's upscale scaled by a bilinear blit: the upscaler off, or a frame it cannot run on (no camera: menus).
static int plain_scale(VkExec *ex, const Img *in, const Img *out) {
    vk_exec_pass_end(ex);
    vk_exec_barrier(ex);
    VkImageBlit bl = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{0, 0, 0}, {(int32_t)in->w, (int32_t)in->h, 1}},
                      {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{0, 0, 0}, {(int32_t)out->w, (int32_t)out->h, 1}}};
    vkCmdBlitImage(ex->cb, in->image, VK_IMAGE_LAYOUT_GENERAL, out->image, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, VK_FILTER_LINEAR);
    ex->recorded++;
    vk_exec_barrier(ex);
    T.plain++;
    return 1;
}

int vk_tmp_dispatch(VkExec *ex, const GfxShader *cs) {
    if (!T.smaa_out) return 0;
    // the game's upscale: the first dispatch after the SMAA blend that reads the blend's output (tex slot 0) and writes
    // a larger image (img slot 1). Its program changes with the ratio (5d920a71d444c084 at 1.25x, cc1d4db34f244182 at
    // 1.5x, measured 15/11/2026), so it is known by what it reads and writes.
    Img in, out;
    if (!img_of(ex, ex->tex[NVN_STAGE_CS][0], &in) || in.image != T.smaa_out) return 0;
    T.smaa_out = VK_NULL_HANDLE;
    if (!img_of(ex, ex->img[NVN_STAGE_CS][1], &out) || out.w <= in.w || out.h <= in.h) return 0;
    T.upscale_frame = T.frame;
    if (T.done) return plain_scale(ex, &in, &out);   // the blend took it this frame, before the dispatch was known
    if (T.engine == VK_TMP_OFF) return plain_scale(ex, &in, &out);
    uint64_t hh = spv_hash(cs);
    static uint64_t told[8];
    for (int i = 0; i < 8; i++) {
        if (told[i] == hh) break;
        if (!told[i]) { told[i] = hh; rt_log("temporal: the game's upscale is cs %016llx (%ux%u -> %ux%u)", (unsigned long long)hh, in.w, in.h, out.w, out.h); break; }
    }
    if (!T.cam_ok) { T.cams_miss++; return plain_scale(ex, &in, &out); }
    // the tonemapped scene before SMAA, when the blend ran this frame at the size of the dispatch's input
    const Img *color = T.pre.image && T.pre.w == in.w && T.pre.h == in.h ? &T.pre : &in;
    return run_upscaler(ex, color, &out, 0) || plain_scale(ex, &in, &out);
}

// ---------------------------------------------------------------- the passes' own resources

typedef struct {
    VkImage img;
    VmaAllocation mem;
    VkImageView view;
    uint32_t w, h;
    VkFormat fmt;
    VkImageUsageFlags usage;
} Own;

static struct {
    int ok;   // -1 not built, 0 failed, 1 ready
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkPipeline mv;
    VkPipeline cas;              // contrast adaptive sharpening (tmp_cas.comp), made at first use
    Own casimg;
    VkSampler samp;
    PFN_vkCmdPushDescriptorSetKHR push;
    Own outimg;
    Own big;                     // the screen target at the game output's size (the retarget, below)
    Own prevc;                   // the last evaluation's scene colour (the kart's still test in tmp_mv.comp)
    uint64_t prevc_frame;        // the frame it is of
    Own mvs[3], zs[3];             // the motion vectors and depth of the last three evaluations (frame generation reads
                                 // a frame's after the replay has gone on to the next ones)
} R = {.ok = -1};

#define FG_SLOTS 3
// What each evaluation leaves for frame generation (vk_tmp_fg_inputs), written by the replay and read by the present
// thread: seq is odd while the replay rewrites the slot (every FG_SLOTS frames), so a reader takes a whole slot of one
// frame or none (PR 462 review: read without a lock), and the slot names its own images (retired through
// vk_fg_retire, never freed under a reader)
typedef struct {
    unsigned seq;
    VkImage mv, z;
    VkImageView mvv, zv;
    uint64_t frame;
    uint32_t w, h;
    float jx, jy, znear, zfar, fov, aspect, frame_ms;
    int reset;
    float cam[CAM_ROWS * 4];
    double m[16];                // this frame's clip space to the last's
} FgSlot;
static FgSlot FS[FG_SLOTS];

static void fs_begin(FgSlot *f) {   // the replay
    __atomic_store_n(&f->seq, f->seq + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
}
static void fs_end(FgSlot *f) { __atomic_store_n(&f->seq, f->seq + 1, __ATOMIC_RELEASE); }
static int fs_read(const FgSlot *f, FgSlot *out) {   // the present thread: 1 with a whole slot
    for (int t = 0; t < 4; t++) {
        unsigned q = __atomic_load_n(&f->seq, __ATOMIC_ACQUIRE);
        if (q & 1) continue;
        memcpy(out, (const void *)f, sizeof *out);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&f->seq, __ATOMIC_RELAXED) == q) return 1;
    }
    return 0;
}

// The replay may not wait for the GPU: a submission already queued can wait on a timeline value that only the one
// being recorded signals (a vkDeviceWaitIdle here hung a run). What the GPU may still use goes once the submission
// being recorded is done (vk_completion_add at its fence), and new images go to GENERAL in its command buffer.
static uint64_t tmp_fence;     // the fence of the submission run_upscaler records into
static VkCommandBuffer tmp_cb;

typedef struct { VkImage img; VmaAllocation mem; VkImageView view; } OwnGone;
static void own_free(void *arg) {   // completion thread
    OwnGone *g = arg;
    const VkCtx *c = vk_ctx();
    if (c) vkDestroyImageView(c->device, g->view, NULL), vmaDestroyImage(c->vma, g->img, g->mem);
    free(g);
}

void vk_tmp_defer(void (*fn)(void *), void *arg) {
    if (tmp_fence) vk_completion_add(tmp_fence, fn, arg);
    else fn(arg);   // (outside a recording: nothing of this run uses it)
}

// An image going: once the replay's submission at fence is done (0: at once), and for an image frame generation reads
// on the present thread (fg: the motion vectors, depth and picture without the HUD) also once no generation can read
// it (vk_framegen.c vk_fg_retire), from this game frame on
typedef struct { OwnGone g; uint64_t frame; } OwnFg;   // g first: own_free frees the whole
static void own_fg(void *arg) { OwnFg *x = arg; vk_fg_retire(x->frame, own_free, x); }   // completion thread
static void gone_retire(VkImage img, VmaAllocation mem, VkImageView view, uint64_t fence, int fg) {
    OwnFg *x = malloc(sizeof *x);
    if (!x) rt_fatal("temporal: out of memory");
    x->g = (OwnGone){img, mem, view}, x->frame = T.frame;
    if (fence) vk_completion_add(fence, fg ? own_fg : own_free, x);
    else if (fg) own_fg(x);
    else own_free(x);
}
static void own_retire(Own *o, uint64_t fence, int fg) {
    if (o->img) gone_retire(o->img, o->mem, o->view, fence, fg);
    *o = (Own){0};
}

#define own_image(o, w, h, f) own_image_x(o, w, h, f, 0)   // an image only the replay uses
static int own_image_x(Own *o, uint32_t w, uint32_t h, VkFormat f, int fg) {
    if (o->img && o->w == w && o->h == h && o->fmt == f) return 1;
    const VkCtx *c = vk_ctx();
    if (o->img) own_retire(o, tmp_fence, fg);   // a size change (the game's dynamic resolution, a mode change)
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f,
                            .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(c->phys, f, &fp);
    if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) ci.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) ci.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    o->usage = ci.usage;
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(c->vma, &ci, &ai, &o->img, &o->mem, NULL) != VK_SUCCESS) return 0;
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = o->img,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = f,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (vkCreateImageView(c->device, &vi, NULL, &o->view) != VK_SUCCESS) {
        vmaDestroyImage(c->vma, o->img, o->mem);
        *o = (Own){0};
        return 0;
    }
    o->w = w, o->h = h, o->fmt = f;
    // to GENERAL in the command buffer being recorded (the calls are direct here: run_upscaler drained the recorder)
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                               .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                               .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .image = o->img, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1,
                           .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(tmp_cb, &di);
    return 1;
}

static int res_init(void) {
    if (R.ok >= 0) return R.ok;
    R.ok = 0;
    const VkCtx *c = vk_ctx();
    R.push = (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(c->device, "vkCmdPushDescriptorSetKHR");
    VkDescriptorSetLayoutBinding b[6] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                                           .bindingCount = 6, .pBindings = b};
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 128};
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST,
                              .minFilter = VK_FILTER_NEAREST, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                              .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof tmp_mv_spv,
                                   .pCode = tmp_mv_spv};
    VkShaderModule m = VK_NULL_HANDLE;
    if (!R.push || vkCreateDescriptorSetLayout(c->device, &dci, NULL, &R.dsl) != VK_SUCCESS) goto fail;
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                     .pSetLayouts = &R.dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    if (vkCreatePipelineLayout(c->device, &li, NULL, &R.layout) != VK_SUCCESS ||
        vkCreateSampler(c->device, &si, NULL, &R.samp) != VK_SUCCESS ||
        vkCreateShaderModule(c->device, &mi, NULL, &m) != VK_SUCCESS)
        goto fail;
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"},
                                      .layout = R.layout};
    VkResult r = vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &ci, NULL, &R.mv);
    vkDestroyShaderModule(c->device, m, NULL);
    if (r != VK_SUCCESS) goto fail;
    return R.ok = 1;
fail:
    rt_log("temporal: the motion vector pass could not be set up: the game's own upscaling");
    return 0;
}

static int cas_init(void) {   // the CAS pipeline, on R.layout (binding 3 the picture, 1 the output)
    static int tried;
    if (R.cas || tried) return R.cas != VK_NULL_HANDLE;
    tried = 1;
    const VkCtx *c = vk_ctx();
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof tmp_cas_spv,
                                   .pCode = tmp_cas_spv};
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(c->device, &mi, NULL, &m) != VK_SUCCESS) return 0;
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"},
                                      .layout = R.layout};
    if (vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &ci, NULL, &R.cas) != VK_SUCCESS) R.cas = VK_NULL_HANDLE;
    vkDestroyShaderModule(c->device, m, NULL);
    rt_log("temporal: CAS %s", R.cas ? "ready" : "could not be set up");
    return R.cas != VK_NULL_HANDLE;
}

// ---------------------------------------------------------------- the screen target at the output's size
// MKW caps its screen target (where the upscale writes and the HUD is drawn) at 1440 lines, and its last pass scales it
// to the game output (MKW_RESOLUTION 2160: 3840x2160). With MKW_TEMPORAL_RETARGET (on by default) and an output larger
// than the screen target, the upscaler writes an image of the output's size instead (R.big), and for the rest of the
// frame that image stands in for the screen target: the passes that draw into it (the HUD) draw into the big one with
// their viewport and scissor scaled (vk_tmp_target, vk_tmp_scale_rect), and the draws that sample it (the last pass)
// sample the big one (vk_tmp_sampled). So the upscaler's picture and the HUD reach the output at its own resolution.
static struct {
    int on;                      // this frame: R.big stands in for src
    VkImage src;                 // the game's screen target
    uint32_t sw, sh;             // its size
    uint32_t ow, oh;             // the game output's size, as the last present showed it
    uint64_t swaps, frames;
} RT;

void vk_tmp_present_size(uint32_t w, uint32_t h) { RT.ow = w, RT.oh = h; }

int vk_tmp_target(VkExecTarget *t) {
    if (!RT.on || !t->image || t->image != RT.src) return 0;
    t->image = R.big.img;
    uint64_t uf = vk_exec_recording_fence();
    if (uf > T.use_fence) T.use_fence = uf;
    t->img = NULL;   // view_of makes the view for the command buffer
    t->width = R.big.w, t->height = R.big.h;
    t->level = 0, t->base_layer = 0, t->layers = 1;
    RT.swaps++;
    return 1;
}

void vk_tmp_scale_rect(const VkExec *ex, float *x, float *y, float *w, float *h) {
    if (!RT.on || ex->color[0].image != R.big.img || !RT.sw || !RT.sh) return;
    float sx = (float)R.big.w / (float)RT.sw, sy = (float)R.big.h / (float)RT.sh;
    *x *= sx, *w *= sx, *y *= sy, *h *= sy;
}

// ---------------------------------------------------------------- the picture without the HUD (frame generation)
// DLSS-G and FSR 3.1 frame generation tell the HUD from the scene by a copy of the presented picture without it
// (DLSSG.HUDLess, FSR's HUDLessColor); without one the HUD is warped with the scene: the race timer's last digits came
// out smeared in DLSS-G's made frames (nooverride m1301, 15/11/2026). The game draws its HUD over the upscaler's picture
// in the screen target, and its last pass then scales and encodes that into the window's picture (sRGB, or PQ with
// HDR10). So the upscaler's picture is also blitted into an image of our own (HL.snap) before the HUD, and the game's
// last pass -- the first draw after it that samples the screen target into a target of the presented picture's size -- is
// recorded a second time with HL.snap in the screen target's place, into an image of the window picture's format and
// size (HL.out): the same scale and the same encode as the presented picture, without the HUD (NX2navi encodes its
// HUD-less picture itself, gfx_present.c hudless_record; here the game's own pass does it). Only with frame generation.
static struct {
    Own snap;                    // the upscaler's picture in the screen target's format, before the HUD
    VkImage out;                 // the last pass again, from snap: the window's picture without the HUD
    VmaAllocation out_mem;
    VkFormat out_fmt;            // its image format (the window picture's), rendered through a view of the target's
    uint32_t out_w, out_h;
    VkImage dst;                 // where the upscaler's picture went this frame (the screen target, or R.big)
    uint64_t snap_frame, out_frame, redos;
    VkExecTarget saved;
    int told;
} HL;
static _Thread_local int hl_saw, hl_redo;
// HL.out as the present thread may read it (vk_tmp_fg_hudless, vk_tmp_fg_ui_frame): the replay publishes each finished
// picture under a sequence count (odd while it writes), as FS[] (PR 462 review: HL.out read without a lock)
static struct { unsigned seq; VkImage img; VkFormat fmt; uint32_t w, h; uint64_t frame; } HLP;
static void hlp_publish(VkImage img, VkFormat fmt, uint32_t w, uint32_t h, uint64_t frame) {   // the replay
    __atomic_store_n(&HLP.seq, HLP.seq + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    HLP.img = img, HLP.fmt = fmt, HLP.w = w, HLP.h = h, HLP.frame = frame;
    __atomic_store_n(&HLP.seq, HLP.seq + 1, __ATOMIC_RELEASE);
}
static int hlp_read(VkImage *img, VkFormat *fmt, uint32_t *w, uint32_t *h, uint64_t *frame) {
    for (int t = 0; t < 4; t++) {
        unsigned q = __atomic_load_n(&HLP.seq, __ATOMIC_ACQUIRE);
        if (q & 1) continue;
        *img = HLP.img, *fmt = HLP.fmt, *w = HLP.w, *h = HLP.h, *frame = HLP.frame;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&HLP.seq, __ATOMIC_RELAXED) == q) return 1;
    }
    return 0;
}

void vk_tmp_sampled(VkImage *image, VkImageView *view) {
    VkImage was = *image;
    if (RT.on && *image == RT.src && R.big.view) *image = R.big.img, *view = R.big.view;
    if (hl_redo && HL.dst && *image == HL.dst && HL.snap.view) *image = HL.snap.img, *view = HL.snap.view;
    if (*image != was) {
        uint64_t uf = vk_exec_recording_fence();
        if (uf > T.use_fence) T.use_fence = uf;
    }
}

// The texture descriptor memo (vk_exec_bind.c tex_info) keys on the slot, not on this frame's substitution: kept for
// the screen target, a texture that samples it read the game's small picture once the upscaler ran, or R.big's view
// after it was turned off or remade at another size (a destroyed view). Neither image is kept, and a hit on the screen
// target looks it up again. (Read without a lock as vk_tmp_sampled is: the recording thread sets RT.)
int vk_tmp_memo_skip(VkImage image) { return image && (image == RT.src || image == R.big.img); }

void vk_tmp_hl_seen(VkImage image) {   // VK_NULL_HANDLE: a draw starts
    if (!image) hl_saw = 0;
    else if (image == HL.dst && HL.snap_frame == T.frame) {
        hl_saw = 1;
        static int n;
        if (cfg_get("MKW_HL_TRACE") && T.frame > 1900 && n < 10) n++, rt_log("temporal: hl sampled in f%llu", (unsigned long long)T.frame);
    }
    else if (image == HL.dst) {
        static int n;
        if (cfg_get("MKW_HL_TRACE") && T.frame > 1900 && n < 20) n++, rt_log("temporal: hl seen in f%llu, snap of f%llu", (unsigned long long)T.frame, (unsigned long long)HL.snap_frame);
    }
}

int vk_tmp_hl_redo(void) { return hl_redo; }

static int hl_wanted(void) {
    // MKW_FG_HUDLESS=0: frame generation sees the HUD (to compare); =ab switches it with the MKW_AB blocks
    static int mode = -1;
    if (mode < 0) mode = rt_knob("FG_HUDLESS", "1");
    return vk_fg_wanted() && RT_KNOB_ON(mode);
}

int vk_tmp_hl_redo_begin(VkExec *ex) {
    int saw = hl_saw;
    hl_saw = 0;
    if (!saw || hl_redo || HL.snap_frame != T.frame || HL.out_frame == T.frame) return 0;
    const VkExecTarget *t = &ex->color[0];
    {   // MKW_HL_TRACE=1: why a draw that sampled the upscaler's picture was not taken as the last pass
        static int on = -1, n;
        if (on < 0) on = cfg_get("MKW_HL_TRACE") ? atoi(cfg_get("MKW_HL_TRACE")) : 0;
        if (on && n < 40 && T.frame > 1900)
            n++, rt_log("temporal: hl f%llu draw into %p %ux%u fmt %d ncolor %u depth %p layers %u samples %u is3d %d blocks %d "
                        "level %u (dst %p, src %p, snap %ux%u)", (unsigned long long)T.frame, (void *)t->image, t->width,
                        t->height, (int)t->format, ex->pass_ncolor, (void *)ex->depth.image, t->layers, t->samples, t->is3d,
                        t->blocks, t->level, (void *)HL.dst, (void *)RT.src, HL.snap.w, HL.snap.h);
    }
    if (!t->image || t->image == HL.dst || t->image == RT.src || ex->pass_ncolor != 1 || ex->depth.image ||
        t->width != RT.ow || t->height != RT.oh || t->layers != 1 || t->samples > 1 || t->is3d || t->blocks ||
        t->level || !(t->aspect & VK_IMAGE_ASPECT_COLOR_BIT))
        return 0;
    VkFormat imf = t->img ? t->img->format : t->format;
    const VkCtx *c = vk_ctx();
    if (!HL.out || HL.out_w != t->width || HL.out_h != t->height || HL.out_fmt != imf) {
        if (HL.out) {   // the present thread may still read the last one (HLP): through vk_fg_retire
            gone_retire(HL.out, HL.out_mem, VK_NULL_HANDLE, ex->fence, 1);
            HL.out = VK_NULL_HANDLE;
        }
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = imf,
                                .flags = imf != t->format ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0,
                                .extent = {t->width, t->height, 1}, .mipLevels = 1, .arrayLayers = 1,
                                .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                         VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        if (vmaCreateImage(c->vma, &ci, &ai, &HL.out, &HL.out_mem, NULL) != VK_SUCCESS) {
            HL.out = VK_NULL_HANDLE;
            if (!HL.told++) rt_log("temporal: no memory for the picture without the HUD: frame generation sees the HUD");
            return 0;
        }
        HL.out_w = t->width, HL.out_h = t->height, HL.out_fmt = imf;
        vk_exec_pass_end(ex);
        VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                   .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                   .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                   .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                   .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                   .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .image = HL.out, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1,
                               .pImageMemoryBarriers = &b};
        vkCmdPipelineBarrier2(ex->cb, &di);
        rt_log("temporal: the picture without the HUD at %ux%u (format %d, drawn as %d)", t->width, t->height, (int)imf,
               (int)t->format);
    }
    vk_exec_pass_end(ex);
    HL.saved = ex->color[0];
    VkExecTarget *w = &ex->color[0];
    w->image = HL.out;
    w->img = NULL;   // view_of makes the view (of the target's format) for this command buffer
    hl_redo = 1;
    vk_exec_draw_forget();   // nothing bound is known: the second recording binds its textures (HL.snap) anew
    ex->dirty = VK_EXEC_DIRTY_ALL;
    return 1;
}

void vk_tmp_hl_redo_end(VkExec *ex) {
    vk_exec_pass_end(ex);
    ex->color[0] = HL.saved;
    ex->rt_resolved = 0;   // the game's targets again, resolved at the next draw
    hl_redo = 0;
    vk_exec_draw_forget();
    ex->dirty = VK_EXEC_DIRTY_ALL;
    HL.out_frame = T.frame;
    hlp_publish(HL.out, HL.out_fmt, HL.out_w, HL.out_h, T.frame);
    if (HL.redos++ % 1800 == 0)
        rt_log("temporal: the last pass recorded again without the HUD (%llu frames)", (unsigned long long)HL.redos);
}

int vk_tmp_fg_hudless(uint64_t frame, VkTmpPic *out) {   // the present thread
    VkImage img;
    VkFormat fmt;
    uint32_t w, h;
    uint64_t f;
    if (!frame || !hlp_read(&img, &fmt, &w, &h, &f) || f != frame || !img) return 0;
    *out = (VkTmpPic){img, VK_NULL_HANDLE, fmt, w, h};
    return 1;
}

// A game frame with the upscaler on and no picture without the HUD: no pass of it sampled the upscaler's picture, so
// it is a menu or another full-screen 2D layer (the pause menu, the course vote after a race, the title's menus).
// Frame generation would interpolate the menu itself: its buttons change colour and its cursor jumps between two game
// frames with no motion to follow, and the made frames tore them into blocks and smeared them (pause6, 16/11/2026: the
// "Resume" button in red and white rectangles between two clean game frames; the player: "flash/flicker like crazy").
// Only once a picture without the HUD has been made (HL.out): an upscaler that never ran (it failed every frame:
// ui1, DLSS native at 3840x1820 HDR, bad00005) made none, and every frame would have counted as a menu.
int vk_tmp_fg_ui_frame(uint64_t frame) {   // the present thread
    VkImage img;
    VkFormat fmt;
    uint32_t w, h;
    uint64_t f;
    if (vk_tmp_engine() == VK_TMP_OFF || !hl_wanted() || !hlp_read(&img, &fmt, &w, &h, &f)) return 0;
    return img && f != frame;
}

static int retarget_wanted(uint32_t sw, uint32_t sh) {
    static int on = -1;
    if (on < 0) on = cfg_get("MKW_TEMPORAL_RETARGET") ? strcmp(cfg_get("MKW_TEMPORAL_RETARGET"), "0") != 0 : 1;
    // larger in both directions; with MKW_ASPECT wider than 16:9 the game keeps 16:9 targets for a wider camera and its
    // last pass stretches them (3840x1820 from 2560x1440 on a 2.11 window): the big target is the output's shape, and
    // the upscaler and the HUD's passes stretch the same way
    return on && RT.ow >= sw && RT.oh >= sh && (RT.ow > sw || RT.oh > sh);
}

// ---------------------------------------------------------------- matrices

static void rows4(const float *cam, int row, double m[16]) {   // 4 rows of the camera block, row-major
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 4; k++) m[r * 4 + k] = cam[(row + r) * 4 + k];
}

static int inv4(const double m[16], double o[16]) {
    double inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    double det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (fabs(det) < 1e-30) return 0;
    for (int i = 0; i < 16; i++) o[i] = inv[i] / det;
    return 1;
}

static void mul4(const double a[16], const double b[16], double o[16]) {   // row-major a * b
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            double s = 0;
            for (int k = 0; k < 4; k++) s += a[r * 4 + k] * b[k * 4 + c];
            o[r * 4 + c] = s;
        }
}

// ---------------------------------------------------------------- the evaluation

typedef struct {
    NgxHandle *feat;
    uint32_t w, h, ow, oh;   // render size it was created for (the largest it takes), output size
    int q, preset, hdr;
} Feat;
// A few features at once: in some frames (HDR10 output, no HUD) the game's upscale writes the output texture itself, of
// another size than the screen target, and a feature for each kept the history of both (one feature made anew at
// each switch waited for the GPU each time).
#define FEATS 3
static Feat FC[FEATS];
static int fcur;
static uint64_t fused[FEATS], ftick;
#define F (FC[fcur])

static int perf_quality(uint32_t w, uint32_t ow) {   // NVSDK_NGX_PerfQuality_Value from the area's ratio
    float r = (float)w / (float)ow;
    return r >= 0.95f ? 5 /* DLAA */ : r >= 0.64f ? 2 /* quality */ : r >= 0.55f ? 1 /* balanced */
         : r >= 0.45f ? 0 /* performance */ : 3 /* ultra performance */;
}

static void feat_release(void *f) { N.release((NgxHandle *)f); }   // completion thread

static int dlss_feature(VkCommandBuffer cb, uint32_t w, uint32_t h, uint32_t ow, uint32_t oh, int hdr) {
    // created for the largest render size the setting gives (the game's dynamic resolution only goes below it), so a
    // smaller frame runs on the same feature with its render subrectangle
    int pm = vk_tmp_scene_permille();
    uint32_t mw = pm ? (ow * (uint32_t)pm + 999) / 1000 : w, mh = pm ? (oh * (uint32_t)pm + 999) / 1000 : h;
    if (mw < w) mw = w;
    if (mh < h) mh = h;
    if (mw > ow) mw = ow;
    if (mh > oh) mh = oh;
    // The setting's share is of the game's screen lines, not of the output: with the output retargeted past the
    // screen (3840x1820 from a 1440-line screen) "native" gave a 3840x1820 feature, DLAA, whose dynamic range is
    // within ~1% of the output (NGX: "RenderSubrect (2560x1440) outside of Min (3802x1802) and Max (3840x1820)"), and
    // every evaluation of the 2560x1440 frames failed (bad00005; ui2, 16/11/2026): no upscale, no picture without the
    // HUD for frame generation. A frame under DLAA's band gets a feature of its own size and mode.
    if (perf_quality(mw, ow) == 5 && perf_quality(w, ow) != 5) mw = w, mh = h;
    ftick++;
    for (int i = 0; i < FEATS; i++) {
        Feat *e = &FC[i];
        if (e->feat && e->ow == ow && e->oh == oh && w <= e->w && h <= e->h && e->preset == T.preset && e->hdr == hdr) {
            if (i != fcur) T.reset = 1;   // its history is of an earlier frame
            fcur = i, fused[i] = ftick;
            return 1;
        }
    }
    int pick = 0;
    for (int i = 0; i < FEATS; i++) {
        if (!FC[i].feat) { pick = i; break; }
        if (fused[i] < fused[pick]) pick = i;
    }
    fcur = pick, fused[pick] = ftick;
    w = mw, h = mh;
    int q = perf_quality(w, ow);
    if (F.feat) {   // the least used one goes once the GPU is done with what used it
        vk_tmp_defer(feat_release, F.feat);
        F.feat = NULL;
    }
    NgxParam *p = NULL;
    if (!NGX_OK(N.alloc_params(&p)) || !p) return 0;
    p_setu(p, "CreationNodeMask", 1);
    p_setu(p, "VisibilityNodeMask", 1);
    p_setu(p, "Width", w);
    p_setu(p, "Height", h);
    p_setu(p, "OutWidth", ow);
    p_setu(p, "OutHeight", oh);
    p_seti(p, "PerfQualityValue", q);
    // MVLowRes (motion vectors at the render size) | IsHDR when the scene's colours go above 1
    p_seti(p, "DLSS.Feature.Create.Flags", (1 << 1) | (hdr ? 1 << 0 : 0));
    p_seti(p, "DLSS.Enable.Output.Subrects", 0);
    static const char *const presets[] = {"DLSS.Hint.Render.Preset.DLAA", "DLSS.Hint.Render.Preset.UltraQuality",
                                          "DLSS.Hint.Render.Preset.Quality", "DLSS.Hint.Render.Preset.Balanced",
                                          "DLSS.Hint.Render.Preset.Performance",
                                          "DLSS.Hint.Render.Preset.UltraPerformance"};
    for (size_t i = 0; i < sizeof presets / sizeof *presets; i++) p_setu(p, presets[i], (unsigned)T.preset);
    NgxResult r = N.create(cb, NGX_FEATURE_SUPERSAMPLING, p, &F.feat);
    if (N.destroy_params) N.destroy_params(p);
    if (!NGX_OK(r) || !F.feat) {
        F.feat = NULL;
        rt_log("temporal: DLSS feature %ux%u -> %ux%u (quality %d) could not be created (%08x)", w, h, ow, oh, q, (unsigned)r);
        return 0;
    }
    F.w = w, F.h = h, F.ow = ow, F.oh = oh, F.q = q, F.preset = T.preset, F.hdr = hdr;
    T.reset = 1;
    rt_log("temporal: DLSS feature %ux%u -> %ux%u, quality %d, preset %d%s", w, h, ow, oh, q, T.preset, hdr ? ", HDR" : "");
    return 1;
}

static NgxResourceVK res_image(VkImageView v, VkImage img, VkFormat f, uint32_t w, uint32_t h, VkImageAspectFlags a, int rw) {
    NgxResourceVK r;
    memset(&r, 0, sizeof r);
    r.r.img = (NgxImageView){v, img, {a, 0, 1, 0, 1}, f, w, h};
    r.type = 0;
    r.rw = rw != 0;
    return r;
}

static VkImageView game_view(const Img *i, VkImageAspectFlags aspect) {
    // vk_exec_target_view keys the view by the image and frees it with the image (vk_exec_image_gone)
    VkTexImage tmp;
    memset(&tmp, 0, sizeof tmp);
    tmp.image = i->image, tmp.format = i->format, tmp.aspect = i->aspect, tmp.usage = i->usage;
    tmp.width = i->w, tmp.height = i->h, tmp.depth = 1, tmp.levels = 1, tmp.layers = 1;
    return vk_exec_target_view(&tmp, i->format, VK_IMAGE_VIEW_TYPE_2D, aspect, 0, 0, 1, 0);
}

static void barrier_all(VkCommandBuffer cb) {
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
}

static int hdr_scene(void) {
    // past the tonemap the scene goes above 1 only when the game draws HDR (an HDR10 window)
    const char *e = cfg_get("MKW_TEMPORAL_HDR");
    if (e) return strcmp(e, "0") != 0;
    uint16_t peak = 0, white = 0;
    return gfx_hdr_output(&peak, &white);
}

// The upscaler in the place of the game's upscale (dispatch) or of its SMAA blend into the screen target (draw).
static int run_upscaler(VkExec *ex, const Img *color, const Img *out, int via_draw) {
    if (!T.depth.image || T.depth.w != color->w || T.depth.h != color->h) {
        static int told;
        if (told++ < 4)
            rt_log("temporal: frame %llu: no scene depth of the colour's size %ux%u (depth %ux%u): the game's own upscaling",
                   (unsigned long long)T.frame, color->w, color->h, T.depth.w, T.depth.h);
        return 0;
    }
    if (!res_init()) return 0;
    uint32_t w = color->w, h = color->h, ow = out->w, oh = out->h;
    int big = retarget_wanted(out->w, out->h);
    if (big) ow = RT.ow, oh = RT.oh;
    // the camera: this frame's and the game's own copy of the previous frame's view-projection
    double vp[16], vpp[16], ivp[16], m[16];
    rows4(T.cam, 3, vp);
    rows4(T.cam, 24, vpp);
    if (!inv4(vp, ivp)) return 0;
    if (T.last_vp_ok) {
        double d = 0;
        for (int i = 0; i < 16; i++) d += fabs(T.last_vp[i] - vpp[i]);
        if (d < 1e-3) T.prev_match++;
        else T.prev_differ++;
    }
    memcpy(T.last_vp, vp, sizeof vp);
    T.last_vp_ok = 1;
    mul4(vpp, ivp, m);
    // camera cut: the previous matrices far from this frame's
    {
        double d = 0;
        for (int i = 12; i < 16; i++) d += fabs(vp[i] - vpp[i]);
        if (d > 50.0) T.reset = 1;
    }
    float znear = T.cam[14 * 4 + 0], zfar = T.cam[14 * 4 + 1];
    float p11 = T.cam[8 * 4 + 1];   // projection [1][1]
    float fov = p11 > 0 ? 2.f * atanf(1.f / p11) : 1.f;
    VkCommandBuffer cb = ex->cb;
    vk_exec_pass_end(ex);
    vk_exec_barrier(ex);
    // NGX and FFX call Vulkan through their own function pointers, while the replay's vkCmd* wrappers enqueue packets.
    // Keep the whole temporal section direct: otherwise both threads record the same command buffer and command pool.
    int rec_saved = rec_suspend();
    tmp_fence = ex->fence, tmp_cb = cb;
    if (ex->fence > T.use_fence) T.use_fence = ex->fence;
    int slot = (int)(T.frame % FG_SLOTS);
    Own *MV = &R.mvs[slot], *Z = &R.zs[slot];
    if (!own_image_x(MV, w, h, VK_FORMAT_R16G16_SFLOAT, 1) || !own_image_x(Z, w, h, VK_FORMAT_R32_SFLOAT, 1)) {
        rec_resume(rec_saved);
        return 0;
    }
    VkImageView zview = game_view(&T.depth, VK_IMAGE_ASPECT_DEPTH_BIT), cview = game_view(color, VK_IMAGE_ASPECT_COLOR_BIT);
    // motion vectors
    {
        struct { float m[16]; float size[2], pad[2]; float kart[4]; float still[4]; float zt[4]; } pc;
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++) pc.m[c * 4 + r] = (float)m[r * 4 + c];   // column-major for GLSL
        pc.size[0] = (float)w, pc.size[1] = (float)h, pc.pad[0] = pc.pad[1] = 0;
        // the kart: a circle of 0.22 of the width around (0.5, 0.78), nearer than 0.9 of the depth range's start
        float kz = 0.f;
        if (T.kart && zfar > znear) {
            // depth of a point 12 units in front of the camera: d = (ndc + 1) / 2 with ndc = (f + n - 2fn / z) / (f - n)
            double z = 12.0, ndc = (zfar + znear - 2.0 * zfar * znear / z) / (zfar - znear);
            kz = (float)((ndc + 1.0) * 0.5);
        }
        pc.kart[0] = T.kart ? 0.22f : 0.f, pc.kart[1] = 0.5f, pc.kart[2] = 0.78f, pc.kart[3] = kz;
        // the still test against the last frame's colour: that frame's, this size, no reset between
        int pv = R.prevc.img && R.prevc.w == w && R.prevc.h == h && R.prevc.fmt == color->format &&
                 R.prevc_frame + 1 == T.frame && !T.reset && !cfg_get("MKW_TEMPORAL_KART_CIRCLE");
        pc.still[0] = pv ? 1.f : 0.f, pc.still[1] = 0.30f, pc.still[2] = 0.32f, pc.still[3] = 0.6f;
        // the last frame's depth (its R32 copy, kept for frame generation): the still test by depth (tmp_mv.comp)
        int ps = (slot + FG_SLOTS - 1) % FG_SLOTS;
        const Own *ZP = &R.zs[ps];
        int zv = ZP->img && ZP->w == w && ZP->h == h && FS[ps].frame + 1 == T.frame && !T.reset &&
                 !(cfg_get("MKW_TEMPORAL_KART_DEPTH") && !strcmp(cfg_get("MKW_TEMPORAL_KART_DEPTH"), "0"));
        pc.zt[0] = zv ? 1.f : 0.f, pc.zt[1] = znear, pc.zt[2] = zfar, pc.zt[3] = 0.f;
        VkDescriptorImageInfo di0 = {R.samp, zview, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo di1 = {VK_NULL_HANDLE, MV->view, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo di2 = {VK_NULL_HANDLE, Z->view, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo di3 = {R.samp, cview, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo di4 = {R.samp, pv ? R.prevc.view : cview, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo di5 = {R.samp, zv ? ZP->view : zview, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet wr[6] = {
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &di0},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 1, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &di1},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 2, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &di2},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 3, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &di3},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 4, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &di4},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 5, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &di5}};
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, R.mv);
        R.push(cb, VK_PIPELINE_BIND_POINT_COMPUTE, R.layout, 0, 6, wr);
        vkCmdPushConstants(cb, R.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
        vkCmdDispatch(cb, (w + 7) / 8, (h + 7) / 8, 1);
        barrier_all(cb);
    }
    // this frame's colour for the next one's still test (a copy: the game reuses its image)
    if (T.kart && (color->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) && own_image(&R.prevc, w, h, color->format)) {
        VkImageCopy ic = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0},
                          {w, h, 1}};
        vkCmdCopyImage(cb, color->image, VK_IMAGE_LAYOUT_GENERAL, R.prevc.img, VK_IMAGE_LAYOUT_GENERAL, 1, &ic);
        barrier_all(cb);
        R.prevc_frame = T.frame;
    }
    int hdr = hdr_scene();
    uint64_t now = plat_now_ns();
    float frame_ms = T.last_eval_ns && now > T.last_eval_ns ? (float)((now - T.last_eval_ns) / 1e6) : 16.7f;
    if (frame_ms > 100.f) T.reset = 1, frame_ms = 16.7f;
    T.last_eval_ns = now;
    int ok = 0;
    // the upscaler writes an image of its own, blitted into the game's target (any format, any usage)
    VkFormat of = VK_FORMAT_R16G16B16A16_SFLOAT;
    if (!own_image(&R.outimg, ow, oh, of)) {
        rec_resume(rec_saved);
        return 0;
    }
    float jx = T.debug == 3 ? 0.f : T.jx * T.jsign[0], jy = T.debug == 3 ? 0.f : T.jy * T.jsign[1];
    int reset_was = T.reset;
    if (T.debug == 1 || T.debug == 2) {
        // the motion vectors (or the depth) scaled into the output, for a look: blit of the vector image
        VkImageBlit bl = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{0, 0, 0}, {(int32_t)w, (int32_t)h, 1}},
                          {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{0, 0, 0}, {(int32_t)ow, (int32_t)oh, 1}}};
        VkClearColorValue cc = {{0.5f, 0.5f, 0.5f, 1}};
        VkImageSubresourceRange rr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cb, R.outimg.img, VK_IMAGE_LAYOUT_GENERAL, &cc, 1, &rr);
        barrier_all(cb);
        if (T.debug == 1)
            vkCmdBlitImage(cb, MV->img, VK_IMAGE_LAYOUT_GENERAL, R.outimg.img, VK_IMAGE_LAYOUT_GENERAL, 1, &bl,
                           VK_FILTER_NEAREST);
        ok = 1;
    } else if (T.engine == VK_TMP_DLSS) {
        if (dlss_feature(cb, w, h, ow, oh, hdr)) {
            NgxParam *p = NULL;
            if (NGX_OK(N.alloc_params(&p)) && p) {
                NgxResourceVK rc = res_image(cview, color->image, color->format, w, h, VK_IMAGE_ASPECT_COLOR_BIT, 0);
                NgxResourceVK ro = res_image(R.outimg.view, R.outimg.img, of, ow, oh, VK_IMAGE_ASPECT_COLOR_BIT, 1);
                NgxResourceVK rz = res_image(zview, T.depth.image, T.depth.format, w, h, VK_IMAGE_ASPECT_DEPTH_BIT, 0);
                NgxResourceVK rm = res_image(MV->view, MV->img, VK_FORMAT_R16G16_SFLOAT, w, h, VK_IMAGE_ASPECT_COLOR_BIT, 0);
                p_setp(p, "Color", &rc);
                p_setp(p, "Output", &ro);
                p_setp(p, "Depth", &rz);
                p_setp(p, "MotionVectors", &rm);
                p_setf(p, "Jitter.Offset.X", jx);
                p_setf(p, "Jitter.Offset.Y", jy);
                p_setf(p, "MV.Scale.X", 1.0f);
                p_setf(p, "MV.Scale.Y", 1.0f);
                p_seti(p, "Reset", T.reset);
                p_setu(p, "DLSS.Render.Subrect.Dimensions.Width", w);
                p_setu(p, "DLSS.Render.Subrect.Dimensions.Height", h);
                p_setf(p, "DLSS.Pre.Exposure", 1.0f);
                p_setf(p, "DLSS.Exposure.Scale", 1.0f);
                p_setf(p, "FrameTimeDeltaInMsec", frame_ms);
                NgxResult r = N.evaluate(cb, F.feat, p, NULL);
                if (N.destroy_params) N.destroy_params(p);
                ok = NGX_OK(r);
                if (!ok && T.fails++ < 4) rt_log("temporal: DLSS evaluation failed (%08x)", (unsigned)r);
            }
        }
    } else if (T.engine == VK_TMP_FSR3) {
        VkTmpRes rc = {color->image, color->format, w, h}, rz = {Z->img, VK_FORMAT_R32_SFLOAT, w, h},
                 rm = {MV->img, VK_FORMAT_R16G16_SFLOAT, w, h}, ro = {R.outimg.img, of, ow, oh};
        ok = vk_tmp_ffx_upscale(cb, &rc, &rz, &rm, &ro, jx, jy, T.reset, frame_ms, znear, zfar, fov, hdr, T.sharp);
        if (!ok && T.fails++ < 4) rt_log("temporal: FSR 3.1 upscale failed");
    }
    VkImage pic = R.outimg.img;   // what goes to the screen target: the upscaler's picture, or CAS's of it
    if (ok && T.cas >= 0.f && T.debug != 1 && cas_init() && own_image(&R.casimg, ow, oh, of) &&
        (R.casimg.usage & VK_IMAGE_USAGE_STORAGE_BIT)) {
        barrier_all(cb);
        float pc[4] = {T.cas, (float)ow, (float)oh, 0.f};
        VkDescriptorImageInfo si = {R.samp, R.outimg.view, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo so = {VK_NULL_HANDLE, R.casimg.view, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet wr[2] = {
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 3, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &si},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 1, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &so}};
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, R.cas);
        R.push(cb, VK_PIPELINE_BIND_POINT_COMPUTE, R.layout, 0, 2, wr);
        vkCmdPushConstants(cb, R.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
        vkCmdDispatch(cb, (ow + 7) / 8, (oh + 7) / 8, 1);
        pic = R.casimg.img;
    }
    if (ok) {
        barrier_all(cb);
        VkImageBlit bl = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{0, 0, 0}, {(int32_t)ow, (int32_t)oh, 1}},
                          {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{0, 0, 0}, {(int32_t)ow, (int32_t)oh, 1}}};
        VkImage dst = out->image;
        if (big && own_image(&R.big, ow, oh, out->format) && (R.big.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) {
            dst = R.big.img;   // the screen target at the output's size from here to the frame's end
            RT.on = 1, RT.src = out->image, RT.sw = out->w, RT.sh = out->h;
            RT.frames++;
        } else if (big) {
            // no big target: the upscaler's picture is scaled down into the game's
            bl.dstOffsets[1] = (VkOffset3D){(int32_t)out->w, (int32_t)out->h, 1};
        }
        vkCmdBlitImage(cb, pic, VK_IMAGE_LAYOUT_GENERAL, dst, VK_IMAGE_LAYOUT_GENERAL, 1, &bl,
                       VK_FILTER_LINEAR);
        // the same picture before the HUD for frame generation (HL, above): the same blit into an image of our own
        uint32_t sw = (uint32_t)bl.dstOffsets[1].x, sh = (uint32_t)bl.dstOffsets[1].y;
        if (hl_wanted() && own_image(&HL.snap, sw, sh, dst == R.big.img ? R.big.fmt : out->format)) {
            vkCmdBlitImage(cb, pic, VK_IMAGE_LAYOUT_GENERAL, HL.snap.img, VK_IMAGE_LAYOUT_GENERAL, 1, &bl,
                           VK_FILTER_LINEAR);
            HL.dst = dst, HL.snap_frame = T.frame;
            static int told;
            if (!told++ || (cfg_get("MKW_HL_TRACE") && told < 10 && T.frame > 1900))
                rt_log("temporal: the upscaler's picture kept before the HUD (%ux%u) f%llu dst %p", sw, sh,
                       (unsigned long long)T.frame, (void *)dst);
        }
        barrier_all(cb);
        T.reset = 0;
        T.done = 1;
        T.evals++;
        T.jw = w, T.jh = h, T.jdepth = T.depth.image;
        // the frame generation's inputs (vk_tmp_fg_inputs)
        fs_begin(&FS[slot]);
        FS[slot].mv = MV->img, FS[slot].mvv = MV->view, FS[slot].z = Z->img, FS[slot].zv = Z->view;
        FS[slot].frame = T.frame, FS[slot].w = w, FS[slot].h = h, FS[slot].jx = jx, FS[slot].jy = jy;
        FS[slot].znear = znear, FS[slot].zfar = zfar, FS[slot].fov = fov, FS[slot].frame_ms = frame_ms;
        FS[slot].aspect = T.cam[7 * 4 + 0] != 0 ? fabsf(T.cam[8 * 4 + 1] / T.cam[7 * 4 + 0]) : 1.777f;
        FS[slot].reset = reset_was;
        memcpy(FS[slot].cam, T.cam, sizeof T.cam);
        memcpy(FS[slot].m, m, sizeof m);
        fs_end(&FS[slot]);
    }
    rec_resume(rec_saved);
    vk_exec_draw_forget();
    ex->dirty = VK_EXEC_DIRTY_ALL;
    ex->recorded += 8;
    (void)via_draw;
    return ok;
}

int vk_tmp_fg_inputs(uint64_t frame, VkTmpFgInputs *out) {   // the present thread
    FgSlot f;
    if (!frame || !fs_read(&FS[frame % FG_SLOTS], &f) || f.frame != frame || !f.mv) return 0;
    out->mv = f.mv, out->mv_view = f.mvv;
    out->depth = f.z, out->depth_view = f.zv, out->depth_fmt = VK_FORMAT_R32_SFLOAT;
    out->w = f.w, out->h = f.h, out->jx = f.jx, out->jy = f.jy, out->reset = f.reset;
    memcpy(out->cam, f.cam, sizeof out->cam);
    memcpy(out->m, f.m, sizeof out->m);
    out->near_ = f.znear, out->far_ = f.zfar, out->fov_y = f.fov, out->aspect = f.aspect;
    out->frame_ms = f.frame_ms;
    out->frame = frame;
    return 1;
}

// ---------------------------------------------------------------- frame boundaries

// The upscaler turned off (live): its images (~0.4 GiB at 4K: the output, the screen target at the output's size, the
// motion vectors, depth and picture without the HUD) and DLSS features go once the last submission that used them is
// done, and those frame generation reads once it no longer can (own_retire). PR 462 review: they stayed allocated.
// FSR 3.1's contexts stay (vk_tmp_ffx.c makes them again only at another size).
static void release_images(void) {
    uint64_t f = T.use_fence;
    uint64_t bytes = 0;
    Own *own[] = {&R.outimg, &R.big, &R.prevc, &R.casimg, &HL.snap};
    for (size_t i = 0; i < sizeof own / sizeof *own; i++) bytes += 4ull * own[i]->w * own[i]->h, own_retire(own[i], f, 0);
    for (int i = 0; i < FG_SLOTS; i++) {
        fs_begin(&FS[i]);
        FS[i].frame = 0, FS[i].mv = FS[i].z = VK_NULL_HANDLE, FS[i].mvv = FS[i].zv = VK_NULL_HANDLE;
        fs_end(&FS[i]);
        bytes += 4ull * R.mvs[i].w * R.mvs[i].h + 4ull * R.zs[i].w * R.zs[i].h;
        own_retire(&R.mvs[i], f, 1), own_retire(&R.zs[i], f, 1);
    }
    hlp_publish(VK_NULL_HANDLE, VK_FORMAT_UNDEFINED, 0, 0, 0);
    if (HL.out) bytes += 4ull * HL.out_w * HL.out_h, gone_retire(HL.out, HL.out_mem, VK_NULL_HANDLE, f, 1);
    HL.out = VK_NULL_HANDLE, HL.out_frame = HL.snap_frame = 0, HL.dst = VK_NULL_HANDLE;
    for (int i = 0; i < FEATS; i++)
        if (FC[i].feat) {
            if (f) vk_completion_add(f, feat_release, FC[i].feat);
            else feat_release(FC[i].feat);
            FC[i].feat = NULL;
        }
    rt_log("temporal: the upscaler is off: its images (~%llu MiB) and features released",
           (unsigned long long)(bytes >> 20));
}

void vk_tmp_present(uint64_t frame) {
    read_cfg();
    if (T.engine != VK_TMP_OFF || T.done) {
        if (!T.done) T.reset = 1;   // a frame without the upscaler (a menu): the next one starts a new history
        next_jitter(T.jw, F.ow ? F.ow : T.jw);
    }
    if (frame >= T.log_frame + 600 && (T.evals || T.engine != VK_TMP_OFF)) {
        rt_log("temporal: %llu evaluations, jittered draws %llu, frames without a camera at the upscale %llu, the game's "
               "previous camera equal to ours %llu / differing %llu, failures %llu, plain scales %llu",
               (unsigned long long)T.evals, (unsigned long long)T.jit_draws, (unsigned long long)T.cams_miss,
               (unsigned long long)T.prev_match, (unsigned long long)T.prev_differ, (unsigned long long)T.fails,
               (unsigned long long)T.plain);
        T.log_frame = frame;
    }
    plat_mutex_lock(&tlock);
    T.cam_ok = 0, T.cam_addr = 0;
    T.depth = (Img){0};
    plat_mutex_unlock(&tlock);
    T.pre = (Img){0};
    T.smaa_out = VK_NULL_HANDLE;
    RT.on = 0;
    if (T.engine == VK_TMP_OFF && (R.outimg.img || R.big.img || R.mvs[0].img || HL.out || HL.snap.img))
        release_images();
    if (frame % 1800 == 0 && RT.frames)
        rt_log("temporal: the screen target at %ux%u in %llu frames (%llu passes moved there)", R.big.w, R.big.h,
               (unsigned long long)RT.frames, (unsigned long long)RT.swaps);
    if (!T.done) T.jdepth = VK_NULL_HANDLE;   // no upscale this frame: nothing to jitter for
    T.done = 0;
    T.frame = frame + 1;
}

void vk_tmp_device_init(void) {
    // Asset-free FSR/OFA probes can isolate their provider and device teardown from unrelated NGX initialization.
    const char *no_ngx = cfg_get("MKW_PROBE_NO_NGX");
    if (!(no_ngx && strcmp(no_ngx,"0"))) ngx_init();
    read_cfg();
}

static void dlssg_release(void);
void vk_tmp_shutdown(void) {
    const VkCtx *c = vk_ctx();
    if (!c) return;
    vkDeviceWaitIdle(c->device);
    for (int i = 0; i < FEATS; i++)
        if (FC[i].feat && N.release) N.release(FC[i].feat), FC[i].feat = NULL;
    dlssg_release();
    vk_tmp_ffx_shutdown();
    Own *o[11] = {&R.mvs[0], &R.mvs[1], &R.mvs[2], &R.outimg, &R.zs[0], &R.zs[1], &R.zs[2], &R.big, &R.prevc, &R.casimg,
                  &HL.snap};
    if (HL.out) vmaDestroyImage(c->vma, HL.out, HL.out_mem), HL.out = VK_NULL_HANDLE;
    for (int i = 0; i < 11; i++)
        if (o[i]->img) vkDestroyImageView(c->device, o[i]->view, NULL), vmaDestroyImage(c->vma, o[i]->img, o[i]->mem), *o[i] = (Own){0};
    if (R.cas) vkDestroyPipeline(c->device, R.cas, NULL), R.cas = VK_NULL_HANDLE;
    if (N.up && N.shutdown1) N.shutdown1(c->device), N.up = 0;
}

// ---------------------------------------------------------------- DLSS Frame Generation through NGX (vk_framegen.c)
// vk_framegen.c's present-thread command buffer, once the game's picture (HUD included) is copied into bb: count
// frames between the last call's picture and this one into out[] (MultiFrameIndex 1..count, in display order), from
// the depth, motion vectors, jitter and camera of the same game frame's upscale (vk_tmp_fg_inputs), and the picture
// without the HUD (DLSSG.HUDLess) when the frame has one: without it the HUD moved with the scene.
#define NGX_FEATURE_FRAME_GENERATION 11
static struct {
    NgxHandle *feat;
    NgxParam *p;
    uint32_t w, h;
    VkFormat fmt;
    uint32_t gw, gh;             // the depth and motion vectors' size it was made for
    int failed, reset;
    uint64_t last_frame, evals, misses, resets, disabled;
    // DLSSG.OutputDisableInterpolation: a byte DLSS-G sets when its made frame should not be shown, one buffer per
    // frame in flight, read FGB frames later (only counted: the frames are shown anyway)
#define FGB 4
    VkBuffer db[FGB];
    VmaAllocation dm[FGB];
    volatile uint8_t *dmap[FGB];
} G;

static void fg_store(float out[16], const double m[16]) {   // handed out transposed, for row vectors
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 4; k++) out[r * 4 + k] = (float)m[k * 4 + r];
}

int vk_tmp_dlssg(VkCommandBuffer cb, uint64_t frame, const VkTmpPic *bb, const VkTmpPic *hl, const VkTmpPic *out, int count) {
    if (!N.up || !N.fg_max || G.failed || count < 1) return 0;
    if (count > N.fg_max) count = N.fg_max;
    VkTmpFgInputs in;
    if (!vk_tmp_fg_inputs(frame, &in)) {
        G.reset = 1;   // a frame without the inputs: the next generation has no valid previous frame
        if (G.misses++ < 4)
            rt_log("temporal: DLSS Frame Generation: no depth or motion vectors for frame %llu (is MKW_TEMPORAL on?)",
                   (unsigned long long)frame);
        return 0;
    }
    if (!G.p && !NGX_OK(N.alloc_params(&G.p))) {
        G.failed = 1;
        return 0;
    }
    NgxParam *p = G.p;
    if (G.feat && (G.w != bb->w || G.h != bb->h || G.fmt != bb->fmt || G.gw != in.w || G.gh != in.h)) {
        vk_queue_lock();   // the present thread, as vk_framegen.c ensure_images
        vkDeviceWaitIdle(vk_ctx()->device);
        vk_queue_unlock();
        N.release(G.feat);
        G.feat = NULL;
    }
    if (!G.feat) {
        p_setu(p, "CreationNodeMask", 1);
        p_setu(p, "VisibilityNodeMask", 1);
        p_setu(p, "Width", bb->w);
        p_setu(p, "Height", bb->h);
        p_setu(p, "DLSSG.BackbufferFormat", (unsigned)bb->fmt);
        p_setu(p, "DLSSG.Width", bb->w);
        p_setu(p, "DLSSG.Height", bb->h);
        p_setu(p, "DLSSG.InternalWidth", in.w);   // the depth and motion vectors are the render size, not the picture's
        p_setu(p, "DLSSG.InternalHeight", in.h);
        p_setu(p, "DLSSG.DynamicResolution", 0);
        {   // MKW_DLSSG_EXP 512: LiteCandidateBlendingEnabled 0, 1024: 1 (checking: unset by default)
            const char *xe0 = cfg_get("MKW_DLSSG_EXP");
            unsigned xp0 = xe0 ? (unsigned)strtoul(xe0, NULL, 0) : 0;
            if (xp0 & 512) p_setu(p, "DLSSG.LiteCandidateBlendingEnabled", 0);
            if (xp0 & 1024) p_setu(p, "DLSSG.LiteCandidateBlendingEnabled", 1);
        }
        NgxResult r = N.create(cb, NGX_FEATURE_FRAME_GENERATION, p, &G.feat);
        if (!NGX_OK(r) || !G.feat) {
            G.failed = 1, G.feat = NULL;
            rt_log("temporal: DLSS Frame Generation could not be created (%08x): the port's own frame generation",
                   (unsigned)r);
            return 0;
        }
        G.w = bb->w, G.h = bb->h, G.fmt = bb->fmt, G.gw = in.w, G.gh = in.h, G.reset = 1;
        const VkCtx *c = vk_ctx();
        for (int i = 0; i < FGB; i++) {
            if (G.db[i]) continue;
            VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 256,
                                     .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
            VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                          .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
            VmaAllocationInfo info;
            if (vmaCreateBuffer(c->vma, &bi, &ai, &G.db[i], &G.dm[i], &info) == VK_SUCCESS) G.dmap[i] = info.pMappedData;
            else G.db[i] = VK_NULL_HANDLE;
        }
        if (ngx_fg_override)
            rt_log("temporal: DLSS Frame Generation: the NVIDIA App's DLSS Frame Generation override is on; its model "
                   "only blends the two frames here: turn the override off in the NVIDIA App (Graphics, DLSS Override)");
        rt_log("temporal: DLSS Frame Generation at %ux%u (format %d), depth and motion %ux%u, up to %d generated",
               bb->w, bb->h, (int)bb->fmt, in.w, in.h, N.fg_max);
    }
    // the camera: projection rows 7-10, view rows 0-2 (right, up, back; it looks down -z)
    // NVN's clip space has z / w in -1..1 (minus-w mode); the depth image DLSS-G reads is 0..1 (depth = (z / w + 1) / 2).
    // Its matrices go to that 0..1 clip space (S), so the camera motion it works out from them and the depth matches
    // the motion vectors: in -1..1 they disagreed, and DLSS-G blended the two frames instead of moving them.
    static const double S[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0.5, 0.5, 0, 0, 0, 1};
    static const double Si[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 2, -1, 0, 0, 0, 1};
    double P0[16], P[16], Pi[16], m[16], mt[16], mi[16];
    for (int i = 0; i < 16; i++) P0[i] = in.cam[28 + i];
    // MKW_DLSSG_EXP (checking DLSS-G, a bit mask): 1 vectors x8, 16 vectors negated, 4 no camera motion in them, 8 depth
    // inverted, 32 NVN's -1..1 clip space, 64 a reset every frame, 128 no invalid vector value, 512/1024 LiteCandidateBlendingEnabled 0/1
    const char *xe = cfg_get("MKW_DLSSG_EXP");
    unsigned xp = xe ? (unsigned)strtoul(xe, NULL, 0) : 0;
    if (xp & 32) {   // (checking: NVN's clip space as it is)
        memcpy(P, P0, sizeof P), memcpy(m, in.m, sizeof m);
    } else {
        mul4(S, P0, P);
        mul4(S, in.m, mt), mul4(mt, Si, m);
    }
    if (!inv4(P, Pi) || !inv4(m, mi)) return 0;
    static float v2c[16], c2v[16], c2p[16], p2c[16];
    fg_store(v2c, P), fg_store(c2v, Pi), fg_store(c2p, m), fg_store(p2c, mi);
    float pos[3], up[3], right[3], fwd[3];
    const float *v = in.cam;
    for (int i = 0; i < 3; i++) {
        right[i] = v[i], up[i] = v[4 + i], fwd[i] = -v[8 + i];
        pos[i] = -(v[i] * v[3] + v[4 + i] * v[7] + v[8 + i] * v[11]);
    }
    NgxResourceVK rb = res_image(bb->view, bb->img, bb->fmt, bb->w, bb->h, VK_IMAGE_ASPECT_COLOR_BIT, 0);
    NgxResourceVK rz = res_image(in.depth_view, in.depth, VK_FORMAT_R32_SFLOAT, in.w, in.h, VK_IMAGE_ASPECT_COLOR_BIT, 0);
    NgxResourceVK rm = res_image(in.mv_view, in.mv, VK_FORMAT_R16G16_SFLOAT, in.w, in.h, VK_IMAGE_ASPECT_COLOR_BIT, 0);
    p_setp(p, "DLSSG.Backbuffer", &rb);
    p_setp(p, "DLSSG.Depth", &rz);
    p_setp(p, "DLSSG.MVecs", &rm);
    // the picture without the HUD (vk_tmp_fg_hudless), so DLSS-G keeps the HUD out of its motion
    NgxResourceVK rh;
    if (hl) rh = res_image(hl->view, hl->img, hl->fmt, hl->w, hl->h, VK_IMAGE_ASPECT_COLOR_BIT, 0);
    p_setp(p, "DLSSG.HUDLess", hl ? &rh : NULL);
    p_setp(p, "DLSSG.UI", NULL);
    p_setp(p, "DLSSG.UIAlpha", NULL);
    p_setp(p, "DLSSG.BidirectionalDistortionField", NULL);
    p_setp(p, "DLSSG.OutputReal", NULL);
    int bs = (int)(G.evals % FGB);
    NgxResourceVK rd;
    memset(&rd, 0, sizeof rd);
    if (G.db[bs]) {
        if (G.evals >= FGB && G.dmap[bs][0]) G.disabled++;   // what the evaluation FGB frames ago said
        G.dmap[bs][0] = 0;
        rd.r.buf.buffer = G.db[bs], rd.r.buf.bytes = 256, rd.type = 1, rd.rw = 1;
    }
    // the buffer goes only with MKW_DLSSG_DISABLE_BUF=1 (checking): DLSS-G writes it by its device address, which a buffer
    // without SHADER_DEVICE_ADDRESS usage does not have (suspected of "device fault: write invalid at 0")
    p_setp(p, "DLSSG.OutputDisableInterpolation", G.db[bs] && cfg_get("MKW_DLSSG_DISABLE_BUF") ? &rd : NULL);
    static const float ident[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    p_setp(p, "DLSSG.CameraViewToClip", v2c);
    p_setp(p, "DLSSG.ClipToCameraView", c2v);
    p_setp(p, "DLSSG.ClipToLensClip", (void *)ident);
    p_setp(p, "DLSSG.ClipToPrevClip", c2p);
    p_setp(p, "DLSSG.PrevClipToClip", p2c);
    p_setf(p, "DLSSG.JitterOffsetX", in.jx);
    p_setf(p, "DLSSG.JitterOffsetY", in.jy);
    float xs = (xp & 1) ? 8.0f : (xp & 16) ? -1.0f : 1.0f;
    p_setf(p, "DLSSG.MvecScaleX", xs / (float)in.w);   // the vectors are in render pixels
    p_setf(p, "DLSSG.MvecScaleY", xs / (float)in.h);
    p_setf(p, "DLSSG.CameraPinholeOffsetX", 0.0f);
    p_setf(p, "DLSSG.CameraPinholeOffsetY", 0.0f);
    static const char *const kp[] = {"DLSSG.CameraPosX", "DLSSG.CameraPosY", "DLSSG.CameraPosZ"};
    static const char *const ku[] = {"DLSSG.CameraUpX", "DLSSG.CameraUpY", "DLSSG.CameraUpZ"};
    static const char *const kr[] = {"DLSSG.CameraRightX", "DLSSG.CameraRightY", "DLSSG.CameraRightZ"};
    static const char *const kf[] = {"DLSSG.CameraFwdX", "DLSSG.CameraFwdY", "DLSSG.CameraFwdZ"};
    for (int i = 0; i < 3; i++)
        p_setf(p, kp[i], pos[i]), p_setf(p, ku[i], up[i]), p_setf(p, kr[i], right[i]), p_setf(p, kf[i], fwd[i]);
    p_setf(p, "DLSSG.CameraNear", in.near_);
    p_setf(p, "DLSSG.CameraFar", in.far_);
    p_setf(p, "DLSSG.CameraFOV", in.fov_y);
    p_setf(p, "DLSSG.CameraAspectRatio", in.aspect);
    p_setu(p, "DLSSG.ColorBuffersHDR", bb->fmt != VK_FORMAT_R8G8B8A8_UNORM && bb->fmt != VK_FORMAT_R8G8B8A8_SRGB);
    p_setu(p, "DLSSG.DepthInverted", (xp & 8) != 0);
    p_setu(p, "DLSSG.CameraMotionIncluded", !(xp & 4));
    int reset = G.reset || in.reset || (G.last_frame && frame != G.last_frame + 1);
    if (xp & 64) reset = 1;
    p_setu(p, "DLSSG.Reset", (unsigned)reset);
    G.resets += reset != 0;
    p_setu64(p, "DLSSG.BackbufferFrameID", frame);
    p_setu(p, "DLSSG.MvecJittered", 0);
    p_setu(p, "DLSSG.InternalWidth", in.w);
    p_setu(p, "DLSSG.InternalHeight", in.h);
    p_setu(p, "DLSSG.AutomodeOverrideReset", 0);
    p_setu(p, "DLSSG.NotRenderingGameFrames", 0);
    p_setu(p, "DLSSG.OrthoProjection", 0);
    p_setu(p, "DLSSG.MvecDilated", 0);
    // a vector component equal to this marks a pixel without motion; never one of ours (FP16's largest)
    p_setf(p, "DLSSG.MvecInvalidValue", (xp & 128) ? 0.0f : 65504.0f);
    p_setu(p, "DLSSG.MenuDetectionEnabled", 0);
    struct { const char *base; uint32_t w, h; } rects[] = {
        {"DLSSG.MVecsSubrect", in.w, in.h}, {"DLSSG.DepthSubrect", in.w, in.h},
        {"DLSSG.HUDLessSubrect", bb->w, bb->h}, {"DLSSG.InputBackbufferSubrect", bb->w, bb->h},
        {"DLSSG.OutputInterpolatedSubrect", bb->w, bb->h}, {"DLSSG.OutputRealSubrect", bb->w, bb->h}};
    for (size_t i = 0; i < sizeof rects / sizeof *rects; i++) {
        char k[64];
        snprintf(k, sizeof k, "%sBaseX", rects[i].base), p_setu(p, k, 0);
        snprintf(k, sizeof k, "%sBaseY", rects[i].base), p_setu(p, k, 0);
        snprintf(k, sizeof k, "%sWidth", rects[i].base), p_setu(p, k, rects[i].w);
        snprintf(k, sizeof k, "%sHeight", rects[i].base), p_setu(p, k, rects[i].h);
    }
    p_setu(p, "DLSSG.MultiFrameCount", (unsigned)count);
    int made = 0;
    for (int i = 0; i < count; i++) {
        NgxResourceVK ro = res_image(out[i].view, out[i].img, out[i].fmt, out[i].w, out[i].h, VK_IMAGE_ASPECT_COLOR_BIT, 1);
        p_setp(p, "DLSSG.OutputInterpolated", &ro);
        p_setu(p, "DLSSG.MultiFrameIndex", (unsigned)(i + 1));
        NgxResult r = N.evaluate(cb, G.feat, p, NULL);
        if (!NGX_OK(r)) {
            static int told;
            if (told++ < 5) rt_log("temporal: DLSS Frame Generation evaluation %d of %d failed (%08x)", i + 1, count, (unsigned)r);
            break;
        }
        made++;
    }
    barrier_all(cb);
    G.reset = 0;
    G.last_frame = frame;
    if (made && (++G.evals == 1 || G.evals % (cfg_get("MKW_TEMPORAL_TRACE") ? 300 : 3600) == 0))
        rt_log("temporal: DLSS Frame Generation: %llu game frames, %d generated per game frame, %llu reset, %llu it "
               "asked not to show", (unsigned long long)G.evals, made, (unsigned long long)G.resets,
               (unsigned long long)G.disabled);
    return made;
}

static void dlssg_release(void) {
    if (G.feat && N.release) N.release(G.feat), G.feat = NULL;
    for (int i = 0; i < FGB; i++)
        if (G.db[i]) vmaDestroyBuffer(vk_ctx()->vma, G.db[i], G.dm[i]), G.db[i] = VK_NULL_HANDLE, G.dmap[i] = NULL;
}
