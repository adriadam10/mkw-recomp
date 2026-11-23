// The Win32 calls of RenHDrum's recomp bridge (third_party/renhdrum/renhdrum_recomp_bridge.c, which includes this
// instead of <windows.h> off Windows), on POSIX: SRW locks, interlocked operations, threads, a waitable timer, the
// library loader and wide paths. Only what that one file uses, with the meaning it relies on:
//
// - SRWLOCK is a pthread rwlock. The bridge leaves most of them zero (static RhDsSlot fields, never initialised): an
//   all-zero pthread_rwlock_t is PTHREAD_RWLOCK_INITIALIZER in glibc, as an all-zero SRWLOCK is SRWLOCK_INIT.
// - A HANDLE is a thread or a waitable timer. WaitForSingleObject joins a thread within its timeout (WAIT_TIMEOUT
//   after it, the thread left running and detached at CloseHandle, as Windows does), or sleeps to a timer's due time
//   (clock_nanosleep on CLOCK_MONOTONIC: the senders' 3.9 to 10.7 ms periods without the bridge's spin fallback).
// - WCHAR is wchar_t (UTF-32 here); the exe path and renhdrum.ini go through UTF-8 by hand, so a folder with
//   non-ASCII characters works whatever the C locale.
// - LoadLibraryExW of renhdrum.dll beside the executable loads librenhdrum.so from there (cargo build's name);
//   GetModuleHandleA("SDL3.dll") is the SDL3 the runtime is linked against, "SDL2.dll" none.
#pragma once
#ifndef _WIN32

#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>

typedef int BOOL;
typedef uint32_t DWORD;
typedef int32_t LONG;
typedef wchar_t WCHAR;
typedef void *HMODULE;
typedef void *HANDLE;
typedef void *FARPROC;
typedef union { int64_t QuadPart; } LARGE_INTEGER;
#define WINAPI
#define MAX_PATH PATH_MAX
#define INFINITE 0xFFFFFFFFu
#define WAIT_OBJECT_0 0u
#define WAIT_TIMEOUT 0x102u
#define WAIT_FAILED 0xFFFFFFFFu
#define TIMER_ALL_ACCESS 0u
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 2u

// ---- locks and atomics
typedef pthread_rwlock_t SRWLOCK;
#define SRWLOCK_INIT PTHREAD_RWLOCK_INITIALIZER
static inline void AcquireSRWLockExclusive(SRWLOCK *l) { pthread_rwlock_wrlock(l); }
static inline void ReleaseSRWLockExclusive(SRWLOCK *l) { pthread_rwlock_unlock(l); }
static inline void AcquireSRWLockShared(SRWLOCK *l) { pthread_rwlock_rdlock(l); }
static inline void ReleaseSRWLockShared(SRWLOCK *l) { pthread_rwlock_unlock(l); }

static inline LONG InterlockedExchange(volatile LONG *p, LONG v) { return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }
// Win32 order: (destination, exchange, comparand); returns the value before
static inline LONG InterlockedCompareExchange(volatile LONG *p, LONG v, LONG cmp) {
    __atomic_compare_exchange_n(p, &cmp, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return cmp;
}

// ---- time
static inline int64_t rh_posix_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}
static inline void rh_posix_sleep_until(int64_t ns) {
    struct timespec ts = {(time_t)(ns / 1000000000), (long)(ns % 1000000000)};
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {}
}
static inline void Sleep(DWORD ms) {
    if (!ms) sched_yield();
    else rh_posix_sleep_until(rh_posix_now_ns() + (int64_t)ms * 1000000);
}
static inline BOOL SwitchToThread(void) { return sched_yield() == 0; }

// ---- handles: a thread or a waitable timer
typedef DWORD(WINAPI *LPTHREAD_START_ROUTINE)(void *);
typedef struct {
    int timer;
    pthread_t th;   // thread
    LPTHREAD_START_ROUTINE fn;
    void *arg;
    int joined;
    int64_t due;    // timer: CLOCK_MONOTONIC ns
} RhPosixHandle;

static void *rh_posix_thread_main(void *p) {
    RhPosixHandle *h = p;
    h->fn(h->arg);
    return NULL;
}
static inline HANDLE CreateThread(void *sa, size_t stack, LPTHREAD_START_ROUTINE fn, void *arg, DWORD flags, DWORD *id) {
    (void)sa, (void)stack, (void)flags, (void)id;
    RhPosixHandle *h = calloc(1, sizeof *h);
    if (!h) return NULL;
    h->fn = fn, h->arg = arg;
    if (pthread_create(&h->th, NULL, rh_posix_thread_main, h)) {
        free(h);
        return NULL;
    }
    return h;
}
static inline HANDLE CreateWaitableTimerExW(void *sa, const WCHAR *name, DWORD flags, DWORD access) {
    (void)sa, (void)name, (void)flags, (void)access;
    RhPosixHandle *h = calloc(1, sizeof *h);
    if (h) h->timer = 1;
    return h;
}
// due < 0: relative, in 100 ns units (the only form the bridge uses)
static inline BOOL SetWaitableTimerEx(HANDLE t, const LARGE_INTEGER *due, LONG period, void *fn, void *arg, void *why,
                                      DWORD tolerance) {
    (void)period, (void)fn, (void)arg, (void)why, (void)tolerance;
    RhPosixHandle *h = t;
    if (!h || !h->timer || due->QuadPart >= 0) return 0;
    h->due = rh_posix_now_ns() - due->QuadPart * 100;
    return 1;
}
static inline DWORD WaitForSingleObject(HANDLE p, DWORD ms) {
    RhPosixHandle *h = p;
    if (!h) return WAIT_FAILED;
    if (h->timer) {
        rh_posix_sleep_until(h->due);
        return WAIT_OBJECT_0;
    }
    if (h->joined) return WAIT_OBJECT_0;
    if (ms == INFINITE) {
        if (pthread_join(h->th, NULL)) return WAIT_FAILED;
    } else {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);   // pthread_timedjoin_np's clock
        ts.tv_sec += ms / 1000, ts.tv_nsec += (long)(ms % 1000) * 1000000;
        if (ts.tv_nsec >= 1000000000) ts.tv_sec++, ts.tv_nsec -= 1000000000;
        int r = pthread_timedjoin_np(h->th, NULL, &ts);
        if (r == ETIMEDOUT) return WAIT_TIMEOUT;
        if (r) return WAIT_FAILED;
    }
    h->joined = 1;
    return WAIT_OBJECT_0;
}
static inline BOOL CloseHandle(HANDLE p) {
    RhPosixHandle *h = p;
    if (!h) return 0;
    if (!h->timer && !h->joined) pthread_detach(h->th);   // still running: it keeps its handle (leaked, as rare as the timeout)
    else free(h);
    return 1;
}

// ---- wide strings <-> UTF-8
static inline size_t rh_posix_to_utf8(const WCHAR *w, char *out, size_t cap) {
    size_t n = 0;
    for (; *w; w++) {
        uint32_t c = (uint32_t)*w;
        char b[4];
        size_t k = c < 0x80 ? (b[0] = (char)c, 1)
                 : c < 0x800 ? (b[0] = (char)(0xC0 | c >> 6), b[1] = (char)(0x80 | (c & 0x3F)), 2)
                 : c < 0x10000 ? (b[0] = (char)(0xE0 | c >> 12), b[1] = (char)(0x80 | (c >> 6 & 0x3F)),
                                  b[2] = (char)(0x80 | (c & 0x3F)), 3)
                 : (b[0] = (char)(0xF0 | c >> 18), b[1] = (char)(0x80 | (c >> 12 & 0x3F)),
                    b[2] = (char)(0x80 | (c >> 6 & 0x3F)), b[3] = (char)(0x80 | (c & 0x3F)), 4);
        if (n + k >= cap) return 0;
        memcpy(out + n, b, k), n += k;
    }
    out[n] = 0;
    return n;
}
static inline size_t rh_posix_from_utf8(const char *s, WCHAR *out, size_t cap) {
    size_t n = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        uint32_t c = *p++;
        int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        if (more) c &= 0x3F >> more;
        for (; more && (*p & 0xC0) == 0x80; more--) c = c << 6 | (*p++ & 0x3F);
        if (more || n + 1 >= cap) return 0;   // truncated sequence, or no room
        out[n++] = (WCHAR)c;
    }
    out[n] = 0;
    return n;
}

static inline FILE *_wfopen(const WCHAR *path, const WCHAR *mode) {
    char p[PATH_MAX * 4], m[8];
    if (!rh_posix_to_utf8(path, p, sizeof p) || !rh_posix_to_utf8(mode, m, sizeof m)) return NULL;
    return fopen(p, m);
}

// ---- modules
static inline DWORD GetModuleFileNameW(HMODULE mod, WCHAR *out, DWORD cap) {
    char p[PATH_MAX];
    ssize_t n = mod ? -1 : readlink("/proc/self/exe", p, sizeof p - 1);
    if (n <= 0) return 0;
    p[n] = 0;
    return (DWORD)rh_posix_from_utf8(p, out, cap);
}
static inline HMODULE LoadLibraryExW(const WCHAR *path, HANDLE file, DWORD flags) {
    (void)file, (void)flags;
    char p[PATH_MAX * 4];
    if (!rh_posix_to_utf8(path, p, sizeof p)) return NULL;
    char *base = strrchr(p, '/');
    base = base ? base + 1 : p;
    if (!strcmp(base, "renhdrum.dll") && (size_t)(base - p) + sizeof "librenhdrum.so" <= sizeof p)
        memcpy(base, "librenhdrum.so", sizeof "librenhdrum.so");
    return dlopen(p, RTLD_NOW | RTLD_LOCAL);
}
static inline BOOL FreeLibrary(HMODULE h) { return h && !dlclose(h); }
static inline FARPROC GetProcAddress(HMODULE h, const char *name) { return h ? dlsym(h, name) : NULL; }
// Already-loaded modules only; the handle stays valid while the runtime keeps SDL3 loaded (always: it links it).
static inline HMODULE GetModuleHandleA(const char *name) {
    if (!name || strcmp(name, "SDL3.dll")) return NULL;
    HMODULE h = dlopen("libSDL3.so.0", RTLD_NOW | RTLD_NOLOAD);
    if (h) dlclose(h);   // NOLOAD took a reference of its own
    return h;
}

#endif
