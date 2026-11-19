// Tegra X1 block-linear <-> linear copies.
// Ported from MacNativeGames games/totk/third_party/d2r_gfx/gfx/tex/nvn_deswizzle.c @ b75be8cf (MIT, Copyright (c) 2026 jhaizhou); comments translated, the copy loop split out as tex_bl_copy.
//
// Layout (public descriptions: deko3d, Ryujinx BlockLinearLayout):
//   GOB = 64 bytes x 8 rows = 512 bytes; offset of (x byte, y row) within a GOB on the Tegra X1 =
//     (x%64/32)*256 + (y%8/2)*64 + (x%32/16)*32 + (y%2)*16 + x%16
//   Switch 2 swaps offset bits 5 and 6 inside a GOB (eden-nx2 NX2_NOTES.md rows 48 and 69, decoders.cpp
//   NativeGobOffset; a fact taken, no code): bit 5 comes from y bit 1 and bit 6 from x bit 4 =
//     (x%64/32)*256 + (y%8/4)*128 + (x%32/16)*64 + (y%4/2)*32 + (y%2)*16 + x%16
//   With the X1 order MKW's textures came out in interleaved 16-byte strips (run59's loading screen).
//   block = 1 GOB wide x 2^th GOBs high x 2^td GOBs deep, ordered y first then z inside a block; blocks fill the level
//   in x -> y -> z row-major order.
// Level and layer start offsets and the block height and depth of a level come from nvn_tex.c; this file only does the
// addressing inside a level and the copy.
#include "tex.h"
#include <string.h>

static uint32_t gob_off(uint32_t x, uint32_t y) {
    return (x % 64 / 32) * 256 + (y % 8 / 4) * 128 + (x % 32 / 16) * 64 + (y % 4 / 2) * 32 + (y % 2) * 16 + x % 16;
}

uint64_t nvn_bl_offset(uint32_t x, uint32_t y, uint32_t z, uint32_t row_bytes, uint32_t rows, unsigned th, unsigned td) {
    uint32_t bh = 1u << th, bd = 1u << td;
    uint64_t gobs_x = (row_bytes + 63) / 64, blocks_y = (rows + 8 * bh - 1) / (8 * bh);
    uint64_t block = 512ull * bh * bd;                                  // bytes in one block
    return (z / bd) * gobs_x * blocks_y * block                        // which block layer (along depth)
         + (y / (8 * bh)) * gobs_x * block                             // which block row
         + (x / 64) * block                                            // which block column
         + ((z % bd) * bh + (y % (8 * bh)) / 8) * 512ull               // which GOB within the block
         + gob_off(x, y);
}

static uint32_t mip(uint32_t s, unsigned l) { s >>= l; return s ? s : 1; }

// Geometry of one level: bytes per row of blocks, block rows, slices, where the level and layer start in the storage,
// and the level's block height and depth. -1 for textures the copy does not handle.
int nvn_deswizzle_geom(const nvn_texdesc *d, unsigned level, unsigned layer,
                       uint32_t *row_bytes, uint32_t *rows, uint32_t *slices, uint64_t *base, unsigned *th, unsigned *td, uint32_t *bpb) {
    if (d->target == 5 || d->target == 6 || d->target == 10 || nvn_tex_is_linear(d) || d->samples > 1) return -1;   // MS / BUFFER / LINEAR
    int bw, bh; *bpb = (uint32_t)nvn_fmt_bpb(d->format, &bw, &bh);
    if (!*bpb || level >= (d->levels ? d->levels : 1)) return -1;
    nvn_texlayout L; nvn_tex_layout(d, &L);
    if (layer >= L.layers) return -1;
    int one_d = d->target == 0 || d->target == 3;
    uint32_t w = mip(d->width, level), h = one_d ? 1 : mip(d->height, level);
    *slices = d->target == 2 ? mip(d->depth ? d->depth : 1, level) : 1;
    *row_bytes = (w + bw - 1) / bw * *bpb; *rows = (h + bh - 1) / bh;
    *base = (uint64_t)layer * L.layer_size + nvn_tex_level_offset(d, level);
    nvn_tex_level_tiles(d, level, th, td);
    return 0;
}

// nvn_bl_offset per row instead of per 16 bytes: within a row, GOB column k starts k blocks further, and the four
// 16-byte segments of a GOB row (x % 64 = 0, 16, 32, 48) sit at 0, 64, 256 and 320 of it (gob_off).
// Linear rows [r0, r1), row r being block row r % rows of slice r / rows.
static void bl_copy_rows(uint8_t *bl, uint8_t *lin, size_t pitch, uint32_t row_bytes, uint32_t rows, unsigned th,
                         unsigned td, int to_linear, uint32_t r0, uint32_t r1) {
    static const uint32_t seg[4] = {0, 64, 256, 320};
    uint64_t block = 512ull << th << td;
    for (uint32_t r = r0; r < r1; r++) {
        uint32_t z = r / rows, y = r % rows;
        uint8_t *row = lin + (size_t)r * pitch, *g = bl + nvn_bl_offset(0, y, z, row_bytes, rows, th, td);
        uint32_t x = 0;
        if (to_linear)
            for (; x + 64 <= row_bytes; x += 64, g += block)
                for (int s = 0; s < 4; s++) memcpy(row + x + 16 * s, g + seg[s], 16);
        else
            for (; x + 64 <= row_bytes; x += 64, g += block)
                for (int s = 0; s < 4; s++) memcpy(g + seg[s], row + x + 16 * s, 16);
        for (int s = 0; x < row_bytes; s++, x += 16) {   // a partial GOB at the row's end
            uint32_t n = row_bytes - x < 16 ? row_bytes - x : 16;
            if (to_linear) memcpy(row + x, g + seg[s], n); else memcpy(g + seg[s], row + x, n);
        }
    }
}

void tex_bl_copy(uint8_t *bl, uint8_t *lin, size_t pitch, uint32_t row_bytes, uint32_t rows, uint32_t slices,
                 unsigned th, unsigned td, int to_linear) {
    bl_copy_rows(bl, lin, pitch, row_bytes, rows, th, td, to_linear, 0, rows * slices);
}

int nvn_deswizzle_rows(const nvn_texdesc *d, unsigned level, unsigned layer, const void *src_storage, void *dst_linear,
                       size_t dst_pitch, uint32_t r0, uint32_t r1, uint32_t *n_rows) {
    uint32_t row_bytes, rows, slices, bpb; uint64_t base; unsigned th, td;
    if (nvn_tex_is_linear(d) || nvn_deswizzle_geom(d, level, layer, &row_bytes, &rows, &slices, &base, &th, &td, &bpb) ||
        dst_pitch < row_bytes)
        return -1;
    uint32_t all = rows * slices;
    if (n_rows) *n_rows = all;
    bl_copy_rows((uint8_t *)src_storage + base, dst_linear, dst_pitch, row_bytes, rows, th, td, 1, r0 < all ? r0 : all,
                 r1 < all ? r1 : all);
    return 0;
}

static int move(const nvn_texdesc *d, unsigned level, unsigned layer, uint8_t *bl, uint8_t *lin, size_t pitch, int to_linear) {
    if (nvn_tex_is_linear(d)) {
        if (level || layer || d->samples > 1 || (d->target != 0 && d->target != 1 && d->target != 7)) return -1;
        int bw, bh, bpb = nvn_fmt_bpb(d->format, &bw, &bh);
        if (!bpb) return -1;
        size_t row = (size_t)((d->width + bw - 1) / bw) * bpb;
        uint32_t rows = d->target == 0 ? 1 : (d->height + bh - 1) / bh, stride = nvn_tex_linear_stride(d);
        if (pitch < row || stride < row) return -1;
        for (uint32_t y = 0; y < rows; y++) {
            if (to_linear) memcpy(lin + y * pitch, bl + (size_t)y * stride, row);
            else memcpy(bl + (size_t)y * stride, lin + y * pitch, row);
        }
        return 0;
    }
    uint32_t row_bytes, rows, slices, bpb; uint64_t base; unsigned th, td;
    if (nvn_deswizzle_geom(d, level, layer, &row_bytes, &rows, &slices, &base, &th, &td, &bpb)) return -1;
    if (pitch < row_bytes) return -1;
    tex_bl_copy(bl + base, lin, pitch, row_bytes, rows, slices, th, td, to_linear);
    return 0;
}

uint64_t nvn_texels_box_bl;

int nvn_texels_box(const nvn_texdesc *d, unsigned level, unsigned layer, uint32_t x, uint32_t y, uint32_t z, uint32_t wb,
                   uint32_t h, void *storage, void *rows, size_t pitch, int to_linear) {
    uint8_t *st = storage, *ln = rows;
    if (nvn_tex_is_linear(d)) {
        if (level || layer || z || d->samples > 1 || (d->target != 0 && d->target != 1 && d->target != 7)) return -1;
        int bw, bh, bpb = nvn_fmt_bpb(d->format, &bw, &bh);
        if (!bpb) return -1;
        size_t row = (size_t)((d->width + bw - 1) / bw) * bpb;
        uint32_t nrows = d->target == 0 ? 1 : (d->height + bh - 1) / bh, stride = nvn_tex_linear_stride(d);
        if (stride < row || x + (size_t)wb > row || y + (uint64_t)h > nrows) return -1;
        for (uint32_t r = 0; r < h; r++) {
            uint8_t *p = st + (size_t)(y + r) * stride + x, *q = ln + r * pitch;
            if (to_linear) memcpy(q, p, wb); else memcpy(p, q, wb);
        }
        return 0;
    }
    uint32_t row_bytes, nrows, slices, bpb; uint64_t base; unsigned th, td;
    if (nvn_deswizzle_geom(d, level, layer, &row_bytes, &nrows, &slices, &base, &th, &td, &bpb)) return -1;
    if (x + (uint64_t)wb > row_bytes || y + (uint64_t)h > nrows || z >= slices) return -1;
    __atomic_fetch_add(&nvn_texels_box_bl, (uint64_t)wb * h, __ATOMIC_RELAXED);
    for (uint32_t r = 0; r < h; r++) {
        uint8_t *q = ln + r * pitch;
        for (uint32_t i = 0; i < wb;) {
            uint32_t xb = x + i, n = 16 - xb % 16;   // contiguous up to the end of its 16-byte segment
            if (n > wb - i) n = wb - i;
            uint8_t *p = st + base + nvn_bl_offset(xb, y + r, z, row_bytes, nrows, th, td);
            if (to_linear) memcpy(q + i, p, n); else memcpy(p, q + i, n);
            i += n;
        }
    }
    return 0;
}

int nvn_deswizzle(const nvn_texdesc *d, unsigned level, unsigned layer, const void *src, void *dst, size_t dst_pitch) {
    return move(d, level, layer, (uint8_t *)src, (uint8_t *)dst, dst_pitch, 1);
}
int nvn_swizzle(const nvn_texdesc *d, unsigned level, unsigned layer, const void *src, size_t src_pitch, void *dst) {
    return move(d, level, layer, (uint8_t *)dst, (uint8_t *)src, src_pitch, 0);
}
