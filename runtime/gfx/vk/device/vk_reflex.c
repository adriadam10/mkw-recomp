// VK_NV_low_latency2 backend. The extension contract follows the Khronos refpages:
// https://docs.vulkan.org/refpages/latest/refpages/source/VK_NV_low_latency2.html and its linked command pages.
//
// A swapchain keeps one timeline semaphore for Reflex sleeps. Only one sleep may be in flight on that swapchain; a
// second caller gets VK_REFLEX_SLEEP_BUSY immediately. The sleep caller gives up its virtual core only around the
// bounded vkWaitSemaphores, as every other guest host wait does. The module state lock is never held across a host
// wait, so another virtual-core worker cannot block behind a sleeping thread while holding the core.
//
// Swapchain calls take a lifecycle reference. Detach stops new calls, waits for those references, and only then
// touches the native swapchain. Retired entries remain allocated until context shutdown, making stale handles safe to
// retry. If a sleep timed out, the driver may still have a pending timeline signal, so detach first waits for device
// idle before destroying that semaphore. If the device wait fails, the semaphore is retained for context shutdown.
#include "vk_reflex.h"

#include <stdlib.h>

#include "host.h"
#include "platform/platform.h"

enum {
    VK_REFLEX_ACQUIRED,
    VK_REFLEX_INACTIVE,
    VK_REFLEX_BUSY,
};

struct VkReflexSwapchain {
    struct VkReflexSwapchain *next;
    VkReflex *owner;
    VkSwapchainKHR swapchain;
    VkSemaphore sleep_semaphore;
    uint64_t sleep_value;
    VkReflexMode mode;
    uint32_t active_calls;
    int sleep_busy;
    int closing;
    int sleep_signal_pending;
    PlatEvent idle;
};

struct VkReflex {
    VkDevice device;
    uint32_t spec_version;
    PFN_vkSetLatencySleepModeNV set_mode;
    PFN_vkLatencySleepNV latency_sleep;
    PFN_vkSetLatencyMarkerNV marker;
    PlatMutex lock;
    VkReflexSwapchain *attached;
    VkReflexSwapchain *retired;
    volatile int32_t timeout_logged;
    volatile int32_t sleep_error_logged;
    volatile int32_t mode_error_logged;
    volatile int32_t detach_error_logged;
};

// The swapchain's owner's lock (gfx_present.c: acquire and present hold it): the driver calls that name the swapchain
// take it first, before entry_acquire, so a detach (made under it) never waits for a call that waits for it
static void (*ext_lock)(void), (*ext_unlock)(void);
void vk_reflex_set_external_lock(void (*lock)(void), void (*unlock)(void)) { ext_lock = lock, ext_unlock = unlock; }
static void xlock(void) { if (ext_lock) ext_lock(); }
static void xunlock(void) { if (ext_unlock) ext_unlock(); }

static int entry_acquire(VkReflex *reflex, VkReflexSwapchain *swapchain, int sleep, VkReflexMode *mode) {
    if (!reflex || !swapchain) return VK_REFLEX_INACTIVE;
    plat_mutex_lock(&reflex->lock);
    if (swapchain->owner != reflex || swapchain->closing) {
        plat_mutex_unlock(&reflex->lock);
        return VK_REFLEX_INACTIVE;
    }
    if (sleep && swapchain->sleep_busy) {
        plat_mutex_unlock(&reflex->lock);
        return VK_REFLEX_BUSY;
    }
    if (sleep) swapchain->sleep_busy = 1;
    swapchain->active_calls++;
    if (mode) *mode = swapchain->mode;
    plat_mutex_unlock(&reflex->lock);
    return VK_REFLEX_ACQUIRED;
}

static void entry_release(VkReflex *reflex, VkReflexSwapchain *swapchain, int sleep) {
    int wake;
    plat_mutex_lock(&reflex->lock);
    if (sleep) swapchain->sleep_busy = 0;
    if (swapchain->active_calls) swapchain->active_calls--;
    wake = swapchain->closing && !swapchain->active_calls;
    plat_mutex_unlock(&reflex->lock);
    if (wake) plat_event_set(&swapchain->idle);
}

static void unlink_entry(VkReflexSwapchain **list, VkReflexSwapchain *swapchain) {
    while (*list && *list != swapchain) list = &(*list)->next;
    if (*list) *list = swapchain->next;
}

static VkResult wait_device_idle(VkReflex *reflex) {
    int away = kern_core_away();
    VkResult r = vkDeviceWaitIdle(reflex->device);
    kern_core_back(&away);
    if (r != VK_SUCCESS && plat_xchg32(&reflex->detach_error_logged, 1) == 0) {
        rt_log("vk: Reflex could not wait for device idle during teardown (%d); a pending sleep semaphore is retained",
               (int)r);
    }
    return r;
}

static int settle_sleep_signal(VkReflex *reflex, VkReflexSwapchain *swapchain) {
    if (!swapchain->sleep_signal_pending) return 1;
    VkSemaphoreWaitInfo wait = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
                                .semaphoreCount = 1,
                                .pSemaphores = &swapchain->sleep_semaphore,
                                .pValues = &swapchain->sleep_value};
    if (vkWaitSemaphores(reflex->device, &wait, 0) != VK_SUCCESS) return 0;
    swapchain->sleep_signal_pending = 0;
    return 1;
}

VkReflex *vk_reflex_create(VkDevice device, uint32_t enabled_spec_version) {
    if (!device || !enabled_spec_version) {
        rt_log("vk: Reflex unavailable: VK_NV_low_latency2 was not enabled");
        return NULL;
    }

    VkReflex *reflex = calloc(1, sizeof *reflex);
    if (!reflex) {
        rt_log("vk: Reflex unavailable: cannot allocate its state");
        return NULL;
    }
    reflex->device = device;
    reflex->spec_version = enabled_spec_version;
    reflex->set_mode = (PFN_vkSetLatencySleepModeNV)vkGetDeviceProcAddr(device, "vkSetLatencySleepModeNV");
    reflex->latency_sleep = (PFN_vkLatencySleepNV)vkGetDeviceProcAddr(device, "vkLatencySleepNV");
    reflex->marker = (PFN_vkSetLatencyMarkerNV)vkGetDeviceProcAddr(device, "vkSetLatencyMarkerNV");
    if (!reflex->set_mode || !reflex->latency_sleep || !reflex->marker) {
        rt_log("vk: Reflex unavailable: VK_NV_low_latency2 entry points are missing (mode %d sleep %d marker %d)",
               reflex->set_mode != NULL, reflex->latency_sleep != NULL, reflex->marker != NULL);
        free(reflex);
        return NULL;
    }
    rt_log("vk: Reflex enabled through VK_NV_low_latency2 revision %u; explicit submission attribution %s",
           enabled_spec_version, enabled_spec_version >= 3 ? "available" : "unavailable (revision below 3)");
    return reflex;
}

int vk_reflex_available(const VkReflex *reflex) {
    return reflex && reflex->device && reflex->set_mode && reflex->latency_sleep && reflex->marker;
}

const void *vk_reflex_swapchain_pnext(VkReflex *reflex, VkSwapchainLatencyCreateInfoNV *storage, const void *next,
                                      int allow_low_latency) {
    if (!vk_reflex_available(reflex) || !storage || !allow_low_latency) return next;
    storage->sType = VK_STRUCTURE_TYPE_SWAPCHAIN_LATENCY_CREATE_INFO_NV;
    storage->pNext = next;
    storage->latencyModeEnable = VK_TRUE;
    return storage;
}

VkReflexSwapchain *vk_reflex_swapchain_attach(VkReflex *reflex, VkSwapchainKHR swapchain) {
    if (!vk_reflex_available(reflex) || swapchain == VK_NULL_HANDLE) return NULL;
    VkReflexSwapchain *entry = calloc(1, sizeof *entry);
    if (!entry) {
        rt_log("vk: Reflex swapchain unavailable: cannot allocate its state");
        return NULL;
    }
    VkSemaphoreTypeCreateInfo type = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                      .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
                                      .initialValue = 0};
    VkSemaphoreCreateInfo info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &type};
    VkResult r = vkCreateSemaphore(reflex->device, &info, NULL, &entry->sleep_semaphore);
    if (r != VK_SUCCESS) {
        rt_log("vk: Reflex swapchain unavailable: timeline semaphore creation failed (%d)", (int)r);
        free(entry);
        return NULL;
    }
    entry->owner = reflex;
    entry->swapchain = swapchain;
    entry->mode = VK_REFLEX_OFF;
    plat_event_init(&entry->idle, 1, 0);
    plat_mutex_lock(&reflex->lock);
    entry->next = reflex->attached;
    reflex->attached = entry;
    plat_mutex_unlock(&reflex->lock);
    return entry;
}

void vk_reflex_swapchain_detach(VkReflex *reflex, VkReflexSwapchain *swapchain) {
    if (!reflex || !swapchain) return;
    int wait;
    plat_mutex_lock(&reflex->lock);
    if (swapchain->owner != reflex || swapchain->closing) {
        plat_mutex_unlock(&reflex->lock);
        return;
    }
    swapchain->closing = 1;
    unlink_entry(&reflex->attached, swapchain);
    swapchain->next = reflex->retired;
    reflex->retired = swapchain;
    wait = swapchain->active_calls != 0;
    plat_mutex_unlock(&reflex->lock);

    if (wait) {
        int away = kern_core_away();
        plat_event_wait(&swapchain->idle);
        kern_core_back(&away);
    }

    if (swapchain->sleep_signal_pending && wait_device_idle(reflex) == VK_SUCCESS)
        settle_sleep_signal(reflex, swapchain);
    VkLatencySleepModeInfoNV off = {.sType = VK_STRUCTURE_TYPE_LATENCY_SLEEP_MODE_INFO_NV};   // not NULL: see set_mode
    VkResult rc = reflex->set_mode(reflex->device, swapchain->swapchain, &off);
    if (rc != VK_SUCCESS && plat_xchg32(&reflex->mode_error_logged, 1) == 0) {
        rt_log("vk: Reflex could not disable low latency for a retiring swapchain (%d)", (int)rc);
    }
    if (!swapchain->sleep_signal_pending && swapchain->sleep_semaphore != VK_NULL_HANDLE) {
        vkDestroySemaphore(reflex->device, swapchain->sleep_semaphore, NULL);
        swapchain->sleep_semaphore = VK_NULL_HANDLE;
    }
}

VkResult vk_reflex_set_mode(VkReflex *reflex, VkReflexSwapchain *swapchain, VkReflexMode mode,
                            uint32_t minimum_interval_us) {
    if (!vk_reflex_available(reflex)) return VK_ERROR_FEATURE_NOT_PRESENT;
    if (mode != VK_REFLEX_OFF && mode != VK_REFLEX_ON && mode != VK_REFLEX_BOOST)
        return VK_ERROR_INITIALIZATION_FAILED;
    xlock();
    if (entry_acquire(reflex, swapchain, 0, NULL) != VK_REFLEX_ACQUIRED) return xunlock(), VK_ERROR_FEATURE_NOT_PRESENT;

    VkLatencySleepModeInfoNV info = {
        .sType = VK_STRUCTURE_TYPE_LATENCY_SLEEP_MODE_INFO_NV,
        .lowLatencyMode = mode != VK_REFLEX_OFF,
        .lowLatencyBoost = mode == VK_REFLEX_BOOST,
        .minimumIntervalUs = mode == VK_REFLEX_OFF ? 0 : minimum_interval_us,
    };
    // Off as a mode of its own rather than NULL, which the spec allows too: NVIDIA's driver 621.01 (RTX 5080) reads the
    // three fields through the pointer it was given and NULL crashed it (nvoglv64.dll, a read at offset 0x18) when
    // Reflex was switched off in the F1 menu.
    VkResult r = reflex->set_mode(reflex->device, swapchain->swapchain, &info);
    if (r == VK_SUCCESS) {
        plat_mutex_lock(&reflex->lock);
        swapchain->mode = mode;
        plat_mutex_unlock(&reflex->lock);
    } else if (plat_xchg32(&reflex->mode_error_logged, 1) == 0) {
        rt_log("vk: Reflex could not set mode %d (%d)", (int)mode, (int)r);
    }
    entry_release(reflex, swapchain, 0);
    xunlock();
    return r;
}

VkReflexSleepResult vk_reflex_sleep(VkReflex *reflex, VkReflexSwapchain *swapchain) {
    if (!vk_reflex_available(reflex)) return VK_REFLEX_SLEEP_UNAVAILABLE;
    VkReflexMode mode = VK_REFLEX_OFF;
    xlock();
    int acq = entry_acquire(reflex, swapchain, 1, &mode);
    if (acq == VK_REFLEX_BUSY) return xunlock(), VK_REFLEX_SLEEP_BUSY;
    if (acq != VK_REFLEX_ACQUIRED) return xunlock(), VK_REFLEX_SLEEP_UNAVAILABLE;
    if (mode == VK_REFLEX_OFF) {
        entry_release(reflex, swapchain, 1);
        xunlock();
        return VK_REFLEX_SLEEP_OK;
    }

    uint64_t value = ++swapchain->sleep_value;
    VkLatencySleepInfoNV sleep = {.sType = VK_STRUCTURE_TYPE_LATENCY_SLEEP_INFO_NV,
                                  .signalSemaphore = swapchain->sleep_semaphore,
                                  .value = value};
    swapchain->sleep_signal_pending = 1;
    VkResult r = reflex->latency_sleep(reflex->device, swapchain->swapchain, &sleep);
    xunlock();   // the wait below names no swapchain
    if (r != VK_SUCCESS) {
        if (plat_xchg32(&reflex->sleep_error_logged, 1) == 0) {
            rt_log("vk: Reflex sleep request failed (%d); input continues", (int)r);
        }
        entry_release(reflex, swapchain, 1);
        return VK_REFLEX_SLEEP_ERROR;
    }

    VkSemaphoreWaitInfo wait = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
                                .semaphoreCount = 1,
                                .pSemaphores = &swapchain->sleep_semaphore,
                                .pValues = &value};
    uint64_t start = plat_now_ns();
    int away = kern_core_away();
    r = vkWaitSemaphores(reflex->device, &wait, 100000000ull);
    kern_core_back(&away);
    if (r == VK_SUCCESS) {
        swapchain->sleep_signal_pending = 0;
        entry_release(reflex, swapchain, 1);
        return VK_REFLEX_SLEEP_OK;
    }

    // There is no cancel operation once vkLatencySleepNV scheduled this signal. Keep the semaphore alive until a later
    // successful sleep or device-idle detach proves the timeline value was reached.
    if (plat_xchg32(&reflex->timeout_logged, 1) == 0) {
        rt_log("vk: Reflex sleep timed out after %.3f ms; input continues and the semaphore is retained through idle "
               "teardown",
               (double)(plat_now_ns() - start) / 1000000.0);
    }
    entry_release(reflex, swapchain, 1);
    return r == VK_TIMEOUT ? VK_REFLEX_SLEEP_TIMEOUT : VK_REFLEX_SLEEP_ERROR;
}

void vk_reflex_marker(VkReflex *reflex, VkReflexSwapchain *swapchain, VkLatencyMarkerNV marker,
                      uint64_t app_frame_id) {
    if (!app_frame_id || !vk_reflex_available(reflex)) return;
    xlock();
    if (entry_acquire(reflex, swapchain, 0, NULL) != VK_REFLEX_ACQUIRED) return xunlock();
    VkSetLatencyMarkerInfoNV info = {.sType = VK_STRUCTURE_TYPE_SET_LATENCY_MARKER_INFO_NV,
                                     .presentID = app_frame_id,
                                     .marker = marker};
    reflex->marker(reflex->device, swapchain->swapchain, &info);
    entry_release(reflex, swapchain, 0);
    xunlock();
}

int vk_reflex_chain_submission(VkReflex *reflex, uint64_t app_frame_id, VkLatencySubmissionPresentIdNV *storage,
                               const void **p_next) {
    if (!vk_reflex_available(reflex) || reflex->spec_version < 3 || !app_frame_id || !storage || !p_next) return 0;
    storage->sType = VK_STRUCTURE_TYPE_LATENCY_SUBMISSION_PRESENT_ID_NV;
    storage->pNext = *p_next;
    storage->presentID = app_frame_id;
    *p_next = storage;
    return 1;
}

void vk_reflex_destroy(VkReflex *reflex) {
    if (!reflex) return;
    for (;;) {
        plat_mutex_lock(&reflex->lock);
        VkReflexSwapchain *swapchain = reflex->attached;
        plat_mutex_unlock(&reflex->lock);
        if (!swapchain) break;
        vk_reflex_swapchain_detach(reflex, swapchain);
    }

    int pending = 0;
    for (VkReflexSwapchain *swapchain = reflex->retired; swapchain; swapchain = swapchain->next)
        pending |= swapchain->sleep_signal_pending && swapchain->sleep_semaphore != VK_NULL_HANDLE;
    if (pending && wait_device_idle(reflex) == VK_SUCCESS) {
        for (VkReflexSwapchain *swapchain = reflex->retired; swapchain; swapchain = swapchain->next)
            settle_sleep_signal(reflex, swapchain);
    }

    while (reflex->retired) {
        VkReflexSwapchain *swapchain = reflex->retired;
        reflex->retired = swapchain->next;
        if (swapchain->sleep_semaphore != VK_NULL_HANDLE) {
            if (swapchain->sleep_signal_pending) {
                rt_log("vk: Reflex semaphore for a detached swapchain was left for device destruction after an idle "
                       "wait failed");
            } else {
                vkDestroySemaphore(reflex->device, swapchain->sleep_semaphore, NULL);
            }
        }
        free(swapchain);
    }
    free(reflex);
}
