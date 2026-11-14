// Part of the kernel (kernel.c; split from it 06/11/2026, same sources and licences as its header).
// Synchronization objects and the thread and synchronization SVCs (kern_svc).
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "kernel_int.h"
#include "platform/platform.h"

// ---- synchronization objects
static int sig_obj(uint32_t h, int *bad) {   // 1 = signaled (holding G)
    unsigned i = h - 0x1000;
    if (h == H_CUR_THREAD) return 0;
    if (i == 0 || i >= MAX_H) {
        *bad = 1;
        return 0;
    }
    switch (g_h[i].type) {
    case O_THREAD: return ((RtThread *)g_h[i].obj)->exited;
    case O_EVENT_R: return ((Event *)g_h[i].obj)->signaled;
    default: *bad = 1; return 0;
    }
}

int kern_thread_state(uint32_t h) {
    lock();
    RtThread *t = h_peek(h, O_THREAD);
    int r = !t ? -1 : t->exited ? KERN_THREAD_EXITED : t->wkind;
    unlock();
    return r;
}

// CloseHandle: an event goes with its last end; a thread object once it has also exited.
static uint32_t handle_close(uint32_t h) {
    unsigned i = h - 0x1000;
    int ok = 0;
    void *tmem = NULL;
    lock();
    if (i > 0 && i < MAX_H) {
        int type = g_h[i].type;
        void *o = g_h[i].obj;
        if (type == O_EVENT_R || type == O_EVENT_W) {
            Event *e = o;
            if (--e->refs == 0) free(e);
            ok = 1;
        } else if (type == O_TMEM) {
            tmem = o;
            ok = 1;
        } else if (type == O_THREAD) {
            RtThread *t = o;
            t->closed = 1;
            if (t->exited) thr_free(t);
            ok = 1;
        }
        if (ok) {
            g_h[i].type = O_FREE;
            g_h[i].obj = NULL;
        }
    }
    unlock();
    free(tmem);
    return ok ? R_OK : R_BAD_HANDLE;
}

void kern_handle_close(uint32_t h) {
    if (handle_close(h)) rt_fatal("kern_handle_close(%#x): bad handle", h);
}

// Hooks of the probe's condvar negative control.
volatile int kern_test_cv_old_order;
void (*volatile kern_test_cv_hook)(void);

static void cv_hook(void) {
    void (*h)(void) = kern_test_cv_hook;
    if (h) h();
}

int kern_svc(Cpu *c, uint32_t imm) {
    uint64_t *x = c->x;
    if (g_diag > 0 && tcur) tcur->dg_svc = imm;
    switch (imm) {
    case 0x08: {   // CreateThread(-> w1 handle; x1 entry, x2 arg, x3 stack top, w4 priority, w5 core)
        int prio = (int)x[4], core = (int)x[5];
        if (!x[1] || !x[3] || prio < 28 || prio > 59 || core < -3 || core >= NCORE) {
            x[0] = R_BAD_STATE;
            return 1;
        }
        RtThread *t = thr_new(prio, core >= 0 ? core : MAIN_CORE, core >= 0 ? 1ull << core : APP_MASK);
        t->entry = x[1];
        t->arg = x[2];
        t->stack_top = x[3] & ~15ull;
        lock();
        thr_link(t);
        unlock();
        if (!plat_thread_spawn(thread_main, t, "guest", HOST_STACK)) rt_fatal("kernel: cannot start a guest thread");
        if (rt_trace & RT_TRACE_SVC) {
            char d[128];
            rt_log("CreateThread %#x entry %s stack %#llx prio %d core %d", t->handle,
                   ldr_describe(t->entry, d, sizeof d), (unsigned long long)t->stack_top, prio, core);
        }
        x[0] = R_OK;
        x[1] = t->handle;
        return 1;
    }
    case 0x09: {   // StartThread(w0)
        lock();
        RtThread *t = h_get((uint32_t)x[0], O_THREAD);
        if (t) {
            t->started = 1;
            plat_cond_signal(&t->cv);
        }
        unlock();
        x[0] = t ? R_OK : R_BAD_HANDLE;
        return 1;
    }
    case 0x0A: {   // ExitThread
        RtThread *t = cur(imm);
        if (t->fiber) {   // on a guest fiber: unwind from the thread's own context (kern_fiber_switch resumes it)
            t->exit_pending = 1;
            t->leaving = t->fiber;
            t->fiber = NULL;
            t->cur = t->cpu;
            plat_fiber_switch(t->base_fiber);
            rt_fatal("kernel: ExitThread on a fiber came back");
        }
        exit_now(t);
    }
    case 0x0B: {   // SleepThread(x0 ns); 0, -1, -2 yield
        int64_t ns = (int64_t)x[0];
        RtThread *t = cur(imm);
        if (ns > 0) {
            lock();
            core_put(t);
            acct(t, S_SLEEP);
            t->wobj = (uint64_t)ns;
            t->nblk++;
            unlock();
            uint64_t t0 = kern_now_ns();
            sleep_ns(t, ns);
            uint64_t took = kern_now_ns() - t0;
            lock();
            t->sl_n++;
            t->sl_req += (uint64_t)ns;
            t->sl_act += took;
            core_take(t);
            unlock();
            return 1;
        }
        // Yield: hand the core to a waiting thread of equal or better priority.
        lock();
        int give = 0;
        int tp = prio_of(t);
        for (RtThread *w = g_core_wait; w && !give; w = w->cnext)
            give = t->cur_core >= 0 && core_allowed(w, t->cur_core) && prio_of(w) <= tp;
        if (give) {
            core_put(t);
            core_take(t);
        }
        unlock();
        if (!give) plat_yield();
        return 1;
    }
    case 0x0C: {   // GetThreadPriority(w1 handle -> w1)
        lock();
        RtThread *t = h_get((uint32_t)x[1], O_THREAD);
        x[0] = t ? R_OK : R_BAD_HANDLE;
        if (t) x[1] = (uint64_t)t->prio;
        unlock();
        return 1;
    }
    case 0x0D: {   // SetThreadPriority(w0 handle, w1 priority)
        if ((uint32_t)x[1] < 28 || (uint32_t)x[1] > 59) {
            x[0] = R_BAD_STATE;
            return 1;
        }
        lock();
        RtThread *t = h_get((uint32_t)x[0], O_THREAD);
        if (t) t->prio = (int)x[1];
        x[0] = t ? R_OK : R_BAD_HANDLE;
        unlock();
        return 1;
    }
    case 0x0E: {   // GetThreadCoreMask(w2 handle -> w1 core, x2 mask)
        lock();
        RtThread *t = h_get((uint32_t)x[2], O_THREAD);
        x[0] = t ? R_OK : R_BAD_HANDLE;
        if (t) {
            x[1] = (uint64_t)(uint32_t)t->core;
            x[2] = t->mask;
        }
        unlock();
        return 1;
    }
    case 0x0F: {   // SetThreadCoreMask(w0 handle, w1 ideal core, x2 mask): -1 don't care, -2 the process's, -3 no update
        lock();
        RtThread *t = h_get((uint32_t)x[0], O_THREAD);
        int core = (int)x[1];
        uint64_t mask = core == -2 ? 1ull << MAIN_CORE : x[2];   // -2: the process's ideal core, alone
        if (core == -2) core = MAIN_CORE;
        // the checks of Atmosphère's kern_svc_thread.cpp SetThreadCoreMask (-1/-3: svc_types_common.hpp)
        int keep = core == -3 && t ? t->core : core;   // no update: the ideal core stays and must be in the mask
        uint32_t bad = (mask & ~APP_MASK) || core < -3 || core >= NCORE ? R_INVALID_CORE
                       : !mask || (keep >= 0 && !(mask >> keep & 1)) ? R_INVALID_COMBINATION : R_OK;
        if (t && bad == R_INVALID_COMBINATION && mask) {
            static int n;
            if (n++ < 4)
                rt_log("kernel: SetThreadCoreMask(core %d, mask %#llx): ideal core outside the mask, core %d taken", core,
                       (unsigned long long)mask, __builtin_ctzll(mask));
            core = __builtin_ctzll(mask), bad = R_OK;
        }
        if (t && bad) {
            x[0] = bad;
            unlock();
            return 1;
        }
        if (t) {
            if (core >= 0) t->core = core;   // -1: no ideal core; ours stays a hint, core_take checks the mask
            t->mask = mask;
            if (t == tcur && t->cur_core >= 0 && !core_allowed(t, t->cur_core)) {   // move off a forbidden core
                core_put(t);
                core_take(t);
            }
        }
        x[0] = t ? R_OK : R_BAD_HANDLE;
        unlock();
        return 1;
    }
    case 0x10: {   // GetCurrentProcessorNumber: the virtual core held
        RtThread *t = cur(imm);
        if (t->cur_core < 0) {   // MKW_CORE_NUM_GATE: a core of its own while it uses the number
            lock();
            core_take_num(t);
            unlock();
        }
        x[0] = (uint64_t)(uint32_t)(t->cur_core >= 0 ? t->cur_core : t->core);
        return 1;
    }
    case 0x36: {   // SynchronizePreemptionState: the SDK's spin locks call it after clearing TLR+0x100 (disable_count)
                   // when TLR+0x102 (interrupt_flag) is set; rt_svc's checkpoint has already honoured a pending preempt
        RtThread *t = cur(imm);
        volatile uint16_t *flag = (volatile uint16_t *)(t->tlr + 0x102);
        __atomic_fetch_add(&g_sync36, 1, __ATOMIC_RELAXED);
        if (*flag) __atomic_fetch_add(&g_sync36_flag, 1, __ATOMIC_RELAXED);
        *flag = 0;
        ap_sync(t);
        x[0] = R_OK;
        return 1;
    }
    case 0x25: {   // GetThreadId(w1 handle -> x1)
        lock();
        RtThread *t = h_get((uint32_t)x[1], O_THREAD);
        x[0] = t ? R_OK : R_BAD_HANDLE;
        if (t) x[1] = t->id;
        unlock();
        return 1;
    }

    case 0x1A: {   // ArbitrateLock(w0 owner handle, x1 address, w2 tag)
        RtThread *t = cur(imm);
        uint64_t addr = x[1];
        uint32_t r = R_OK;
        lock();
        if (ld32(addr) == ((uint32_t)x[0] | WAITERS_BIT)) {
            t->waddr = addr;
            t->wtag = (uint32_t)x[2];
            t->wobj = (uint32_t)x[0];
            r = block(t, W_LOCK, -1);
        }
        unlock();
        x[0] = r;
        return 1;
    }
    case 0x1B:   // ArbitrateUnlock(x0 address)
        lock();
        lock_handoff(x[0]);
        unlock();
        x[0] = R_OK;
        return 1;
    case 0x1C: {   // WaitProcessWideKeyAtomic(x0 lock, x1 key, w2 tag, x3 timeout)
        // The key is set before the lock is released. The SDK's Signal reads the key in user mode and skips svc 0x1D
        // when it is 0, and user mode takes the lock without G: releasing first lets another thread take the lock,
        // change the condition, read key 0 and wake nobody, after which this thread waits forever (MNG threads.c
        // 439-446). Linking into the wait list happens in block, still holding G, so svc 0x1D cannot come between.
        RtThread *t = cur(imm);
        uint64_t addr = x[0], key = x[1];
        lock();
        if (!kern_test_cv_old_order) {
            st32(key, 1);
            t->wkey = key;
            t->waddr = addr;
            t->wtag = (uint32_t)x[2];
            lock_handoff(addr);
            cv_hook();
        } else {   // the probe's negative control: MNG's pre-fix order
            lock_handoff(addr);
            cv_hook();
            st32(key, 1);
            t->wkey = key;
            t->waddr = addr;
            t->wtag = (uint32_t)x[2];
        }
        t->wobj = key;
        uint32_t r = block(t, W_CONDVAR, (int64_t)x[3]);   // on timeout user mode takes the lock itself
        unlock();
        x[0] = r;
        return 1;
    }
    case 0x1D: {   // SignalProcessWideKey(x0 key, w1 count; <= 0 all)
        uint64_t key = x[0];
        int cnt = (int)x[1], n = 0;
        lock();
        for (RtThread *w; (cnt <= 0 || n < cnt) && (w = pick(W_CONDVAR, key)); n++) lock_acquire_for(w, w->waddr, w->wtag);
        int left = 0;
        for (RtThread *w = g_wait; w && !left; w = w->next) left = w->wkind == W_CONDVAR && w->wkey == key;
        if (!left) st32(key, 0);
        unlock();
        x[0] = R_OK;
        return 1;
    }

    case 0x34: {   // WaitForAddress(x0 address, w1 type, x2 value, x3 timeout): 0 if less, 1 decrement if less, 2 if
                   // equal, 3 if the 64-bit word is equal (nn::os::WaitForAddress(long const*, long))
        RtThread *t = cur(imm);
        uint64_t addr = x[0];
        int type = (int)x[1];
        int32_t val = (int32_t)x[2];
        if (addr & (type == 3 ? 7 : 3)) {   // Atmosphère's kern_svc_address_arbiter.cpp checks this first
            x[0] = KERN_R_INVALID_ADDRESS;
            return 1;
        }
        if (type < 0 || type > 3) {
            x[0] = R_INVALID_ENUM;
            return 1;
        }
        AddrQueue q = addr_queue(addr);
        plat_mutex_lock(q.m);
        uint32_t r, v = ld32(addr);
        int ok = type == 3 ? ld64(addr) == x[2] : type == 2 ? (int32_t)v == val : (int32_t)v < val;
        if (ok && type == 1)
            while (!cas32(addr, &v, v - 1))
                if (!((int32_t)v < val)) {
                    ok = 0;
                    break;
                }
        if (!ok) r = R_BAD_STATE;
        else {
            t->waddr = addr;
            t->wobj = addr;
            r = addr_block(t, q, (int64_t)x[3]);
        }
        plat_mutex_unlock(q.m);
        x[0] = r;
        return 1;
    }
    case 0x35: {   // SignalToAddress(x0 address, w1 type, w2 value, w3 count): 0 wake, 1 increment if equal,
                   // 2 if equal modify by the number of waiters
        uint64_t addr = x[0];
        int type = (int)x[1], cnt = (int)x[3];
        int32_t val = (int32_t)x[2];
        if (type < 0 || type > 2) {
            x[0] = R_INVALID_ENUM;
            return 1;
        }
        AddrQueue q = addr_queue(addr);
        plat_mutex_lock(q.m);
        int nw = 0;
        for (RtThread *w = *q.list; w; w = w->next) nw += w->wkind == W_ADDRESS && w->waddr == addr;
        uint32_t r = R_OK;
        if (type != 0) {
            int32_t nv = val + 1;
            if (type == 2) {
                if (cnt <= 0) nv = nw > 0 ? val - 1 : val + 1;
                else nv = nw == 0 ? val + 1 : nw <= cnt ? val - 1 : val;
            }
            uint32_t e = (uint32_t)val;
            if (!cas32(addr, &e, (uint32_t)nv)) r = R_BAD_STATE;
        }
        if (!r)
            for (int n = 0; cnt <= 0 || n < cnt; n++) {
                RtThread *w = pick_in(*q.list, W_ADDRESS, addr);
                if (!w) break;
                wake_in(q.list, w, 0);
            }
        plat_mutex_unlock(q.m);
        x[0] = r;
        return 1;
    }

    case 0x45: {   // CreateEvent(-> w1 writable, w2 readable)
        Event *e = calloc(1, sizeof *e);
        if (!e) rt_fatal("kernel: event allocation");
        e->refs = 2;
        lock();
        uint32_t hw = h_new(O_EVENT_W, e), hr = h_new(O_EVENT_R, e);
        unlock();
        x[0] = R_OK;
        x[1] = hw;
        x[2] = hr;
        return 1;
    }
    case 0x11: case 0x12: case 0x17: {   // SignalEvent(writable), ClearEvent(either end), ResetSignal(readable)
        uint32_t h = (uint32_t)x[0];
        uint32_t r = R_OK;
        lock();
        Event *e = imm == 0x11 ? h_peek(h, O_EVENT_W) : imm == 0x17 ? h_peek(h, O_EVENT_R)
                 : h_peek(h, O_EVENT_W) ? h_peek(h, O_EVENT_W) : h_peek(h, O_EVENT_R);
        if (!e) {
            r = imm == 0x17 && h_peek(h, O_THREAD) ? R_BAD_STATE : R_BAD_HANDLE;
            if (r == R_BAD_HANDLE) rt_log("kernel: event svc %#x: invalid handle %#x", imm, h);
        } else if (imm == 0x11) {
            e->signaled = 1;
            wake_sync_waiters();
        } else if (imm == 0x17 && !e->signaled) {
            r = R_BAD_STATE;
        } else {
            e->signaled = 0;
        }
        unlock();
        x[0] = r;
        return 1;
    }
    case 0x18: {   // WaitSynchronization(-> w1 index; x1 handles, w2 count, x3 timeout)
        RtThread *t = cur(imm);
        const uint32_t *hs = (const uint32_t *)(uintptr_t)x[1];
        int n = (int)x[2];
        int64_t ns = (int64_t)x[3];
        if (n < 0 || n > 64 || (!hs && n)) {
            x[0] = R_BAD_STATE;
            return 1;
        }
        uint64_t start = kern_now_ns();
        uint32_t r;
        int idx = -1;
        lock();
        g_sync_calls++;
        for (;;) {
            if (t->cancel) {
                t->cancel = 0;
                r = R_CANCELLED;
                break;
            }
            int bad = 0;
            for (int i = 0; i < n && idx < 0; i++)
                if (sig_obj(hs[i], &bad)) idx = i;
            if (bad) {
                r = R_BAD_HANDLE;
                break;
            }
            if (idx >= 0) {
                r = R_OK;
                break;
            }
            int64_t left = ns;
            if (ns > 0) {
                uint64_t spent = kern_now_ns() - start;
                if (spent >= (uint64_t)ns) {
                    r = R_TIMEOUT;
                    break;
                }
                left = ns - (int64_t)spent;
            }
            t->wobj = n ? hs[0] | (uint64_t)n << 32 : 0;
            g_sync_blocks++;
            if (block(t, W_SYNC, left) == R_TIMEOUT) {
                r = R_TIMEOUT;
                break;
            }
        }
        unlock();
        x[0] = r;
        x[1] = idx >= 0 ? (uint64_t)idx : 0;
        return 1;
    }
    case 0x19: {   // CancelSynchronization(w0 thread)
        lock();
        RtThread *t = h_get((uint32_t)x[0], O_THREAD);
        if (t) {
            t->cancel = 1;
            if (t->wkind == W_SYNC) wake(t, (uint32_t)-1);
        }
        unlock();
        x[0] = t ? R_OK : R_BAD_HANDLE;
        return 1;
    }
    case 0x16:   // CloseHandle(w0)
        x[0] = handle_close((uint32_t)x[0]);
        return 1;
    }
    return 0;
}
