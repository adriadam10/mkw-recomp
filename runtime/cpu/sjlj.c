// Guest setjmp/longjmp across lifted code.
//
// The guest's setjmp and longjmp (the SDK's, imported by main) run as lifted code and save and restore the guest
// registers themselves. What lifting breaks is longjmp's last instruction, BR x30 to the setjmp call's return
// address: that is the middle of a C function, not an entry, and the frame that would continue there was left long
// ago (a RET is a C return). 1.8.0 in Thai longjmps out of its text code to main+0x1bb0b34 (discussion 193).
//
// So recomp/gen.py builds every function that calls setjmp (its "owner") with all its labels and enters its body
// through rt_sj_owner, which keeps a host setjmp point in a frame that lives as long as the owner does. A dispatch
// that finds no function at an address inside a live owner is that longjmp (rt_sj_resume): the guest registers are
// restored already, so the host jumps back to the owner's frame and the body continues at c->pc. The idea is
// Hookshot's (GPL-3.0, docs/project/research.md); the code is ours. Every owner in MKW's main runs its work after setjmp
// and before it returns, so the owner's own frame is enough (Hookshot also wraps owners that return first).
#ifdef _WIN32
#define __USE_MINGW_SETJMP_NON_SEH   // as runtime/core/kernel/kernel_int.h: longjmp restores registers, no SEH unwind
#endif
#include <setjmp.h>

#include "cpu.h"
#include "dispatch.h"
#include "host.h"

typedef struct SjSlot {
    struct SjSlot *up;   // the owner below, or NULL
    uint64_t lo, hi;     // the owner's guest code [lo, hi)
    int depth;           // the thread's host depth at entry (hostfn.c), put back by a longjmp out of host calls
    jmp_buf jb;
} SjSlot;

void rt_sj_owner(Cpu *c, GuestFn body, uint64_t lo, uint64_t hi) {
    SjSlot s = {.up = c->sj, .lo = lo, .hi = hi, .depth = c->thread ? *kern_host_depth(c->thread) : 0};
    c->sj = &s;
    // Both returns run the body: first from the entry, after a longjmp from the setjmp's return address. c and body
    // are not written after setjmp, so their values survive the longjmp.
    setjmp(s.jb);
    body(c);
    c->sj = s.up;
}

// From rt_dispatch's slow path, for an address that is no function start: returns only when no live owner holds it.
void rt_sj_resume(Cpu *c) {
    for (SjSlot *s = c->sj; s; s = s->up) {
        if (c->pc <= s->lo || c->pc >= s->hi) continue;
        static int logged;
        if (!__atomic_exchange_n(&logged, 1, __ATOMIC_RELAXED)) {
            char at[64], owner[64];
            rt_log("sjlj: longjmp to %s resumes setjmp owner %s", rt_guest_name(c->pc, at, sizeof at),
                   rt_guest_name(s->lo, owner, sizeof owner));
        }
        c->sj = s;   // owners above it are gone, as their guest frames are
        if (c->thread) *kern_host_depth(c->thread) = s->depth;
        longjmp(s->jb, 1);
    }
}
