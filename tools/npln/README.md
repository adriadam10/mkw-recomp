# tools/npln

A self-hosted server for Mario Kart World's online services (NPLN, Nintendo's gRPC platform), so the time trial's
"Ver clasificación" and "Subir fantasma" work between players who point their game at it. The runtime side is
`runtime/sdk/ov/ov_npln.c`: Settings > Game > Ghost server (`MKW_NPLN=<address>[:port]`).

The time trial's menu works against it: uploading a ghost, the leaderboards, and downloading another player's ghost
(tested on Windows, two profiles on one PC).

Between PCs: one runs the server (`uv run --with grpcio --with cryptography tools/npln/server.py`; its first line
gives the address to type), lets TCP ports 8443 and 8444 in (on Windows, allow Python on private networks when the
firewall asks), and every player types that address in Settings > Game > Ghost server, then restarts the game.

On a cloud server (any Linux VM with a public IP, or a domain name pointing at it): copy `tools/npln/` there, open
TCP ports 8443 and 8444 in the provider's firewall (security group), and run it with uv:

```sh
curl -LsSf https://astral.sh/uv/install.sh | sh
uv run --with grpcio --with cryptography npln/server.py --data ~/npln-data
```

Players type the VM's public IP or domain (`ghosts.example.org`, `ghosts.example.org:8443` or a pasted
`https://ghosts.example.org:8443/`). The ghost files' URLs name a Nintendo host that the runtime sends to the
server the player typed, so the server needs no setting for its own address. What it does not have yet for a server
open to anyone: accounts (a player is who their game says it is, so a leaderboard can be faked) and a check of the
CA beyond its first download over plain HTTP.

| File | What it is |
|---|---|
| `server.py` | The server: gRPC over TLS on the port (8443), plain HTTP on the next one (the CA for the runtime, the ghost files). Makes its own CA and certificate in `--data`. `uv run --with grpcio --with cryptography tools/npln/server.py` |
| `services.py` | The NPLN methods it answers (auth, leaderboard, ugcstore documents and uploads, friends, maintenance) and the store behind them; every call is logged with its decoded request |
| `pb.py` | Protobuf wire format by hand: decode without a schema, build messages from field numbers |
