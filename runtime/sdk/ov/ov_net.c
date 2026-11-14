// The console's network for MKW: nifm, nsd, sockets, the ssl service's built-in certificates and the network service
// account. Off by default, as a console with no network: the game takes its offline path. With MKW_ONLINE=1 the
// sockets are the host's (Winsock / BSD), nifm reports a connection, and with MKW_ONLINE_SERVER the game's online play
// goes to a private server (net_online.h; the one built into mkw: runtime/srv): Nintendo's names resolve to it, its
// certificate is the built-in CA the game trusts, and the account's ID token carries this player's friend code.
// LAN play (MKW_LAN=1, Settings > Game > LAN play, or MKW_ONLINE=1): nifm's local network mode, the one the game makes
// for its LAN lobby (SetLocalNetworkMode(mode == 3) at main+0x2247a74), is connected with the LAN address
// (ov_lan_address, ov_lanaddr.c). Without MKW_ONLINE it reaches local network addresses only. MKW_LAN_PEERS lists the
// PCs of a virtual LAN that carries no broadcast (Tailscale): the lobby's broadcasts also go to each of them.
//
// Guest sockets are Horizon's BSD interface: BSD sockaddr (sin_len first), Linux errno numbers, O_NONBLOCK 0x800,
// MSG_DONTWAIT 0x80 (the values Horizon's bsd service uses, as switchbrew and yuzu document them).
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <windows.h>
#ifndef SIO_UDP_CONNRESET   // mstcpip.h of MinGW lacks it
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
typedef SOCKET HostSock;
#define HOST_BAD INVALID_SOCKET
#define host_close closesocket
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
typedef int HostSock;
#define HOST_BAD (-1)
#define host_close close
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "net_online.h"
#include "ov.h"
#include "platform/platform.h"

// nifm results (module 110); the description number is a hypothesis: any failure keeps the game offline.
#define NIFM_R_NO_NETWORK (110u | (1u << 9))
// nn::account::ResultNetworkServiceAccountUnavailable is module 124; this description number is a hypothesis.
#define ACCOUNT_R_NSA_UNAVAILABLE (124u | (280u << 9))

static int local_mode;   // SetLocalNetworkMode's last value

static int lan_up(void) { return local_mode && (ov_lan() || net_enabled()); }

static void ov_ok(Cpu *c) { ret_x(c, 0); }
// An error the network overrides hand the game, logged the first times per call site: the online mode shows only
// "a communication error", this says which call it was.
static void net_refused(Cpu *c, const char *what, uint64_t result) {
    static struct { uint64_t lr; int n; } seen[32];
    uint64_t lr = c->x[30];
    for (int i = 0; i < 32; i++) {
        if (seen[i].lr == lr || !seen[i].lr) {
            seen[i].lr = lr;
            if (seen[i].n++ < 2) {
                char d[96];
                rt_log("net: %s refused (result %#llx) to %s", what, (unsigned long long)result,
                       ldr_describe(lr - 4, d, sizeof d));
            }
            break;
        }
    }
    ret_x(c, result);
}
static void ov_nothing(Cpu *c) { (void)c; }
// void SetLocalNetworkMode(bool)
static void ov_local_mode(Cpu *c) {
    local_mode = (int)(arg_x(c, 0) & 1);
    rt_log("nifm: local network mode %s", local_mode ? "on (LAN play)" : "off");
}
// connected: online with a server set up, or the LAN lobby with LAN play on
static int nifm_up(void) { return lan_up() || net_enabled(); }
// bool IsNetworkAvailable()
static void ov_network_available(Cpu *c) { ret_x(c, (uint64_t)nifm_up()); }
// Result HandleNetworkRequestErrorResult()
static void ov_request_result(Cpu *c) {
    if (!nifm_up()) net_error_notice(0, 0);   // the console's network applet: said where the player can read it
    ret_x(c, nifm_up() ? 0 : NIFM_R_NO_NETWORK);
}
// Result GetCurrentIpConfigInfo(in_addr *ip, in_addr *mask, in_addr *gateway, in_addr *dns1, in_addr *dns2). LAN
// lobby: the LAN address (ov_lan_address). Online: the address of the route to the server, the one the other players
// reach (through Tailscale, its 100.x address).
static void ov_ip_config(Cpu *c) {
    uint8_t ip[4] = {0}, mask[4] = {255, 255, 255, 0}, gw[4];
    if (lan_up()) {
        if (ov_lan_address(ip, mask)) return ret_x(c, NIFM_R_NO_NETWORK);
    } else if (net_enabled()) {
        uint32_t a = net_local_ip();
        memcpy(ip, &a, 4);
    }
    if (!ip[0]) return ret_x(c, NIFM_R_NO_NETWORK);
    for (int i = 0; i < 4; i++) gw[i] = ip[i] & mask[i];
    gw[3] |= 1;   // a LAN needs no gateway; the usual first address, for whoever prints it
    const uint8_t *v[5] = {ip, mask, gw, gw, gw};
    for (int i = 0; i < 5; i++)
        if (arg_p(c, i)) memcpy(arg_p(c, i), v[i], 4);
    static uint32_t logged;   // each new address once
    uint32_t key;
    memcpy(&key, ip, 4);
    if (key != logged)
        logged = key, rt_log("nifm: %s, address %u.%u.%u.%u mask %u.%u.%u.%u", lan_up() ? "LAN mode" : "online",
                             ip[0], ip[1], ip[2], ip[3], mask[0], mask[1], mask[2], mask[3]);
    ret_x(c, 0);
}
// Result GetCurrentProxySetting(ProxySetting *): no proxy. The SDK copies 0xaa bytes of the profile's setting.
static void ov_proxy(Cpu *c) {
    if (!nifm_up()) return ret_x(c, NIFM_R_NO_NETWORK);
    void *out = arg_p(c, 0);
    if (out) memset(out, 0, 0xaa);
    ret_x(c, 0);
}

HOST_OVERRIDE("_ZN2nn4nifm10InitializeEv", ov_ok);
HOST_OVERRIDE("_ZN2nn4nifm18IsNetworkAvailableEv", ov_network_available);
HOST_OVERRIDE("_ZN2nn4nifm19SetLocalNetworkModeEb", ov_local_mode);
HOST_OVERRIDE("_ZN2nn4nifm20CancelNetworkRequestEv", ov_nothing);
HOST_OVERRIDE("_ZN2nn4nifm27SubmitNetworkRequestAndWaitEv", ov_nothing);
HOST_OVERRIDE("_ZN2nn4nifm31HandleNetworkRequestErrorResultEv", ov_request_result);
HOST_OVERRIDE("_ZN2nn4nifm22GetCurrentIpConfigInfoEP7in_addrS2_S2_S2_S2_", ov_ip_config);
HOST_OVERRIDE("_ZN2nn4nifm22GetCurrentProxySettingEPNS0_12ProxySettingE", ov_proxy);

// ==== nn::friends: no friend list from the console (MKW's online friends come from its NPLN server); play history
// registration is accepted and dropped. Offline the list is unavailable; with a server it is available and empty,
// as Droplet answers (its friend service: CheckFriendListAvailability true, GetPlayHistoryRegistrationKey success),
// since the online mode stops with "a communication error" when either fails (main+0x2228108, main+0x2228180).
#define FRIENDS_R_UNAVAILABLE (121u | (1u << 9))   // module 121; description is a hypothesis
static int online(void);
static void ov_friend_list_available(Cpu *c) {
    uint8_t *out = arg_p(c, 0);
    if (out) *out = (uint8_t)online();
    ret_x(c, 0);
}
static void ov_friend_list(Cpu *c) {
    int32_t *count = arg_p(c, 0);
    if (count) *count = 0;
    ret_x(c, 0);
}
static void ov_friends_unavailable(Cpu *c) {
    if (online()) return ret_x(c, 0);
    net_refused(c, "friends", FRIENDS_R_UNAVAILABLE);
}
// friends::AsyncContext::HasDone(bool *): the context EnsureFriendListAvailable got keeps no session (its SDK
// destructor skips it, sdk+0x3c18f0): done.
static void ov_friends_async_done(Cpu *c) {
    uint8_t *out = arg_p(c, 1);
    if (out) *out = 1;
    ret_x(c, 0);
}
HOST_OVERRIDE("_ZN2nn7friends12AsyncContext7HasDoneEPb", ov_friends_async_done);

HOST_OVERRIDE("_ZN2nn7friends10InitializeEv", ov_nothing);
HOST_OVERRIDE("_ZN2nn7friends12SetAllocatorEPFPvmEPFvS1_mE", ov_nothing);
HOST_OVERRIDE("_ZN2nn7friends27CheckFriendListAvailabilityEPbRKNS_7account3UidE", ov_friend_list_available);
HOST_OVERRIDE("_ZN2nn7friends13GetFriendListEPiPNS0_6FriendERKNS_7account3UidEiiRKNS0_12FriendFilterE", ov_friend_list);
HOST_OVERRIDE("_ZN2nn7friends25EnsureFriendListAvailableEPNS0_12AsyncContextERKNS_7account3UidE", ov_friends_unavailable);
HOST_OVERRIDE("_ZN2nn7friends29GetPlayHistoryRegistrationKeyEPNS0_26PlayHistoryRegistrationKeyERKNS_7account3UidEb",
              ov_friends_unavailable);
HOST_OVERRIDE("_ZN2nn7friends14AddPlayHistoryERKNS_7account3UidERKNS0_26PlayHistoryRegistrationKeyERKNS0_15InAppScreenNameESA_",
              ov_ok);

// ==== nn::ldn (local wireless play): disabled, as on a console in airplane mode. MKW's LAN play uses sockets, not
// ldn. Initialize fails, so the game does not use the rest; they fail too in case it does.
#define LDN_R_DISABLED (203u | (22u << 9))   // module 203; description is a hypothesis
static void ov_ldn_disabled(Cpu *c) { net_refused(c, "ldn", LDN_R_DISABLED); }
static void ov_ldn_state(Cpu *c) { ret_x(c, 0); }

HOST_OVERRIDE("_ZN2nn3ldn10InitializeEv", ov_ldn_disabled);
HOST_OVERRIDE("_ZN2nn3ldn8FinalizeEv", ov_nothing);
HOST_OVERRIDE("_ZN2nn3ldn8GetStateEv", ov_ldn_state);
HOST_OVERRIDE("_ZN2nn3ldn19GetDisconnectReasonEv", ov_ldn_state);
HOST_OVERRIDE("_ZN2nn3ldn10DisconnectEv", ov_ldn_disabled);
HOST_OVERRIDE("_ZN2nn3ldn11OpenStationEv", ov_ldn_disabled);
HOST_OVERRIDE("_ZN2nn3ldn12CloseStationEv", ov_ldn_disabled);
HOST_OVERRIDE("_ZN2nn3ldn15OpenAccessPointEv", ov_ldn_disabled);
HOST_OVERRIDE("_ZN2nn3ldn16CloseAccessPointEv", ov_ldn_disabled);
HOST_OVERRIDE("_ZN2nn3ldn14DestroyNetworkEv", ov_ldn_disabled);
HOST_OVERRIDE("_ZN2nn3ldn4ScanEPNS0_11NetworkInfoEPiiRKNS0_10ScanFilterEi", ov_ldn_disabled);
HOST_OVERRIDE("_ZN2nn3ldn11ScanPrivateEPNS0_11NetworkInfoEPiiRKNS0_10ScanFilterEi", ov_ldn_disabled);
HOST_OVERRIDE("_ZN2nn3ldn13CreateNetworkERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_10UserConfigE", ov_ldn_disabled);
HOST_OVERRIDE("_ZN2nn3ldn20CreateNetworkPrivateERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_17SecurityParameterERKNS0_10UserConfigEiPKNS0_12AddressEntryE",
              ov_ldn_disabled);
HOST_OVERRIDE("_ZN2nn3ldn7ConnectERKNS0_11NetworkInfoERKNS0_14SecurityConfigERKNS0_10UserConfigEiNS0_13ConnectOptionE",
              ov_ldn_disabled);
HOST_OVERRIDE("_ZN2nn3ldn14ConnectPrivateERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_17SecurityParameterERKNS0_10UserConfigEiNS0_13ConnectOptionE",
              ov_ldn_disabled);
HOST_OVERRIDE("_ZN2nn3ldn14GetIpv4AddressEPNS0_11Ipv4AddressEPNS0_10SubnetMaskE", ov_ldn_disabled);
HOST_OVERRIDE("_ZN2nn3ldn14GetNetworkInfoEPNS0_11NetworkInfoE", ov_ldn_disabled);
HOST_OVERRIDE("_ZN2nn3ldn20GetSecurityParameterEPNS0_17SecurityParameterE", ov_ldn_disabled);
HOST_OVERRIDE("_ZN2nn3ldn16SetAdvertiseDataEPKvm", ov_ldn_disabled);
HOST_OVERRIDE("_ZN2nn3ldn20AddAcceptFilterEntryENS0_10MacAddressE", ov_ldn_disabled);
HOST_OVERRIDE("_ZN2nn3ldn22SetStationAcceptPolicyENS0_12AcceptPolicyE", ov_ldn_disabled);
HOST_OVERRIDE("_ZN2nn3ldn6RejectENS0_11Ipv4AddressE", ov_ldn_disabled);
HOST_OVERRIDE("_ZN2nn3ldn22AttachStateChangeEventEPNS_2os15SystemEventTypeE", ov_nothing);

// ==== nn::nsd: the environment is lp1 (production) and names resolve to themselves; the socket layer sends
// Nintendo's to the server.
static void ov_nsd_env(Cpu *c) {
    if (!net_enabled()) return ret_x(c, NIFM_R_NO_NETWORK);
    char *out = arg_p(c, 0);
    if (out) memcpy(out, "lp1\0\0\0\0", 8);
    ret_x(c, 0);
}
// Result ResolveEx(Fqdn *out, const Fqdn &in), Fqdn = char[0x100]
static void ov_nsd_resolve(Cpu *c) {
    if (!net_enabled()) return ret_x(c, NIFM_R_NO_NETWORK);
    char *out = arg_p(c, 0);
    const char *in = arg_p(c, 1);
    if (!out || !in) return ret_x(c, NIFM_R_NO_NETWORK);
    snprintf(out, 0x100, "%.255s", in);
    ret_x(c, 0);
}
HOST_OVERRIDE("_ZN2nn3nsd24GetEnvironmentIdentifierEPNS0_21EnvironmentIdentifierE", ov_nsd_env);
HOST_OVERRIDE("_ZN2nn3nsd9ResolveExEPNS0_4FqdnERKS1_", ov_nsd_resolve);

// ---- nn::prepo: play reports are built by the SDK's own code and never sent.
HOST_OVERRIDE("_ZN2nn5prepo28RequestImmediateTransmissionEv", ov_ok);

// ==== sockets
enum {   // guest values (Horizon bsd: Linux errno numbers)
    G_EINTR = 4, G_EBADF = 9, G_EAGAIN = 11, G_ENOMEM = 12, G_EACCES = 13, G_EFAULT = 14, G_EINVAL = 22,
    G_EMFILE = 24, G_EPIPE = 32, G_ENOTSOCK = 88, G_EMSGSIZE = 90, G_ENOPROTOOPT = 92, G_EPROTONOSUPPORT = 93,
    G_EOPNOTSUPP = 95, G_EAFNOSUPPORT = 97, G_EADDRINUSE = 98, G_EADDRNOTAVAIL = 99, G_ENETDOWN = 100,
    G_ENETUNREACH = 101, G_ECONNABORTED = 103, G_ECONNRESET = 104, G_ENOBUFS = 105, G_EISCONN = 106,
    G_ENOTCONN = 107, G_ETIMEDOUT = 110, G_ECONNREFUSED = 111, G_EHOSTUNREACH = 113, G_EALREADY = 114,
    G_EINPROGRESS = 115,
};
#define G_O_NONBLOCK 0x800
#define G_MSG_OOB 0x1
#define G_MSG_PEEK 0x2
#define G_MSG_WAITALL 0x40
#define G_MSG_DONTWAIT 0x80
#define G_SOCK_NONBLOCK 0x20000000   // OR'ed into socket()'s type
#define G_SOCK_CLOEXEC 0x10000000

static __thread int last_errno;

// errno for the callers: nn::socket's own (GetLastErrno) and the C library's (gRPC reads errno).
static uint64_t errno_fn;
static void set_errno(Cpu *c, int e) {
    last_errno = e;
    if (!errno_fn) errno_fn = ldr_symbol("__errno_location");
    if (errno_fn && c) {
        int32_t *p = (int32_t *)(uintptr_t)rt_call(c, errno_fn, 0);
        if (p) *p = e;
    }
}

static int host_error(void) {
#ifdef _WIN32
    switch (WSAGetLastError()) {
    case WSAEWOULDBLOCK: return G_EAGAIN;
    case WSAEINPROGRESS: return G_EINPROGRESS;
    case WSAEALREADY: return G_EALREADY;
    case WSAENOTSOCK: return G_ENOTSOCK;
    case WSAEMSGSIZE: return G_EMSGSIZE;
    case WSAENOPROTOOPT: return G_ENOPROTOOPT;
    case WSAEPROTONOSUPPORT: return G_EPROTONOSUPPORT;
    case WSAEOPNOTSUPP: return G_EOPNOTSUPP;
    case WSAEAFNOSUPPORT: return G_EAFNOSUPPORT;
    case WSAEADDRINUSE: return G_EADDRINUSE;
    case WSAEADDRNOTAVAIL: return G_EADDRNOTAVAIL;
    case WSAENETDOWN: return G_ENETDOWN;
    case WSAENETUNREACH: return G_ENETUNREACH;
    case WSAECONNABORTED: return G_ECONNABORTED;
    case WSAECONNRESET: return G_ECONNRESET;
    case WSAENOBUFS: return G_ENOBUFS;
    case WSAEISCONN: return G_EISCONN;
    case WSAENOTCONN: return G_ENOTCONN;
    case WSAESHUTDOWN: return G_EPIPE;
    case WSAETIMEDOUT: return G_ETIMEDOUT;
    case WSAECONNREFUSED: return G_ECONNREFUSED;
    case WSAEHOSTUNREACH: return G_EHOSTUNREACH;
    case WSAEACCES: return G_EACCES;
    case WSAEFAULT: return G_EFAULT;
    case WSAEMFILE: return G_EMFILE;
    case WSAEINTR: return G_EINTR;
    default: return G_EINVAL;
    }
#else
    return errno == EWOULDBLOCK ? G_EAGAIN : errno;   // Linux numbers are Horizon's
#endif
}

enum { FD_FREE, FD_SOCKET, FD_EVENT };
#define GFD_MAX 512
#define GFD_FIRST 3
#define GFD_OPTS 24
typedef struct {
    int kind, type, nonblock;
    HostSock s;             // the socket; an eventfd's read end (a loopback UDP socket the writes wake)
    HostSock w;             // an eventfd's write end
    uint64_t value;         // an eventfd's counter
    int semaphore;
    struct { uint32_t level, name, len; uint8_t v[16]; } opt[GFD_OPTS];   // set options, read back as set (gRPC)
    int nopt;
} GFd;
static GFd gfd[GFD_MAX];
static PlatMutex fd_lock = PLAT_MUTEX_INIT;

static void sock_startup(void) {
#ifdef _WIN32
    static volatile int done;
    if (!done) {
        WSADATA w;
        if (WSAStartup(MAKEWORD(2, 2), &w)) rt_log("net: WSAStartup failed");
        done = 1;
    }
#endif
}

static int fd_new(int kind) {
    plat_mutex_lock(&fd_lock);
    for (int i = GFD_FIRST; i < GFD_MAX; i++)
        if (gfd[i].kind == FD_FREE) {
            memset(&gfd[i], 0, sizeof gfd[i]);
            gfd[i].kind = kind;
            gfd[i].s = gfd[i].w = HOST_BAD;
            plat_mutex_unlock(&fd_lock);
            return i;
        }
    plat_mutex_unlock(&fd_lock);
    return -1;
}

static GFd *fd_get(int fd, int kind) {
    if (fd < GFD_FIRST || fd >= GFD_MAX) return NULL;
    GFd *f = &gfd[fd];
    return f->kind != FD_FREE && (kind == 0 || f->kind == kind) ? f : NULL;   // kind 0: any open one
}

static void host_nonblock(HostSock s, int on) {
#ifdef _WIN32
    u_long v = on ? 1 : 0;
    ioctlsocket(s, FIONBIO, &v);
#else
    int fl = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, on ? fl | O_NONBLOCK : fl & ~O_NONBLOCK);
#endif
}

// guest sockaddr_in {u8 len, u8 family, u16 port, u32 addr, u8 zero[8]} <-> host
static int to_host_addr(const uint8_t *g, uint32_t len, struct sockaddr_in *h) {
    if (!g || len < 8) return 0;
    memset(h, 0, sizeof *h);
    h->sin_family = AF_INET;
    memcpy(&h->sin_port, g + 2, 2);
    memcpy(&h->sin_addr, g + 4, 4);
    return g[1] == 2 || g[1] == 0;
}
static void to_guest_addr(const struct sockaddr_in *h, uint8_t *g, uint32_t *len) {
    if (!g || !len) return;
    uint8_t t[16] = {16, 2};
    memcpy(t + 2, &h->sin_port, 2);
    memcpy(t + 4, &h->sin_addr, 4);
    memcpy(g, t, *len < 16 ? *len : 16);
    *len = 16;
}

// Sockets are the host's online (MKW_ONLINE) or with LAN play (MKW_LAN); LAN play alone reaches local network
// addresses only (ov_lan_local), as upstream's ov_socket.c did.
static int sock_on(void) { return net_enabled() || ov_lan(); }
static int lan_refused(Cpu *c, const struct sockaddr_in *h, const char *call) {
    uint32_t a;
    memcpy(&a, &h->sin_addr, 4);
    if (net_enabled() || ov_lan_local(a)) return 0;
    static int n;
    if (n++ < 8) {
        char t[INET_ADDRSTRLEN] = "?";
        inet_ntop(AF_INET, &h->sin_addr, t, sizeof t);
        rt_log("socket: %s to %s refused: LAN play reaches only local network addresses", call, t);
    }
    set_errno(c, G_ENETUNREACH);
    return 1;
}

// The server's port 443 is its online port (the server listens unprivileged, 8443 by default).
static void redirect(struct sockaddr_in *h) {
    const NetServer *s = net_server();
    if (s->on && !memcmp(&h->sin_addr, s->ip, 4) && ntohs(h->sin_port) == 443) h->sin_port = htons(s->port);
}

static void ret_fail(Cpu *c, int e) {
    set_errno(c, e);
    ret_x(c, (uint64_t)-1);
}

// int Socket(int domain, int type, int protocol)
static void ov_socket(Cpu *c) {
    sock_startup();
    int domain = (int)arg_x(c, 0), type = (int)arg_x(c, 1), proto = (int)arg_x(c, 2);
    int nonblock = (type & G_SOCK_NONBLOCK) != 0;
    type &= ~(G_SOCK_NONBLOCK | G_SOCK_CLOEXEC);
    if (!sock_on()) return ret_fail(c, G_ENETDOWN);   // offline: the game's no-connection path (issue #63)
    if (domain != 2) return ret_fail(c, G_EAFNOSUPPORT);
    if (type != 1 && type != 2) return ret_fail(c, G_EPROTONOSUPPORT);   // no raw sockets
    int fd = fd_new(FD_SOCKET);
    if (fd < 0) return ret_fail(c, G_EMFILE);
    HostSock s = socket(AF_INET, type == 1 ? SOCK_STREAM : SOCK_DGRAM, proto ? proto : (type == 1 ? 6 : 17));
    if (s == HOST_BAD) {
        int e = host_error();
        gfd[fd].kind = FD_FREE;
        return ret_fail(c, e);
    }
#ifdef _WIN32
    if (type == 2) {
        // An ICMP port unreachable would make the next recvfrom of this UDP socket fail with WSAECONNRESET, which
        // Pia's peer sockets would take as a dead socket: Windows only, off.
        BOOL off = FALSE;
        DWORD got = 0;
        WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof off, NULL, 0, &got, NULL, NULL);
    }
#endif
    if (nonblock) host_nonblock(s, 1);
    gfd[fd].s = s;
    gfd[fd].type = type;
    gfd[fd].nonblock = nonblock;
    ret_x(c, (uint64_t)fd);
}

// int Close(int fd)
static void ov_close(Cpu *c) {
    int fd = (int)arg_x(c, 0);
    plat_mutex_lock(&fd_lock);
    GFd *f = fd_get(fd, 0);
    HostSock s = f ? f->s : HOST_BAD, w = f ? f->w : HOST_BAD;
    if (f) f->kind = FD_FREE;
    plat_mutex_unlock(&fd_lock);
    if (!f) return ret_fail(c, G_EBADF);
    if (s != HOST_BAD) host_close(s);
    if (w != HOST_BAD) host_close(w);
    ret_x(c, 0);
}

static GFd *sock_of(Cpu *c, int fd) {
    GFd *f = fd_get(fd, FD_SOCKET);
    if (!f) set_errno(c, G_EBADF);
    return f;
}

// A call that may block gives the virtual core up meanwhile (kern_core_away), so the other game threads run.
#define BLOCKING(f, flags) (!(f)->nonblock && !((flags) & G_MSG_DONTWAIT))
#define AWAY(cond, stmt)                          \
    do {                                          \
        if (cond) {                               \
            int away_ = kern_core_away();         \
            stmt;                                 \
            kern_core_back(&away_);               \
        } else {                                  \
            stmt;                                 \
        }                                         \
    } while (0)

// int Bind(int fd, const sockaddr *, socklen_t)
static void ov_bind(Cpu *c) {
    GFd *f = sock_of(c, (int)arg_x(c, 0));
    struct sockaddr_in h;
    if (!f) return ret_x(c, (uint64_t)-1);
    if (!to_host_addr(arg_p(c, 1), (uint32_t)arg_x(c, 2), &h)) return ret_fail(c, G_EINVAL);
    if (lan_refused(c, &h, "Bind")) return ret_x(c, (uint64_t)-1);
    if (!h.sin_addr.s_addr) h.sin_addr.s_addr = ov_lan_bind_address();
    if (bind(f->s, (struct sockaddr *)&h, sizeof h)) return ret_fail(c, host_error());
    ret_x(c, 0);
}

// int Connect(int fd, const sockaddr *, socklen_t)
static void ov_connect(Cpu *c) {
    GFd *f = sock_of(c, (int)arg_x(c, 0));
    struct sockaddr_in h;
    if (!f) return ret_x(c, (uint64_t)-1);
    if (!to_host_addr(arg_p(c, 1), (uint32_t)arg_x(c, 2), &h)) return ret_fail(c, G_EINVAL);
    if (lan_refused(c, &h, "Connect")) return ret_x(c, (uint64_t)-1);
    redirect(&h);
    int r;
    AWAY(!f->nonblock, r = connect(f->s, (struct sockaddr *)&h, sizeof h));
    if (!r) return ret_x(c, 0);
    int e = host_error();
    if (e == G_EISCONN) return ret_x(c, 0);
    // Windows says WOULDBLOCK for a non-blocking connect under way, POSIX EINPROGRESS
    ret_fail(c, e == G_EAGAIN && f->type == 1 ? G_EINPROGRESS : e);
}

// int Listen(int fd, int backlog)
static void ov_listen(Cpu *c) {
    GFd *f = sock_of(c, (int)arg_x(c, 0));
    if (!f) return ret_x(c, (uint64_t)-1);
    if (listen(f->s, (int)arg_x(c, 1))) return ret_fail(c, host_error());
    ret_x(c, 0);
}

// int Accept(int fd, sockaddr *, socklen_t *)
static void ov_accept(Cpu *c) {
    GFd *f = sock_of(c, (int)arg_x(c, 0));
    if (!f) return ret_x(c, (uint64_t)-1);
    struct sockaddr_in h;
#ifdef _WIN32
    int hl = sizeof h;
#else
    socklen_t hl = sizeof h;
#endif
    HostSock s;
    AWAY(!f->nonblock, s = accept(f->s, (struct sockaddr *)&h, &hl));
    if (s == HOST_BAD) return ret_fail(c, host_error());
    int fd = fd_new(FD_SOCKET);
    if (fd < 0) {
        host_close(s);
        return ret_fail(c, G_EMFILE);
    }
    gfd[fd].s = s;
    gfd[fd].type = 1;
    to_guest_addr(&h, arg_p(c, 1), arg_p(c, 2));
    ret_x(c, (uint64_t)fd);
}

static int host_flags(int g) {
    return (g & G_MSG_OOB ? MSG_OOB : 0) | (g & G_MSG_PEEK ? MSG_PEEK : 0) | (g & G_MSG_WAITALL ? MSG_WAITALL : 0);
}

// MSG_DONTWAIT on a blocking socket: non-blocking for this call only.
static void dontwait(GFd *f, int flags, int on) {
    if ((flags & G_MSG_DONTWAIT) && !f->nonblock) host_nonblock(f->s, on);
}

// ssize_t SendTo(int fd, const void *, size_t, int flags, const sockaddr *, socklen_t); Send has no address.
static void send_common(Cpu *c, int with_addr) {
    GFd *f = sock_of(c, (int)arg_x(c, 0));
    if (!f) return ret_x(c, (uint64_t)-1);
    const char *buf = arg_p(c, 1);
    size_t len = (size_t)arg_x(c, 2);
    int flags = (int)arg_x(c, 3);
    struct sockaddr_in h;
    int has = with_addr && arg_p(c, 4) && to_host_addr(arg_p(c, 4), (uint32_t)arg_x(c, 5), &h);
    if (has && lan_refused(c, &h, "SendTo")) return ret_x(c, (uint64_t)-1);
    if (has) redirect(&h);
    if (len > 0x7fffffff) len = 0x7fffffff;
    // A broadcast of the LAN lobby: also, one datagram each, to the PCs of MKW_LAN_PEERS (a virtual LAN such as
    // Tailscale carries no broadcast). Sent: the broadcast's own result may then be a failure, the lobby is reached.
    uint32_t peers[OV_LAN_PEERS_MAX], dst;
    int npeers = 0, fanned = 0;
    if (has && f->type == 2) memcpy(&dst, &h.sin_addr, 4), npeers = ov_lan_peers(dst, peers, OV_LAN_PEERS_MAX);
    for (int i = 0; i < npeers; i++) {
        struct sockaddr_in p = h;
        memcpy(&p.sin_addr, &peers[i], 4);
        if (sendto(f->s, buf, (int)len, 0, (struct sockaddr *)&p, sizeof p) >= 0) fanned = 1;
    }
    long n;
    dontwait(f, flags, 1);
    AWAY(BLOCKING(f, flags) && f->type == 1,
         n = has ? (long)sendto(f->s, buf, (int)len, host_flags(flags), (struct sockaddr *)&h, sizeof h)
                 : (long)send(f->s, buf, (int)len, host_flags(flags)));
    int e = n < 0 ? host_error() : 0;
    dontwait(f, flags, 0);
    if (n < 0 && fanned) n = (long)len;
    if (n < 0) return ret_fail(c, e);
    ret_x(c, (uint64_t)n);
}
// The guest's buffer goes straight to the host's socket call: its pages committed first (Windows' lazy commit,
// memory.c: the kernel does not fault into the handler), as upstream's ov_socket.c does
static void ov_send(Cpu *c) {
    mem_commit_range(arg_x(c, 1), arg_x(c, 2));
    send_common(c, 0);
}
static void ov_sendto(Cpu *c) {
    mem_commit_range(arg_x(c, 1), arg_x(c, 2));
    send_common(c, 1);
}

// ssize_t RecvFrom(int fd, void *, size_t, int flags, sockaddr *, socklen_t *); Recv has no address.
static void recv_common(Cpu *c, int with_addr) {
    GFd *f = sock_of(c, (int)arg_x(c, 0));
    if (!f) return ret_x(c, (uint64_t)-1);
    char *buf = arg_p(c, 1);
    size_t len = (size_t)arg_x(c, 2);
    int flags = (int)arg_x(c, 3);
    struct sockaddr_in h;
#ifdef _WIN32
    int hl = sizeof h;
#else
    socklen_t hl = sizeof h;
#endif
    memset(&h, 0, sizeof h);
    if (len > 0x7fffffff) len = 0x7fffffff;
    long n;
    dontwait(f, flags, 1);
    AWAY(BLOCKING(f, flags), n = (long)recvfrom(f->s, buf, (int)len, host_flags(flags), (struct sockaddr *)&h, &hl));
    int e = n < 0 ? host_error() : 0;
#ifdef _WIN32
    if (n < 0 && e == G_EMSGSIZE) n = (long)len, e = 0;   // POSIX truncates a datagram silently
#endif
    dontwait(f, flags, 0);
    if (n < 0) return ret_fail(c, e);
    if (with_addr) to_guest_addr(&h, arg_p(c, 4), arg_p(c, 5));
    ret_x(c, (uint64_t)n);
}
static void ov_recv(Cpu *c) {
    mem_commit_range(arg_x(c, 1), arg_x(c, 2));
    recv_common(c, 0);
}
static void ov_recvfrom(Cpu *c) {
    mem_commit_range(arg_x(c, 1), arg_x(c, 2));
    recv_common(c, 1);
}

// msghdr (64-bit BSD): name @0, namelen @8, iov @16, iovlen @24, control @32, controllen @40, flags @44;
// iovec {base @0, len @8}. Gathered into one buffer: datagrams and TCP writes keep their bytes and order.
static size_t iov_total(const uint8_t *m, const uint64_t **iov, int *n) {
    *iov = (const uint64_t *)(uintptr_t)*(const uint64_t *)(m + 16);
    *n = *(const int32_t *)(m + 24);
    size_t total = 0;
    for (int i = 0; *iov && i < *n; i++) total += (size_t)(*iov)[2 * i + 1];
    return total;
}

static void ov_sendmsg(Cpu *c) {
    uint8_t *m = arg_p(c, 1);
    if (!m) return ret_fail(c, G_EFAULT);
    const uint64_t *iov;
    int n;
    size_t total = iov_total(m, &iov, &n), o = 0;
    char *buf = malloc(total ? total : 1);
    if (!buf) return ret_fail(c, G_ENOMEM);
    for (int i = 0; iov && i < n; o += (size_t)iov[2 * i + 1], i++)
        memcpy(buf + o, (const void *)(uintptr_t)iov[2 * i], (size_t)iov[2 * i + 1]);
    uint64_t flags = arg_x(c, 2), name = *(uint64_t *)m;
    c->x[1] = (uint64_t)(uintptr_t)buf, c->x[2] = total, c->x[3] = flags;
    c->x[4] = name, c->x[5] = *(uint32_t *)(m + 8);
    send_common(c, name != 0);
    free(buf);
}

static void ov_recvmsg(Cpu *c) {
    uint8_t *m = arg_p(c, 1);
    if (!m) return ret_fail(c, G_EFAULT);
    const uint64_t *iov;
    int n;
    size_t total = iov_total(m, &iov, &n);
    char *buf = malloc(total ? total : 1);
    if (!buf) return ret_fail(c, G_ENOMEM);
    uint64_t flags = arg_x(c, 2), name = *(uint64_t *)m;
    c->x[1] = (uint64_t)(uintptr_t)buf, c->x[2] = total, c->x[3] = flags;
    c->x[4] = name, c->x[5] = (uint64_t)(uintptr_t)(m + 8);
    recv_common(c, name != 0);
    int64_t got = (int64_t)c->x[0];
    size_t o = 0;
    for (int i = 0; iov && got > 0 && i < n && o < (size_t)got; o += (size_t)iov[2 * i + 1], i++) {
        size_t k = (size_t)got - o < (size_t)iov[2 * i + 1] ? (size_t)got - o : (size_t)iov[2 * i + 1];
        memcpy((void *)(uintptr_t)iov[2 * i], buf + o, k);
    }
    *(uint32_t *)(m + 40) = 0;   // no control data
    *(int32_t *)(m + 44) = 0;
    free(buf);
}

// int Shutdown(int fd, int how): 0, 1, 2 as SD_RECEIVE, SD_SEND, SD_BOTH
static void ov_shutdown(Cpu *c) {
    GFd *f = sock_of(c, (int)arg_x(c, 0));
    if (!f) return ret_x(c, (uint64_t)-1);
    if (shutdown(f->s, (int)arg_x(c, 1))) return ret_fail(c, host_error());
    ret_x(c, 0);
}

// int Fcntl(int fd, int cmd, ...): F_GETFL 3, F_SETFL 4 with O_NONBLOCK (variadic: the third argument in x2)
static void ov_fcntl(Cpu *c) {
    int fd = (int)arg_x(c, 0), cmd = (int)arg_x(c, 1);
    GFd *f = fd_get(fd, 0);
    if (!f) return ret_fail(c, G_EBADF);
    if (cmd == 3) return ret_x(c, (uint64_t)(2 | (f->nonblock ? G_O_NONBLOCK : 0)));   // O_RDWR
    if (cmd == 4) {
        f->nonblock = (arg_x(c, 2) & G_O_NONBLOCK) != 0;
        if (f->kind == FD_SOCKET) host_nonblock(f->s, f->nonblock);
    }
    ret_x(c, 0);
}

static int opt_find(GFd *f, uint32_t level, uint32_t name) {
    for (int i = 0; i < f->nopt; i++)
        if (f->opt[i].level == level && f->opt[i].name == name) return i;
    return -1;
}

// int SetSockOpt(int fd, int level, int name, const void *, socklen_t)
static void ov_setsockopt(Cpu *c) {
    GFd *f = fd_get((int)arg_x(c, 0), 0);
    if (!f) return ret_fail(c, G_EBADF);
    uint32_t level = (uint32_t)arg_x(c, 1), name = (uint32_t)arg_x(c, 2), len = (uint32_t)arg_x(c, 4);
    const uint8_t *v = arg_p(c, 3);
    if (!v && len) return ret_fail(c, G_EFAULT);
    int i = opt_find(f, level, name);
    if (i < 0 && f->nopt < GFD_OPTS) i = f->nopt++;
    if (i >= 0) {
        f->opt[i].level = level, f->opt[i].name = name, f->opt[i].len = len < 16 ? len : 16;
        if (v) memcpy(f->opt[i].v, v, f->opt[i].len);
    }
    if (f->kind != FD_SOCKET || !v) return ret_x(c, 0);
    int32_t iv = 0;
    memcpy(&iv, v, len < 4 ? len : 4);
    int r = 0;
    if (level == 0xffff) {   // SOL_SOCKET, BSD numbers
        switch (name) {
        case 0x4: r = setsockopt(f->s, SOL_SOCKET, SO_REUSEADDR, (const char *)&iv, 4); break;
        case 0x8: r = setsockopt(f->s, SOL_SOCKET, SO_KEEPALIVE, (const char *)&iv, 4); break;
        case 0x20: r = setsockopt(f->s, SOL_SOCKET, SO_BROADCAST, (const char *)&iv, 4); break;
        case 0x1001: r = setsockopt(f->s, SOL_SOCKET, SO_SNDBUF, (const char *)&iv, 4); break;
        case 0x1002: r = setsockopt(f->s, SOL_SOCKET, SO_RCVBUF, (const char *)&iv, 4); break;
        case 0x80: {   // linger {u32 on, u32 seconds}
            uint32_t lv[2] = {0, 0};
            memcpy(lv, v, len < 8 ? len : 8);
            struct linger l = {(unsigned short)(lv[0] != 0), (unsigned short)lv[1]};
            r = setsockopt(f->s, SOL_SOCKET, SO_LINGER, (const char *)&l, sizeof l);
            break;
        }
        case 0x1005: case 0x1006: {   // timeval {s64 sec, s64 usec}
            int64_t tv64[2] = {0, 0};
            memcpy(tv64, v, len < 16 ? len : 16);
#ifdef _WIN32
            DWORD ms = (DWORD)(tv64[0] * 1000 + tv64[1] / 1000);
            r = setsockopt(f->s, SOL_SOCKET, name == 0x1005 ? SO_SNDTIMEO : SO_RCVTIMEO, (const char *)&ms, sizeof ms);
#else
            struct timeval tv = {(time_t)tv64[0], (suseconds_t)tv64[1]};
            r = setsockopt(f->s, SOL_SOCKET, name == 0x1005 ? SO_SNDTIMEO : SO_RCVTIMEO, &tv, sizeof tv);
#endif
            break;
        }
        default: break;   // kept and read back only (Switch 2's own options among them)
        }
    } else if (level == 6 && name == 1) {   // TCP_NODELAY
        r = setsockopt(f->s, IPPROTO_TCP, TCP_NODELAY, (const char *)&iv, 4);
    } else if (level == 0 && (name == 3 || name == 4 || (name >= 9 && name <= 13))) {
        // IP_TOS, IP_TTL, IP_MULTICAST_IF/TTL/LOOP, IP_ADD/DROP_MEMBERSHIP: the same numbers and layouts in Winsock
        mem_commit_range((uint64_t)(uintptr_t)v, len);   // read by the kernel (Windows' lazy commit)
        r = setsockopt(f->s, IPPROTO_IP, (int)name, (const char *)v, (int)len);
    }
    if (r) rt_log("net: setsockopt level %#x name %#x on fd %d: host error %d (kept)", level, name,
                  (int)arg_x(c, 0), host_error());
    ret_x(c, 0);
}

// int GetSockOpt(int fd, int level, int name, void *, socklen_t *)
static void ov_getsockopt(Cpu *c) {
    GFd *f = fd_get((int)arg_x(c, 0), 0);
    if (!f) return ret_fail(c, G_EBADF);
    uint32_t level = (uint32_t)arg_x(c, 1), name = (uint32_t)arg_x(c, 2);
    uint8_t *out = arg_p(c, 3);
    uint32_t *len = arg_p(c, 4);
    if (!out || !len) return ret_fail(c, G_EFAULT);
    int32_t v = 0;
    if (level == 0xffff && name == 0x1007 && f->kind == FD_SOCKET) {   // SO_ERROR
        int e = 0;
#ifdef _WIN32
        int el = sizeof e;
        getsockopt(f->s, SOL_SOCKET, SO_ERROR, (char *)&e, &el);
        if (e) {
            WSASetLastError(e);
            e = host_error();
        }
#else
        socklen_t el = sizeof e;
        getsockopt(f->s, SOL_SOCKET, SO_ERROR, &e, &el);
#endif
        v = e;
    } else if (level == 0xffff && name == 0x1008) {   // SO_TYPE
        v = f->type;
    } else {
        int i = opt_find(f, level, name);
        if (i >= 0) {
            uint32_t n = *len < f->opt[i].len ? *len : f->opt[i].len;
            memcpy(out, f->opt[i].v, n);
            *len = n;
            return ret_x(c, 0);
        }
    }
    uint32_t n = *len < 4 ? *len : 4;
    memcpy(out, &v, n);
    *len = n;
    ret_x(c, 0);
}

// int GetSockName / GetPeerName(int fd, sockaddr *, socklen_t *)
static void name_common(Cpu *c, int peer) {
    GFd *f = sock_of(c, (int)arg_x(c, 0));
    if (!f) return ret_x(c, (uint64_t)-1);
    struct sockaddr_in h;
#ifdef _WIN32
    int hl = sizeof h;
#else
    socklen_t hl = sizeof h;
#endif
    int r = peer ? getpeername(f->s, (struct sockaddr *)&h, &hl) : getsockname(f->s, (struct sockaddr *)&h, &hl);
    if (r) return ret_fail(c, host_error());
    to_guest_addr(&h, arg_p(c, 1), arg_p(c, 2));
    ret_x(c, 0);
}
static void ov_sockname(Cpu *c) { name_common(c, 0); }
static void ov_peername(Cpu *c) { name_common(c, 1); }

// ---- eventfd: a counter, plus a loopback datagram per write so Poll wakes on it with the sockets.
#ifdef _WIN32
typedef WSAPOLLFD HostPoll;
#define host_poll(p, n, t) WSAPoll(p, (ULONG)(n), t)
#define HP_IN POLLRDNORM
#define HP_OUT POLLWRNORM
#else
typedef struct pollfd HostPoll;
#define host_poll(p, n, t) poll(p, (nfds_t)(n), t)
#define HP_IN POLLIN
#define HP_OUT POLLOUT
#endif

static PlatMutex ev_lock = PLAT_MUTEX_INIT;

static void drain(HostSock s) {
    char b[16];
    while (recv(s, b, sizeof b, 0) > 0) {
    }
}

// int EventFd(uint64_t initval, int flags)
static void ov_eventfd(Cpu *c) {
    sock_startup();
    if (!sock_on()) return ret_fail(c, G_ENETDOWN);
    int fd = fd_new(FD_EVENT);
    if (fd < 0) return ret_fail(c, G_EMFILE);
    GFd *f = &gfd[fd];
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(0x7f000001);
#ifdef _WIN32
    int al = sizeof a;
#else
    socklen_t al = sizeof a;
#endif
    f->s = socket(AF_INET, SOCK_DGRAM, 0);
    f->w = socket(AF_INET, SOCK_DGRAM, 0);
    if (f->s == HOST_BAD || f->w == HOST_BAD || bind(f->s, (struct sockaddr *)&a, sizeof a) ||
        getsockname(f->s, (struct sockaddr *)&a, &al) || connect(f->w, (struct sockaddr *)&a, sizeof a)) {
        int e = host_error();
        if (f->s != HOST_BAD) host_close(f->s);
        if (f->w != HOST_BAD) host_close(f->w);
        f->kind = FD_FREE;
        return ret_fail(c, e);
    }
    host_nonblock(f->s, 1);
    f->type = 2;
    f->value = arg_x(c, 0);
    // Bit 0 is the semaphore mode. Reads never block: gRPC reads its wakeup eventfd only after Poll said it is
    // readable, and an emulator that runs MKW online (Droplet) keeps every guest eventfd non-blocking the same way.
    f->semaphore = (uint32_t)arg_x(c, 1) & 1;
    f->nonblock = 1;
    if (f->value) send(f->w, "", 1, 0);
    ret_x(c, (uint64_t)fd);
}

// int EventFdWrite(int fd, uint64_t value)
static void ov_eventfd_write(Cpu *c) {
    GFd *f = fd_get((int)arg_x(c, 0), FD_EVENT);
    if (!f) return ret_fail(c, G_EBADF);
    plat_mutex_lock(&ev_lock);
    f->value += arg_x(c, 1);
    send(f->w, "", 1, 0);
    plat_mutex_unlock(&ev_lock);
    ret_x(c, 0);
}

// int EventFdRead(int fd, uint64_t *value)
static void ov_eventfd_read(Cpu *c) {
    GFd *f = fd_get((int)arg_x(c, 0), FD_EVENT);
    uint64_t *out = arg_p(c, 1);
    if (!f) return ret_fail(c, G_EBADF);
    for (;;) {
        plat_mutex_lock(&ev_lock);
        if (f->value) {
            uint64_t v = f->semaphore ? 1 : f->value;
            f->value -= v;
            if (!f->value) drain(f->s);
            plat_mutex_unlock(&ev_lock);
            if (out) *out = v;
            return ret_x(c, 0);
        }
        drain(f->s);
        plat_mutex_unlock(&ev_lock);
        if (f->nonblock) return ret_fail(c, G_EAGAIN);
        HostPoll p;
        memset(&p, 0, sizeof p);
        p.fd = f->s;
        p.events = HP_IN;
        int away = kern_core_away();
        host_poll(&p, 1, 100);
        kern_core_back(&away);
    }
}

// int Poll(pollfd *, nfds_t, int timeout); pollfd {s32 fd, s16 events, s16 revents}; guest bits IN 1, PRI 2, OUT 4,
// ERR 8, HUP 0x10, NVAL 0x20, RDNORM 0x40, RDBAND 0x80, WRBAND 0x100.
static void ov_poll(Cpu *c) {
    uint8_t *pf = arg_p(c, 0);
    uint64_t n = arg_x(c, 1);
    int timeout = (int)arg_x(c, 2);
    if (n > 256 || (n && !pf)) return ret_fail(c, G_EINVAL);
    HostPoll hp[256];
    int map[256], m = 0, ready = 0;
    for (uint64_t i = 0; i < n; i++) {
        int32_t fd = *(int32_t *)(pf + 8 * i);
        uint16_t ev = *(uint16_t *)(pf + 8 * i + 4);
        *(uint16_t *)(pf + 8 * i + 6) = 0;
        if (fd < 0) continue;   // ignored, as POSIX says
        GFd *f = fd_get(fd, 0);
        if (!f) {
            *(uint16_t *)(pf + 8 * i + 6) = 0x20;   // NVAL
            ready++;
            continue;
        }
        memset(&hp[m], 0, sizeof hp[m]);
        hp[m].fd = f->s;
        // WSAPoll refuses POLLPRI and POLLERR in events: only the read and write sides are asked
        hp[m].events = (short)((ev & (0x1 | 0x40 | 0x80) ? HP_IN : 0) | (ev & (0x4 | 0x100) ? HP_OUT : 0));
        map[m++] = (int)i;
    }
    if (ready) timeout = 0;   // an invalid descriptor is already an answer
    int r = 0;
    if (m) {
        AWAY(timeout != 0, r = host_poll(hp, m, timeout));
    } else if (timeout > 0) {
        int away = kern_core_away();
        plat_sleep_ns((uint64_t)timeout * 1000000ull);
        kern_core_back(&away);
    }
    if (r < 0) return ret_fail(c, host_error());
    for (int k = 0; k < m; k++) {
        short re = hp[k].revents;
        if (!re) continue;
        uint16_t ev = *(uint16_t *)(pf + 8 * map[k] + 4), g = 0;
        if (re & (POLLRDNORM | POLLRDBAND | POLLIN)) g |= (ev & 0xc1) ? (uint16_t)(ev & 0xc1) : 0x1;
        if (re & (POLLWRNORM | POLLOUT)) g |= (ev & 0x104) ? (uint16_t)(ev & 0x104) : 0x4;
        if (re & POLLERR) g |= 0x8;
        if (re & POLLHUP) g |= 0x10;
        if (re & POLLNVAL) g |= 0x20;
        *(uint16_t *)(pf + 8 * map[k] + 6) = g;
        if (g) ready++;
    }
    ret_x(c, (uint64_t)ready);
}

// ---- name resolution: only Nintendo's names (to the server), numeric addresses and localhost resolve, so nothing
// of the game reaches Nintendo or another public server. addrinfo (64-bit): flags @0, family @4, socktype @8,
// protocol @12, addrlen @16, then two pointers @24 and @32 (canonname and addr, whose order differs between BSD and
// Linux: both hold the address, and a canonical name is never asked for), next @40.
#define AI_SLOTS 64
#define AI_SLOT 80
static uint8_t *ai_pool;
static uint8_t ai_used[AI_SLOTS];
static PlatMutex ai_lock = PLAT_MUTEX_INIT;

enum { EAI_G_FAIL = 4, EAI_G_FAMILY = 5, EAI_G_MEMORY = 6, EAI_G_NONAME = 8 };

// int GetAddrInfo(const char *node, const char *service, const addrinfo *hints, addrinfo **res[, options...])
static void ov_resolve(Cpu *c) {
    const char *node = arg_p(c, 0), *service = arg_p(c, 1);
    const uint8_t *hints = arg_p(c, 2);
    uint64_t *res = arg_p(c, 3);
    if (res) *res = 0;
    if (!net_enabled() || !res) return ret_x(c, EAI_G_FAIL);
    int flags = hints ? *(const int32_t *)hints : 0, family = hints ? *(const int32_t *)(hints + 4) : 0;
    int socktype = hints ? *(const int32_t *)(hints + 8) : 0, proto = hints ? *(const int32_t *)(hints + 12) : 0;
    if (family != 0 && family != 2) return ret_x(c, EAI_G_FAMILY);
    struct in_addr ip;
    if (!node) ip.s_addr = flags & 1 ? 0 : htonl(0x7f000001);   // AI_PASSIVE: any address
    else if (inet_pton(AF_INET, node, &ip) == 1) {
    } else if (net_redirected_host(node)) memcpy(&ip, net_server()->ip, 4);
    else if (!strcmp(node, "localhost")) ip.s_addr = htonl(0x7f000001);
    else {
        rt_log("net: name %s not resolved (only the online server's names are)", node);
        return ret_x(c, EAI_G_NONAME);
    }
    unsigned port = service ? (unsigned)strtoul(service, NULL, 10) : 0;
    plat_mutex_lock(&ai_lock);
    if (!ai_pool) ai_pool = mem_kernel_alloc(AI_SLOTS * AI_SLOT, 16);
    int k = -1;
    for (int i = 0; i < AI_SLOTS && k < 0; i++)
        if (!ai_used[i]) ai_used[i] = 1, k = i;
    plat_mutex_unlock(&ai_lock);
    if (k < 0) return ret_x(c, EAI_G_MEMORY);
    uint8_t *ai = ai_pool + k * AI_SLOT, *sa = ai + 48;
    memset(ai, 0, AI_SLOT);
    *(int32_t *)(ai + 0) = flags;
    *(int32_t *)(ai + 4) = 2;
    *(int32_t *)(ai + 8) = socktype ? socktype : 1;
    *(int32_t *)(ai + 12) = proto;
    *(uint32_t *)(ai + 16) = 16;
    *(uint64_t *)(ai + 24) = (uint64_t)(uintptr_t)sa;
    *(uint64_t *)(ai + 32) = (uint64_t)(uintptr_t)sa;
    sa[0] = 16, sa[1] = 2;
    uint16_t nport = htons((uint16_t)port);
    memcpy(sa + 2, &nport, 2);
    memcpy(sa + 4, &ip, 4);
    *res = (uint64_t)(uintptr_t)ai;
    if (node) {
        const uint8_t *b = (const uint8_t *)&ip;
        rt_log("net: %s -> %u.%u.%u.%u", node, b[0], b[1], b[2], b[3]);
    }
    ret_x(c, 0);
}

static void ov_freeaddrinfo(Cpu *c) {
    uint8_t *ai = arg_p(c, 0);
    plat_mutex_lock(&ai_lock);
    while (ai && ai_pool && ai >= ai_pool && ai < ai_pool + AI_SLOTS * AI_SLOT) {
        ai_used[(ai - ai_pool) / AI_SLOT] = 0;
        ai = (uint8_t *)(uintptr_t)*(uint64_t *)(ai + 40);
    }
    plat_mutex_unlock(&ai_lock);
}

static void ov_gai_strerror(Cpu *c) {
    static const char *const text[] = {"success", "address family not supported", "temporary failure",
                                       "invalid flags", "non-recoverable failure", "family not supported",
                                       "memory allocation failure", "no address", "name not known",
                                       "service not supported", "socket type not supported", "system error"};
    int64_t e = (int32_t)arg_x(c, 0);
    ret_x(c, (uint64_t)(uintptr_t)(e >= 0 && e < 12 ? text[e] : "unknown error"));
}

// Off (no MKW_ONLINE, no MKW_LAN): what main's stub answered, the console's BSD ENETDOWN (50), as upstream's
// ov_socket.c did; on, the Linux numbers the game compares with (11 for EAGAIN)
static void ov_sock_errno(Cpu *c) { ret_x(c, sock_on() ? (uint64_t)(int64_t)last_errno : 50); }
static void ov_sock_set_errno(Cpu *c) { last_errno = (int)arg_x(c, 0); }
static void ov_sock_initialized(Cpu *c) { ret_x(c, 1); }
static void ov_sock_init(Cpu *c) {
    sock_startup();
    rt_log("net: sockets %s", net_enabled() ? "on (host sockets)"
                              : ov_lan() ? "on for LAN play (local network addresses only)"
                                         : "off (a console with no network)");
    ret_x(c, 0);
}

// The C++ interface (nn::socket::*) and the C one (nnsocket*) take the same arguments. The byte-order and
// address-text helpers are the SDK's own code and need no service.
HOST_OVERRIDE("_ZN2nn6socket8internal10InitializeEi", ov_sock_init);
HOST_OVERRIDE("_ZN2nn6socket12SetAllocatorEPNS_14MemoryResourceE", ov_nothing);
HOST_OVERRIDE("_ZN2nn6socket8FinalizeEv", ov_ok);
HOST_OVERRIDE("_ZN2nn6socket13IsInitializedEv", ov_sock_initialized);
HOST_OVERRIDE("_ZN2nn6socket12GetLastErrnoEv", ov_sock_errno);
HOST_OVERRIDE("_ZN2nn6socket12GetLastErrorEv", ov_sock_errno);
HOST_OVERRIDE("_ZN2nn6socket6SocketEiii", ov_socket);
HOST_OVERRIDE("_ZN2nn6socket6SocketENS0_6FamilyENS0_4TypeENS0_8ProtocolE", ov_socket);
HOST_OVERRIDE("_ZN2nn6socket7EventFdEmNS0_12EventFdFlagsE", ov_eventfd);
HOST_OVERRIDE("_ZN2nn6socket11EventFdReadEiPm", ov_eventfd_read);
HOST_OVERRIDE("_ZN2nn6socket12EventFdWriteEim", ov_eventfd_write);
HOST_OVERRIDE("_ZN2nn6socket4BindEiPKNS0_8SockAddrEj", ov_bind);
HOST_OVERRIDE("_ZN2nn6socket4BindEiPK8sockaddrj", ov_bind);
HOST_OVERRIDE("_ZN2nn6socket10SetSockOptEiNS0_5LevelENS0_6OptionEPKvj", ov_setsockopt);
HOST_OVERRIDE("_ZN2nn6socket6SendToEiPKvmiPK8sockaddrj", ov_sendto);
HOST_OVERRIDE("_ZN2nn6socket6SendToEiPKvmNS0_7MsgFlagEPKNS0_8SockAddrEj", ov_sendto);
HOST_OVERRIDE("_ZN2nn6socket8RecvFromEiPvmiP8sockaddrPj", ov_recvfrom);
HOST_OVERRIDE("_ZN2nn6socket8RecvFromEiPvmNS0_7MsgFlagEPNS0_8SockAddrEPj", ov_recvfrom);
HOST_OVERRIDE("_ZN2nn6socket4PollEP6pollfdmi", ov_poll);
HOST_OVERRIDE("_ZN2nn6socket8ShutdownEii", ov_shutdown);
HOST_OVERRIDE("_ZN2nn6socket5CloseEi", ov_close);
HOST_OVERRIDE("_ZN2nn6socket11GetAddrInfoEPKcS2_PK8addrinfoPPS3_", ov_resolve);
HOST_OVERRIDE("_ZN2nn6socket11GetAddrInfoEPKcS2_PKNS0_8AddrInfoEPPS3_", ov_resolve);
HOST_OVERRIDE("_ZN2nn6socket12FreeAddrInfoEP8addrinfo", ov_freeaddrinfo);
HOST_OVERRIDE("_ZN2nn6socket12FreeAddrInfoEPNS0_8AddrInfoE", ov_freeaddrinfo);
HOST_OVERRIDE("_ZN2nn6socket11GAIStrErrorENS0_7AiErrnoE", ov_gai_strerror);

HOST_OVERRIDE("nnsocketSocket", ov_socket);
HOST_OVERRIDE("nnsocketClose", ov_close);
HOST_OVERRIDE("nnsocketBind", ov_bind);
HOST_OVERRIDE("nnsocketConnect", ov_connect);
HOST_OVERRIDE("nnsocketListen", ov_listen);
HOST_OVERRIDE("nnsocketAccept", ov_accept);
HOST_OVERRIDE("nnsocketSend", ov_send);
HOST_OVERRIDE("nnsocketRecv", ov_recv);
HOST_OVERRIDE("nnsocketSendMsg", ov_sendmsg);
HOST_OVERRIDE("nnsocketRecvMsg", ov_recvmsg);
HOST_OVERRIDE("nnsocketShutdown", ov_shutdown);
HOST_OVERRIDE("nnsocketFcntl", ov_fcntl);
HOST_OVERRIDE("nnsocketPoll", ov_poll);
HOST_OVERRIDE("nnsocketSetSockOpt", ov_setsockopt);
HOST_OVERRIDE("nnsocketGetSockOpt", ov_getsockopt);
HOST_OVERRIDE("nnsocketGetSockName", ov_sockname);
HOST_OVERRIDE("nnsocketGetPeerName", ov_peername);
HOST_OVERRIDE("nnsocketGetAddrInfo", ov_resolve);
HOST_OVERRIDE("nnsocketGetAddrInfoWithOptions", ov_resolve);
HOST_OVERRIDE("nnsocketFreeAddrInfo", ov_freeaddrinfo);
HOST_OVERRIDE("nnsocketGAIStrError", ov_gai_strerror);
HOST_OVERRIDE("nnsocketGetLastErrno", ov_sock_errno);
HOST_OVERRIDE("nnsocketSetLastErrno", ov_sock_set_errno);
HOST_OVERRIDE("nnsocketResolverGetOption", ov_ok);
HOST_OVERRIDE("nnsocketResolverSetOption", ov_ok);

// ==== nn::ssl: MKW does its own TLS (gRPC) and takes only the console's built-in CA certificates from the ssl
// service. With a server set, its certificate is that built-in list (filed as the first id the game asks for), so
// the game trusts the private server and nothing else.
// BuiltInCertificateInfo (24 bytes): {u32 id, u32 status, u64 size, u64 data}; the SDK turns data's offset into a
// pointer (sdk+0x51df90) and ends the list with {All, Invalid}. EnabledTrusted is 1.
#define SSL_R_NO_CA (123u | (205u << 9))   // module 123 (ssl); description is a hypothesis
static uint8_t *ca_der;
static size_t ca_len;
static PlatMutex ca_lock = PLAT_MUTEX_INIT;

static int server_ca(void) {
    const NetServer *s = net_server();
    if (!s->on) return 0;
    plat_mutex_lock(&ca_lock);
    if (!ca_der) {
        ca_der = net_server_ca(s->text, &ca_len);
        if (!ca_der) {
            char err[200];
            const char *e = net_fetch_ca(s->text, err, sizeof err);
            if (!*e) ca_der = net_server_ca(s->text, &ca_len);
            else rt_log("ssl: no certificate for %s: %s (the game shows 2321-4992)", s->text, e);
        }
        if (ca_der) rt_log("ssl: the server's certificate (%zu bytes) is the built-in CA", ca_len);
    }
    plat_mutex_unlock(&ca_lock);
    return ca_der != NULL;
}

static uint32_t ca_id(const uint32_t *ids, uint32_t n) {
    for (uint32_t i = 0; ids && i < n; i++)
        if (ids[i] == 0xffffffffu || ids[i] == 1) return 1;   // All or NintendoCAG3
    return ids && n ? ids[0] : 1;
}

// Result GetBuiltInCertificateBufSize(u32 *size, CaCertificateId *ids, u32 count)
static void ov_ca_size(Cpu *c) {
    uint32_t *out = arg_p(c, 0);
    if (!out) return ret_x(c, SSL_R_NO_CA);
    *out = 24 + (server_ca() ? 24 + (uint32_t)((ca_len + 3) & ~(size_t)3) : 0);
    ret_x(c, 0);
}

// Result GetBuiltInCertificates(BuiltInCertificateInfo **out, u8 *buf, u32 size, CaCertificateId *ids, u32 count)
static void ov_ca_get(Cpu *c) {
    uint64_t *out = arg_p(c, 0);
    uint8_t *buf = arg_p(c, 1);
    uint32_t size = (uint32_t)arg_x(c, 2), n = (uint32_t)arg_x(c, 4);
    const uint32_t *ids = arg_p(c, 3);
    int have = server_ca();
    uint32_t need = 24 + (have ? 24 + (uint32_t)ca_len : 0);
    if (!out || !buf || size < need) return ret_x(c, SSL_R_NO_CA);
    uint8_t *e = buf;
    if (have) {
        *(uint32_t *)e = ca_id(ids, n);
        *(uint32_t *)(e + 4) = 1;
        *(uint64_t *)(e + 8) = ca_len;
        *(uint64_t *)(e + 16) = (uint64_t)(uintptr_t)(buf + 48);
        memcpy(buf + 48, ca_der, ca_len);
        e += 24;
    }
    *(uint32_t *)e = 0xffffffffu;         // All
    *(uint32_t *)(e + 4) = 0xffffffffu;   // Invalid
    *(uint64_t *)(e + 8) = 0;
    *(uint64_t *)(e + 16) = 0;
    *out = (uint64_t)(uintptr_t)buf;
    ret_x(c, 0);
}

// Context::Create(SslVersion) and the Import* members: the game makes a context and imports into it but opens no ssl
// connection (it imports none of Connection's functions); ids only.
static uint64_t ssl_next_id = 1;
static void ov_ssl_context(Cpu *c) {
    uint64_t *self = arg_p(c, 0);
    if (self) *self = __atomic_fetch_add(&ssl_next_id, 1, __ATOMIC_RELAXED);
    ret_x(c, 0);
}
static void ov_ssl_import(Cpu *c) {
    uint64_t *id = arg_p(c, 1);
    if (id) *id = __atomic_fetch_add(&ssl_next_id, 1, __ATOMIC_RELAXED);
    ret_x(c, 0);
}

HOST_OVERRIDE("_ZN2nn3ssl10InitializeEv", ov_ok);
HOST_OVERRIDE("_ZN2nn3ssl8FinalizeEv", ov_ok);
HOST_OVERRIDE("_ZN2nn3ssl7Context6CreateENS1_10SslVersionE", ov_ssl_context);
HOST_OVERRIDE("_ZN2nn3ssl7Context15ImportServerPkiEPmPKcjNS0_17CertificateFormatE", ov_ssl_import);
HOST_OVERRIDE("_ZN2nn3ssl7Context15ImportClientPkiEPmPKcS4_jj", ov_ssl_import);
HOST_OVERRIDE("_ZN2nn3ssl7Context9ImportCrlEPmPKcj", ov_ssl_import);
HOST_OVERRIDE("_ZN2nn3ssl14BuiltInManager28GetBuiltInCertificateBufSizeEPjPNS0_15CaCertificateIdEj", ov_ca_size);
HOST_OVERRIDE("_ZN2nn3ssl14BuiltInManager22GetBuiltInCertificatesEPPNS0_26BuiltInCertificateInfoBaseEPhjPNS0_15CaCertificateIdEj",
              ov_ca_get);

// ==== the network service account (nn::account): available with a server set. Its id is new at each start, so two
// programs on one PC (Droplet and the port) are two players; the ID token is the server's identity token.
static int online(void) { return net_server()->on; }

static void ov_nsa_available(Cpu *c) {
    uint8_t *out = arg_p(c, 0);
    if (out) *out = (uint8_t)online();
    ret_x(c, 0);
}
static void ov_nsa_ensure(Cpu *c) { ret_x(c, online() ? 0 : ACCOUNT_R_NSA_UNAVAILABLE); }

static void ov_nsa_id(Cpu *c) {
    static uint64_t id;
    uint64_t *out = arg_p(c, 0);
    if (!online() || !out) return ret_x(c, ACCOUNT_R_NSA_UNAVAILABLE);
    if (!id) {
        uint64_t v = 0;
        for (int i = 0; i < 8 && !v; i++) plat_random(&v, sizeof v);
        id = v ? v : 0x6d6b7700000001ull;
        rt_log("account: network service account id %016llx for this start", (unsigned long long)id);
    }
    *out = id;
    ret_x(c, 0);
}

// The main module's build id (the game's exact version), from its NSO header: the server compares it.
static const char *build_id(void) {
    static char hex[65];
    if (hex[0]) return hex;
    char path[1200];
    snprintf(path, sizeof path, "%s/main", rt_config()->exefs);
    uint64_t size = 0;
    uint8_t *nso = plat_file_read(path, &size);
    if (nso && size >= 0x60) {
        int n = 32;
        while (n > 0 && !nso[0x40 + n - 1]) n--;
        for (int i = 0; i < n; i++) snprintf(hex + 2 * i, 3, "%02x", nso[0x40 + i]);
    }
    free(nso);
    return hex;
}

// Result LoadNetworkServiceAccountIdTokenCache(size_t *size, char *buf, size_t cap, const UserHandle &)
static void ov_nsa_token(Cpu *c) {
    uint64_t *size = arg_p(c, 0);
    char *buf = arg_p(c, 1);
    uint64_t cap = arg_x(c, 2);
    if (!online() || !buf || !net_identity_token(build_id(), buf, (size_t)cap))
        return ret_x(c, ACCOUNT_R_NSA_UNAVAILABLE);
    if (size) *size = strlen(buf);
    rt_log("account: ID token for %s (this player's friend code)", net_server()->text);
    ret_x(c, 0);
}

// The token is ready at once: the AsyncContext the game passes keeps no session (its SDK destructor then skips it,
// sdk+0x18197c), and its queries answer done and success here. Its system event is made signalled.
static void ov_async_has_done(Cpu *c) {
    uint8_t *out = arg_p(c, 1);
    if (out) *out = 1;
    ret_x(c, 0);
}
static void ov_async_event(Cpu *c) {
    static uint64_t create, signal;
    uint64_t ev = arg_x(c, 1);
    if (!create) {
        create = ldr_symbol("_ZN2nn2os17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb");
        signal = ldr_symbol("_ZN2nn2os17SignalSystemEventEPNS0_15SystemEventTypeE");
    }
    if (!ev || !create || !signal) return ret_x(c, ACCOUNT_R_NSA_UNAVAILABLE);
    if ((uint32_t)rt_call(c, create, 3, ev, (uint64_t)0, (uint64_t)0)) return ret_x(c, ACCOUNT_R_NSA_UNAVAILABLE);
    rt_call(c, signal, 1, ev);
    ret_x(c, 0);
}

HOST_OVERRIDE("_ZN2nn7account32IsNetworkServiceAccountAvailableEPbRKNS0_10UserHandleE", ov_nsa_available);
HOST_OVERRIDE("_ZN2nn7account36EnsureNetworkServiceAccountAvailableERKNS0_10UserHandleE", ov_nsa_ensure);
HOST_OVERRIDE("_ZN2nn7account26GetNetworkServiceAccountIdEPNS0_23NetworkServiceAccountIdERKNS0_10UserHandleE", ov_nsa_id);
HOST_OVERRIDE("_ZN2nn7account37LoadNetworkServiceAccountIdTokenCacheEPmPcmRKNS0_10UserHandleE", ov_nsa_token);
HOST_OVERRIDE("_ZN2nn7account44EnsureNetworkServiceAccountIdTokenCacheAsyncEPNS0_12AsyncContextERKNS0_10UserHandleE",
              ov_nsa_ensure);
HOST_OVERRIDE("_ZN2nn7account12AsyncContext7HasDoneEPb", ov_async_has_done);
HOST_OVERRIDE("_ZN2nn7account12AsyncContext9GetResultEv", ov_ok);
HOST_OVERRIDE("_ZN2nn7account12AsyncContext6CancelEv", ov_ok);
HOST_OVERRIDE("_ZN2nn7account12AsyncContext14GetSystemEventEPNS_2os11SystemEventE", ov_async_event);
