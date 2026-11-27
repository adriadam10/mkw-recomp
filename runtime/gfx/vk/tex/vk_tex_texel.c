#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "gfx.h"
#include "host.h"
#include "nvn_api.h"
#include "nvn_hmap.h"
#include "nvn_tex.h"
#include "tex.h"
#include "vk.h"
#include "vk_exec.h"
#include "vk_tex_internal.h"
#include "platform/platform.h"

// ---------------------------------------------------------------- texel reads and writes

int region_of(const tex_t *t, const view_t *v, const NVNcopyRegion *r, uint64_t row, uint64_t img, Region *g) {
    g->bpb = nvn_fmt_bpb(t->d.format, &g->bw, &g->bh);
    if (!g->bpb || r->width <= 0 || r->height <= 0 || r->depth < 0 || r->xoffset < 0 || r->yoffset < 0 || r->zoffset < 0 ||
        r->xoffset % g->bw || r->yoffset % g->bh)
        return 0;
    g->level = v && v->set_levels ? v->base_level : 0;
    g->layer0 = v && v->set_layers ? v->min_layer : 0;
    g->nz = r->depth ? r->depth : 1;
    g->is3d = t->d.target == NVN_TEXTURE_TARGET_3D;
    g->wb = (size_t)((r->width + g->bw - 1) / g->bw) * (size_t)g->bpb;
    g->hb = (size_t)(r->height + g->bh - 1) / (size_t)g->bh;
    g->row = row ? row : g->wb;
    g->img = img ? img : g->row * g->hb;
    g->x0 = (size_t)(r->xoffset / g->bw) * (size_t)g->bpb;
    g->y0 = (size_t)(r->yoffset / g->bh);
    return 1;
}

// Pool bytes <-> caller rows of the region alone (nvn_texels_box). MacNativeGames' nvn_mtl.m goes through a
// linear copy of the whole level; for a small region of a large texture that cost tens of milliseconds while the guest
// thread held its virtual core and the caller the texture lock (monsters2 replay, frames 765-790).
int storage_texels(const tex_t *t, const Region *g, const NVNcopyRegion *r, uint8_t *mem, int write) {
    for (int z = 0; z < g->nz; z++) {
        unsigned layer = g->is3d ? 0 : g->layer0 + (unsigned)r->zoffset + (unsigned)z;
        uint32_t slice = g->is3d ? (uint32_t)(r->zoffset + z) : 0;
        if (nvn_texels_box(&t->d, g->level, layer, (uint32_t)g->x0, (uint32_t)g->y0, slice, (uint32_t)g->wb,
                           (uint32_t)g->hb, t->mem, mem + (size_t)z * g->img, g->row, !write))
            return 0;
    }
    return 1;
}

static int read_aspect(const Tex *e, VkImageAspectFlags *a) {
    if (!e->f.depth && !e->f.stencil) { *a = VK_IMAGE_ASPECT_COLOR_BIT; return 1; }
    switch (e->t.d.format) {   // packed depth-stencil storage has no single-aspect copy
    case 0x31: *a = VK_IMAGE_ASPECT_STENCIL_BIT; return 1;
    case 0x32: case 0x34: *a = VK_IMAGE_ASPECT_DEPTH_BIT; return 1;
    default: return 0;
    }
}

// The image's level (vk_tex_image.c tex_drop: image level = NVN level - drop) and the copy's extent: the staging rows
// hold whole blocks (g->wb bytes, g->hb rows of blocks), and Vulkan wants the extent in whole blocks too, or reaching
// the level's edge (VUID 00207/00208): MKW writes 1019x1023 texels of BC7 into a 1024x1024 level. 0: no such level in
// the image (-1: a level it leaves out).
static int image_region(const Tex *e, const Region *g, const NVNcopyRegion *r, Region *gd, uint32_t *w, uint32_t *h) {
    *gd = *g;
    if (e->pub.drop) {
        if (g->level < e->pub.drop) return -1;
        gd->level -= e->pub.drop;
    }
    if (gd->level >= e->pub.levels) return 0;
    *w = (uint32_t)r->width, *h = (uint32_t)r->height;
    if (!e->f.astc_bw && (g->bw > 1 || g->bh > 1)) {
        uint32_t lw = e->pub.width >> gd->level, lh = e->pub.height >> gd->level;
        uint32_t rw = (uint32_t)(g->wb / (size_t)g->bpb) * (uint32_t)g->bw, rh = (uint32_t)g->hb * (uint32_t)g->bh;
        lw = lw ? lw : 1, lh = lh ? lh : 1;
        *w = rw < lw - (uint32_t)r->xoffset ? rw : lw - (uint32_t)r->xoffset;
        *h = rh < lh - (uint32_t)r->yoffset ? rh : lh - (uint32_t)r->yoffset;
    }
    return 1;
}

int image_read_record(Tex *e, const Region *g, const NVNcopyRegion *r, TexRead *rd) {
    if (!image_read_check(e, g, r)) return 0;
    image_read_stage(e, g, r, rd);   // before the batch opens: a staging allocation may have to send it
    VkCommandBuffer cb = vk_upload_begin();
    barrier(cb, ALL_RW, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    image_read_into(e, g, r, rd, cb);
    barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
            VK_ACCESS_2_HOST_READ_BIT);
    vk_upload_end();
    return 1;
}

// Whether image_read_into can read the region (logged once per kind when not).
int image_read_check(Tex *e, const Region *g, const NVNcopyRegion *r) {
    static volatile int32_t n_astc, n_ds;
    VkImageAspectFlags aspect;
    if (e->f.astc_bw) {
        log_limited(&n_astc, "ReadTexels of ASTC texture %#llx: the image holds decoded RGBA8; zeros returned",
                    (unsigned long long)e->pub.addr);
        return 0;
    }
    if (!read_aspect(e, &aspect)) {
        log_limited(&n_ds, "texels of packed depth-stencil format %#x are not copied", e->t.d.format);
        return 0;
    }
    Region gd;
    uint32_t w, h;
    return image_region(e, g, r, &gd, &w, &h) > 0;
}

// The staging buffer of a region image_read_check accepted, into rd.
void image_read_stage(Tex *e, const Region *g, const NVNcopyRegion *r, TexRead *rd) {
    Region gd;
    uint32_t w, h;
    image_region(e, g, r, &gd, &w, &h);
    rd->rgb = e->f.expand_rgb, rd->fmt = e->t.d.format, rd->w = w;
    rd->ib = rd->rgb ? (size_t)g->bpb / 3 * 4 : 0;
    rd->slice = rd->ib ? (size_t)w * h * rd->ib : g->wb * g->hb;
    rd->sa = NULL;
    rd->stg = vk_staging(rd->slice * (size_t)g->nz, &rd->sb, &rd->sa);
}

// The copy of that region into rd's staging buffer, recorded into cb without barriers (the caller puts them around one
// or many): the upload batch (image_read_record) or a submission's end (vk_texture_eager_wb).
void image_read_into(Tex *e, const Region *g, const NVNcopyRegion *r, TexRead *rd, VkCommandBuffer cb) {
    VkImageAspectFlags aspect;
    read_aspect(e, &aspect);
    Region gd;
    uint32_t w, h;
    image_region(e, g, r, &gd, &w, &h);
    VkBufferImageCopy *rg = calloc((size_t)g->nz, sizeof *rg);
    if (!rg) rt_fatal("vk: out of memory for a texel read");
    for (int z = 0; z < g->nz; z++)
        rg[z] = (VkBufferImageCopy){.bufferOffset = rd->slice * (size_t)z,
                                    .imageSubresource = {aspect, gd.level, g->is3d ? 0 : g->layer0 + (uint32_t)r->zoffset + (uint32_t)z, 1},
                                    .imageOffset = {r->xoffset, r->yoffset, g->is3d ? r->zoffset + z : 0},
                                    .imageExtent = {w, h, 1}};
    vkCmdCopyImageToBuffer(cb, e->pub.image, VK_IMAGE_LAYOUT_GENERAL, rd->sb, (uint32_t)g->nz, rg);
    free(rg);
}

void image_read_rows(const Region *g, TexRead *rd, uint8_t *mem) {
    VkCtx *c = vk_ctx();
    if (mem) {
        vmaInvalidateAllocation(c->vma, rd->sa, 0, VK_WHOLE_SIZE);
        for (int z = 0; z < g->nz; z++) {
            const uint8_t *s = rd->stg + rd->slice * (size_t)z;
            uint8_t *m = mem + (size_t)z * g->img;
            for (size_t y = 0; y < g->hb; y++) {
                if (rd->rgb) shrink_rgb(rd->fmt, s + y * rd->w * rd->ib, m + y * g->row, rd->w);
                else memcpy(m + y * g->row, s + y * g->wb, g->wb);
            }
        }
    }
    if (rd->sa) vmaDestroyBuffer(c->vma, rd->sb, rd->sa);
    rd->sa = NULL;
}

// Image <-> caller rows through a staging buffer; a read waits for the GPU copy.
int image_texels(Tex *e, const Region *g, const NVNcopyRegion *r, uint8_t *mem, int write) {
    if (!write) {
        TexRead rd;
        if (!image_read_record(e, g, r, &rd)) return 0;
        vk_upload_flush(1);
        image_read_rows(g, &rd, mem);
        return 1;
    }
    VkImageAspectFlags aspect;
    if (!read_aspect(e, &aspect)) {
        static volatile int32_t n_ds;
        log_limited(&n_ds, "texels of packed depth-stencil format %#x are not copied", e->t.d.format);
        return 0;
    }
    Region gd;
    uint32_t w, h;
    int at = image_region(e, g, r, &gd, &w, &h);
    if (at <= 0) return at < 0;   // a write to a level the image leaves out: done
    g = &gd;
    int rgb = e->f.expand_rgb, astc = e->f.astc_bw != 0, agpu = astc && vk_astc_gpu();
    size_t texels = (size_t)w * h, ib = agpu ? 0 : astc ? 4 : rgb ? (size_t)g->bpb / 3 * 4 : 0;
    size_t slice = ib ? texels * ib : g->wb * g->hb;   // agpu: the raw blocks, decoded by vk_astc_record
    // A write goes into the open upload batch, its staging freed with it, and leaves without waiting: the next
    // submission's part follows the batch in queue order. Waiting for each write (vk_upload_flush(1)) cost ~2.3 ms
    // apiece: 19 of the 22 s mkw --replay took to load the title capture's 8 155 GPU-only textures.
    static volatile int32_t n_astc;
    VkBuffer sb;
    vk_upload_flush_over(64ull << 20);
    uint8_t *stg;
    VkCommandBuffer cb = vk_upload_begin_staging(slice * (size_t)g->nz, &sb, (void **)&stg);
    VkBufferImageCopy *rg = calloc((size_t)g->nz, sizeof *rg);
    for (int z = 0; z < g->nz; z++) {
        uint8_t *s = stg + slice * (size_t)z;
        const uint8_t *m = mem + (size_t)z * g->img;
        if (agpu) {
            for (size_t y = 0; y < g->hb; y++) memcpy(s + y * g->wb, m + y * g->row, g->wb);
        } else if (astc) {
            uint8_t *tight = malloc(g->wb * g->hb);
            for (size_t y = 0; y < g->hb; y++) memcpy(tight + y * g->wb, m + y * g->row, g->wb);
            uint64_t a0 = perf_begin();
            astc_decode_ldr_mt(tight, w, h, e->f.astc_bw, e->f.astc_bh, e->f.astc_srgb, s);
            perf_end(PERF_ASTC, a0);
            free(tight);
        } else if (rgb) {
            for (size_t y = 0; y < g->hb; y++) expand_rgb(e->t.d.format, m + y * g->row, s + y * w * ib, w);
        } else {
            for (size_t y = 0; y < g->hb; y++) memcpy(s + y * g->wb, m + y * g->row, g->wb);
        }
        rg[z] = (VkBufferImageCopy){.bufferOffset = slice * (size_t)z,
                                    .imageSubresource = {aspect, g->level, g->is3d ? 0 : g->layer0 + (uint32_t)r->zoffset + (uint32_t)z, 1},
                                    .imageOffset = {r->xoffset, r->yoffset, g->is3d ? r->zoffset + z : 0},
                                    .imageExtent = {w, h, 1}};
    }
    barrier(cb, ALL_RW, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    int done = 0;
    if (agpu) {
        VkAstcRegion *ar = calloc((size_t)g->nz, sizeof *ar);
        for (int z = 0; z < g->nz; z++)
            ar[z] = (VkAstcRegion){.src = slice * (size_t)z, .w = w, .h = h, .slices = 1, .level = rg[z].imageSubresource.mipLevel,
                                   .layer = rg[z].imageSubresource.baseArrayLayer, .offset = rg[z].imageOffset,
                                   .lw = e->pub.width >> g->level, .lh = e->pub.height >> g->level};
        uint64_t a0 = perf_begin();
        done = vk_astc_record(cb, sb, slice * (size_t)g->nz, ar, g->nz, e->f.astc_bw, e->f.astc_bh, e->f.astc_srgb,
                              e->pub.image, aspect, e->f.astc_bc7, VK_NULL_HANDLE);
        perf_end(PERF_ASTC, a0);
        free(ar);
    }
    if (!agpu) vkCmdCopyBufferToImage(cb, sb, e->pub.image, VK_IMAGE_LAYOUT_GENERAL, (uint32_t)g->nz, rg);
    else if (!done) log_limited(&n_astc, "WriteTexels of ASTC texture %#llx not decoded on the GPU: left as it was",
                                (unsigned long long)e->pub.addr);
    barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, ALL_RW);
    vk_upload_end();
    free(rg);
    return 1;
}

static void zero_rows(const Region *g, uint8_t *dst) {
    for (int z = 0; z < g->nz; z++)
        for (size_t y = 0; y < g->hb; y++) memset(dst + (size_t)z * g->img + y * g->row, 0, g->wb);
}

int vk_texture_read_image(const tex_t *t, const view_t *v, const NVNcopyRegion *r, uint8_t *dst, uint64_t row, uint64_t img) {
    Region g;
    if (!t || !region_of(t, v, r, row, img, &g)) return 0;
    lock();
    Tex *e = entry_of(t);
    int ok = 0;
    if (e) {
        make_fresh(e);
        ok = image_texels(e, &g, r, dst, 0);
    }
    unlock();
    if (!ok) zero_rows(&g, dst);
    return ok;
}

// ---------------------------------------------------------------- decoded ASTC images (runtime/gfx/cap)
// The image of an ASTC texture the device does not sample holds what its blocks were decoded into (RGBA8, or the BC7
// or BC1 encoded from that), and ReadTexels has no ASTC blocks to give back for it: it answers zeros. A capture that
// took such a texture through ReadTexels replayed it as the decoder's error colour (a race capture's course banner,
// an array filled by copies between textures and so never in guest memory: flat magenta, 15/11/2026). These hand the
// image's own bytes to the capture and put them back in the replay.
static int decoded_block(VkFormat f, uint32_t *bw, uint32_t *bpb) {
    switch (f) {
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: *bw = 1, *bpb = 4; return 1;
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK: case VK_FORMAT_BC1_RGB_SRGB_BLOCK: *bw = 4, *bpb = 8; return 1;
    case VK_FORMAT_BC7_UNORM_BLOCK: case VK_FORMAT_BC7_SRGB_BLOCK: *bw = 4, *bpb = 16; return 1;
    default: return 0;
    }
}

// Bytes of one layer (a 3D image: all its slices) of image level `level`; 0 when e is no decoded ASTC image or has no
// such level.
static uint64_t decoded_layer(const Tex *e, uint32_t level, VkExtent3D *ext) {
    uint32_t bw, bpb;
    if (!e->f.astc_bw || !e->pub.image || level >= e->pub.levels || !decoded_block(e->pub.format, &bw, &bpb)) return 0;
    uint32_t w = e->pub.width >> level, h = e->pub.height >> level,
             d = e->pub.type == VK_IMAGE_TYPE_3D ? e->pub.depth >> level : 1;
    *ext = (VkExtent3D){w ? w : 1, h ? h : 1, d ? d : 1};
    return (uint64_t)((ext->width + bw - 1) / bw) * ((ext->height + bw - 1) / bw) * bpb * ext->depth;
}

int vk_texture_decoded(const tex_t *t, VkTexDecoded *out) {
    if (!t || !t->mem) return 0;
    TexKey k;
    make_key(t, &k);
    lock();
    Tex *e = nvn_hm_get(&by_key, &k);   // the live image only: none is made for a capture
    VkExtent3D ext;
    int ok = e && !e->released && decoded_layer(e, 0, &ext);
    if (ok) {
        *out = (VkTexDecoded){.format = e->pub.format, .levels = e->pub.levels < 16 ? e->pub.levels : 16,
                              .layers = e->pub.type == VK_IMAGE_TYPE_3D ? 1 : e->pub.layers};
        for (uint32_t l = 0; l < out->levels; l++) out->bytes[l] = decoded_layer(e, l, &ext);
    }
    unlock();
    return ok;
}

int vk_texture_decoded_copy(const tex_t *t, VkFormat format, uint32_t level, uint32_t layer, uint32_t nlayers,
                            uint8_t *buf, uint64_t bytes, int write) {
    if (!t || !t->mem || !nlayers) return 0;
    VkCtx *c = vk_ctx();
    lock();
    Tex *e;
    if (write) e = entry_of(t);   // the replay: the image is made here when nothing looked the texture up yet
    else {
        TexKey k;
        make_key(t, &k);
        e = nvn_hm_get(&by_key, &k);
        if (e && e->released) e = NULL;
    }
    VkExtent3D ext;
    uint64_t one = e ? decoded_layer(e, level, &ext) : 0;
    uint32_t layers = e && e->pub.type != VK_IMAGE_TYPE_3D ? e->pub.layers : 1;
    // Another format or size than the caller's: the capture was made with another ASTC setting or on another GPU
    if (!one || e->pub.format != format || layer + nlayers > layers || one * nlayers != bytes) {
        unlock();
        return 0;
    }
    VkBuffer sb;
    VmaAllocation sa = NULL;
    uint8_t *stg;
    VkCommandBuffer cb;
    if (write) {
        vk_upload_flush_over(64ull << 20);
        cb = vk_upload_begin_staging(bytes, &sb, (void **)&stg);
        memcpy(stg, buf, bytes);
    } else {
        stg = vk_staging(bytes, &sb, &sa);
        cb = vk_upload_begin();
    }
    VkBufferImageCopy rg = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, layer, nlayers}, .imageExtent = ext};
    if (write) {
        barrier(cb, ALL_RW, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        vkCmdCopyBufferToImage(cb, sb, e->pub.image, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
        barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, ALL_RW);
    } else {
        barrier(cb, ALL_RW, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        vkCmdCopyImageToBuffer(cb, e->pub.image, VK_IMAGE_LAYOUT_GENERAL, sb, 1, &rg);
        barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
                VK_ACCESS_2_HOST_READ_BIT);
    }
    vk_upload_end();
    if (write) {
        // What the image holds now is newer than its storage, as after a GPU copy: a mirrored texture is not uploaded
        // from that storage again (the capture's storage never held these texels).
        if (e->pub.mirrored) {
            vk_mem_changed(e->pub.addr, e->t.L.size, 0, &e->wgen);
            e->uploaded = 1;
            e->pub.gpu_owned = 1;
        }
    } else {
        vk_upload_flush(1);
        vmaInvalidateAllocation(c->vma, sa, 0, VK_WHOLE_SIZE);
        memcpy(buf, stg, bytes);
        vmaDestroyBuffer(c->vma, sb, sa);
    }
    unlock();
    return 1;
}

// ReadTexels: the pool bytes are the content of a mirrored texture the GPU has not rendered into; every other texture
// is read back from its image.
void vk_read_texels(const tex_t *t, const view_t *v, const NVNcopyRegion *r, uint8_t *dst, uint64_t row, uint64_t img) {
    static volatile int32_t n_bad;
    Region g;
    if (!region_of(t, v, r, row, img, &g)) {
        log_limited(&n_bad, "ReadTexels: invalid region %d,%d,%d %dx%dx%d (format %#x)", r->xoffset, r->yoffset,
                    r->zoffset, r->width, r->height, r->depth, t->d.format);
        return;
    }
    lock();
    Tex *e = entry_of(t);
    int ok = 0;
    if (e && e->pub.mirrored && !e->pub.gpu_owned) ok = storage_texels(t, &g, r, dst, 0);
    else if (e) {
        make_fresh(e);
        ok = image_texels(e, &g, r, dst, 0);
    }
    unlock();
    if (!ok) zero_rows(&g, dst);
}

// WriteTexels: a mirrored texture gets the texels in its pool bytes (and in its image once the GPU owns it); every
// other texture in its image.
void vk_write_texels(const tex_t *t, const view_t *v, const NVNcopyRegion *r, const uint8_t *src, uint64_t row, uint64_t img) {
    static volatile int32_t n_bad;
    Region g;
    if (!region_of(t, v, r, row, img, &g) || (v && v->set_format && v->format != t->d.format)) {
        log_limited(&n_bad, "WriteTexels: invalid region %d,%d,%d %dx%dx%d or view format (format %#x)", r->xoffset,
                    r->yoffset, r->zoffset, r->width, r->height, r->depth, t->d.format);
        return;
    }
    lock();
    Tex *e = entry_of(t);
    if (tracing((uintptr_t)t->mem))
        rt_log("vk: trace %#llx: WriteTexels %d,%d %dx%d into entry %p (mirrored %d, uploaded %d, gpu-owned %d)",
               (unsigned long long)(uintptr_t)t->mem, r->xoffset, r->yoffset, r->width, r->height, (void *)e,
               e ? e->pub.mirrored : -1, e ? e->uploaded : -1, e ? e->pub.gpu_owned : -1);
    if (e && e->pub.mirrored) {
        if (!storage_texels(t, &g, r, (uint8_t *)src, 1))
            log_limited(&n_bad, "WriteTexels: region outside level %u of texture %#llx", g.level, (unsigned long long)e->pub.addr);
        vk_guest_written((uintptr_t)t->mem, t->L.size);
        if (e->pub.gpu_owned) image_texels(e, &g, r, (uint8_t *)src, 1);
        else if (e->uploaded) e->wt_n++;
    } else if (e) {
        image_texels(e, &g, r, (uint8_t *)src, 1);
    }
    unlock();
}

// A storage image may be written by the shaders of the submission being recorded (texture lock held, vk_exec_bind.c
// tex_info). Its storage is not marked GPU-owned: only the write fence for ReadTexels.
void vk_texture_storage_bound_locked(const VkTexImage *img) {
    Tex *e = (Tex *)img;   // pub is the first member
    uint64_t f = vk_exec_recording_fence();
    if (f > e->wr_fence) e->wr_fence = f;
}

uint64_t vk_texture_last_write(const tex_t *t) {
    lock();
    Tex *e = entry_of(t);
    uint64_t f = e ? e->wr_fence : 0;
    unlock();
    return f;
}
