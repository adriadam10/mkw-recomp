// nn::time functions MKW imports that totk-pc-recomp does not replace (ov_system.c has the clocks and calendars).
// The host clock is the network clock (ov_system.c answers StandardNetworkSystemClock from it), so it is accurate and
// the network clock never needs a correction from the ntc service.
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "platform/platform.h"

// bool StandardNetworkSystemClock::IsAccuracySufficient()
static void ov_accuracy(Cpu *c) { ret_x(c, 1); }

// void GetDeviceLocationName(LocationName *out): char[0x24], the host's time zone (plat_time_zone), else UTC
static void ov_location(Cpu *c) {
    char *out = arg_p(c, 0);
    if (!out) return;
    memset(out, 0, 0x24);
    if (!plat_time_zone(out, 0x24)) memcpy(out, "UTC", 3);
    static int logged;
    if (!logged++) rt_log("time: device location %s", out);
}

// Result LoadTimeZoneRule(TimeZoneRule *out, const LocationName &): MKW converts times with ToCalendarTime, which
// ov_system.c answers in host local time without a rule; the rule is left empty.
static void ov_load_rule(Cpu *c) { ret_x(c, 0); }

// AsyncEnsureNetworkClockAvailabilityTask (main+0xef94f8, the online setup with MKW_NPLN) wraps nn::ntc's correction
// task. The host clock is the network clock, so the task is done at once. The SDK's Initialize opened the ntc service
// (sdk+0x5325e0), so the whole task is replaced. Its finish event is a real nn::os::SystemEvent, made signalled the
// first time it is asked for, one per task object (guest-visible memory of the kernel region, never freed: the game
// makes one task per online session).
static void ov_clock_task_ok(Cpu *c) { ret_x(c, 0); }
static void ov_clock_task_event(Cpu *c) {
    static PlatMutex lock = PLAT_MUTEX_INIT;
    static struct {
        uint64_t task, ev;
    } evs[16];
    static uint64_t create, signal;
    uint64_t task = arg_x(c, 0), ev = 0;
    if (!create) {
        create = ldr_symbol("_ZN2nn2os17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb");
        signal = ldr_symbol("_ZN2nn2os17SignalSystemEventEPNS0_15SystemEventTypeE");
    }
    plat_mutex_lock(&lock);
    int i = 0;
    while (i < 16 && evs[i].task && evs[i].task != task) i++;
    if (i == 16) i = (int)(task >> 4) % 16;   // all taken: reuse one (an old session's)
    if (evs[i].task == task) ev = evs[i].ev;
    plat_mutex_unlock(&lock);
    if (!ev) {
        ev = (uint64_t)(uintptr_t)mem_kernel_alloc(0x100, 16);
        rt_call(c, create, 3, ev, 0, 0);
        rt_call(c, signal, 1, ev);
        plat_mutex_lock(&lock);
        evs[i].task = task, evs[i].ev = ev;
        plat_mutex_unlock(&lock);
    }
    ret_x(c, ev);
}

// The SDK's musl reads CLOCK_MONOTONIC (1) from the system tick and CLOCK_REALTIME (0) through a function pointer that
// nntimeStartup sets (sdk+0x91c78, from nn::time) and nothing sets here, so the realtime clock failed with EAGAIN. Its
// callers do not all check: gRPC's gpr_now (main+0x104b1e8) aborted on stack garbage, and the SDK's own
// pthread_cond_timedwait (sdk+0x150300) returned EAGAIN at once, which gRPC's timer thread (gpr_cv_wait,
// main+0x104a850) takes for a spurious wakeup and spins on at full speed (MKW_NPLN: the game crawls at 0.02x).
// HOST_OVERRIDE("clock_gettime") alone misses the SDK's internal direct calls, so the pointer itself is set to a host
// function giving the host's wall clock. Its slot comes from the code: clock_gettime's stub branches to the
// implementation, whose realtime case calls a stub "adrp x8, page; ldr x1, [x8, #off]; br x1".
static void ov_realtime(Cpu *c) {   // int (*)(struct timespec *)
    int64_t *ts = arg_p(c, 0);
    uint64_t ns = plat_unix_time_ns();
    if (ts) ts[0] = (int64_t)(ns / 1000000000u), ts[1] = (int64_t)(ns % 1000000000u);
    ret_x(c, 0);
}
HOST_FUNCTION("ov_realtime", ov_realtime);

static int64_t sext(uint64_t v, int bits) { return (int64_t)(v << (64 - bits)) >> (64 - bits); }
static uint64_t branch_target(uint64_t pc) {   // B or BL at pc, else 0
    uint32_t w = *(const uint32_t *)(uintptr_t)pc;
    if ((w & 0x7c000000) != 0x14000000) return 0;
    return pc + (uint64_t)(sext(w & 0x3ffffff, 26) * 4);
}
static uint64_t realtime_slot(void) {
    uint64_t stub = ldr_symbol("clock_gettime"), impl = stub ? branch_target(stub) : 0;
    for (int i = 0; impl && i < 48; i++) {
        uint64_t pc = impl + 4 * (uint64_t)i;
        uint32_t w = *(const uint32_t *)(uintptr_t)pc;
        if ((w & 0xfc000000) != 0x94000000) continue;   // BL
        uint64_t t = branch_target(pc);
        uint32_t adrp = ((const uint32_t *)(uintptr_t)t)[0], ldr = ((const uint32_t *)(uintptr_t)t)[1],
                 br = ((const uint32_t *)(uintptr_t)t)[2];
        if ((adrp & 0x9f00001f) != 0x90000008 || (ldr & 0xffc003ff) != 0xf9400101 || br != 0xd61f0020) continue;
        int64_t page = sext(((uint64_t)((adrp >> 5) & 0x7ffff) << 2) | ((adrp >> 29) & 3), 21) << 12;
        return (t & ~0xfffull) + (uint64_t)page + 8 * (uint64_t)((ldr >> 10) & 0xfff);
    }
    return 0;
}
static void realtime_install(void) {
    static volatile int32_t once;
    if (plat_inc32(&once) != 1) return;
    uint64_t slot = realtime_slot();
    if (!slot) {
        rt_log("time: the SDK's realtime clock hook was not found; CLOCK_REALTIME stays unset");
        return;
    }
    *(uint64_t *)(uintptr_t)slot = hostfn_address(ov_realtime);
    rt_log("time: the SDK's CLOCK_REALTIME reads the host clock (hook at %#llx)", (unsigned long long)slot);
}

// int clock_gettime(clockid_t id, struct timespec *ts): installs the realtime hook on the first call, then the SDK's
// own code for every clock
static void ov_clock_gettime(Cpu *c) {
    static uint64_t sdk;
    realtime_install();
    if (!sdk && !(sdk = ldr_symbol("clock_gettime"))) rt_fatal("time: the SDK's clock_gettime not found");
    ret_x(c, rt_call(c, sdk, 2, arg_x(c, 0), arg_x(c, 1)));
}

HOST_OVERRIDE("clock_gettime", ov_clock_gettime);
HOST_OVERRIDE("_ZN2nn4time26StandardNetworkSystemClock20IsAccuracySufficientEv", ov_accuracy);
HOST_OVERRIDE("_ZN2nn4time21GetDeviceLocationNameEPNS0_12LocationNameE", ov_location);
HOST_OVERRIDE("_ZN2nn4time16LoadTimeZoneRuleEPNS0_12TimeZoneRuleERKNS0_12LocationNameE", ov_load_rule);
HOST_OVERRIDE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTaskC1Ev", ov_clock_task_ok);
HOST_OVERRIDE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTaskD1Ev", ov_clock_task_ok);
HOST_OVERRIDE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTask10InitializeENS_2os14EventClearModeE", ov_clock_task_ok);
HOST_OVERRIDE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTask9StartTaskEv", ov_clock_task_ok);
HOST_OVERRIDE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTask9GetResultEv", ov_clock_task_ok);
HOST_OVERRIDE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTask6CancelEv", ov_clock_task_ok);
HOST_OVERRIDE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTask14GetFinishEventEv", ov_clock_task_event);
