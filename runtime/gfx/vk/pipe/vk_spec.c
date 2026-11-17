// Specialization of the fixed-address constant words. The translator reads each
// constant word at a fixed address through specialization constants (SCU<k> ? SCV<k> : cb<bank>...; ids 2k, 2k + 1,
// GfxShader "spec" list). Here, per shader, the values its draws bind are watched:
// - MKW_CB_CENSUS=<file>: every 120 presents <file> is rewritten with, per shader seen, its draws and how many words
//   kept one value, changed, or could not be read (a binding outside the submission's uniform snapshot). Measure only.
// - MKW_SPEC=1 (or ab): a shader's words are read in one of every 4 of its draws until it has 32 such draws over at
//   least 120 presents; the words that kept one value then make its variant (gfx_shader_variant: the SPIR-V with those
//   constants' defaults set). From then on each draw reads only the fixed words and takes the variant when all match,
//   the generic shader otherwise; the caller also takes the generic one while the variant's pipeline is being built,
//   so no draw is skipped for it (Dolphin's hybrid ubershader mode, docs/project/research.md), and it is built from the
//   low queue (vk_pipeline_graphics_async_low). A variant missing in over
//   a quarter of its first 256 draws goes back to watching, up to 3 times (values set in the menus can change in the
//   race); the old variant stays alive, as recorded draws may still use it. MKW_SPEC_MAX caps the live variants (512):
//   RADV holds gigabytes building a large pipeline (vk_pipeline.c workers_start). Vertex and fragment stages only:
//   compute pipelines are built on the calling thread.
// A value is known only where the GPU reads exactly the bytes looked at: the program's own constants (bank 1) and
// uniform blocks served from the snapshot of the submission (vk_usnap). Anything else counts as changing.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "vk.h"
#include "platform/platform.h"

#define ENT_BUCKETS 4096

#define WATCH_EVERY 4
#define WATCH_DRAWS 32
#define WATCH_FRAMES 120
#define DROP_AFTER 256
#define REWATCH 3

enum { W_UNSEEN, W_STABLE, W_VARIES, W_UNKNOWN };
enum { E_WATCH, E_HAVE, E_NONE, E_DROPPED };

typedef struct SpecEnt {
    struct SpecEnt *next;
    const GfxShader *s;
    const uint32_t *words;
    uint32_t n;
    uint64_t draws;
    uint32_t *val;
    uint8_t *st;                  // W_*
    // MKW_SPEC
    int state;                    // E_*
    uint64_t first_frame, watched, hits, misses;
    int rewatched;
    uint32_t *fixed, nfixed;      // word indices the variant fixes
    const GfxShader *variant;
} SpecEnt;

static struct {
    int census;                   // -1 unread
    char path[512];
    PlatMutex lock;               // the table: replay thread inserts and updates, present thread writes the file
    SpecEnt *b[ENT_BUCKETS];
    uint32_t n;
    int mode;                     // MKW_SPEC + 1 (rt_knob), 0 unread
    uint32_t max, variants, dropped;
    uint64_t hits, misses;        // since the last summary
} S = {.census = -1, .lock = PLAT_MUTEX_INIT};

int vk_spec_mode(void) {
    if (!S.mode) {
        S.mode = rt_knob("SPEC", "0") + 1;
        const char *m = cfg_get("MKW_SPEC_MAX");
        S.max = m && *m ? (uint32_t)atoi(m) : 512;
        if (S.mode > 1) rt_log("vk_spec: MKW_SPEC=%s: up to %u specialized variants", S.mode == 3 ? "ab" : "1", S.max);
    }
    return S.mode - 1;
}

int vk_spec_census_on(void) {
    if (S.census >= 0) return S.census;
    const char *e = cfg_get("MKW_CB_CENSUS");
    S.census = e && *e && strcmp(e, "0");
    if (S.census) {
        snprintf(S.path, sizeof S.path, "%s", e);
        rt_log("vk_spec: MKW_CB_CENSUS: constant words per shader to %s every 120 presents", S.path);
    }
    return S.census;
}

static SpecEnt *ent_of(const GfxShader *s) {   // S.lock held
    uint32_t h = (uint32_t)(((uintptr_t)s * 0x9E3779B97F4A7C15ull) >> 40) % ENT_BUCKETS;
    for (SpecEnt *e = S.b[h]; e; e = e->next)
        if (e->s == s) return e;
    SpecEnt *e = calloc(1, sizeof *e);
    if (!e) rt_fatal("vk_spec: out of memory");
    e->s = s;
    e->n = (uint32_t)gfx_shader_spec(s, &e->words);
    e->val = calloc(e->n ? e->n : 1, sizeof *e->val);
    e->st = calloc(e->n ? e->n : 1, 1);
    if (!e->val || !e->st) rt_fatal("vk_spec: out of memory");
    e->next = S.b[h], S.b[h] = e, S.n++;
    return e;
}

static void watch(SpecEnt *e, const uint32_t *val, const uint8_t *known) {
    for (uint32_t k = 0; k < e->n; k++) {
        uint8_t *st = &e->st[k];
        if (!known[k]) *st = W_UNKNOWN;
        else if (*st == W_UNSEEN) *st = W_STABLE, e->val[k] = val[k];
        else if (*st == W_STABLE && e->val[k] != val[k]) *st = W_VARIES;
    }
}

// The words that kept one value become the variant (S.lock held).
static void decide(SpecEnt *e) {
    e->state = E_NONE;
    if (S.variants >= S.max) return;
    uint8_t *fix = calloc(e->n ? e->n : 1, 1);
    e->fixed = malloc(sizeof *e->fixed * (e->n ? e->n : 1));
    if (!fix || !e->fixed) rt_fatal("vk_spec: out of memory");
    for (uint32_t k = 0; k < e->n; k++)
        if (e->st[k] == W_STABLE) fix[k] = 1, e->fixed[e->nfixed++] = k;
    if (e->nfixed && (e->variant = gfx_shader_variant(e->s, e->val, fix))) e->state = E_HAVE, S.variants++;
    free(fix);
}

// A variant that misses too often: watch again from scratch, or give up after REWATCH tries (S.lock held).
static void rewatch(SpecEnt *e) {
    S.dropped++;
    S.variants--;   // the slot goes to another shader; the old variant's pipeline stays (recorded draws)
    if (e->rewatched++ >= REWATCH) {
        e->state = E_DROPPED;
        return;
    }
    e->state = E_WATCH, e->watched = e->hits = e->misses = 0, e->nfixed = 0, e->variant = NULL;
    free(e->fixed), e->fixed = NULL;
    memset(e->st, W_UNSEEN, e->n ? e->n : 1);
}

// Replay thread: the shader a draw of s should use. read(ctx, idx, n, val, known) gives the values this draw binds
// for words idx[0..n) (idx NULL: all of s's words).
const GfxShader *vk_spec_pick(const GfxShader *s, uint64_t frame, VkSpecRead read, void *ctx) {
    static uint32_t val[4096];
    static uint8_t known[4096];
    int mode = vk_spec_mode();
    plat_mutex_lock(&S.lock);
    SpecEnt *e = ent_of(s);
    const GfxShader *out = s;
    if (e->state == E_WATCH) {
        if (!e->watched) e->first_frame = frame;
        if (e->n > 4096) {
            e->state = E_NONE;
        } else if (e->draws++ % WATCH_EVERY == 0) {
            read(ctx, NULL, (int)e->n, val, known);
            watch(e, val, known);
            if (++e->watched >= WATCH_DRAWS && frame - e->first_frame >= WATCH_FRAMES) decide(e);
        }
    } else if (e->state == E_HAVE && RT_KNOB_ON(mode)) {
        read(ctx, e->fixed, (int)e->nfixed, val, known);
        uint32_t j = 0;
        while (j < e->nfixed && known[j] && val[j] == e->val[e->fixed[j]]) j++;
        if (j == e->nfixed) {
            e->hits++, S.hits++, out = e->variant;
        } else {
            e->misses++, S.misses++;
            if (e->hits + e->misses >= DROP_AFTER && e->misses * 4 > e->hits + e->misses) rewatch(e);
        }
    }
    plat_mutex_unlock(&S.lock);
    return out;
}

// Replay thread: the values one draw binds for s's words; known[k] 0 where word k could not be read.
void vk_spec_observe(const GfxShader *s, const uint32_t *val, const uint8_t *known) {
    plat_mutex_lock(&S.lock);
    SpecEnt *e = ent_of(s);
    e->draws++;
    watch(e, val, known);
    plat_mutex_unlock(&S.lock);
}

static void census_write(void) {
    char tmp[540];
    snprintf(tmp, sizeof tmp, "%s.tmp", S.path);
    FILE *f = fopen(tmp, "w");
    if (!f) {
        rt_log("vk_spec: cannot write %s", tmp);
        return;
    }
    fprintf(f, "key\tstage\tdraws\twords\tstable\tvaries\tunknown\tstable_words\n");
    plat_mutex_lock(&S.lock);
    for (int h = 0; h < ENT_BUCKETS; h++)
        for (SpecEnt *e = S.b[h]; e; e = e->next) {
            uint32_t c[4] = {0, 0, 0, 0};
            for (uint32_t k = 0; k < e->n; k++) c[e->st[k]]++;
            const uint8_t *key = gfx_shader_key(e->s);
            for (int j = 0; j < 32; j++) fprintf(f, "%02x", key[j]);
            fprintf(f, "\t%d\t%llu\t%u\t%u\t%u\t%u\t", gfx_shader_stage(e->s), (unsigned long long)e->draws, e->n,
                    c[W_STABLE], c[W_VARIES], c[W_UNKNOWN]);
            for (uint32_t k = 0; k < e->n; k++)
                if (e->st[k] == W_STABLE)
                    fprintf(f, "%u:%#x=%#x ", e->words[k] >> 16, e->words[k] & 0xffff, e->val[k]);
            fputc('\n', f);
        }
    plat_mutex_unlock(&S.lock);
    fclose(f);
    if (rename(tmp, S.path)) rt_log("vk_spec: cannot rename %s", tmp);
}

void vk_spec_frame(uint64_t frame) {
    if (!frame) return;
    if (frame % 120 == 0 && vk_spec_census_on()) census_write();   // a race run ends ~600 presents after it starts
    if (frame % 600 == 0 && S.mode > 1) {
        plat_mutex_lock(&S.lock);
        rt_log("vk_spec: frame %llu: %u variants of %u shaders seen, %u dropped; last 600 presents %llu draws on a "
               "variant, %llu on the generic shader after a mismatch", (unsigned long long)frame, S.variants, S.n,
               S.dropped, (unsigned long long)S.hits, (unsigned long long)S.misses);
        S.hits = S.misses = 0;
        plat_mutex_unlock(&S.lock);
    }
}
