// Adapted from LAS Recomp runtime/hle.h and runtime/dispatch.c @ da35b83 (MIT, Copyright (c) 2026 Peiikai).
// Host functions at synthetic guest addresses: the HOST_OVERRIDE / HOST_OBSERVER / HOST_FUNCTION
// linker tables, the registry behind them, and the calls between host and guest (rt_call, rt_original).
//
// Every registered function dispatches through hostfn_entry, which counts the call, traces the first one
// (MKW_TRACE=host) and tail-calls the real function. The registry is fixed by hostfn_seal, which builds the dispatch
// table; guest threads start after that, so lookups need no lock.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "prof.h"
#include "platform/platform.h"

#define MAX_HOSTFN 4096   // dispatch.c holds HOST_CAP / 2 host functions

#ifdef _WIN32
// PE/COFF: the linker sorts .ovr$a < .ovr$m (entries, host.h) < .ovr$z by the text after '$'.
__attribute__((section(".ovr$a"), used)) static const RtHostEntry ovr_start[1];
__attribute__((section(".ovr$z"), used)) static const RtHostEntry ovr_stop[1];
__attribute__((section(".obs$a"), used)) static const RtHostEntry obs_start[1];
__attribute__((section(".obs$z"), used)) static const RtHostEntry obs_stop[1];
__attribute__((section(".fns$a"), used)) static const RtHostEntry fns_start[1];
__attribute__((section(".fns$z"), used)) static const RtHostEntry fns_stop[1];
#else
// ELF: the linker defines __start_<section> and __stop_<section> around a section named like a C identifier. Weak: a
// build without observers has no such section, and both bounds are then NULL.
extern const RtHostEntry __start_mkw_ovr[] __attribute__((weak)), __stop_mkw_ovr[] __attribute__((weak));
extern const RtHostEntry __start_mkw_obs[] __attribute__((weak)), __stop_mkw_obs[] __attribute__((weak));
extern const RtHostEntry __start_mkw_fns[] __attribute__((weak)), __stop_mkw_fns[] __attribute__((weak));
#define ovr_start __start_mkw_ovr
#define ovr_stop __stop_mkw_ovr
#define obs_start __start_mkw_obs
#define obs_stop __stop_mkw_obs
#define fns_start __start_mkw_fns
#define fns_stop __stop_mkw_fns
#endif

typedef struct {
    const char *name;
    GuestFn fn;
    uint64_t original;   // observers: the guest definition rt_original calls
    uint64_t calls;
    int blocking;        // runs with the virtual core given up (hostfn_blocking)
    uint64_t ns_render, ns_other, n_render;   // MKW_HOST_TIME: time inside, on the presenting thread and elsewhere
} Slot;
static Slot slots[MAX_HOSTFN];
static int nslots;
static RtHostEntry *added;   // overrides from probes and plugins (plugin/plugin.c), any number
static int nadded, cap_added;
static int sealed;

static GuestFn table_find(const RtHostEntry *start, const RtHostEntry *stop, const char *name) {
    __asm__("" : "+r"(start), "+r"(stop));   // separate objects: keep the compiler from reasoning about them
    for (const RtHostEntry *p = start; p < stop; p++)
        if (p->name && !strcmp(p->name, name)) return p->fn;
    return NULL;
}

// The latest added override of name wins (a plugin loaded later over an earlier one), then the linked table.
GuestFn hostfn_override(const char *name) {
    for (int i = nadded - 1; i >= 0; i--)
        if (!strcmp(added[i].name, name)) return added[i].fn;
    return table_find(ovr_start, ovr_stop, name);
}

// The runtime's own handler of name (a linked override or observer), whatever was added over it.
GuestFn hostfn_stock(const char *name) {
    GuestFn f = table_find(ovr_start, ovr_stop, name);
    return f ? f : table_find(obs_start, obs_stop, name);
}

GuestFn hostfn_observer(const char *name) { return table_find(obs_start, obs_stop, name); }

// Linked HOST_OVERRIDE / HOST_OBSERVER entries (the loader reports how many it installed).
int hostfn_table_size(int observers) {
    const RtHostEntry *p = observers ? obs_start : ovr_start, *e = observers ? obs_stop : ovr_stop;
    __asm__("" : "+r"(p), "+r"(e));
    int n = 0;
    for (; p < e; p++) n += p->name != NULL;
    return n;
}

const char *hostfn_table_name(int observers, int i) {
    const RtHostEntry *p = observers ? obs_start : ovr_start, *e = observers ? obs_stop : ovr_stop;
    __asm__("" : "+r"(p), "+r"(e));
    for (; p < e; p++)
        if (p->name && i-- == 0) return p->name;
    return NULL;
}

void hostfn_add_override(const char *name, GuestFn fn) {
    if (ldr_count()) rt_fatal("hostfn_add_override(%s) after ldr_load", name);
    if (nadded == cap_added) {
        cap_added = cap_added ? cap_added * 2 : 64;
        if (!(added = realloc(added, (size_t)cap_added * sizeof *added))) rt_fatal("hostfn_add_override: out of memory");
    }
    added[nadded++] = (RtHostEntry){name, fn};
}

static void hostfn_entry(Cpu *c);

static uint64_t slot_addr(int i) { return RT_HOSTFN_BASE + 16ull * (uint64_t)i; }

uint64_t hostfn_register(const char *name, GuestFn fn) {
    if (sealed) rt_fatal("hostfn_register(%s) after hostfn_seal", name);
    if (!name || !fn) rt_fatal("hostfn_register: empty entry");
    for (int i = 0; i < nslots; i++)
        if (slots[i].fn == fn && !strcmp(slots[i].name, name)) return slot_addr(i);
    if (nslots == MAX_HOSTFN) rt_fatal("hostfn_register: more than %d host functions", MAX_HOSTFN);
    uint64_t a = slot_addr(nslots);
    if (a >= RT_RET_HOST) rt_fatal("hostfn_register: host-function window full");
    slots[nslots] = (Slot){.name = name, .fn = fn, .blocking = !strncmp(name, "_ZN2nn2fs", 9)};
    rt_register_host(a, hostfn_entry);
    nslots++;
    return a;
}

// Loader: an observer slot and the guest definition it wraps.
uint64_t hostfn_register_observer(const char *name, GuestFn fn, uint64_t original) {
    uint64_t a = hostfn_register(name, fn);
    slots[(a - RT_HOSTFN_BASE) >> 4].original = original;
    return a;
}

const char *hostfn_name(uint64_t addr) {
    if (addr == RT_RET_HOST) return "<return to host>";
    uint64_t i = (addr - RT_HOSTFN_BASE) >> 4;
    if ((addr & 15) || i >= (uint64_t)nslots) return NULL;
    return slots[i].name;
}

__attribute__((noinline, noreturn, cold)) static void bad_entry(Cpu *c) {
    char lr[96];
    rt_fatal("host function call at %#llx: no host function registered there (lr %s)", (unsigned long long)c->pc,
             ldr_describe(c->x[30], lr, sizeof lr));
}

// kern_host_track: the call counted in the thread's host depth. Not a tail call, so the host stack holds this frame
// while fn runs; longjmp out of fn (ExitThread) leaves the depth raised on a thread that ends.
// fn may be SwitchToFiber: the fiber can come back on another guest thread, which the switch wrote to c->thread.
__attribute__((noinline)) static void hostfn_tracked(Cpu *c, GuestFn fn) {
    int *d = kern_host_depth(c->thread);
    __atomic_store_n(d, *d + 1, __ATOMIC_RELAXED);
    fn(c);
    void *t = __atomic_load_n(&c->thread, __ATOMIC_RELAXED);
    d = kern_host_depth(t);
    int n = *d - 1;
    __atomic_store_n(d, n, __ATOMIC_RELAXED);
    if (!n && kern_preempt_pending(t)) kern_host_left(t);
}

// nn::fs: the call waits for the disk and the OS, and on the Switch it is an IPC request that blocks its thread, so
// the thread gives its virtual core up for it. MKW_CORE_DIAG found the game's loader threads (priority 58) holding
// a core through CloseFile and CreateCacheStorage while job workers and better threads waited 2-6 ms for it.
__attribute__((noinline)) static void hostfn_blocking(Cpu *c, GuestFn fn) {
    int away = kern_core_away();
    if (kern_host_track && c->thread) hostfn_tracked(c, fn);
    else fn(c);
    kern_core_back(&away);
}

// MKW_HOST_TIME=1: time every host function call takes on the guest thread that made it, the work done in series with
// the game (01/11/2026: nothing serial that can run elsewhere). Inclusive: a host function that calls guest code
// that calls another counts both. hostfn_time_report lists the costliest every 600 presents.
static int host_time = -1;

__attribute__((noinline)) static void hostfn_timed(Cpu *c, Slot *s) {
    uint64_t t0 = plat_now_ns();
    int render = plat_thread_id() == prof_render_tid;
    if (s->blocking) hostfn_blocking(c, s->fn);
    else if (kern_host_track && c->thread) hostfn_tracked(c, s->fn);
    else s->fn(c);
    uint64_t d = plat_now_ns() - t0;
    if (render) __atomic_fetch_add(&s->ns_render, d, __ATOMIC_RELAXED), __atomic_fetch_add(&s->n_render, 1, __ATOMIC_RELAXED);
    else __atomic_fetch_add(&s->ns_other, d, __ATOMIC_RELAXED);
}

static int by_time(const void *a, const void *b) {
    const Slot *x = *(const Slot *const *)a, *y = *(const Slot *const *)b;
    uint64_t tx = x->ns_render + x->ns_other, ty = y->ns_render + y->ns_other;
    return tx < ty ? 1 : tx > ty ? -1 : 0;
}

void hostfn_time_report(uint64_t frame) {
    if (frame % 600 == 0) hostfn_time_window(frame);
}

void hostfn_time_window(uint64_t frame) {
    static uint64_t t_last;
    static const Slot *order[MAX_HOSTFN];
    static Slot prev[MAX_HOSTFN];
    static Slot win[MAX_HOSTFN];
    if (host_time <= 0) return;
    uint64_t now = plat_now_ns(), span = t_last ? now - t_last : 0;
    t_last = now;
    int n = 0;
    for (int i = 0; i < nslots; i++) {
        Slot cur = slots[i];
        win[i] = cur;
        win[i].ns_render -= prev[i].ns_render, win[i].ns_other -= prev[i].ns_other;
        win[i].n_render -= prev[i].n_render, win[i].calls -= prev[i].calls;
        prev[i] = cur;
        if (win[i].ns_render + win[i].ns_other) order[n++] = &win[i];
    }
    if (!span) return;
    qsort(order, n, sizeof *order, by_time);
    rt_log("host time: frame %llu, %.1f s; per second of wall clock (render thread / other guest threads):",
           (unsigned long long)frame, span / 1e9);
    for (int i = 0; i < n && i < 25; i++)
        rt_log("  %7.1f ms/s render %7.1f ms/s other %8llu calls  %s", order[i]->ns_render * 1e3 / span,
               order[i]->ns_other * 1e3 / span, (unsigned long long)order[i]->calls, order[i]->name);
}

static void hostfn_entry(Cpu *c) {
    uint64_t i = (c->pc - RT_HOSTFN_BASE) >> 4;
    if (__builtin_expect((c->pc & 15) || i >= (uint64_t)nslots, 0)) bad_entry(c);
    Slot *s = &slots[i];
    if (__builtin_expect(host_time, 0)) {
        if (host_time < 0) {
            const char *e = cfg_get("MKW_HOST_TIME");
            host_time = e && *e && *e != '0';
        }
        if (host_time) {
            __atomic_fetch_add(&s->calls, 1, __ATOMIC_RELAXED);
            hostfn_timed(c, s);
            return;
        }
    }
    uint64_t n = __atomic_fetch_add(&s->calls, 1, __ATOMIC_RELAXED);
    if (__builtin_expect(!n && (rt_trace & RT_TRACE_HOSTCALL), 0)) {
        char lr[96];
        rt_log("host call %s from %s", s->name, ldr_describe(c->x[30] - 4, lr, sizeof lr));
    }
    if (s->blocking) {
        hostfn_blocking(c, s->fn);
        return;
    }
    if (__builtin_expect(kern_host_track && c->thread, 0)) {
        hostfn_tracked(c, s->fn);
        return;
    }
    __attribute__((musttail)) return s->fn(c);
}

// A guest jump to the rt_call return sentinel: guest code used x30 as data, which a lifted RET never does.
static void return_sentinel(Cpu *c) {
    rt_fatal("guest code jumped to the host return address %#llx (x0 %#llx)", (unsigned long long)RT_RET_HOST,
             (unsigned long long)c->x[0]);
}

uint64_t hostfn_address(GuestFn fn) {
    if (!sealed) rt_fatal("hostfn_address before hostfn_seal");
    for (int i = 0; i < nslots; i++)
        if (slots[i].fn == fn) return slot_addr(i);
    rt_fatal("hostfn_address: %p is not a HOST_FUNCTION", (void *)fn);
}

void hostfn_seal(void) {
    static RtModule mods[16];   // kept by the dispatcher
    if (sealed) return;
    const RtHostEntry *p = fns_start, *e = fns_stop;
    __asm__("" : "+r"(p), "+r"(e));
    for (; p < e; p++)
        if (p->name) hostfn_register(p->name, p->fn);
    rt_register_host(RT_RET_HOST, return_sentinel);
    rt_patch_hosts();
    sealed = 1;
    int n = rt_linked_modules(mods, 16);
    rt_dispatch_init(mods, n);
}

uint64_t hostfn_calls(void) {
    uint64_t n = 0;
    for (int i = 0; i < nslots; i++) n += __atomic_load_n(&slots[i].calls, __ATOMIC_RELAXED);
    return n;
}

static int by_calls(const void *a, const void *b) {
    uint64_t x = (*(const Slot *const *)a)->calls, y = (*(const Slot *const *)b)->calls;
    return x < y ? 1 : x > y ? -1 : 0;
}

void hostfn_report(void) {
    static const Slot *order[MAX_HOSTFN];
    int n = 0;
    for (int i = 0; i < nslots; i++)
        if (__atomic_load_n(&slots[i].calls, __ATOMIC_RELAXED)) order[n++] = &slots[i];
    qsort(order, n, sizeof *order, by_calls);
    rt_log("host calls: %llu in %d of %d host functions", (unsigned long long)hostfn_calls(), n, nslots);
    for (int i = 0; i < n; i++) rt_log("  %10llu  %s", (unsigned long long)order[i]->calls, order[i]->name);
}

uint64_t rt_call(Cpu *c, uint64_t fn, int n, ...) {
    if (n < 0 || n > 8) rt_fatal("rt_call: %d arguments", n);
    if (!sealed) hostfn_seal();
    va_list ap;
    va_start(ap, n);
    for (int i = 0; i < n; i++) c->x[i] = va_arg(ap, uint64_t);
    va_end(ap);
    uint64_t lr = c->x[30], pc = c->pc;
    c->x[30] = RT_RET_HOST;
    c->pc = fn;
    rt_fpcr_sync(c);   // guest code may start here on a host thread or fiber that has not run it
    rt_dispatch(c);
    c->x[30] = lr;
    c->pc = pc;
    return c->x[0];
}

uint64_t rt_original(Cpu *c) {
    uint64_t i = (c->pc - RT_HOSTFN_BASE) >> 4;
    if ((c->pc & 15) || i >= (uint64_t)nslots || !slots[i].original)
        rt_fatal("rt_original at %#llx: not inside an observer with a guest definition", (unsigned long long)c->pc);
    uint64_t lr = c->x[30], pc = c->pc;
    c->x[30] = RT_RET_HOST;
    c->pc = slots[i].original;
    rt_dispatch(c);
    c->x[30] = lr;
    c->pc = pc;
    return c->x[0];
}
