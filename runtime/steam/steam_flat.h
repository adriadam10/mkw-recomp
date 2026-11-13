// The part of Steam's flat C API that steam_rpt.c calls, declared here so the runtime builds without the Steamworks
// SDK and links nothing of Valve's: the library (libsteam_api.so, steam_api64.dll) is loaded at run time from the
// folder of mkw, and a machine without it runs as before. Names, layouts and ids are those of Steamworks SDK 1.65's
// public headers (steam_api.h, steam_api_flat.h, steam_api_internal.h, steamclientpublic.h, isteamremoteplay.h);
// direct input (BEnableRemotePlayTogetherDirectInput, GetInput, SetMouseVisibility) arrived in SDK 1.62, so an older
// library is refused when its symbols are bound.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef _WIN32
#define STEAM_CALL __cdecl
#else
#define STEAM_CALL
#endif

typedef int32_t SteamPipe;          // HSteamPipe
typedef int32_t SteamUser;          // HSteamUser
typedef uint64_t SteamId;           // uint64_steamid; 0 is an invalid id (BSendRemotePlayTogetherInvite: a guest link)
typedef uint32_t SteamSession;      // RemotePlaySessionID_t
typedef char SteamErrMsg[1024];     // k_cchMaxSteamErrMsg

enum {   // ESteamAPIInitResult
    STEAM_INIT_OK = 0, STEAM_INIT_FAILED = 1, STEAM_INIT_NO_CLIENT = 2, STEAM_INIT_VERSION_MISMATCH = 3,
};

enum {   // ERemotePlayInputType: only the keys are read (the guest's mouse has nothing to drive in the game)
    STEAM_RP_KEY_DOWN = 5, STEAM_RP_KEY_UP = 6,
};

// k_iSteamRemotePlayCallbacks (steam_api_internal.h) + 1..3
enum { STEAM_CB_SESSION_CONNECTED = 5701, STEAM_CB_SESSION_DISCONNECTED = 5702, STEAM_CB_GUEST_INVITE = 5703 };

// Steam's callback structures are packed to 4 bytes on Linux and macOS and to 8 on Windows (VALVE_CALLBACK_PACK_SMALL /
// _LARGE, steamclientpublic.h).
#ifdef _WIN32
#pragma pack(push, 8)
#else
#pragma pack(push, 4)
#endif
typedef struct {   // CallbackMsg_t
    SteamUser user;
    int32_t id;
    uint8_t *param;
    int32_t size;
} SteamCallbackMsg;

typedef struct {   // RemotePlayInput_t; scancode is a USB HID keyboard usage (ERemotePlayScancode), as SDL_Scancode
    SteamSession session;
    int32_t type;
    union {
        struct { int32_t scancode; uint32_t modifiers; uint32_t keycode; } key;
        char padding[56];
    };
} SteamRemoteInput;

typedef struct { SteamSession session; } SteamSessionEvent;   // SteamRemotePlaySession(Dis)Connected_t
typedef struct { char url[1024]; } SteamGuestInvite;          // SteamRemotePlayTogetherGuestInvite_t
#pragma pack(pop)

_Static_assert(sizeof(SteamRemoteInput) == 64, "RemotePlayInput_t ABI");
#ifdef _WIN32
_Static_assert(sizeof(SteamCallbackMsg) == 24, "CallbackMsg_t ABI");
#else
_Static_assert(sizeof(SteamCallbackMsg) == 20, "CallbackMsg_t ABI");
#endif

// What SteamInternal_SteamAPI_Init checks against the client: the interfaces used, each NUL-terminated, then one more NUL.
#define STEAM_INTERFACES "SteamFriends018\0STEAMREMOTEPLAY_INTERFACE_VERSION004\0"

typedef struct {
    int (STEAM_CALL *init)(const char *interfaces, SteamErrMsg *err);   // SteamInternal_SteamAPI_Init
    void (STEAM_CALL *shutdown)(void);
    SteamPipe (STEAM_CALL *pipe)(void);
    void (STEAM_CALL *dispatch_init)(void);
    void (STEAM_CALL *dispatch_frame)(SteamPipe p);
    bool (STEAM_CALL *dispatch_next)(SteamPipe p, SteamCallbackMsg *msg);
    void (STEAM_CALL *dispatch_free)(SteamPipe p);

    void *(STEAM_CALL *friends)(void);                                   // SteamAPI_SteamFriends_v018
    const char *(STEAM_CALL *persona_name)(void *friends, SteamId id);

    void *(STEAM_CALL *remote_play)(void);                               // SteamAPI_SteamRemotePlay_v004
    uint32_t (STEAM_CALL *session_count)(void *rp);
    SteamSession (STEAM_CALL *session_id)(void *rp, int index);
    SteamId (STEAM_CALL *session_steam_id)(void *rp, SteamSession s);
    const char *(STEAM_CALL *session_client_name)(void *rp, SteamSession s);
    bool (STEAM_CALL *show_ui)(void *rp);
    bool (STEAM_CALL *send_invite)(void *rp, SteamId friend_id);
    bool (STEAM_CALL *direct_input_on)(void *rp);
    void (STEAM_CALL *direct_input_off)(void *rp);
    uint32_t (STEAM_CALL *get_input)(void *rp, SteamRemoteInput *events, uint32_t max);
    void (STEAM_CALL *mouse_visible)(void *rp, SteamSession s, bool visible);
} SteamApi;

// The symbol each SteamApi field is bound to, in field order.
#define STEAM_SYMBOLS(X)                                                                                               \
    X(init, "SteamInternal_SteamAPI_Init") X(shutdown, "SteamAPI_Shutdown") X(pipe, "SteamAPI_GetHSteamPipe")         \
    X(dispatch_init, "SteamAPI_ManualDispatch_Init") X(dispatch_frame, "SteamAPI_ManualDispatch_RunFrame")            \
    X(dispatch_next, "SteamAPI_ManualDispatch_GetNextCallback")                                                       \
    X(dispatch_free, "SteamAPI_ManualDispatch_FreeLastCallback")                                                      \
    X(friends, "SteamAPI_SteamFriends_v018") X(persona_name, "SteamAPI_ISteamFriends_GetFriendPersonaName")           \
    X(remote_play, "SteamAPI_SteamRemotePlay_v004")                                                                   \
    X(session_count, "SteamAPI_ISteamRemotePlay_GetSessionCount")                                                     \
    X(session_id, "SteamAPI_ISteamRemotePlay_GetSessionID")                                                           \
    X(session_steam_id, "SteamAPI_ISteamRemotePlay_GetSessionSteamID")                                               \
    X(session_client_name, "SteamAPI_ISteamRemotePlay_GetSessionClientName")                                         \
    X(show_ui, "SteamAPI_ISteamRemotePlay_ShowRemotePlayTogetherUI")                                                  \
    X(send_invite, "SteamAPI_ISteamRemotePlay_BSendRemotePlayTogetherInvite")                                         \
    X(direct_input_on, "SteamAPI_ISteamRemotePlay_BEnableRemotePlayTogetherDirectInput")                              \
    X(direct_input_off, "SteamAPI_ISteamRemotePlay_DisableRemotePlayTogetherDirectInput")                             \
    X(get_input, "SteamAPI_ISteamRemotePlay_GetInput")                                                                \
    X(mouse_visible, "SteamAPI_ISteamRemotePlay_SetMouseVisibility")

#ifdef _WIN32
#define STEAM_LIBRARY "steam_api64.dll"
#elif defined(__APPLE__)
#define STEAM_LIBRARY "libsteam_api.dylib"
#else
#define STEAM_LIBRARY "libsteam_api.so"
#endif
