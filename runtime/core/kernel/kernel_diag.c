// Part of the kernel (kernel.c; split from it 06/11/2026, same sources and licences as its header).
// MKW_CRIT, the MKW_TRACE=sched accounting and report, and MKW_CORE_DIAG: diagnostics of the scheduler.
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "kernel_int.h"
#include "prof.h"
#include "platform/platform.h"


// ---- MKW_CRIT=<first>-<last frame> with MKW_TRACE=sched: every state change and wake of the guest threads and
// every present over those frames, written to MKW_CRIT_OUT (default crit.bin) at the last one, for
// tools/crit_path.py, which walks the chain of waits back from each present (holding G). MKW_CRIT=w<lo>-<hi> takes
// world-clock seconds instead (frame_step_world_us): from the first present at or after lo to the first after hi, so a
#define CRIT_N (1u << 22)                // 128 MiB, about 2000 fight frames
static CritEv *g_crit;
static uint32_t g_crit_n;
int g_crit_on;
static uint64_t g_crit_lo, g_crit_hi;   // frames; UINT64_MAX in world mode until the world clock fixes them
static int64_t g_crit_wlo, g_crit_whi;  // world mode: the window in microseconds of world clock, else 0

// Under G or an address queue's lock (kernel.c addr_queue): the slot is claimed atomically.
void crit_add(int kind, uint32_t a, uint32_t b, int st, uint64_t obj, uint64_t ts) {
    uint32_t i = __atomic_fetch_add(&g_crit_n, 1, __ATOMIC_RELAXED);
    if (i < CRIT_N) g_crit[i] = (CritEv){ts, obj, a, b, (uint16_t)kind, (uint16_t)st, 0};
    else __atomic_store_n(&g_crit_n, CRIT_N, __ATOMIC_RELAXED);
}

// ---- MKW_TRACE=sched accounting (holding G)
#define RW_N 32
static RtThread *g_render;                               // the thread that presents (sched trace, core diag)
typedef struct { int st; uint32_t waker; uint64_t obj, ns; } WaitRec;
static WaitRec g_rw[RW_N], g_mw[RW_N];                   // the render and main thread's waits by object and waker
uint64_t g_pre_req, g_pre_n, g_pre_ns, g_pre_max, g_pre_stale;  // preempt requests; releases after one and their latency
uint64_t g_sync_calls, g_sync_blocks;              // WaitSynchronization calls and the blocks inside them

static void wait_add(WaitRec *w, int st, uint64_t obj, uint32_t waker, uint64_t ns) {
    for (int i = 0; i < RW_N; i++)
        if (!w[i].ns || (w[i].st == st && w[i].obj == obj && w[i].waker == waker)) {
            w[i].st = st;
            w[i].obj = obj;
            w[i].waker = waker;
            w[i].ns += ns;
            return;
        }
}

// Close the current state's interval and enter st.
void acct(RtThread *t, int st) {
    if (!(rt_trace & RT_TRACE_SCHED)) return;
    uint64_t now = kern_now_ns();
    if (t->st_since) {
        t->st_ns[t->st] += now - t->st_since;
        if (t->st >= S_HOST && (t == g_render || t->is_main)) {
            uint32_t waker = t->st == S_HOST || t->st == S_SLEEP ? 0 : t->waker;
            wait_add(t == g_render ? g_rw : g_mw, t->st, t->wobj, waker, now - t->st_since);
        }
    }
    if (g_crit_on) crit_add(CRIT_STATE, t->handle, t->waker, st, t->wobj, now);
    t->st = st;
    t->st_since = now;
}

// ---- MKW_CORE_DIAG=1: why ready threads wait for a virtual core, and for how long
// Each core wait is split into classes: the owner asked to give the core up ran game code (DG_GAME, the part an
// asynchronous preemption would remove), was in a host call or SVC that kept the core (DG_HOST), every owner the waiter
// may displace was of better or equal priority (DG_EQ: Horizon waits too), or the worse owners were claimed by better
// waiters (DG_NOCORE). Per 60 presents: time per frame and a wait-length histogram per class, the owners that cost
// most; a line per present over 25 ms; a line per wait with over 2 ms of DG_GAME (where the owner reached its
// checkpoint), and one host RIP sample of the owner per such wait (exe RVA, for mkw.map).
static const char *const dg_name[DG_N] = {"game", "host", "eq", "nocore"};
#define DG_NB 8   // wait lengths: < 0.05, 0.25, 1, 2, 4, 8, 16 ms, longer
static const uint64_t dg_edge[DG_NB - 1] = {50000, 250000, 1000000, 2000000, 4000000, 8000000, 16000000};
#define DG_LONG_LINES 400
#define DG_SAMPLES 600
#define DG_OWN 32
int kern_host_track;
int g_diag = -1;
typedef struct {
    uint64_t ns[DG_N], n[DG_N], hist[DG_N][DG_NB];                    // this window
    struct { uint32_t h; int prio, cls; uint64_t ns; } own[DG_OWN];   // this window: owners by class
    uint64_t long_frames, long_game_frames;
} DgWindow;
static DgWindow g_dgw;
static uint64_t g_dgf[DG_N], g_dgfr[DG_N];   // this frame: all threads, the render thread
uint64_t g_dg_samples, g_dg_lines, g_sync36, g_sync36_flag;

void diag_init(void) {
    if (g_diag >= 0) return;
    const char *e = cfg_get("MKW_CORE_DIAG");
    g_diag = e && *e && strcmp(e, "0");
    if (g_diag) {
        kern_host_track = 1;
        rt_log("core-diag: on (MKW_CORE_DIAG)");
    }
}

// A tracked host call or SVC returned to depth 0 with a preempt request pending (host.h); on the thread itself.
void kern_host_left(void *thread) {
    RtThread *t = thread;
    if (g_diag > 0 && t->dg_kick && t->dg_host_kick && !t->dg_host_end) t->dg_host_end = kern_now_ns();
}

int dg_in_host(const RtThread *o) { return __atomic_load_n(&o->host_depth, __ATOMIC_RELAXED) > 0; }

static void dg_add(const RtThread *w, int cls, uint64_t ns, uint32_t oh, int oprio) {
    if (!ns) return;
    g_dgw.ns[cls] += ns;
    g_dgf[cls] += ns;
    if (w == g_render) g_dgfr[cls] += ns;
    if (!oh) return;
    int i = 0;
    for (; i < DG_OWN && g_dgw.own[i].ns; i++)
        if (g_dgw.own[i].h == oh && g_dgw.own[i].cls == cls) break;
    if (i == DG_OWN) return;
    g_dgw.own[i].h = oh;
    g_dgw.own[i].prio = oprio;
    g_dgw.own[i].cls = cls;
    g_dgw.own[i].ns += ns;
}

// The waiter's class at the start of its wait, after core_kick claimed (or did not claim) an owner for it.
int dg_class0(const RtThread *t) {
    if (t->target) return dg_in_host(t->target) ? DG_HOST : DG_GAME;
    for (int i = 0; i < NCORE; i++)
        if (core_allowed(t, i) && g_core_owner[i] && g_core_owner[i]->prio > t->prio) return DG_NOCORE;
    return DG_EQ;
}

// The waiter got a core: split [dg_since, now] by what the owner that released it did after its preempt request.
void dg_done(RtThread *t) {
    uint64_t now = kern_now_ns(), t0 = t->dg_since, w = now - t0;
    t->dg_since = 0;
    if (!t0) return;
    const struct DgRel *r = &t->dg_rel;
    uint64_t part[DG_N] = {0};
    uint32_t oh = t->target ? t->target->handle : r->h;
    int oprio = t->target ? t->target->prio : r->prio;
    if (r->pre && r->kick) {
        uint64_t k = r->kick > t0 ? (r->kick < now ? r->kick : now) : t0;
        part[t->dg_cls] += k - t0;
        if (r->host) {
            uint64_t he = r->host_end > k && r->host_end < now ? r->host_end : now;
            part[DG_HOST] += he - k;
            part[DG_GAME] += now - he;
        } else
            part[DG_GAME] += now - k;
        oh = r->h;
        oprio = r->prio;
    } else
        part[t->dg_cls] += w;
    int top = 0;
    for (int c = 0; c < DG_N; c++) {
        dg_add(t, c, part[c], oh, oprio);
        if (part[c] > part[top]) top = c;
    }
    int b = 0;
    while (b < DG_NB - 1 && w >= dg_edge[b]) b++;
    g_dgw.n[top]++;
    g_dgw.hist[top][b]++;
    if ((part[DG_GAME] >= 2000000 || part[DG_HOST] >= 2000000) && g_dg_lines < DG_LONG_LINES) {
        g_dg_lines++;
        char a[128], l[128];
        rt_log("core-diag: wait %.2f ms (game %.2f host %.2f): %#x p%d mask %llx; released by %#x p%d%s at %s lr %s",
               w / 1e6, part[DG_GAME] / 1e6, part[DG_HOST] / 1e6, t->handle, t->prio, (unsigned long long)t->mask, r->h,
               r->prio, r->pre ? " (preempted)" : "", r->pc ? ldr_describe(r->pc, a, sizeof a) : "-",
               r->lr ? ldr_describe(r->lr, l, sizeof l) : "-");
    }
    t->target = NULL;
    memset(&t->dg_rel, 0, sizeof t->dg_rel);
}

// Once per long DG_GAME or DG_HOST wait: where the owner's host thread runs (module and offset), its latest SVC and
// its guest lr. Holding G; the owner is resumed before anything that could take a lock it holds.
void dg_sample(RtThread *t) {
    RtThread *o = t->target;
    if (!o || o->cur_core < 0 || !o->hthr || g_dg_samples >= DG_SAMPLES) return;
    uint64_t rip, rsp;
    if (!plat_intr_stop(o->hthr, &rip, &rsp)) return;
    int depth = __atomic_load_n(&o->host_depth, __ATOMIC_RELAXED);
    plat_intr_resume(o->hthr, 0);
    g_dg_samples++;
    char mn[256] = "?";
    uintptr_t m = 0;
    plat_module_of((uintptr_t)rip, mn, sizeof mn, &m);
    char lr[128];
    rt_log("core-diag: sample owner %#x p%d depth %d rip %s+%#llx, last svc %#x, guest lr %s (waiter %#x p%d, %s, "
           "%.2f ms)", o->handle, o->prio, depth, mn, (unsigned long long)(rip - m), o->dg_svc,
           o->cur ? ldr_describe(o->cur->x[30], lr, sizeof lr) : "-", t->handle, t->prio,
           t->dg_cls == DG_HOST ? "host" : "game", (kern_now_ns() - t->dg_since) / 1e6);
}


// perf_present, every present: a line for a present over 25 ms, the 60-present summary.
void kern_core_diag_frame(uint64_t frame, uint64_t interval_ns) {
    if (g_ap > 0 && frame && frame % 600 == 0) {   // MKW_ASYNC_PREEMPT counters since the start
        lock();
        uint64_t v[8] = {g_ap_sent, g_ap_taken, g_ap_host, g_ap_pc, g_ap_deferred, g_ap_fail, g_ap_spin_ok, g_sync36};
        unlock();
        rt_log("kernel: frame %llu: asynchronous preemption: %llu interrupted, %llu cores given up there; refused: "
               "%llu in a host call, %llu outside game code, %llu preemption disabled, %llu failed; %llu waits ended "
               "in the first spin; SVC 0x36 %llu",
               (unsigned long long)frame, (unsigned long long)v[0], (unsigned long long)v[1], (unsigned long long)v[2],
               (unsigned long long)v[3], (unsigned long long)v[4], (unsigned long long)v[5], (unsigned long long)v[6],
               (unsigned long long)v[7]);
    }
    if (g_diag <= 0) return;
    DgWindow w;
    uint64_t f[DG_N], fr[DG_N], s36, s36f;
    lock();
    g_render = tcur;
    memcpy(f, g_dgf, sizeof f);
    memcpy(fr, g_dgfr, sizeof fr);
    memset(g_dgf, 0, sizeof g_dgf);
    memset(g_dgfr, 0, sizeof g_dgfr);
    int lng = interval_ns > 25000000ull;
    if (lng) {
        g_dgw.long_frames++;
        if (f[DG_GAME] > 1000000) g_dgw.long_game_frames++;
    }
    int rep = frame && frame % 60 == 0;
    if (rep) {
        w = g_dgw;
        memset(&g_dgw, 0, sizeof g_dgw);
    }
    s36 = g_sync36;
    s36f = g_sync36_flag;
    unlock();
    if (lng)
        rt_log("core-diag: frame %llu: %.1f ms; core wait game %.2f host %.2f eq %.2f nocore %.2f ms; render thread game "
               "%.2f host %.2f eq %.2f nocore %.2f ms",
               (unsigned long long)frame, interval_ns / 1e6, f[0] / 1e6, f[1] / 1e6, f[2] / 1e6, f[3] / 1e6,
               fr[0] / 1e6, fr[1] / 1e6, fr[2] / 1e6, fr[3] / 1e6);
    if (!rep) return;
    char s[1024];
    int o = 0;
    for (int c = 0; c < DG_N; c++) {
        o += snprintf(s + o, sizeof s - o, "%s %.3f ms/frame %llu waits [", dg_name[c], w.ns[c] / 60e6,
                      (unsigned long long)w.n[c]);
        for (int b = 0; b < DG_NB; b++)
            o += snprintf(s + o, sizeof s - o, "%s%llu", b ? " " : "", (unsigned long long)w.hist[c][b]);
        o += snprintf(s + o, sizeof s - o, "]%s", c < DG_N - 1 ? "; " : "");
    }
    rt_log("core-diag: frames %llu-%llu: %s; long frames %llu (game wait > 1 ms in %llu); SVC 0x36 %llu (flag set %llu)",
           (unsigned long long)frame - 59, (unsigned long long)frame, s, (unsigned long long)w.long_frames,
           (unsigned long long)w.long_game_frames, (unsigned long long)s36, (unsigned long long)s36f);
    o = 0;
    for (int k = 0; k < 6; k++) {
        int bi = -1;
        for (int i = 0; i < DG_OWN; i++)
            if (w.own[i].ns && (bi < 0 || w.own[i].ns > w.own[bi].ns)) bi = i;
        if (bi < 0) break;
        o += snprintf(s + o, sizeof s - o, " %#x p%d %s %.3f;", w.own[bi].h, w.own[bi].prio, dg_name[w.own[bi].cls],
                      w.own[bi].ns / 60e6);
        w.own[bi].ns = 0;
    }
    if (o) rt_log("core-diag: owners (ms/frame):%s", s);
}
// MKW_TRACE=sched: every 120 presents, the time split of the busiest threads (running, waiting for a core, host
// calls) and of the render thread, the render and the main thread's longest waits by object and by the thread whose
// signal ended them, and the preemption latency.
#define REP_N 8
// At each present (holding G): starts recording at MKW_CRIT's first frame, stops and writes the file at its last.
// File: "CRIT", thread count, event count (u32 each); per thread handle, priority, flags (1 main, 2 presents), host
// tid (u32 each); then the CritEv records.
static void crit_frame(RtThread *self, uint64_t frame) {
    static int parsed;
    if (!parsed) {
        parsed = 1;
        const char *e = cfg_get("MKW_CRIT");
        unsigned long long lo, hi;
        double wl, wh;
        if (e && e[0] == 'w' && sscanf(e + 1, "%lf-%lf", &wl, &wh) == 2 && wl >= 0 && wl < wh &&
            (g_crit = malloc(CRIT_N * sizeof *g_crit))) {
            g_crit_wlo = (int64_t)(wl * 1e6), g_crit_whi = (int64_t)(wh * 1e6);
            g_crit_lo = g_crit_hi = UINT64_MAX;
            rt_log("crit: recording world seconds %g-%g", wl, wh);
        } else if (e && sscanf(e, "%llu-%llu", &lo, &hi) == 2 && lo < hi && (g_crit = malloc(CRIT_N * sizeof *g_crit))) {
            g_crit_lo = lo;
            g_crit_hi = hi;
            rt_log("crit: recording frames %llu-%llu", lo, hi);
        }
    }
    if (g_crit && g_crit_whi && g_crit_hi == UINT64_MAX) {   // world mode: fix the frames as the clock passes lo and hi
        int64_t w = frame_step_world_us();
        if (g_crit_lo == UINT64_MAX) {
            if (w < g_crit_wlo) return;
            g_crit_lo = frame;
        }
        if (w > g_crit_whi) g_crit_hi = frame;
    }
    if (!g_crit || frame < g_crit_lo || frame > g_crit_hi) return;
    g_crit_on = 1;
    crit_add(CRIT_FRAME, self ? self->handle : 0, 0, 0, frame, kern_now_ns());
    if (frame < g_crit_hi) return;
    g_crit_on = 0;
    const char *path = cfg_get("MKW_CRIT_OUT");
    FILE *f = fopen(path && *path ? path : "crit.bin", "wb");
    if (!f) {
        rt_log("crit: cannot write %s", path && *path ? path : "crit.bin");
        return;
    }
    uint32_t nt = 0;
    for (RtThread *t = g_all; t; t = t->all) nt++;
    fwrite("CRIT", 1, 4, f);
    fwrite(&nt, 4, 1, f);
    fwrite(&g_crit_n, 4, 1, f);
    for (RtThread *t = g_all; t; t = t->all) {
        uint32_t row[4] = {t->handle, (uint32_t)t->prio, (uint32_t)(t->is_main | (t == g_render) << 1), t->tid};
        fwrite(row, 4, 4, f);
    }
    fwrite(g_crit, sizeof *g_crit, g_crit_n, f);
    fclose(f);
    rt_log("crit: %u events, %u threads, frames %llu-%llu%s", g_crit_n, nt, (unsigned long long)g_crit_lo,
           (unsigned long long)g_crit_hi, g_crit_n >= CRIT_N ? " (buffer full: the last frames are incomplete)" : "");
}

// MKW_GUEST_PRIO (default 0; "ab" in the B blocks): the guest threads' host threads above normal priority. With the game
// running ahead of the recording (MKW_FENCE_EARLY) the frame's critical path was one guest thread running (guest 0x105c
// in a 24-kart Grand Prix, 9 ms of an 18 ms frame, fe-crit 27/11/2026) while the 7 chunk threads, the replay and the
// driver's threads competed with it for 8 cores at the same priority. Applied at a present when the switch changes and
// every 60 presents for threads made since. Called from perf_present.
void kern_prio_frame(uint64_t frame) {
    static int mode = -1, applied = -1;
    static uint32_t n_applied;
    if (mode < 0) {
        mode = rt_knob("GUEST_PRIO", "0");
        if (mode) rt_log("kernel: guest threads above normal priority%s (MKW_GUEST_PRIO)", mode == 2 ? " in the B blocks" : "");
    }
    if (!mode) return;
    int on = RT_KNOB_ON(mode);
    uint32_t tids[512], n = 0;
    lock();
    uint32_t all = 0;
    for (RtThread *t = g_all; t; t = t->all) all++;
    if (on == applied && (all == n_applied || frame % 60)) {
        unlock();
        return;
    }
    for (RtThread *t = g_all; t && n < 512; t = t->all)
        if (t->tid) tids[n++] = t->tid;
    unlock();
    for (uint32_t i = 0; i < n; i++) plat_thread_set_priority_tid(tids[i], on ? PLAT_PRIO_ABOVE_NORMAL : PLAT_PRIO_NORMAL);
    applied = on, n_applied = all;
}

// prof.h: the sampler's MKW_PROFILE_HANDLE (the guest thread tools/run/crit_path.py names, as a role of its own).
uint32_t kern_thread_tid(uint32_t handle) {
    uint32_t tid = 0;
    lock();
    for (RtThread *t = g_all; t && !tid; t = t->all)
        if (t->handle == handle) tid = t->tid;
    unlock();
    return tid;
}

void kern_sched_report(uint64_t frame) {
    static uint64_t last;
    typedef struct {
        uint32_t h;
        int prio, render;
        uint64_t mask, entry, ns[S_N], nblk, npre, cpu, lw_ns, lw_n, lw_max, lg_ns, lg_n, lg_max, sl_n, sl_req, sl_act;
        uint64_t sp_n, sp_ho, sp_site;
    } Row;
    Row rows[REP_N + 1];
    WaitRec rw[2][4] = {0};   // the render and the main thread's longest waits
    int nr = 0;
    RtThread *self = tcur;
    lock();
    if (g_render != self) {
        g_render = self;
        memset(g_rw, 0, sizeof g_rw);
    }
    crit_frame(self, frame);
    if (frame % 120) {
        unlock();
        return;
    }
    uint64_t now = kern_now_ns(), span = last ? now - last : 0;
    last = now;
    RtThread *top[REP_N + 1] = {0};
    uint64_t busy[REP_N + 1] = {0}, all_run = 0, all_core = 0;
    for (RtThread *t = g_all; t; t = t->all) {
        acct(t, t->st);   // close the open interval
        uint64_t b = t->st_ns[S_RUN] + t->st_ns[S_CORE] + t->st_ns[S_HOST];
        all_run += t->st_ns[S_RUN];
        all_core += t->st_ns[S_CORE];
        if (t == self) continue;
        for (int i = 0; i < REP_N; i++)
            if (!top[i] || b > busy[i]) {
                memmove(top + i + 1, top + i, (REP_N - 1 - i) * sizeof *top);
                memmove(busy + i + 1, busy + i, (REP_N - 1 - i) * sizeof *busy);
                top[i] = t;
                busy[i] = b;
                break;
            }
    }
    if (self) {   // the render thread first
        memmove(top + 1, top, REP_N * sizeof *top);
        top[0] = self;
    }
    for (int i = 0; i <= REP_N && top[i]; i++) {
        RtThread *t = top[i];
        Row *r = &rows[nr++];
        r->h = t->handle;
        r->prio = t->prio;
        r->render = t == self;
        r->mask = t->mask;
        r->entry = t->entry;
        memcpy(r->ns, t->st_ns, sizeof r->ns);
        r->nblk = t->nblk;
        r->npre = t->npre;
        r->lw_ns = t->lw_ns, r->lw_n = t->lw_n, r->lw_max = t->lw_max;
        r->lg_ns = t->lg_ns, r->lg_n = t->lg_n, r->lg_max = t->lg_max;
        r->sl_n = t->sl_n, r->sl_req = t->sl_req, r->sl_act = t->sl_act;
        r->sp_n = t->sp_n, r->sp_ho = t->sp_ho, r->sp_site = t->sp_site;
        r->cpu = 0;
        uint64_t c = t->tid ? plat_thread_cpu_ns(t->tid) : 0;
        if (c) {
            r->cpu = t->cpu_last ? c - t->cpu_last : 0;
            t->cpu_last = c;
        }
    }
    for (int k = 0; k < 2; k++) {
        const WaitRec *src = k ? g_mw : g_rw;
        for (int i = 0; i < RW_N && src[i].ns; i++)
            for (int j = 0; j < 4; j++)
                if (src[i].ns > rw[k][j].ns) {
                    memmove(rw[k] + j + 1, rw[k] + j, (3 - j) * sizeof **rw);
                    rw[k][j] = src[i];
                    break;
                }
    }
    uint64_t pre_stale = g_pre_stale, pre_req = g_pre_req, pre_n = g_pre_n, pre_ns = g_pre_ns, pre_max = g_pre_max;
    uint64_t sc = g_sync_calls, sb = g_sync_blocks;
    for (RtThread *t = g_all; t; t = t->all) {
        memset(t->st_ns, 0, sizeof t->st_ns);
        t->nblk = t->npre = 0;
        t->lw_ns = t->lw_n = t->lw_max = t->lg_ns = t->lg_n = t->lg_max = t->sl_n = t->sl_req = t->sl_act = 0;
        t->sp_n = t->sp_ho = 0;   // racy against the thread's own increments: a few passes may be lost
    }
    memset(g_rw, 0, sizeof g_rw);
    memset(g_mw, 0, sizeof g_mw);
    g_pre_req = g_pre_n = g_pre_ns = g_pre_max = g_pre_stale = 0;
    g_sync_calls = g_sync_blocks = 0;
    unlock();
    if (!span) return;
    rt_log("sched: frame %llu: %.2f s, cores busy %.2f of %d, core wait %.2f thread-s, preempt req %llu released %llu "
           "avg %.2f ms max %.2f ms, WaitSynchronization %llu calls %llu blocks, stale preempt %llu%s",
           (unsigned long long)frame, span / 1e9, (double)all_run / span, NCORE, all_core / 1e9,
           (unsigned long long)pre_req, (unsigned long long)pre_n, pre_n ? pre_ns / 1e6 / pre_n : 0.0, pre_max / 1e6,
           (unsigned long long)sc, (unsigned long long)sb, (unsigned long long)pre_stale, self ? "" : " (render thread is not a guest thread)");
    for (int i = 0; i < nr; i++) {
        Row *r = &rows[i];
        char d[128], s[256];
        int o = 0;
        for (int k = S_RUN; k < S_N; k++)
            o += snprintf(s + o, sizeof s - o, " %s %.0f%%", s_name[k], 100.0 * r->ns[k] / span);
        ldr_describe(r->entry, d, sizeof d);
        rt_log("sched: %s%#x p%d m%llx%s cpu %.0f%% blocks %llu preempted %llu (%s)", r->render ? "render " : "",
               r->h, r->prio, (unsigned long long)r->mask, s, 100.0 * r->cpu / span, (unsigned long long)r->nblk,
               (unsigned long long)r->npre, r->entry ? d : "main");
        rt_log("sched:   %#x wake to run avg %.1f us max %.0f us (%llu), core grant to run avg %.1f us max %.0f us "
               "(%llu), sleep %llu asked avg %.1f us took avg %.1f us", r->h, r->lw_n ? r->lw_ns / 1e3 / r->lw_n : 0.0,
               r->lw_max / 1e3, (unsigned long long)r->lw_n, r->lg_n ? r->lg_ns / 1e3 / r->lg_n : 0.0, r->lg_max / 1e3,
               (unsigned long long)r->lg_n, (unsigned long long)r->sl_n, r->sl_n ? r->sl_req / 1e3 / r->sl_n : 0.0,
               r->sl_n ? r->sl_act / 1e3 / r->sl_n : 0.0);
        if (r->sp_n) {
            ldr_describe(r->sp_site, d, sizeof d);
            rt_log("sched:   %#x spin wait passes %llu, core handoffs %llu, last at %s", r->h,
                   (unsigned long long)r->sp_n, (unsigned long long)r->sp_ho, d);
        }
    }
    for (int k = 0; k < 2; k++)
        for (int j = 0; j < 4 && rw[k][j].ns; j++) {
            char by[32] = "";
            if (rw[k][j].waker) snprintf(by, sizeof by, ", woken by %#x", rw[k][j].waker);
            rt_log("sched: %s wait %d: %s on %s%#llx%s: %.1f%%", k ? "main" : "render", j + 1, s_name[rw[k][j].st],
                   rw[k][j].st == S_HOST ? "exe+" : "", (unsigned long long)rw[k][j].obj, by, 100.0 * rw[k][j].ns / span);
        }
}

// ctl `threads` (any thread, no lock: a snapshot for a hung run, where every guest thread sleeps and ptrace is not
// allowed): each guest thread's wait (kind, address or key, object) and its guest return addresses from lr and the
// frame-pointer chain, main+off for main. lhz120tt2 (19/11/2026): the race stopped at frame 1097 with every thread in
// a futex and nothing said where.
void kern_dump_threads(void) {
    static const char *const wk[] = {"run", "lock", "condvar", "address", "sync"};
    const uint64_t main_base = 0x7000000000ull, main_end = main_base + 0x10000000ull;
    for (RtThread *t = __atomic_load_n(&g_all, __ATOMIC_ACQUIRE); t; t = t->all) {
        Cpu *c = t->cur ? t->cur : t->cpu;
        char bt[256];
        int n = 0;
        uint64_t ra[9];
        int k = 0;
        ra[k++] = c->x[30];
        uint64_t fp = c->x[29];
        for (; k < 9 && fp && !(fp & 7) && t->stack_top && fp < t->stack_top && fp + 0x1000000 > t->stack_top; k++) {
            ra[k] = ((const uint64_t *)(uintptr_t)fp)[1];
            uint64_t up = ((const uint64_t *)(uintptr_t)fp)[0];
            if (up <= fp) { k++; break; }
            fp = up;
        }
        for (int i = 0; i < k && n < (int)sizeof bt - 32; i++)
            n += ra[i] >= main_base && ra[i] < main_end
                     ? snprintf(bt + n, sizeof bt - n, " main+%#llx", (unsigned long long)(ra[i] - main_base))
                     : snprintf(bt + n, sizeof bt - n, " %#llx", (unsigned long long)ra[i]);
        int w = t->wkind;
        rt_log("threads: h %#x prio %d core %d %s host %d addr %#llx key %#llx obj %#llx svc %#x exited %d; lr/fp:%s",
               t->handle, t->prio, t->cur_core, w >= 0 && w < 5 ? wk[w] : "?",
               __atomic_load_n(&t->host_depth, __ATOMIC_RELAXED), (unsigned long long)t->waddr,
               (unsigned long long)t->wkey, (unsigned long long)t->wobj, t->dg_svc, t->exited, n ? bt : "");
    }
}
