# /// script
# dependencies = []
# ///
"""A fake Cemuhook / DSU server: synthetic motion for MKW_DSU=1 (runtime/input/dsu.c) without a gyro controller.

Why: the DSU client is checked in seconds by `mkw --probe-dsu`, but seeing the kart steer needs the game, and not
everyone has a DSU server and a controller with a gyro at hand. This answers the client's pad data requests on
127.0.0.1:26760 (as DS4Windows or BetterJoy would) with one slot: a controller held flat that rolls left and right
(a sine, --amplitude degrees each way every --period seconds), 250 samples a second, for as long as the client keeps
asking. The game's log shows "dsu: slot 0 sends ... samples/s" and "input: player 1 motion from DSU slot 0".

The protocol: https://v1993.github.io/cemuhook-protocol/ (version 1001; docs/research.md). Axes as dsu.c reads them:
x right, y up out of the face, z away from the player, acceleration as gravity reads (flat: y = -1 G), gyro pitch,
yaw, roll in deg/s (roll right positive).

Usage: uv run tools/test/dsu_server.py [--port 26760] [--slot 0] [--amplitude 30] [--period 4]
  then run the game with MKW_DSU=1 (Settings > Controls > Motion from a DSU server) and steer with motion on.
"""

import argparse
import math
import socket
import struct
import time
import zlib

PAD_DATA = 0x100002


def packet(slot: int, number: int, stamp_us: int, acc: tuple, gyro: tuple) -> bytes:
    body = struct.pack("<I", PAD_DATA)
    body += bytes([slot, 2, 2, 1]) + bytes(6) + bytes([5])  # connected, full gyro, USB, no MAC, battery full
    body += bytes([1]) + struct.pack("<I", number) + bytes(4) + bytes([128] * 4) + bytes(12) + bytes(12)
    body += struct.pack("<Q3f3f", stamp_us, *acc, *gyro)
    head = b"DSUS" + struct.pack("<HHII", 1001, len(body), 0, 0x4D4B5744)
    crc = zlib.crc32(head + body)
    return b"DSUS" + struct.pack("<HHII", 1001, len(body), crc, 0x4D4B5744) + body


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--port", type=int, default=26760)
    ap.add_argument("--slot", type=int, default=0)
    ap.add_argument("--amplitude", type=float, default=30, help="roll each way, degrees")
    ap.add_argument("--period", type=float, default=4, help="seconds for a full left-right-left roll")
    args = ap.parse_args()
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("127.0.0.1", args.port))
    sock.settimeout(0.004)
    print(f"dsu_server: 127.0.0.1:{args.port}, slot {args.slot}, roll ±{args.amplitude}° every {args.period} s")
    client, asked, number, t0 = None, 0.0, 0, time.monotonic()
    while True:
        try:
            data, addr = sock.recvfrom(128)
            if len(data) >= 20 and data[:4] == b"DSUC" and struct.unpack_from("<I", data, 16)[0] == PAD_DATA:
                if client != addr:
                    print(f"dsu_server: client {addr[0]}:{addr[1]}")
                client, asked = addr, time.monotonic()
        except TimeoutError:
            pass
        now = time.monotonic()
        if client is None or now - asked > 5:  # stop a few seconds after the last request, as servers do
            continue
        t = now - t0
        w = 2 * math.pi / args.period
        angle = math.radians(args.amplitude) * math.sin(w * t)  # roll, right side down positive
        rate = args.amplitude * w * math.cos(w * t)  # deg/s
        acc = (math.sin(angle), -math.cos(angle), 0.0)  # gravity as read in the rolled controller's axes
        sock.sendto(packet(args.slot, number, int(t * 1e6), acc, (0.0, 0.0, rate)), client)
        number += 1


if __name__ == "__main__":
    main()
