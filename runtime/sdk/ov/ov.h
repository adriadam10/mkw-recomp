// Shared by the system overrides: runtime/ov_system.c, ov_input.c, ov_media.c and ov_gfx.c, and their probe.
// oe_push_message, oe_set_focus and ov_nv_state are in gfx.h (the window posts focus messages, the NVN layer
// allocates through the recorded callbacks).
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "gfx.h"

// ---- ov_system.c
int ov_handheld(void);                     // MKW_OPERATION_MODE=handheld (default docked); fatal on another value
const char *ov_language(void);             // the game's language, a Switch language code ("en-US", "zh-Hans", ...)
// A language the game does not ship whose texts are rebuilt over ov_language() ("ca", "gl", "eu"), else NULL:
// MKW_LANG names it, or unset, the locale's language is one
const char *ov_text_language(void);
const char *ov_shared_font_dir(char *buf, size_t n);   // MKW_SHARED_FONTS, else <romfs>/../../firmware/shared-fonts
uint64_t ov_bfttf_size(const uint8_t *p, uint64_t len); // decoded font size of a BFTTF image, 0 if it is not one

// ---- ov_socket.c
int ov_lan(void);                                    // MKW_LAN set: sockets and nifm's local mode are live
int ov_lan_only(uint32_t host_order);   // 1 for an IPv4 address that never leaves a local network (ov_socket.c)
int ov_lan_address(uint8_t ip[4], uint8_t mask[4]);   // MKW_LAN_IP, else the first interface up with a local address; -1 with none
int ov_sock_host(int fd, intptr_t *out, int *nonblock);   // the host socket of a guest descriptor; -1 if not open
#define OV_IO_AGAIN (-1)
#define OV_IO_ERROR (-2)
// One send (recv_side 0) or receive on descriptor fd for ov_ssl.c: bytes (0: closed), OV_IO_AGAIN or OV_IO_ERROR
int64_t ov_sock_io(int fd, void *buf, uint64_t n, int recv_side, int peek, int nonblock);

// ---- ov_npln.c: MKW_NPLN=<address>[:<port>], a self-hosted NPLN server (tools/npln/server.py) for the online ghosts
int ov_npln_on(void);
// 1 with the server's IPv4 (network order), gRPC port and HTTP port (gRPC + 1); 0 when MKW_NPLN is off or unusable.
// Any output may be NULL.
int ov_npln_server(uint32_t *ip, uint16_t *grpc_port, uint16_t *http_port);
// The server's CA certificate (DER), fetched from its HTTP port once; NULL when it cannot be had
const uint8_t *ov_npln_ca(uint32_t *size);
