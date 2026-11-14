"""A self-hosted NPLN server for mkw-recomp's online time trials: the ranking ("Ver clasificación"), ghost uploads
("Subir fantasma") and downloads, between players who point their game at it (Settings > Game > Ghost server, or
MKW_NPLN=<address>[:<port>]).

Why: Mario Kart World's online services are NPLN, Nintendo's gRPC platform. The runtime (runtime/sdk/ov/ov_npln.c)
sends the game's NPLN traffic here instead of to Nintendo, and makes the game's TLS trust this server's own CA, so the
game's unmodified client talks to it. Nothing here comes from Nintendo: the messages are protobuf, encoded and decoded
by hand from field numbers learned from the game's traffic and public notes (docs/research.md, NPLN).

It serves:
  <port>      gRPC over TLS (HTTP/2), with a certificate for *.npln.srv.nintendo.net signed by its CA
  <port + 1>  plain HTTP: /ca.der (the CA the runtime installs in the game) and the ghost files (UGC uploads and
              downloads; the runtime sends libcurl here without TLS)
Data (CA, certificate, scores, ghosts) lives in --data (default tools/npln/data, never committed).

Usage: uv run --with grpcio --with cryptography tools/npln/server.py [--host 0.0.0.0] [--port 8443] [--data DIR]
Every call is logged with its decoded request, so a call the server does not know yet shows what the game asked.
"""

import argparse
import datetime
import http.server
import ipaddress
import logging
import os
import socket
import sys
import threading
from concurrent import futures
from pathlib import Path

import grpc
from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import rsa
from cryptography.x509.oid import NameOID

sys.path.insert(0, str(Path(__file__).resolve().parent))
import pb  # noqa: E402  (tools/npln/pb.py: protobuf wire format)
import services  # noqa: E402  (tools/npln/services.py: the NPLN methods)

log = logging.getLogger("npln")


# ---- TLS: a CA of our own and a server certificate for Nintendo's NPLN names, made once
def make_certs(data: Path) -> tuple[bytes, bytes, bytes]:
    """(CA DER, server certificate chain PEM, server key PEM), created in data on first use. RSA, as Nintendo's own:
    with ECDSA (P-256) the game's OpenSSL left the handshake right after the server's certificate."""
    ca_key_p, ca_p, key_p, crt_p = (data / n for n in ("ca.key", "ca.der", "server.key", "server.pem"))
    if not ca_p.exists():
        now = datetime.datetime.now(datetime.UTC)
        ca_key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
        name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "mkw-recomp NPLN CA")])
        ca = (
            x509.CertificateBuilder().subject_name(name).issuer_name(name).public_key(ca_key.public_key())
            .serial_number(x509.random_serial_number()).not_valid_before(now - datetime.timedelta(days=1))
            .not_valid_after(now + datetime.timedelta(days=3650))
            .add_extension(x509.BasicConstraints(ca=True, path_length=0), critical=True)
            .add_extension(x509.KeyUsage(digital_signature=True, key_cert_sign=True, crl_sign=True,
                                         content_commitment=False, key_encipherment=False, data_encipherment=False,
                                         key_agreement=False, encipher_only=False, decipher_only=False), critical=True)
            .sign(ca_key, hashes.SHA256())
        )  # fmt: skip
        key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
        names = ["*.lp1.t.npln.srv.nintendo.net", "*.t.npln.srv.nintendo.net", "*.npln.srv.nintendo.net",
                 "npln.srv.nintendo.net", "*.srv.nintendo.net", "localhost"]  # fmt: skip
        crt = (
            x509.CertificateBuilder()
            .subject_name(x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "*.lp1.t.npln.srv.nintendo.net")]))
            .issuer_name(name).public_key(key.public_key()).serial_number(x509.random_serial_number())
            .not_valid_before(now - datetime.timedelta(days=1)).not_valid_after(now + datetime.timedelta(days=3650))
            .add_extension(x509.SubjectAlternativeName([x509.DNSName(n) for n in names]
                                                       + [x509.IPAddress(ipaddress.ip_address("127.0.0.1"))]),
                           critical=False)
            .add_extension(x509.ExtendedKeyUsage([x509.oid.ExtendedKeyUsageOID.SERVER_AUTH]), critical=False)
            .sign(ca_key, hashes.SHA256())
        )  # fmt: skip
        pem = serialization.Encoding.PEM
        nokey = serialization.NoEncryption()
        ca_key_p.write_bytes(ca_key.private_bytes(pem, serialization.PrivateFormat.PKCS8, nokey))
        key_p.write_bytes(key.private_bytes(pem, serialization.PrivateFormat.PKCS8, nokey))
        crt_p.write_bytes(crt.public_bytes(pem) + ca.public_bytes(pem))
        ca_p.write_bytes(ca.public_bytes(serialization.Encoding.DER))
        log.info("made a new CA and server certificate in %s", data)
    return ca_p.read_bytes(), crt_p.read_bytes(), key_p.read_bytes()


# ---- gRPC: one generic handler; requests and responses stay bytes, services.py decodes and builds them
class Handler(grpc.GenericRpcHandler):
    def __init__(self, store: "services.Store"):
        self.store = store

    def service(self, details):
        method = details.method
        meta = dict(details.invocation_metadata)

        def unary(req: bytes, ctx):
            return services.call(self.store, method, meta, req, ctx)

        def stream(req: bytes, ctx):
            yield from services.call_stream(self.store, method, meta, req, ctx)

        if method in services.SERVER_STREAMS:
            return grpc.unary_stream_rpc_method_handler(stream)
        return grpc.unary_unary_rpc_method_handler(unary)


# ---- HTTP: the CA and the UGC files
class Http(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    store: "services.Store"
    ca: bytes

    def _send(self, code: int, body: bytes = b"", ctype: str = "application/octet-stream") -> None:
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def _body(self) -> bytes:
        n = int(self.headers.get("Content-Length") or 0)
        return self.rfile.read(n) if n else b""

    def do_GET(self):  # noqa: N802
        if self.path == "/ca.der":
            return self._send(200, self.ca)
        data = self.store.blob_get(self.path)
        log.info("HTTP GET %s -> %s", self.path, f"{len(data)} bytes" if data is not None else "missing")
        self._send(200, data) if data is not None else self._send(404)

    do_HEAD = do_GET

    def do_PUT(self):  # noqa: N802
        body = self._body()
        ok = self.store.blob_put(self.path, body)
        log.info("HTTP PUT %s, %d bytes -> %s", self.path, len(body), "stored" if ok else "refused")
        self._send(200 if ok else 403)

    do_POST = do_PUT

    def log_message(self, fmt, *args):
        log.debug("http: " + fmt, *args)


def local_address() -> str:
    """The address other machines reach this one at (the route to the internet's interface), for the start line."""
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.connect(("192.0.2.1", 9))  # TEST-NET: no packet is sent for a datagram connect
            return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=8443, help="gRPC port; HTTP is the next one")
    ap.add_argument("--data", type=Path, default=Path(__file__).resolve().parent / "data")
    ap.add_argument("-v", "--verbose", action="store_true")
    ap.add_argument("--external-tls", action="store_true", help="plain gRPC on 127.0.0.1:<port + 2>, TLS done in front")
    args = ap.parse_args()
    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO,
                        format="%(asctime)s %(levelname)s %(message)s", datefmt="%H:%M:%S")  # fmt: skip
    args.data.mkdir(parents=True, exist_ok=True)
    ca, chain, key = make_certs(args.data)
    public = args.host if args.host not in ("0.0.0.0", "") else local_address()
    store = services.Store(args.data, f"http://{public}:{args.port + 1}")

    Http.store, Http.ca = store, ca
    httpd = http.server.ThreadingHTTPServer((args.host, args.port + 1), Http)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()

    server = grpc.server(futures.ThreadPoolExecutor(max_workers=16), handlers=[Handler(store)])
    if args.external_tls:  # TLS done in front (an experiment's proxy): plain gRPC on the loopback, port + 2
        server.add_insecure_port(f"127.0.0.1:{args.port + 2}")
    else:
        server.add_secure_port(f"{args.host}:{args.port}", grpc.ssl_server_credentials([(key, chain)]))
    server.start()
    log.info("NPLN server ready: in the game set Ghost server (MKW_NPLN) to %s:%d", public, args.port)
    log.info("gRPC/TLS on %s:%d, HTTP on %s:%d, data in %s", args.host, args.port, args.host, args.port + 1, args.data)
    try:
        server.wait_for_termination()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    os.environ.setdefault("GRPC_VERBOSITY", "ERROR")

    main()
