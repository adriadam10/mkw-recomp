// nn::codec's video decoder API (VP9) and nn::syncpt's sync point events, replaced by name on the host with libvpx.
//
// MKW 1.0.0 plays its movies (romfs/Movie/*.webm: ad boards, cup openings) through its own player in main
// (main+0x19e5860 and the per-codec wrappers main+0x19e7830..0x19e8110), which calls the SDK's video API
// directly; TotK went through movie::Decoder instead, and so does MKW 1.8.0 (subsdk0's movie library, with the newer
// names of the API registered at the end of this file). The SDK's version talks to the gsv / vdecsvc
// services, which this runtime does not have (no emulation below the API).
//
// What the player does (measured in its code):
// - an image pool per output buffer (MakeDecodeImagePreference + CalculateDecoderImagePoolBuffer, then
//   InitializeDecoderImagePool with its own memory; the pool object is 0x20 bytes, the game stores width, height and
//   mode at +0x20..+0x27 after it) and RegisterVp9DecoderBuffer(decoder, slot, pool, 0) for each;
// - SubmitVp9DecoderInputBuffer per compressed frame (0x1c266f "busy" is retried after the submission-ready event);
// - AcquireVp9DecoderOutputBuffer always succeeds: it hands out the next output slot and a sync handle the player
//   waits on through nn::syncpt (RequestSyncPointEvent + a multi-wait holder); then the frame's properties and, in its
//   CPU mode, ReadDecoderOutputImageNv12 into memory of w * h * 3 / 2 bytes; ReleaseVp9DecoderOutputBuffer gives the
//   slot back.
// Here, as on the console, Submit only queues the compressed frame: a host thread per decoder runs libvpx (with its
// own tile threads) outside the lock. Decoding inside Submit held the player's guest thread ~one 2560x1440 frame each
// time (Adrià: movie audio cuts). Frames go to slots in the order they were acquired: the k-th acquired slot gets the
// k-th frame, and its sync handle is {decoder id, k}. A sync point event is a guest nn::os::SystemEvent (so the game's
// multi-waits work unchanged), bound to a kernel event (kern_bind_system_event) so the decoder thread can signal it
// when its frame exists. Only VP9 is served; another codec stops the run with its name.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <vpx/vp8dx.h>
#include <vpx/vpx_decoder.h>

#include "hacks.h"
#include "host.h"
#include "nvn_api.h"
#include "platform/platform.h"

#define R_BUSY 0x1c266fu          // the player retries Submit after the submission-ready event
#define MAX_SLOTS 16
#define AHEAD 3                   // decoded frames no slot has taken yet, before Submit answers busy
#define DEC_MAGIC 0x39504456u     // "VDP9" at +8 of a Vp9DecoderType this file initialized

static PlatMutex lock = PLAT_MUTEX_INIT;

// The first calls of each kind and then every 256th go to the log.
enum { T_SUBMIT, T_FRAME, T_ACQUIRE, T_RELEASE, T_PROPS, T_READ, T_REQUEST, T_SIGNAL, T_BUSY, T_N };
static const char *const tnames[T_N] = {"submit", "frame", "acquire", "release", "props", "read", "request", "signal",
                                        "busy"};
static volatile int32_t tcount[T_N];
static int traced(int k) {
    int32_t n = plat_inc32(&tcount[k]);
    return n <= 6 || !(n & 255);
}
#define TRACE(k, fmt, ...) do { if (traced(k)) rt_log("codec: %s %d: " fmt, tnames[k], tcount[k], __VA_ARGS__); } while (0)

// Decode time of a packet on the decoder thread: mean and worst every 256 frames (Adrià: movie audio cuts).
static uint64_t dec_ns, dec_max, dec_n;   // lock
static void note_decode(uint64_t ns) {
    dec_ns += ns, dec_n++;
    if (ns > dec_max) dec_max = ns;
    if (dec_n == 256) {
        rt_log("codec: decode %.2f ms a frame, worst %.2f ms (256 frames)", dec_ns / 1e6 / 256, dec_max / 1e6);
        dec_ns = dec_max = dec_n = 0;
    }
}

static uint64_t fn_signal, fn_clear, fn_trywait, fn_holder;
static uint64_t sdk_fn(const char *name) {
    uint64_t a = ldr_symbol(name);
    if (!a) rt_fatal("codec: SDK function %s not found", name);
    return a;
}
static void fns(void) {
    if (fn_signal) return;
    fn_signal = sdk_fn("_ZN2nn2os17SignalSystemEventEPNS0_15SystemEventTypeE");
    fn_clear = sdk_fn("_ZN2nn2os16ClearSystemEventEPNS0_15SystemEventTypeE");
    fn_trywait = sdk_fn("_ZN2nn2os18TryWaitSystemEventEPNS0_15SystemEventTypeE");
    fn_holder = sdk_fn("_ZN2nn2os25InitializeMultiWaitHolderEPNS0_19MultiWaitHolderTypeEPNS0_15SystemEventTypeE");
}

// Guest SystemEventTypes from the kernel region (it cannot free), bound to kernel events so a host thread can signal
// them (kern_signal_system_event). Finished ones are cleared and kept, bound, for the next: the kernel's table binds an
// address once (kern_bind_system_event returns early for a known one), so they are never destroyed.
static uint64_t free_events[64];
static int nfree_events;
static uint64_t event_new(Cpu *c) {
    uint64_t ev = nfree_events ? free_events[--nfree_events] : (uint64_t)(uintptr_t)mem_kernel_alloc(0x100, 16);
    kern_bind_system_event(c, ev, 1);   // clear mode 1, as CreateSystemEvent got before
    return ev;
}
static void event_free(Cpu *c, uint64_t ev) {
    rt_call(c, fn_clear, 1, ev);
    if (nfree_events < 64) free_events[nfree_events++] = ev;
}

// ---- decoded frames and decoders
typedef struct {
    uint64_t ord;                 // 1-based decode order
    uint16_t w, h;
    int64_t pts;
    uint8_t eos;
    uint8_t *nv12;                // w * h luma, then w * h / 2 interleaved CbCr
} Frame;

typedef struct { uint8_t *data; uint64_t size; int64_t pts; int eos; } Packet;   // a copy: the player reuses its buffer

typedef struct Dec {
    uint32_t id;
    vpx_codec_ctx_t vpx;
    int live;
    uint64_t ready_ev;            // guest SystemEventType: submission ready
    uint64_t decoded, acquired;   // frames decoded so far; slots handed out so far
    uint64_t slot_ord[MAX_SLOTS]; // ordinal of the frame each slot holds (0: not acquired)
    uint64_t slot_pool[MAX_SLOTS];
    uint8_t slot_out[MAX_SLOTS];  // acquired and not released
    Frame *frames;                // decoded and not yet released, by ordinal
    int nframes, cframes;
    uint64_t next_slot;           // round-robin start for Acquire
    PlatThread *thread;           // runs libvpx (dec_main)
    PlatCond cv;                  // input queued, stop, a packet done
    Packet *queue;                // submitted and not decoded yet, oldest first
    int nqueue, cqueue, busy, stop;   // busy: dec_main decodes a packet outside the lock
    struct Dec *next;
} Dec;

static Dec *decs;
static uint32_t next_id = 1;

typedef struct Wait { uint32_t id; uint64_t value, ev; struct Wait *next; } Wait;   // RequestSyncPointEvent pending
static Wait *waits;

static Dec *dec_by_id(uint32_t id) {
    for (Dec *d = decs; d; d = d->next)
        if (d->id == id) return d;
    return NULL;
}
static Dec *dec_of(Cpu *c, int arg) {
    uint8_t *o = arg_p(c, arg);
    uint32_t magic, id;
    memcpy(&magic, o + 8, 4), memcpy(&id, o + 12, 4);
    Dec *d = magic == DEC_MAGIC ? dec_by_id(id) : NULL;
    if (!d) rt_fatal("codec: a Vp9DecoderType %p this runtime did not initialize", (void *)o);
    return d;
}
static Frame *frame_of(Dec *d, uint64_t ord) {
    for (int i = 0; i < d->nframes; i++)
        if (d->frames[i].ord == ord) return &d->frames[i];
    return NULL;
}

// Signals the sync point events whose frame now exists (lock held; any thread).
static void wake(void) {
    for (Wait **pp = &waits; *pp;) {
        Wait *w = *pp;
        Dec *d = dec_by_id(w->id);
        if (!d || d->decoded >= w->value) {
            TRACE(T_SIGNAL, "decoder %u frame %llu", w->id, (unsigned long long)w->value);
            kern_signal_system_event(w->ev);
            *pp = w->next;
            free(w);
        } else
            pp = &w->next;
    }
}

// A decoded picture as NV12 (w * h luma, then interleaved CbCr), made outside the lock: 5.5 MB a 2560x1440 frame, which
// ReadDecoderOutputImageNv12 on the player's thread would otherwise wait for (MKW_HOST_TIME, live34).
static uint8_t *nv12_of(const vpx_image_t *img) {
    uint32_t w = img->d_w, h = img->d_h, cw = (w + 1) / 2, ch = (h + 1) / 2;
    uint8_t *out = malloc((size_t)w * h + (size_t)cw * 2 * ch);
    if (!out) rt_fatal("codec: out of memory");
    for (uint32_t y = 0; y < h; y++) memcpy(out + (size_t)y * w, img->planes[0] + (size_t)y * img->stride[0], w);
    uint8_t *uv = out + (size_t)w * h;
    for (uint32_t y = 0; y < ch; y++) {
        const uint8_t *u = img->planes[1] + (size_t)y * img->stride[1], *v = img->planes[2] + (size_t)y * img->stride[2];
        uint8_t *o = uv + (size_t)y * cw * 2;
        for (uint32_t x = 0; x < cw; x++) o[2 * x] = u[x], o[2 * x + 1] = v[x];
    }
    return out;
}

// Appends a frame (lock held). nv12: a picture from nv12_of (taken over) or NULL with eos: the last picture again, or
// none when there was no picture before the end of stream.
static void push_frame(Dec *d, uint8_t *nv12, uint32_t w, uint32_t h, int64_t pts, int eos) {
    if (d->nframes == d->cframes) {
        d->cframes = d->cframes ? 2 * d->cframes : 8;
        if (!(d->frames = realloc(d->frames, sizeof *d->frames * (size_t)d->cframes))) rt_fatal("codec: out of memory");
    }
    const Frame *last = NULL;   // the newest picture (release keeps it; the array is not in decode order)
    for (int i = 0; i < d->nframes; i++)
        if (!last || d->frames[i].ord > last->ord) last = &d->frames[i];
    if (!nv12 && last && last->w) {   // end of stream: the last picture again, marked
        size_t n = (size_t)last->w * last->h * 3 / 2;
        if (!(nv12 = malloc(n))) rt_fatal("codec: out of memory");
        memcpy(nv12, last->nv12, n);
        w = last->w, h = last->h;
    } else if (!nv12) {
        if (!(nv12 = malloc(1))) rt_fatal("codec: out of memory");
        w = h = 0;
    }
    Frame *f = &d->frames[d->nframes++];
    f->ord = ++d->decoded, f->pts = pts, f->eos = (uint8_t)eos;
    f->w = (uint16_t)w, f->h = (uint16_t)h, f->nv12 = nv12;
    TRACE(T_FRAME, "decoder %u frame %llu %ux%u eos %d", d->id, (unsigned long long)f->ord, w, h, eos);
}

static int vpx_open(Dec *d) {
    vpx_codec_dec_cfg_t cfg = {.threads = 4};
    if (vpx_codec_dec_init(&d->vpx, vpx_codec_vp9_dx(), &cfg, 0) != VPX_CODEC_OK) return 0;
    vpx_codec_control(&d->vpx, VP9D_SET_ROW_MT, 1);
    d->live = 1;
    return 1;
}

// Frames ahead of the player: decoded and not taken by an acquired slot, plus packets queued or being decoded.
static uint64_t pending_frames(const Dec *d) {
    return (d->decoded > d->acquired ? d->decoded - d->acquired : 0) + (uint64_t)d->nqueue + (uint64_t)d->busy;
}

static void queue_drop(Dec *d) {   // lock held
    for (int i = 0; i < d->nqueue; i++) free(d->queue[i].data);
    d->nqueue = 0;
}

// The decoder thread: one packet at a time, libvpx outside the lock (only this thread touches d->vpx while busy).
static void dec_main(void *arg) {
    Dec *d = arg;
    plat_mutex_lock(&lock);
    for (;;) {
        while (!d->nqueue && !d->stop) plat_cond_wait(&d->cv, &lock);
        if (d->stop) break;
        Packet p = d->queue[0];
        memmove(d->queue, d->queue + 1, sizeof *d->queue * (size_t)--d->nqueue);
        d->busy = 1;
        plat_mutex_unlock(&lock);
        uint64_t t0 = kern_now_ns();
        vpx_codec_err_t err = p.size ? vpx_codec_decode(&d->vpx, p.data, (unsigned)p.size, NULL, 0) : VPX_CODEC_OK;
        uint64_t took = kern_now_ns() - t0;
        uint8_t *pics[4];
        uint32_t pw[4], ph[4];
        int npics = 0;
        if (p.size) {
            vpx_codec_iter_t it = NULL;
            const vpx_image_t *img;
            while ((img = vpx_codec_get_frame(&d->vpx, &it)) != NULL && npics < 4)
                pics[npics] = nv12_of(img), pw[npics] = img->d_w, ph[npics] = img->d_h, npics++;
        }
        plat_mutex_lock(&lock);
        if (p.size) {
            note_decode(took);
            if (err != VPX_CODEC_OK) {
                static int warned;
                if (warned++ < 8) rt_log("codec: VP9 decoder %u: %s (%s)", d->id, vpx_codec_error(&d->vpx),
                                         vpx_codec_error_detail(&d->vpx) ? vpx_codec_error_detail(&d->vpx) : "");
            }
            for (int k = 0; k < npics; k++) push_frame(d, pics[k], pw[k], ph[k], p.pts, 0);
        }
        if (p.eos) push_frame(d, NULL, 0, 0, p.pts, 1), seqwatch_movie_end();
        free(p.data);
        d->busy = 0;
        // a packet with no picture leaves room the player may be waiting for
        if (pending_frames(d) < AHEAD) kern_signal_system_event(d->ready_ev);
        wake();
        plat_cond_broadcast(&d->cv);
    }
    plat_mutex_unlock(&lock);
}

// ---- nn::codec::video
static void ov_video_init(Cpu *c) { ret_x(c, 0); }
static void ov_video_fini(Cpu *c) { ret_x(c, 0); }

// ---- image pools: VideoDecoderImagePreference is ours ({w, h, format, mode}); the pool object keeps a magic and
// nothing else (frames live in the decoder).
typedef struct { uint16_t w, h; uint32_t format, mode, magic; } Pref;
#define PREF_MAGIC 0x46455250u
static void ov_make_pref(Cpu *c) {   // returned in memory at x8
    Pref *p = (Pref *)(uintptr_t)c->x[8];
    *p = (Pref){(uint16_t)arg_x(c, 0), (uint16_t)arg_x(c, 1), (uint32_t)arg_x(c, 2), (uint32_t)arg_x(c, 3), PREF_MAGIC};
}
// Result CalculateDecoderImagePoolBuffer(size_t *size, size_t *align, const Preference &, unsigned count)
static void ov_pool_size(Cpu *c) {
    const Pref *p = arg_p(c, 2);
    uint64_t n = arg_x(c, 3) ? arg_x(c, 3) : 1;
    *(uint64_t *)arg_p(c, 0) = ((uint64_t)p->w * p->h * 3 / 2 + 0xfff) / 0x1000 * 0x1000 * n;
    *(uint64_t *)arg_p(c, 1) = 0x1000;
    ret_x(c, 0);
}
// Result InitializeDecoderImagePool(PoolType *, void *mem, size_t, const Preference &, unsigned count, CodecMode)
static void ov_pool_init(Cpu *c) {
    uint8_t *o = arg_p(c, 0);
    const Pref *p = arg_p(c, 3);
    memset(o, 0, 0x20);
    memcpy(o, &p->w, 2), memcpy(o + 2, &p->h, 2);
    rt_log("codec: image pool %p %ux%u format %u mode %u count %u", (void *)o, p->w, p->h, p->format, p->mode,
           (unsigned)arg_x(c, 4));
    ret_x(c, 0);
}
static void ov_pool_fini(Cpu *c) { ret_x(c, 0); }
// u32 GetDecoderImagePoolNativeImageHandle(const PoolType *, unsigned index): the player's GPU mode; not served yet.
static void ov_pool_native(Cpu *c) {
    static int warned;
    if (!warned++) rt_log("codec: GetDecoderImagePoolNativeImageHandle (GPU mode of the movie player): handle 0");
    ret_x(c, 0);
}

// The decoder and slot a pool is registered to (lock held).
static Dec *pool_owner(uint64_t pool, int *slot) {
    for (Dec *d = decs; d; d = d->next)
        for (int i = 0; i < MAX_SLOTS; i++)
            if (d->slot_pool[i] == pool) return *slot = i, d;
    return NULL;
}

// Result ReadDecoderOutputImageNv12(const PoolType *, unsigned index, u16 x, u16 y, u16 w, u16 h, void *luma,
// size_t luma_pitch, size_t luma_size, void *chroma, size_t chroma_pitch, size_t chroma_size)
static volatile uint64_t movie_at;   // 1 + the tick of the last frame read of a full-screen movie; 0 none yet

int ov_movie_playing(void) {
    uint64_t at = movie_at;
    return at && frame_step_ticks() + 1 - at < 30;
}

static void ov_read_nv12(Cpu *c) {
    uint64_t pool = arg_x(c, 0);
    uint32_t x0 = (uint16_t)arg_x(c, 2), y0 = (uint16_t)arg_x(c, 3), w = (uint16_t)arg_x(c, 4), h = (uint16_t)arg_x(c, 5);
    uint8_t *ly = arg_p(c, 6), *lc = (uint8_t *)(uintptr_t)arg_x(c, 9);
    // pointer, pitch, size for each plane (MKW's wrapper main+0x19e7950: x7 = width, then w*h on the stack; run-race22
    // read them the other way round and wrote one row: a green movie)
    uint64_t ypitch = arg_x(c, 7), ysize = arg_x(c, 8), cpitch = arg_x(c, 10), csize = arg_x(c, 11);
    plat_mutex_lock(&lock);
    int slot;
    Dec *d = pool_owner(pool, &slot);
    const Frame *f = d ? frame_of(d, d->slot_ord[slot]) : NULL;
    TRACE(T_READ, "pool %#llx %ux%u at %u,%u: %s; luma %p+%#llx pitch %llu chroma %p+%#llx pitch %llu (GPU pool %p)",
          (unsigned long long)pool, w, h, x0, y0, !d ? "no decoder" : f ? "frame" : "no frame", (void *)ly,
          (unsigned long long)ysize, (unsigned long long)ypitch, (void *)lc, (unsigned long long)csize,
          (unsigned long long)cpitch, (const void *)nvn_pool_find((uintptr_t)ly, 1));
    if (ysize) memset(ly, 0x10, ysize);
    if (csize) memset(lc, 0x80, csize);
    if (f && f->w >= 1280) movie_at = frame_step_ticks() + 1;
    if (f && f->w && f->h) {
        for (uint32_t y = 0; y < h && y0 + y < f->h && (y + 1) * ypitch <= ysize; y++) {
            uint32_t n = x0 < f->w ? f->w - x0 : 0;
            memcpy(ly + y * ypitch, f->nv12 + (size_t)(y0 + y) * f->w + x0, n < w ? n : w);
        }
        uint32_t cw = (f->w + 1) / 2 * 2, ch = (f->h + 1) / 2;
        const uint8_t *uv = f->nv12 + (size_t)f->w * f->h;
        for (uint32_t y = 0; y < (h + 1) / 2 && y0 / 2 + y < ch && (y + 1) * cpitch <= csize; y++) {
            uint32_t n = x0 < cw ? cw - x0 : 0;
            memcpy(lc + y * cpitch, uv + (size_t)(y0 / 2 + y) * cw + x0, n < w ? n : w);
        }
    }
    plat_mutex_unlock(&lock);
    // Host writes: the write watch does not see them, and the texture the player draws from this memory would keep
    // its first upload (a green screen: NV12 zeros, run-race4).
    if (ysize) nvn_guest_written((uintptr_t)ly, ysize);
    if (csize) nvn_guest_written((uintptr_t)lc, csize);
    ret_x(c, 0);
}

// ---- the VP9 decoder
// Result CalculateVp9DecoderWorkBuffer(size_t *size, size_t *align, size_t, u16 w, u16 h): nothing is kept there.
static void ov_vp9_work(Cpu *c) {
    *(uint64_t *)arg_p(c, 0) = 0x1000;
    *(uint64_t *)arg_p(c, 1) = 0x1000;
    ret_x(c, 0);
}
static void vp9_init(Cpu *c, uint8_t *o, uint32_t w, uint32_t h) {
    fns();
    Dec *d = calloc(1, sizeof *d);
    if (!d) rt_fatal("codec: out of memory");
    if (!vpx_open(d)) rt_fatal("codec: libvpx VP9 decoder init failed");
    plat_mutex_lock(&lock);
    d->id = next_id++;
    d->next = decs, decs = d;
    plat_mutex_unlock(&lock);
    d->ready_ev = event_new(c);
    rt_call(c, fn_signal, 1, d->ready_ev);
    if (!(d->thread = plat_thread_new(dec_main, d, "vp9 decode"))) rt_fatal("codec: cannot start the decoder thread");
    uint32_t magic = DEC_MAGIC;
    memcpy(o + 8, &magic, 4), memcpy(o + 12, &d->id, 4);
    rt_log("codec: VP9 decoder %u (%ux%u) on the host", d->id, w, h);
}
// Result InitializeVp9Decoder(Vp9DecoderType *, void *work, size_t, size_t, u16 w, u16 h, unsigned, in, out)
static void ov_vp9_init_work(Cpu *c) { vp9_init(c, arg_p(c, 0), (uint16_t)arg_x(c, 4), (uint16_t)arg_x(c, 5)); ret_x(c, 0); }
// Result InitializeVp9Decoder(Vp9DecoderType *, size_t, size_t, u16 w, u16 h, unsigned, in, out)
static void ov_vp9_init(Cpu *c) { vp9_init(c, arg_p(c, 0), (uint16_t)arg_x(c, 3), (uint16_t)arg_x(c, 4)); ret_x(c, 0); }

static void ov_vp9_fini(Cpu *c) {
    Dec *d = dec_of(c, 0);
    uint8_t *o = arg_p(c, 0);
    plat_mutex_lock(&lock);
    for (Dec **pp = &decs; *pp; pp = &(*pp)->next)
        if (*pp == d) { *pp = d->next; break; }
    d->stop = 1;
    plat_cond_broadcast(&d->cv);
    plat_mutex_unlock(&lock);
    plat_thread_join(d->thread);
    queue_drop(d);
    free(d->queue);
    if (d->live) vpx_codec_destroy(&d->vpx);
    for (int i = 0; i < d->nframes; i++) free(d->frames[i].nv12);
    free(d->frames);
    event_free(c, d->ready_ev);
    memset(o + 8, 0, 8);
    rt_log("codec: VP9 decoder %u finalized", d->id);
    free(d);
    skip_movie_closed();   // core/hacks/skip.c: MKW_SKIP_MOVIES stops pressing
    ret_x(c, 0);
}

// Result RegisterVp9DecoderBuffer(Vp9DecoderType *, unsigned slot, PoolType *, unsigned index)
static void ov_vp9_register(Cpu *c) {
    Dec *d = dec_of(c, 0);
    uint32_t slot = (uint32_t)arg_x(c, 1);
    if (slot >= MAX_SLOTS) rt_fatal("codec: decoder buffer slot %u", slot);
    plat_mutex_lock(&lock);
    d->slot_pool[slot] = arg_x(c, 2);
    plat_mutex_unlock(&lock);
    ret_x(c, 0);
}
static void ov_vp9_ok(Cpu *c) { (void)dec_of(c, 0); ret_x(c, 0); }   // Start, Stop

static void ov_vp9_flush(Cpu *c) {   // drops what was submitted and not decoded
    Dec *d = dec_of(c, 0);
    plat_mutex_lock(&lock);
    queue_drop(d);
    while (d->busy) plat_cond_wait(&d->cv, &lock);
    if (d->live) vpx_codec_destroy(&d->vpx), d->live = 0;
    vpx_open(d);
    plat_mutex_unlock(&lock);
    ret_x(c, 0);
}

// Result GetVp9DecoderSubmissionReadyEvent(SystemEventType **, Vp9DecoderType *)
static void ov_vp9_ready_event(Cpu *c) {
    Dec *d = dec_of(c, 1);
    *(uint64_t *)arg_p(c, 0) = d->ready_ev;
    ret_x(c, 0);
}

// Result SubmitVp9DecoderInputBuffer(u64 *, Vp9DecoderType *, const void *data, size_t, TimeSpan pts, bool eos)
static void ov_vp9_submit(Cpu *c) {
    Dec *d = dec_of(c, 1);
    const uint8_t *data = arg_p(c, 2);
    uint64_t size = arg_x(c, 3);
    int64_t pts = (int64_t)arg_x(c, 4);
    int eos = (int)(arg_x(c, 5) & 1);
    uint64_t *consumed = arg_p(c, 0);
    plat_mutex_lock(&lock);
    TRACE(T_SUBMIT, "decoder %u %llu bytes pts %lld eos %d (decoded %llu, acquired %llu)", d->id,
          (unsigned long long)size, (long long)pts, eos, (unsigned long long)d->decoded, (unsigned long long)d->acquired);
    if (pending_frames(d) >= AHEAD) {
        TRACE(T_BUSY, "decoder %u", d->id);
        rt_call(c, fn_clear, 1, d->ready_ev);
        plat_mutex_unlock(&lock);
        ret_x(c, R_BUSY);
        return;
    }
    if (size || eos) {
        if (d->nqueue == d->cqueue) {
            d->cqueue = d->cqueue ? 2 * d->cqueue : 8;
            if (!(d->queue = realloc(d->queue, sizeof *d->queue * (size_t)d->cqueue))) rt_fatal("codec: out of memory");
        }
        Packet *p = &d->queue[d->nqueue++];
        *p = (Packet){malloc(size ? size : 1), size, pts, eos};
        if (!p->data) rt_fatal("codec: out of memory");
        if (size) memcpy(p->data, data, size);
        plat_cond_broadcast(&d->cv);
    }
    if (pending_frames(d) >= AHEAD) rt_call(c, fn_clear, 1, d->ready_ev);
    *consumed = size;
    plat_mutex_unlock(&lock);
    ret_x(c, 0);
}

// Result AcquireVp9DecoderOutputBuffer(unsigned *slot, NativeSyncHandle *, Vp9DecoderType *)
static void ov_vp9_acquire(Cpu *c) {
    Dec *d = dec_of(c, 2);
    plat_mutex_lock(&lock);
    int slot = -1;
    for (int k = 0; k < MAX_SLOTS && slot < 0; k++) {
        int i = (int)((d->next_slot + (uint64_t)k) % MAX_SLOTS);
        if (d->slot_pool[i] && !d->slot_out[i]) slot = i;
    }
    if (slot < 0) rt_fatal("codec: VP9 decoder %u: Acquire with every registered slot out", d->id);
    d->next_slot = (uint64_t)slot + 1;
    d->slot_out[slot] = 1;
    d->slot_ord[slot] = ++d->acquired;
    *(uint32_t *)arg_p(c, 0) = (uint32_t)slot;
    uint64_t *h = arg_p(c, 1);
    h[0] = d->id, h[1] = d->acquired;
    TRACE(T_ACQUIRE, "decoder %u slot %d frame %llu (decoded %llu)", d->id, slot, (unsigned long long)d->acquired,
          (unsigned long long)d->decoded);
    if (pending_frames(d) < AHEAD) rt_call(c, fn_signal, 1, d->ready_ev);
    plat_mutex_unlock(&lock);
    ret_x(c, 0);
}

// Result ReleaseVp9DecoderOutputBuffer(Vp9DecoderType *, unsigned slot, NativeSyncHandle)
static void ov_vp9_release(Cpu *c) {
    Dec *d = dec_of(c, 0);
    uint32_t slot = (uint32_t)arg_x(c, 1);
    plat_mutex_lock(&lock);
    TRACE(T_RELEASE, "decoder %u slot %u", d->id, slot);
    if (slot < MAX_SLOTS && d->slot_out[slot]) {
        // the newest decoded picture stays: an end of stream decoded after this release repeats it (push_frame)
        uint64_t ord = d->slot_ord[slot];
        for (int i = 0; i < d->nframes; i++)
            if (d->frames[i].ord <= ord && d->frames[i].ord != d->decoded)
                free(d->frames[i].nv12), d->frames[i--] = d->frames[--d->nframes];
        d->slot_out[slot] = 0;
    }
    plat_mutex_unlock(&lock);
    ret_x(c, 0);
}

// Result GetVp9DecoderOutputBufferProperties(u16 *w, u16 *h, u16 *, u16 *, ColorSpace *, TimeSpan *pts, bool *eos,
// Vp9DecoderType *, unsigned slot); 1.8.0's SDK has no ColorSpace * (cs < 0): every later argument moves up one.
static void vp9_props(Cpu *c, int cs) {
    int k = cs < 0 ? -1 : 0;
    Dec *d = dec_of(c, 7 + k);
    uint32_t slot = (uint32_t)arg_x(c, 8 + k);
    plat_mutex_lock(&lock);
    const Frame *f = slot < MAX_SLOTS ? frame_of(d, d->slot_ord[slot]) : NULL;
    uint16_t w = f ? f->w : 0, h = f ? f->h : 0;
    TRACE(T_PROPS, "decoder %u slot %u frame %llu: %s", d->id, slot,
          (unsigned long long)(slot < MAX_SLOTS ? d->slot_ord[slot] : 0), f ? "decoded" : "missing");
    *(uint16_t *)arg_p(c, 0) = w, *(uint16_t *)arg_p(c, 1) = h;
    *(uint16_t *)arg_p(c, 2) = w, *(uint16_t *)arg_p(c, 3) = h;
    if (cs >= 0) *(uint32_t *)arg_p(c, cs) = 0;
    *(int64_t *)arg_p(c, 5 + k) = f ? f->pts : 0;
    *(uint8_t *)arg_p(c, 6 + k) = f ? f->eos : 0;
    plat_mutex_unlock(&lock);
    ret_x(c, 0);
}
static void ov_vp9_props(Cpu *c) { vp9_props(c, 4); }
static void ov_vp9_props_180(Cpu *c) { vp9_props(c, -1); }

// ---- 1.8.0's SDK: the movie library of subsdk0 (movie::detail, subsdk0+0xf770..0xff80) plays the movies instead of
// main's own player, through newer names of the same calls; what they do here is the same.
// Result InitializeVp9DecoderByRemoteMemory(Vp9DecoderType *, size_t, u16 w, u16 h, unsigned, in, out, OutputMode)
static void ov_vp9_init_remote(Cpu *c) { vp9_init(c, arg_p(c, 0), (uint16_t)arg_x(c, 2), (uint16_t)arg_x(c, 3)); ret_x(c, 0); }
// SystemEventType *GetVp9DecoderSubmissionReadyEvent(Vp9DecoderType *): the event itself (subsdk0+0x3430 keeps x0)
static void ov_vp9_ready_event_180(Cpu *c) { ret_x(c, dec_of(c, 0)->ready_ev); }

// Result ReadDecoderOutputImageRgba(const PoolType *, unsigned index, u16 x, u16 y, u16 w, u16 h, void *out,
// size_t pitch, size_t size): the movie library's RGBA mode (its pool's format 2, subsdk0+0xf918: pitch w * 4). The
// NV12 frame to RGBA8 with BT.709's limited-range matrix, the one VP9 movies are made with.
static uint8_t clamp8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }
static void ov_read_rgba(Cpu *c) {
    uint64_t pool = arg_x(c, 0);
    uint32_t x0 = (uint16_t)arg_x(c, 2), y0 = (uint16_t)arg_x(c, 3), w = (uint16_t)arg_x(c, 4), h = (uint16_t)arg_x(c, 5);
    uint8_t *out = arg_p(c, 6);
    uint64_t pitch = arg_x(c, 7), size = arg_x(c, 8);
    plat_mutex_lock(&lock);
    int slot;
    Dec *d = pool_owner(pool, &slot);
    const Frame *f = d ? frame_of(d, d->slot_ord[slot]) : NULL;
    TRACE(T_READ, "pool %#llx %ux%u at %u,%u: %s; rgba %p pitch %llu size %llu", (unsigned long long)pool, w, h, x0, y0,
          !d ? "no decoder" : f ? "frame" : "no frame", (void *)out, (unsigned long long)pitch, (unsigned long long)size);
    if (size) memset(out, 0, size);
    if (f && f->w >= 1280) movie_at = frame_step_ticks() + 1;
    if (f && f->w && f->h) {
        uint32_t cw = (f->w + 1) / 2 * 2;
        const uint8_t *uv = f->nv12 + (size_t)f->w * f->h;
        for (uint32_t y = 0; y < h && y0 + y < f->h && (y + 1) * pitch <= size; y++) {
            uint8_t *o = out + y * pitch;
            const uint8_t *ly = f->nv12 + (size_t)(y0 + y) * f->w, *lc = uv + (size_t)((y0 + y) / 2) * cw;
            for (uint32_t x = 0; x < w && x0 + x < f->w; x++) {
                int Y = (ly[x0 + x] - 16) * 298, U = lc[(x0 + x) & ~1u] - 128, V = lc[(x0 + x) | 1u] - 128;
                o[4 * x + 0] = clamp8((Y + 459 * V + 128) >> 8);
                o[4 * x + 1] = clamp8((Y - 55 * U - 136 * V + 128) >> 8);
                o[4 * x + 2] = clamp8((Y + 541 * U + 128) >> 8);
                o[4 * x + 3] = 255;
            }
        }
    }
    plat_mutex_unlock(&lock);
    if (size) nvn_guest_written((uintptr_t)out, size);   // as ov_read_nv12: the write watch does not see host writes
    ret_x(c, 0);
}

static void ov_other_codec(Cpu *c) {
    (void)c;
    rt_fatal("codec: H.264, AV1 or VP8 video decoder: only VP9 is served (runtime/sdk/ov/ov_codec.c)");
}

// ---- nn::syncpt::sdk: a SyncPointEventType holds the address of a guest SystemEventType at +0.
static uint64_t sp_event(Cpu *c, int arg) { return *(uint64_t *)arg_p(c, arg); }
// Arguments are read before any rt_call: it leaves the guest function's result in x0.
static void ov_sp_create(Cpu *c) {
    uint64_t *o = arg_p(c, 0);
    fns();
    *o = event_new(c);
    ret_x(c, 0);
}
static void ov_sp_destroy(Cpu *c) {
    uint64_t *o = arg_p(c, 0), ev = *o;
    plat_mutex_lock(&lock);
    for (Wait **pp = &waits; *pp;)
        if ((*pp)->ev == ev) { Wait *w = *pp; *pp = w->next; free(w); } else pp = &(*pp)->next;
    plat_mutex_unlock(&lock);
    event_free(c, ev);
    *o = 0;
}
static void ov_sp_holder(Cpu *c) {
    uint64_t holder = arg_x(c, 0), ev = sp_event(c, 1);
    rt_call(c, fn_holder, 2, holder, ev);
}
static void ov_sp_trywait(Cpu *c) { ret_x(c, rt_call(c, fn_trywait, 1, sp_event(c, 0)) & 1); }
static void ov_sp_cancel(Cpu *c) {
    uint64_t ev = sp_event(c, 0);
    plat_mutex_lock(&lock);
    for (Wait **pp = &waits; *pp;)
        if ((*pp)->ev == ev) { Wait *w = *pp; *pp = w->next; free(w); } else pp = &(*pp)->next;
    plat_mutex_unlock(&lock);
    rt_call(c, fn_clear, 1, ev);
}
// void RequestSyncPointEvent(SyncPointEventType *, const SyncPointHandle &): {decoder id, frame ordinal}
static void ov_sp_request(Cpu *c) {
    uint64_t ev = sp_event(c, 0);
    const uint64_t *h = arg_p(c, 1);
    plat_mutex_lock(&lock);
    rt_call(c, fn_clear, 1, ev);
    TRACE(T_REQUEST, "decoder %u frame %llu", (unsigned)h[0], (unsigned long long)h[1]);
    Wait *w = malloc(sizeof *w);
    if (!w) rt_fatal("codec: out of memory");
    *w = (Wait){(uint32_t)h[0], h[1], ev, waits};
    waits = w;
    wake();
    plat_mutex_unlock(&lock);
}

HOST_OVERRIDE("_ZN2nn5codec5video10InitializeEv", ov_video_init);
HOST_OVERRIDE("_ZN2nn5codec5video8FinalizeEv", ov_video_fini);
HOST_OVERRIDE("_ZN2nn5codec25MakeDecodeImagePreferenceEttNS0_24VideoDecoderOutputFormatENS0_9CodecModeE", ov_make_pref);
HOST_OVERRIDE("_ZN2nn5codec3sdk34MakeDecodeImagePreferenceForCompatEttNS0_24VideoDecoderOutputFormatENS0_9CodecModeE", ov_make_pref);
HOST_OVERRIDE("_ZN2nn5codec31CalculateDecoderImagePoolBufferEPmS1_RKNS0_27VideoDecoderImagePreferenceEj", ov_pool_size);
HOST_OVERRIDE("_ZN2nn5codec26InitializeDecoderImagePoolEPNS0_25VideoDecoderImagePoolTypeEPvmRKNS0_27VideoDecoderImagePreferenceEjNS0_9CodecModeE", ov_pool_init);
HOST_OVERRIDE("_ZN2nn5codec24FinalizeDecoderImagePoolEPNS0_25VideoDecoderImagePoolTypeE", ov_pool_fini);
HOST_OVERRIDE("_ZN2nn5codec36GetDecoderImagePoolNativeImageHandleEPKNS0_25VideoDecoderImagePoolTypeEj", ov_pool_native);
HOST_OVERRIDE("_ZN2nn5codec26ReadDecoderOutputImageNv12EPKNS0_25VideoDecoderImagePoolTypeEjttttPvmmS4_mm", ov_read_nv12);
HOST_OVERRIDE("_ZN2nn5codec29CalculateVp9DecoderWorkBufferEPmS1_mtt", ov_vp9_work);
HOST_OVERRIDE("_ZN2nn5codec20InitializeVp9DecoderEPNS0_14Vp9DecoderTypeEPvmmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatE", ov_vp9_init_work);
HOST_OVERRIDE("_ZN2nn5codec20InitializeVp9DecoderEPNS0_14Vp9DecoderTypeEmmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatE", ov_vp9_init);
HOST_OVERRIDE("_ZN2nn5codec18FinalizeVp9DecoderEPNS0_14Vp9DecoderTypeE", ov_vp9_fini);
HOST_OVERRIDE("_ZN2nn5codec24RegisterVp9DecoderBufferEPNS0_14Vp9DecoderTypeEjPNS0_25VideoDecoderImagePoolTypeEj", ov_vp9_register);
HOST_OVERRIDE("_ZN2nn5codec15StartVp9DecoderEPNS0_14Vp9DecoderTypeE", ov_vp9_ok);
HOST_OVERRIDE("_ZN2nn5codec14StopVp9DecoderEPNS0_14Vp9DecoderTypeE", ov_vp9_ok);
HOST_OVERRIDE("_ZN2nn5codec15FlushVp9DecoderEPNS0_14Vp9DecoderTypeE", ov_vp9_flush);
HOST_OVERRIDE("_ZN2nn5codec33GetVp9DecoderSubmissionReadyEventEPPNS_2os15SystemEventTypeEPNS0_14Vp9DecoderTypeE", ov_vp9_ready_event);
HOST_OVERRIDE("_ZN2nn5codec27SubmitVp9DecoderInputBufferEPmPNS0_14Vp9DecoderTypeEPKvmNS_8TimeSpanEb", ov_vp9_submit);
HOST_OVERRIDE("_ZN2nn5codec29AcquireVp9DecoderOutputBufferEPjPNS_2gs16NativeSyncHandleEPNS0_14Vp9DecoderTypeE", ov_vp9_acquire);
HOST_OVERRIDE("_ZN2nn5codec29ReleaseVp9DecoderOutputBufferEPNS0_14Vp9DecoderTypeEjNS_2gs16NativeSyncHandleE", ov_vp9_release);
HOST_OVERRIDE("_ZN2nn5codec35GetVp9DecoderOutputBufferPropertiesEPtS1_S1_S1_PNS_5movie10ColorSpaceEPNS_8TimeSpanEPbPNS0_14Vp9DecoderTypeEj", ov_vp9_props);
HOST_OVERRIDE("_ZN2nn5codec21InitializeH264DecoderEPNS0_15H264DecoderTypeEPvmmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatEPKvm", ov_other_codec);
HOST_OVERRIDE("_ZN2nn5codec21InitializeH264DecoderEPNS0_15H264DecoderTypeEmmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatEPKvm", ov_other_codec);
HOST_OVERRIDE("_ZN2nn5codec20InitializeAv1DecoderEPNS0_14Av1DecoderTypeEmmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatE", ov_other_codec);
HOST_OVERRIDE("_ZN2nn5codec20InitializeVp8DecoderEPNS0_14Vp8DecoderTypeEmmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatE", ov_other_codec);
HOST_OVERRIDE("_ZN2nn5codec20InitializeAv1DecoderEPNS0_14Av1DecoderTypeEPvmmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatE", ov_other_codec);
HOST_OVERRIDE("_ZN2nn5codec20InitializeVp8DecoderEPNS0_14Vp8DecoderTypeEPvmmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatE", ov_other_codec);
// bool nn::gs::TryWaitNativeSync(const NativeSyncHandle &) and nn::syncpt::sdk::TryWaitSyncPoint(const
// SyncPointHandle &): 1.8.0's movie library polls an acquired frame's handle ({decoder id, frame ordinal}, as
// ov_vp9_acquire hands it out) instead of waiting on its event; the SDK's versions reach the gs driver (a null pointer
// from sdk+0x5ca424). Done when that frame is decoded, or its decoder is gone.
static void ov_sync_trywait(Cpu *c) {
    const uint64_t *h = arg_p(c, 0);
    plat_mutex_lock(&lock);
    Dec *d = dec_by_id((uint32_t)h[0]);
    int done = !d || d->decoded >= h[1];
    plat_mutex_unlock(&lock);
    ret_x(c, done);
}
HOST_OVERRIDE("_ZN2nn2gs17TryWaitNativeSyncERKNS0_16NativeSyncHandleE", ov_sync_trywait);
HOST_OVERRIDE("_ZN2nn6syncpt3sdk16TryWaitSyncPointERKNS1_15SyncPointHandleE", ov_sync_trywait);
// 1.8.0's SDK (the movie library of subsdk0)
HOST_OVERRIDE("_ZN2nn5codec25MakeDecodeImagePreferenceEttNS0_24VideoDecoderOutputFormatENS0_15ImageTilingModeE", ov_make_pref);
HOST_OVERRIDE("_ZN2nn5codec26InitializeDecoderImagePoolEPNS0_25VideoDecoderImagePoolTypeEPvmRKNS0_27VideoDecoderImagePreferenceEj", ov_pool_init);
HOST_OVERRIDE("_ZN2nn5codec26ReadDecoderOutputImageRgbaEPKNS0_25VideoDecoderImagePoolTypeEjttttPvmm", ov_read_rgba);
HOST_OVERRIDE("_ZN2nn5codec36InitializeVp9DecoderByTransferMemoryEPNS0_14Vp9DecoderTypeEPvmmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatENS0_22VideoDecoderOutputModeE", ov_vp9_init_work);
HOST_OVERRIDE("_ZN2nn5codec34InitializeVp9DecoderByRemoteMemoryEPNS0_14Vp9DecoderTypeEmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatENS0_22VideoDecoderOutputModeE", ov_vp9_init_remote);
HOST_OVERRIDE("_ZN2nn5codec33GetVp9DecoderSubmissionReadyEventEPNS0_14Vp9DecoderTypeE", ov_vp9_ready_event_180);
HOST_OVERRIDE("_ZN2nn5codec35GetVp9DecoderOutputBufferPropertiesEPtS1_S1_S1_PNS_8TimeSpanEPbPNS0_14Vp9DecoderTypeEj", ov_vp9_props_180);
HOST_OVERRIDE("_ZN2nn5codec37InitializeH264DecoderByTransferMemoryEPNS0_15H264DecoderTypeEPvmmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatENS0_22VideoDecoderOutputModeEPKvm", ov_other_codec);
HOST_OVERRIDE("_ZN2nn5codec35InitializeH264DecoderByRemoteMemoryEPNS0_15H264DecoderTypeEmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatENS0_22VideoDecoderOutputModeEPKvm", ov_other_codec);
HOST_OVERRIDE("_ZN2nn5codec36InitializeAv1DecoderByTransferMemoryEPNS0_14Av1DecoderTypeEPvmmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatENS0_22VideoDecoderOutputModeE", ov_other_codec);
HOST_OVERRIDE("_ZN2nn5codec34InitializeAv1DecoderByRemoteMemoryEPNS0_14Av1DecoderTypeEmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatENS0_22VideoDecoderOutputModeE", ov_other_codec);
HOST_OVERRIDE("_ZN2nn5codec36InitializeVp8DecoderByTransferMemoryEPNS0_14Vp8DecoderTypeEPvmmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatENS0_22VideoDecoderOutputModeE", ov_other_codec);
HOST_OVERRIDE("_ZN2nn5codec34InitializeVp8DecoderByRemoteMemoryEPNS0_14Vp8DecoderTypeEmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatENS0_22VideoDecoderOutputModeE", ov_other_codec);
HOST_OVERRIDE("_ZN2nn6syncpt3sdk20CreateSyncPointEventEPNS1_18SyncPointEventTypeE", ov_sp_create);
HOST_OVERRIDE("_ZN2nn6syncpt3sdk21DestroySyncPointEventEPNS1_18SyncPointEventTypeE", ov_sp_destroy);
HOST_OVERRIDE("_ZN2nn6syncpt3sdk25InitializeMultiWaitHolderEPNS_2os19MultiWaitHolderTypeEPNS1_18SyncPointEventTypeE", ov_sp_holder);
HOST_OVERRIDE("_ZN2nn6syncpt3sdk21TryWaitSyncPointEventEPKNS1_18SyncPointEventTypeE", ov_sp_trywait);
HOST_OVERRIDE("_ZN2nn6syncpt3sdk20CancelSyncPointEventEPNS1_18SyncPointEventTypeE", ov_sp_cancel);
HOST_OVERRIDE("_ZN2nn6syncpt3sdk21RequestSyncPointEventEPNS1_18SyncPointEventTypeERKNS1_15SyncPointHandleE", ov_sp_request);
