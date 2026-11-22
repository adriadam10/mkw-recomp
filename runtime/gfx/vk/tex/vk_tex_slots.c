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

// ---------------------------------------------------------------- descriptor slots
// Slot tables under slots_lock alone: the game registers while the replay may hold tex_lock for uploads/evicts.
void vk_texture_register(int id, const tex_t *t, const view_t *v) {
    if (id < 0 || id >= (1 << 20)) rt_fatal("vk: texture slot %d out of range", id);
    lock_slots();
    if (id >= ntslots) {
        int n = ntslots ? ntslots : 1024;
        while (n <= id) n *= 2;
        TexSlot *s = realloc(tslots, sizeof *s * (size_t)n);
        if (!s) rt_fatal("vk: out of memory for texture slots");
        memset(s + ntslots, 0, sizeof *s * (size_t)(n - ntslots));
        tslots = s, ntslots = n;
    }
    if (!sinfo) {
        uint64_t *p = calloc(1 << 20, sizeof *p);
        if (!p) rt_fatal("vk: out of memory for texture slot tokens");
        __atomic_store_n(&sinfo, p, __ATOMIC_RELEASE);
        uint64_t *c = calloc(1 << 20, sizeof *c);
        if (!c) rt_fatal("vk: out of memory for texture slot checks");
        __atomic_store_n(&schk, c, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&schk[id], 0, __ATOMIC_RELEASE);
    TexSlot *s = &tslots[id];
    s->t = *t;
    s->used = 1;
    s->ce = NULL;
    __atomic_fetch_add(&slots_gen, 1, __ATOMIC_RELEASE);
    uint32_t rs = reg_serial + 1;
    if (!rs) rs = 1;
    __atomic_store_n(&reg_serial, rs, __ATOMIC_RELEASE);
    __atomic_store_n(&sinfo[id], (uint64_t)rs << 16 | agen_idx((uintptr_t)t->mem), __ATOMIC_RELEASE);
    s->has_view = v != NULL;
    if (v) s->v = *v;
    else memset(&s->v, 0, sizeof s->v);
    unlock_slots();
}

// The entry slot s resolves to, and the view and has_view a caller gets with it (the slot's, with the descriptor's
// swizzle when the entry is another descriptor at the same storage). NULL when the slot names no texture.
static Tex *slot_resolve(TexSlot *s, view_t *view, int *has_view) {
    *view = s->v;
    *has_view = s->has_view;
    TexKey k;
    make_key(&s->t, &k);
    Tex *e = nvn_hm_get(&by_key, &k);
    if (!e || e->released) {   // MNG hle_nvn.c: a finalized slot follows the live texture at its storage
        // Prefer the live entry that differs from the slot descriptor only in its swizzle: transient render targets
        // share storage, and the newest one at the address is usually another size. The map screen samples its
        // 2250x1080 map target through an (R,G,B,1) descriptor and followed a 960x60 target at the same address.
        Tex *own = e, *any = NULL;
        e = NULL;
        for (Tex *p = nvn_hm_get(&by_addr, &k.addr); p; p = p->next_at) {
            if (p->released) continue;
            if (!any) any = p;
            if (same_but_swizzle(&p->key, &k)) { e = p; break; }
        }
        if (!e) e = any ? any : own;
    }
    if (!e) e = s->t.mem ? entry_of(&s->t) : NULL;
    // A followed texture is another descriptor at the same storage: its image holds the content, but sampling takes the
    // slot descriptor's swizzle. The UI renders text into RGBA8 and samples it through a (A,A,A,A) descriptor; with the
    // render target's RGBA swizzle the text blur saw R = 1 everywhere and drew solid boxes behind tips and labels.
    if (e && !(s->has_view && s->v.set_swizzle)) {
        const nvn_texdesc *d = &s->t.d;
        uint8_t sw[4] = {(uint8_t)d->swizzle_r, (uint8_t)d->swizzle_g, (uint8_t)d->swizzle_b, (uint8_t)d->swizzle_a};
        if (!(sw[0] | sw[1] | sw[2] | sw[3]))   // descriptors built without nvnTextureBuilderSetDefaults
            sw[0] = NVN_TEXTURE_SWIZZLE_R, sw[1] = NVN_TEXTURE_SWIZZLE_G, sw[2] = NVN_TEXTURE_SWIZZLE_B,
            sw[3] = NVN_TEXTURE_SWIZZLE_A;
        if (memcmp(sw, e->pub.swz, 4)) {
            if (!s->has_view) memset(view, 0, sizeof *view);
            memcpy(view->swz, sw, 4);
            view->set_swizzle = 1;
            *has_view = 1;
        }
    }
    // ...and the slot descriptor's sRGB-ness: the cup movies are converted into an RGBA8 target and composited through
    // an RGBA8_SRGB descriptor of that storage, whose sampling decodes to linear; through the target's UNORM format the
    // movie came out with the sRGB curve applied twice (grey, live23 01/11/2026). Only the twin, which the image lists.
    if (e && !(*has_view && view->set_format)) {
        TexVkFormat f;
        if (tex_vk_format(s->t.d.format, &f) && f.vk != e->pub.format && f.vk == vk_srgb_twin(e->pub.format)) {
            if (!*has_view) memset(view, 0, sizeof *view);
            view->format = (uint16_t)s->t.d.format;
            view->set_format = 1;
            *has_view = 1;
        }
    }
    return e;
}

// Public "many locked lookups" hold: tex then slots (by_id_locked / sampler_by_id_locked need both tables).
void vk_texture_lock(void) { lock(); lock_slots(); }
void vk_texture_unlock(void) { unlock_slots(); unlock(); }
uint32_t vk_texture_slots_gen(void) { return __atomic_load_n(&slots_gen, __ATOMIC_ACQUIRE); }

uint64_t vk_texture_slot_token(int id) {
    const uint64_t *si = __atomic_load_n(&sinfo, __ATOMIC_ACQUIRE);
    if (!si || id < 0 || id >= (1 << 20)) return 0;
    uint64_t v = __atomic_load_n(&si[id], __ATOMIC_ACQUIRE);
    return v ? (v >> 16) << 32 | __atomic_load_n(&agen[v & 0xffff], __ATOMIC_ACQUIRE) : 0;
}
uint32_t vk_sampler_gen(void) { return __atomic_load_n(&samp_gen, __ATOMIC_ACQUIRE); }

const VkTexImage *vk_texture_by_id(int id, view_t *view, int *has_view) {
    vk_texture_lock();
    const VkTexImage *r = vk_texture_by_id_locked(id, view, has_view);
    vk_texture_unlock();
    return r;
}

// The NVN format and address the game registered in slot id (the dump compares them with the image the slot resolves
// to: a slot may resolve to an image another texture at that address made, in its own format). 0 if not registered.
uint32_t vk_texture_slot_format(int id, uint64_t *addr) {
    lock_slots();
    uint32_t f = id >= 0 && id < ntslots && tslots[id].used ? tslots[id].t.d.format : 0;
    if (addr) *addr = f ? (uint64_t)(uintptr_t)tslots[id].t.mem : 0;
    unlock_slots();
    return f;
}

// A sampled slot that resolves to no image (the draw binds a zero stand-in): the first 32 slot ids, with why. run106's
// final composite read four such slots and came out black.
static void unresolved_note(int id, const TexSlot *s) {
    static int seen[32], n;
    for (int i = 0; i < n; i++) if (seen[i] == id) return;
    if (n == 32) return;
    seen[n++] = id;
    if (!s) { rt_log("vk: texture slot %d sampled but never registered: zero stand-in", id); return; }
    const nvn_texdesc *d = &s->t.d;
    rt_log("vk: texture slot %d (addr %#llx fmt %#x target %d %ux%ux%u levels %d flags %#x) has no image: zero stand-in",
           id, (unsigned long long)(uintptr_t)s->t.mem, d->format, d->target, d->width, d->height, d->depth, d->levels, d->flags);
}

// MKW_REG_LATE=1 (a measurement, with MKW_REG_QUEUED off): the replay resolving a slot that the game registered again
// after the submission being recorded was made. Registrations apply at once on the game's thread while the replay
// records a few submits behind: such a lookup gets what the game put there for a later frame (vk_backend.c reg_queued;
// Hookshot's "MKW shadows in bands for a frame"; candados-lectura, the kart's shadow). One line a frame with the count
// and the first slots.
uint32_t vk_tex_reg_limit;   // replay thread: the last registration made before the submission being recorded; 0 none

uint32_t vk_texture_reg_serial(void) { return __atomic_load_n(&reg_serial, __ATOMIC_ACQUIRE); }

static void late_note(int id, const TexSlot *s) {
    static struct { uint64_t frame; uint32_t n; int k; char first[160]; } L;
    uint64_t f = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    if (f != L.frame) {
        if (L.n) rt_log("tex: f%llu %u slot lookups saw a registration made after their submission:%s",
                        (unsigned long long)L.frame, L.n, L.first);
        L.frame = f, L.n = 0, L.k = 0, L.first[0] = 0;
    }
    if (++L.n <= 4 && L.k < (int)sizeof L.first - 40)
        L.k += snprintf(L.first + L.k, sizeof L.first - (size_t)L.k, " %d=%ux%ux%u/%#x", id, s->t.d.width, s->t.d.height,
                        s->t.d.depth, s->t.d.format);
}

// Caller holds tex_lock then slots_lock (vk_texture_lock): slot tables and by_key / by_addr / make_fresh.
const VkTexImage *vk_texture_by_id_locked(int id, view_t *view, int *has_view) {
    if (id < 0 || id >= ntslots || !tslots[id].used) { unresolved_note(id, NULL); return NULL; }
    TexSlot *s = &tslots[id];
    if (vk_tex_reg_limit && (int32_t)((uint32_t)(sinfo[id] >> 16) - vk_tex_reg_limit) > 0) late_note(id, s);
    uint32_t g = agen[sinfo[id] & 0xffff];
    if (!s->ce || s->cgen != g) {   // resolved again: an entry at its address was created, dropped or released, or the
        view_t v;                   // slot registered (s->ce cleared); an empty result is not kept: image creation may
        int hv;                     // succeed later
        s->ce = slot_resolve(s, &v, &hv);   // entry_of may create an entry and bump g: g read before it
        s->cview = v, s->chas_view = (uint8_t)hv;
        s->cgen = g;
        s->cepoch = 0;
    }
    Tex *e = s->ce;
    // Bindings of one submit share one check, answered from the submit's write-watch snapshot (guest writes during a
    // replay race the GPU read on hardware too). After one check an entry is either uploaded or never uploads (not
    // mirrored, GPU-owned or no CPU layout), so a slot that checked its entry in this submit has nothing left to do.
    if (e && s->cepoch != submit_epoch) {
        if (!e->uploaded || e->checked != submit_epoch) {
            make_fresh_snap(e);
            e->checked = submit_epoch;
        }
        s->cepoch = submit_epoch;
        __atomic_store_n(&schk[id], (uint64_t)submit_epoch << 32 | s->cgen, __ATOMIC_RELEASE);
    }
    if (view) *view = has_view ? s->cview : s->v;
    if (has_view) *has_view = view ? s->chas_view : s->has_view;
    if (e) e->seen = submit_epoch;
    if (!e) unresolved_note(id, s);
    else if (vk_guard_on()) {
        vk_guard_note(&e->use);
        if (vk_guard_test() == GUARD_TEST_FREE) vk_guard_test_free(e->use);
    }
    return e ? &e->pub : NULL;
}

// Whether slot id had its freshness check in submit epoch ep (vk_texture_epoch) on an entry still current, so that
// vk_texture_by_id_locked would only repeat it: the chunks of MKW_REPLAY_PAR skip the texture lock for the first binding
// of a slot another chunk already checked (each thread's descriptor memo is its own). Unlocked; a registration since
// clears it, and an entry created, dropped or released at the slot's address moves its agen.
int vk_texture_checked(int id, uint32_t ep) {
    uint64_t *c = __atomic_load_n(&schk, __ATOMIC_ACQUIRE);
    if (!c || id < 0 || id >= (1 << 20)) return 0;
    uint64_t v = __atomic_load_n(&c[id], __ATOMIC_ACQUIRE);
    if (!v || (uint32_t)(v >> 32) != ep) return 0;
    uint64_t si = __atomic_load_n(&sinfo[id], __ATOMIC_ACQUIRE);
    return (uint32_t)v == __atomic_load_n(&agen[si & 0xffff], __ATOMIC_ACQUIRE);
}

// vk_dump: the swizzle selectors of the descriptor registered in slot id; 0 when the slot is empty.
int vk_texture_slot_swz(int id, uint8_t swz[4]) {
    lock_slots();
    int ok = id >= 0 && id < ntslots && tslots[id].used;
    if (ok) {
        const nvn_texdesc *d = &tslots[id].t.d;
        swz[0] = (uint8_t)d->swizzle_r, swz[1] = (uint8_t)d->swizzle_g, swz[2] = (uint8_t)d->swizzle_b;
        swz[3] = (uint8_t)d->swizzle_a;
    }
    unlock_slots();
    return ok;
}

uint32_t vk_texture_epoch(void) { return __atomic_load_n(&submit_epoch, __ATOMIC_RELAXED); }

void vk_texture_new_submit(void) {
    lock();
    if (!++submit_epoch) submit_epoch = 1;
    static uint64_t log_at;
    uint64_t f = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) / 60;
    if ((rt_trace & RT_TRACE_PERF) && f != log_at && tex_alias > 0) {
        log_at = f;
        rt_log("perf: frame %llu: texture aliases: content copied %llu, filled from storage %llu, left uninitialised %llu",
               (unsigned long long)f * 60, (unsigned long long)n_alias_copied, (unsigned long long)n_alias_upload,
               (unsigned long long)n_alias_uninit);
        n_alias_copied = n_alias_upload = n_alias_uninit = 0;
        rt_log("perf: frame %llu: texture re-uploads %llu (%.1f MiB, %.2f ms), after WriteTexels %llu (%.1f MiB, %.2f ms)",
               (unsigned long long)f * 60, (unsigned long long)ru_n, ru_bytes / 1048576.0, ru_ns / 1e6,
               (unsigned long long)ru_wt_n, ru_wt_bytes / 1048576.0, ru_wt_ns / 1e6);
        ru_n = ru_bytes = ru_ns = ru_wt_n = ru_wt_bytes = ru_wt_ns = 0;
        uint64_t ck, calls, untr;
        vk_mem_snap_stats(&ck, &calls, &untr);
        rt_log("perf: frame %llu: binding storage checks %llu (texture %llu), write-watch snapshot queries %llu, untracked %llu",
               (unsigned long long)f * 60, (unsigned long long)ck, (unsigned long long)n_checks, (unsigned long long)calls,
               (unsigned long long)untr);
        n_checks = 0;
    }
    unlock();
}

// ---------------------------------------------------------------- samplers (LAS textures.c)
static VkCompareOp compare_op(uint32_t f) {
    switch (f) {
    case NVN_COMPARE_FUNC_NEVER: return VK_COMPARE_OP_NEVER;
    case NVN_COMPARE_FUNC_LESS: return VK_COMPARE_OP_LESS;
    case NVN_COMPARE_FUNC_EQUAL: return VK_COMPARE_OP_EQUAL;
    case NVN_COMPARE_FUNC_LEQUAL: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case NVN_COMPARE_FUNC_GREATER: return VK_COMPARE_OP_GREATER;
    case NVN_COMPARE_FUNC_NOTEQUAL: return VK_COMPARE_OP_NOT_EQUAL;
    case NVN_COMPARE_FUNC_GEQUAL: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    default: return VK_COMPARE_OP_ALWAYS;
    }
}

static struct { int init, mirror_clamp_edge, minmax, border_no_format; } sf;

static void sampler_features(VkCtx *c) {
    if (sf.init) return;
    VkPhysicalDeviceCustomBorderColorFeaturesEXT cbf = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT};
    VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                            .pNext = c->has_custom_border_color ? &cbf : NULL};
    VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f12};
    vkGetPhysicalDeviceFeatures2(c->phys, &f2);
    sf.mirror_clamp_edge = f12.samplerMirrorClampToEdge;
    sf.minmax = f12.samplerFilterMinmax;
    sf.border_no_format = c->has_custom_border_color && cbf.customBorderColors && cbf.customBorderColorWithoutFormat;
    sf.init = 1;
}

static VkSampler make_sampler(VkCtx *c, const samp_par *d) {
    sampler_features(c);
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = d->magf == NVN_MAG_FILTER_LINEAR ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    uint32_t mf = d->minf;
    si.minFilter = mf == 1 || mf == 3 || mf == 5 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    si.mipmapMode = mf == 4 || mf == 5 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    VkSamplerAddressMode modes[3];
    const uint8_t wrap[3] = {d->ws, d->wt, d->wr};
    int border = 0;
    for (int i = 0; i < 3; i++) {
        switch (wrap[i]) {
        case NVN_WRAP_MODE_CLAMP: case NVN_WRAP_MODE_CLAMP_TO_EDGE: modes[i] = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; break;
        case NVN_WRAP_MODE_MIRROR_CLAMP: case NVN_WRAP_MODE_MIRROR_CLAMP_TO_EDGE:
            modes[i] = sf.mirror_clamp_edge ? VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
            break;
        case NVN_WRAP_MODE_MIRROR_CLAMP_TO_BORDER: case NVN_WRAP_MODE_CLAMP_TO_BORDER:
            modes[i] = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
            border = 1;
            break;
        case NVN_WRAP_MODE_MIRRORED_REPEAT: modes[i] = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT; break;
        default: modes[i] = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        }
    }
    si.addressModeU = modes[0], si.addressModeV = modes[1], si.addressModeW = modes[2];
    si.minLod = d->lod_min;
    si.maxLod = d->lod_max > 1000 ? VK_LOD_CLAMP_NONE : d->lod_max;
    if (mf <= NVN_MIN_FILTER_LINEAR) si.maxLod = si.minLod + 0.25f;   // not mipmapped: stay on the base level
    if (si.maxLod < si.minLod) si.maxLod = si.minLod;
    float lim = c->props.limits.maxSamplerLodBias;
    si.mipLodBias = fmaxf(-lim, fminf(lim, d->lod_bias));
    // MKW_ANISO=N (Graphics tab; "game" by default): at least N-times anisotropic
    // filtering on mipmapped samplers with linear filtering, on top of what the game asks.
    float aniso = d->aniso;
    static float aniso_min = -1;
    if (aniso_min < 0) aniso_min = (float)atof(cfg_value("MKW_ANISO"));   // "game": 0
    if (mf >= NVN_MIN_FILTER_NEAREST_MIPMAP_NEAREST && si.minFilter == VK_FILTER_LINEAR && aniso < aniso_min) aniso = aniso_min;
    if (aniso > 1) {
        si.anisotropyEnable = VK_TRUE;
        si.maxAnisotropy = fminf(aniso, c->props.limits.maxSamplerAnisotropy);
    }
    if (d->cmp_mode) {
        si.compareEnable = VK_TRUE;
        si.compareOp = compare_op(d->cmp_func);
    }
    VkSamplerCustomBorderColorCreateInfoEXT cb = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT};
    VkSamplerReductionModeCreateInfo rm = {.sType = VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO};
    const void **tail = &si.pNext;
    if (border) {
        const float *b = d->border;
        if (b[0] == 0 && b[1] == 0 && b[2] == 0 && b[3] == 0) si.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        else if (b[0] == 0 && b[1] == 0 && b[2] == 0 && b[3] == 1) si.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
        else if (b[0] == 1 && b[1] == 1 && b[2] == 1 && b[3] == 1) si.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        else if (sf.border_no_format) {
            memcpy(cb.customBorderColor.float32, b, 16);
            si.borderColor = VK_BORDER_COLOR_FLOAT_CUSTOM_EXT;
            *tail = &cb;
            tail = &cb.pNext;
        } else si.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    }
    if (d->reduction && sf.minmax && !si.compareEnable) {
        rm.reductionMode = d->reduction == NVN_SAMPLER_REDUCTION_MIN ? VK_SAMPLER_REDUCTION_MODE_MIN : VK_SAMPLER_REDUCTION_MODE_MAX;
        *tail = &rm;
    }
    VkSampler s;
    VkResult r = vkCreateSampler(c->device, &si, NULL, &s);
    if (r != VK_SUCCESS) rt_fatal("vk: vkCreateSampler failed (%d)", (int)r);
    return s;
}

void vk_sampler_register(int id, const samp_par *s) {
    if (id < 0 || id >= (1 << 12)) rt_fatal("vk: sampler slot %d out of range", id);
    lock_slots();
    if (id >= nsslots) {
        int n = nsslots ? nsslots : 256;
        while (n <= id) n *= 2;
        SampSlot *p = realloc(sslots, sizeof *p * (size_t)n);
        if (!p) rt_fatal("vk: out of memory for sampler slots");
        memset(p + nsslots, 0, sizeof *p * (size_t)(n - nsslots));
        sslots = p, nsslots = n;
    }
    sslots[id].p = *s;
    sslots[id].s = sslots[id].sp = VK_NULL_HANDLE;
    sslots[id].used = 1;
    __atomic_fetch_add(&samp_gen, 1, __ATOMIC_RELEASE);
    unlock_slots();
}

// Caller holds slots_lock (samplers hashmap and sslots cache).
static VkSampler sampler_for(VkCtx *c, const samp_par *s) {
    samp_par k;
    memset(&k, 0, sizeof k);
    k = *s;
    VkSampler v = (VkSampler)nvn_hm_get(&samplers, &k);
    if (!v) {
        v = make_sampler(c, &k);
        nvn_hm_put(&samplers, &k, (void *)v);
    }
    return v;
}

VkSampler vk_sampler_for(const samp_par *s) {
    VkCtx *c = vk_ctx();
    if (!c || !s) return VK_NULL_HANDLE;
    lock_slots();
    VkSampler v = sampler_for(c, s);
    unlock_slots();
    return v;
}

// Samplers live until shutdown, so a slot keeps the one its parameters map to until it is registered again.
// Caller holds slots_lock (vk_texture_lock holds tex then slots).
VkSampler vk_sampler_by_id_locked(int id) {
    VkCtx *c = vk_ctx();
    if (!c) return VK_NULL_HANDLE;
    VkSampler v = VK_NULL_HANDLE;
    if (id >= 0 && id < nsslots && sslots[id].used) {
        SampSlot *s = &sslots[id];
        if (!s->s) s->s = sampler_for(c, &s->p);
        v = s->s;
    }
    return v;
}

VkSampler vk_sampler_point_by_id_locked(int id) {
    VkCtx *c = vk_ctx();
    if (!c || id < 0 || id >= nsslots || !sslots[id].used) return VK_NULL_HANDLE;
    SampSlot *s = &sslots[id];
    if (!s->sp) {
        samp_par k = s->p;
        k.magf = NVN_MAG_FILTER_NEAREST;
        k.minf = k.minf == NVN_MIN_FILTER_LINEAR ? NVN_MIN_FILTER_NEAREST :
                 k.minf > NVN_MIN_FILTER_LINEAR ? NVN_MIN_FILTER_NEAREST_MIPMAP_NEAREST : k.minf;
        k.aniso = 1;
        s->sp = sampler_for(c, &k);
    }
    return s->sp;
}

VkSampler vk_sampler_by_id(int id) {
    lock_slots();
    VkSampler v = vk_sampler_by_id_locked(id);
    unlock_slots();
    return v;
}

int vk_sampler_params(int id, samp_par *out) {
    lock_slots();
    int ok = id >= 0 && id < nsslots && sslots[id].used;
    if (ok) *out = sslots[id].p;
    unlock_slots();
    return ok;
}

