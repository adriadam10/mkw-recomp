// Shaders of MKW: SPIR-V prepared before the game runs (tools/shaders/translate.py), one <key>.spv and <key>.res.json
// per program under <exe dir>/spirv, key = nvn2_program_key(control, code). A program is loaded once and kept for the
// life of the process in a memory cache by that key, which is also the shader's id in a pipeline key record
// (vk_pipeline.c). Vertex shaders that read integer vertex formats get a variant with integer inputs (gfx_shader_int.c:
// int_input_variant), fragment shaders that write integer targets one with integer outputs (int_output_variant).
//
// Reflection: every shader that enters the cache goes through gfx_shader_reflect (gfx_shader_refl.c); the gfx.h
// accessors are at the end of the file.
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "nvn_api.h"
#include "gfx_shader_internal.h"
#include "host.h"
#include "platform/platform.h"

#define BUCKETS 4096


static PlatMutex cache_lock = PLAT_MUTEX_INIT;                  // memory cache
static GfxShader *buckets[BUCKETS];
static volatile int64_t n_keys;                            // memory cache entries

#define read_file plat_file_read

// ---- cache

GfxShader *gfx_shader_find(const uint8_t key[32]) {
    plat_mutex_lock_shared(&cache_lock);
    GfxShader *s = buckets[(key[0] | key[1] << 8) % BUCKETS];
    while (s && memcmp(s->key, key, 32)) s = s->next;
    plat_mutex_unlock_shared(&cache_lock);
    return s;
}

static void shader_free(GfxShader *s) {
    for (int k = 0; k < GFX_RES_KINDS; k++) free(s->res[k]);
    free(s->spirv), free(s->reflection), free(s->spec), free(s);
}


// Inserts s, or frees it and returns the entry another thread inserted first.
GfxShader *gfx_shader_insert(GfxShader *s) {
    plat_mutex_lock(&cache_lock);
    GfxShader **head = &buckets[(s->key[0] | s->key[1] << 8) % BUCKETS], *r = *head;
    while (r && memcmp(r->key, s->key, 32)) r = r->next;
    if (r) {
        shader_free(s);
        s = r;
    } else {
        s->next = *head;
        *head = s;
        n_keys++;
    }
    plat_mutex_unlock(&cache_lock);
    return s;
}

void gfx_shader_key_hex(const uint8_t key[32], char out[65]) {
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) out[i * 2] = hex[key[i] >> 4], out[i * 2 + 1] = hex[key[i] & 15];
    out[64] = 0;
}

// A .res.json the translator wrote: one JSON object that names its stage, no NUL inside. A file an interrupted write
// left zero-filled (3647 of 21924 in one cache were) fails this; gfx_shader_reflect() would take it for a vertex shader with
// no resources, and its pipeline layout would lack the shader's buffers and textures.
static int reflection_valid(const char *res, uint64_t size) {
    if (strlen(res) != size) return 0;
    const char *b = res, *e = res + size;
    while (b < e && isspace((unsigned char)*b)) b++;
    while (e > b && isspace((unsigned char)e[-1])) e--;
    if (e - b < 2 || *b != '{' || e[-1] != '}') return 0;
    const char *st = strstr(b, "\"stage\"");
    if (!st) return 0;
    st = strchr(st + 7, '"');
    return st && (!strncmp(st, "\"vs\"", 4) || !strncmp(st, "\"ps\"", 4) || !strncmp(st, "\"cs\"", 4) ||
                  !strncmp(st, "\"task\"", 6) || !strncmp(st, "\"mesh\"", 6));   // task/mesh: gfx_shader_reflect() mesh_kind
}

// Every instruction after the header has a word count and the last one ends at the module's end: a module with
// zero-filled clusters fails.
static int spirv_complete(const uint32_t *w, uint64_t n) {
    uint64_t i = 5;
    while (i < n) {
        uint32_t len = w[i] >> 16;
        if (!len) return 0;
        i += len;
    }
    return i == n;
}

// ---- MKW: SPIR-V prepared before the game runs (tools/shaders/translate.py)
// <game dir>/spirv/<key>.spv and .res.json, key = nvn2_program_key(control, code): every program the game ships was
// translated offline, so play never translates. MKW_SPIRV names another directory.
static char prepared_dir[PATH_CAP];
_Static_assert(CFG_GAME_DIR_MAX + 256 <= PATH_CAP, "game directory must leave room for derived shader paths");

static void prepared_init(void *param) {
    (void)param;
    const char *e = cfg_get("MKW_SPIRV");
    if (e && *e) {
        snprintf(prepared_dir, sizeof prepared_dir, "%s", e);
    } else {
        snprintf(prepared_dir, sizeof prepared_dir, "%s/spirv", cfg_game_dir());
    }
    rt_log("gfx_shader: prepared SPIR-V from %s", prepared_dir);
    gfx_shader_pack_open(prepared_dir);
}

static void prepared_paths(void) {
    static PlatOnce once = PLAT_ONCE_INIT;
    plat_once(&once, prepared_init, NULL);
}

// A program's prepared file of kind 0 .spv, 1 .res.json, 2 .cs.spv: from spirv/shaders.pack, else <key><suffix>.
static uint8_t *prepared_read(const uint8_t key[32], int kind, uint64_t *size) {
    static const char *const suffix[] = {".spv", ".res.json", ".cs.spv"};
    uint8_t *b;
    if (gfx_shader_pack_read(key, kind, &b, size)) return b;
    char hex[65], p[PATH_CAP + 80];
    gfx_shader_key_hex(key, hex);
    snprintf(p, sizeof p, "%s/%s%s", prepared_dir, hex, suffix[kind]);
    return read_file(p, size);
}

// MKW_HDR_TONEMAP=psychov: the game's final tone curve programs as tools/shaders/renodx.py rewrote them (RenoDX's
// PsychoV35 in their HDR mode), from <spirv>-renodx (beside spirv/, so its shaders.pack stays current); NULL for the
// game's own. Read once: the pipelines keep the programs they were built with, so it takes effect after a restart.
static char tonemap_path[PATH_CAP + 16];
static void tonemap_init(void *param) {
    (void)param;
    if (strcmp(cfg_value("MKW_HDR_TONEMAP"), "psychov")) return;
    snprintf(tonemap_path, sizeof tonemap_path, "%s-renodx", prepared_dir);
    rt_log("gfx_shader: HDR tone mapper PsychoV35 (%s)", tonemap_path);
}
static const char *tonemap_dir(void) {
    static PlatOnce once = PLAT_ONCE_INIT;
    plat_once(&once, tonemap_init, NULL);
    return tonemap_path[0] ? tonemap_path : NULL;
}

// The display's headroom for those programs: peak over paper white as the game is told them (gfx_hdr_headroom), 0
// when unknown (the programs then keep the game's own 1.5). Taken once, at the first program: a program read again
// after its SPIR-V was released (spirv_reload) gets the same words as the first time.
static float headroom_v;
static void headroom_init(void *param) {
    (void)param;
    headroom_v = gfx_hdr_headroom();
    rt_log("gfx_shader: RenoDX headroom %.2f (peak over paper white)", headroom_v);
}
static float rdx_headroom(void) {
    static PlatOnce once = PLAT_ONCE_INIT;
    plat_once(&once, headroom_init, NULL);
    return headroom_v;
}

// The constants renodx.py declares (gfx_rdx_patch, gfx_rdx.c). RDX_SPLIT_ID: MKW_RDX_SPLIT=<pixels>, the picture left
// of it keeps the game's curve, to compare the two in one frame.
enum { RDX_HEADROOM_ID = 30000, RDX_SPLIT_ID = 30001 };

// A program's SPIR-V as the runtime uses it: the RenoDX one with its constants set when the tone mapper is on and
// renodx.py made one (the same bindings and interface as the game's: it edits only the end), else the prepared one.
// prepared_load and spirv_reload both read through here, so a released program comes back with the same words.
static uint8_t *prepared_spv(const uint8_t key[32], uint64_t *size, int log) {
    const char *tm = tonemap_dir();
    if (tm) {
        char hex[65], p[PATH_CAP + 96];
        gfx_shader_key_hex(key, hex);
        snprintf(p, sizeof p, "%s/%s.spv", tm, hex);
        uint8_t *spv = read_file(p, size);
        if (spv && *size % 4 == 0) {
            float h = rdx_headroom();
            int ok = gfx_rdx_patch((uint32_t *)spv, *size / 4, RDX_HEADROOM_ID, h);
            const char *split = cfg_get("MKW_RDX_SPLIT");
            if (split && *split) gfx_rdx_patch((uint32_t *)spv, *size / 4, RDX_SPLIT_ID, (float)atof(split));
            if (log) rt_log("gfx_shader: %s from RenoDX, headroom %.2f%s", hex, h, ok ? "" : " (no constant to set)");
            return spv;
        }
        free(spv);
        snprintf(p, sizeof p, "%s/%s.none", tm, hex);
        if (log && plat_file_exists(p)) rt_log("gfx_shader: %s is a tone curve renodx.py could not edit: the game's", hex);
    }
    return prepared_read(key, 0, size);
}

static GfxShader *prepared_load(const uint8_t key[32], const uint8_t *control) {
    uint64_t spv_size = 0, res_size = 0;
    uint8_t *res = prepared_read(key, 1, &res_size);
    if (!res || !reflection_valid((const char *)res, res_size)) {
        free(res);
        return NULL;
    }
    uint8_t *spv = prepared_spv(key, &spv_size, 1);
    uint32_t magic = 0;
    if (spv && spv_size >= 20) memcpy(&magic, spv, 4);
    if (!spv || spv_size % 4 || magic != 0x07230203 || !spirv_complete((const uint32_t *)spv, spv_size / 4)) {
        free(res), free(spv);
        return NULL;
    }
    GfxShader *s = calloc(1, sizeof *s);
    memcpy(s->key, key, 32);
    memcpy(s->iid, key, 32);   // pipeline key records name a prepared shader by its program key (gfx_shader_load_input)
    s->spirv = (uint32_t *)spv;
    s->words = spv_size / 4;
    s->reflection = (char *)res;
    s->reflection_size = res_size;
    if (control) {
        uint16_t st;
        memcpy(&st, control + 0x56, 2);   // NVNshaderStage (nvn2_program_stage): 5 compute
        if (st == 5) {
            s->cs_table[0] = control[0xb1], s->cs_table[1] = control[0xb2];
            s->cs_table[2] = control[0xb3], s->cs_table[3] = control[0xb6];
            s->cs_table[4] = control[0xa9];
            s->cs_known = 1;
        }
    }
    gfx_shader_reflect(s);
    if (s->stage == GFX_STAGE_CS && !s->cs_known) {
        // loaded by key (a prewarm) from a translation older than its "cs_table": its resources would sit in the
        // wrong slots, and the cache would keep it so for the game's dispatch. Left out: the game loads it with its
        // control.
        shader_free(s);
        return NULL;
    }
    s->spirv_state = SPIRV_PREPARED;
    return gfx_shader_insert(s);
}

// The prepared shader of a program, or NULL (logged once per program) when the offline translator could not do it.
static const GfxShader *prepared_get(const uint8_t *control, const uint8_t *code) {
    prepared_paths();
    uint8_t key[32];
    nvn2_program_key(control, code, key);
    GfxShader *s = gfx_shader_find(key);
    if (s) return s;
    s = prepared_load(key, control);
    if (!s) {
        char hex[65];
        gfx_shader_key_hex(key, hex);
        static volatile int32_t n_missing;
        int32_t n = plat_inc32(&n_missing);
        if (n <= 20 || !(n & (n - 1))) rt_log("gfx_shader: no prepared SPIR-V %s (%d lookups so far)", hex, n);
        // MKW_SPIRV_DUMP=<dir>: the missing program's control section and code, to find where it comes from.
        const char *dump = cfg_get("MKW_SPIRV_DUMP");
        if (dump && *dump) {   // every missing program once (the file exists after the first time)
            char p[PATH_CAP + 80];
            uint32_t ctl_size = 0xd8, size;
            memcpy(&size, control + 0x44, 4);
            plat_file_mkdir_p(dump);
            snprintf(p, sizeof p, "%s/%s.ctl", dump, hex);
            if (plat_file_exists(p)) return s;
            plat_file_write(p, control, ctl_size);
            snprintf(p, sizeof p, "%s/%s.code", dump, hex);
            plat_file_write(p, code, size < (16u << 20) ? size : 0);
        }
    }
    return s;
}

// ---- public

// The prepared shader of a program, as its integer-input variant when a vertex shader's attributes in a have integer
// formats; NULL when the program has no prepared SPIR-V (logged).
const GfxShader *gfx_shader_get(const uint8_t *control, const uint8_t *code, const GfxShaderAnswers *a) {
    GfxShader *s = (GfxShader *)prepared_get(control, code);
    return gfx_shader_int_variant(s, a);
}

uint64_t gfx_shader_count(void) { return (uint64_t)n_keys; }

const uint8_t *gfx_shader_key(const GfxShader *s) { return s->key; }

const uint8_t *gfx_shader_input_id(const GfxShader *s) { return s->iid; }

// By program key (a pipeline key record's shader id): memory, then the prepared directory. NULL when neither has it.
const GfxShader *gfx_shader_load_input(const uint8_t iid[32]) {
    prepared_paths();
    GfxShader *s = gfx_shader_find(iid);
    return s ? s : prepared_load(iid, NULL);
}

// The shaders named by pipeline key records, loaded (out[i] NULL when one is missing). Off from run49 to 06/11/2026: the
// records' integer-input vertex shaders were built from the base program (vk_pipe_keys.c), and the key file was never
// read after the TOTK magic rename (05/11/2026).
// read on every core: one by one, 729 shaders took 1 056 ms the guest waited for (pw2, 06/11/2026)
static struct { const uint8_t (*iids)[32]; const GfxShader **out; size_t n; volatile int64_t next, miss; } PW;

static void prewarm_worker(void *arg) {
    plat_thread_background();   // off the frame's fast cores (MKW_FAST_CORES)
    (void)arg;
    for (int64_t i; (i = plat_inc64(&PW.next) - 1) < (int64_t)PW.n;)
        if (!(PW.out[i] = gfx_shader_load_input(PW.iids[i]))) plat_inc64(&PW.miss);
}

void gfx_shader_prewarm(const uint8_t (*iids)[32], size_t n, const GfxShader **out) {
    PW.iids = iids, PW.out = out, PW.n = n, PW.next = 0, PW.miss = 0;
    PlatThread *t[64];
    int nt = 0, cpus = (int)plat_cpu_count();
    for (int i = 0; i < cpus && i < 64 && n > 16; i++)
        if ((t[nt] = plat_thread_new(prewarm_worker, NULL, "shader prewarm"))) nt++;
    prewarm_worker(NULL);
    for (int i = 0; i < nt; i++)
        while (!plat_thread_join_ns(t[i], 1000000000u)) {}
    rt_log("gfx_shader: prewarm: %zu shaders named by pipeline keys, %lld not found, on %d threads", n,
           (long long)PW.miss, nt + 1);
}

// ---- every program of shaders.pack, loaded while the boot prewarm creates the device (vk_pipe_keys.c prewarm_run):
// the pairs prewarm reads them all, and they need no device. pack-boot2 (18/11/2026, file cache cold, Windows PC): the
// device took 7.2 s and the load 2.7 s more after it.
static struct { uint8_t (*ids)[32]; const GfxShader **out; size_t n; } PL;

static void preload_main(void *arg) {
    (void)arg;
    gfx_shader_prewarm((const uint8_t(*)[32])PL.ids, PL.n, PL.out);
}

PlatThread *gfx_shader_preload_start(void) {
    prepared_paths();
    PL.n = gfx_shader_pack_keys(&PL.ids);
    PL.out = PL.n ? calloc(PL.n, sizeof *PL.out) : NULL;
    PlatThread *t = PL.out ? plat_thread_new(preload_main, NULL, "shader preload") : NULL;
    if (!t) free(PL.ids), free(PL.out), PL.ids = NULL, PL.out = NULL;
    return t;
}

void gfx_shader_preload_join(PlatThread *t) {
    if (!t) return;
    plat_thread_join(t);
    free(PL.ids), free(PL.out), PL.ids = NULL, PL.out = NULL;
}

// ---- SPIR-V released after the prewarm
// The pairs prewarm (vk_pipe_pairs.c) loads every program of the copy, 306 MiB of SPIR-V on the 660M (pk-mem,
// 17/11/2026), to build their libraries; most are never drawn in a session. Those the prewarm alone read are freed
// after it and read again from their file if a draw asks for one.
static PlatMutex spirv_lock = PLAT_MUTEX_INIT;   // a release against a read back
static _Thread_local int spirv_quiet;

void gfx_shader_spirv_quiet(int on) { spirv_quiet = on; }

static void spirv_reload(GfxShader *s) {
    plat_mutex_lock(&spirv_lock);
    if (__atomic_load_n(&s->spirv_state, __ATOMIC_ACQUIRE) == SPIRV_RELEASED) {
        uint64_t n = 0;
        uint8_t *spv = prepared_spv(s->key, &n, 0);
        if (!spv || n != s->words * 4) {
            char hex[65];
            gfx_shader_key_hex(s->key, hex);
            rt_fatal("gfx_shader: %s/%s.spv changed or went missing since it was loaded", prepared_dir, hex);
        }
        s->spirv = (uint32_t *)spv;
        __atomic_store_n(&s->spirv_state, SPIRV_KEPT, __ATOMIC_RELEASE);
    }
    plat_mutex_unlock(&spirv_lock);
}

const uint32_t *gfx_shader_spirv(const GfxShader *cs, uint64_t *words) {
    GfxShader *s = (GfxShader *)cs;
    if (words) *words = s->words;
    int32_t st = __atomic_load_n(&s->spirv_state, __ATOMIC_ACQUIRE);
    if (st == SPIRV_PREPARED && !spirv_quiet) st = plat_cas32(&s->spirv_state, SPIRV_KEPT, SPIRV_PREPARED);
    if (st == SPIRV_RELEASED) spirv_reload(s);
    return s->spirv;
}

// Frees the SPIR-V of every prepared shader nothing but the prewarm has read. The caller has joined the prewarm's
// threads: a quiet read never takes the state, so one running now could read freed memory.
uint64_t gfx_shader_spirv_release(void) {
    uint64_t bytes = 0, before = 0, after = 0, priv = 0;
    plat_process_memory(&before, &priv);
    plat_mutex_lock(&spirv_lock);
    plat_mutex_lock_shared(&cache_lock);
    for (int b = 0; b < BUCKETS; b++)
        for (GfxShader *s = buckets[b]; s; s = s->next)
            if (plat_cas32(&s->spirv_state, SPIRV_RELEASED, SPIRV_PREPARED) == SPIRV_PREPARED) {
                bytes += s->words * 4;
                free(s->spirv);
                s->spirv = NULL;
            }
    plat_mutex_unlock_shared(&cache_lock);
    plat_mutex_unlock(&spirv_lock);
    plat_heap_trim();   // ~28 KiB blocks: without it the C heap keeps the pages
    plat_process_memory(&after, &priv);
    rt_log("gfx_shader: released %.0f MiB of SPIR-V only the prewarm read; resident %.0f -> %.0f MiB",
           (double)bytes / (1 << 20), (double)before / (1 << 20), (double)after / (1 << 20));
    return bytes;
}

int gfx_shader_stage(const GfxShader *s) { return s->stage; }
int gfx_shader_first_store(const GfxShader *s) { return s->first_store; }

uint32_t gfx_shader_int_inputs(const GfxShader *s) { return s->int_inputs; }

uint32_t gfx_shader_raw_outputs(const GfxShader *s) { return s->raw_outputs; }

// A copy of s whose specialization constants default to val[k] for each word k with fix[k] (SCU<k> true, SCV<k> =
// val[k]), the same as spirv-opt --set-spec-const-default-value: the pipeline code sees an ordinary shader, so the
// pipeline cache, the GPL libraries and the background builds treat a variant like any other. The copy lives for
// the process and shares s's reflection; its key and input id are sha256(s's key, the fixed words). NULL when the
// SPIR-V has none of the constants.
const GfxShader *gfx_shader_variant(const GfxShader *s, const uint32_t *val, const uint8_t *fix) {
    enum { OP_SPEC_TRUE = 48, OP_SPEC_FALSE = 49, OP_SPEC = 50, OP_DECORATE = 71, DEC_SPEC_ID = 1 };
    if (s->words < 5) return NULL;
    const uint32_t *src = gfx_shader_spirv(s, NULL);
    uint32_t bound = src[3], *w = malloc(sizeof *w * s->words);
    int32_t *sid = malloc(sizeof *sid * (bound ? bound : 1));
    if (!w || !sid) rt_fatal("gfx_shader: out of memory");
    memcpy(w, src, sizeof *w * s->words);
    memset(sid, 0xff, sizeof *sid * bound);
    for (uint64_t i = 5; i < s->words;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > s->words) break;
        if (op == OP_DECORATE && len >= 4 && w[i + 2] == DEC_SPEC_ID && w[i + 1] < bound) sid[w[i + 1]] = (int32_t)w[i + 3];
        i += len;
    }
    int patched = 0;
    for (uint64_t i = 5; i < s->words;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > s->words) break;
        if ((op == OP_SPEC_TRUE || op == OP_SPEC_FALSE || op == OP_SPEC) && len >= 3 && w[i + 2] < bound) {
            int32_t id = sid[w[i + 2]];
            int k = id >> 1;
            if (id >= 0 && k < s->nspec && fix[k]) {
                if (!(id & 1) && op != OP_SPEC) w[i] = (w[i] & 0xffff0000u) | OP_SPEC_TRUE, patched++;
                else if ((id & 1) && op == OP_SPEC && len == 4) w[i + 3] = val[k], patched++;
            }
        }
        i += len;
    }
    free(sid);
    if (!patched) {
        free(w);
        return NULL;
    }
    GfxShader *v = malloc(sizeof *v);
    if (!v) rt_fatal("gfx_shader: out of memory");
    *v = *s;
    v->next = NULL, v->spirv = w, v->spirv_state = SPIRV_KEPT, v->variant_of = s;
    size_t n = 32 + (size_t)s->nspec * 5;
    uint8_t *buf = malloc(n);
    if (!buf) rt_fatal("gfx_shader: out of memory");
    memcpy(buf, s->key, 32);
    for (int k = 0; k < s->nspec; k++) buf[32 + 5 * k] = fix[k], memcpy(buf + 33 + 5 * k, fix[k] ? &val[k] : &(uint32_t){0}, 4);
    rt_sha256(buf, n, v->key);
    memcpy(v->iid, v->key, 32);
    free(buf);
    return v;
}

const GfxShader *gfx_shader_variant_of(const GfxShader *s) { return s->variant_of; }

int gfx_shader_spec(const GfxShader *s, const uint32_t **words) {
    *words = s->spec;
    return s->nspec;
}

int gfx_shader_resources(const GfxShader *s, int kind, const GfxShaderRes **res) {
    if (kind < 0 || kind >= GFX_RES_KINDS) return 0;
    if (res) *res = s->res[kind];
    return s->nres[kind];
}

uint32_t gfx_shader_inputs(const GfxShader *s, GfxShaderIo io[32]) {
    if (io) memcpy(io, s->in, sizeof s->in);
    return s->in_mask;
}

uint32_t gfx_shader_outputs(const GfxShader *s, GfxShaderIo io[32]) {
    if (io) memcpy(io, s->out, sizeof s->out);
    return s->out_mask;
}

int gfx_shader_mesh_kind(const GfxShader *s) { return s ? s->mesh_kind : GFX_MESH_NONE; }

int gfx_shader_cs_of_mesh(const GfxShader *s) { return s && s->cs_of_mesh; }

// The vertex shader of task/mesh as compute: a prepared shader under a fixed key that no program has
// (tools/shaders/translate.py write_mesh_cs_vs, sm86 head.py MESH_CS_VS_KEY), so pipeline key records name it too.
static const uint8_t mesh_cs_vs_key[32] = "mkw-mesh-cs-vs-1mkw-mesh-cs-vs-1";

const GfxShader *gfx_shader_mesh_cs_vs(void) {
    static const GfxShader *vs;
    static int tried;
    if (!tried) {
        vs = gfx_shader_load_input(mesh_cs_vs_key), tried = 1;
        if (!vs) rt_log("gfx_shader: no vertex shader for task/mesh as compute (spirv/%s.spv)", "6d6b772d...");
    }
    return vs;
}

int gfx_shader_is_mesh_cs_vs(const GfxShader *s) { return s && !memcmp(s->key, mesh_cs_vs_key, 32); }

// The compute form of a task or mesh program, <key>.cs.spv beside its .spv, with the
// same reflection: stage CS, the task/mesh kind kept for its binding tags. Loaded once and kept with the program (not
// in the cache by key, where the program itself is). NULL for any other program or when the file is missing.
const GfxShader *gfx_shader_cs_variant(const GfxShader *s) {
    static PlatMutex lock = PLAT_MUTEX_INIT;
    if (!s || s->mesh_kind == GFX_MESH_NONE || s->cs_of_mesh) return NULL;
    GfxShader *m = (GfxShader *)s;
    plat_mutex_lock(&lock);
    if (!m->cs_variant) {
        char hex[65], p[PATH_CAP + 80];
        gfx_shader_key_hex(s->key, hex);
        snprintf(p, sizeof p, "%s/%s.cs.spv", prepared_dir, hex);
        uint64_t n = 0;
        uint8_t *spv = prepared_read(s->key, 2, &n);
        uint32_t magic = 0;
        if (spv && n >= 20) memcpy(&magic, spv, 4);
        char *res = malloc(s->reflection_size + 1);
        if (spv && n % 4 == 0 && magic == 0x07230203 && spirv_complete((const uint32_t *)spv, n / 4) && res) {
            GfxShader *v = calloc(1, sizeof *v);
            memcpy(v->key, s->key, 32), memcpy(v->iid, s->iid, 32);
            v->spirv = (uint32_t *)spv, v->words = n / 4;
            memcpy(res, s->reflection, s->reflection_size), res[s->reflection_size] = 0;
            v->reflection = res, v->reflection_size = s->reflection_size;
            gfx_shader_reflect(v);
            v->stage = GFX_STAGE_CS, v->cs_of_mesh = 1;
            m->cs_variant = v;
        } else {
            free(spv), free(res);
            rt_log("gfx_shader: no compute form of %s program %s (%s)", s->mesh_kind == GFX_MESH_TASK ? "task" : "mesh", hex, p);
        }
    }
    plat_mutex_unlock(&lock);
    return m->cs_variant;
}
const uint8_t *gfx_shader_cs_table(const GfxShader *s) { return s && s->cs_known ? s->cs_table : NULL; }
