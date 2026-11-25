// --probe-services, the overrides only MKW imports (ov_mkw_list.h): the offline network services answer as a console
// with no network, the account's network service account stays unavailable, oe and time give MKW's version, scale and
// clock answers, the touch screen reads no touches, and the VP9 decoder (ov_codec.c) decodes frames this probe encodes
// with libvpx, through the player's call sequence: image pools, slots, Submit, Acquire, a sync point event, the frame's
// properties and its NV12 picture. H.264, AV1 and VP8 stop with a named fatal (child processes, ov_probe.c). Sockets
// (ov_socket.c) send and receive datagrams over the host's loopback.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vpx/vp8cx.h>
#include <vpx/vpx_encoder.h>

#include "kernel.h"
#include "host.h"
#include "ov.h"
#include "ov_probe.h"
#include "platform/platform.h"

#define CHECK(ok, ...) ovp_check(ok, __VA_ARGS__)
#define CALL OVP_CALL

// Result codes of the overrides (ov_net.c, ov_system.c)
#define NIFM_R_NO_NETWORK (110u | (1u << 9))
#define FRIENDS_R_UNAVAILABLE (121u | (1u << 9))
#define LDN_R_DISABLED (203u | (22u << 9))
#define ACCOUNT_R_NSA_UNAVAILABLE (124u | (280u << 9))
#define R_BUSY 0x1c266fu

static uint64_t U(const void *p) { return (uint64_t)(uintptr_t)p; }

static uint64_t sdk(const char *name) {
    uint64_t a = ldr_symbol(name);
    if (!a) rt_fatal("probe-services: the SDK exports no %s", name);
    return a;
}

enum { SOCK_ENETDOWN = 50, SOCK_EAI_FAIL = 4 };

// ---- nifm, friends, ldn, nsd, socket: every call with writable guest buffers in x0..x7, its result against the table
static void probe_offline(void) {
    enum { VOID = -1 };
    static const struct { const char *key; int64_t want; } cases[] = {
        {"nifm::Initialize", 0}, {"nifm::IsNetworkAvailable", 0}, {"nifm::SetLocalNetworkMode", VOID},
        {"nifm::CancelNetworkRequest", VOID}, {"nifm::SubmitNetworkRequestAndWait", VOID},
        {"nifm::HandleNetworkRequestErrorResult", NIFM_R_NO_NETWORK},
        {"nifm::GetCurrentIpConfigInfo", NIFM_R_NO_NETWORK}, {"nifm::GetCurrentProxySetting", NIFM_R_NO_NETWORK},
        {"friends::Initialize", VOID}, {"friends::SetAllocator", VOID},
        {"friends::EnsureFriendListAvailable", FRIENDS_R_UNAVAILABLE},
        {"friends::GetPlayHistoryRegistrationKey", FRIENDS_R_UNAVAILABLE}, {"friends::AddPlayHistory", 0},
        {"ldn::Initialize", LDN_R_DISABLED}, {"ldn::Finalize", VOID}, {"ldn::GetState", 0},
        {"ldn::GetDisconnectReason", 0}, {"ldn::Disconnect", LDN_R_DISABLED}, {"ldn::OpenStation", LDN_R_DISABLED},
        {"ldn::CloseStation", LDN_R_DISABLED}, {"ldn::OpenAccessPoint", LDN_R_DISABLED},
        {"ldn::CloseAccessPoint", LDN_R_DISABLED}, {"ldn::DestroyNetwork", LDN_R_DISABLED},
        {"ldn::Scan", LDN_R_DISABLED}, {"ldn::ScanPrivate", LDN_R_DISABLED}, {"ldn::CreateNetwork", LDN_R_DISABLED},
        {"ldn::CreateNetworkPrivate", LDN_R_DISABLED}, {"ldn::Connect", LDN_R_DISABLED},
        {"ldn::ConnectPrivate", LDN_R_DISABLED}, {"ldn::GetIpv4Address", LDN_R_DISABLED},
        {"ldn::GetNetworkInfo", LDN_R_DISABLED}, {"ldn::GetSecurityParameter", LDN_R_DISABLED},
        {"ldn::SetAdvertiseData", LDN_R_DISABLED}, {"ldn::AddAcceptFilterEntry", LDN_R_DISABLED},
        {"ldn::SetStationAcceptPolicy", LDN_R_DISABLED}, {"ldn::Reject", LDN_R_DISABLED},
        {"ldn::AttachStateChangeEvent", VOID},
        {"nsd::GetEnvironmentIdentifier", NIFM_R_NO_NETWORK}, {"nsd::ResolveEx", NIFM_R_NO_NETWORK},
        // nn::socket as with the network down (ov_net.c): every socket call -1, errno ENETDOWN
        {"socket::internal::Initialize", 0}, {"socket::SetAllocator", VOID}, {"socket::Finalize", 0}, {"socket::IsInitialized", 1}, {"socket::GetLastErrno", SOCK_ENETDOWN}, {"socket::GetLastError", SOCK_ENETDOWN}, {"socket::Socket(int)", -1}, {"socket::Socket", -1}, {"socket::EventFd", -1}, {"socket::EventFdRead", -1}, {"socket::EventFdWrite", -1}, {"socket::Bind", -1}, {"socket::Bind(sockaddr)", -1}, {"socket::SetSockOpt", -1}, {"socket::SendTo(sockaddr)", -1}, {"socket::SendTo", -1}, {"socket::RecvFrom(sockaddr)", -1}, {"socket::RecvFrom", -1}, {"socket::Poll", -1}, {"socket::Shutdown", -1}, {"socket::Close", -1}, {"socket::GetAddrInfo(addrinfo)", SOCK_EAI_FAIL}, {"socket::GetAddrInfo", SOCK_EAI_FAIL}, {"socket::FreeAddrInfo(addrinfo)", VOID}, {"socket::FreeAddrInfo", VOID},
    };
    uint8_t *b = ovp_alloc(8 * 0x400);
    int bad = 0;
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        memset(b, 0, 8 * 0x400);
        uint64_t r = CALL(cases[i].key, U(b), U(b + 0x400), U(b + 0x800), U(b + 0xc00), U(b + 0x1000), U(b + 0x1400),
                          U(b + 0x1800), U(b + 0x1c00));
        if (cases[i].want != VOID && (uint32_t)r != (uint32_t)cases[i].want) {
            bad++;
            CHECK(0, "%s returned %#x, not %#x", cases[i].key, (uint32_t)r, (uint32_t)cases[i].want);
        }
    }
    // outputs written as "none"
    uint8_t *avail = b;
    int32_t *count = (int32_t *)(b + 0x400);
    *avail = 1, *count = 7;
    CHECK(CALL("friends::CheckFriendListAvailability", U(avail), U(b + 0x800)) == 0 && *avail == 0,
          "CheckFriendListAvailability: %u", *avail);
    CHECK(CALL("friends::GetFriendList", U(count), U(b + 0x800), U(b + 0xc00), 0, 4, U(b + 0x1000)) == 0 && *count == 0,
          "GetFriendList: %d friends", *count);
    printf("net: nifm, friends, ldn and nsd answer as a console with no network: %d of %d results as expected\n",
           (int)(sizeof cases / sizeof *cases) - bad, (int)(sizeof cases / sizeof *cases));

    // MKW_LAN=1: a local network request is connected and reports an address; an internet one stays unavailable
    plat_setenv("MKW_LAN", "1");
    CALL("nifm::SetLocalNetworkMode", 0);
    CHECK(CALL("nifm::IsNetworkAvailable") == 0, "MKW_LAN: an internet request stays unavailable");
    CALL("nifm::SetLocalNetworkMode", 1);
    uint8_t *ip = b;
    memset(b, 0, 0x20);
    int have = !ov_lan_address(b + 0x18, b + 0x1c);   // a machine with no IPv4 address (CI) answers no network
    uint64_t r = CALL("nifm::GetCurrentIpConfigInfo", U(ip), U(ip + 4), U(ip + 8), U(ip + 12), U(ip + 16));
    CHECK(CALL("nifm::IsNetworkAvailable") == 1 && CALL("nifm::HandleNetworkRequestErrorResult") == 0,
          "MKW_LAN: local network available");
    CHECK(have ? r == 0 && ip[0] && ip[4] == 255 : (uint32_t)r == NIFM_R_NO_NETWORK,
          "MKW_LAN: address %u.%u.%u.%u mask %u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3], ip[4], ip[5], ip[6], ip[7]);
    printf("net: MKW_LAN, local network mode connected at %u.%u.%u.%u/%u.%u.%u.%u\n", ip[0], ip[1], ip[2], ip[3],
           ip[4], ip[5], ip[6], ip[7]);
    CALL("nifm::SetLocalNetworkMode", 0);
    plat_setenv("MKW_LAN", "");
}

// ---- account: the one local user, its network service account unavailable
static void probe_account_mkw(void) {
    int32_t *count = ovp_alloc(16);
    uint64_t *uid = ovp_alloc(16), *handle = ovp_alloc(32);
    uint8_t *flag = ovp_alloc(16);
    CHECK(CALL("account::ListAllUsers", U(count), U(uid), 4) == 0 && *count == 1 && (uid[0] | uid[1]),
          "ListAllUsers: %d users", *count);
    CHECK(CALL("account::ListAllUsers", U(count), 0, 0) == 0 && *count == 0, "ListAllUsers with no room: %d", *count);
    CHECK(CALL("account::OpenUser", U(handle), U(uid)) == 0 && handle[0] == uid[0] && handle[1] == uid[1],
          "OpenUser of the local user");
    *flag = 1;
    CHECK(CALL("account::IsNetworkServiceAccountAvailable", U(flag), U(handle)) == 0 && *flag == 0,
          "IsNetworkServiceAccountAvailable: %u", *flag);
    static const char *const nsa[] = {"account::EnsureNetworkServiceAccountAvailable",
                                      "account::GetNetworkServiceAccountId",
                                      "account::LoadNetworkServiceAccountIdTokenCache",
                                      "account::EnsureNetworkServiceAccountIdTokenCacheAsync"};
    uint8_t *b = ovp_alloc(0x400);
    for (size_t i = 0; i < sizeof nsa / sizeof *nsa; i++) {
        uint32_t r = (uint32_t)CALL(nsa[i], U(b), U(b + 0x100), 0x100, U(handle));
        CHECK(r == ACCOUNT_R_NSA_UNAVAILABLE, "%s returned %#x", nsa[i], r);
    }
    printf("account: one local user, opened by its UID; network service account unavailable\n");
}

// ---- oe, time, touch, nv
static void probe_system_mkw(void) {
    char *version = ovp_alloc(32);
    memset(version, 0x55, 32);
    CALL("oe::GetDisplayVersion", U(version));
    CHECK(!strcmp(version, "1.0.0") && !version[15], "GetDisplayVersion: %.16s", version);
    CHECK(CALL("oe::GetCurrentGraphicsScale", 0) == 10, "GetCurrentGraphicsScale");
    CALL("oe::EnterExitRequestHandlingSection", 0);
    CALL("oe::LeaveExitRequestHandlingSection", 0);
    CALL("oe::SendServerMaintenanceOverlayNotification", 1700000000, 1700003600);
    CALL("oe::SetGraphicsScaleChangedNotificationEnabled", 1);

    CHECK((uint8_t)CALL("time::StandardNetworkSystemClock::IsAccuracySufficient", 0) == 1, "network clock accuracy");
    char *loc = ovp_alloc(0x40);
    memset(loc, 0x55, 0x40);
    CALL("time::GetDeviceLocationName", U(loc));
    CHECK(loc[0] && memchr(loc, 0, 0x24), "GetDeviceLocationName: not a NUL-terminated name");
    CHECK(CALL("time::LoadTimeZoneRule", U(ovp_alloc(0x4000)), U(loc)) == 0, "LoadTimeZoneRule");
    // the network clock correction task: the SDK's constructor, its finish event made by the SDK's CreateSystemEvent
    // where Initialize would (GetFinishEvent: the task + 8; Initialize itself connects to the ntc service, which the
    // runtime does not serve), then StartTask signals it
    Cpu *gc = ovp_cpu();
    uint64_t task = U(ovp_alloc(0x400));
    rt_call(gc, sdk("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTaskC1Ev"), 1, task);
    uint64_t ev = rt_call(gc, sdk("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTask14GetFinishEventEv"), 1, task);
    rt_call(gc, sdk("_ZN2nn2os17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb"), 3, ev, 0, 0);
    uint64_t try_wait = sdk("_ZN2nn2os18TryWaitSystemEventEPNS0_15SystemEventTypeE");
    int before = (uint8_t)rt_call(gc, try_wait, 1, ev);
    uint32_t started = (uint32_t)CALL("time::AsyncEnsureNetworkClockAvailabilityTask::StartTask", task);
    int after = (uint8_t)rt_call(gc, try_wait, 1, ev);
    CHECK(!before && started == 0 && after, "clock correction task: finish event %d -> %d, StartTask %#x", before, after,
          started);
    CHECK(CALL("time::AsyncEnsureNetworkClockAvailabilityTask::GetResult", task) == 0, "clock correction task result");

    uint8_t *touch = ovp_alloc(0x400);
    CALL("hid::InitializeTouchScreen", 0);
    memset(touch, 0x55, 0x400);
    uint64_t s1 = 0, s2 = 0;
    int32_t touches = -1;
    int n1 = (int)CALL("hid::GetTouchScreenStates", U(touch), 1);
    memcpy(&s1, touch, 8), memcpy(&touches, touch + 8, 4);
    int n2 = (int)CALL("hid::GetTouchScreenStates", U(touch), 1);
    memcpy(&s2, touch, 8);
    CHECK(n1 == 1 && n2 == 1 && touches == 0 && s2 > s1, "touch screen: %d/%d states, %d touches", n1, n2, touches);
    CHECK(CALL("hid::GetTouchScreenStates", U(touch), 0) == 0, "touch screen: a state for count 0");

    CALL("nv::InitCheck", 0);   // after nv::InitializeGraphics (probe_media_gfx): no fatal
    printf("oe/time/hid/nv: version %s, graphics scale 10, location %s, clock correction finished at once, no touches\n",
           version, loc);
}

// ---- the VP9 decoder: three 64x64 frames of flat grey levels, then the end of stream
enum { W = 64, H = 64, FRAMES = 3 };

// The probe's input: frame k is luma 40 + 60k with neutral chroma, one packet each (no lag).
static int encode(uint8_t *pkts[FRAMES], size_t sizes[FRAMES]) {
    vpx_codec_enc_cfg_t cfg;
    vpx_codec_ctx_t enc;
    if (vpx_codec_enc_config_default(vpx_codec_vp9_cx(), &cfg, 0)) return 0;
    cfg.g_w = W, cfg.g_h = H, cfg.g_lag_in_frames = 0, cfg.rc_target_bitrate = 2000, cfg.g_timebase = (vpx_rational_t){1, 30};
    if (vpx_codec_enc_init(&enc, vpx_codec_vp9_cx(), &cfg, 0)) return 0;
    vpx_image_t img;
    vpx_img_alloc(&img, VPX_IMG_FMT_I420, W, H, 1);
    int got = 0;
    for (int k = 0; k < FRAMES; k++) {
        for (int y = 0; y < H; y++) memset(img.planes[0] + y * img.stride[0], 40 + 60 * k, W);
        for (int y = 0; y < H / 2; y++) {
            memset(img.planes[1] + y * img.stride[1], 128, W / 2);
            memset(img.planes[2] + y * img.stride[2], 128, W / 2);
        }
        if (vpx_codec_encode(&enc, &img, k, 1, 0, VPX_DL_GOOD_QUALITY)) break;
        vpx_codec_iter_t it = NULL;
        const vpx_codec_cx_pkt_t *p;
        while ((p = vpx_codec_get_cx_data(&enc, &it)))
            if (p->kind == VPX_CODEC_CX_FRAME_PKT && got < FRAMES) {
                pkts[got] = malloc(p->data.frame.sz);
                memcpy(pkts[got], p->data.frame.buf, p->data.frame.sz);
                sizes[got++] = p->data.frame.sz;
            }
    }
    vpx_img_free(&img);
    vpx_codec_destroy(&enc);
    return got == FRAMES;
}

static int mean(const uint8_t *p, size_t n) {
    uint64_t s = 0;
    for (size_t i = 0; i < n; i++) s += p[i];
    return (int)(s / n);
}

// Submit until accepted: busy is answered while three frames wait for a slot
static uint32_t submit(uint64_t dec, const void *data, size_t size, int64_t pts, int eos) {
    uint64_t *consumed = ovp_alloc(16);
    for (int tries = 0; tries < 200; tries++) {
        uint32_t r = (uint32_t)CALL("codec::SubmitVp9DecoderInputBuffer", U(consumed), dec, U(data), size, (uint64_t)pts,
                                    (uint64_t)eos);
        if (r != R_BUSY) return r;
        plat_sleep_ms(5);
    }
    return R_BUSY;
}

static void probe_codec(void) {
    uint8_t *pkts[FRAMES] = {0};
    size_t sizes[FRAMES] = {0};
    if (!encode(pkts, sizes)) {
        CHECK(0, "codec: libvpx could not encode the probe's frames");
        return;
    }
    CHECK(CALL("codec::video::Initialize", 0) == 0, "video::Initialize");
    // two image pools, one per slot, as the player makes them
    uint8_t *pref = ovp_alloc(64), *pref2 = ovp_alloc(64);
    ovp_call_full("codec::MakeDecodeImagePreference", (const uint64_t[8]){W, H, 1, 0}, U(pref), NULL, 0);
    ovp_call_full("codec::sdk::MakeDecodeImagePreferenceForCompat", (const uint64_t[8]){W, H, 1, 0}, U(pref2), NULL, 0);
    uint16_t pw, ph;
    memcpy(&pw, pref, 2), memcpy(&ph, pref + 2, 2);
    CHECK(pw == W && ph == H && !memcmp(pref, pref2, 16), "decode image preference %ux%u", pw, ph);
    uint64_t *sz = ovp_alloc(16);
    CHECK(CALL("codec::CalculateDecoderImagePoolBuffer", U(sz), U(sz + 1), U(pref), 1) == 0 && sz[0] >= W * H * 3 / 2 &&
              sz[1] == 0x1000, "image pool size %#llx align %#llx", (unsigned long long)sz[0], (unsigned long long)sz[1]);
    uint64_t pools[2];
    for (int i = 0; i < 2; i++) {
        pools[i] = U(ovp_alloc(0x40));
        CHECK(CALL("codec::InitializeDecoderImagePool", pools[i], U(ovp_alloc(sz[0])), sz[0], U(pref), 1, 0) == 0,
              "InitializeDecoderImagePool %d", i);
    }
    CHECK(CALL("codec::GetDecoderImagePoolNativeImageHandle", pools[0], 0) == 0, "native image handle (GPU mode)");
    CHECK(CALL("codec::CalculateVp9DecoderWorkBuffer", U(sz), U(sz + 1), 0, W, H) == 0 && sz[0], "VP9 work buffer");

    // one decoder through each Initialize overload; the second is only initialized and finalized
    uint64_t dec = U(ovp_alloc(0x100)), dec2 = U(ovp_alloc(0x100));
    const uint64_t stack[1] = {0};   // the output format, ninth argument
    CHECK(ovp_call_full("codec::InitializeVp9Decoder(work)", (const uint64_t[8]){dec, U(ovp_alloc(sz[0])), sz[0], 0, W, H,
                                                                                  2, 0}, 0, stack, 1) == 0,
          "InitializeVp9Decoder with a work buffer");
    CHECK(CALL("codec::InitializeVp9Decoder", dec2, 0, 0, W, H, 2, 0, 0) == 0, "InitializeVp9Decoder");
    CHECK(CALL("codec::FinalizeVp9Decoder", dec2) == 0, "FinalizeVp9Decoder of the second decoder");
    for (int i = 0; i < 2; i++)
        CHECK(CALL("codec::RegisterVp9DecoderBuffer", dec, (uint64_t)i, pools[i], 0) == 0, "RegisterVp9DecoderBuffer %d", i);
    CHECK(CALL("codec::StartVp9Decoder", dec) == 0, "StartVp9Decoder");
    uint64_t *ready = ovp_alloc(16);
    CHECK(CALL("codec::GetVp9DecoderSubmissionReadyEvent", U(ready), dec) == 0 && *ready, "submission ready event");

    // the sync point event the player waits on for each acquired frame
    uint64_t sp = U(ovp_alloc(0x40));
    CALL("syncpt::sdk::CreateSyncPointEvent", sp);
    CALL("syncpt::sdk::InitializeMultiWaitHolder", U(ovp_alloc(0x100)), sp);

    uint8_t *luma = ovp_alloc(W * H), *chroma = ovp_alloc(W * H / 2);
    uint64_t *handle = ovp_alloc(16);
    uint32_t *slot = ovp_alloc(16);
    // as the player: the frames submitted ahead of it, the end of stream while it still holds a picture
    for (int k = 0; k < FRAMES; k++) CHECK(submit(dec, pkts[k], sizes[k], 1000 * k, 0) == 0, "Submit %d", k);
    int good = 0;
    for (int k = 0; k <= FRAMES; k++) {   // k == FRAMES: the end of stream, the last picture again
        int eos = k == FRAMES;
        CHECK(CALL("codec::AcquireVp9DecoderOutputBuffer", U(slot), U(handle), dec) == 0 && *slot < 2,
              "Acquire %d: slot %u", k, *slot);
        CALL("syncpt::sdk::RequestSyncPointEvent", sp, U(handle));
        int done = 0;
        for (int t = 0; t < 400 && !done; t++)
            if (!(done = (int)(CALL("syncpt::sdk::TryWaitSyncPointEvent", sp) & 1))) plat_sleep_ms(5);
        CHECK(done, "frame %d: the sync point event was not signalled in 2 s", k);
        // 1.8.0's poll of the same handle: done once the frame is decoded, not for a frame further on
        uint64_t *ahead = ovp_alloc(16);
        ahead[0] = handle[0], ahead[1] = handle[1] + 1000;
        CHECK((CALL("syncpt::sdk::TryWaitSyncPoint", U(handle)) & 1) && !(CALL("syncpt::sdk::TryWaitSyncPoint", U(ahead)) & 1),
              "frame %d: TryWaitSyncPoint", k);
        uint16_t *dims = ovp_alloc(16);
        int64_t *pts = ovp_alloc(16);
        uint8_t *eos_out = ovp_alloc(16);
        const uint64_t props_stack[1] = {*slot};
        ovp_call_full("codec::GetVp9DecoderOutputBufferProperties",
                      (const uint64_t[8]){U(dims), U(dims + 1), U(dims + 2), U(dims + 3), U(ovp_alloc(16)), U(pts),
                                          U(eos_out), dec}, 0, props_stack, 1);
        CHECK(dims[0] == W && dims[1] == H && *pts == 1000 * k && *eos_out == eos,
              "frame %d properties: %ux%u pts %lld eos %u", k, dims[0], dims[1], (long long)*pts, *eos_out);
        const uint64_t read_stack[4] = {W * H, U(chroma), W, W * H / 2};
        CHECK(ovp_call_full("codec::ReadDecoderOutputImageNv12", (const uint64_t[8]){pools[*slot], 0, 0, 0, W, H, U(luma), W},
                            0, read_stack, 4) == 0, "ReadDecoderOutputImageNv12 %d", k);
        int want = 40 + 60 * (eos ? FRAMES - 1 : k), y = mean(luma, W * H), c = mean(chroma, W * H / 2);
        int ok = abs(y - want) <= 4 && abs(c - 128) <= 4;
        CHECK(ok, "frame %d: mean luma %d (want %d), chroma %d", k, y, want, c);
        good += ok;
        if (k == 0) CHECK(submit(dec, NULL, 0, 1000 * FRAMES, 1) == 0, "Submit the end of stream");
        CHECK(CALL("codec::ReleaseVp9DecoderOutputBuffer", dec, *slot, handle[0], handle[1]) == 0, "Release %d", k);
    }
    CALL("syncpt::sdk::CancelSyncPointEvent", sp);
    CALL("syncpt::sdk::DestroySyncPointEvent", sp);
    CHECK(CALL("codec::FlushVp9Decoder", dec) == 0, "FlushVp9Decoder");
    CHECK(CALL("codec::StopVp9Decoder", dec) == 0, "StopVp9Decoder");
    CHECK(CALL("codec::FinalizeVp9Decoder", dec) == 0, "FinalizeVp9Decoder");
    for (int i = 0; i < 2; i++) CHECK(CALL("codec::FinalizeDecoderImagePool", pools[i]) == 0, "FinalizeDecoderImagePool");
    CHECK(CALL("codec::video::Finalize", 0) == 0, "video::Finalize");
    for (int k = 0; k < FRAMES; k++) free(pkts[k]);
    printf("codec: VP9 %dx%d, %d of %d pictures (and the end of stream) decoded with their grey level, through image "
           "pools, slots, sync point events and NV12 reads\n", W, H, good, FRAMES + 1);
}

// Region k of a heap of at least k + 1 regions, frozen (memory.c mem_freeze): read-only until a write or mem_thaw.
// The kernel's write into it fails (EFAULT, WSAEFAULT) instead of faulting into the thaw, so every override that has
// the kernel write guest memory thaws the buffer first. A region thawed by mem_thaw stays thawed 5 s: one each.
static uint8_t *frozen_region(uint64_t heap, int k) {
    uint64_t a = heap + ((uint64_t)k << 21);
    *(volatile uint8_t *)(uintptr_t)a = 1;   // committed (Windows' lazy commit takes the whole 2 MiB) and written
    CHECK(mem_freeze(a) == 1 && mem_frozen(a), "freeze of heap region %d", k);
    return (uint8_t *)(uintptr_t)a;
}

// ---- socket (ov_socket.c): datagrams over the host's loopback with the console's sockaddr, flags and errno, an
// empty nonblocking read, poll, eventfd, a receive, an eventfd read and random bytes into a frozen region, and every
// overload once
static void probe_socket(void) {
    enum { AF = 2, DGRAM = 2, UDP = 17, NONBLOCK = 2 << 28, SOL = 0xffff, EAGAIN_ = 11, EBADF_ = 9 };
    uint8_t *b = ovp_alloc(0x400);
    uint8_t *ab = b, *aa = b + 0x10, *from = b + 0x20, *buf = b + 0x40;
    uint32_t *fromlen = (uint32_t *)(b + 0x80), *one = (uint32_t *)(b + 0x84);
    uint64_t *val = (uint64_t *)(b + 0x88);
    int32_t *pfd = (int32_t *)(b + 0x90);
    void **res = (void **)(b + 0xb0);
    plat_setenv("MKW_LAN", "1");   // off, probe_offline's table checks main's stub
    CALL("socket::SetAllocator", 0);
    CHECK(CALL("socket::internal::Initialize", 4) == 0, "Initialize");
    CHECK(CALL("socket::IsInitialized") == 1, "IsInitialized");
    int sa = (int)CALL("socket::Socket(int)", AF, DGRAM, UDP);
    int sb = (int)CALL("socket::Socket", AF, DGRAM | NONBLOCK, UDP);
    CHECK(sa >= 0 && sb >= 0 && sa != sb, "Socket: %d %d", sa, sb);
    *one = 1;
    CHECK((int)CALL("socket::SetSockOpt", sb, SOL, 0x4, U(one), 4) == 0, "SetSockOpt SO_REUSEADDR");
    CHECK((int)CALL("socket::SetSockOpt", sa, SOL, 0x20, U(one), 4) == 0, "SetSockOpt SO_BROADCAST");
    CHECK((int)CALL("socket::SetSockOpt", sa, SOL, 0x1015, U(one), 4) == 0, "SetSockOpt unmapped option accepted");
    // FreeBSD sockaddr_in: len, family, port (big endian), 127.0.0.1; a free port from 47000
    const uint8_t lo[4] = {127, 0, 0, 1};
    int port = 0;
    for (int p = 47000; p < 47040 && !port; p++) {
        memset(ab, 0, 16);
        ab[0] = 16, ab[1] = AF, ab[2] = (uint8_t)(p >> 8), ab[3] = (uint8_t)p;
        memcpy(ab + 4, lo, 4);
        if ((int)CALL("socket::Bind(sockaddr)", sb, U(ab), 16) == 0) port = p;
    }
    memset(aa, 0, 16);
    aa[0] = 16, aa[1] = AF;
    memcpy(aa + 4, lo, 4);
    CHECK(port && (int)CALL("socket::Bind", sa, U(aa), 16) == 0, "Bind: port %d", port);
    // nothing to read: -1 and EAGAIN, on the nonblocking socket and with MSG_DONTWAIT on the blocking one
    *fromlen = 16;
    CHECK((int64_t)CALL("socket::RecvFrom(sockaddr)", sb, U(buf), 64, 0, U(from), U(fromlen)) == -1 &&
              CALL("socket::GetLastErrno") == EAGAIN_, "RecvFrom on an empty nonblocking socket");
    CHECK((int64_t)CALL("socket::RecvFrom", sa, U(buf), 64, 0x80, U(from), U(fromlen)) == -1 &&
              CALL("socket::GetLastError") == EAGAIN_, "RecvFrom with MSG_DONTWAIT");
    memcpy(buf, "hello", 5);
    CHECK((int64_t)CALL("socket::SendTo(sockaddr)", sa, U(buf), 5, 0, U(ab), 16) == 5, "SendTo");
    pfd[0] = sb, pfd[1] = 1;     // events POLLIN, revents 0
    pfd[2] = 63, pfd[3] = 1;     // a descriptor never opened
    int n = (int)CALL("socket::Poll", U(pfd), 2, 1000);
    CHECK(n == 2 && (pfd[1] >> 16 & 1) && (pfd[3] >> 16) == 0x20, "Poll: %d ready, revents %#x %#x", n,
          (uint32_t)pfd[1] >> 16, (uint32_t)pfd[3] >> 16);
    memset(buf, 0, 8);
    memset(from, 0xee, 16);
    *fromlen = 16;
    int64_t got = (int64_t)CALL("socket::RecvFrom(sockaddr)", sb, U(buf), 64, 0, U(from), U(fromlen));
    CHECK(got == 5 && !memcmp(buf, "hello", 5) && from[0] == 16 && from[1] == AF && !memcmp(from + 4, lo, 4) &&
              *fromlen == 16, "RecvFrom: %lld bytes, from len %u family %u", (long long)got, from[0], from[1]);
    CHECK((int64_t)CALL("socket::SendTo", sa, U(buf), 3, 0x80, U(ab), 16) == 3, "SendTo with MSG_DONTWAIT");
    *fromlen = 16;
    CHECK((int64_t)CALL("socket::RecvFrom", sb, U(buf), 64, 0, U(from), U(fromlen)) == 3, "RecvFrom 2");
    // the game's receive buffer in a frozen region: the datagram lands and the region is thawed
    uint64_t heap = 0;
    CHECK(mem_set_heap_size(3ull << 21, &heap) == 0, "SetHeapSize 6 MiB");
    uint8_t *fz = frozen_region(heap, 0);
    CHECK((int64_t)CALL("socket::SendTo", sa, U("frozen"), 6, 0, U(ab), 16) == 6, "SendTo for the frozen buffer");
    pfd[0] = sb, pfd[1] = 1;
    CHECK((int)CALL("socket::Poll", U(pfd), 1, 1000) == 1, "Poll before the frozen receive");
    *fromlen = 16;
    got = (int64_t)CALL("socket::RecvFrom", sb, U(fz + 0x100), 64, 0, U(from), U(fromlen));
    int64_t err = got < 0 ? (int64_t)CALL("socket::GetLastErrno") : 0;
    CHECK(got == 6 && !memcmp(fz + 0x100, "frozen", 6) && !mem_frozen(U(fz)),
          "RecvFrom into a frozen region: %lld bytes, errno %lld, still frozen %d", (long long)got, (long long)err,
          mem_frozen(U(fz)));
    // LAN only: an internet address is refused before anything is sent (8.8.8.8:53, a public one), and so is a bind to it
    const uint8_t pub[4] = {8, 8, 8, 8};
    memset(from, 0, 16);
    from[0] = 16, from[1] = AF, from[3] = 53;
    memcpy(from + 4, pub, 4);
    CHECK((int64_t)CALL("socket::SendTo", sa, U(buf), 3, 0, U(from), 16) == -1 && CALL("socket::GetLastErrno") == 101,
          "SendTo an internet address: refused, ENETUNREACH");
    CHECK((int)CALL("socket::Bind", sb, U(from), 16) == -1 && CALL("socket::GetLastErrno") == 101,
          "Bind to an internet address: refused");
    int sh = (int)CALL("socket::Shutdown", sa, 2);   // a datagram socket with no peer: 0, or ENOTCONN (107)
    CHECK(sh == 0 || CALL("socket::GetLastErrno") == 107, "Shutdown: %d", sh);
#ifndef _WIN32
    int e = (int)CALL("socket::EventFd", 0, 4);
    *val = 0;
    CHECK(e >= 0 && (int)CALL("socket::EventFdWrite", e, 3) == 0 && (int)CALL("socket::EventFdRead", e, U(val)) == 0 &&
              *val == 3, "EventFd: %d, read %llu", e, (unsigned long long)*val);
    uint64_t *fv = (uint64_t *)(frozen_region(heap, 1) + 0x200);   // the counter read into a frozen region
    CHECK((int)CALL("socket::EventFdWrite", e, 5) == 0 && (int)CALL("socket::EventFdRead", e, U(fv)) == 0 && *fv == 5 &&
              !mem_frozen(U(fv)), "EventFdRead into a frozen region: read %llu", (unsigned long long)*fv);
    CHECK((int)CALL("socket::Close", e) == 0, "Close eventfd");
#else
    CHECK((int)CALL("socket::EventFd", 0, 4) == -1 && (int)CALL("socket::EventFdWrite", 0, 1) == -1 &&
              (int)CALL("socket::EventFdRead", 0, U(val)) == -1, "EventFd unsupported on Windows");
#endif
    // random bytes into a frozen region, whose fresh pages read as zeros (Linux: getrandom is the kernel's write, which
    // failed into a fatal; Windows' BCryptGenRandom writes from user mode and faults into the thaw)
    uint8_t *fr = frozen_region(heap, 2) + 0x300;
    CALL("crypto.GenerateCryptographicallyRandomBytes", U(fr), 32);
    CHECK(memcmp(fr, (uint8_t[32]){0}, 32) && !mem_frozen(U(fr)), "random bytes into a frozen region: all zero");
    CHECK(mem_set_heap_size(0, &heap) == 0, "SetHeapSize 0");
    CHECK((int)CALL("socket::Close", sa) == 0 && (int)CALL("socket::Close", sb) == 0, "Close");
    CHECK((int)CALL("socket::Close", sa) == -1 && CALL("socket::GetLastErrno") == EBADF_, "Close twice: EBADF");
    *res = b;
    CHECK(CALL("socket::GetAddrInfo(addrinfo)", U("example.com"), 0, 0, U(res)) != 0 && !*res, "GetAddrInfo offline");
    CHECK(CALL("socket::GetAddrInfo", U("example.com"), 0, 0, U(res)) != 0, "GetAddrInfo (AddrInfo)");
    CALL("socket::FreeAddrInfo(addrinfo)", 0);
    CALL("socket::FreeAddrInfo", 0);
    CHECK(CALL("socket::Finalize") == 0, "Finalize");
    plat_setenv("MKW_LAN", "");
    CHECK((int)CALL("socket::Socket(int)", AF, DGRAM, UDP) == -1 && CALL("socket::GetLastErrno") == 50,
          "MKW_LAN off again: Socket -1, ENETDOWN");
    printf("socket: UDP on loopback port %d through the console's sockaddr, poll, EAGAIN and eventfd\n", port);
}

void probe_mkw_services(void) {
    probe_offline();
    probe_account_mkw();
    probe_system_mkw();
    probe_codec();
    probe_socket();
}
