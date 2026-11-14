# tools/npln

A self-hosted server for Mario Kart World's online services (NPLN, Nintendo's gRPC platform), so the time trial's
"Ver clasificación" and "Subir fantasma" work between players who point their game at it. The runtime side is
`runtime/sdk/ov/ov_npln.c` (`MKW_NPLN=<address>[:port]`).

Work in progress: the game reaches the server and starts TLS; the gRPC handshake does not complete yet (see the
status in `server.py`'s history and the pull request).

| File | What it is |
|---|---|
| `server.py` | The server: gRPC over TLS on the port (8443), plain HTTP on the next one (the CA for the runtime, the ghost files). Makes its own CA and certificate in `--data`. `uv run --with grpcio --with cryptography tools/npln/server.py` |
| `services.py` | The NPLN methods it answers (auth, leaderboard, ugcstore documents and uploads, friends, maintenance) and the store behind them; every call is logged with its decoded request |
| `pb.py` | Protobuf wire format by hand: decode without a schema, build messages from field numbers |
