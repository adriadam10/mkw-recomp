// nn::time functions MKW imports that totk-pc-recomp does not replace (ov_system.c has the clocks and calendars).
// The host clock is the network clock (ov_system.c answers StandardNetworkSystemClock from it), so it is accurate and
// the network clock never needs a correction from the ntc service.
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
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

// AsyncEnsureNetworkClockAvailabilityTask wraps nn::ntc's correction task. Its SDK Initialize opens the ntc service
// (reached by the online mode: main+0xef9508), so the task is the host's: the object (0x98 bytes, its constructor
// the SDK's: all zero) holds its finish event at +8, where the SDK's GetFinishEvent points (the services probe makes
// it there), and the SDK's "initialized" flag at 0x90 (sdk+0x52b0f4).
// StartTask finishes at once: it signals that event, and GetResult reports success.
#define TASK_EVENT 8
#define TASK_INITIALIZED 0x90
static uint64_t sdk_fn(const char *name) {
    uint64_t a = ldr_symbol(name);
    if (!a) rt_fatal("time: SDK function %s not found", name);
    return a;
}
static uint64_t fn_create, fn_signal, fn_destroy;
static void event_fns(void) {
    if (fn_create) return;
    fn_create = sdk_fn("_ZN2nn2os17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb");
    fn_signal = sdk_fn("_ZN2nn2os17SignalSystemEventEPNS0_15SystemEventTypeE");
    fn_destroy = sdk_fn("_ZN2nn2os18DestroySystemEventEPNS0_15SystemEventTypeE");
}
// Result Initialize(EventClearMode)
static void ov_task_initialize(Cpu *c) {
    uint8_t *self = arg_p(c, 0);
    event_fns();
    if (!self[TASK_INITIALIZED]) {
        uint32_t r = (uint32_t)rt_call(c, fn_create, 3, (uint64_t)(uintptr_t)(self + TASK_EVENT), arg_x(c, 1) & 1,
                                       (uint64_t)0);
        if (r) return ret_x(c, r);
        self[TASK_INITIALIZED] = 1;
    }
    ret_x(c, 0);
}
// SystemEvent &GetFinishEvent()
static void ov_task_event(Cpu *c) { ret_x(c, arg_x(c, 0) + TASK_EVENT); }
static void ov_task_start(Cpu *c) {
    uint8_t *self = arg_p(c, 0);
    event_fns();
    rt_call(c, fn_signal, 1, (uint64_t)(uintptr_t)(self + TASK_EVENT));   // made by Initialize, as upstream assumes
    ret_x(c, 0);
}
static void ov_task_result(Cpu *c) { ret_x(c, 0); }
static void ov_task_destroy(Cpu *c) {
    uint8_t *self = arg_p(c, 0);
    if (!self || !self[TASK_INITIALIZED]) return;
    event_fns();
    rt_call(c, fn_destroy, 1, (uint64_t)(uintptr_t)(self + TASK_EVENT));
    self[TASK_INITIALIZED] = 0;
}

// int clock_gettime(clockid_t, struct timespec *): the SDK's own (the host's for CLOCK_REALTIME), its timespec made valid. gRPC (the online
// mode's NPLN client) aborts on a tv_nsec outside [0, 1e9) (main+0x104b258, gpr_now); the first ones are logged with
// their clock, so the source can be fixed where it is.
static void obs_clock_gettime(Cpu *c) {
    int id = (int)arg_x(c, 0);
    int64_t *ts = arg_p(c, 1);
    // CLOCK_REALTIME: the SDK's goes through a time service hook the runtime never sets and fails (-1, the timespec
    // left as it was: measured 12/11/2026 at main+0x104b1e8); the host's wall clock is the network clock already
    // (ov_system.c StandardNetworkSystemClock), so it is the real time here too.
    if (id == 0 && ts) {
        uint64_t ns = plat_unix_time_ns();
        ts[0] = (int64_t)(ns / 1000000000ull);
        ts[1] = (int64_t)(ns % 1000000000ull);
        return ret_x(c, 0);
    }
    uint64_t r = rt_original(c);
    static int traced;
    if (traced < 12 && cfg_get("MKW_CLOCK_LOG")) {
        char lr[96];
        traced++;
        rt_log("time: clock_gettime(%d) = %d {%lld s, %lld ns} from %s", id, (int)r, ts ? (long long)ts[0] : 0,
               ts ? (long long)ts[1] : 0, ldr_describe(c->x[30] - 4, lr, sizeof lr));
    }
    if ((uint32_t)r == 0 && ts && (ts[1] < 0 || ts[1] >= 1000000000)) {
        static int logged;
        if (logged < 5) {
            logged++;
            rt_log("time: clock_gettime(%d) gave {%lld s, %lld ns}: normalised", id, (long long)ts[0], (long long)ts[1]);
        }
        int64_t carry = ts[1] / 1000000000, ns = ts[1] % 1000000000;
        if (ns < 0) ns += 1000000000, carry--;
        ts[0] += carry;
        ts[1] = ns;
    }
    ret_x(c, r);
}
HOST_OBSERVER("clock_gettime", obs_clock_gettime);

HOST_OVERRIDE("_ZN2nn4time26StandardNetworkSystemClock20IsAccuracySufficientEv", ov_accuracy);
HOST_OVERRIDE("_ZN2nn4time21GetDeviceLocationNameEPNS0_12LocationNameE", ov_location);
HOST_OVERRIDE("_ZN2nn4time16LoadTimeZoneRuleEPNS0_12TimeZoneRuleERKNS0_12LocationNameE", ov_load_rule);
HOST_OVERRIDE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTask9StartTaskEv", ov_task_start);
HOST_OVERRIDE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTask9GetResultEv", ov_task_result);
HOST_OVERRIDE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTask10InitializeENS_2os14EventClearModeE",
              ov_task_initialize);
HOST_OVERRIDE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTask14GetFinishEventEv", ov_task_event);
HOST_OVERRIDE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTaskD1Ev", ov_task_destroy);
