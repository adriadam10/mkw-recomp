// Part of the kernel (kernel.c; split from it 06/11/2026, same sources and licences as its header).
// Asynchronous preemption of virtual cores (MKW_ASYNC_PREEMPT), priority inheritance and preemption disable.
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "kernel_int.h"
#include "platform/platform.h"

// ---- asynchronous preemption (MKW_ASYNC_PREEMPT, on unless 0)
// Adapted from MacNativeGames games/totk/runtime/threads.c @ 33e8fa8 (8bf0893, PR #488; MIT, Copyright (c) 2026
// MacNativeGames contributors): Horizon gives a core to a strictly better thread the moment that thread is ready.
// Here a better waiter only marks the owner, which gives the core up at its next SVC or indirect call; an owner in a
// stretch of game code with neither keeps it (MKW_CORE_DIAG: 0.5-1.1 ms of thread-time per frame, waits to 16 ms and
// more). With it on, three parts, all of which MacNativeGames found necessary:
// - The waiter interrupts the owner (plat_intr_stop) when it has waited AP_FIRST_NS, then every
//   millisecond: if the owner's host rip is in the lifted modules' code (g_gen_lo..g_gen_hi) and its host depth is 0
//   (not in a host call or SVC, nor in game code a host call made), it holds no host lock, so its rip is moved to
//   kern_ap_stub, which saves every volatile register, gives the core up like a checkpoint (kern_ap_yield) and jumps
//   back. Otherwise it is only marked, as before.
// - Priority inheritance (eff_prio): a thread owning a guest mutex counts at the priority of the best thread blocked on
//   it, for granting, kicking, preempting and yielding. Without it MacNativeGames' game stopped at its first
//   "Continue" (a better thread took the core of the low-priority owner of a mutex it then waited for).
// - Horizon's preemption disable: the SDK's spin locks (1.2.1 sdk f_70051242a8, f_7005124360 and siblings) count
//   TLR+0x100 (disable_count) while they try or hold the lock, and after clearing it call SVC 0x36 when TLR+0x102
//   (interrupt_flag) is set. Such an owner is not preempted, at a checkpoint either: interrupt_flag is set and the
//   switch happens at that SVC. Preempting it anyway hung MacNativeGames' game at "Continue" (the lock holder lost its
//   core to a thread that then spun on that lock).
// MKW_ASYNC_PREEMPT=0 keeps the old scheduling exactly: raw priorities, no interruption, no disable_count; =2 keeps
// inheritance and the preemption disable without interrupting owners.
#define AP_GAP_NS 1000000ull    // and interrupts the same owner at most this often
int g_ap = -1, g_ap_int;   // g_ap_int: interrupt owners (MKW_ASYNC_PREEMPT=2: inheritance and disable only)
static int g_ap_log;              // redirects logged so far (AP_LOG_N)
#define AP_LOG_N 40
static uintptr_t g_gen_lo, g_gen_hi;   // host code of the lifted modules: first and last function start
uint64_t g_ap_sent, g_ap_taken, g_ap_host, g_ap_pc, g_ap_deferred, g_ap_fail, g_ap_spin_ok;
void kern_ap_stub(void);

void ap_init(void) {
    if (g_ap >= 0) return;
    const char *e = cfg_value("MKW_ASYNC_PREEMPT");
    g_ap = strcmp(e, "0") != 0;
    g_ap_int = g_ap && strcmp(e, "2") != 0;
    if (g_ap) {
        RtModule m[16];
        int n = rt_linked_modules(m, 16);
        uintptr_t lo = UINTPTR_MAX, hi = 0;
        for (int i = 0; i < n; i++)
            for (uint64_t k = 0; k < m[i].count; k++) {
                uintptr_t f = (uintptr_t)m[i].fns[k];
                if (f < lo) lo = f;
                if (f > hi) hi = f;
            }
        const uintptr_t host[] = {(uintptr_t)kern_ap_stub, (uintptr_t)kern_checkpoint, (uintptr_t)rt_svc,
                                  (uintptr_t)rt_dispatch, (uintptr_t)rt_call};
        int bad = !n || lo >= hi;
        for (unsigned i = 0; i < sizeof host / sizeof *host; i++) bad |= host[i] >= lo && host[i] <= hi;
        if (bad) {
            rt_log("kernel: asynchronous preemption off: the lifted code range %#llx-%#llx holds host code",
                   (unsigned long long)lo, (unsigned long long)hi);
            g_ap = 0;
        } else {
            g_gen_lo = lo;
            g_gen_hi = hi;
            kern_host_track = 1;
        }
    }
    rt_log("kernel: asynchronous preemption of virtual cores %s, priority inheritance and preemption disable %s; lifted "
           "code exe+%#llx-exe+%#llx", g_ap_int ? "on" : "off", g_ap ? "on (MKW_ASYNC_PREEMPT=0 turns all off)" : "off",
           (unsigned long long)(g_gen_lo ? g_gen_lo - plat_exe_base() : 0),
           (unsigned long long)(g_gen_hi ? g_gen_hi - plat_exe_base() : 0));
}

// Horizon's priority inheritance: o counts at the priority of the best thread blocked on a guest mutex it owns (the
// owner is read from the mutex word, so it follows hand-offs). Holding G.
static int eff_prio(const RtThread *o) {
    int p = o->prio;
    for (const RtThread *w = g_wait; w; w = w->next)
        if (w->wkind == W_LOCK && w->prio < p && w->waddr &&
            (__atomic_load_n((const uint32_t *)(uintptr_t)w->waddr, __ATOMIC_RELAXED) & ~WAITERS_BIT) == o->handle)
            p = w->prio;
    return p;
}

// The priority the virtual cores go by: raw with MKW_ASYNC_PREEMPT=0, as before.
int prio_of(const RtThread *t) { return g_ap > 0 ? eff_prio(t) : t->prio; }

// TLR+0x100 disable_count, +0x102 interrupt_flag (any thread may read them; the flag is set from outside as the
// kernel does).
volatile uint16_t *ap_tlr(const RtThread *t) { return (volatile uint16_t *)(t->tlr + 0x100); }
int ap_disabled(const RtThread *t) { return g_ap > 0 && ap_tlr(t)[0]; }

// kern_ap_stub: entered in place of the interrupted instruction of top-level game code. It keeps every register the
// game code may have live (volatile GPRs, flags, YMM0-15, MXCSR; the C calls preserve the rest), gives the core up
// in kern_ap_yield and goes back to kern_ap_rip(), whose value it first stores in the return slot it reserved at
// entry, so a stack walk sees the interrupted function as its caller. It goes back with a ret, which reads the slot
// and releases it in one instruction. The jump it used before read the slot after releasing it: a signal or an APC
// landing between the two (Wine suspends a thread with a signal on its own stack, Linux interrupts with one) wrote its
// frame over the slot, and the jump went to garbage (wdist1, 06/11/2026: execute at 0x3A5F9E, one run in two). The
// ret costs one mispredicted return per preemption. The waiter never writes the owner's stack.
#ifdef _WIN32
__asm__(".text\n"
        ".globl kern_ap_stub\n"
        ".def kern_ap_stub; .scl 2; .type 32; .endef\n"
        ".p2align 4\n"
        "kern_ap_stub:\n"
        ".seh_proc kern_ap_stub\n"
        "    leaq -8(%rsp), %rsp\n"   // not sub: the flags are still the interrupted code's until pushfq
        ".seh_stackalloc 8\n"
        "    pushfq\n"
        ".seh_stackalloc 8\n"
        "    pushq %rax\n"
        ".seh_pushreg %rax\n"
        "    pushq %rcx\n"
        ".seh_pushreg %rcx\n"
        "    pushq %rdx\n"
        ".seh_pushreg %rdx\n"
        "    pushq %r8\n"
        ".seh_pushreg %r8\n"
        "    pushq %r9\n"
        ".seh_pushreg %r9\n"
        "    pushq %r10\n"
        ".seh_pushreg %r10\n"
        "    pushq %r11\n"
        ".seh_pushreg %r11\n"
        "    pushq %rbp\n"
        ".seh_pushreg %rbp\n"
        "    movq %rsp, %rbp\n"
        ".seh_setframe %rbp, 0\n"
        ".seh_endprologue\n"
        "    andq $-32, %rsp\n"
        "    subq $576, %rsp\n"   // 0: shadow space, 32: MXCSR saved, 36: MXCSR default, 64: YMM0-15
        "    vmovdqa %ymm0, 64(%rsp)\n"
        "    vmovdqa %ymm1, 96(%rsp)\n"
        "    vmovdqa %ymm2, 128(%rsp)\n"
        "    vmovdqa %ymm3, 160(%rsp)\n"
        "    vmovdqa %ymm4, 192(%rsp)\n"
        "    vmovdqa %ymm5, 224(%rsp)\n"
        "    vmovdqa %ymm6, 256(%rsp)\n"
        "    vmovdqa %ymm7, 288(%rsp)\n"
        "    vmovdqa %ymm8, 320(%rsp)\n"
        "    vmovdqa %ymm9, 352(%rsp)\n"
        "    vmovdqa %ymm10, 384(%rsp)\n"
        "    vmovdqa %ymm11, 416(%rsp)\n"
        "    vmovdqa %ymm12, 448(%rsp)\n"
        "    vmovdqa %ymm13, 480(%rsp)\n"
        "    vmovdqa %ymm14, 512(%rsp)\n"
        "    vmovdqa %ymm15, 544(%rsp)\n"
        "    stmxcsr 32(%rsp)\n"
        "    movl $0x1f80, 36(%rsp)\n"
        "    ldmxcsr 36(%rsp)\n"
        "    cld\n"
        "    vzeroupper\n"
        "    call kern_ap_rip\n"
        "    movq %rax, 72(%rbp)\n"   // the return slot: rbp + 8 pushes + pushfq
        "    call kern_ap_yield\n"
        "    ldmxcsr 32(%rsp)\n"
        "    vmovdqa 64(%rsp), %ymm0\n"
        "    vmovdqa 96(%rsp), %ymm1\n"
        "    vmovdqa 128(%rsp), %ymm2\n"
        "    vmovdqa 160(%rsp), %ymm3\n"
        "    vmovdqa 192(%rsp), %ymm4\n"
        "    vmovdqa 224(%rsp), %ymm5\n"
        "    vmovdqa 256(%rsp), %ymm6\n"
        "    vmovdqa 288(%rsp), %ymm7\n"
        "    vmovdqa 320(%rsp), %ymm8\n"
        "    vmovdqa 352(%rsp), %ymm9\n"
        "    vmovdqa 384(%rsp), %ymm10\n"
        "    vmovdqa 416(%rsp), %ymm11\n"
        "    vmovdqa 448(%rsp), %ymm12\n"
        "    vmovdqa 480(%rsp), %ymm13\n"
        "    vmovdqa 512(%rsp), %ymm14\n"
        "    vmovdqa 544(%rsp), %ymm15\n"
        "    movq %rbp, %rsp\n"
        "    popq %rbp\n"
        "    popq %r11\n"
        "    popq %r10\n"
        "    popq %r9\n"
        "    popq %r8\n"
        "    popq %rdx\n"
        "    popq %rcx\n"
        "    popq %rax\n"
        "    popfq\n"
        "    ret\n"   // the slot, reserved at entry, is at rsp
        ".seh_endproc\n");
#elif defined(__aarch64__)
// The AArch64 version. AAPCS64 on Linux has no red zone: the frame goes right below the interrupted sp. It saves x0-x17,
// x29, x30, q0-q31 (the callee keeps only the low halves of v8-v15), NZCV, FPCR and FPSR, runs host code with the
// default FPCR, and links a frame record {x29, interrupted pc} so a stack walk sees the interrupted function as its
// caller. There is no return through memory on AArch64: the way back is br x18, the one register the lifted code may
// not hold (it is built with -ffixed-x18, tools/build/build.py GEN_FLAGS; x18 is a caller-saved temporary for
// everything else, so no caller of game code keeps a value in it). Frame (720 bytes): 0 x0-x17, 144 x29 x30, 160 NZCV
// FPCR, 176 FPSR, 192 q0-q31, 704 frame record {x29, pc}.
__asm__(".text\n"
        ".globl kern_ap_stub\n"
        ".type kern_ap_stub, %function\n"
        ".p2align 4\n"
        "kern_ap_stub:\n"
        "    .cfi_startproc\n"
        "    sub sp, sp, #720\n"
        "    stp x0, x1, [sp, #0]\n"
        "    stp x2, x3, [sp, #16]\n"
        "    stp x4, x5, [sp, #32]\n"
        "    stp x6, x7, [sp, #48]\n"
        "    stp x8, x9, [sp, #64]\n"
        "    stp x10, x11, [sp, #80]\n"
        "    stp x12, x13, [sp, #96]\n"
        "    stp x14, x15, [sp, #112]\n"
        "    stp x16, x17, [sp, #128]\n"
        "    stp x29, x30, [sp, #144]\n"
        "    mrs x0, nzcv\n"
        "    mrs x1, fpcr\n"
        "    mrs x2, fpsr\n"
        "    stp x0, x1, [sp, #160]\n"
        "    str x2, [sp, #176]\n"
        "    stp q0, q1, [sp, #192]\n"
        "    stp q2, q3, [sp, #224]\n"
        "    stp q4, q5, [sp, #256]\n"
        "    stp q6, q7, [sp, #288]\n"
        "    stp q8, q9, [sp, #320]\n"
        "    stp q10, q11, [sp, #352]\n"
        "    stp q12, q13, [sp, #384]\n"
        "    stp q14, q15, [sp, #416]\n"
        "    stp q16, q17, [sp, #448]\n"
        "    stp q18, q19, [sp, #480]\n"
        "    stp q20, q21, [sp, #512]\n"
        "    stp q22, q23, [sp, #544]\n"
        "    stp q24, q25, [sp, #576]\n"
        "    stp q26, q27, [sp, #608]\n"
        "    stp q28, q29, [sp, #640]\n"
        "    stp q30, q31, [sp, #672]\n"
        "    msr fpcr, xzr\n"
        "    bl kern_ap_rip\n"
        "    add x9, sp, #704\n"          // (stp of X registers reaches only 504)
        "    stp x29, x0, [x9]\n"         // the frame record: caller's x29, the interrupted pc
        "    mov x29, x9\n"
        "    bl kern_ap_yield\n"
        "    ldp x0, x1, [sp, #160]\n"
        "    ldr x2, [sp, #176]\n"
        "    msr nzcv, x0\n"
        "    msr fpcr, x1\n"
        "    msr fpsr, x2\n"
        "    ldp q0, q1, [sp, #192]\n"
        "    ldp q2, q3, [sp, #224]\n"
        "    ldp q4, q5, [sp, #256]\n"
        "    ldp q6, q7, [sp, #288]\n"
        "    ldp q8, q9, [sp, #320]\n"
        "    ldp q10, q11, [sp, #352]\n"
        "    ldp q12, q13, [sp, #384]\n"
        "    ldp q14, q15, [sp, #416]\n"
        "    ldp q16, q17, [sp, #448]\n"
        "    ldp q18, q19, [sp, #480]\n"
        "    ldp q20, q21, [sp, #512]\n"
        "    ldp q22, q23, [sp, #544]\n"
        "    ldp q24, q25, [sp, #576]\n"
        "    ldp q26, q27, [sp, #608]\n"
        "    ldp q28, q29, [sp, #640]\n"
        "    ldp q30, q31, [sp, #672]\n"
        "    ldp x0, x1, [sp, #0]\n"
        "    ldp x2, x3, [sp, #16]\n"
        "    ldp x4, x5, [sp, #32]\n"
        "    ldp x6, x7, [sp, #48]\n"
        "    ldp x8, x9, [sp, #64]\n"
        "    ldp x10, x11, [sp, #80]\n"
        "    ldp x12, x13, [sp, #96]\n"
        "    ldp x14, x15, [sp, #112]\n"
        "    ldp x16, x17, [sp, #128]\n"
        "    ldp x29, x30, [sp, #144]\n"
        "    ldr x18, [sp, #712]\n"   // the interrupted pc
        "    add sp, sp, #720\n"
        "    br x18\n"
        "    .cfi_endproc\n"
        ".size kern_ap_stub, .-kern_ap_stub\n");
#else
// The System V version: rsi and rdi are volatile there too, and the interrupted code may keep data in the 128 bytes
// below its stack pointer (the red zone), so the return slot and the saves go below that. Without a red zone in the
// way the slot is at entry - 136; the exit's ret $128 takes the slot and leaves rsp at the entry value.
__asm__(".text\n"
        ".globl kern_ap_stub\n"
        ".type kern_ap_stub, @function\n"
        ".p2align 4\n"
        "kern_ap_stub:\n"
        "    .cfi_startproc\n"
        "    leaq -136(%rsp), %rsp\n"   // not sub: the flags are still the interrupted code's until pushfq
        "    pushfq\n"
        "    pushq %rax\n"
        "    pushq %rcx\n"
        "    pushq %rdx\n"
        "    pushq %rsi\n"
        "    pushq %rdi\n"
        "    pushq %r8\n"
        "    pushq %r9\n"
        "    pushq %r10\n"
        "    pushq %r11\n"
        "    pushq %rbp\n"
        "    movq %rsp, %rbp\n"
        "    andq $-32, %rsp\n"
        "    subq $576, %rsp\n"   // 32: MXCSR saved, 36: MXCSR default, 64: YMM0-15
        "    vmovdqa %ymm0, 64(%rsp)\n"
        "    vmovdqa %ymm1, 96(%rsp)\n"
        "    vmovdqa %ymm2, 128(%rsp)\n"
        "    vmovdqa %ymm3, 160(%rsp)\n"
        "    vmovdqa %ymm4, 192(%rsp)\n"
        "    vmovdqa %ymm5, 224(%rsp)\n"
        "    vmovdqa %ymm6, 256(%rsp)\n"
        "    vmovdqa %ymm7, 288(%rsp)\n"
        "    vmovdqa %ymm8, 320(%rsp)\n"
        "    vmovdqa %ymm9, 352(%rsp)\n"
        "    vmovdqa %ymm10, 384(%rsp)\n"
        "    vmovdqa %ymm11, 416(%rsp)\n"
        "    vmovdqa %ymm12, 448(%rsp)\n"
        "    vmovdqa %ymm13, 480(%rsp)\n"
        "    vmovdqa %ymm14, 512(%rsp)\n"
        "    vmovdqa %ymm15, 544(%rsp)\n"
        "    stmxcsr 32(%rsp)\n"
        "    movl $0x1f80, 36(%rsp)\n"
        "    ldmxcsr 36(%rsp)\n"
        "    cld\n"
        "    vzeroupper\n"
        "    call kern_ap_rip\n"
        "    movq %rax, 88(%rbp)\n"   // the return slot: rbp + 10 pushes + pushfq
        "    call kern_ap_yield\n"
        "    ldmxcsr 32(%rsp)\n"
        "    vmovdqa 64(%rsp), %ymm0\n"
        "    vmovdqa 96(%rsp), %ymm1\n"
        "    vmovdqa 128(%rsp), %ymm2\n"
        "    vmovdqa 160(%rsp), %ymm3\n"
        "    vmovdqa 192(%rsp), %ymm4\n"
        "    vmovdqa 224(%rsp), %ymm5\n"
        "    vmovdqa 256(%rsp), %ymm6\n"
        "    vmovdqa 288(%rsp), %ymm7\n"
        "    vmovdqa 320(%rsp), %ymm8\n"
        "    vmovdqa 352(%rsp), %ymm9\n"
        "    vmovdqa 384(%rsp), %ymm10\n"
        "    vmovdqa 416(%rsp), %ymm11\n"
        "    vmovdqa 448(%rsp), %ymm12\n"
        "    vmovdqa 480(%rsp), %ymm13\n"
        "    vmovdqa 512(%rsp), %ymm14\n"
        "    vmovdqa 544(%rsp), %ymm15\n"
        "    movq %rbp, %rsp\n"
        "    popq %rbp\n"
        "    popq %r11\n"
        "    popq %r10\n"
        "    popq %r9\n"
        "    popq %r8\n"
        "    popq %rdi\n"
        "    popq %rsi\n"
        "    popq %rdx\n"
        "    popq %rcx\n"
        "    popq %rax\n"
        "    popfq\n"
        "    ret $128\n"   // pops the slot at entry - 136, then the 128 bytes of red zone above it: rsp = entry
        "    .cfi_endproc\n"
        ".size kern_ap_stub, .-kern_ap_stub\n");
#endif


// The waiter w interrupts its target owner if that runs top-level game code (holding G; the owner is stopped only
// between plat_intr_stop and plat_intr_resume, during which nothing here takes a lock).
int ap_try(RtThread *w) {
    RtThread *o = w->target;
    if (!o || o->cur_core < 0 || !o->preempt) return AP_NONE;
    if (!o->hthr || o->ap_pending) return AP_RETRY;
    volatile uint16_t *tl = ap_tlr(o);
    if (__atomic_load_n(&o->host_depth, __ATOMIC_RELAXED) > 0) {
        g_ap_host++;
        return AP_RETRY;
    }
    if (tl[0]) {   // preemption disabled: SVC 0x36 switches
        tl[1] = 1;
        g_ap_deferred++;
        return AP_RETRY;
    }
    uint64_t now = kern_now_ns();
    if (o->ap_last && now - o->ap_last < AP_GAP_NS) return AP_RETRY;
    o->ap_last = now;
    uint64_t rip, rsp;
    if (!plat_intr_stop(o->hthr, &rip, &rsp)) {
        g_ap_fail++;
        return AP_RETRY;
    }
    int r = AP_RETRY;
    uint64_t to = 0;
    if (rip < g_gen_lo || rip > g_gen_hi)
        g_ap_pc++;
    else if (__atomic_load_n(&o->host_depth, __ATOMIC_RELAXED) > 0)
        g_ap_host++;
    else if (tl[0]) {
        tl[1] = 1;
        g_ap_deferred++;
    } else {
        o->ap_rip = rip;
        o->ap_pending = 1;
        to = (uint64_t)(uintptr_t)kern_ap_stub;
    }
    if (!plat_intr_resume(o->hthr, to)) {
        o->ap_pending = 0;
        g_ap_fail++;
    } else if (to) {
        g_ap_sent++;
        r = AP_SENT;
    }
    if (r == AP_SENT && g_ap_log < AP_LOG_N) {
        g_ap_log++;
        rt_log("kernel: interrupt %#x p%d at exe+%#llx rsp %#llx for %#x p%d", o->handle, o->prio,
               (unsigned long long)(o->ap_rip - plat_exe_base()), (unsigned long long)rsp, w->handle, w->prio);
    }
    return r;
}
