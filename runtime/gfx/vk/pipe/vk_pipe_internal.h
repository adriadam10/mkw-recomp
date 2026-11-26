// Internal interface of the pipeline builder (vk_pipeline.c, vk_pipe_layout.c, vk_pipe_gpl.c and vk_pipe_keys.c): the state, records and
// calls the graphics pipeline library, the pipeline-key file and the boot prewarm share with the rest. Everything else goes through vk.h.
#pragma once

#include <stdint.h>

#include "gfx.h"
#include "vk.h"
#include "vk_exec.h"
#include "platform/platform.h"

#define MAX_BINDINGS 128
#define PIPE_BUCKETS 4096
#define LAYOUT_BUCKETS 256

typedef struct {
    PlatMutex lock;
    volatile int32_t ready;
    VkDevice device;
    VkPipelineCache cache;
    uint32_t max_push;
    int draw_cache;                    // MKW_DRAW_CACHE: no push sets, uniform buffers dynamic (vk_exec_dcache.c)
    uint32_t smem_limit;               // workgroup memory for spirv_clamp_shared (MKW_VK_SHARED_LIMIT lowers it)
    uint32_t max_dyn_ubo;              // maxDescriptorSetUniformBuffersDynamic
    int indep;                         // stage-independent layouts (vk_caps.gpl): vertex set 0, fragment set 1
    int push_stage;                    // GFX_STAGE_* whose set is pushed with independent layouts
    int gpl;                           // draws whose pipeline is building get a fast-linked one (vk_caps.gpl)
    struct { uint64_t links, wlinks, dlinks, requeued, link_fail, defer, lib_n[4], lib_fail, swaps, slow; double link_ms, link_max, lib_ms[4], lib_max[4], dlink_ms, dlink_max; } g;
    int vs_outs_all;                   // MKW_VS_OUTS: 0 union of paired fragment inputs, 1 (all) all 32 output locations
    int dyn_vi, dyn_clamp, dyn_poly, dyn_blend_en, dyn_blend_eq, dyn_wmask, dyn_a2c;
    int f_logic_op, f_depth_clamp, f_fill_non_solid, f_dual_src;
    int pv_last;                       // provokingVertexLast enabled: NVN's default is the last vertex
    VkShaderStageFlags sg32_stages;    // stages that accept requiredSubgroupSize = 32 (0: not available)
    VkDynamicState dyn[32];
    uint32_t n_dyn;
    PFN_vkCmdPushDescriptorSetKHR push;
    int tpl;                           // descriptor update templates on (make_tpl)
    PFN_vkCreateDescriptorUpdateTemplate tpl_create;
    PFN_vkDestroyDescriptorUpdateTemplate tpl_destroy;
    PFN_vkUpdateDescriptorSetWithTemplate tpl_update;
    PFN_vkCmdPushDescriptorSetWithTemplateKHR tpl_push;
    PFN_vkCmdSetVertexInputEXT set_vi;
    PFN_vkCmdSetDepthClampEnableEXT set_clamp;
    PFN_vkCmdSetPolygonModeEXT set_poly;
    PFN_vkCmdSetColorBlendEnableEXT set_blend_en;
    PFN_vkCmdSetColorBlendEquationEXT set_blend_eq;
    PFN_vkCmdSetColorWriteMaskEXT set_wmask;
    PFN_vkCmdSetAlphaToCoverageEnableEXT set_a2c;
    char cache_path[PLAT_PATH_MAX * 2];
    VkPipeStats stats;
} PipeGlobals;
extern PipeGlobals P;

typedef struct Layout {
    struct Layout *next;
    uint64_t hash;
    VkDescriptorSetLayout set;
    VkPipelineLayout layout;
    int push;
    VkShaderStageFlags pc_stages;      // Bindings.pc_stages
    uint32_t n;
    VkDescriptorSetLayout set1;        // independent layouts: the fragment set (set 1); VK_NULL_HANDLE otherwise
    int push1;                         // set 1 is a push-descriptor set
    uint32_t n0;                       // independent layouts: b[0..n0) are set 0, the rest set 1
    VkPipelineLayout half[2];          // GPL libraries: {set 0, null}, {null, set 1} (made on first use)
    VkDescriptorUpdateTemplate tpl[2]; // per set (make_tpl); VK_NULL_HANDLE: written with write structs
    VkDescriptorSetLayoutBinding b[];
} Layout;

typedef struct Pipe {
    struct Pipe *next;
    uint64_t hash;
    VkPipeInfo info;
    VkPipeBinding *bindings;
    int building;                      // a background job owns info.pipeline (under P.lock)
    struct Job *job;                   // that job while building
    const struct Layout *lay;          // graphics: the layout it was entered with
    VkPipeline fast;                   // GPL: the fast-linked pipeline (kept until shutdown: recorded draws use it)
    uint8_t fast_tried;
    uint8_t gave_up;                   // a draw waited its whole wait_ms for it: later draws skip it without waiting
    size_t key_size;
    uint8_t key[];
} Pipe;

#define FNV0 0xcbf29ce484222325ull

typedef struct {
    uint32_t n;
    VkShaderStageFlags pc_stages;      // stages with the MESH_PC_BYTES push constants of task/mesh as compute, 0 none
    VkDescriptorSetLayoutBinding b[MAX_BINDINGS];
    VkPipeBinding pb[MAX_BINDINGS];
} Bindings;
#define MESH_PC_BYTES 48               // sm86/translate/glsl/head.py MeshPass: four buffer addresses and has_task

#define VS_OUTS_CAP 8192

typedef struct {
    PlatMutex lock;
    const GfxShader *vs[VS_OUTS_CAP];
    uint32_t mask[VS_OUTS_CAP];
    GfxShaderIo io[VS_OUTS_CAP][32];
    uint32_t n, full;
} VsOutsCache;
extern VsOutsCache VO;
extern Layout *layouts[LAYOUT_BUCKETS];   // vk_pipe_layout.c; freed by vk_pipeline_shutdown

typedef struct {                       // normalized: zero where the state is dynamic or unused
    const GfxShader *vs, *fs;
    uint32_t color[8], depth, stencil; // VkFormat
    uint8_t n_color, samples, topo_class, minus_w;
    uint8_t clamp, poly_mode, a2c, logic_op;   // logic_op: VkLogicOp + 1 when enabled
    uint8_t blend_en, dsb, fbl, alpha_func;   // dsb: an enabled target blends with a SRC1 factor; fbl: VkPipeState.feedback;
                                              // alpha_func: VkCompareOp + 1 of NVN's alpha test, 0 off
    uint32_t wmask;
    uint8_t blend[8][6];               // src, dst, op, src alpha, dst alpha, alpha op (VkBlendFactor / VkBlendOp)
    uint32_t attr_fmt[32], attr_off[32];
    uint8_t attr_bind[32];
    uint8_t rate[17];                  // per binding: 0 unused, 1 vertex, 2 instance
    uint8_t indep;                     // P.indep when built: the probe draws through both layouts
    uint8_t meshp;                     // a task/mesh pipeline (mesh_pipe): monolithic, one set, no vertex input
    uint8_t pad2;
    const GfxShader *mesh;             // VkPipeState.mesh
} GKey;

extern PlatCond work_cv, done_cv;

enum { LIB_VI, LIB_PR, LIB_FS, LIB_FO };

typedef struct Lib { struct Lib *next; uint64_t hash; VkPipeline pipe; uint32_t size; uint8_t building, pad[3];
                     struct LibJob *job; uint8_t key[]; } Lib;

typedef struct { uint32_t kind, topo_class, attr_fmt[32], attr_off[32]; uint8_t attr_bind[32], rate[17], pad[3]; } ViKey;

typedef struct { uint32_t kind, missing; const GfxShader *vs; uint8_t clamp, poly_mode, minus_w, pad; GfxShaderIo fin[32]; } PrKey;

typedef struct { uint32_t kind; uint8_t dsb, samples, a2c, alpha_func; const GfxShader *fs; } FsKey;

typedef struct { uint32_t kind; uint8_t n_color, samples, a2c, logic_op, blend_en, pad[3]; uint32_t color[8], depth, stencil, wmask;
                 uint8_t blend[8][6]; } FoKey;

typedef struct LibJob { struct LibJob *next; Lib *b; int kind; Layout *l; VkPipeState s; GKey k; } LibJob;

typedef struct {
    LibJob *head, *tail;
    int idle;                          // workers asleep on work_cv
    int running;                       // jobs workers are building
    int on;                            // MKW_PIPE_WAIT: 1 workers (default), 0 the drawing thread builds them (draw);
                                       // set by vk_pipeline_async_enable, before the replay runs
    int helpers;                       // lib_wait's helper threads still building
    uint64_t waits, queued, by_worker, stolen, timeouts;
} LibQueue;
extern LibQueue U;

typedef struct Job {
    struct Job *next, *prev;           // prev: low queue only
    Pipe *p;
    const Layout *l;
    uint32_t fs_off;
    uint8_t low;                       // in the low queue, or running at below-normal priority
    uint8_t prebuild, demanded;        // from the key file; a draw asked for it before it was ready
    int worker;                        // running on A.thread[worker]; -1 queued
    VkPipeState s;
    GKey k;
} Job;

#define MAX_WORKERS 4

typedef struct {
    int on;                            // set once by vk_pipeline_async_enable, before the replay runs
    int threads;
    PlatThread *thread[MAX_WORKERS];
    uint8_t prio_low[MAX_WORKERS];     // worker runs below normal (under P.lock)
    Job *head, *tail;                  // draws' misses
    Job *low, *low_tail;               // prebuild
    uint32_t inflight;                 // queued or running, both queues
    int low_running;                   // pool workers running a job taken from the low queue
    PlatCond idle;           // with P.lock: no job queued or running
} AsyncBuilds;
extern AsyncBuilds A;

typedef struct { char magic[8]; uint32_t version, rec_size; } KeysHeader;
typedef struct {
    uint8_t vs[32], fs[32];            // gfx_shader_input_id of each stage; fs all zero with has_fs 0
    uint32_t has_fs, pad;
    VkPipeState s;                     // vs, fs zero
} KeyRec;

typedef struct {
    char path[PLAT_PATH_MAX * 2];
    int header_ok;                     // the file exists with our header: append
    uint64_t *seen;                    // open addressing set of record hashes (0 empty)
    uint32_t seen_cap, seen_n;
    KeyRec *pend;                      // records not written yet
    uint32_t pend_n, pend_cap;
    uint8_t *file;                     // the file as read at boot, until the prewarm is done with it
    size_t file_n;                     // records in it
    PlatMutex io;                        // one writer
    // prebuild progress (under P.lock)
    int loading;                       // the prewarm still queues records
    int warm;                          // vk_pipeline_prewarm owns the file (prebuild_start leaves it)
    uint32_t left;                     // prebuild jobs queued or running
    uint32_t built, failed, promoted;
    double t0;                         // now_ms() when the prebuild started
} KeyFile;
extern KeyFile K;

uint64_t pipe_fnv(uint64_t h, const void *p, size_t n);
uint64_t key_hash(const void *k, size_t n);
void pipe_test_stall(const GfxShader *vs, const GfxShader *fs);   // MKW_PIPE_STALL (vk_pipeline.c)
int ensure_init(void);
void pin_subgroup32(VkPipelineShaderStageCreateInfo *st, VkPipelineShaderStageRequiredSubgroupSizeCreateInfo *rq,
                           const uint32_t *w, uint64_t words);
VkShaderModule make_module(const uint32_t *w, uint64_t words);
// NEAR_IS_MINUS_W depth (minus_w) on a device without VK_EXT_depth_clip_control: the module of sh that writes the
// position, converted (vk_spirv_depth.c). 0: nothing to do; 1: *code and *words are the malloc'd copy now in *owned
// (the copy it held before freed); -1: it cannot be converted (logged): no pipeline, rather than draws at a wrong depth.
int depth_convert(int minus_w, const GfxShader *sh, const uint32_t **code, uint64_t *words, uint32_t **owned);
// The viewport state's pNext for minus_w: VK_EXT_depth_clip_control's struct where the device has it, else NULL
const void *depth_clip_next(const VkPipelineViewportDepthClipControlCreateInfoEXT *clip);
uint32_t binding_span(const GfxShader *s);
int add_stage(Bindings *B, const GfxShader *s, uint32_t off);
int layout_covers(const VkPipeState *s, const Layout *l, uint32_t fs_off, char *why, size_t cap);
Layout *layout_get(Bindings *B);
Layout *layout_pair(Bindings *B, Bindings *F);
uint32_t vs_outs(const GfxShader *vs, const GfxShader *fs, GfxShaderIo io[32]);
Pipe *pipe_find(const void *key, size_t size, uint64_t h);
Pipe *pipe_add(const void *key, size_t size, uint64_t h, VkPipeline pipeline, const Layout *l, const Bindings *B,
                      VkPipelineBindPoint bp, uint32_t vs_inputs);
int mesh_pipe(const VkPipeState *s);
void gkey_fill(const VkPipeState *s, GKey *k);
void mesh_offsets(const VkPipeState *s, uint32_t *mesh_off, uint32_t *fs_off);
VkPipeline create_graphics(const VkPipeState *s, const GKey *k, const Layout *l, uint32_t fs_off);
void log_stats(VkPipeline pipe, const VkPipeState *s);   // MKW_SPEC_STATS=1 (and MKW_SPEC_ISA), vk_pipeline.c
typedef struct {
    uint32_t n_attr, n_bind;
    VkVertexInputAttributeDescription2EXT a[32];
    VkVertexInputBindingDescription2EXT b[17];
} VtxInput;
void vertex_input(const VkPipeState *s, VtxInput *v);
uint32_t vs_union_missing(const VkPipeState *s, GfxShaderIo fin[32]);
// vk_pipe_gpl.c
uint64_t pipe_qpc(void);
double ms_since(uint64_t t0);
VkPipelineLayout half_layout(Layout *l, int h);
Lib *lib_find(uint64_t h, const void *key, uint32_t size);
Lib *lib_new(uint64_t h, const void *key, uint32_t size);
VkPipeline lib_build(Lib *b, int kind, const VkPipeState *s, const GKey *k, Layout *l);
void lib_steal(Lib *b);
void lib_drop(Lib *b);   // a building entry no job will build: removed, so a later draw builds its own
LibJob *lib_take(Lib *b);
void lib_keys(const VkPipeState *s, const GKey *k, ViKey *v, PrKey *pk, FsKey *fk, FoKey *ok);
VkPipeline gpl_link(const VkPipeState *s, const GKey *k, const Layout *lc, int build, int *final);
void libs_destroy(void);
void keys_flush(void);
void keys_open(void);
void workers_drain(void);
VkPipeline pipe_graphics(const VkPipeState *s, const VkPipeInfo **info, int async, int *pending, uint32_t wait_ms,
                           int low);
void prebuild_start(void);
// vk_pipe_pairs.c: the shader libraries of every program pair in pipeline-pairs.bin, at boot
void pairs_prewarm(void);
int vk_pipeline_compute_warm(const GfxShader *cs, uint32_t first);   // caller holds P.lock (released while it builds)
void pairs_release(void);   // after vk_pipeline_prewarm: the SPIR-V only the prewarms read (not the records' after a stall)
