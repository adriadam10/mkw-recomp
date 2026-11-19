// --probe-plugin: the native plugin API (plugin/plugin.c) with tools/plugin/probe_plugin.c, which tools/plugin/probe.sh
// builds as the enabled mod plugin-probe. Loads it the way rt_boot does (plugins_load, ldr_load, the game-loaded
// callbacks, the seal), then calls guest code both ways: through main's GOT slot of nn::os::GetSystemTick (the
// override by name, chained to the runtime's clock) and through the game's own definition of it (the override by
// address, which calls the translated original). The plugin's overlay drawing must be on (overlay_set from its init
// accepted, from the game-loaded callback refused).
#include <stdio.h>
#include <string.h>

#include "host.h"
#include "plugin.h"
#include "platform/platform.h"

#define TICK "_ZN2nn2os13GetSystemTickEv"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("  FAIL: " __VA_ARGS__); putchar('\n'); } } while (0)

int probe_plugin(void) {
    plugins_load();
    ldr_load(rt_config()->exefs);
    plugins_game_loaded();
    hostfn_seal();
    plugins_started();

    CHECK(plugins_overlay_wanted(), "the plugin's overlay is not on (overlay_set from init refused, or accepted late)");
    Cpu *c = plat_aligned_alloc(sizeof(Cpu), 64);
    memset(c, 0, sizeof *c);
    uint8_t *stack = mem_kernel_alloc(0x10000, 16);
    c->sp = (uint64_t)(uintptr_t)stack + 0x10000;
    c->excl_addr = ~0ull;

    uint64_t stub = ldr_plt_stub("main", TICK), guest = ldr_symbol(TICK);
    char d[128];
    printf("probe-plugin: main's PLT stub of %s %s, its definition %s\n", TICK, ldr_describe(stub, d, sizeof d),
           guest ? "in the game" : "none");
    CHECK(stub, "main has no PLT stub for %s", TICK);
    if (stub) {
        uint64_t r = rt_call(c, stub, 0);
        printf("  by name: %#llx\n", (unsigned long long)r);
        CHECK(r >> 32 == 0x5eed0001, "the call through the PLT did not reach the plugin's override by name");
    }
    if (guest) {
        uint64_t r = rt_call(c, guest, 0);
        printf("  by address: %#llx\n", (unsigned long long)r);
        CHECK(r >> 32 == 0x5eed0002, "the indirect call to %#llx did not reach the plugin's override by address",
              (unsigned long long)guest);
    }
    mem_kernel_free(stack, 0x10000);
    plat_aligned_free(c);
    printf("probe-plugin: %s\n", failures ? "FAILED" : "ok");
    return failures != 0;
}
