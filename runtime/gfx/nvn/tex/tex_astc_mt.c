// astc_decode_ldr_mt: astc_decode_ldr with the block rows of one image spread over the texture workers (tex_par.c).
// Each block goes through astc_decode_block and lands where astc_decode_ldr puts it, so the output is the same bytes.
// An image under MT_MIN_BLOCKS, or one while the workers are busy, decodes on the caller's thread alone.
#include <string.h>

#include "tex.h"

#define MT_MIN_BLOCKS 1024   // below this, waking the pool costs about as much as the decode

typedef struct {
    const uint8_t *blocks;
    uint8_t *out;
    uint32_t width, height, bw, bh, bx, by, chunk;
    bool srgb;
} Job;

// Block rows [i * chunk, (i + 1) * chunk) of the image, as astc_decode_ldr's loop does them.
static void decode_rows(void *ctx, uint32_t item) {
    const Job *J = ctx;
    uint32_t bw = J->bw, bh = J->bh, width = J->width, height = J->height;
    uint32_t j0 = item * J->chunk, j1 = j0 + J->chunk < J->by ? j0 + J->chunk : J->by;
    uint8_t tmp[12 * 12 * 4];
    for (uint32_t j = j0; j < j1; j++) {
        for (uint32_t i = 0; i < J->bx; i++) {
            const uint8_t *src = J->blocks + 16 * ((size_t)j * J->bx + i);
            uint32_t x0 = i * bw, y0 = j * bh;
            uint32_t cw = width - x0 < bw ? width - x0 : bw, ch = height - y0 < bh ? height - y0 : bh;
            uint8_t *dst = width == bw && ch == bh ? J->out + (size_t)y0 * width * 4 : tmp;
            astc_decode_block(src, bw, bh, J->srgb, dst);
            if (dst == tmp)
                for (uint32_t y = 0; y < ch; y++)
                    memcpy(J->out + ((size_t)(y0 + y) * width + x0) * 4, tmp + y * bw * 4, cw * 4);
        }
    }
}

void astc_decode_ldr_mt(const uint8_t *blocks, uint32_t width, uint32_t height, uint32_t bw, uint32_t bh, bool srgb,
                        uint8_t *rgba_out) {
    if (bw < 4 || bh < 4 || bw > 12 || bh > 12) return;
    uint32_t bx = (width + bw - 1) / bw, by = (height + bh - 1) / bh;
    if ((uint64_t)bx * by < MT_MIN_BLOCKS || by < 2) {
        astc_decode_ldr(blocks, width, height, bw, bh, srgb, rgba_out);
        return;
    }
    uint32_t parts = (uint32_t)tex_par_threads() * 4;   // a few chunks per thread evens out uneven blocks
    Job J = {blocks, rgba_out, width, height, bw, bh, bx, by, by / parts ? by / parts : 1, srgb};
    tex_par_for((by + J.chunk - 1) / J.chunk, decode_rows, &J);
}
