// A renhdrum.dll with only the eight functions the bridge needs (renhdrum_recomp_bridge_win.c RH_REQ), no controller, built
// by test_renhdrum.py with -DMOCK_VERSION=0x000500 (the version the bridge wants) or another to see it refuse. Why: the
// real one is a Rust binary we neither build nor run in tests.
#include <stdbool.h>
#include <stdint.h>

#define EXPORT __declspec(dllexport)

EXPORT uint32_t renhdrum_version(void) { return MOCK_VERSION; }
EXPORT void *renhdrum_engine_create(bool bluetooth, bool pro) { (void)bluetooth; (void)pro; return 0; }
EXPORT void renhdrum_engine_destroy(void *e) { (void)e; }
EXPORT int32_t renhdrum_settings_default(void *out) { (void)out; return 0; }
EXPORT int32_t renhdrum_engine_apply_settings(void *e, const void *s) { (void)e; (void)s; return 0; }
EXPORT int32_t renhdrum_engine_push_half_sample(void *e, int64_t t, bool right, float a, float b, float c, float d) {
    (void)e; (void)t; (void)right; (void)a; (void)b; (void)c; (void)d;
    return 0;
}
EXPORT int32_t renhdrum_engine_tick(void *e, int64_t t, uint8_t *out) { (void)e; (void)t; (void)out; return 0; }
EXPORT void renhdrum_set_nx2_mode(int32_t mode) { (void)mode; }
