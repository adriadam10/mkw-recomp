// Online play on a private server that speaks the game's online protocol (the one built into mkw, runtime/srv, or
// another): the settings, this player's friend code, the server's certificate and its friends API. Shared by the
// game's network overrides (ov_net.c) and the start screen's Online page (launcher).
//
// Settings (mkw.ini [online] or the environment):
//   MKW_ONLINE=1           sockets, nifm and nsd work (online play with a server, the game's hidden LAN play)
//   MKW_ONLINE_SERVER=<ipv4 or name>[:<port>]   the server; Nintendo's host names resolve to it and
//                          connections to its port 443 go to <port> (8443 by default)
//   MKW_ONLINE_CA=<file>   its certificate (ca.der or ca.pem); else the one saved in profile/online, else Droplet's
// The friend code and secret of each server live in <exe dir>/profile/online/<server>.txt, in Droplet's format, so
// the server sees the same kind of identity token as from Droplet.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int on;                 // a valid server is set
    uint8_t ip[4];
    uint16_t port;          // where its port 443 goes
    char text[160];         // the setting as written
} NetServer;

int net_enabled(void);                          // MKW_ONLINE
const NetServer *net_server(void);              // parsed once per settings change; on 0 when unset or invalid
int net_redirected_host(const char *host);      // a Nintendo name that goes to the server
uint32_t net_local_ip(void);                    // this PC's IPv4 toward the server (network order), 0 unknown

#define NET_MAX_CODES 100
typedef struct {
    char code[13];          // 12 digits
    char secret[65];        // hex
    char friends[NET_MAX_CODES][13];
    int nfriends;
    char removed[NET_MAX_CODES][13];
    int nremoved;
    char machine[17];       // the PC it was made on (net_machine_id), "" in a file from before it
} NetProfile;
// The profile of server text (made with a new friend code when create and none exists). 1 when loaded.
int net_profile_load(const char *server, NetProfile *p, int create);
int net_profile_save(const char *server, const NetProfile *p);
void net_format_code(const char *code, char out[18]);          // "SW-1234-5678-9012"
int net_parse_code(const char *text, char out[13]);            // any spelling of 12 digits -> 1

// The identity token the game sends at sign-in. build: the main module's build ID in hex, may be "". 0 when off.
int net_identity_token(const char *build, char *out, size_t cap);

// The server's certificate, DER, from MKW_ONLINE_CA, the saved copy or Droplet's; NULL when none (free it).
uint8_t *net_server_ca(const char *server, size_t *len);
// Fetches it from the server (https on port + 1, /v1/ca.der) and saves it; "" on success, else why not.
const char *net_fetch_ca(const char *server, char *err, size_t cap);
// The saved certificate of server forgotten: the next call trusts what the server hands out (a player's Connect)
void net_forget_ca(const char *server);

// The friends API (the server's https port + 1, the server's certificate pinned).
typedef struct { char code[13]; char name[64]; int online; } NetFriend;
typedef struct {
    int reached;            // the server answered
    char error[200];
    char name[64];          // this player's name on the server (the game's, after a first sign-in)
    NetFriend friends[NET_MAX_CODES], received[NET_MAX_CODES], sent[NET_MAX_CODES];
    int nfriends, nreceived, nsent;
} NetFriendLists;
void net_friends_get(const char *server, NetFriendLists *out);
// op: request, accept, decline, cancel, remove. "" on success, else why not (from the server when it said).
const char *net_friends_do(const char *server, const char *op, const char *code, char *err, size_t cap);

// A quick check of the server from this PC: "" when its online port answers, else why not.
const char *net_probe(const char *server, char *err, size_t cap);

// ---- this PC's networks and its own server (net_host.c)
// MKW_NETWORK=auto|home|tailscale: the networks this PC plays on; MKW_SERVER_HOST=1: mkw runs its built-in server
// (mkw --server, data in server/ next to it), which stops when mkw closes.
typedef struct { char home[16], ts[16]; } NetAddrs;   // "" when that network is not up
void net_addrs(NetAddrs *a);                    // the home network's (the adapter with the default route), Tailscale's
const char *net_network(void);                  // "auto", "home" or "tailscale"
const char *net_network_address(const NetAddrs *a);   // the chosen network's address of this PC, "" when none
int net_is_this_pc(uint32_t ip);                // ip (network order) is one of this PC's addresses or loopback
#define NET_HOST_PORT 8443
typedef enum { NET_HOST_OFF, NET_HOST_STARTING, NET_HOST_RUNNING, NET_HOST_FAILED, NET_HOST_OTHER } NetHostState;
int net_host_enabled(void);                     // MKW_SERVER_HOST
int net_host_available(void);                   // this build can start the server (Windows)
// 0 when it runs (started now or before by this process); data made on another PC is set aside first
int net_host_start(char *err, size_t cap);
void net_host_stop(void);
// RUNNING/STARTING: detail = its addresses; FAILED: why; OTHER: a server this process did not start answers
NetHostState net_host_state(char *detail, size_t cap);
int net_host_listen_changed(void);              // ours listens on other addresses than the network choice's now
int net_host_firewall(void);                    // lets the server in from the local network and Tailscale (Windows asks)
// The network error dialog the console would show, as a 10-second notice with what to check (category 0: no network)
void net_error_notice(unsigned category, unsigned number);
void net_host_boot(void);                       // the game without the start screen: starts it when set and not yet
// This PC, as a hash of the system's machine id: a friend code, a nickname or a server made on another PC (a
// copied folder) is not this one's
const char *net_machine_id(void);               // 16 hex digits

#ifdef __cplusplus
}
#endif
