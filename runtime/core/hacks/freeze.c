// Freeze: stops the game on a frame to look at it, and steps one frame at a time. F3 freezes and
// unfreezes, F4 lets one more frame through, with MKW_FREEZE_KEYS=1; tools/run/ctl.sh NAME pause | step [N] | resume do
// the same from a script. The game thread waits in its present (nvn_api_obj.c nvnQueuePresentTexture), after the frame
// went to the back end, so the window holds that frame; the game's other threads (audio, loading) go on. The pause key
// (pause.c) and a plugin's hold suspend the game through here too (ov_system.c hold_apply); its key is polled here, so
// it is read also while the game waits.
#include <stdatomic.h>

#include "cfg.h"
#include "hacks.h"
#include "host.h"
#include "input.h"
#include "platform/platform.h"

static _Atomic int frozen;
static _Atomic int64_t steps;   // frames still to let through while frozen
static int f3_was, f4_was;

void freeze_set(int on, int64_t n) {
    atomic_store(&steps, n);
    atomic_store(&frozen, on);
    rt_log("freeze: %s%s", on ? "frozen" : "running", on && n ? " after the next frames" : "");
}

static void keys(void) {
    pause_poll();
    if (!cfg_bool("MKW_FREEZE_KEYS")) return;
    int f3 = input_hack_key(HACK_KEY_FREEZE), f4 = input_hack_key(HACK_KEY_STEP);
    if (f3 && !f3_was) freeze_set(!atomic_load(&frozen), 0);
    if (f4 && !f4_was && atomic_load(&frozen)) atomic_fetch_add(&steps, 1);
    f3_was = f3, f4_was = f4;
}

void freeze_present(void) {
    keys();
    if (!atomic_load(&frozen)) return;
    if (atomic_load(&steps) > 0) {
        atomic_fetch_sub(&steps, 1);
        return;
    }
    while (atomic_load(&frozen) && atomic_load(&steps) <= 0) {
        plat_sleep_ms(10);
        keys();
    }
    if (atomic_load(&steps) > 0) atomic_fetch_sub(&steps, 1);
}
