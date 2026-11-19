// Guest address -> host function lookup for indirect branches (rt_dispatch, declared in cpu.h).
//
// One flat table covers every module's code range: slot (pc - lo) >> 2 holds the host function for a guest
// function start, as a 32-bit offset from an anchor address. Lookup is one range check and one load. The table is
// built once by rt_dispatch_init and never written again, so threads read it without locks.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "cpu.h"

// A lifted module: its place in the guest layout and the generator's sorted function table.
typedef struct RtModule {
    const char *name;
    uint64_t base, size;      // guest image (recomp/layout.py)
    const uint64_t *addrs;    // <mod>_fn_addrs: guest function starts, strictly ascending
    const GuestFn *fns;       // <mod>_fn_ptrs: the lifted function for each start
    uint64_t count;           // <mod>_fn_count
} RtModule;

// The game code's modules (core/boot/gamelib.h rt_game), in layout order.
int rt_linked_modules(RtModule *out, int max);

// Register a host function at a guest address outside every module (HLE entry points). Allowed only before
// rt_dispatch_init; the registry is fixed afterwards.
void rt_register_host(uint64_t addr, GuestFn fn);

// Indirect calls to the guest function starting at addr run fn instead (plugins: plugin/plugin.c); a later call for
// the same addr replaces fn. Before rt_dispatch_init only (0 otherwise); an addr where no function starts is fatal
// there. rt_dispatch_original: the translated function it replaced, NULL before rt_dispatch_init or if none.
int rt_dispatch_override(uint64_t addr, GuestFn fn);
GuestFn rt_dispatch_original(uint64_t addr);

// Build the table from the modules (kept by pointer; they must outlive the process). Call it once, before any guest
// thread starts: thread creation publishes the table to the threads. Fatal on a malformed table:
// unsorted or duplicate starts, a start outside its module, overlapping modules, host code spread over 4 GiB.
void rt_dispatch_init(const RtModule *mods, int count);

// A guest longjmp to c->pc inside a live setjmp owner: back to that owner's host frame, not returning (sjlj.c).
// Returns when no owner holds c->pc.
void rt_sj_resume(Cpu *c);

// The host function for a guest address, or NULL when nothing is there. Does not run guest code.
GuestFn rt_dispatch_lookup(uint64_t pc);

// "0x7000001234 (main+0x1234)" for a guest address inside a module, else the bare address. Returns buf.
const char *rt_guest_name(uint64_t pc, char *buf, int size);

typedef struct RtDispatchStats {
    uint64_t lo, hi;          // guest range the table covers
    uint64_t slots;           // (hi - lo) / 4
    uint64_t table_bytes;     // reserved for the table
    uint64_t functions;       // filled slots
    uint64_t host_fns;        // rt_register_host entries
} RtDispatchStats;
RtDispatchStats rt_dispatch_stats(void);

// Runtime fatal error: message to stderr, then exit. Used by the dispatcher and the stubs.
void rt_fatal(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
