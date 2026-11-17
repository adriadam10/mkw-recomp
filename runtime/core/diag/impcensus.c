// Import census (MKW_IMPORT_CENSUS=<file>): which functions of the SDK modules (sdk, subsdk0) the game's own code
// calls during a run, how often and from which frame on, so the SDK work list follows what MKW really uses rather
// than its 1 289 imports. Overridden imports never reach guest code and are not
// listed here: hostfn_report counts those.
//
// It rides the function-entry hook of the lifted code (FN_ENTER, g_fnprof), as MKW_SEQ_WATCH does: a call counts when
// the callee is outside main and the return address inside it, which takes the PLT calls and the game's calls through
// SDK function pointers alike. No wrapper sits around the call, so threads and preemption behave as without the
// census; the cost is a range check per guest call and a hash probe per call from main into the SDK.
//
//   file   one line per function: module, offset, calls, first frame, "symbol+offset" (ldr_describe), written at exit
//          (prof_write); tools/analisis/runtime/sdk_gaps.py --census reads it
#include <stdio.h>
#include <string.h>

#include "cfg.h"
#include "gamelib.h"
#include "host.h"
#include "nvn_api.h"
#include "platform/platform.h"

#define HSIZE 8192   // power of two; main imports 1 289 functions

static uint64_t main_lo, main_hi;
static uint64_t keys[HSIZE];   // callee address, 0 = free; claimed once by CAS, never freed
static uint64_t calls[HSIZE], first_frame[HSIZE];
static const char *out_path;
volatile int impcensus_on;

static uint32_t h(uint64_t a) { return (uint32_t)((a >> 2) * 0x9E3779B1u) & (HSIZE - 1); }

void impcensus_init(void) {
    const char *p = cfg_get("MKW_IMPORT_CENSUS");
    if (!p || !*p) return;
    const RtLoaded *m = ldr_module(0);
    if (strcmp(m->name, "main")) rt_fatal("impcensus: module 0 is %s, not main", m->name);
    main_lo = m->base, main_hi = m->base + m->size, out_path = p;
    impcensus_on = 1;
    rt_fnprof_set(1);
    rt_log("impcensus: counting calls from main into the SDK modules (MKW_IMPORT_CENSUS=%s)", p);
}

void impcensus_hit(uint64_t addr, uint64_t lr) {
    if (lr - main_lo >= main_hi - main_lo || addr - main_lo < main_hi - main_lo) return;
    for (uint32_t i = h(addr), n = 0; n < HSIZE; i = (i + 1) & (HSIZE - 1), n++) {
        uint64_t k = __atomic_load_n(&keys[i], __ATOMIC_ACQUIRE);
        if (!k) {
            uint64_t zero = 0;
            if (!__atomic_compare_exchange_n(&keys[i], &zero, addr, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) k = zero;
            else {
                __atomic_store_n(&first_frame[i], __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED),
                                 __ATOMIC_RELAXED);
                k = addr;
            }
        }
        if (k == addr) {
            __atomic_fetch_add(&calls[i], 1, __ATOMIC_RELAXED);
            return;
        }
    }
    rt_fatal("impcensus: more than %d SDK functions called from main", HSIZE);
}

void impcensus_write(void) {
    if (!impcensus_on) return;
    FILE *f = fopen(out_path, "w");
    if (!f) { rt_log("impcensus: cannot write %s", out_path); return; }
    fprintf(f, "# module\toffset\tcalls\tfirst_frame\tsymbol (MKW_IMPORT_CENSUS, runtime/core/diag/impcensus.c)\n");
    int n = 0;
    for (int i = 0; i < HSIZE; i++) {
        uint64_t a = __atomic_load_n(&keys[i], __ATOMIC_ACQUIRE);
        if (!a) continue;
        char where[512], mod[16] = "?";
        unsigned long long off = 0;
        ldr_describe(a, where, sizeof where);
        sscanf(where, "%15[^+]+%llx", mod, &off);
        const char *sym = strchr(where, '(');
        fprintf(f, "%s\t%#llx\t%llu\t%llu\t%.*s\n", mod, off, (unsigned long long)calls[i],
                (unsigned long long)first_frame[i], sym ? (int)strlen(sym + 1) - 1 : 0, sym ? sym + 1 : "");
        n++;
    }
    fclose(f);
    rt_log("impcensus: %d SDK functions called from main written to %s", n, out_path);
}
