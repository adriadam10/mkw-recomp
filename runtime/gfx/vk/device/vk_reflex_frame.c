// NVIDIA Reflex for MKW (MKW_REFLEX=off|on|boost, live): the glue between the runtime and vk_reflex.c (the
// VK_NV_low_latency2 calls). One application frame is one game present: its id is the number the back end gives that
// present (vk_backend.c game_frames), the same on the game thread, the replay and the present thread.
//
//   game thread, before the game's frame update (game.c host_frame_update): the Reflex sleep, then SIMULATION_START;
//     after it: SIMULATION_END and RENDERSUBMIT_START (the game records and submits inside its frame update; the
//     replay turns that into Vulkan work as it comes);
//   replay thread, once the frame's last submission is recorded (vk_backend.c present_image): RENDERSUBMIT_END;
//   present thread, around the vkQueuePresentKHR of the game's frame: PRESENT_START and PRESENT_END, with the frame's
//     id as its VkPresentIdKHR. Frames made by frame generation have no own VkPresentIdKHR and use out-of-band present
//     markers with the game frame's id, so they belong to that application frame.
// The swapchain is made with low-latency mode allowed whenever the device has the extension, so the setting changes
// without a new swapchain.
#include <string.h>

#include "cfg.h"
#include "gfx.h"
#include "platform/platform.h"
#include "host.h"
#include "vk.h"
#include "vk_reflex_frame.h"
#include "vk_reflex.h"

static struct {
    VkReflex *r;
    VkReflexSwapchain *sc;
    VkSwapchainKHR sc_handle;
    VkReflexMode mode;         // as last set on the swapchain
    unsigned cfg_seen;
    int want;                  // MKW_REFLEX now
    uint64_t sim_id;           // the frame the game thread is simulating
    uint64_t sleeps, timeouts;
} L = {.cfg_seen = ~0u};

static PlatMutex lk = PLAT_MUTEX_INIT;

// The calls below come from the game thread (the sleep, which waits up to a frame), the replay and the present and flip
// threads, without lk (a lock around the sleep would hold every present behind it). vk_rfx_shutdown frees L.r: it first
// turns new calls away (closing) and waits for every admitted call to leave. It must return before vk_device.c destroys
// the device, because a driver call can otherwise still be inside the driver while Vulkan tears its objects down.
static int users, closing;
static int enter(void) {
    __atomic_add_fetch(&users, 1, __ATOMIC_SEQ_CST);
    if (!__atomic_load_n(&closing, __ATOMIC_SEQ_CST) && __atomic_load_n(&L.r, __ATOMIC_ACQUIRE)) return 1;
    __atomic_sub_fetch(&users, 1, __ATOMIC_RELEASE);
    return 0;
}
static void leave(void) { __atomic_sub_fetch(&users, 1, __ATOMIC_RELEASE); }

void vk_rfx_init(uint32_t spec_version) {
    // Vulkan can be initialized again after vk_shutdown. No calls can be admitted while this sets up a new L.r.
    __atomic_store_n(&closing, 1, __ATOMIC_SEQ_CST);
    const VkCtx *c = vk_ctx();
    if (!c || !spec_version) {
        if (c && c->low_latency2_have)   // the device has it, left out with Reflex off at start (vk_device.c)
            rt_log("reflex: off at start: VK_NV_low_latency2 not enabled (Reflex on in the menu applies after a restart)");
        else
            rt_log("reflex: the driver has no VK_NV_low_latency2: NVIDIA Reflex unavailable");
        __atomic_store_n(&closing, 0, __ATOMIC_SEQ_CST);
        return;
    }
    __atomic_store_n(&L.r, vk_reflex_create(c->device, spec_version), __ATOMIC_RELEASE);
    vk_reflex_set_external_lock(gfx_swapchain_lock, gfx_swapchain_unlock);
    __atomic_store_n(&closing, 0, __ATOMIC_SEQ_CST);
    rt_log("reflex: VK_NV_low_latency2 revision %u %s", spec_version, L.r ? "ready" : "could not be set up");
}

int vk_rfx_available(void) {
    if (!enter()) return 0;
    int a = vk_reflex_available(L.r);
    leave();
    return a;
}

// The menu's note under MKW_REFLEX: 0 none, 1 after a restart (VK_NV_low_latency2 is enabled only with Reflex asked
// for at start, vk_device.c), 2 not available on this PC (no VK_NV_low_latency2: not an NVIDIA driver).
int vk_rfx_menu_state(void) {
    const VkCtx *c = vk_ctx();
    if (!strcmp(cfg_value("MKW_REFLEX"), "off") || !c) return 0;
    if (!c->low_latency2_have) return 2;
    return __atomic_load_n(&L.r, __ATOMIC_ACQUIRE) ? 0 : 1;
}

const void *vk_rfx_swapchain_pnext(VkSwapchainLatencyCreateInfoNV *storage, const void *next) {
    if (!enter()) return next;
    const void *p = vk_reflex_swapchain_pnext(L.r, storage, next, 1);
    leave();
    return p;
}

static VkReflexMode cfg_mode(void) {
    const char *v = cfg_value("MKW_REFLEX");
    return !strcmp(v, "boost") ? VK_REFLEX_BOOST : !strcmp(v, "on") ? VK_REFLEX_ON : VK_REFLEX_OFF;
}

static void apply_mode_locked(void) {
    if (!L.r || !L.sc) return;
    VkReflexMode m = (VkReflexMode)L.want;
    if (m == L.mode) return;
    VkResult r = vk_reflex_set_mode(L.r, L.sc, m, 0);
    if (r == VK_SUCCESS) L.mode = m;
    rt_log("reflex: low-latency mode %s (%d)", m == VK_REFLEX_BOOST ? "on + boost" : m == VK_REFLEX_ON ? "on" : "off",
           (int)r);
}

void vk_rfx_swapchain_created(VkSwapchainKHR sc) {
    if (!enter()) return;
    plat_mutex_lock(&lk);
    L.sc = vk_reflex_swapchain_attach(L.r, sc);
    L.sc_handle = sc;
    L.mode = VK_REFLEX_OFF;
    L.want = cfg_mode();
    apply_mode_locked();
    plat_mutex_unlock(&lk);
    leave();
}

void vk_rfx_swapchain_destroying(VkSwapchainKHR sc) {
    if (!enter()) return;
    if (!L.sc || sc != L.sc_handle) {
        leave();
        return;
    }
    plat_mutex_lock(&lk);
    VkReflexSwapchain *s = L.sc;
    L.sc = NULL, L.sc_handle = VK_NULL_HANDLE;
    plat_mutex_unlock(&lk);
    vk_reflex_swapchain_detach(L.r, s);
    leave();
}

static int on(void) {
    return __atomic_load_n(&L.r, __ATOMIC_ACQUIRE) && L.sc && L.mode != VK_REFLEX_OFF;
}

void vk_rfx_frame_start(uint64_t id) {
    if (!enter()) return;
    unsigned v = cfg_version();
    if (v != L.cfg_seen) {
        L.cfg_seen = v;
        gfx_swapchain_lock();   // before lk, as create_swapchain (gfx_present.c) takes them
        plat_mutex_lock(&lk);
        L.want = cfg_mode();
        apply_mode_locked();
        plat_mutex_unlock(&lk);
        gfx_swapchain_unlock();
    }
    L.sim_id = id;
    if (!on()) {
        leave();
        return;
    }
    VkReflexSleepResult s = vk_reflex_sleep(L.r, L.sc);
    L.sleeps++;
    if (s == VK_REFLEX_SLEEP_TIMEOUT) L.timeouts++;
    vk_reflex_marker(L.r, L.sc, VK_LATENCY_MARKER_SIMULATION_START_NV, id);
    vk_reflex_marker(L.r, L.sc, VK_LATENCY_MARKER_INPUT_SAMPLE_NV, id);
    if (L.sleeps % 3600 == 0) rt_log("reflex: %llu sleeps, %llu timed out", (unsigned long long)L.sleeps,
                                     (unsigned long long)L.timeouts);
    leave();
}

static void mark2(VkLatencyMarkerNV a, VkLatencyMarkerNV b, uint64_t id) {   // one or two markers, if on
    if (!enter()) return;
    if (on()) {
        vk_reflex_marker(L.r, L.sc, a, id);
        if (b != a) vk_reflex_marker(L.r, L.sc, b, id);
    }
    leave();
}

void vk_rfx_frame_end(void) {
    mark2(VK_LATENCY_MARKER_SIMULATION_END_NV, VK_LATENCY_MARKER_RENDERSUBMIT_START_NV, L.sim_id);
}

void vk_rfx_submitted(uint64_t id) {
    mark2(VK_LATENCY_MARKER_RENDERSUBMIT_END_NV, VK_LATENCY_MARKER_RENDERSUBMIT_END_NV, id);
}

static uint64_t mark_if_on(VkLatencyMarkerNV m, uint64_t id) {   // id when marked, else 0
    if (!id || !enter()) return 0;
    uint64_t r = 0;
    if (on()) vk_reflex_marker(L.r, L.sc, m, id), r = id;
    leave();
    return r;
}

uint64_t vk_rfx_present_start(uint64_t id) { return mark_if_on(VK_LATENCY_MARKER_PRESENT_START_NV, id); }
void vk_rfx_present_end(uint64_t id) { (void)mark_if_on(VK_LATENCY_MARKER_PRESENT_END_NV, id); }
uint64_t vk_rfx_oob_present_start(uint64_t id) { return mark_if_on(VK_LATENCY_MARKER_OUT_OF_BAND_PRESENT_START_NV, id); }
void vk_rfx_oob_present_end(uint64_t id) { (void)mark_if_on(VK_LATENCY_MARKER_OUT_OF_BAND_PRESENT_END_NV, id); }

void vk_rfx_shutdown(void) {
    VkReflex *reflex = __atomic_load_n(&L.r, __ATOMIC_ACQUIRE);
    if (!reflex) return;
    __atomic_store_n(&closing, 1, __ATOMIC_SEQ_CST);

    // Destroying the device while a driver entry point is still running is invalid even when that call is stuck. Wait
    // without a deadline instead. The regular sleep gives up its virtual core and times out; a device-lost call also
    // returns, so this blocks only on the pathological driver failure where continuing to teardown would be unsafe.
    uint64_t next_report = plat_now_ns() + 500000000ull;
    while (__atomic_load_n(&users, __ATOMIC_ACQUIRE)) {
        uint64_t now = plat_now_ns();
        if (now >= next_report) {
            rt_log("reflex: waiting for %d admitted call(s) before device destruction",
                   __atomic_load_n(&users, __ATOMIC_ACQUIRE));
            next_report = now + 1000000000ull;
        }
        plat_sleep_ms(1);
    }
    if (L.sc) {
        vk_reflex_swapchain_detach(reflex, L.sc);
        L.sc = NULL;
        L.sc_handle = VK_NULL_HANDLE;
    }
    vk_reflex_destroy(reflex);
    __atomic_store_n(&L.r, NULL, __ATOMIC_RELEASE);
}
