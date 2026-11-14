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
// MKW_LAN off (the default) keeps main's stub, a console whose network is down: every socket call -1, errno ENETDOWN,
// IsInitialized 1, no WSAStartup, so the online menu takes its no-connection path as before (issue #63).
//
// eventfd exists only on Linux; Windows answers ENOSYS. In main only gRPC's wakeup_fd_eventfd.cc (main+0x10208c0, the
// online services) calls EventFd*, after its availability check (main+0x1020a30); Pia never does.
#include <stdint.h>
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
#include <netinet/in.h>
#include <poll.h>
#include <netinet/tcp.h>
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
       G_ETIMEDOUT = 110, G_EHOSTUNREACH = 113, G_EPIPE = 32, G_ECONNABORTED = 103, G_EISCONN = 106,
       G_ENOTCONN = 107, G_ECONNREFUSED = 111, G_EALREADY = 114, G_EINPROGRESS = 115 };

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
// MKW_NPLN_TRACE=1: every socket call of the online services in the log (ov_npln.c). A diagnostic knob without a
// row in cfg.c, so it reads cfg_get (cfg_bool stops the game on a name without a row)
static int ntrace_on(void) {
    const char *v = cfg_get("MKW_NPLN_TRACE");
    return v && *v && strcmp(v, "0") != 0;
}
#define NTRACE(...)                                                                                                    \
    do {                                                                                                               \
        if (ntrace_on()) {                                                                              \
            char line_[600];                                                                                           \
            snprintf(line_, sizeof line_, __VA_ARGS__);                                                                \
            rt_log("socket: %llu ms t%u %s", (unsigned long long)(plat_now_ns() / 1000000u), plat_thread_id(), line_); \
        }                                                                                                              \
    } while (0)

#define B_ENETDOWN 50   // what main's stub answered: the console's BSD errno for a network that is down
int ov_lan(void) { return cfg_bool("MKW_LAN"); }
// Host sockets are live with LAN play or with an NPLN server (ov_npln.c)
static int net_on(void) { return ov_lan() || ov_npln_on(); }
// Both off: the call fails as main's stub did
#define LAN_OR_DOWN(c)                                                                                                \
    do {                                                                                                               \
        if (!net_on()) {                                                                                               \
            ret_x(c, (uint64_t)-1);                                                                                    \
            return;                                                                                                    \
        }                                                                                                              \
    } while (0)

#ifdef _WIN32
static int host_err_map(int e) {
    switch (e) {
    case 0: return 0;
    case WSAEWOULDBLOCK: return G_EAGAIN;
    case WSAEINPROGRESS: return G_EINPROGRESS;
    case WSAEALREADY: return G_EALREADY;
    case WSAEISCONN: return G_EISCONN;
    case WSAENOTCONN: return G_ENOTCONN;
    case WSAECONNREFUSED: return G_ECONNREFUSED;
    case WSAECONNABORTED: return G_ECONNABORTED;
    case WSAESHUTDOWN: return G_EPIPE;
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
static int host_errno(void) { return host_err_map(WSAGetLastError()); }
#else
static int host_err_map(int e) { return e; }
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

// 0, or -1 with ENETUNREACH (logged, a few times) for an address outside a local network
static int lan_check(const struct sockaddr_in *a, const char *call) {
    if (lan_only(a->sin_addr)) return 0;
    uint32_t npln_ip;   // the NPLN server (MKW_NPLN), wherever it is: the one non-local address the game may reach
    if (ov_npln_server(&npln_ip, NULL, NULL) && a->sin_addr.s_addr == npln_ip) return 0;
    static volatile int32_t n;
    if (plat_inc32(&n) <= 8) {
        char t[INET_ADDRSTRLEN] = "?";
        inet_ntop(AF_INET, &a->sin_addr, t, sizeof t);
        rt_log("socket: %s to %s refused: LAN play reaches only local network addresses", call, t);
    }
    return fail(G_ENETUNREACH), -1;
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
    if (!net_on()) {   // main's stub: "succeeds" (MKW ignores the result, main+0x2247900)
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
static void ov_is_initialized(Cpu *c) { ret_x(c, net_on() ? ready : 1); }
static void ov_nothing(Cpu *c) { (void)c; }
static void ov_last_errno(Cpu *c) { ret_x(c, net_on() ? (uint32_t)last_errno : B_ENETDOWN); }
// void SetLastErrno(int)
static void ov_set_errno(Cpu *c) { last_errno = (int)arg_x(c, 0); }

// ---- sockets
// int Socket(int family, int type, int protocol)
static void ov_socket(Cpu *c) {
    int family = (int)arg_x(c, 0), protocol = (int)arg_x(c, 2);
    uint32_t type = (uint32_t)arg_x(c, 1);
    LAN_OR_DOWN(c);
    if (family != B_AF_INET) {
        NTRACE("Socket family %d refused (IPv4 only)", family);
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
    NTRACE("Socket family %d type %#x protocol %d -> %d", family, type, protocol, fd);
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
    NTRACE("Close %d", fd);
    ret_x(c, 0);
}

// int Shutdown(int fd, int how): SHUT_RD/WR/RDWR are 0/1/2 everywhere
static void ov_shutdown(Cpu *c) {
    Slot sl;
    LAN_OR_DOWN(c);
    int ok = !slot_get((int)arg_x(c, 0), &sl), r = ok ? shutdown(sl.s, (int)arg_x(c, 1)) : -1;
    NTRACE("Shutdown %d how %d", (int)arg_x(c, 0), (int)arg_x(c, 1));
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
        r = bind(sl.s, (struct sockaddr *)&a, sizeof a);
        if (r < 0) fail(host_errno());
    }
    ret_x(c, (uint64_t)(int64_t)r);
}

// The host's level and option of the console's (FreeBSD numbers); 1 mapped, 2 SO_NOSIGPIPE (nothing to do), 0 none
static int opt_map(int level, int opt, int *hl, int *ho) {
    *hl = *ho = -1;
    if (level == B_SOL_SOCKET) {
        *hl = SOL_SOCKET;
        switch (opt) {
        case 0x4: *ho = SO_REUSEADDR; break;
#ifdef SO_REUSEPORT
        case 0x200: *ho = SO_REUSEPORT; break;
#else
        case 0x200: *ho = SO_REUSEADDR; break;
#endif
        case 0x8: *ho = SO_KEEPALIVE; break;
        case 0x20: *ho = SO_BROADCAST; break;
        case 0x80: *ho = SO_LINGER; break;
        case 0x1001: *ho = SO_SNDBUF; break;
        case 0x1002: *ho = SO_RCVBUF; break;
        case 0x800: return 2;
        }
    } else if (level == IPPROTO_IP) {
        *hl = IPPROTO_IP;
        switch (opt) {
        case 3: *ho = IP_TOS; break;
        case 4: *ho = IP_TTL; break;
        case 9: *ho = IP_MULTICAST_IF; break;
        case 10: *ho = IP_MULTICAST_TTL; break;
        case 11: *ho = IP_MULTICAST_LOOP; break;
        case 12: *ho = IP_ADD_MEMBERSHIP; break;
        case 13: *ho = IP_DROP_MEMBERSHIP; break;
        }
    } else if (level == IPPROTO_TCP && opt == 1) {   // TCP_NODELAY (gRPC, curl): 1 on every host
        *hl = IPPROTO_TCP, *ho = TCP_NODELAY;
    }
    return *ho >= 0;
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
    int m = opt_map(level, opt, &hl, &ho);
    if (m == 2) {   // SO_NOSIGPIPE: the host never raises SIGPIPE for datagrams
        ret_x(c, 0);
        return;
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
    int64_t n = sendto(sl.s, arg_p(c, 1), (int)arg_x(c, 2), hf, (struct sockaddr *)to, to ? sizeof a : 0);
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
    if (ntrace_on() && ms != 0 && n <= NSOCK) {   // a poll that may wait: what it waits for
        char list[256] = "";
        for (uint64_t i = 0, k = 0; i < n && k < sizeof list - 32; i++)
            k += (uint64_t)snprintf(list + k, sizeof list - k, " %d:%#x", g[i].fd, g[i].events);
        NTRACE("Poll waits on%s, timeout %d", list, ms);
    }
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
    static volatile int32_t traced;
    if (ntrace_on() && plat_inc32(&traced) <= 400) {
        char list[256] = "";
        for (uint64_t i = 0, k = 0; i < n && k < sizeof list - 32; i++)
            k += (uint64_t)snprintf(list + k, sizeof list - k, " %d:%#x/%#x", g[i].fd, g[i].events, g[i].revents);
        NTRACE("Poll%s timeout %d -> %d", list, ms, r);
    }
    ret_x(c, (uint64_t)(int64_t)r);
}

// ---- eventfd: Linux's own, with the console's flags (semaphore 1, nonblocking 4)
// int EventFd(uint64_t initval, EventFdFlags flags)
static void ov_eventfd(Cpu *c) {
    LAN_OR_DOWN(c);
#ifdef _WIN32
    // No eventfd on Windows: a loopback datagram socket connected to itself stands in, so WSAPoll sees it among the
    // sockets as gRPC's wakeup fd (wakeup_fd_eventfd.cc, only online). Each write is one datagram of its 8-byte value,
    // a read takes them all and adds them up (eventfd's counter); semaphore mode takes one and sends the rest back.
    uint32_t f = (uint32_t)arg_x(c, 1);
    HostSock s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in a = {.sin_family = AF_INET};
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int alen = sizeof a;
    if (s == HOST_BAD || bind(s, (struct sockaddr *)&a, sizeof a) || getsockname(s, (struct sockaddr *)&a, &alen) ||
        connect(s, (struct sockaddr *)&a, sizeof a) || set_nonblock(s)) {
        fail(host_errno());
        if (s != HOST_BAD) host_close(s);
        ret_x(c, (uint64_t)-1);
        return;
    }
    uint64_t init = arg_x(c, 0);
    if (init) send(s, (const char *)&init, 8, 0);
    int fd = slot_new(s, 1 | (f & B_EFD_SEMAPHORE ? 2 : 0), (f & B_EFD_NONBLOCK) != 0);
    if (fd < 0) host_close(s), fail(G_EMFILE);
    NTRACE("EventFd flags %#x -> %d", f, fd);
    ret_x(c, (uint64_t)(int64_t)fd);
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
    if (ok && (sl.nonblock || wait_readable(sl.s, -1))) {
        uint64_t sum = 0, v;
        int got = 0;
        while (recv(sl.s, (char *)&v, 8, 0) == 8) {
            got = 1;
            if (sl.eventfd & 2) {   // semaphore: one unit out, the rest back in
                if (v > 1) v--, send(sl.s, (const char *)&v, 8, 0);
                sum = 1;
                break;
            }
            sum += v;
        }
        if (got) *(uint64_t *)arg_p(c, 1) = sum, r = 0;
        else fail(G_EAGAIN);
    }
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
    uint64_t v = arg_x(c, 1);
    if (ok) {
        r = send(sl.s, (const char *)&v, 8, 0) == 8 ? 0 : -1;
        if (r < 0) fail(host_errno());
    }
#endif
    ret_x(c, (uint64_t)(int64_t)r);
}

// ---- stream sockets: the C API (nnsocket*) of main's gRPC (main+0x1016000..0x1022000, the NPLN services) and libcurl
// (main+0x14c5000..0x14f2000, the UGC uploads and downloads). The sdk's nnsocket* jump straight to its own nn::socket::*
// (sdk+0x15e1d0: b nn::socket::Connect), so they are replaced by their own names. Only with MKW_NPLN (or LAN play) on;
// a connection may go only where Bind and SendTo may (lan_check: a local network or the NPLN server).

// Waits up to ms (-1: forever) for fd to be writable (a connect that completed or failed); 1 ready, 0 timeout
static int wait_writable(HostSock s, int ms) {
    struct pollfd p = {.fd = s, .events = POLLOUT};
    int away = ms ? kern_core_away() : 0;
    int r = host_poll(&p, 1, ms);
    kern_core_back(&away);
    return r > 0;
}

// int Connect(int fd, const sockaddr *addr, socklen_t len)
static void ov_connect(Cpu *c) {
    Slot sl;
    struct sockaddr_in a;
    LAN_OR_DOWN(c);
    if (slot_get((int)arg_x(c, 0), &sl) || addr_in(arg_p(c, 1), (uint32_t)arg_x(c, 2), &a) || lan_check(&a, "Connect")) {
        ret_x(c, (uint64_t)-1);
        return;
    }
    if (!sl.nonblock) set_nonblock(sl.s);   // a blocking connect waits below, with the virtual core given up
    int r = connect(sl.s, (struct sockaddr *)&a, sizeof a), e = r < 0 ? host_errno() : 0;
    if (r < 0 && (e == G_EAGAIN || e == G_EINPROGRESS)) e = G_EINPROGRESS;   // Windows: WSAEWOULDBLOCK
    if (!sl.nonblock) {
        if (e == G_EINPROGRESS) {
            wait_writable(sl.s, -1);
            int err = 0;
            socklen_t n = sizeof err;
            getsockopt(sl.s, SOL_SOCKET, SO_ERROR, (char *)&err, &n);
            e = host_err_map(err), r = e ? -1 : 0;
        }
#ifdef _WIN32
        u_long off = 0;
        ioctlsocket(sl.s, FIONBIO, &off);
#else
        fcntl(sl.s, F_SETFL, fcntl(sl.s, F_GETFL) & ~O_NONBLOCK);
#endif
    }
    if (r < 0) fail(e);
    char t[INET_ADDRSTRLEN] = "?";
    inet_ntop(AF_INET, &a.sin_addr, t, sizeof t);
    rt_log("socket: Connect %d to %s:%u: %s", (int)arg_x(c, 0), t, ntohs(a.sin_port),
           r == 0 ? "connected" : e == G_EINPROGRESS ? "in progress" : "failed");
    ret_x(c, (uint64_t)(int64_t)r);
}

// One send or receive on a stream socket, waiting unless it is nonblocking or dontwait; bytes, or -1 with errno
static int64_t stream_io(const Slot *sl, void *buf, uint64_t n, int hflags, int dontwait, int recv_side) {
    int nb = sl->nonblock || dontwait;
    if (!nb) NTRACE("%s may wait: blocking socket, flags %#x", recv_side ? "receive" : "send", hflags);
    for (;;) {
        if (recv_side ? !wait_readable(sl->s, nb ? 0 : -1) : !wait_writable(sl->s, nb ? 0 : -1)) {
            if (nb) return fail(G_EAGAIN), -1;
            continue;
        }
        mem_commit_range((uint64_t)(uintptr_t)buf, n);
        int64_t r = recv_side ? recv(sl->s, buf, (int)n, hflags) : send(sl->s, buf, (int)n, hflags);
        if (r >= 0) {
            NTRACE("%s %lld of %llu bytes", recv_side ? "received" : "sent", (long long)r, (unsigned long long)n);
            if (ntrace_on() && r > 0) {   // the bytes too, as hex (up to 2 KiB)
                static const char hx[] = "0123456789abcdef";
                char line[4200];
                int64_t k = r < 2048 ? r : 2048;
                for (int64_t i = 0; i < k; i++)
                    line[2 * i] = hx[((uint8_t *)buf)[i] >> 4], line[2 * i + 1] = hx[((uint8_t *)buf)[i] & 15];
                line[2 * k] = 0;
                rt_log("socket: bytes %s", line);   // whole, past NTRACE's line length
            }
            return r;
        }
        int e = host_errno();
        if (recv_side && e == G_EPIPE) return 0;   // Windows' WSAESHUTDOWN after a shutdown: end of stream, as on Linux
        if (e != G_EAGAIN) NTRACE("%s on a stream socket failed, errno %d", recv_side ? "receive" : "send", e);
        if (e != G_EAGAIN || nb) return fail(e), -1;
    }
}

// ssize_t Send(int fd, const void *buf, size_t n, int flags) / Recv(int fd, void *buf, size_t n, int flags)
static void ov_send(Cpu *c) {
    Slot sl;
    LAN_OR_DOWN(c);
    if (slot_get((int)arg_x(c, 0), &sl)) {
        ret_x(c, (uint64_t)-1);
        return;
    }
    int dontwait, hf = msg_flags((int)arg_x(c, 3), &dontwait);
    ret_x(c, (uint64_t)stream_io(&sl, arg_p(c, 1), arg_x(c, 2), hf, dontwait, 0));
}
static void ov_recv(Cpu *c) {
    Slot sl;
    LAN_OR_DOWN(c);
    if (slot_get((int)arg_x(c, 0), &sl)) {
        ret_x(c, (uint64_t)-1);
        return;
    }
    int dontwait, hf = msg_flags((int)arg_x(c, 3), &dontwait);
    ret_x(c, (uint64_t)stream_io(&sl, arg_p(c, 1), arg_x(c, 2), hf, dontwait, 1));
}

// The console's msghdr and iovec (FreeBSD, LP64): name 0, namelen 8, iov 0x10, iovlen 0x18, control 0x20,
// controllen 0x28, flags 0x2c; iovec {base, len}. gRPC's tcp_posix sends with SendMsg and no name or control data.
typedef struct {
    uint64_t base, len;
} GIov;
static GIov *msg_iov(const uint8_t *m, int *count, uint64_t *total) {
    GIov *v = (GIov *)(uintptr_t)*(const uint64_t *)(m + 0x10);
    *count = *(const int32_t *)(m + 0x18), *total = 0;
    for (int i = 0; i < *count; i++) *total += v[i].len;
    return v;
}

// ssize_t SendMsg(int fd, const msghdr *msg, int flags): the pieces gathered into one send
static void ov_sendmsg(Cpu *c) {
    Slot sl;
    LAN_OR_DOWN(c);
    const uint8_t *m = arg_p(c, 1);
    if (slot_get((int)arg_x(c, 0), &sl) || !m) {
        ret_x(c, (uint64_t)-1);
        return;
    }
    int count, dontwait, hf = msg_flags((int)arg_x(c, 2), &dontwait);
    uint64_t total, off = 0;
    GIov *v = msg_iov(m, &count, &total);
    uint8_t *buf = malloc(total ? total : 1);
    for (int i = 0; i < count; i++) {
        mem_commit_range(v[i].base, v[i].len);
        memcpy(buf + off, (void *)(uintptr_t)v[i].base, v[i].len), off += v[i].len;
    }
    int64_t r = stream_io(&sl, buf, total, hf, dontwait, 0);
    free(buf);
    ret_x(c, (uint64_t)r);
}

// ssize_t RecvMsg(int fd, msghdr *msg, int flags): one receive scattered over the pieces
static void ov_recvmsg(Cpu *c) {
    Slot sl;
    LAN_OR_DOWN(c);
    uint8_t *m = arg_p(c, 1);
    if (slot_get((int)arg_x(c, 0), &sl) || !m) {
        ret_x(c, (uint64_t)-1);
        return;
    }
    int count, dontwait, hf = msg_flags((int)arg_x(c, 2), &dontwait);
    uint64_t total;
    GIov *v = msg_iov(m, &count, &total);
    NTRACE("RecvMsg %d flags %#x, %d pieces of %llu bytes, nonblocking %d", (int)arg_x(c, 0), (int)arg_x(c, 2), count,
           (unsigned long long)total, sl.nonblock);
#ifdef _WIN32
    static volatile int32_t nothing_traced;
    if (!total && ntrace_on() && plat_inc32(&nothing_traced) <= 3) {   // who reads nothing: host stack
        void *fr[40];
        USHORT n = RtlCaptureStackBackTrace(0, 40, fr, NULL);
        char line[1200];
        int k = 0;
        for (USHORT i = 0; i < n && k < (int)sizeof line - 40; i++) {
            HMODULE m = NULL;
            char name[MAX_PATH] = "?";
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)fr[i], &m);
            if (m) GetModuleFileNameA(m, name, sizeof name);
            const char *b = strrchr(name, '\\');
            k += snprintf(line + k, sizeof line - k, " %s+%#llx", b ? b + 1 : name,
                          (unsigned long long)((uintptr_t)fr[i] - (uintptr_t)m));
        }
        rt_log("socket: RecvMsg of nothing, host stack:%s", line);
    }
#endif
    uint8_t *buf = malloc(total ? total : 1);
    int64_t r = stream_io(&sl, buf, total, hf, dontwait, 1);
    NTRACE("RecvMsg %d -> %lld (errno %d)", (int)arg_x(c, 0), (long long)r, r < 0 ? last_errno : 0);
    for (int i = 0, off = 0; r > 0 && i < count && off < r; i++) {
        uint64_t k = v[i].len < (uint64_t)(r - off) ? v[i].len : (uint64_t)(r - off);
        mem_commit_range(v[i].base, k);
        memcpy((void *)(uintptr_t)v[i].base, buf + off, k), off += (int)k;
    }
    free(buf);
    if (r >= 0) *(uint32_t *)(m + 0x28) = 0, *(int32_t *)(m + 0x2c) = 0;   // no control data, no flags
    ret_x(c, (uint64_t)r);
}

// int Fcntl(int fd, int cmd, ...): F_GETFL 3 and F_SETFL 4 with Linux's O_NONBLOCK 0x800, as the console's errno is
// Linux's (gRPC sets 0x802 at main+0x1019e40's socket setup); F_GETFD/F_SETFD answered
static void ov_fcntl(Cpu *c) {
    int fd = (int)arg_x(c, 0), cmd = (int)arg_x(c, 1), arg = (int)arg_x(c, 2), r = 0;
    Slot sl;
    LAN_OR_DOWN(c);
    if (slot_get(fd, &sl)) {
        ret_x(c, (uint64_t)-1);
        return;
    }
    NTRACE("Fcntl %d cmd %d arg %#x", fd, cmd, arg);
    if (cmd == 3) r = 2 | (sl.nonblock ? 0x800 : 0);   // O_RDWR
    else if (cmd == 4) {
        int nb = (arg & 0x800) != 0;
#ifdef _WIN32
        u_long on = (u_long)nb;
        ioctlsocket(sl.s, FIONBIO, &on);
#else
        fcntl(sl.s, F_SETFL, nb ? fcntl(sl.s, F_GETFL) | O_NONBLOCK : fcntl(sl.s, F_GETFL) & ~O_NONBLOCK);
#endif
        plat_mutex_lock(&slots_lock);
        if (slots[fd].used) slots[fd].nonblock = nb;
        plat_mutex_unlock(&slots_lock);
    }
    ret_x(c, (uint64_t)(int64_t)r);
}

// int GetSockOpt(int fd, int level, int option, void *value, socklen_t *len): SO_ERROR (0x1007) and SO_TYPE (0x1008)
// from the host, the console's errno for the error; anything else reads as 0
static void ov_getsockopt(Cpu *c) {
    Slot sl;
    LAN_OR_DOWN(c);
    int level = (int)arg_x(c, 1), opt = (int)arg_x(c, 2);
    int32_t *v = arg_p(c, 3);
    uint32_t *len = arg_p(c, 4);
    if (slot_get((int)arg_x(c, 0), &sl) || !v || !len || *len < 4) {
        ret_x(c, (uint64_t)-1);
        return;
    }
    int x = 0;
    socklen_t n = sizeof x;
    if (level == B_SOL_SOCKET && opt == 0x1007) {
        getsockopt(sl.s, SOL_SOCKET, SO_ERROR, (char *)&x, &n);
        x = host_err_map(x);
    } else if (level == B_SOL_SOCKET && opt == 0x1008) {
        getsockopt(sl.s, SOL_SOCKET, SO_TYPE, (char *)&x, &n);
    } else {   // what SetSockOpt set (gRPC reads each option back to check it, main+0x1019e40's setup)
        int hl, ho;
        if (opt_map(level, opt, &hl, &ho) == 1 && !getsockopt(sl.s, hl, ho, (char *)&x, &n) && n == 1) x &= 0xff;
    }
    *v = x, *len = 4;
    NTRACE("GetSockOpt %d level %#x option %#x -> %d", (int)arg_x(c, 0), level, opt, x);
    ret_x(c, 0);
}

// int GetSockName / GetPeerName(int fd, sockaddr *addr, socklen_t *len)
static void sock_name(Cpu *c, int peer) {
    Slot sl;
    LAN_OR_DOWN(c);
    struct sockaddr_in a = {0};
    socklen_t n = sizeof a;
    if (slot_get((int)arg_x(c, 0), &sl)) {
        ret_x(c, (uint64_t)-1);
        return;
    }
    int r = peer ? getpeername(sl.s, (struct sockaddr *)&a, &n) : getsockname(sl.s, (struct sockaddr *)&a, &n);
    if (r < 0) fail(host_errno());
    else addr_out(&a, arg_p(c, 1), arg_p(c, 2));
    ret_x(c, (uint64_t)(int64_t)r);
}
static void ov_getsockname(Cpu *c) { sock_name(c, 0); }
static void ov_getpeername(Cpu *c) { sock_name(c, 1); }

// Listen, Accept: nothing in the game serves; they fail as with the network down
static void ov_sock_refuse(Cpu *c) {
    fail(G_EINVAL);
    ret_x(c, (uint64_t)-1);
}

// HostSock of guest descriptor fd for ov_ssl.c; 0, or -1 when it is not open
int ov_sock_host(int fd, intptr_t *out, int *nonblock) {
    Slot sl;
    if (slot_get(fd, &sl)) return -1;
    *out = (intptr_t)sl.s;
    if (nonblock) *nonblock = sl.nonblock;
    return 0;
}

// ov_ssl.c's passthrough I/O on descriptor fd: bytes, OV_IO_AGAIN (nothing now, nonblocking) or OV_IO_ERROR
int64_t ov_sock_io(int fd, void *buf, uint64_t n, int recv_side, int peek, int nonblock) {
    Slot sl;
    if (slot_get(fd, &sl)) return OV_IO_ERROR;
    sl.nonblock = nonblock;
    int64_t r = stream_io(&sl, buf, n, peek ? MSG_PEEK : 0, 0, recv_side);
    return r >= 0 ? r : last_errno == G_EAGAIN ? OV_IO_AGAIN : OV_IO_ERROR;
}

// ---- names. Off (no LAN, no MKW_NPLN): no resolver, EAI_FAIL (4 in the console's AiErrno). With MKW_NPLN, the names
// of Nintendo's servers (*.nintendo.net, *.nintendo.com) resolve to the NPLN server and a numeric address to itself;
// any other name gets EAI_NONAME (8), so the game reaches nothing else. Port 443 becomes the server's gRPC port for
// gRPC's resolver and its HTTP port for curl's (the UGC files), so each client lands on the service it speaks.
// The console's addrinfo is FreeBSD's (main+0x1016d8c reads ai_addrlen at 0x10, ai_addr at 0x20, ai_next at 0x28).
enum { GAI_GRPC, GAI_HTTP };
static int ends_with(const char *s, const char *tail) {
    size_t n = strlen(s), k = strlen(tail);
    return n >= k && !strcmp(s + n - k, tail);
}

static void getaddrinfo_npln(Cpu *c, int kind) {
    const char *node = arg_p(c, 0), *service = arg_p(c, 1);
    uint64_t *out = arg_p(c, 3);
    if (out) *out = 0;
    uint32_t ip;
    uint16_t grpc_port, http_port;
    if (!out || !ov_npln_server(&ip, &grpc_port, &http_port)) {
        ret_x(c, 4);
        return;
    }
    struct in_addr a;
    char host[256];
    snprintf(host, sizeof host, "%s", node ? node : "");
    for (char *p = host; *p; p++) *p = (char)(*p >= 'A' && *p <= 'Z' ? *p + 32 : *p);
    if (inet_pton(AF_INET, host, &a) == 1) {
    } else if (ends_with(host, ".nintendo.net") || ends_with(host, ".nintendo.com") || !strcmp(host, "localhost")) {
        a.s_addr = !strcmp(host, "localhost") ? htonl(INADDR_LOOPBACK) : ip;
    } else {
        rt_log("npln: GetAddrInfo %s: not a Nintendo server, not resolved", host);
        ret_x(c, 8);
        return;
    }
    unsigned port = service ? (unsigned)strtoul(service, NULL, 10) : 0;
    if (service && !strcmp(service, "https")) port = 443;
    if (port == 443 && a.s_addr == ip) port = kind == GAI_HTTP ? http_port : grpc_port;
    // The answers live in a ring of guest-visible blocks of the kernel region (it cannot free; FreeAddrInfo is a no-op):
    // gRPC and curl copy the address out before they ask again.
    static uint8_t *ring[32];
    static volatile int32_t next;
    int i = (int)((uint32_t)plat_inc32(&next) % 32);
    if (!ring[i]) ring[i] = mem_kernel_alloc(0x40, 16);
    uint8_t *ai = ring[i];
    memset(ai, 0, 0x40);
    const int32_t *hints = arg_p(c, 2);
    *(int32_t *)(ai + 4) = B_AF_INET;
    *(int32_t *)(ai + 8) = hints && hints[2] ? hints[2] : SOCK_STREAM;
    *(int32_t *)(ai + 12) = hints ? hints[3] : 0;
    *(uint32_t *)(ai + 0x10) = 16;
    *(uint64_t *)(ai + 0x20) = (uint64_t)(uintptr_t)(ai + 0x30);
    struct sockaddr_in h = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port), .sin_addr = a};
    uint32_t len = 16;
    addr_out(&h, ai + 0x30, &len);
    *out = (uint64_t)(uintptr_t)ai;
    char t[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &a, t, sizeof t);
    NTRACE("GetAddrInfo %s:%s -> %s:%u (%s)", host, service ? service : "", t, port, kind ? "curl" : "gRPC");
    ret_x(c, 0);
}
static void ov_getaddrinfo(Cpu *c) { getaddrinfo_npln(c, GAI_GRPC); }
static void ov_getaddrinfo_opts(Cpu *c) { getaddrinfo_npln(c, GAI_HTTP); }
static void ov_resolver_option(Cpu *c) { ret_x(c, 0); }

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
HOST_OVERRIDE("_ZN2nn6socket11GetAddrInfoEPKcS2_PK8addrinfoPPS3_", ov_getaddrinfo);
HOST_OVERRIDE("_ZN2nn6socket11GetAddrInfoEPKcS2_PKNS0_8AddrInfoEPPS3_", ov_getaddrinfo);
HOST_OVERRIDE("_ZN2nn6socket12FreeAddrInfoEP8addrinfo", ov_nothing);   // nothing to free: GetAddrInfo gives none
HOST_OVERRIDE("_ZN2nn6socket12FreeAddrInfoEPNS0_8AddrInfoE", ov_nothing);
// The C API's callers (gRPC, curl: musl code) read the error from musl's errno, a thread-local the SDK's nnsocket*
// set; GetLastErrno is only the C++ API's. So a failed C call also stores its errno there (__errno_location, the
// console's numbers: Linux's, as musl's are). Without it a nonblocking connect's EINPROGRESS read as a failure and gRPC
// closed the socket at once.
// rt_call leaves __errno_location's result in X0: the -1 the caller must see is put back
static void guest_errno(Cpu *c) {
    static uint64_t loc;
    if (!loc && !(loc = ldr_symbol("__errno_location"))) return;
    uint64_t ret = c->x[0];
    int32_t *e = (int32_t *)(uintptr_t)rt_call(c, loc, 0);
    c->x[0] = ret;
    if (e) *e = last_errno;
}
#define C_API(name, inner)                                                                                             \
    static void name(Cpu *c) {                                                                                         \
        inner(c);                                                                                                      \
        if ((int64_t)c->x[0] < 0) guest_errno(c);                                                                      \
    }
C_API(c_eventfd, ov_eventfd)
C_API(c_eventfd_read, ov_eventfd_read)
C_API(c_eventfd_write, ov_eventfd_write)

C_API(c_socket, ov_socket)
C_API(c_close, ov_close)
C_API(c_shutdown, ov_shutdown)
C_API(c_bind, ov_bind)
C_API(c_connect, ov_connect)
C_API(c_sock_refuse, ov_sock_refuse)
C_API(c_send, ov_send)
C_API(c_recv, ov_recv)
C_API(c_sendmsg, ov_sendmsg)
C_API(c_recvmsg, ov_recvmsg)
C_API(c_poll, ov_poll)
C_API(c_fcntl, ov_fcntl)
C_API(c_setsockopt, ov_setsockopt)
C_API(c_getsockopt, ov_getsockopt)
C_API(c_getsockname, ov_getsockname)
C_API(c_getpeername, ov_getpeername)

// eventfd: only gRPC's wakeup fd (musl code, reads errno) calls it
HOST_OVERRIDE("_ZN2nn6socket7EventFdEmNS0_12EventFdFlagsE", c_eventfd);
HOST_OVERRIDE("_ZN2nn6socket11EventFdReadEiPm", c_eventfd_read);
HOST_OVERRIDE("_ZN2nn6socket12EventFdWriteEim", c_eventfd_write);
// The C API, by its own names (see "stream sockets")
HOST_OVERRIDE("nnsocketSocket", c_socket);
HOST_OVERRIDE("nnsocketClose", c_close);
HOST_OVERRIDE("nnsocketShutdown", c_shutdown);
HOST_OVERRIDE("nnsocketBind", c_bind);
HOST_OVERRIDE("nnsocketConnect", c_connect);
HOST_OVERRIDE("nnsocketListen", c_sock_refuse);
HOST_OVERRIDE("nnsocketAccept", c_sock_refuse);
HOST_OVERRIDE("nnsocketSend", c_send);
HOST_OVERRIDE("nnsocketRecv", c_recv);
HOST_OVERRIDE("nnsocketSendMsg", c_sendmsg);
HOST_OVERRIDE("nnsocketRecvMsg", c_recvmsg);
HOST_OVERRIDE("nnsocketPoll", c_poll);
HOST_OVERRIDE("nnsocketFcntl", c_fcntl);
HOST_OVERRIDE("nnsocketSetSockOpt", c_setsockopt);
HOST_OVERRIDE("nnsocketGetSockOpt", c_getsockopt);
HOST_OVERRIDE("nnsocketGetSockName", c_getsockname);
HOST_OVERRIDE("nnsocketGetPeerName", c_getpeername);
HOST_OVERRIDE("nnsocketGetLastErrno", ov_last_errno);
HOST_OVERRIDE("nnsocketSetLastErrno", ov_set_errno);
HOST_OVERRIDE("nnsocketGetAddrInfo", ov_getaddrinfo);
HOST_OVERRIDE("nnsocketGetAddrInfoWithOptions", ov_getaddrinfo_opts);
HOST_OVERRIDE("nnsocketFreeAddrInfo", ov_nothing);
HOST_OVERRIDE("nnsocketResolverSetOption", ov_resolver_option);
HOST_OVERRIDE("nnsocketResolverGetOption", ov_resolver_option);
