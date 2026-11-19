// Internals shared by the kernel files (kernel.c, kernel_cores.c, kernel_preempt.c, kernel_diag.c, kernel_svc.c,
// kernel_fiber.c): the thread record, the kernel lock G and what one file uses of another. Not for other files
// (they use kernel.h or host.h).
#pragma once
#ifdef _WIN32
#define __USE_MINGW_SETJMP_NON_SEH   // setjmp(b) = _setjmp(b, NULL): longjmp restores registers, no SEH unwinding
#endif
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "kernel.h"
#include "platform/platform.h"

#define R_OK 0u
#define R_TIMEOUT KERN_R_TIMEOUT
#define R_CANCELLED KERN_R_CANCELLED
#define R_BAD_HANDLE KERN_R_BAD_HANDLE
#define R_BAD_STATE KERN_R_BAD_STATE
#define R_INVALID_ENUM KERN_R_INVALID_ENUM
#define R_INVALID_CORE 0xE201u   // svc InvalidCoreId (113)
#define R_INVALID_COMBINATION 0xE801u   // svc InvalidCombination (116)
#define WAITERS_BIT KERN_WAITERS_BIT
#define H_CUR_THREAD KERN_H_CURRENT_THREAD

#define MAX_H 4096
#define NCORE 6                          // Switch 2 gives applications cores 0-5 (main.npdm core mask 0x3f)
#define APP_MASK ((1ull << NCORE) - 1)
#define HOST_STACK (256ull << 20)        // reserved, committed on demand
#define TLR_SIZE 0x200
#define THREAD_OBJ_SIZE 0x400            // "current thread object" at TLR+0x1F8 (d2r-mobile thread.c)
#define TLS_SIZE 0x2000
#define MAIN_STACK (1ull << 20)          // main.npdm META+0x1C
#define MAIN_PRIO 44                     // META+0xE
#define MAIN_CORE 0                      // META+0xF
#define FIBER_MAX 1024

enum { O_FREE, O_THREAD, O_EVENT_R, O_EVENT_W, O_TMEM = KERN_TMEM };
_Static_assert(O_EVENT_W < O_TMEM, "handle kinds");
enum { W_NONE = KERN_WAIT_NONE, W_LOCK = KERN_WAIT_LOCK, W_CONDVAR = KERN_WAIT_CONDVAR, W_ADDRESS = KERN_WAIT_ADDRESS,
       W_SYNC = KERN_WAIT_SYNC };

// Scheduler accounting states (MKW_TRACE=sched).
enum { S_OFF, S_RUN, S_CORE, S_HOST, S_SLEEP, S_SYNC, S_CV, S_LOCK, S_ADDR, S_N };
extern const char *const s_name[S_N];

typedef struct Event { int signaled, refs; } Event;
typedef struct Fiber Fiber;

struct RtThread {
    int preempt;               // first member: rt_dispatch reads it (host.h kern_preempt_pending)
    int host_depth;            // second member: host calls and SVCs in progress, while kern_host_track (host.h)
    uint32_t handle;
    uint64_t id;
    Cpu *cpu;                  // the thread's own context
    Cpu *cur;                  // the context running now: cpu, or the current fiber's
    uint64_t entry, arg, stack_top;
    int prio, core;            // core: the scheduler's hint (core_take), always a real core
    int ideal;                 // the ideal core the guest set and GetThreadCoreMask reports: -1 after "don't care"
    uint64_t mask;
    int started, exited, closed, cancel, is_main;
    PlatCond cv;
    // current wait
    int wkind, woke;
    uint64_t waddr, wkey;
    uint32_t wtag, wres;
    uint64_t wseq;             // registration order: equal priority wakes first come first
    RtThread *next;            // wait list
    RtThread *all;             // every thread
    // virtual core
    int cur_core;              // core held while running guest code, -1 while blocked
    uint64_t cseq;
    RtThread *cnext;
    uint8_t *tlr, *tls;
    jmp_buf exit_jmp;          // the thread start; ExitThread longjmps to it
    // fibers
    PlatFiber *base_fiber;     // the host thread as a host fiber, after the first switch
    Fiber *fiber;              // current guest fiber, NULL on the thread's own context
    Fiber *leaving;            // fiber just switched away from, released by whoever resumes next
    int exit_pending;          // ExitThread on a fiber: unwind once back on the thread's own context
    // MKW_TRACE=sched accounting (holding G)
    uint32_t tid;              // host thread id
    int st;                    // S_* state since st_since
    uint64_t st_since, st_ns[S_N], nblk, npre, pre_since, cpu_last;
    uint64_t wobj;             // what the current wait is on: owner handle, key, address or first handle
    uint32_t waker;            // the thread whose signal ended the last wait (0: a host thread or a timeout)
    uint64_t wake_at, grant_at;   // when a wait was signalled and a core granted, until the thread runs again
    uint64_t lw_ns, lw_n, lw_max, lg_ns, lg_n, lg_max, sl_n, sl_req, sl_act;   // those latencies; SleepThread asked/took
    uint64_t sp_n, sp_ho, sp_site;   // spin wait passes and core handoffs, the last loop head (the thread writes them)
    RtThread *target;          // as a core waiter: the owner core_kick asked to give its core up for it (holding G)
    // MKW_ASYNC_PREEMPT (holding G)
    PlatIntr *hthr;            // the host thread, for plat_intr_stop / resume; open while it may own a core
    uint64_t ap_rip;           // the interrupted host rip kern_ap_stub returns to
    uint64_t ap_last;          // the last redirect attempt on this owner
    int ap_pending;            // redirected to kern_ap_stub, which has not reached kern_ap_yield yet
    // MKW_CORE_DIAG (holding G, except dg_host_end, which only the thread itself writes)
    uint64_t dg_since;         // as a waiter: core wait start
    int dg_cls;                // as a waiter: DG_* class at the start of the wait
    uint64_t dg_kick;          // as an owner: the preempt request
    uint64_t dg_host_end;      // as an owner: the return from the host call or SVC it was in at the request
    uint64_t dg_pc, dg_lr;     // as an owner: guest pc and lr at the checkpoint that gave the core up
    int dg_host_kick;          // as an owner: in a host call or SVC at the request
    uint32_t dg_svc;           // the thread's latest SVC (MKW_CORE_DIAG: what an owner in a host call is in)
    struct DgRel { uint64_t kick, host_end, pc, lr; int host, pre, prio; uint32_t h; } dg_rel;   // as a waiter: the
                                                                                                 // owner that released
};

extern PlatMutex G;   // the kernel lock
typedef struct { int type; void *obj; } HandleSlot;
extern HandleSlot g_h[MAX_H];
extern _Thread_local RtThread *tcur;
_Static_assert(offsetof(RtThread, preempt) == 0, "host.h kern_preempt_pending reads the first int");
_Static_assert(offsetof(RtThread, host_depth) == sizeof(int), "host.h kern_host_depth reads the second int");
extern RtThread *g_all, *g_wait, *g_main;
extern uint64_t g_next_id, g_wseq, g_created;
extern uint64_t g_main_stack;

static inline void lock(void) { plat_mutex_lock(&G); }
static inline void unlock(void) { plat_mutex_unlock(&G); }

// session covers the same game time at any frame rate.
typedef struct { uint64_t ts, obj; uint32_t a, b; uint16_t kind, st; uint32_t pad; } CritEv;
enum { CRIT_STATE, CRIT_WAKE, CRIT_FRAME };   // a: thread; b: its waker (STATE), the waker (WAKE); st: new S_* state
#define AP_FIRST_NS 250000ull   // the waiter spins this long before the first interruption (most waits end sooner)
enum { AP_NONE, AP_RETRY, AP_SENT };
enum { DG_GAME, DG_HOST, DG_EQ, DG_NOCORE, DG_N };

// Small helpers, inline in every kernel file.
static inline void st32(uint64_t a, uint32_t v) { __atomic_store_n((uint32_t *)(uintptr_t)a, v, __ATOMIC_SEQ_CST); }
static inline int cas32(uint64_t a, uint32_t *exp, uint32_t v) {
    return __atomic_compare_exchange_n((uint32_t *)(uintptr_t)a, exp, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}
static inline RtThread *cur(uint32_t svc) {
    if (!tcur) rt_fatal("kernel: svc %#x on a host thread (no guest thread)", svc);
    return tcur;
}
static inline uint32_t ld32(uint64_t a) { return __atomic_load_n((uint32_t *)(uintptr_t)a, __ATOMIC_SEQ_CST); }
static inline uint64_t ld64(uint64_t a) { return __atomic_load_n((uint64_t *)(uintptr_t)a, __ATOMIC_SEQ_CST); }

// Defined in one kernel file, used by another.
int st_of(int kind);   // kernel.c
uint32_t h_new(int type, void *obj);   // kernel.c
void *h_peek(uint32_t h, int type);   // kernel.c
void *h_get(uint32_t h, int type);   // kernel.c
RtThread *pick(int kind, uint64_t a);   // kernel.c
RtThread *pick_in(RtThread *list, int kind, uint64_t a);   // kernel.c
void wake_in(RtThread **list, RtThread *t, uint32_t res);   // kernel.c
typedef struct { PlatMutex *m; RtThread **list; } AddrQueue;   // the lock and wait list of one address
AddrQueue addr_queue(uint64_t addr);   // kernel.c
uint32_t addr_block(RtThread *t, AddrQueue q, int64_t ns);   // kernel.c, holding q.m
void wake(RtThread *t, uint32_t res);   // kernel.c
void wake_sync_waiters(void);   // kernel.c
uint32_t block_inner(RtThread *t, int kind, int64_t ns);   // kernel.c
void lock_handoff(uint64_t addr);   // kernel.c
void lock_acquire_for(RtThread *t, uint64_t addr, uint32_t tag);   // kernel.c
RtThread *thr_new(int prio, int core, uint64_t mask);   // kernel.c
void thr_link(RtThread *t);   // kernel.c
void thr_free(RtThread *t);   // kernel.c
__attribute__((noreturn)) void exit_now(RtThread *t);   // kernel.c
void thread_main(void *p);   // kernel.c
void sleep_ns(RtThread *t, int64_t ns);   // kernel.c
extern RtThread *g_core_owner[NCORE], *g_core_wait;   // kernel_cores.c
int cores_enabled(void);   // kernel_cores.c
void core_take_num(RtThread *t);
int core_allowed(const RtThread *t, int c);   // kernel_cores.c
void core_put(RtThread *t);   // kernel_cores.c
void core_kick(void);   // kernel_cores.c
void core_take(RtThread *t);   // kernel_cores.c
uint32_t block(RtThread *t, int kind, int64_t ns);   // kernel_cores.c
void ap_sync(RtThread *t);   // kernel_cores.c
extern int g_crit_on;   // kernel_diag.c
void crit_add(int kind, uint32_t a, uint32_t b, int st, uint64_t obj, uint64_t ts);   // kernel_diag.c
extern uint64_t g_pre_req, g_pre_n, g_pre_ns, g_pre_max, g_pre_stale;   // kernel_diag.c
extern uint64_t g_sync_calls, g_sync_blocks;   // kernel_diag.c
void acct(RtThread *t, int st);   // kernel_diag.c
extern int g_diag;   // kernel_diag.c
extern uint64_t g_dg_samples, g_dg_lines, g_sync36, g_sync36_flag;   // kernel_diag.c
void diag_init(void);   // kernel_diag.c
int dg_in_host(const RtThread *o);   // kernel_diag.c
int dg_class0(const RtThread *t);   // kernel_diag.c
void dg_done(RtThread *t);   // kernel_diag.c
void dg_sample(RtThread *t);   // kernel_diag.c
extern int g_ap, g_ap_int;   // kernel_preempt.c
extern uint64_t g_ap_sent, g_ap_taken, g_ap_host, g_ap_pc, g_ap_deferred, g_ap_fail, g_ap_spin_ok;   // kernel_preempt.c
void ap_init(void);   // kernel_preempt.c
int prio_of(const RtThread *t);   // kernel_preempt.c
volatile uint16_t *ap_tlr(const RtThread *t);   // kernel_preempt.c
int ap_disabled(const RtThread *t);   // kernel_preempt.c
int ap_try(RtThread *w);   // kernel_preempt.c
