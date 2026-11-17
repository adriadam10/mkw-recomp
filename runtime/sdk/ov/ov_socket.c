// nn::socket for MKW: the console's BSD sockets over the host's, so the game's own network library (Pia, linked in
// main: nn::pia::lan for LAN play, nn::pia::local over ldn) can reach other machines. main imports only datagram calls
// (Socket, Bind, SendTo, RecvFrom, Poll, SetSockOpt, Shutdown, Close, EventFd*), no Connect/Listen/Accept.
//
// What differs between the console and the host, and what each side uses:
// - sockaddr: the console's is FreeBSD's (sin_len, then a one-byte family); the host's has a two-byte family.
// - Option levels and names, message flags and the type's creation flags are FreeBSD's (SOL_SOCKET 0xffff, SO_BROADCAST
//   0x20, MSG_DONTWAIT 0x80, SOCK_NONBLOCK as 2 << 28). Values from Ryujinx (MIT) @ 4d5a19d,
//   src/Ryujinx.HLE/HOS/Services/Sockets/Bsd/Types: BsdSocketOption.cs, BsdSocketFlags.cs, BsdSocketCreationFlags.cs,
//   EventFdFlags.cs, BsdSockAddr.cs, PollEventTypeMask.cs.
// - errno is Linux's: the game compares GetLastErrno with 11 for EAGAIN (main+0x14c562c) and Ryujinx answers with its
//   LinuxError; on Linux the host's errno goes through as is, on Windows WSA errors are mapped to it.
// - poll events are the same bits on both (POLLIN 1, POLLOUT 4, POLLERR 8, POLLHUP 0x10, POLLNVAL 0x20).
//
// Guest descriptors are indexes into a table of host sockets: Windows' SOCKET does not fit an int, and the game gets the
// small numbers a console gives. A call that can wait (Poll with a timeout, RecvFrom on a blocking socket) gives its
// virtual core up meanwhile (kern_core_away), as audio and NVN waits do, so a waiting network thread holds no core.
//
// MKW_LAN on, LAN only: Bind and SendTo refuse any address outside a local network (lan_only), and GetAddrInfo resolves
// nothing, so no datagram can go to a server on the internet.
// MKW_LAN_PEERS: the PCs of a virtual LAN that carries no broadcast (Tailscale, ZeroTier, a VPN), as IPv4 addresses or
// names, separated by commas or spaces. A broadcast of the lobby also goes to each of them (ov_lan_peers), and the LAN
// address is the one of the route to the first (its 100.x Tailscale address, ov_lanaddr.c), so the others can answer.
// MKW_LAN_IP in 127.0.0.0/8: sockets bound to any address bind to it instead, so two instances on one PC (127.0.0.1
// and 127.0.0.2, each the other's MKW_LAN_PEERS) each get their own port 49152 (tests).
// MKW_LAN off (the default) keeps main's stub, a console whose network is down: every socket call -1, errno ENETDOWN,
// IsInitialized 1, no WSAStartup, so the online menu takes its no-connection path as before (issue #63).
//
// eventfd exists only on Linux; Windows answers ENOSYS. In main only gRPC's wakeup_fd_eventfd.cc (main+0x10208c0, the
// online services) calls EventFd*, after its availability check (main+0x1020a30); Pia never does.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <iphlpapi.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
typedef SOCKET HostSock;
#define HOST_BAD INVALID_SOCKET
#define host_close closesocket
#define host_poll WSAPoll
#else
#include <errno.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int HostSock;
#define HOST_BAD (-1)
#define host_close close
#define host_poll poll
#endif

#include "cfg.h"
#include "host.h"
#include "ov.h"
#include "platform/platform.h"

// Linux errno values the game reads (its numbering on every host)
enum { G_EINTR = 4, G_EIO = 5, G_EBADF = 9, G_EAGAIN = 11, G_EACCES = 13, G_EINVAL = 22, G_EMFILE = 24,
       G_ENOSYS = 38, G_ENOTSOCK = 88, G_EMSGSIZE = 90, G_ENOPROTOOPT = 92, G_EAFNOSUPPORT = 97,
       G_EADDRINUSE = 98, G_EADDRNOTAVAIL = 99, G_ENETUNREACH = 101, G_ECONNRESET = 104, G_ENOBUFS = 105,
       G_ETIMEDOUT = 110, G_EHOSTUNREACH = 113 };

// FreeBSD values of the console's side
#define B_SOL_SOCKET 0xffff
#define B_AF_INET 2
#define B_SOCK_NONBLOCK (2u << 28)
#define B_SOCK_FLAGS (0xfu << 28)
#define B_MSG_OOB 0x1
#define B_MSG_PEEK 0x2
#define B_MSG_DONTROUTE 0x4
#define B_MSG_WAITALL 0x40
#define B_MSG_DONTWAIT 0x80
#define B_EFD_SEMAPHORE 1
#define B_EFD_NONBLOCK 4
// poll events: the console's and Linux's bits; WSAPoll's differ (POLLIN is 0x300 there)
enum { G_POLLIN = 1, G_POLLPRI = 2, G_POLLOUT = 4, G_POLLERR = 8, G_POLLHUP = 0x10, G_POLLNVAL = 0x20 };
#ifdef _WIN32
static short ev_host(int g) {
    return (short)((g & G_POLLIN ? POLLRDNORM : 0) | (g & G_POLLPRI ? POLLRDBAND : 0) | (g & G_POLLOUT ? POLLWRNORM : 0));
}
static int ev_guest(int h) {
    return (h & (POLLRDNORM | POLLRDBAND) ? G_POLLIN : 0) | (h & POLLPRI ? G_POLLPRI : 0) |
           (h & POLLWRNORM ? G_POLLOUT : 0) | (h & POLLERR ? G_POLLERR : 0) | (h & POLLHUP ? G_POLLHUP : 0) |
           (h & POLLNVAL ? G_POLLNVAL : 0);
}
#else
static short ev_host(int g) { return (short)(g & 0x3f); }
static int ev_guest(int h) { return h & 0x3f; }
#endif

#define NSOCK 64   // ponytail: fixed table; Pia opens a handful, raise it if MKW_LOG shows "no free socket"

typedef struct {
    int used, nonblock, eventfd;
    HostSock s;
} Slot;
// The table is read and written under one lock: a Poll on one thread while another opens or closes a slot sees either
// the old socket or the new one, whole. Calls use a copy of the slot, so no call holds the lock while it waits.
static Slot slots[NSOCK];
static PlatMutex slots_lock = PLAT_MUTEX_INIT;
static int ready;
static _Thread_local int last_errno;   // per host thread: each guest thread is one (kernel.c)

static void fail(int e) { last_errno = e; }

#define B_ENETDOWN 50   // what main's stub answered: the console's BSD errno for a network that is down
int ov_lan(void) { return cfg_bool("MKW_LAN"); }
// MKW_LAN off: the call fails as main's stub did
#define LAN_OR_DOWN(c)                                                                                                \
    do {                                                                                                               \
        if (!ov_lan()) {                                                                                               \
            ret_x(c, (uint64_t)-1);                                                                                    \
            return;                                                                                                    \
        }                                                                                                              \
    } while (0)

#ifdef _WIN32
static int host_errno(void) {
    switch (WSAGetLastError()) {
    case WSAEWOULDBLOCK: return G_EAGAIN;
    case WSAEINTR: return G_EINTR;
    case WSAEBADF: case WSAENOTSOCK: return G_ENOTSOCK;
    case WSAEACCES: return G_EACCES;
    case WSAEINVAL: return G_EINVAL;
    case WSAEMFILE: return G_EMFILE;
    case WSAEMSGSIZE: return G_EMSGSIZE;
    case WSAENOPROTOOPT: return G_ENOPROTOOPT;
    case WSAEAFNOSUPPORT: return G_EAFNOSUPPORT;
    case WSAEADDRINUSE: return G_EADDRINUSE;
    case WSAEADDRNOTAVAIL: return G_EADDRNOTAVAIL;
    case WSAENETUNREACH: return G_ENETUNREACH;
    case WSAECONNRESET: return G_ECONNRESET;
    case WSAENOBUFS: return G_ENOBUFS;
    case WSAETIMEDOUT: return G_ETIMEDOUT;
    case WSAEHOSTUNREACH: return G_EHOSTUNREACH;
    default: return G_EIO;
    }
}
#else
static int host_errno(void) { return errno; }
#endif

// A copy of slot fd into *out; 0, or -1 with EBADF when fd is not open
static int slot_get(int fd, Slot *out) {
    int ok = 0;
    plat_mutex_lock(&slots_lock);
    if (fd >= 0 && fd < NSOCK && slots[fd].used) *out = slots[fd], ok = 1;
    plat_mutex_unlock(&slots_lock);
    return ok ? 0 : (fail(G_EBADF), -1);
}

static int slot_new(HostSock s, int eventfd, int nonblock) {
    int fd = -1;
    plat_mutex_lock(&slots_lock);
    for (int i = 0; i < NSOCK && fd < 0; i++)
        if (!slots[i].used) slots[i] = (Slot){.used = 1, .nonblock = nonblock, .eventfd = eventfd, .s = s}, fd = i;
    plat_mutex_unlock(&slots_lock);
    if (fd < 0) rt_log("socket: no free socket (%d in use)", NSOCK);
    return fd;
}

static int set_nonblock(HostSock s) {
#ifdef _WIN32
    u_long on = 1;
    return ioctlsocket(s, FIONBIO, &on);
#else
    return fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);
#endif
}

// FreeBSD sockaddr_in (len, family, port, addr, zero[8]) <-> the host's; only IPv4 (all main asks for)
static int addr_in(const uint8_t *b, uint32_t len, struct sockaddr_in *h) {
    if (!b || len < 8 || b[1] != B_AF_INET) return fail(G_EAFNOSUPPORT), -1;
    memset(h, 0, sizeof *h);
    h->sin_family = AF_INET;
    memcpy(&h->sin_port, b + 2, 2);
    memcpy(&h->sin_addr, b + 4, 4);
    return 0;
}

static void addr_out(const struct sockaddr_in *h, uint8_t *b, uint32_t *len) {
    if (!b || !len) return;
    uint8_t g[16] = {16, B_AF_INET};
    memcpy(g + 2, &h->sin_port, 2);
    memcpy(g + 4, &h->sin_addr, 4);
    memcpy(b, g, *len < 16 ? *len : 16);
    *len = 16;
}

// LAN only: Bind and SendTo take only addresses that never leave a local network, so LAN play cannot reach any server
// on the internet (Nintendo's or another), whatever the game asks: any (bind), loopback, the private ranges of RFC 1918,
// shared address space (RFC 6598, the virtual LANs of Tailscale and the like), link-local, multicast and broadcast. The
// game's online services stay off in any case (GetAddrInfo resolves nothing, nifm reports no internet).
int ov_lan_only(uint32_t h) {   // h: the IPv4 address in host order
    return h == 0 || (h >> 24) == 127 || (h >> 24) == 10 || (h >> 20) == 0xac1 || (h >> 16) == 0xc0a8 ||
           (h >> 22) == (100u << 2 | 1) || (h >> 16) == 0xa9fe || (h >> 28) == 0xe || h == 0xffffffffu;
}
static int lan_only(struct in_addr a) { return ov_lan_only(ntohl(a.s_addr)); }
int ov_lan_local(uint32_t addr) { return ov_lan_only(ntohl(addr)); }

// 0, or -1 with ENETUNREACH (logged, a few times) for an address outside a local network
static int lan_check(const struct sockaddr_in *a, const char *call) {
    if (lan_only(a->sin_addr)) return 0;
    static volatile int32_t n;
    if (plat_inc32(&n) <= 8) {
        char t[INET_ADDRSTRLEN] = "?";
        inet_ntop(AF_INET, &a->sin_addr, t, sizeof t);
        rt_log("socket: %s to %s refused: LAN play reaches only local network addresses", call, t);
    }
    return fail(G_ENETUNREACH), -1;
}

static int net_startup(void) {
#ifdef _WIN32
    static int started;   // apart from ready (the game's Initialize): nifm reads the peers too (ov_lanaddr.c)
    WSADATA wsa;
    if (!started && WSAStartup(MAKEWORD(2, 2), &wsa)) return -1;
    started = 1;
#endif
    return 0;
}

// ---- MKW_LAN_PEERS, parsed again when the setting changes (names resolved then, IPv4 only)
static PlatMutex peers_lock = PLAT_MUTEX_INIT;
static char peers_text[512];
static uint32_t peers[OV_LAN_PEERS_MAX];
static int npeers = -1;   // -1: not parsed yet

static int peers_get(uint32_t *out, int max) {
    const char *e = cfg_get("MKW_LAN_PEERS");
    if (!e) e = "";
    plat_mutex_lock(&peers_lock);
    if (npeers < 0 || strncmp(e, peers_text, sizeof peers_text)) {
        snprintf(peers_text, sizeof peers_text, "%s", e);
        npeers = 0;
        char buf[512], *save = NULL;
        snprintf(buf, sizeof buf, "%s", e);
        for (char *t = strtok_r(buf, ", ;\t", &save); t && npeers < OV_LAN_PEERS_MAX; t = strtok_r(NULL, ", ;\t", &save)) {
            struct in_addr a = {0};
            if (inet_pton(AF_INET, t, &a) != 1 && !net_startup()) {
                struct addrinfo hint = {.ai_family = AF_INET, .ai_socktype = SOCK_DGRAM}, *r = NULL;
                if (!getaddrinfo(t, NULL, &hint, &r) && r) a = ((struct sockaddr_in *)r->ai_addr)->sin_addr;
                if (r) freeaddrinfo(r);
            }
            char ip[INET_ADDRSTRLEN] = "?";
            inet_ntop(AF_INET, &a, ip, sizeof ip);
            if (!a.s_addr || !ov_lan_local(a.s_addr)) {
                rt_log("lan: MKW_LAN_PEERS: %s skipped (%s)", t, a.s_addr ? "not a local network address" : "unknown");
                continue;
            }
            peers[npeers++] = a.s_addr;
            rt_log("lan: peer %s (%s): the lobby's broadcasts go to it too", t, ip);
        }
    }
    int n = npeers < max ? npeers : max;
    memcpy(out, peers, (size_t)n * sizeof *out);
    plat_mutex_unlock(&peers_lock);
    return n;
}

static volatile uint32_t lan_bcast;   // the broadcast of the last address ov_lan_address gave, network order

void ov_lan_set_broadcast(uint32_t addr) { lan_bcast = addr; }

int ov_lan_peer_first(uint32_t *addr) { return peers_get(addr, 1) == 1; }

int ov_lan_peers(uint32_t addr, uint32_t *out, int max) {
    if (addr != 0xffffffffu && addr != lan_bcast) return 0;
    return peers_get(out, max);
}

uint32_t ov_lan_bind_address(void) {
    const char *e = cfg_get("MKW_LAN_IP");
    struct in_addr a = {0};
    if (e && *e && inet_pton(AF_INET, e, &a) == 1 && (ntohl(a.s_addr) >> 24) == 127) return a.s_addr;
    return 0;
}

static int msg_flags(int f, int *dontwait) {
    int h = 0;
    if (f & B_MSG_OOB) h |= MSG_OOB;
    if (f & B_MSG_PEEK) h |= MSG_PEEK;
    if (f & B_MSG_DONTROUTE) h |= MSG_DONTROUTE;
    if (f & B_MSG_WAITALL) h |= MSG_WAITALL;
    *dontwait = (f & B_MSG_DONTWAIT) != 0;   // Windows has no MSG_DONTWAIT: done with a poll below on every host
    return h;
}

// ---- lifetime
// Result internal::Initialize(int concurrency)
static void ov_init(Cpu *c) {
    if (!ov_lan()) {   // main's stub: "succeeds" (MKW ignores the result, main+0x2247900)
        ret_x(c, 0);
        return;
    }
#ifdef _WIN32
    WSADATA wsa;
    if (!ready && WSAStartup(MAKEWORD(2, 2), &wsa)) {
        ret_x(c, 1);
        return;
    }
#endif
    ready = 1;
    ret_x(c, 0);
}
static void ov_finalize(Cpu *c) { ret_x(c, 0); }   // sockets stay open until Close, as the game closes them first
static void ov_is_initialized(Cpu *c) { ret_x(c, ov_lan() ? ready : 1); }
static void ov_nothing(Cpu *c) { (void)c; }
static void ov_last_errno(Cpu *c) { ret_x(c, ov_lan() ? (uint32_t)last_errno : B_ENETDOWN); }

// ---- sockets
// int Socket(int family, int type, int protocol)
static void ov_socket(Cpu *c) {
    int family = (int)arg_x(c, 0), protocol = (int)arg_x(c, 2);
    uint32_t type = (uint32_t)arg_x(c, 1);
    LAN_OR_DOWN(c);
    if (family != B_AF_INET) {
        fail(G_EAFNOSUPPORT);
        ret_x(c, (uint64_t)-1);
        return;
    }
    HostSock s = socket(AF_INET, (int)(type & ~B_SOCK_FLAGS), protocol);
    if (s == HOST_BAD) {
        fail(host_errno());
        ret_x(c, (uint64_t)-1);
        return;
    }
#ifdef _WIN32
    // Windows hands a datagram socket's ICMP port unreachable to its next recvfrom as WSAECONNRESET even when it is not
    // connected; Linux reports those only on connected sockets, and Pia never connects. Off, as Linux behaves
    // (SIO_UDP_CONNRESET, Microsoft's KB263823).
    if ((type & ~B_SOCK_FLAGS) == SOCK_DGRAM) {
        BOOL off = FALSE;
        DWORD n;
        WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof off, NULL, 0, &n, NULL, NULL);
    }
#endif
    int nonblock = (type & B_SOCK_NONBLOCK) != 0;
    if (nonblock) set_nonblock(s);
    int fd = slot_new(s, 0, nonblock);
    if (fd < 0) host_close(s), fail(G_EMFILE);
    ret_x(c, (uint64_t)(int64_t)fd);
}

// int Close(int fd)
static void ov_close(Cpu *c) {
    int fd = (int)arg_x(c, 0), open = 0;
    HostSock s = HOST_BAD;
    LAN_OR_DOWN(c);
    plat_mutex_lock(&slots_lock);   // freed before the host socket closes: no new call can take it meanwhile
    if (fd >= 0 && fd < NSOCK && slots[fd].used) s = slots[fd].s, slots[fd].used = 0, open = 1;
    plat_mutex_unlock(&slots_lock);
    if (!open) {
        fail(G_EBADF);
        ret_x(c, (uint64_t)-1);
        return;
    }
    host_close(s);
    ret_x(c, 0);
}

// int Shutdown(int fd, int how): SHUT_RD/WR/RDWR are 0/1/2 everywhere
static void ov_shutdown(Cpu *c) {
    Slot sl;
    LAN_OR_DOWN(c);
    int ok = !slot_get((int)arg_x(c, 0), &sl), r = ok ? shutdown(sl.s, (int)arg_x(c, 1)) : -1;
    if (ok && r < 0) fail(host_errno());
    ret_x(c, (uint64_t)(int64_t)r);
}

// int Bind(int fd, const sockaddr *addr, socklen_t len)
static void ov_bind(Cpu *c) {
    Slot sl;
    struct sockaddr_in a;
    int r = -1;
    LAN_OR_DOWN(c);
    if (!slot_get((int)arg_x(c, 0), &sl) && !addr_in(arg_p(c, 1), (uint32_t)arg_x(c, 2), &a) && !lan_check(&a, "Bind")) {
        if (!a.sin_addr.s_addr) a.sin_addr.s_addr = ov_lan_bind_address();
        r = bind(sl.s, (struct sockaddr *)&a, sizeof a);
        if (r < 0) fail(host_errno());
    }
    ret_x(c, (uint64_t)(int64_t)r);
}

// int SetSockOpt(int fd, int level, int option, const void *value, socklen_t len)
static void ov_setsockopt(Cpu *c) {
    Slot sl;
    LAN_OR_DOWN(c);
    int level = (int)arg_x(c, 1), opt = (int)arg_x(c, 2), hl = -1, ho = -1;
    const void *v = arg_p(c, 3);
    uint32_t len = (uint32_t)arg_x(c, 4);
    if (slot_get((int)arg_x(c, 0), &sl)) {
        ret_x(c, (uint64_t)-1);
        return;
    }
    if (level == B_SOL_SOCKET) {
        hl = SOL_SOCKET;
        switch (opt) {
        case 0x4: ho = SO_REUSEADDR; break;
#ifdef SO_REUSEPORT
        case 0x200: ho = SO_REUSEPORT; break;
#else
        case 0x200: ho = SO_REUSEADDR; break;
#endif
        case 0x8: ho = SO_KEEPALIVE; break;
        case 0x20: ho = SO_BROADCAST; break;
        case 0x80: ho = SO_LINGER; break;
        case 0x1001: ho = SO_SNDBUF; break;
        case 0x1002: ho = SO_RCVBUF; break;
        case 0x800: ret_x(c, 0); return;   // SO_NOSIGPIPE: the host never raises SIGPIPE for datagrams
        }
    } else if (level == IPPROTO_IP) {
        hl = IPPROTO_IP;
        switch (opt) {
        case 3: ho = IP_TOS; break;
        case 4: ho = IP_TTL; break;
        case 9: ho = IP_MULTICAST_IF; break;
        case 10: ho = IP_MULTICAST_TTL; break;
        case 11: ho = IP_MULTICAST_LOOP; break;
        case 12: ho = IP_ADD_MEMBERSHIP; break;
        case 13: ho = IP_DROP_MEMBERSHIP; break;
        }
    }
    if (ho < 0) {
        // ponytail: an option with no host equivalent is accepted and logged, so Pia's setup goes on; map it when the
        // log names one that matters
        rt_log("socket: SetSockOpt level %#x option %#x (%u bytes) not mapped, accepted", level, opt, len);
        ret_x(c, 0);
        return;
    }
    mem_commit_range((uint64_t)(uintptr_t)v, len);   // the kernel reads these buffers (Windows' lazy commit, memory.c)
    int r = setsockopt(sl.s, hl, ho, v, (socklen_t)len);
    if (r < 0) fail(host_errno());
    ret_x(c, (uint64_t)(int64_t)r);
}

// Waits up to ms (-1: forever) for fd to be readable, giving the virtual core up meanwhile; 1 readable, 0 timeout
static int wait_readable(HostSock s, int ms) {
    struct pollfd p = {.fd = s, .events = POLLIN};
    int away = ms ? kern_core_away() : 0;
    int r = host_poll(&p, 1, ms);
    kern_core_back(&away);
    return r > 0;
}

// ssize_t RecvFrom(int fd, void *buf, size_t n, int flags, sockaddr *from, socklen_t *fromlen)
static void ov_recvfrom(Cpu *c) {
    Slot sl;
    LAN_OR_DOWN(c);
    if (slot_get((int)arg_x(c, 0), &sl)) {
        ret_x(c, (uint64_t)-1);
        return;
    }
    int dontwait, hf = msg_flags((int)arg_x(c, 3), &dontwait);
    if (!wait_readable(sl.s, dontwait || sl.nonblock ? 0 : -1)) {
        fail(G_EAGAIN);
        ret_x(c, (uint64_t)-1);
        return;
    }
    struct sockaddr_in a;
    socklen_t alen = sizeof a;
    mem_commit_range(arg_x(c, 1), arg_x(c, 2));
#ifdef _WIN32
    int n = recvfrom(sl.s, arg_p(c, 1), (int)arg_x(c, 2), hf, (struct sockaddr *)&a, &alen);
#else
    ssize_t n = recvfrom(sl.s, arg_p(c, 1), (size_t)arg_x(c, 2), hf | MSG_DONTWAIT, (struct sockaddr *)&a, &alen);
#endif
    if (n < 0) fail(host_errno());
    else addr_out(&a, arg_p(c, 4), arg_p(c, 5));
    ret_x(c, (uint64_t)(int64_t)n);
}

// ssize_t SendTo(int fd, const void *buf, size_t n, int flags, const sockaddr *to, socklen_t tolen)
static void ov_sendto(Cpu *c) {
    Slot sl;
    LAN_OR_DOWN(c);
    if (slot_get((int)arg_x(c, 0), &sl)) {
        ret_x(c, (uint64_t)-1);
        return;
    }
    int dontwait, hf = msg_flags((int)arg_x(c, 3), &dontwait);
    struct sockaddr_in a, *to = NULL;
    if (arg_p(c, 4)) {
        if (addr_in(arg_p(c, 4), (uint32_t)arg_x(c, 5), &a) || lan_check(&a, "SendTo")) {
            ret_x(c, (uint64_t)-1);
            return;
        }
        to = &a;
    }
    // ponytail: a datagram send does not wait on a LAN, so MSG_DONTWAIT changes nothing here
    mem_commit_range(arg_x(c, 1), arg_x(c, 2));
    // A broadcast of the LAN lobby: also, one datagram each, to the PCs of MKW_LAN_PEERS (a virtual LAN such as
    // Tailscale carries no broadcast). Sent: the broadcast's own result may then be a failure, the lobby is reached.
    uint32_t fan[OV_LAN_PEERS_MAX];
    int nfan = to ? ov_lan_peers(to->sin_addr.s_addr, fan, OV_LAN_PEERS_MAX) : 0, fanned = 0;
    for (int i = 0; i < nfan; i++) {
        struct sockaddr_in p = *to;
        p.sin_addr.s_addr = fan[i];
        if (sendto(sl.s, arg_p(c, 1), (int)arg_x(c, 2), hf, (struct sockaddr *)&p, sizeof p) >= 0) fanned = 1;
    }
    int64_t n = sendto(sl.s, arg_p(c, 1), (int)arg_x(c, 2), hf, (struct sockaddr *)to, to ? sizeof a : 0);
    if (n < 0 && fanned) n = (int64_t)arg_x(c, 2);
    if (n < 0) fail(host_errno());
    ret_x(c, (uint64_t)n);
}

// int Poll(pollfd *fds, nfds_t n, int timeout_ms): the console's pollfd is {int fd; short events, revents}
typedef struct {
    int32_t fd;
    int16_t events, revents;
} GPollFd;

static void ov_poll(Cpu *c) {
    GPollFd *g = arg_p(c, 0);
    uint64_t n = arg_x(c, 1);
    int ms = (int)arg_x(c, 2);
    struct pollfd h[NSOCK];
    LAN_OR_DOWN(c);
    if (n > NSOCK) {
        fail(G_EINVAL);
        ret_x(c, (uint64_t)-1);
        return;
    }
    int nvalid = 0;
    plat_mutex_lock(&slots_lock);
    for (uint64_t i = 0; i < n; i++) {
        int fd = g[i].fd;
        g[i].revents = 0;
        h[i].fd = HOST_BAD, h[i].events = 0, h[i].revents = 0;
        if (fd < 0) continue;   // ignored, as poll(2) does
        if (fd >= NSOCK || !slots[fd].used) {
            g[i].revents = G_POLLNVAL;
            continue;
        }
        h[i].fd = slots[fd].s, h[i].events = ev_host(g[i].events);
        nvalid++;
    }
    plat_mutex_unlock(&slots_lock);
    int invalid = 0;
    for (uint64_t i = 0; i < n; i++) invalid += g[i].revents != 0;
    int r = 0;
    if (!nvalid && !invalid && ms) {   // nothing to watch: poll(2) still sleeps its timeout
        int away = kern_core_away();
        if (ms > 0) plat_sleep_ms((uint32_t)ms);
        else rt_log("socket: Poll with no descriptors and no timeout");
        kern_core_back(&away);
    }
    if (nvalid) {
        int away = (ms && !invalid) ? kern_core_away() : 0;
        r = host_poll(h, (unsigned)n, invalid ? 0 : ms);
        kern_core_back(&away);
        if (r < 0) {
            fail(host_errno());
            ret_x(c, (uint64_t)-1);
            return;
        }
        for (uint64_t i = 0; i < n; i++)
            if (h[i].fd != HOST_BAD) g[i].revents = (int16_t)ev_guest(h[i].revents);
    }
    r = 0;
    for (uint64_t i = 0; i < n; i++) r += g[i].revents != 0;
    ret_x(c, (uint64_t)(int64_t)r);
}

// ---- eventfd: Linux's own, with the console's flags (semaphore 1, nonblocking 4)
// int EventFd(uint64_t initval, EventFdFlags flags)
static void ov_eventfd(Cpu *c) {
    LAN_OR_DOWN(c);
#ifdef _WIN32
    // ponytail: no eventfd on Windows; only gRPC (online) calls it, see the top. A loopback socket pair if Pia ever does
    fail(G_ENOSYS);
    ret_x(c, (uint64_t)-1);
#else
    uint32_t f = (uint32_t)arg_x(c, 1);
    int e = eventfd((unsigned)arg_x(c, 0), (f & B_EFD_SEMAPHORE ? EFD_SEMAPHORE : 0) | EFD_CLOEXEC);
    int nonblock = (f & B_EFD_NONBLOCK) != 0;
    if (e >= 0 && nonblock) set_nonblock(e);
    int fd = e < 0 ? -1 : slot_new(e, 1, nonblock);
    if (e < 0) fail(errno);
    else if (fd < 0) close(e), fail(G_EMFILE);
    ret_x(c, (uint64_t)(int64_t)fd);
#endif
}

// int EventFdRead(int fd, uint64_t *value) / int EventFdWrite(int fd, uint64_t value)
static void ov_eventfd_read(Cpu *c) {
    Slot sl;
    LAN_OR_DOWN(c);
    int ok = !slot_get((int)arg_x(c, 0), &sl), r = -1;
#ifndef _WIN32
    if (ok && (sl.nonblock || wait_readable(sl.s, -1))) {
        r = read(sl.s, arg_p(c, 1), 8) == 8 ? 0 : -1;
        if (r < 0) fail(errno);
    }
#else
    if (ok) fail(G_ENOSYS);
#endif
    ret_x(c, (uint64_t)(int64_t)r);
}
static void ov_eventfd_write(Cpu *c) {
    Slot sl;
    LAN_OR_DOWN(c);
    int ok = !slot_get((int)arg_x(c, 0), &sl), r = -1;
#ifndef _WIN32
    uint64_t v = arg_x(c, 1);
    if (ok) {
        r = write(sl.s, &v, 8) == 8 ? 0 : -1;
        if (r < 0) fail(errno);
    }
#else
    if (ok) fail(G_ENOSYS);
#endif
    ret_x(c, (uint64_t)(int64_t)r);
}

// ---- names: no resolver offline (online play would need one); EAI_FAIL is 4 in the console's AiErrno
static void ov_getaddrinfo(Cpu *c) {
    void **out = arg_p(c, 3);
    if (out) *out = NULL;
    ret_x(c, 4);
}

HOST_OVERRIDE("_ZN2nn6socket8internal10InitializeEi", ov_init);
HOST_OVERRIDE("_ZN2nn6socket8FinalizeEv", ov_finalize);
HOST_OVERRIDE("_ZN2nn6socket13IsInitializedEv", ov_is_initialized);
HOST_OVERRIDE("_ZN2nn6socket12SetAllocatorEPNS_14MemoryResourceE", ov_nothing);
HOST_OVERRIDE("_ZN2nn6socket12GetLastErrnoEv", ov_last_errno);
HOST_OVERRIDE("_ZN2nn6socket12GetLastErrorEv", ov_last_errno);   // nn::socket::Errno: the same numbers
HOST_OVERRIDE("_ZN2nn6socket6SocketEiii", ov_socket);
HOST_OVERRIDE("_ZN2nn6socket6SocketENS0_6FamilyENS0_4TypeENS0_8ProtocolE", ov_socket);
HOST_OVERRIDE("_ZN2nn6socket5CloseEi", ov_close);
HOST_OVERRIDE("_ZN2nn6socket8ShutdownEii", ov_shutdown);
HOST_OVERRIDE("_ZN2nn6socket4BindEiPK8sockaddrj", ov_bind);
HOST_OVERRIDE("_ZN2nn6socket4BindEiPKNS0_8SockAddrEj", ov_bind);
HOST_OVERRIDE("_ZN2nn6socket10SetSockOptEiNS0_5LevelENS0_6OptionEPKvj", ov_setsockopt);
HOST_OVERRIDE("_ZN2nn6socket8RecvFromEiPvmiP8sockaddrPj", ov_recvfrom);
HOST_OVERRIDE("_ZN2nn6socket8RecvFromEiPvmNS0_7MsgFlagEPNS0_8SockAddrEPj", ov_recvfrom);
HOST_OVERRIDE("_ZN2nn6socket6SendToEiPKvmiPK8sockaddrj", ov_sendto);
HOST_OVERRIDE("_ZN2nn6socket6SendToEiPKvmNS0_7MsgFlagEPKNS0_8SockAddrEj", ov_sendto);
HOST_OVERRIDE("_ZN2nn6socket4PollEP6pollfdmi", ov_poll);
HOST_OVERRIDE("_ZN2nn6socket7EventFdEmNS0_12EventFdFlagsE", ov_eventfd);
HOST_OVERRIDE("_ZN2nn6socket11EventFdReadEiPm", ov_eventfd_read);
HOST_OVERRIDE("_ZN2nn6socket12EventFdWriteEim", ov_eventfd_write);
HOST_OVERRIDE("_ZN2nn6socket11GetAddrInfoEPKcS2_PK8addrinfoPPS3_", ov_getaddrinfo);
HOST_OVERRIDE("_ZN2nn6socket11GetAddrInfoEPKcS2_PKNS0_8AddrInfoEPPS3_", ov_getaddrinfo);
HOST_OVERRIDE("_ZN2nn6socket12FreeAddrInfoEP8addrinfo", ov_nothing);   // nothing to free: GetAddrInfo gives none
HOST_OVERRIDE("_ZN2nn6socket12FreeAddrInfoEPNS0_8AddrInfoE", ov_nothing);
