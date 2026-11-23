// renhdrum_host_win.c with the bridge (renhdrum_recomp_bridge_win.c) and the stub headers test_renhdrum.py writes: what a player sees
// with MKW_RENHDRUM off, on without renhdrum.dll beside the exe, and on with a renhdrum.dll of another version or of the
// right one (t_renhdrum_dll.c, no controller). Every call the game makes must be harmless in each case, and the log says
// why RenHDrum is not used. Run by test_renhdrum.py with the DLL (or none) in the exe's folder.
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "renhdrum_host.h"

int cfg_bool(const char *name) {
    const char *e = getenv(name);
    return e && !strcmp(e, "1");
}

void rt_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("[host] ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

uint64_t plat_now_ns(void) {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (uint64_t)((double)c.QuadPart * 1e9 / (double)f.QuadPart);
}

int32_t plat_xchg32(volatile int32_t *p, int32_t v) { return (int32_t)InterlockedExchange((volatile LONG *)p, v); }

bool SDL_InitSubSystem(uint32_t flags) { (void)flags; return true; }

int main(void) {
    int pad;
    const float v[4] = {0.5f, 160.0f, 0.5f, 320.0f};
    renhdrum_start();
    renhdrum_attach(&pad);
    renhdrum_vibration(0, v);
    renhdrum_vibration(1, v);
    printf("claims=%d\n", renhdrum_claims());
    renhdrum_permit(0);
    renhdrum_permit(1);
    renhdrum_detach(&pad);
    renhdrum_shutdown();
    renhdrum_shutdown();   // the exit path may run twice
    printf("done\n");
    return 0;
}
