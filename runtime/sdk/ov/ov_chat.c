// nn::chat (GameChat, the Switch 2's voice and camera chat), new in MKW 1.8.0: replaced by name as a console where no
// chat is ever started. The SDK's versions open the chat service through sm (svc 0x1f), which this runtime does not
// have (the first 1.8.0 boot stopped in nn::chat::Initialize from main+0xd321c0). main+0xd321a0 initializes, attaches
// its room-info event (x19+8) and asks IsChatting; the rooms, sharing and camera calls only follow a chat.
#include <string.h>

#include "host.h"

static void ov_chat_nothing(Cpu *c) { ret_x(c, 0); }

// void AttachToRoomInfoChangedEvent(nn::os::SystemEvent *): a real event the game can wait on, never signalled (no
// room changes without a chat). nn::os::SystemEvent holds its SystemEventType first.
static void ov_chat_attach(Cpu *c) {
    static uint64_t create;
    uint64_t ev = arg_x(c, 0);
    if (!create && !(create = ldr_symbol("_ZN2nn2os17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb")))
        rt_fatal("chat: nn::os::CreateSystemEvent not found");
    rt_call(c, create, 3, ev, 0, 0);   // manual clear, not interprocess
}

// Result GetRoomInfo(RoomInfo *): no room (only called while chatting)
static void ov_chat_room(Cpu *c) { ret_x(c, 0); }

// size_t Calculate{Local,Remote}CameraVideoBufferSize(int, int, int): a page, never filled
static void ov_chat_size(Cpu *c) { ret_x(c, 0x1000); }

// Initialize{Local,Remote}CameraVideoStorage(VideoType *, int w, int h, int count, void *, size_t): the size is kept
// in the video object (game memory, at least the 8 bytes written here) for the image handles.
static void ov_chat_storage(Cpu *c) {
    uint32_t *o = arg_p(c, 0);
    o[0] = (uint32_t)arg_x(c, 1), o[1] = (uint32_t)arg_x(c, 2);
    ret_x(c, 0);
}
// NativeImageHandle Get{Local,Remote}CameraImageHandle(const VideoType *, int): width << 16 | height, which
// nvn2.c's nvnTextureInitializeFromNativeImage reads back
static void ov_chat_image(Cpu *c) {
    const uint32_t *o = arg_p(c, 0);
    ret_x(c, (o[0] & 0xffff) << 16 | (o[1] & 0xffff));
}

HOST_OVERRIDE("_ZN2nn4chat10InitializeEv", ov_chat_nothing);
HOST_OVERRIDE("_ZN2nn4chat10IsChattingEv", ov_chat_nothing);
HOST_OVERRIDE("_ZN2nn4chat28AttachToRoomInfoChangedEventEPNS_2os11SystemEventE", ov_chat_attach);
HOST_OVERRIDE("_ZN2nn4chat11GetRoomInfoEPNS0_8RoomInfoE", ov_chat_room);
HOST_OVERRIDE("_ZN2nn4chat13SetSharingKeyERKNS0_10SharingKeyE", ov_chat_nothing);
HOST_OVERRIDE("_ZN2nn4chat15ClearSharingKeyEv", ov_chat_nothing);
HOST_OVERRIDE("_ZN2nn4chat13ShowChatStartERKNS_7account3UidE", ov_chat_nothing);
HOST_OVERRIDE("_ZN2nn4chat20SetScreenSharingModeENS0_17ScreenSharingModeE", ov_chat_nothing);
HOST_OVERRIDE("_ZN2nn4chat22ShowCameraSharingGuideEv", ov_chat_nothing);
HOST_OVERRIDE("_ZN2nn4chat36ShowLocalCameraConfigurationVersion0ENS0_25CameraImageProcessingModeE", ov_chat_nothing);
HOST_OVERRIDE("_ZN2nn4chat46BeginFullScreenWithLocalCameraAvailableSectionEb", ov_chat_nothing);
HOST_OVERRIDE("_ZN2nn4chat44EndFullScreenWithLocalCameraAvailableSectionEv", ov_chat_nothing);
HOST_OVERRIDE("_ZN2nn4chat25StartLocalCameraStreamingEv", ov_chat_nothing);
HOST_OVERRIDE("_ZN2nn4chat24StopLocalCameraStreamingEv", ov_chat_nothing);
HOST_OVERRIDE("_ZN2nn4chat26StartRemoteCameraStreamingERKNS_7account23NetworkServiceAccountIdE", ov_chat_nothing);
HOST_OVERRIDE("_ZN2nn4chat25StopRemoteCameraStreamingERKNS_7account23NetworkServiceAccountIdE", ov_chat_nothing);
HOST_OVERRIDE("_ZN2nn4chat25GetLocalCameraImageHandleEPKNS0_20LocalCameraVideoTypeEi", ov_chat_image);
HOST_OVERRIDE("_ZN2nn4chat26GetRemoteCameraImageHandleEPKNS0_21RemoteCameraVideoTypeEi", ov_chat_image);
HOST_OVERRIDE("_ZN2nn4chat27RequestNextLocalCameraImageEPNS_2gs16NativeSyncHandleEPNS0_20LocalCameraVideoTypeEiS2_",
              ov_chat_nothing);
HOST_OVERRIDE("_ZN2nn4chat28RequestNextRemoteCameraImageEPNS_2gs16NativeSyncHandleEPNS0_21RemoteCameraVideoTypeEiS2_RKNS_7account23NetworkServiceAccountIdE",
              ov_chat_nothing);
HOST_OVERRIDE("_ZN2nn4chat33InitializeLocalCameraVideoStorageEPNS0_20LocalCameraVideoTypeEiiiPvm", ov_chat_storage);
HOST_OVERRIDE("_ZN2nn4chat34InitializeRemoteCameraVideoStorageEPNS0_21RemoteCameraVideoTypeEiiiPvm", ov_chat_storage);
HOST_OVERRIDE("_ZN2nn4chat31FinalizeLocalCameraVideoStorageEPNS0_20LocalCameraVideoTypeE", ov_chat_nothing);
HOST_OVERRIDE("_ZN2nn4chat32FinalizeRemoteCameraVideoStorageEPNS0_21RemoteCameraVideoTypeE", ov_chat_nothing);
HOST_OVERRIDE("_ZN2nn4chat35CalculateLocalCameraVideoBufferSizeEiii", ov_chat_size);
HOST_OVERRIDE("_ZN2nn4chat36CalculateRemoteCameraVideoBufferSizeEiii", ov_chat_size);
