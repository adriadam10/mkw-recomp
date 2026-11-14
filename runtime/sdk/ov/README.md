# runtime/sdk/ov

Replacements of `nn::*` functions by name: system, input, audio, video, graphics, network, time and touch.

| File | What it is |
|---|---|
| `ov_system.c` | Accounts, applets, time, system fonts, etc. |
| `ov_input.c`, `ov_media.c`, `ov_gfx.c`, `ov_zstd.c` | Input, Opus, the SDK's graphics (`nv::InitializeGraphics(0, 0)` is valid on Switch 2; `nv::InitCheck`), native zstd |
| `ov_codec.c` | MKW's video: the `nn::codec` video API (VP9 with libvpx, decoded in `Submit`) and the `nn::syncpt` events as the game's `SystemEvent` |
| `ov.h` | Header shared by the replacements and their test (`ov_probe.c`) |
| `ov_touch.c` | MKW's own: touch screen off (optional), reads without touches |
| `ov_oe2.c` | MKW's own: the `nn::oe` that totk-pc-recomp does not replace (version 1.0.0, graphics scale, notifications) |
| `ov_chat.c` | MKW 1.8.0's `nn::chat` (GameChat): a console where no chat is started; the room-info event is real and never signalled |
| `ov_net.c` | MKW's own network: `nifm`, `nsd`, `friends`, `ldn`, `prepo`, the network service account and `nn::socket` over the host's sockets (TCP and UDP, poll, eventfd). Off by default, as a console whose network is down. `MKW_ONLINE=1` with `MKW_ONLINE_SERVER`: online play on a private server (Nintendo's names resolve to it, its CA is the built-in one the game trusts); `MKW_LAN=1`: `nifm`'s local network mode (the LAN lobby), local network addresses only, a lobby broadcast also sent to `MKW_LAN_PEERS` |
| `ov_lanaddr.c` | MKW's own: the address and mask `nifm` reports in LAN mode: `MKW_LAN_IP`, else the route to the first of `MKW_LAN_PEERS`, else the `MKW_NETWORK` choice (home, Tailscale), else the best of the host's interfaces (the default route's network, another local network, link-local, a /32 last); Tailscale's 100.64.0.0/10 as its mask |
| `ov_socket.c` | MKW's own: the rules of LAN play (the sockets themselves are `ov_net.c`'s): local network addresses only without `MKW_ONLINE` (private, shared, link-local, multicast, broadcast), `MKW_LAN_PEERS` (the lobby's broadcasts also sent to each listed PC), a loopback `MKW_LAN_IP` bound for two instances on one PC |
| `net_online.c`, `net_online.h` | Online play on a server that speaks the game's protocol (the one built into mkw, `runtime/srv`, or another): the settings, this player's friend code (one per PC and server, `profile/online`), the server's CA, the identity token, the friends API over https with that CA pinned (WinHTTP on Windows); this PC's own server's CA read from its folder before any saved copy, a saved copy forgotten when the player presses Connect |
| `net_host.c` | This PC's networks (home network, Tailscale) and its own server: `mkw --server` (data in `server/` next to mkw), started hidden in a job object that stops it with mkw, a `server/` made on another PC set aside (this PC's server makes its own keys); the firewall rule for mkw.exe (local network and Tailscale only); the network error notices (the dialogs the console shows, for 10 s); the PC's id (a salted hash of the machine id) |
| `ov_time2.c` | MKW's own: the `nn::time` that totk-pc-recomp does not replace (exact network clock, host time zone, instant clock correction) |
