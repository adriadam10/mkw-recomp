// The LAN address nifm reports (ov_net.c ov_ip_config), split from ov_socket.c.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "cfg.h"
#include "host.h"
#include "net_online.h"
#include "ov.h"

static int lan_only(struct in_addr a) { return ov_lan_only(ntohl(a.s_addr)); }

static int is_tailscale(struct in_addr a) { return (ntohl(a.s_addr) >> 22) == (100u << 2 | 1); }   // 100.64.0.0/10

// This PC's address on the route to peer (nothing is sent: a datagram socket's connect only picks the route)
static uint32_t route_to(uint32_t peer) {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa)) return 0;
    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == INVALID_SOCKET) return 0;
    int len;
#else
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return 0;
    socklen_t len;
#endif
    struct sockaddr_in d = {.sin_family = AF_INET, .sin_port = htons(49152)}, me = {0};
    d.sin_addr.s_addr = peer;
    len = sizeof me;
    uint32_t r = 0;
    if (!connect(s, (struct sockaddr *)&d, sizeof d) && !getsockname(s, (struct sockaddr *)&me, &len))
        r = me.sin_addr.s_addr;
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
    return r;
}

// ---- the LAN address nifm reports (ov_net.c): MKW_LAN_IP; else, with MKW_LAN_PEERS (a virtual LAN without
// broadcast), the address on the route to the first of them; else the address of the network MKW_NETWORK names (home,
// tailscale: the Online page's choice); else the best interface that is up with a local network address (lan_only,
// not loopback), read from the system's interface list (nothing is sent), with its mask. Pia builds its broadcast from
// both. An address of Tailscale's 100.64.0.0/10 gets that whole range as its mask: Tailscale gives each PC a /32 that
// holds no other PC, and with the range the others are on the same network for Pia (a local network below, not a /32).
// Best first (a tie keeps the first in the system's order):
//   3  a network with the default route: the one the PC reaches its router and the other PCs through
//   2  another local network: a virtual machine's host-only adapter (VirtualBox 192.168.56.1, Hyper-V), a cable
//      between two PCs
//   1  a link-local 169.254.x.x address: an adapter that found no DHCP server
//   0  a /32 mask (a VPN such as Tailscale gives one): Pia refuses it with error 2318-0007 (#148, reproduced with
//      tools/run/lan2.sh at LAN2_PREFIX=32)
// Discussion 148 (two Windows PCs that could not see each other): one took 169.254.83.107, the other VirtualBox's
// 192.168.56.1, both ahead of the home network in the adapter list.
static int lan_pick(struct in_addr a, struct in_addr want) {
    return want.s_addr ? a.s_addr == want.s_addr : (ntohl(a.s_addr) >> 24) != 127 && lan_only(a) && a.s_addr;
}

static int lan_score(struct in_addr x, struct in_addr xm, int default_route) {
    if (xm.s_addr == 0xffffffffu) return 0;
    if ((ntohl(x.s_addr) >> 16) == 0xa9fe) return 1;
    return default_route ? 3 : 2;
}

typedef struct {
    struct in_addr a, m;
    int score, seen;
} LanBest;

static void lan_offer(LanBest *b, struct in_addr x, struct in_addr xm, int default_route, const char *ifname) {
    if (is_tailscale(x)) xm.s_addr = htonl(0xffc00000u);   // its /32 widened to the range (above)
    int s = lan_score(x, xm, default_route);
    static int logged;   // the candidates of the first look, for a report
    if (logged < 8) {
        char t[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &x, t, sizeof t);
        uint32_t bits = 0;
        for (uint32_t v = ntohl(xm.s_addr); v & 0x80000000u; v <<= 1) bits++;
        rt_log("nifm: LAN candidate %s/%u on %s: %s", t, (unsigned)bits, ifname,
               s == 3 ? "default route" : s == 2 ? "local network" : s == 1 ? "link-local" : "/32");
        logged++;
    }
    if (!b->seen || s > b->score) b->a = x, b->m = xm, b->score = s, b->seen = 1;
}

#ifndef _WIN32
// 1 when ifname carries the default route (/proc/net/route: a line with destination 00000000)
static int linux_default_route(const char *ifname) {
    FILE *f = fopen("/proc/net/route", "r");
    if (!f) return 0;
    char line[256], name[64];
    unsigned dest;
    int found = 0;
    while (!found && fgets(line, sizeof line, f))
        found = sscanf(line, "%63s %x", name, &dest) == 2 && dest == 0 && !strcmp(name, ifname);
    fclose(f);
    return found;
}
#endif

int ov_lan_address(uint8_t ip[4], uint8_t mask[4]) {
    struct in_addr want = {0}, a = {0}, m = {0};
    LanBest best = {0};
    const char *e = cfg_get("MKW_LAN_IP");
    if (e && *e && inet_pton(AF_INET, e, &want) != 1) rt_log("nifm: MKW_LAN_IP %s is not an IPv4 address", e);
    uint32_t first;
    if (!want.s_addr && ov_lan_peer_first(&first) && !(want.s_addr = route_to(first)))
        rt_log("nifm: no route to the first of MKW_LAN_PEERS");
    if (!want.s_addr && strcmp(net_network(), "auto")) {   // MKW_NETWORK home or tailscale: that network's address
        NetAddrs na;
        net_addrs(&na);
        const char *ip = net_network_address(&na);
        if (*ip) inet_pton(AF_INET, ip, &want);
        else rt_log("nifm: the %s network is not up", net_network());
    }
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa)) return -1;
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER |
                        GAA_FLAG_INCLUDE_GATEWAYS;
    ULONG size = 16 << 10;
    IP_ADAPTER_ADDRESSES *list = malloc(size);
    if (list && GetAdaptersAddresses(AF_INET, flags, NULL, list, &size) == ERROR_BUFFER_OVERFLOW) {
        free(list);
        list = malloc(size);
        if (list && GetAdaptersAddresses(AF_INET, flags, NULL, list, &size) != NO_ERROR) free(list), list = NULL;
    }
    for (IP_ADAPTER_ADDRESSES *i = list; i; i = i->Next) {
        if (i->OperStatus != IfOperStatusUp || i->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        char name[64];
        snprintf(name, sizeof name, "%ls", i->FriendlyName ? i->FriendlyName : L"?");
        for (IP_ADAPTER_UNICAST_ADDRESS *u = i->FirstUnicastAddress; u; u = u->Next) {
            struct in_addr x = ((struct sockaddr_in *)u->Address.lpSockaddr)->sin_addr, xm;
            if (!lan_pick(x, want)) continue;
            uint8_t bits = u->OnLinkPrefixLength > 32 ? 24 : u->OnLinkPrefixLength;
            xm.s_addr = htonl(bits ? 0xffffffffu << (32 - bits) : 0);
            lan_offer(&best, x, xm, i->FirstGatewayAddress != NULL, name);
        }
    }
    free(list);
#else
    struct ifaddrs *list, *i;
    if (!getifaddrs(&list)) {
        for (i = list; i; i = i->ifa_next)
            if (i->ifa_addr && i->ifa_netmask && i->ifa_addr->sa_family == AF_INET && (i->ifa_flags & IFF_UP) &&
                lan_pick(((struct sockaddr_in *)i->ifa_addr)->sin_addr, want))
                lan_offer(&best, ((struct sockaddr_in *)i->ifa_addr)->sin_addr,
                          ((struct sockaddr_in *)i->ifa_netmask)->sin_addr, linux_default_route(i->ifa_name),
                          i->ifa_name);
        freeifaddrs(list);
    }
#endif
    if (best.seen) a = best.a, m = best.m;
    if (!a.s_addr && want.s_addr) a = want, m.s_addr = htonl(0xffffff00u);   // MKW_LAN_IP not on an interface: /24
    if (a.s_addr && is_tailscale(a)) m.s_addr = htonl(0xffc00000u);   // the range, even when picked by want
    if (!a.s_addr) {
        rt_log("nifm: no network interface with a local network address: no LAN");
        return -1;
    }
    static int warned;
    if (m.s_addr == 0xffffffffu && !warned++) {
        char t[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &a, t, sizeof t);
        rt_log("nifm: %s has a /32 mask and no other interface has a local network address: the game refuses it "
               "(error 2318-0007); LAN play needs a network with a broadcast address, or MKW_LAN_PEERS", t);
    }
    ov_lan_set_broadcast(a.s_addr | ~m.s_addr);
    memcpy(ip, &a, 4);
    memcpy(mask, &m, 4);
    return 0;
}
