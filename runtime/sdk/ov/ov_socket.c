// The rules of LAN play (the sockets themselves: ov_net.c; the address nifm reports: ov_lanaddr.c): MKW's LAN play is
// the game's own network library (Pia, linked in main: nn::pia::lan) over nn::socket, which ov_net.c answers with the
// host's sockets. LAN Play finds games with UDP broadcasts on the local network (ports 49152 and 35000).
//
// MKW_LAN on, LAN only: without MKW_ONLINE, Bind, Connect and SendTo refuse any address outside a local network
// (ov_lan_local), and GetAddrInfo resolves nothing, so no datagram can go to a server on the internet.
// MKW_LAN_PEERS: the PCs of a virtual LAN that carries no broadcast (Tailscale, ZeroTier, a VPN), as IPv4 addresses or
// names, separated by commas or spaces. A broadcast of the lobby also goes to each of them (ov_lan_peers), and the LAN
// address is the one of the route to the first (its 100.x Tailscale address, ov_lanaddr.c), so the others can answer.
// MKW_LAN_IP in 127.0.0.0/8: sockets bound to any address bind to it instead, so two instances on one PC (127.0.0.1
// and 127.0.0.2, each the other's MKW_LAN_PEERS) each get their own port 49152 (tests).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#include "cfg.h"
#include "host.h"
#include "ov.h"
#include "platform/platform.h"

// LAN only: addresses that never leave a local network, so LAN play cannot reach any server on the internet
// (Nintendo's or another), whatever the game asks: any (bind), loopback, the private ranges of RFC 1918, shared
// address space (RFC 6598, the virtual LANs of Tailscale and the like), link-local, multicast and broadcast.
int ov_lan_only(uint32_t h) {   // h: the IPv4 address in host order
    return h == 0 || (h >> 24) == 127 || (h >> 24) == 10 || (h >> 20) == 0xac1 || (h >> 16) == 0xc0a8 ||
           (h >> 22) == (100u << 2 | 1) || (h >> 16) == 0xa9fe || (h >> 28) == 0xe || h == 0xffffffffu;
}
int ov_lan_local(uint32_t addr) { return ov_lan_only(ntohl(addr)); }

int ov_lan(void) { return cfg_bool("MKW_LAN"); }

static int net_startup(void) {
#ifdef _WIN32
    static int ready;
    WSADATA wsa;
    if (!ready && WSAStartup(MAKEWORD(2, 2), &wsa)) return -1;
    ready = 1;
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
