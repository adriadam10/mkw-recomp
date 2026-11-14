# tools/npln

A self-hosted server for Mario Kart World's online services (NPLN, Nintendo's gRPC platform), so the time trial's
"Ver clasificación" and "Subir fantasma" work between players who point their game at it. The runtime side is
`runtime/sdk/ov/ov_npln.c`: Settings > Game > Ghost server (`MKW_NPLN=<address>[:port]`).

The time trial's menu works against it: uploading a ghost, the leaderboards, and downloading another player's ghost
(tested on Windows, two profiles on one PC).

Between PCs: one runs the server (`uv run --with grpcio --with cryptography tools/npln/server.py`; its first line
gives the address to type), lets TCP ports 8443 and 8444 in (on Windows, allow Python on private networks when the
firewall asks), and every player types that address in Settings > Game > Ghost server, then restarts the game.

| File | What it is |
|---|---|
| `server.py` | The server: gRPC over TLS on the port (8443), plain HTTP on the next one (the CA for the runtime, the ghost files). Makes its own CA and certificate in `--data`. `uv run --with grpcio --with cryptography tools/npln/server.py` |
| `services.py` | The NPLN methods it answers (auth, leaderboard, ugcstore documents and uploads, friends, maintenance) and the store behind them; every call is logged with its decoded request |
| `pb.py` | Protobuf wire format by hand: decode without a schema, build messages from field numbers |
