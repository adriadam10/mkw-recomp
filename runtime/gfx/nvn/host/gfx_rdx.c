// The RenoDX tone mapper's programs (tools/shaders/renodx.py, loaded by gfx_shader.c): the specialization constants
// renodx.py declares get their value written into the module itself, as the default of the OpSpecConstant, since no
// pipeline gives those ids. Apart from gfx_shader.c so that tests/translator/test_rdx.py builds it alone.
#include <stdint.h>
#include <string.h>

#include "gfx.h"

// Sets the float specialization constant decorated SpecId id to v in the module's words (n of them, the 5-word
// header included). 1 when found; 0, and the words as they were, when the module has no such constant or ends
// inside an instruction.
int gfx_rdx_patch(uint32_t *w, uint64_t n, uint32_t id, float v) {
    uint32_t target = 0;
    for (uint64_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > n) return 0;
        if (op == 71 && len == 4 && w[i + 2] == 1 && w[i + 3] == id) target = w[i + 1];   // OpDecorate %t SpecId id
        if (op == 50 && len == 4 && target && w[i + 2] == target) {   // OpSpecConstant %float %t value
            memcpy(&w[i + 3], &v, 4);
            return 1;
        }
        i += len;
    }
    return 0;
}
