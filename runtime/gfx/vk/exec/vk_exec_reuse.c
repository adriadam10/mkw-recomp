// MKW_REUSE=1|check|ab: a draw whose replay state equals the state of a
// draw recorded in one of the last submissions gets that draw's packets again (vk_rec_reuse.c) instead of being
// replayed. MKW draws a race frame like the one two frames before (87.5 % of the draws, period 2: recruns.py), and
// replaying a draw costs ~3.3 us of the replay thread (ceil*, medidas.tsv).
// - Key: the program, the draw's payload, the fixed-function state, targets and pass, the occlusion query state. Each
//   key may hold several entries (the frame alternates two sets of buffers), told apart by their inputs: a hash of
//   the vertex buffers, the storage blocks of the stages the draw binds (c0 holds them all) and the texture handles of
//   the slots it samples. The uniform blocks are not inputs: their sets are bound again from this submission's copy.
//   Bound state left by earlier draws in the bound arrays is not part of either (it made every key differ, ru3).
// - A captured draw starts from nothing bound (as after a split): its packets set everything it needs.
// - A hit also needs: the bound Vulkan state its packets did not set to be what it was (vk_rec_reuse.c slots), every
//   memory range it resolved to resolve to the same buffer and offset, every texture slot to name the same entry, the
//   samplers unchanged. Its descriptor sets are bound again (ru_bind_set) with the uniform blocks, c0 and support
//   buffers made for this submission; the texture slots get their once-per-submission check.
// - Not cached: draws whose packets begin or end a pass, hold a barrier or a query (vk_rec_reuse.c allowed), or whose
//   replay copied guest memory into scratch, writes memory, reads a storage address from a uniform block, binds a
//   storage image or has a set without an update template (ru_note_bad).
// - check: a hit is replayed as usual as well, and its packets compared with the cached ones. store (diagnosis): every
//   draw is captured and stored, none reused: what capturing costs the misses.
// Entries live in an arena per submission and move to the current one when hit; those not hit for RU_GENS
// submissions go.
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "tex.h"
#include "platform/platform.h"
#include "vk.h"
#include "vk_draw_internal.h"
#include "vk_rec_reuse.h"
#include "vk_reuse.h"

#define RU_GENS 12          // submissions an entry stays without a hit (a race frame sends ~4)
#define RU_TAB (1u << 15)
#define RU_MEM 48
#define RU_TEXN 64
#define RU_SETN 96          // descriptors of a set

int ru_on = -1;
_Thread_local RuCap *ru_cap;

typedef struct { uint64_t addr, size, off; VkBuffer buf; } RuMem;
// A texture slot as the draw found it: its token, and what it resolved to (entry and view) for when the game registers
// the slot again each frame with the same texture (ru14: 546 misses on the token in one frame).
typedef struct { uint64_t h, tok, vh; VkImage image; const VkTexImage *ti; } RuTex;
typedef struct {
    const VkPipeInfo *info;
    uint32_t s, n, i0, i1;
    VkDescSlot ds[RU_SETN];
    uint8_t kind[RU_SETN], kst[RU_SETN], kslot[RU_SETN];
} RuSet;

// An entry: this header, then mem[nmem], tex[ntex], dep slots (uint8_t[ndep] padded to 8) and dep hashes, the sets
// (each a RuSetH, its n descriptors and its kind, kst and kslot bytes padded to 8), the window's words and relocations.
typedef struct RuEnt {
    uint64_t key, inputs, dgen;   // dgen: vk_destroy_gen when stored (a handle in its sets destroyed since: unusable)
    uint32_t gen, size, fb_after, fbl_read, sgen, c0_binst, set_bytes, vmask;
    uint16_t nmem, ntex, ndep, nset, nwords, nreloc, npk, ntd, npair;
    uint8_t td[RU_TEXN][2];
} RuEnt;
typedef struct { const VkPipeInfo *info; uint32_t s, n, i0, i1; RuSetMemo m; } RuSetH;
static size_t set_bytes(uint32_t n) { return sizeof(RuSetH) + sizeof(VkDescSlot) * n + ((3 * (size_t)n + 7) & ~(size_t)7); }

struct RuCap {
    uint64_t key;
    int bad;
    uint32_t nmem, ntex, nset;
    RuMem mem[RU_MEM];
    RuTex tex[RU_TEXN];
    uint8_t dkind[1024], dst[1024], dslot[1024];   // per descriptor of the draw's packed array (MAX_DESC)
    uint32_t ntd;                                  // inputs it read (td: kind << 4 | stage, slot)
    const VkPipeInfo *info;                        // its pipeline's (the vertex inputs it reads)
    uint64_t dgen;
    uint32_t fb_before, fbl_read;                  // ex->fb_dirty before the draw; attachments it samples
    uint8_t td[RU_TEXN][2];
    struct RuEnt *check;                           // check: the hit this replay is compared with
    RuSet set[2];
    RecWin win;
};
static RuCap CAPX;
static int why_on = -1;   // MKW_REUSE_WHY (key diagnosis below)
static const VkTexImage *tex_resolve(uint64_t h, uint64_t *vh);
static void kset_end(void);

// Each submission's entries: blocks of 1 MiB (an entry is ~1 KiB, a race frame stores ~3000).
#define RU_BLOCK (1u << 20)
static struct {
    uint8_t **blk;
    int nblk, cblk, cur;
    size_t used;
} arena[RU_GENS];
static struct { uint64_t key; RuEnt *e; uint32_t gen; } *tab;
static uint32_t cur_gen;
static struct { uint64_t tsc[2], lap[5], vlap[2], klap, hits, armed, stored, bad[8], nocap, miss_none, miss_inputs, miss_destroy, miss_dep, miss_mem, miss_tex, miss_samp, miss_set,
                check_ok, check_bad, check_hash, win_why[64], slap, alap, sets, sets_memo; } RS2;

static uint64_t mix(uint64_t h, uint64_t w) { return (h ^ w) * 0xBF58476D1CE4E5B9ull ^ ((h ^ w) >> 31); }

static uint64_t hbytes(uint64_t h, const void *v, size_t n) {
    const uint8_t *p = v;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        memcpy(&w, p + i, 8);
        h = mix(h, w);
    }
    if (i < n) {
        uint64_t w = 0;
        memcpy(&w, p + i, n - i);
        h = mix(h, w);
    }
    return h;
}


static void ru_store_end(VkExec *ex, RuCap *c, int ok);

static int mode_read(void) {
    if (ru_on >= 0) return ru_on;
    const char *v = cfg_get("MKW_REUSE");
    ru_on = !v ? 0 : !strcmp(v, "check") ? 2 : !strcmp(v, "ab") ? 3 : !strcmp(v, "store") ? 4 : atoi(v) ? 1 : 0;
    const char *par = cfg_get("MKW_REPLAY_PAR");
    if (ru_on && !vk_pipeline_draw_cache()) {
        rt_log("vk_reuse: MKW_REUSE needs MKW_DRAW_CACHE=1 (its sets outlive the submission): off");
        ru_on = 0;
    } else if (ru_on && (vk_mirror_on || (par && strcmp(par, "0")))) {   // not handled yet: mirrors' per-use sync, chunks
        rt_log("vk_reuse: not with VRAM mirrors or MKW_REPLAY_PAR yet: off");
        ru_on = 0;
    }
    if (ru_on) {
        tab = calloc(RU_TAB, sizeof *tab);
        if (!tab) rt_fatal("vk_reuse: out of memory");
        rec_slots_on = 1;
        rt_log("vk_reuse: draws equal to an earlier submission's reuse its packets%s",
               ru_on == 2 ? " (check: replayed as well and compared)" : ru_on == 3 ? " in the B blocks (MKW_AB)" : "");
    }
    return ru_on;
}

void ru_submission(VkExec *ex) {
    if (!mode_read()) return;
    if (why_on > 0) kset_end();
    cur_gen++;
    arena[cur_gen % RU_GENS].used = 0, arena[cur_gen % RU_GENS].cur = 0;   // RU_GENS submissions old: slots read stale
}

static int stale(uint32_t gen) { return cur_gen - gen >= RU_GENS - 1; }

static void *arena_alloc(size_t n) {
    typeof(arena[0]) *a = &arena[cur_gen % RU_GENS];
    n = (n + 15) & ~(size_t)15;
    if (n > RU_BLOCK) return NULL;
    if (!a->nblk || a->used + n > RU_BLOCK) {
        if (a->nblk && a->cur + 1 < a->nblk) a->cur++;
        else {
            if (a->nblk == a->cblk && !(a->blk = realloc(a->blk, sizeof *a->blk * (size_t)(a->cblk = a->cblk ? 2 * a->cblk : 8))))
                rt_fatal("vk_reuse: out of memory");
            if (!(a->blk[a->nblk] = malloc(RU_BLOCK))) rt_fatal("vk_reuse: out of memory");
            a->cur = a->nblk++;
        }
        a->used = 0;
    }
    void *p = a->blk[a->cur] + a->used;
    a->used += n;
    return p;
}



// The targets by what they mean: the game builds its texture views on its stack and leaves their padding as it was
// (view_t.pad1), and p_set_rt has tail padding (ru6: the targets differed in 1864 of 1867 new keys).
static uint64_t view_hash(uint64_t h, const view_t *v) {   // what cmd_copy_view keeps (captures hold the raw view)
    uint64_t f = (uint64_t)v->set_levels | (uint64_t)v->set_layers << 1 | (uint64_t)v->set_format << 2 |
                 (uint64_t)v->set_target << 3 | (uint64_t)v->set_swizzle << 4 | (uint64_t)v->set_dsmode << 5;
    h = mix(h, f);
    if (v->set_levels) h = mix(h, (uint64_t)v->base_level << 8 | v->nlevels);
    if (v->set_layers) h = mix(h, (uint64_t)v->min_layer << 16 | v->nlayers);
    if (v->set_format) h = mix(h, v->format);
    if (v->set_target) h = mix(h, v->target);
    if (v->set_swizzle) h = mix(h, (uint64_t)v->swz[0] | (uint64_t)v->swz[1] << 8 | (uint64_t)v->swz[2] << 16 | (uint64_t)v->swz[3] << 24);
    if (v->set_dsmode) h = mix(h, v->dsmode);
    return h;
}
// A target by its storage and description: a texture initialized again (a new serial, another pinned copy) over the
// same memory is the same image.
static uint64_t tex_id(uint64_t h, const tex_t *t) {
    if (!t) return mix(h, 0);
    nvn_texdesc d = t->d;
    d.swizzle_pad = 0;
    return hbytes(mix(h, (uint64_t)(uintptr_t)t->mem), &d, sizeof d);
}
static uint64_t rt_hash(const p_set_rt *r) {
    uint64_t h = mix(0, (uint64_t)(uint32_t)r->n);
    for (int i = 0; i < 8; i++) {
        h = tex_id(h, r->color[i]);
        h = r->has_cview[i] ? view_hash(mix(h, 1), &r->cview[i]) : mix(h, 0);
    }
    h = tex_id(h, r->depth);
    return r->has_dview ? view_hash(mix(h, 1), &r->dview) : mix(h, 0);
}

// The pipeline inputs with the state objects by their fields: the game's NVN state objects keep bits the replay does not
// define (the words around the bitfields), which differed every frame (ru10: the state part repeated for 52-54 % of the
// draws against 95-97 % for the rest of the key).
static VkPipeState pipe_norm(const VkPipeState *p) {
    VkPipeState s = *p;
    memset(&s.color, 0, sizeof s.color), memset(&s.poly, 0, sizeof s.poly), memset(&s.ds, 0, sizeof s.ds);
    s.color.blend_en = p->color.blend_en, s.color.logic_op = p->color.logic_op, s.color.alpha_func = p->color.alpha_func;
    s.poly.cull = p->poly.cull, s.poly.front = p->poly.front, s.poly.mode = p->poly.mode, s.poly.offset_en = p->poly.offset_en;
    s.ds.dt = p->ds.dt, s.ds.dw = p->ds.dw, s.ds.df = p->ds.df, s.ds.st = p->ds.st, s.ds.sf_f = p->ds.sf_f;
    s.ds.sf_b = p->ds.sf_b, s.ds.fail_f = p->ds.fail_f, s.ds.zfail_f = p->ds.zfail_f, s.ds.zpass_f = p->ds.zpass_f;
    s.ds.fail_b = p->ds.fail_b, s.ds.zfail_b = p->ds.zfail_b, s.ds.zpass_b = p->ds.zpass_b;
    for (int i = 0; i < 8; i++) {
        const blend_st *b = &p->blend[i];
        memset(&s.blend[i], 0, sizeof s.blend[i]);
        s.blend[i].target = b->target, s.blend[i].src_rgb = b->src_rgb, s.blend[i].dst_rgb = b->dst_rgb;
        s.blend[i].src_a = b->src_a, s.blend[i].dst_a = b->dst_a, s.blend[i].eq_rgb = b->eq_rgb, s.blend[i].eq_a = b->eq_a;
    }
    return s;
}

// The attachments written since the pass began are not in the key: they follow where the part was cut, which goes by
// time (MKW_SPLIT_US: 29 % of the draws repeated this part two frames later, ru11). They matter to a draw only through
// a feedback barrier, which a cached window never holds: a hit needs none of the attachments it samples written now
// (RuEnt.fbl_read).
static uint32_t fb_key(const VkExec *ex) { (void)ex; return 0; }

// The targets' and fixed state's part of the key, kept with the raw bytes it was made from: consecutive draws mostly
// share them, and comparing the bytes costs less than hashing them (ru21: key 1 100-1 900 ticks a draw).
#define FIX_BYTES (offsetof(VkExec, pipe) + sizeof(VkPipeState) - offsetof(VkExec, vp))
static struct { int valid; p_set_rt rt; uint8_t fix[FIX_BYTES]; uint64_t h; } KM;

static uint64_t fixed_key(const VkExec *ex) {
    if (KM.valid && !memcmp(KM.fix, ex->vp, FIX_BYTES) && !memcmp(&KM.rt, &ex->rt, sizeof ex->rt)) return KM.h;
    // The game's targets and fixed state, pools and pipeline inputs; the resolved targets and pass sizes follow from
    // them (and hold struct padding: every key differed, ru4).
    uint64_t h = rt_hash(&ex->rt);
    h = hbytes(h, ex->vp, offsetof(VkExec, pipe) - offsetof(VkExec, vp));
    VkPipeState ps = pipe_norm(&ex->pipe);
    h = hbytes(h, &ps, sizeof ps);
    memcpy(KM.fix, ex->vp, FIX_BYTES), KM.rt = ex->rt, KM.h = h, KM.valid = 1;
    return h;
}

static uint64_t draw_key(const VkExec *ex, uint32_t op, const void *payload, size_t bytes) {
    uint64_t h = mix(op, ex->prog ? ex->prog->serial : 0);   // the serial: a program freed and another at its address
    h = mix(h, fixed_key(ex));
    h = mix(h, (uint64_t)(uint32_t)ex->prog_stages);
    // The last conditional-rendering op stays in the state but moves every frame (recdiff: RENDER_COND 0 % equal); it
    // only matters while drawing is disabled, and then nothing is reused (active).
    h = mix(mix(h, (uint64_t)ex->in_pass | (uint64_t)ex->fbl_mask << 8 | (uint64_t)fb_key(ex) << 32), (uint64_t)ex->nfbl);
    h = hbytes(h, ex->fbl_img, sizeof ex->fbl_img[0] * (size_t)ex->nfbl);
    h = mix(h, (uint64_t)vk_exec_oq_state());
    return hbytes(h, payload, bytes) | 1;
}

// MKW_REUSE_WHY=N (diagnosis): for the first N draws per submission whose key is new but whose program and payload an
// earlier draw had, which parts of the key differ from that draw's.
#define WHY_N 12
static const char *const why_name[WHY_N] = {"rt", "viewports", "scissor..blend", "copy stride", "pools", "pipe formats",
                                             "pipe blend", "pipe state", "pipe vertex", "pass", "query", "prog stages"};
static void key_parts(const VkExec *ex, uint64_t p[WHY_N]) {
    p[0] = rt_hash(&ex->rt);
    p[1] = hbytes(0, ex->vp, sizeof ex->vp + sizeof ex->has_vp);
    p[2] = hbytes(0, &ex->scissor, offsetof(VkExec, copy_stride) - offsetof(VkExec, scissor));
    p[3] = (uint64_t)ex->copy_stride;
    p[4] = hbytes(0, &ex->tex_pool, 2 * sizeof ex->tex_pool);
    VkPipeState ps = pipe_norm(&ex->pipe);
    p[5] = hbytes(0, ps.color_fmt, offsetof(VkPipeState, blend) - offsetof(VkPipeState, color_fmt));
    p[6] = hbytes(0, ps.blend, sizeof ps.blend);
    p[7] = hbytes(0, &ps.color, offsetof(VkPipeState, vattr) - offsetof(VkPipeState, color));
    p[8] = hbytes(0, ex->pipe.vattr, sizeof ex->pipe.vattr + sizeof ex->pipe.vstream);
    p[9] = (uint64_t)ex->in_pass | (uint64_t)ex->fbl_mask << 8 | (uint64_t)fb_key(ex) << 32 | (uint64_t)ex->nfbl << 48;
    p[10] = (uint64_t)vk_exec_oq_state();
    p[11] = (uint64_t)(uint32_t)ex->prog_stages;
}
static struct { uint64_t pk; uint64_t parts[WHY_N]; p_set_rt rt; } *why_tab;
static uint64_t why_cnt[WHY_N], why_seen;
static void why_note(const VkExec *ex, const void *payload, size_t bytes, int miss) {
    if (why_on < 0) {
        why_on = cfg_get("MKW_REUSE_WHY") ? atoi(cfg_get("MKW_REUSE_WHY")) : 0;
        if (why_on && !(why_tab = calloc(1u << 16, sizeof *why_tab))) rt_fatal("vk_reuse: out of memory");
    }
    if (!why_on) return;
    uint64_t pk = hbytes(ex->prog ? ex->prog->serial : 0, payload, bytes) | 1, parts[WHY_N];
    key_parts(ex, parts);
    typeof(*why_tab) *w = &why_tab[(pk >> 20) & 0xffff];
    if (miss && w->pk == pk) {
        why_seen++;
        for (int i = 0; i < WHY_N; i++) why_cnt[i] += parts[i] != w->parts[i];
        static int nlog;
        if (parts[0] != w->parts[0] && nlog++ < 6) {
            const p_set_rt *a = &w->rt, *b = &ex->rt;
            rt_log("vk_reuse why: targets n %d/%d color0 %p/%p depth %p/%p cview0 %d/%d dview %d/%d", a->n, b->n,
                   (const void *)a->color[0], (const void *)b->color[0], (const void *)a->depth, (const void *)b->depth,
                   a->has_cview[0], b->has_cview[0], a->has_dview, b->has_dview);
            for (int i = 0; i < 8; i++)
                if (a->color[i] != b->color[i] || a->has_cview[i] != b->has_cview[i] ||
                    (a->has_cview[i] && memcmp(&a->cview[i], &b->cview[i], 14)))
                    rt_log("vk_reuse why:  color %d differs (%p/%p)", i, (const void *)a->color[i], (const void *)b->color[i]);
        }
    }
    w->pk = pk, memcpy(w->parts, parts, sizeof parts), w->rt = ex->rt;
}

// With MKW_REUSE_WHY: the share of a large submission's keys present among those of the large submission one and two
// before (key stability, against what the cache finds).
#define KP (WHY_N + 2)   // the key, program and payload alone, and program and payload with each part
static uint64_t *kset[KP][3];
static int kn[3], kcap;
static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}
static void kset_add(const uint64_t *k) {
    if (kn[0] == kcap) {
        kcap = kcap ? 2 * kcap : 8192;
        for (int p = 0; p < KP; p++)
            for (int i = 0; i < 3; i++)
                if (!(kset[p][i] = realloc(kset[p][i], 8 * (size_t)kcap))) rt_fatal("vk_reuse: out of memory");
    }
    for (int p = 0; p < KP; p++) kset[p][0][kn[0]] = k[p];
    kn[0]++;
}
static void kset_end(void) {
    if (kn[0] < 1000) { kn[0] = 0; return; }
    char buf[700];
    int len = 0;
    for (int p = 0; p < KP; p++) {
        qsort(kset[p][0], (size_t)kn[0], 8, cmp_u64);
        int in2 = 0;
        for (int i = 0; i < kn[0]; i++) in2 += kn[2] && bsearch(&kset[p][0][i], kset[p][2], (size_t)kn[2], 8, cmp_u64) != NULL;
        len += snprintf(buf + len, sizeof buf - (size_t)len, " %s %.1f%%",
                        p == 0 ? "key" : p == 1 ? "prog+payload" : why_name[p - 2], 100.0 * in2 / kn[0]);
        uint64_t *t = kset[p][2];
        kset[p][2] = kset[p][1], kset[p][1] = kset[p][0], kset[p][0] = t;
    }
    rt_log("perf: reuse: of %d draws, keys also two large submissions before, by part:%s", kn[0], buf);
    kn[2] = kn[1], kn[1] = kn[0], kn[0] = 0;
}

// What the draw read of the bound arrays: the vertex streams its attributes take (vmask), and per td entry a texture
// handle (kind 0), a storage block (1) or a uniform block read as storage (2). Slots it did not read stay out: earlier
// draws leave them different every frame (ru13: 1999 misses on inputs with the whole vertex and storage tables).
static uint64_t inputs_of(const VkExec *ex, uint32_t vmask, const uint8_t (*td)[2], uint32_t ntd) {
    uint64_t h = mix(0, vmask);
    for (int b = 0; b < 16; b++)
        if (vmask >> b & 1) h = hbytes(h, &ex->vbuf[b], sizeof ex->vbuf[b]);
    for (uint32_t i = 0; i < ntd; i++) {
        int kind = td[i][0] >> 4, st = td[i][0] & 15, slot = td[i][1];
        if (kind == 0) h = mix(h, ex->tex[st][slot]);
        else {
            const p_bind_buf *b = kind == 1 ? &ex->ssbo[st][slot] : &ex->ubo[st][slot];
            h = mix(mix(h, b->addr), b->size);
        }
    }
    return h;
}


static uint32_t slot_of(uint64_t key) { return (uint32_t)(key >> 17) & (RU_TAB - 1); }
#define RU_RUN 64   // table slots looked at from a key's

// The entry of key whose inputs match ex's. *any: some entry of key was there.
static RuEnt *lookup(const VkExec *ex, uint64_t key, int *any) {
    *any = 0;
    for (uint32_t k = slot_of(key), n = 0; n < RU_RUN; k = (k + 1) & (RU_TAB - 1), n++) {
        if (!tab[k].key) return NULL;
        if (tab[k].key != key || stale(tab[k].gen)) continue;
        RuEnt *e = tab[k].e;
        *any = 1;
        if (!(e->fbl_read & ex->fb_dirty) && inputs_of(ex, e->vmask, (const uint8_t (*)[2])e->td, e->ntd) == e->inputs)
            return e;
    }
    return NULL;
}

// e goes in for its key: over an entry with the same key and inputs, else into a free or stale slot.
static void put(RuEnt *e) {
    uint32_t k = slot_of(e->key), free_k = UINT32_MAX;
    for (uint32_t n = 0; n < RU_RUN; k = (k + 1) & (RU_TAB - 1), n++) {
        if (tab[k].key == e->key && !stale(tab[k].gen) && tab[k].e->inputs == e->inputs) { free_k = k; break; }
        if (!tab[k].key) { if (free_k == UINT32_MAX) free_k = k; break; }
        if (free_k == UINT32_MAX && stale(tab[k].gen)) free_k = k;
    }
    if (free_k == UINT32_MAX) return;   // a full run: not cached
    tab[free_k].key = e->key, tab[free_k].e = e, tab[free_k].gen = cur_gen;
}

// The entry's parts.
#define E_MEM(e) ((RuMem *)((e) + 1))
#define E_TEX(e) ((RuTex *)(E_MEM(e) + (e)->nmem))
#define E_DSL(e) ((uint8_t *)(E_TEX(e) + (e)->ntex))
#define E_DH(e) ((uint64_t *)(E_DSL(e) + (((e)->ndep + 7u) & ~7u)))
#define E_SET(e) ((uint8_t *)(E_DH(e) + (e)->ndep))
#define E_WORDS(e) ((uint64_t *)(E_SET(e) + (e)->set_bytes))
#define E_PH(e) (E_WORDS(e) + (e)->nwords)
#define E_RELOC(e) ((uint16_t *)(E_PH(e) + (e)->npair))
#define E_PKP(e) (E_RELOC(e) + (e)->nreloc)
#define E_PSL(e) ((uint8_t *)(E_PKP(e) + (e)->npk + 1))

static int active(const VkExec *ex) {
    if (mode_read() == 0 || (ru_on == 3 && !rt_ab()) || !ex->in_pass || ex->render_disabled || !rec_owned(ex->cb)) return 0;
    if (vk_dump_armed || vk_spec_mode() || vk_spec_census_on() || (vk_pt_enabled() && vk_pt_draw_mode())) return 0;
    return 1;
}

// The deps of a captured window: the slots it read without setting.
static int deps_of(const RecWin *w, uint8_t *sl, uint64_t *h) {
    int n = 0;
    for (int s = 0; s < REC_SLOTS; s++)
        if (!(w->written[s >> 6] >> (s & 63) & 1) && w->before[s]) sl[n] = (uint8_t)s, h[n] = w->before[s], n++;
    return n;
}

static void store(const VkExec *ex, RuCap *c) {
    uint8_t dsl[REC_SLOTS];
    uint64_t dh[REC_SLOTS];
    int ndep = deps_of(&c->win, dsl, dh);
    size_t sb = 0;
    for (uint32_t i = 0; i < c->nset; i++) sb += set_bytes(c->set[i].n);
    size_t size = sizeof(RuEnt) + sizeof(RuMem) * c->nmem + sizeof(RuTex) * c->ntex + (((size_t)ndep + 7) & ~(size_t)7) +
                  8 * (size_t)ndep + sb + 8 * (size_t)c->win.nwords + 2 * (size_t)c->win.nreloc +
                  9 * (size_t)c->win.npair + 2 * ((size_t)c->win.npk + 1);
    RuEnt *e = arena_alloc(size);
    if (!e) return;
    *e = (RuEnt){.key = c->key, .gen = cur_gen, .size = (uint32_t)size, .fb_after = ex->fb_dirty & ~c->fb_before, .fbl_read = c->fbl_read, .sgen = vk_sampler_gen(),
                 .c0_binst = c0_binst, .set_bytes = (uint32_t)sb, .nmem = (uint16_t)c->nmem, .ntex = (uint16_t)c->ntex, .ndep = (uint16_t)ndep,
                 .nset = (uint16_t)c->nset, .nwords = (uint16_t)c->win.nwords, .nreloc = (uint16_t)c->win.nreloc,
                 .npk = (uint16_t)c->win.npk, .npair = (uint16_t)c->win.npair, .vmask = vk_exec_streams(ex, c->info), .ntd = (uint16_t)c->ntd};
    memcpy(e->td, c->td, sizeof c->td[0] * c->ntd);
    e->inputs = inputs_of(ex, e->vmask, (const uint8_t (*)[2])c->td, c->ntd);
    e->dgen = c->dgen;
    memcpy(E_MEM(e), c->mem, sizeof(RuMem) * c->nmem);
    for (uint32_t i = 0; i < c->ntex; i++) c->tex[i].ti = tex_resolve(c->tex[i].h, &c->tex[i].vh);
    memcpy(E_TEX(e), c->tex, sizeof(RuTex) * c->ntex);
    memcpy(E_DSL(e), dsl, (size_t)ndep);
    memcpy(E_DH(e), dh, 8 * (size_t)ndep);
    uint8_t *at = E_SET(e);
    for (uint32_t i = 0; i < c->nset; i++) {
        const RuSet *r = &c->set[i];
        *(RuSetH *)at = (RuSetH){r->info, r->s, r->n, r->i0, r->i1, {0}};
        VkDescSlot *ds = (VkDescSlot *)(at + sizeof(RuSetH));
        memcpy(ds, r->ds, sizeof *ds * r->n);
        uint8_t *k = (uint8_t *)(ds + r->n);
        memcpy(k, r->kind, r->n), memcpy(k + r->n, r->kst, r->n), memcpy(k + 2 * r->n, r->kslot, r->n);
        at += set_bytes(r->n);
    }
    memcpy(E_WORDS(e), c->win.words, 8 * (size_t)c->win.nwords);
    memcpy(E_RELOC(e), c->win.reloc, 2 * (size_t)c->win.nreloc);
    memcpy(E_PH(e), c->win.ph, 8 * (size_t)c->win.npair);
    memcpy(E_PKP(e), c->win.pk_pair, 2 * ((size_t)c->win.npk + 1));
    memcpy(E_PSL(e), c->win.psl, c->win.npair);
    put(e);
    RS2.stored++;
}

static const VkTexImage *tex_resolve(uint64_t h, uint64_t *vh) {
    view_t v;
    int hv = 0;
    vk_texture_lock();
    const VkTexImage *ti = h ? vk_texture_by_id_locked((int)(h & 0xfffff), &v, &hv) : NULL;
    vk_texture_unlock();
    *vh = hv ? view_hash(1, &v) : 0;
    return ti;
}

static int tex_same(const RuTex *t) {
    uint64_t vh;
    const VkTexImage *ti = tex_resolve(t->h, &vh);
    return ti && ti == t->ti && vh == t->vh;
}

// What a hit must find unchanged; side effects of looking (a resolve, a token) are ones the replay would have had too.
static int still_valid(const RuEnt *e) {
    // Its descriptors name views, samplers and buffers that the draw cache's handle set holds; a destroyed one moves
    // vk_destroy_gen (vk_exec_dcache.c). Without this a hit bound a destroyed view (SIGSEGV in RADV, rab1-1).
    if (e->dgen != __atomic_load_n(&vk_destroy_gen, __ATOMIC_RELAXED)) return RS2.miss_destroy++, 0;
    if (!rec_slots_match(E_DSL(e), E_DH(e), e->ndep)) return RS2.miss_dep++, 0;
    const RuMem *m = E_MEM(e);
    for (int i = 0; i < e->nmem; i++) {
        VkMemSpan sp;
        if (!vk_mem_resolve(m[i].addr, m[i].size, &sp) || sp.buffer != m[i].buf || sp.offset != m[i].off)
            return RS2.miss_mem++, 0;
    }
    const RuTex *t = E_TEX(e);
    for (int i = 0; i < e->ntex; i++)
        if (token_of(t[i].h) != t[i].tok && !tex_same(&t[i])) return RS2.miss_tex++, 0;
    if (e->ntex && vk_sampler_gen() != e->sgen) return RS2.miss_samp++, 0;
    return 1;
}

// Per texture slot id, the submission epoch (vk_texture_epoch) whose storage check a hit already made: the check is
// once per slot and submission (vk_texture_by_id_locked), and a hit made it again under the texture lock for every
// slot of every reused draw (ru21: 1 900 of the hit's 9 000 ticks).
static uint32_t tex_ep[1u << 20];

// The hit: texture checks, the window up to the draw, the sets, the draw.
static int replay_hit(VkExec *ex, RuEnt *e) {
    uint64_t c = plat_cycles();
#define LAP(i) (RS2.lap[i] += plat_cycles() - c, c = plat_cycles())
    const RuTex *t = E_TEX(e);
    if (e->ntex) {
        uint32_t ep = vk_texture_epoch();
        int locked = 0;
        for (int i = 0; i < e->ntex; i++) {
            uint32_t id = (uint32_t)(t[i].h & 0xfffff);
            if (!t[i].h || tex_ep[id] == ep) continue;
            if (!locked) vk_texture_lock(), locked = 1;
            vk_texture_by_id_locked((int)id, NULL, NULL);
            tex_ep[id] = ep;
        }
        if (locked) vk_texture_unlock();
        for (int i = 0; i < e->ntex; i++) vk_exec_dc_read(t[i].image);
    }
    LAP(0);
    RecWin *w = &CAPX.win;   // the emitter reads a RecWin: words and relocations only
    memcpy(w->words, E_WORDS(e), 8 * (size_t)e->nwords);
    memcpy(w->reloc, E_RELOC(e), 2 * (size_t)e->nreloc);
    memcpy(w->ph, E_PH(e), 8 * (size_t)e->npair);
    memcpy(w->pk_pair, E_PKP(e), 2 * ((size_t)e->npk + 1));
    memcpy(w->psl, E_PSL(e), e->npair);
    w->nwords = e->nwords, w->nreloc = e->nreloc, w->npk = e->npk, w->npair = e->npair;
    LAP(1);
    rec_win_emit(ex->cb, w, 0, e->npk - 1);
    LAP(2);
    vk_exec_draw_forget();   // the window's pipeline may disturb the sets bound: dc_bind must not skip one as bound
    uint8_t *at = E_SET(e);
    for (int i = 0; i < e->nset; i++) {
        RuSetH *h = (RuSetH *)at;
        VkDescSlot ds[RU_SETN];
        memcpy(ds, at + sizeof *h, sizeof ds[0] * h->n);
        const uint8_t *k = at + sizeof *h + sizeof ds[0] * h->n;
        int r = ru_bind_set(ex, h->info, h->s, ds, h->n, h->i0, h->i1, k, k + h->n, k + 2 * h->n, e->c0_binst, &h->m);
        if (!r) rt_fatal("vk_reuse: a set of a reused draw could not be bound again after its window was emitted");
        RS2.sets++, RS2.sets_memo += r == 2;
        at += set_bytes(h->n);
    }
    LAP(3);
    rec_win_emit(ex->cb, w, e->npk - 1, e->npk);
    ex->recorded += e->npk + e->nset;
    ex->fb_dirty |= e->fb_after;   // the attachments the draw writes
    ex->dirty = VK_EXEC_DIRTY_ALL;
    vk_exec_draw_forget();   // the replay's memory of what is bound no longer holds
    vk_exec_frame.draws++;
    LAP(4);
#undef LAP
    return 1;
}

// The sets need this submission's uniform-block copies: all must be found before anything is emitted.
static int sets_possible(VkExec *ex, const RuEnt *e) {
    const uint8_t *at = E_SET(e);
    for (int i = 0; i < e->nset; i++) {
        const RuSetH *h = (const RuSetH *)at;
        const uint8_t *k = at + sizeof *h + sizeof(VkDescSlot) * h->n;
        for (uint32_t j = 0; j < h->n; j++)
            if (k[j] == RUK_UBO && !ru_ubo_ok(ex, k[h->n + j], k[2 * h->n + j])) return RS2.miss_set++, 0;
        at += set_bytes(h->n);
    }
    return 1;
}

int ru_draw(VkExec *ex, uint32_t op, const void *payload) {
    if (!active(ex)) return 0;
    size_t bytes = op == OP_DRAW ? sizeof(p_draw) : sizeof(p_draw_idx);
    uint64_t k0 = plat_cycles();
    uint64_t key = draw_key(ex, op, payload, bytes);
    int any;
    RuEnt *e = lookup(ex, key, &any);
    RS2.klap += plat_cycles() - k0;
    CAPX.check = NULL;
    uint64_t t0 = plat_cycles();
    int valid = e && still_valid(e);
    RS2.vlap[0] += plat_cycles() - t0;
    valid = valid && ru_on != 4 && sets_possible(ex, e);
    RS2.tsc[0] += plat_cycles() - t0;
    if (valid) {
        if (ru_on == 2) {   // check: replay it as usual below and compare
            CAPX.check = e;
        } else {
            RuEnt *copy = e->gen == cur_gen ? NULL : arena_alloc(e->size);
            if (copy) memcpy(copy, e, e->size), copy->gen = cur_gen, put(copy), e = copy;
            RS2.hits++;
            t0 = plat_cycles();
            int r = replay_hit(ex, e);
            RS2.tsc[1] += plat_cycles() - t0;
            return r;
        }
    } else if (!e) {
        if (any) RS2.miss_inputs++;
        else RS2.miss_none++;
    }
    why_note(ex, payload, bytes, !any && !e);
    if (why_on > 0) {
        uint64_t k[KP], parts[WHY_N];
        key_parts(ex, parts);
        k[0] = key, k[1] = hbytes(mix(op, ex->prog ? ex->prog->serial : 0), payload, bytes);
        for (int i = 0; i < WHY_N; i++) k[2 + i] = mix(k[1], parts[i]);
        kset_add(k);
    }
    CAPX.key = key;
    CAPX.bad = 0, CAPX.nmem = CAPX.ntex = CAPX.nset = CAPX.ntd = 0, CAPX.info = NULL;
    CAPX.fb_before = ex->fb_dirty, CAPX.fbl_read = 0;
    // Only uniform, c0 and support descriptors are noted: the rest must read stable, not an earlier draw's kind (a
    // texture rebound as a uniform block: SIGSEGV in RADV, rab1-1, ru17).
    memset(CAPX.dkind, RUK_STABLE, sizeof CAPX.dkind);
    CAPX.dgen = __atomic_load_n(&vk_destroy_gen, __ATOMIC_RELAXED);   // before its sets are made
    // From nothing bound: the window then holds every bound state the draw uses (the replay re-records it all).
    vk_exec_draw_forget();
    ex->dirty = VK_EXEC_DIRTY_ALL;
    if (!rec_cap_begin(ex->cb, &CAPX.win)) return RS2.nocap++, 0;
    ru_cap = &CAPX;
    RS2.armed++;
    RS2.alap += plat_cycles() - k0;
    return 0;
}

static void check_against(const RuEnt *e, const RecWin *w) {
    int same = e->nwords == w->nwords && e->nreloc == w->nreloc && !memcmp(E_WORDS(e), w->words, 8 * (size_t)w->nwords);
    if (same) { RS2.check_ok++; return; }
    if (RS2.check_bad++ < 20) {
        uint32_t i = 0;
        while (i < w->nwords && i < e->nwords && E_WORDS(e)[i] == w->words[i]) i++;
        uint32_t at = 0, op = 0, pw = 0;   // the packet holding word i
        while (at < w->nwords && at + (uint32_t)(w->words[at] >> 32) <= i) at += (uint32_t)(w->words[at] >> 32);
        if (at < w->nwords) op = (uint32_t)(w->words[at] & 0xffff), pw = i - at;
        rt_log("vk_reuse check: a hit's packets differ from its replay: %u/%u words, first difference at word %u "
               "(%s word %u; cached %#llx, replayed %#llx)", e->nwords, w->nwords, i, rec_op_name(op), pw,
               i < e->nwords ? (unsigned long long)E_WORDS(e)[i] : 0ull, i < w->nwords ? (unsigned long long)w->words[i] : 0ull);
    }
}

void ru_draw_end(VkExec *ex) {
    RuCap *c = ru_cap;
    if (!c) return;
    ru_cap = NULL;
    uint64_t t0 = plat_cycles();
    int ok = rec_cap_end();
    ru_store_end(ex, c, ok);
    RS2.slap += plat_cycles() - t0;
}

static void ru_store_end(VkExec *ex, RuCap *c, int ok) {
    if (c->check) {
        if (ok) check_against(c->check, &c->win);
        return;
    }
    if (c->bad) return (void)RS2.bad[c->bad & 7]++;
    if (!ok) return (void)RS2.bad[0]++, (void)RS2.win_why[rec_cap_why() & 63]++;
    store(ex, c);
}

// ---- notes of the draw being captured

void ru_note_mem(uint64_t addr, uint64_t size, const VkMemSpan *sp, int ok) {
    RuCap *c = ru_cap;
    if (!ok) return ru_note_bad(RUB_TOO_MANY);   // an unresolved range: the replay skips or stands in, not cached
    if (c->nmem >= RU_MEM) return ru_note_bad(RUB_TOO_MANY);
    c->mem[c->nmem++] = (RuMem){addr, size, sp->offset, sp->buffer};
}

void ru_note_tex(uint64_t h, VkImage image, int storage) {
    RuCap *c = ru_cap;
    if (storage) return ru_note_bad(RUB_STORAGE_IMG);
    if (c->ntex >= RU_TEXN) return ru_note_bad(RUB_TOO_MANY);
    c->tex[c->ntex++] = (RuTex){h, token_of(h), 0, image, NULL};   // ti and vh at store (tex_resolve)
}

void ru_note_bad(int why) {
    if (ru_cap && !ru_cap->bad) ru_cap->bad = why;
}

void ru_note_fbl(uint32_t fbl) { ru_cap->fbl_read = fbl; }

void ru_note_slot(int kind, int st, int slot) {
    RuCap *c = ru_cap;
    if (c->ntd >= RU_TEXN) return ru_note_bad(RUB_TOO_MANY);
    c->td[c->ntd][0] = (uint8_t)(kind << 4 | (st & 15)), c->td[c->ntd][1] = (uint8_t)slot, c->ntd++;
}

void ru_note_desc(uint32_t at, int kind, int st, int slot) {
    if (at >= 1024) return ru_note_bad(RUB_TOO_MANY);
    ru_cap->dkind[at] = (uint8_t)kind, ru_cap->dst[at] = (uint8_t)st, ru_cap->dslot[at] = (uint8_t)slot;
}

void ru_note_set(const VkPipeInfo *info, uint32_t s, const VkDescSlot *ds, uint32_t n, uint32_t at, uint32_t i0, uint32_t i1) {
    RuCap *c = ru_cap;
    if (!info->tpl[s]) return ru_note_bad(RUB_NO_TPL);
    if (c->nset >= 2 || n > RU_SETN) return ru_note_bad(RUB_TOO_MANY);
    if (info->bind_point == VK_PIPELINE_BIND_POINT_GRAPHICS) c->info = info;
    RuSet *r = &c->set[c->nset++];
    r->info = info, r->s = s, r->n = n, r->i0 = i0, r->i1 = i1;
    memcpy(r->ds, ds, sizeof *ds * n);
    memcpy(r->kind, c->dkind + at, n), memcpy(r->kst, c->dst + at, n), memcpy(r->kslot, c->dslot + at, n);
}

void ru_log(void) {
    if (ru_on <= 0 || !(rt_trace & RT_TRACE_PERF)) return;
    uint64_t d = RS2.hits + RS2.armed;
    if (!d) return;
    rt_log("perf: reuse: %llu draws, %.1f %% from the cache, %llu stored; misses: no key %llu, other inputs %llu, destroyed %llu, bound state %llu, memory "
           "%llu, texture %llu, samplers %llu, uniform copy %llu, no ring %llu; not cached: window %llu, scratch copy %llu, "
           "written %llu, uniform-read address %llu, storage image %llu, scratch index %llu, no template %llu, too many %llu%s",
           (unsigned long long)d, 100.0 * (double)RS2.hits / (double)d, (unsigned long long)RS2.stored,
           (unsigned long long)RS2.miss_none, (unsigned long long)RS2.miss_inputs, (unsigned long long)RS2.miss_destroy, (unsigned long long)RS2.miss_dep, (unsigned long long)RS2.miss_mem,
           (unsigned long long)RS2.miss_tex, (unsigned long long)RS2.miss_samp, (unsigned long long)RS2.miss_set,
           (unsigned long long)RS2.nocap, (unsigned long long)RS2.bad[0], (unsigned long long)RS2.bad[RUB_VOLATILE],
           (unsigned long long)RS2.bad[RUB_WRITTEN], (unsigned long long)RS2.bad[RUB_UBO_PTR],
           (unsigned long long)RS2.bad[RUB_STORAGE_IMG], (unsigned long long)RS2.bad[RUB_SCRATCH_INDEX],
           (unsigned long long)RS2.bad[RUB_NO_TPL], (unsigned long long)RS2.bad[RUB_TOO_MANY], "");
    if (RS2.hits)
        rt_log("perf: reuse: per hit %.0f ticks to check, %.0f to emit and bind (per draw looked up %.0f)",
               (double)RS2.tsc[0] / (double)(RS2.hits + RS2.armed), (double)RS2.tsc[1] / (double)RS2.hits,
               (double)RS2.tsc[0] / (double)(RS2.hits + RS2.armed));
    rt_log("perf: reuse: per draw looked up, ticks: key and lookup %.0f, validity %.0f; per capture: lookup to armed %.0f, "
           "end and store %.0f", (double)RS2.klap / (double)d, (double)RS2.vlap[0] / (double)d,
           RS2.armed ? (double)RS2.alap / (double)RS2.armed : 0.0, RS2.armed ? (double)RS2.slap / (double)RS2.armed : 0.0);
    if (RS2.hits)
        rt_log("perf: reuse: per hit, ticks: textures %.0f, copy %.0f, window %.0f, sets %.0f (%.1f %% of them bound "
               "again as the last hit bound them), draw %.0f",
               (double)RS2.lap[0] / RS2.hits, (double)RS2.lap[1] / RS2.hits, (double)RS2.lap[2] / RS2.hits,
               (double)RS2.lap[3] / RS2.hits, RS2.sets ? 100.0 * (double)RS2.sets_memo / (double)RS2.sets : 0.0,
               (double)RS2.lap[4] / RS2.hits);
    char why[512];
    int k = 0;
    for (int i = 0; i < 64; i++)
        if (RS2.win_why[i] && k < (int)sizeof why - 40)
            k += snprintf(why + k, sizeof why - (size_t)k, " %s %llu",
                          i == 1 ? "new-part" : i == 2 ? "fallback" : i < 5 ? "arrays" : i == 5 ? "pairs" : i == 63 ? "size" : i == 62 ? "last"
                                                                                          : rec_op_name((uint32_t)i),
                          (unsigned long long)RS2.win_why[i]);
    if (k) rt_log("perf: reuse: windows not kept by cause:%s", why);
    if (why_seen) {
        k = 0;
        for (int i = 0; i < WHY_N; i++)
            k += snprintf(why + k, sizeof why - (size_t)k, " %s %llu", why_name[i], (unsigned long long)why_cnt[i]);
        rt_log("perf: reuse: of %llu new keys with a known program and payload, parts that differ:%s",
               (unsigned long long)why_seen, why);
        why_seen = 0, memset(why_cnt, 0, sizeof why_cnt);
    }
    if (ru_on == 2)
        rt_log("perf: reuse check: %llu hits replayed equal, %llu differ, %llu hash drifts", (unsigned long long)RS2.check_ok,
               (unsigned long long)RS2.check_bad, (unsigned long long)RS2.check_hash);
    memset(&RS2, 0, sizeof RS2);
}
