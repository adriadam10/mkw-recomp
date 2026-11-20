// The parts of platform.h built on its locks or its other calls rather than on the OS: recursive mutexes, one-time
// initialisation, events and the clean-up of torn atomic writes. Every OS shares them.
#include <stdio.h>
#include <string.h>

#include "platform.h"

// The present thread used this policy locally. Share it with image acquisition: millisecond Sleep overshoots by
// several ms with TIMER_1MS=0 on Windows, while a shorter high-resolution guard can still wake after the deadline.
// Keep twice the platform's expected late wake-up as margin; yield there rather than occupying a core with PAUSE.
void plat_sleep_until_ns(uint64_t deadline) {
    const uint64_t guard = 2ull * PLAT_WAIT_LATE_NS;
    uint64_t now = plat_now_ns();
    if (deadline > now && deadline - now > guard + guard / 2) plat_wait_ns(deadline - now - guard);
    while (plat_now_ns() < deadline) plat_yield();
}

// ---- recursive mutex: owner is the holder's thread id (0 when free); only the holder writes depth.

void plat_rmutex_lock(PlatRMutex *m) {
    uint32_t me = plat_thread_id();
    if (__atomic_load_n(&m->owner, __ATOMIC_RELAXED) == me) {
        m->depth++;
        return;
    }
    plat_mutex_lock_from(&m->m, (uintptr_t)__builtin_return_address(0));
    __atomic_store_n(&m->owner, me, __ATOMIC_RELAXED);
    m->depth = 1;
}

int plat_rmutex_trylock(PlatRMutex *m) {
    uint32_t me = plat_thread_id();
    if (__atomic_load_n(&m->owner, __ATOMIC_RELAXED) == me) {
        m->depth++;
        return 1;
    }
    if (!plat_mutex_trylock(&m->m)) return 0;
    __atomic_store_n(&m->owner, me, __ATOMIC_RELAXED);
    m->depth = 1;
    return 1;
}

void plat_rmutex_unlock(PlatRMutex *m) {
    if (--m->depth) return;
    __atomic_store_n(&m->owner, 0, __ATOMIC_RELAXED);
    plat_mutex_unlock(&m->m);
}

// ---- once: state 0 not run, 1 running, 2 done. Waiters for a running initialiser sleep on one process-wide
// condition variable; initialisers are rare and short.

static PlatMutex once_lock = PLAT_MUTEX_INIT;
static PlatCond once_done = PLAT_COND_INIT;

void plat_once(PlatOnce *o, void (*fn)(void *arg), void *arg) {
    if (__atomic_load_n(&o->state, __ATOMIC_ACQUIRE) == 2) return;
    int32_t expected = 0;
    if (__atomic_compare_exchange_n(&o->state, &expected, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        fn(arg);
        plat_mutex_lock(&once_lock);
        __atomic_store_n(&o->state, 2, __ATOMIC_RELEASE);
        plat_cond_broadcast(&once_done);
        plat_mutex_unlock(&once_lock);
        return;
    }
    plat_mutex_lock(&once_lock);
    while (__atomic_load_n(&o->state, __ATOMIC_ACQUIRE) != 2) plat_cond_wait(&once_done, &once_lock);
    plat_mutex_unlock(&once_lock);
}

// ---- events

void plat_event_init(PlatEvent *e, int manual, int initially_set) {
    plat_mutex_init(&e->m);
    plat_cond_init(&e->c);
    e->manual = manual;
    e->set = initially_set;
}

void plat_event_set(PlatEvent *e) {
    plat_mutex_lock(&e->m);
    e->set = 1;
    if (e->manual) plat_cond_broadcast(&e->c);
    else plat_cond_signal(&e->c);
    plat_mutex_unlock(&e->m);
}

void plat_event_wait(PlatEvent *e) {
    plat_mutex_lock(&e->m);
    while (!e->set) plat_cond_wait(&e->c, &e->m);
    if (!e->manual) e->set = 0;
    plat_mutex_unlock(&e->m);
}

int plat_event_wait_ns(PlatEvent *e, uint64_t ns) {
    uint64_t end = plat_now_ns() + ns;
    plat_mutex_lock(&e->m);
    while (!e->set) {
        uint64_t now = plat_now_ns();
        if (now >= end) break;
        plat_cond_wait_ns(&e->c, &e->m, end - now);
    }
    int got = e->set;
    if (got && !e->manual) e->set = 0;
    plat_mutex_unlock(&e->m);
    return got;
}

typedef struct { char dir[PLAT_PATH_MAX * 2]; const char *base; size_t base_len; int n; } Stale;

static int stale_tmp(const char *name, void *ctx) {
    Stale *s = ctx;
    unsigned pid, tid;
    char end;
    if (strncmp(name, s->base, s->base_len) || name[s->base_len] != '.') return 1;
    if (sscanf(name + s->base_len, ".%u.%u.tm%c", &pid, &tid, &end) != 3 || end != 'p') return 1;
    if (pid == plat_process_id() || plat_process_alive(pid)) return 1;
    char path[PLAT_PATH_MAX * 3];
    snprintf(path, sizeof path, "%s/%s", s->dir, name);
    plat_file_delete(path);
    s->n++;
    return 1;
}

int plat_file_atomic_cleanup(const char *path) {
    Stale s = {0};
    const char *cut = plat_last_sep(path);
    if (cut) snprintf(s.dir, sizeof s.dir, "%.*s", (int)(cut - path), path);
    else snprintf(s.dir, sizeof s.dir, ".");
    s.base = cut ? cut + 1 : path;
    s.base_len = strlen(s.base);
    plat_file_list(s.dir, ".tmp", stale_tmp, &s);
    return s.n;
}
