// --probe-audio: the SDK's own nn::audio client (lifted sdk code, called through main's PLT stubs) against the host's
// audio objects (audren_sf.c): parameter, work buffer size, OpenAudioRenderer, a render loop of WaitSystemEvent +
// RequestUpdateAudioRenderer timed against the 5 ms frame, Stop and Close. Taken from the SDK client part of the
// former --probe-ipc (ipc.c), whose hand-built HIPC requests went with the IPC layer.
#include <stdio.h>
#include <string.h>

#include "audren.h"
#include "host.h"
#include "platform/platform.h"

#define PROBE_FRAMES 200
#define SYM_INIT_PARAM "_ZN2nn5audio32InitializeAudioRendererParameterEPNS0_22AudioRendererParameterE"
#define SYM_WORK_SIZE "_ZN2nn5audio30GetAudioRendererWorkBufferSizeERKNS0_22AudioRendererParameterE"
#define SYM_OPEN "_ZN2nn5audio17OpenAudioRendererEPNS0_19AudioRendererHandleEPNS_2os11SystemEventERKNS0_22AudioRendererParameterEPvm"
#define SYM_CONFIG_SIZE "_ZN2nn5audio36GetAudioRendererConfigWorkBufferSizeERKNS0_22AudioRendererParameterE"
#define SYM_CONFIG_INIT "_ZN2nn5audio29InitializeAudioRendererConfigEPNS0_19AudioRendererConfigERKNS0_22AudioRendererParameterEPvm"
#define SYM_START "_ZN2nn5audio18StartAudioRendererENS0_19AudioRendererHandleE"
#define SYM_STOP "_ZN2nn5audio17StopAudioRendererENS0_19AudioRendererHandleE"
#define SYM_UPDATE "_ZN2nn5audio26RequestUpdateAudioRendererENS0_19AudioRendererHandleEPKNS0_19AudioRendererConfigE"
#define SYM_FRAMES "_ZN2nn5audio33GetAudioRendererElapsedFrameCountEPKNS0_19AudioRendererConfigE"
#define SYM_CLOSE "_ZN2nn5audio18CloseAudioRendererENS0_19AudioRendererHandleE"
#define SYM_WAIT_EVENT "_ZN2nn2os15WaitSystemEventEPNS0_15SystemEventTypeE"
#define SYM_DEVICE_NAME "_ZN2nn5audio30GetActiveAudioOutputDeviceNameEPNS0_15AudioDeviceNameE"
#define SYM_CHANNELS "_ZN2nn5audio41GetActiveAudioDeviceChannelCountForOutputEv"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("  FAIL: " __VA_ARGS__); putchar('\n'); } } while (0)

static int revision_number(uint32_t rev) { return (int)(rev >> 24) - '0'; }

static uint64_t stub(const char *sym) {
    uint64_t a = ldr_plt_stub("main", sym);
    if (!a) rt_fatal("probe-audio: no PLT stub for %s in main", sym);
    return a;
}

// Without nn::init::Start the SDK's own initialisation has not run; the probe sets up the two things the audio client
// path reads (found by running it):
// - the "current thread object" at TLR+0x1F8 is the kernel's zeroed placeholder. SdkMutex compares its lock tag with
//   ThreadType+0x1B0 (sdk+0x13186c), which nnosInitialize sets to the thread's handle; a zero there makes every free
//   mutex look owned by the caller (__cxa_guard_acquire aborts);
// - sdk's full DT_INIT needs nn::os initialised (its TLS slot constructor reaches ThreadManager), which is the boot
//   sequence's job. OpenAudioRenderer takes nn::audio::detail::g_RendererResourceSemaphore first, which the audio
//   constructor at sdk+0x1f7f00 initializes (GOT sdk+0xd78a58, 2, 2).
static void sdk_prepare(Cpu *c) {
    Cpu *again;
    ST32(LD64(c->tpidrro + 0x1F8) + 0x1B0, kern_main_thread(&again));
    const RtLoaded *sdk = ldr_module(2);
    const uint64_t ctor = 0x1f7f00;
    int found = 0;
    for (uint64_t e = 0; e < sdk->init_array_size; e += 8) found |= LD64(sdk->init_array + e) == sdk->base + ctor;
    if (!found) rt_fatal("probe-audio: sdk+%#llx is not an sdk constructor", (unsigned long long)ctor);
    rt_call(c, sdk->base + ctor, 0);
}

static void probe_client(Cpu *c) {
    sdk_prepare(c);
    int32_t *param = mem_kernel_alloc(0x100, 64);
    rt_call(c, stub(SYM_INIT_PARAM), 1, (uint64_t)(uintptr_t)param);
    printf("sdk: InitializeAudioRendererParameter -> %d Hz x%d, mix buffers %d, revision word %#x (REV%d)\n", param[0],
           param[1], param[2], (uint32_t)param[13], revision_number((uint32_t)param[13]));
    // Switch 2 spells it "REv" (lowercase v; eden-nx2 NX2_NOTES.md row 39): only the revision byte is compared
    CHECK(param[0] == 48000 && param[1] == 240 && (uint32_t)param[13] == (AUDREN_REV(15) | 0x200000u),
          "SDK default parameter (MKW's SDK 19.3.3 builds REv15 requests)");
    // A renderer like MKW's: final mix plus submixes, two sinks
    param[2] = 24, param[3] = 8, param[4] = 192, param[5] = 2, param[6] = 12;
    uint64_t work = rt_call(c, stub(SYM_WORK_SIZE), 1, (uint64_t)(uintptr_t)param);
    printf("sdk: GetAudioRendererWorkBufferSize -> %#llx (host %#x + client part)\n", (unsigned long long)work,
           AUDREN_WORK_BUFFER_BYTES);
    CHECK(work >= AUDREN_WORK_BUFFER_BYTES && work < (64u << 20), "SDK GetAudioRendererWorkBufferSize %#llx",
          (unsigned long long)work);
    uint8_t *wb = mem_kernel_alloc(work, 0x1000), *event = mem_kernel_alloc(0x100, 64);
    uint64_t *handle = mem_kernel_alloc(0x40, 64);   // AudioRendererHandle: 16 bytes, passed in x0 and x1
    uint32_t r = (uint32_t)rt_call(c, stub(SYM_OPEN), 5, (uint64_t)(uintptr_t)handle, (uint64_t)(uintptr_t)event,
                                   (uint64_t)(uintptr_t)param, (uint64_t)(uintptr_t)wb, work);
    printf("sdk: OpenAudioRenderer -> %#x, renderers running %ld\n", r, audren_renderers_live());
    CHECK(!r && handle[0] && audren_renderers_live() == 1, "SDK OpenAudioRenderer %#x", r);
    if (r) return;
    uint64_t csize = rt_call(c, stub(SYM_CONFIG_SIZE), 1, (uint64_t)(uintptr_t)param);
    uint8_t *cbuf = mem_kernel_alloc(csize, 0x1000), *config = mem_kernel_alloc(0x200, 64);
    rt_call(c, stub(SYM_CONFIG_INIT), 4, (uint64_t)(uintptr_t)config, (uint64_t)(uintptr_t)param,
            (uint64_t)(uintptr_t)cbuf, csize);
    rt_call(c, stub(SYM_START), 2, handle[0], handle[1]);
    uint64_t t0 = 0, t1 = 0;
    int bad = 0, f0 = 0, f1 = 0;
    for (int i = 0; i < PROBE_FRAMES; i++) {
        rt_call(c, stub(SYM_WAIT_EVENT), 1, (uint64_t)(uintptr_t)event);
        uint64_t now = kern_now_ns();
        bad += rt_call(c, stub(SYM_UPDATE), 3, handle[0], handle[1], (uint64_t)(uintptr_t)config) != 0;
        int f = (int)rt_call(c, stub(SYM_FRAMES), 1, (uint64_t)(uintptr_t)config);
        if (!i) t0 = now, f0 = f;
        t1 = now, f1 = f;
    }
    double period = (double)(t1 - t0) / (PROBE_FRAMES - 1) / 1e6;
    printf("sdk: %d x WaitSystemEvent + RequestUpdateAudioRenderer: %d failed, mean period %.3f ms, elapsed frames "
           "%d -> %d\n", PROBE_FRAMES, bad, period, f0, f1);
    CHECK(!bad && period > 4.5 && period < 5.5 && f1 - f0 >= PROBE_FRAMES - 4 && f1 - f0 <= PROBE_FRAMES + 2,
          "SDK render loop");
    char *name = mem_kernel_alloc(0x100, 64);
    rt_call(c, stub(SYM_DEVICE_NAME), 1, (uint64_t)(uintptr_t)name);
    int channels = (int)rt_call(c, stub(SYM_CHANNELS), 0);
    printf("sdk: GetActiveAudioOutputDeviceName -> \"%s\", channels %d\n", name, channels);
    CHECK(!strcmp(name, audio_device_name) && channels == audio_output_channels(), "SDK audio device");
    rt_call(c, stub(SYM_STOP), 2, handle[0], handle[1]);
    rt_call(c, stub(SYM_CLOSE), 2, handle[0], handle[1]);
    printf("sdk: CloseAudioRenderer: renderers running %ld\n", audren_renderers_live());
    CHECK(audren_renderers_live() == 0, "SDK CloseAudioRenderer");
}

// A real-time thread that never sleeps must lose its priority, not end the process (RLIMIT_RTTIME: SIGXCPU at 100 ms,
// SIGKILL at 200). Skipped without RealtimeKit, as in a container.
static int rt_got;
static void realtime_hog(void *arg) {
    (void)arg;
    rt_got = plat_thread_realtime();
    for (uint64_t end = plat_now_ns() + 400000000ull; rt_got && plat_now_ns() < end;) {}
    plat_thread_realtime_end();
}

static void probe_realtime(void) {
    PlatThread *t = plat_thread_new(realtime_hog, NULL, "rt hog");
    CHECK(t, "no thread");
    if (!t) return;
    plat_thread_join(t);
    if (!rt_got) {
        printf("  real time: not granted here, not checked\n");
        return;
    }
    CHECK(!plat_thread_realtime(), "real time granted again after an overrun");
    printf("  real time: 400 ms without sleeping survived\n");
}

int probe_audio(void) {
    probe_realtime();
    ldr_load(rt_config()->exefs);
    kern_run_on_guest_main(probe_client);
    RtStats s = rt_stats();
    printf("stats: %llu SVCs, %llu threads, %llu host calls\n", (unsigned long long)s.svcs,
           (unsigned long long)s.threads, (unsigned long long)s.host_calls);
    if (failures) {
        printf("probe-audio: FAILED (%d failures)\n", failures);
        return 1;
    }
    printf("probe-audio: passed\n");
    return 0;
}
