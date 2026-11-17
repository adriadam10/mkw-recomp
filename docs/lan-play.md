# LAN play between two machines

LAN play turns the title menu's Wireless Play into the game's own LAN mode: two machines on one local network play a
race between themselves, with no Nintendo account, no server and no internet on either side. It is off by default
because it had not been played between two PCs ([#144](https://github.com/adriadam10/mkw-recomp/issues/144)). This
page is the recipe it was played with, and what the two machines needed.

The switch is Settings > General > LAN play, or `MKW_LAN=1`, or `lan = 1` in `mkw.ini`; it is read at start
(`runtime/core/base/cfg.c`), so the game is restarted after changing it. With it on, the `nifm` request the game
makes for its LAN lobby is answered as connected, with this machine's own IPv4 address and mask
(`SetLocalNetworkMode(3)` then `IsNetworkAvailable`, `runtime/sdk/ov/ov_net.c`), and the game's sockets become real
UDP sockets that reach only local network addresses — private ranges, link-local, multicast and broadcast — whatever
the game asks (`runtime/sdk/ov/ov_socket.c`). An internet request still comes back as "no network": there is no
server here, and none is contacted.

## What the two machines need

- **One local network, seen both ways.** Pia, the game's network library, looks for a room in broadcast: it builds
  the broadcast address from the address and the mask that `nifm` reports (`runtime/sdk/ov/ov_lanaddr.c`), so both
  machines must be on the same subnet and reach each other's broadcasts. Two PCs on one switch or access point is the
  case that was tried; a router between them, or client isolation on the access point, was not.
- **A mask with a broadcast address.** The runtime takes an interface with a local network first, and falls back to a
  `/32` — what a VPN's adapter often is — only when nothing else is left: there is then no broadcast to send to, and
  LAN play stops with `2318-0007` (`runtime/sdk/ov/ov_lanaddr.c`; reproduced on Linux in #148, and #245 made a real
  network win over an adapter that comes first in the system's list). The log says which case it is: `nifm: … has a
  /32 mask and no other interface has a local network address`.
- **UDP 49152 and 35000**, the two ports the game's LAN play uses (the `#144` entry in
  [CHANGELOG.md](../CHANGELOG.md) names them), open in and out on both machines' firewalls.
- **An address each, and different.** With more than one adapter, `MKW_LAN_IP=<a.b.c.d>` says which address `nifm`
  reports; without it the runtime prefers the interface that carries the default route, then any other interface with
  a local address, then a link-local one, and an address that is on no interface is taken as a `/24`
  (`runtime/sdk/ov/ov_lanaddr.c`). The log says what was picked: `nifm: LAN mode, address … mask … (MKW_LAN)`, after
  the `nifm: LAN candidate …` lines it chose from.

## Playing

1. On each machine, `MKW_LAN=1` (or the menu row) and the game started; `MKW_LAN_IP` when the machine has more than
   one adapter.
2. At the title screen, **Online** > **LAN Play** on both.
3. One machine creates a room; the other sees it in the list and joins it.
4. Start the race.

Nothing is shared over the network: each machine keeps its own profile and its own pipeline cache. Both machines ran
the same build in the test below, one Windows 11 PC and one Linux PC.

## A machine with an integrated GPU (UMA)

The memory VMA may allocate is capped. On an integrated GPU, with `MKW_GPU_MEM_LIMIT` unset, the cap is 40 % of the
machine's RAM in 256 MiB steps, 4096 MiB at least, plus what the firmware carved out for the GPU
(`vk_default_mem_limit_mib` in `runtime/gfx/vk/device/vk_defaults.c`), split over the heaps by size
(`runtime/gfx/vk/device/vk_device.c`). That is a share of the very RAM the game is living in, and on a 14 GiB
machine it was too much for a LAN race:

| `MKW_GPU_MEM_LIMIT` | cap, heap 0 + heap 1 | GTT in use | RAM available | the race |
|---|---|---|---|---|
| unset | 2 133 + 4 266 = 6 399 MiB | 7 179 MiB | **0 kB** | `2318-0006` on both sides |
| `4096` | 1 365 + 2 730 = 4 095 MiB | 4 905 MiB | 2.4 GB | starts and is played |
| `3072` | not recorded | 3 068-3 104 MiB | 3 268 MiB | two races, no error |

The failure looked like a network one and was not: both machines showed `err: game error dialog 2318-0006 (result
0xd3e)` at the same moment, while the UMA machine sat at `MemAvailable 0 kB`, 5.3 GiB of swap in use and 7 179 MiB
of GTT taken. The circuit's new textures and geometry went to swap, the load stretched out, and the session ended.
One side failing alone is what a blocked port looks like; **the same error on both sides at the same moment is the
session out of time** — on a machine whose GPU shares the RAM with the game, read the memory before the network.

How to set it: the log prints the default it computed (`vk: integrated GPU, N MiB carved out of RAM by the
firmware: memory cap M MiB`) and then one line per heap (`vk: heap 0 limited to … MiB (MKW_GPU_MEM_LIMIT)`); put a
`MKW_GPU_MEM_LIMIT` below the share that leaves the game's own working set alone. On Linux the host side that goes
with it is `vm.swappiness` and a reboot, so that a swap the earlier attempts filled starts empty: the run that gave
two clean races had `MKW_GPU_MEM_LIMIT=3072`, `vm.swappiness=100` (it was 180) and a freshly booted machine. Which of
the three mattered how much is not measured — `4096` with the older swappiness already started a race.

The machine in the table: AMD BC-250 (GFX1013, Mesa RADV, Linux), 14 GiB of RAM shared with the GPU, `mem: class uma
(integrated gpu)` in its log. Its GTT use is what the kernel reports in
`/sys/class/drm/card*/device/mem_info_gtt_used`.

## Two instances on one machine

They do not find each other. `tools/run/lan2.sh` runs the two instances in two network namespaces joined by a veth
pair, each with its own address, because two instances side by side in the host's network report the same address
and bind the same Pia port. Two adapters do not replace that: with `nifm` reporting `.51` on one instance and `.106`
on the other, neither room appeared in either list — Pia finds rooms in broadcast, and here the broadcast sent by one
instance never arrived at the other. So: two machines, or `tools/run/lan2.sh` on one Linux machine. Windows has no
equivalent of that script.

## Not tried

- More than two machines, and two machines racing for longer than two races.
- A virtual LAN between two sites (ZeroTier and the like): it would have to carry the broadcast, and it was not
  tried — this page says nothing about whether one works.
- Two machines on different versions of the runtime.
