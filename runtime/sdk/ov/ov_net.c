// nn::nifm for MKW: the console's network connection manager. By default it answers as a console with no network (the
// runtime serves no network service): a request "completes" without a connection, so the game takes its offline path.
// MKW_LAN=1 (Settings > Game > LAN play, off by default): a request in local network mode, the one the game
// makes for its LAN lobby (SetLocalNetworkMode(mode == 3) at main+0x2247a74, then IsNetworkAvailable), is answered as
// connected, with the host's address (ov_lan_address); Pia then talks over the host's sockets (ov_socket.c). An
// internet request stays unavailable: there is no server to reach.
#include <stdint.h>
#include <string.h>

#include "host.h"
#include "ov.h"

// nifm results (module 110); the description number is a hypothesis: any failure keeps the game offline.
#define NIFM_R_NO_NETWORK (110u | (1u << 9))

static int local_mode;   // SetLocalNetworkMode's last value

static int lan_up(void) { return local_mode && ov_lan(); }
// MKW_NPLN (ov_npln.c): an internet request is connected too, as the self-hosted server is the internet the game sees
static int net_up(void) { return lan_up() || (!local_mode && ov_npln_on()); }

static void ov_ok(Cpu *c) { ret_x(c, 0); }
static void ov_nothing(Cpu *c) { (void)c; }
// void SetLocalNetworkMode(bool)
static void ov_local_mode(Cpu *c) { local_mode = (int)(arg_x(c, 0) & 1); }
// bool IsNetworkAvailable()
static void ov_network_available(Cpu *c) { ret_x(c, (uint64_t)net_up()); }
// Result HandleNetworkRequestErrorResult()
static void ov_request_result(Cpu *c) { ret_x(c, net_up() ? 0 : NIFM_R_NO_NETWORK); }
static void ov_no_network(Cpu *c) { ret_x(c, NIFM_R_NO_NETWORK); }
// Result GetCurrentIpConfigInfo(in_addr *ip, in_addr *mask, in_addr *gateway, in_addr *dns1, in_addr *dns2)
static void ov_ip_config(Cpu *c) {
    uint8_t ip[4], mask[4], gw[4];
    if (!net_up() || ov_lan_address(ip, mask)) {
        ret_x(c, NIFM_R_NO_NETWORK);
        return;
    }
    for (int i = 0; i < 4; i++) gw[i] = ip[i] & mask[i];
    gw[3] |= 1;   // a LAN needs no gateway; the usual first address, for whoever prints it
    const uint8_t *v[5] = {ip, mask, gw, gw, gw};
    for (int i = 0; i < 5; i++)
        if (arg_p(c, i)) memcpy(arg_p(c, i), v[i], 4);
    static int logged;
    if (!logged++) rt_log("nifm: LAN mode, address %u.%u.%u.%u mask %u.%u.%u.%u (MKW_LAN)", ip[0], ip[1], ip[2], ip[3],
                          mask[0], mask[1], mask[2], mask[3]);
    ret_x(c, 0);
}

HOST_OVERRIDE("_ZN2nn4nifm10InitializeEv", ov_ok);
HOST_OVERRIDE("_ZN2nn4nifm18IsNetworkAvailableEv", ov_network_available);
HOST_OVERRIDE("_ZN2nn4nifm19SetLocalNetworkModeEb", ov_local_mode);
HOST_OVERRIDE("_ZN2nn4nifm20CancelNetworkRequestEv", ov_nothing);
HOST_OVERRIDE("_ZN2nn4nifm27SubmitNetworkRequestAndWaitEv", ov_nothing);
HOST_OVERRIDE("_ZN2nn4nifm31HandleNetworkRequestErrorResultEv", ov_request_result);
HOST_OVERRIDE("_ZN2nn4nifm22GetCurrentIpConfigInfoEP7in_addrS2_S2_S2_S2_", ov_ip_config);
HOST_OVERRIDE("_ZN2nn4nifm22GetCurrentProxySettingEPNS0_12ProxySettingE", ov_no_network);

// ---- nn::friends: no friend list offline; play history registration is accepted and dropped.
#define FRIENDS_R_UNAVAILABLE (121u | (1u << 9))   // module 121; description is a hypothesis
// Result CheckFriendListAvailability(bool *out, const Uid &)
static void ov_friend_list_available(Cpu *c) {
    uint8_t *out = arg_p(c, 0);
    if (out) *out = 0;
    ret_x(c, 0);
}
// Result GetFriendList(int *count, Friend *out, const Uid &, int offset, int max, const FriendFilter &)
static void ov_friend_list(Cpu *c) {
    int32_t *count = arg_p(c, 0);
    if (count) *count = 0;
    ret_x(c, 0);
}
static void ov_friends_unavailable(Cpu *c) { ret_x(c, FRIENDS_R_UNAVAILABLE); }

HOST_OVERRIDE("_ZN2nn7friends10InitializeEv", ov_nothing);
HOST_OVERRIDE("_ZN2nn7friends12SetAllocatorEPFPvmEPFvS1_mE", ov_nothing);
HOST_OVERRIDE("_ZN2nn7friends27CheckFriendListAvailabilityEPbRKNS_7account3UidE", ov_friend_list_available);
HOST_OVERRIDE("_ZN2nn7friends13GetFriendListEPiPNS0_6FriendERKNS_7account3UidEiiRKNS0_12FriendFilterE", ov_friend_list);
HOST_OVERRIDE("_ZN2nn7friends25EnsureFriendListAvailableEPNS0_12AsyncContextERKNS_7account3UidE", ov_friends_unavailable);
HOST_OVERRIDE("_ZN2nn7friends29GetPlayHistoryRegistrationKeyEPNS0_26PlayHistoryRegistrationKeyERKNS_7account3UidEb",
              ov_friends_unavailable);
HOST_OVERRIDE("_ZN2nn7friends14AddPlayHistoryERKNS_7account3UidERKNS0_26PlayHistoryRegistrationKeyERKNS0_15InAppScreenNameESA_",
              ov_ok);

// ---- nn::ldn (local wireless play): disabled, as on a console in airplane mode. Initialize fails, so the game does
// not use the rest; they fail too in case it does.
#define LDN_R_DISABLED (203u | (22u << 9))   // module 203; description is a hypothesis
static void ov_ldn_disabled(Cpu *c) { ret_x(c, LDN_R_DISABLED); }
// State GetState(): 0, None
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

// ---- nn::nsd: no server name resolution offline. With MKW_NPLN, the production environment ("lp1", as the NPLN host
// names carry it) and every name resolved to itself (ov_socket.c's GetAddrInfo sends Nintendo's names to the server).
static void ov_nsd_none(Cpu *c) { ret_x(c, NIFM_R_NO_NETWORK); }
// Result GetEnvironmentIdentifier(EnvironmentIdentifier *out): char[8]
static void ov_nsd_env(Cpu *c) {
    char *out = arg_p(c, 0);
    if (!ov_npln_on() || !out) {
        ov_nsd_none(c);
        return;
    }
    memset(out, 0, 8);
    memcpy(out, "lp1", 3);
    ret_x(c, 0);
}
// Result ResolveEx(Fqdn *out, const Fqdn &in): char[256] each
static void ov_nsd_resolve(Cpu *c) {
    char *out = arg_p(c, 0);
    const char *in = arg_p(c, 1);
    if (!ov_npln_on() || !out || !in) {
        ov_nsd_none(c);
        return;
    }
    if (out != in) memmove(out, in, 256);
    out[255] = 0;
    ret_x(c, 0);
}
HOST_OVERRIDE("_ZN2nn3nsd24GetEnvironmentIdentifierEPNS0_21EnvironmentIdentifierE", ov_nsd_env);
HOST_OVERRIDE("_ZN2nn3nsd9ResolveExEPNS0_4FqdnERKS1_", ov_nsd_resolve);

// ---- nn::prepo: play reports are built by the SDK's own code and never sent.
HOST_OVERRIDE("_ZN2nn5prepo28RequestImmediateTransmissionEv", ov_ok);

// ---- nn::socket: ov_socket.c, over the host's sockets (it also covers #63: Initialize needs no bsd service).
