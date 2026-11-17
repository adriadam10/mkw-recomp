"""Phones as controllers (runtime/input/phonepad_*.c) and the QR code of their address (runtime/core/base/qr.c):
tests/c/t_phonepad.c built with the C compiler at hand, then its checks (packets, the HTTP head, the motion's axes,
the QR code against python-qrcode's rows, the certificate) and its server on 127.0.0.1, driven here as a phone's
browser drives it: plain sockets, and TLS that checks the server's own certificate for the address.

Why a real server: what a phone meets is the sum of the parts (a request cut in two, several on one connection, a
browser that came without https://, a phone that goes silent), and none of it needs SDL, the game or a phone.
BearSSL's objects are kept in build/test between runs (294 files; the key is their contents and the compiler).

Usage: uv run pytest tests/test_phonepad.py     (CC names another compiler: CC="zig cc")
"""

import concurrent.futures
import hashlib
import os
import queue
import shlex
import shutil
import socket
import ssl
import struct
import subprocess
import threading
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
BEARSSL = ROOT / "runtime/third_party/bearssl"
PAGE = b"<!doctype html><title>phone controller test page</title>"   # phonepad_srv.c under PHONEPAD_TEST_PAGE
CODE = "4821"
A, RSTICK = 1 << 0, 1 << 15   # phonepad_proto.h PPAD_A, PPAD_RSTICK


def compiler() -> list[str] | None:
    if os.environ.get("CC"):
        return shlex.split(os.environ["CC"], posix=os.name != "nt")
    cc = shutil.which("clang") or shutil.which("cc") or shutil.which("gcc")
    if not cc:
        return None
    # A clang that targets MSVC (the Windows CI runner's) is not a compiler the runtime is ever built with (Windows
    # is llvm-mingw), and BearSSL does not build under its <intrin.h> (the fork's CI, 14/11/2026).
    machine = subprocess.run([cc, "-dumpmachine"], capture_output=True, text=True, check=False).stdout
    return None if "msvc" in machine else [cc]


def bearssl_objects(cc: list[str]) -> list[Path]:
    """BearSSL's objects, compiled once per compiler and source tree into build/test."""
    sources = sorted((BEARSSL / "src").rglob("*.c"))
    key = hashlib.sha256(" ".join(cc).encode())
    key.update(subprocess.run([*cc, "--version"], capture_output=True, check=True).stdout)
    for path in [*sources, *sorted(BEARSSL.rglob("*.h"))]:
        key.update(path.read_bytes())
    out = ROOT / "build/test" / f"bearssl-{key.hexdigest()[:16]}"
    objects = [out / (src.stem + ".o") for src in sources]
    if not (out / "done").exists():
        out.mkdir(parents=True, exist_ok=True)
        flags = ["-O1", f"-I{BEARSSL / 'inc'}", f"-I{BEARSSL / 'src'}", "-c"]

        def build(pair: tuple[Path, Path]) -> None:
            subprocess.run([*cc, *flags, str(pair[0]), "-o", str(pair[1])], check=True)

        with concurrent.futures.ThreadPoolExecutor(os.cpu_count() or 2) as pool:
            list(pool.map(build, zip(sources, objects)))
        (out / "done").write_text("")
    return objects


@pytest.fixture(scope="module")
def exe(tmp_path_factory: pytest.TempPathFactory) -> Path:
    cc = compiler()
    if not cc:
        pytest.skip("no C compiler the runtime is built with")
    tmp = tmp_path_factory.mktemp("phonepad")
    rsp = tmp / "bearssl.rsp"   # 294 objects: past what a Windows command line takes
    rsp.write_text("\n".join(f'"{o.as_posix()}"' for o in bearssl_objects(cc)))
    out = tmp / "t_phonepad.exe"
    libs = ["-lws2_32", "-ladvapi32"] if os.name == "nt" else ["-lm"]
    sources = ["tests/c/t_phonepad.c", "runtime/input/phonepad_srv.c", "runtime/input/phonepad_cert.c",
               "runtime/core/base/qr.c"]  # fmt: skip
    subprocess.run([*cc, "-O1", "-std=gnu11", "-DPHONEPAD_TEST_PAGE", f"-I{ROOT / 'runtime/input'}",
                    f"-I{ROOT / 'runtime/core/base'}", f"-I{BEARSSL / 'inc'}", *(str(ROOT / s) for s in sources),
                    f"@{rsp}", "-o", str(out), *libs], check=True)  # fmt: skip
    return out


def test_checks(exe: Path) -> None:
    out = subprocess.run([str(exe)], capture_output=True, text=True, check=False)
    assert out.returncode == 0 and " 0 failed" in out.stdout, out.stdout


class Server:
    """t_phonepad serve: its events as lines (ready, attach, state, detach, log)."""

    def __init__(self, exe: Path, tls: bool, gone_ms: int = 0) -> None:
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            self.port = probe.getsockname()[1]
        self.proc = subprocess.Popen([str(exe), "serve", str(self.port), CODE, str(int(tls)), str(gone_ms)],
                                     stdout=subprocess.PIPE, text=True)  # fmt: skip
        self.lines: queue.Queue[str] = queue.Queue()
        threading.Thread(target=lambda: [self.lines.put(line.rstrip("\n")) for line in self.proc.stdout],
                         daemon=True).start()  # fmt: skip
        ready = self.event("ready").split()
        self.cert = bytes.fromhex(ready[2]) if tls else b""

    def event(self, kind: str, timeout: float = 5) -> str:
        """The next line of that kind; the server's log lines in between are skipped."""
        while True:
            try:
                line = self.lines.get(timeout=timeout)
            except queue.Empty:
                raise AssertionError(f"no '{kind}' line from the server in {timeout} s") from None
            if line.startswith(kind + " "):
                return line
            assert line.startswith("log "), f"expected '{kind}', the server said: {line}"

    def quiet(self, seconds: float = 0.3) -> None:
        """Nothing but log lines for that long."""
        try:
            while True:
                line = self.lines.get(timeout=seconds)
                assert line.startswith("log "), f"unexpected: {line}"
        except queue.Empty:
            pass

    def close(self) -> None:
        self.proc.kill()
        self.proc.wait()


class Client:
    """One connection, kept open, as a browser keeps it."""

    def __init__(self, server: Server, tls: bool = False) -> None:
        self.sock = socket.create_connection(("127.0.0.1", server.port), timeout=5)
        if tls:
            # the server's certificate is the only one trusted, and it has to name the address
            ctx = ssl.create_default_context(cadata=ssl.DER_cert_to_PEM_cert(server.cert))
            ctx.verify_flags &= ~ssl.VERIFY_X509_STRICT   # a certificate that is its own issuer has no key identifiers
            self.sock = ctx.wrap_socket(self.sock, server_hostname="127.0.0.1")
        self.buf = b""

    def send(self, method: str, target: str, body: bytes = b"", extra: str = "") -> None:
        head = f"{method} {target} HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: {len(body)}\r\n{extra}\r\n"
        self.sock.sendall(head.encode() + body)

    def answer(self) -> tuple[int, dict[str, str], bytes]:
        while b"\r\n\r\n" not in self.buf:
            more = self.sock.recv(65536)
            assert more, "the server closed the connection"
            self.buf += more
        head, _, self.buf = self.buf.partition(b"\r\n\r\n")
        lines = head.decode().split("\r\n")
        headers = {k.lower(): v.strip() for k, v in (line.split(":", 1) for line in lines[1:])}
        size = int(headers["content-length"])
        while len(self.buf) < size:
            self.buf += self.sock.recv(65536)
        body, self.buf = self.buf[:size], self.buf[size:]
        return int(lines[0].split()[1]), headers, body

    def request(self, method: str, target: str, body: bytes = b"") -> tuple[int, dict[str, str], bytes]:
        self.send(method, target, body)
        return self.answer()

    def closed(self) -> bool:
        try:
            return self.sock.recv(1) == b""
        except (ConnectionError, ssl.SSLError):
            return True

    def join(self, ident: str) -> str:
        status, _, body = self.request("POST", f"/join?code={CODE}&id={ident}")
        assert status == 200 and len(body) == 16, (status, body)
        return body.decode()


def packet(buttons: int = 0, sticks: tuple[int, int, int, int] = (0, 0, 0, 0), acc: tuple[int, int, int] | None = None,
           gyro: tuple[int, int, int] = (0, 0, 0)) -> bytes:  # fmt: skip
    """phonepad_proto.h's packet; acc in 1/500 m/s^2 and gyro in 1/1000 rad/s, or no sensors."""
    return struct.pack("<BBH4h3h3h", 1, 1 if acc else 0, buttons, *sticks, *(acc or (0, 0, 0)), *gyro)


@pytest.fixture
def plain(exe: Path):
    server = Server(exe, tls=False, gone_ms=1500)
    yield server
    server.close()


@pytest.fixture
def secure(exe: Path):
    server = Server(exe, tls=True)
    yield server
    server.close()


def test_a_phone_plays(plain: Server) -> None:
    c = Client(plain)
    status, headers, body = c.request("GET", "/")
    assert (status, body) == (200, PAGE) and headers["content-type"].startswith("text/html")
    assert c.request("GET", "/nothing")[0] == 404
    assert c.request("POST", "/join?code=0000&id=ab")[0] == 403
    sid = c.join("ab")
    assert plain.event("attach") == "attach 0 127.0.0.1"
    status, _, body = c.request("POST", f"/i?s={sid}", packet(A | RSTICK, (1234, -5, 32767, -32768), (-500, 1000, 4903), (1000, -2000, 0)))
    assert (status, body) == (200, bytes([0x10, 0x80, 1, 0]))   # rumble low and high (A is held), player 1
    assert plain.event("state") == "state 0 8001 1234 -5 32767 -32767 1 -1.000 2.000 9.806 1.000 -2.000 0.000"
    assert c.request("POST", f"/i?s={sid}", packet())[2] == bytes([0, 0, 1, 0])   # A let go: the motors stop
    assert plain.event("state") == "state 0 0000 0 0 0 0 0 0.000 0.000 0.000 0.000 0.000 0.000"
    assert c.request("POST", "/i?s=0123456789abcdef", packet())[0] == 410   # a session the server does not know: join again
    assert c.request("POST", f"/i?s={sid}", packet()[:-1])[0] == 400
    assert c.join("ab") == sid   # the same page again (reloaded): the same controller
    plain.quiet()
    assert c.request("POST", f"/bye?s={sid}")[0] == 204
    assert plain.event("detach") == "detach 0"
    assert c.request("POST", f"/i?s={sid}", packet())[0] == 410


def test_a_silent_phone(plain: Server) -> None:
    c = Client(plain)
    sid = c.join("cd")
    plain.event("attach")
    c.request("POST", f"/i?s={sid}", packet(A, (20000, 0, 0, 0)))
    assert plain.event("state").startswith("state 0 0001 20000 ")
    # 0.7 s without a packet: every button released, the stick centred; then (gone_ms 1500) its controller goes
    assert plain.event("state", timeout=2) == "state 0 0000 0 0 0 0 0 0.000 0.000 0.000 0.000 0.000 0.000"
    assert plain.event("detach", timeout=3) == "detach 0"
    assert c.request("POST", f"/i?s={sid}", packet())[0] == 410


def test_requests_as_they_arrive(plain: Server) -> None:
    c = Client(plain)
    sid = c.join("ef")
    plain.event("attach")
    # two requests in one write: answered in order
    one = f"POST /i?s={sid} HTTP/1.1\r\nContent-Length: 24\r\n\r\n".encode() + packet(A)
    c.sock.sendall(one + one)
    assert c.answer()[0] == 200 and c.answer()[0] == 200
    plain.event("state"), plain.event("state")
    # one request in three writes, cut in the head and in the body
    c.sock.sendall(one[:20])
    plain.quiet(0.1)
    c.sock.sendall(one[20:-7])
    plain.quiet(0.1)
    c.sock.sendall(one[-7:])
    assert c.answer()[0] == 200
    plain.event("state")
    # Connection: close is honoured
    c.send("GET", "/", extra="Connection: close\r\n")
    status, headers, _ = c.answer()
    assert status == 200 and headers["connection"] == "close" and c.closed()
    # not HTTP: 400, and the connection goes
    bad = Client(plain)
    bad.sock.sendall(b"hello\r\n\r\n")
    assert bad.answer()[0] == 400 and bad.closed()


def test_who_may_join(plain: Server) -> None:
    c = Client(plain)
    for n in range(8):   # eight controllers, as many as players
        c.join(f"a{n}")
        assert plain.event("attach") == f"attach {n} 127.0.0.1"
    assert c.request("POST", f"/join?code={CODE}&id=a8")[0] == 503
    for _ in range(8):   # eight wrong codes: nobody joins for ten seconds, not even with the right one
        assert c.request("POST", "/join?code=9999&id=b0")[0] == 403
    assert c.request("POST", f"/join?code={CODE}&id=a0")[0] == 429


def test_https(secure: Server) -> None:
    # a browser given the address without https:// is sent to it
    c = Client(secure)
    c.send("GET", "/")
    status, headers, _ = c.answer()
    assert status == 301 and headers["location"] == "https://127.0.0.1/" and c.closed()
    # TLS, the certificate checked against the address
    t = Client(secure, tls=True)
    assert t.sock.version() == "TLSv1.2"
    assert t.request("GET", "/")[2] == PAGE
    sid = t.join("ab")
    assert secure.event("attach") == "attach 0 127.0.0.1"
    for n in range(50):   # a second of play on the one connection
        status, _, body = t.request("POST", f"/i?s={sid}", packet(A, (n * 100, 0, 0, 0)))
        assert (status, body) == (200, bytes([0x10, 0x80, 1, 0]))
        assert secure.event("state").startswith(f"state 0 0001 {n * 100} ")
    # a phone that turns the certificate down ends the handshake halfway: the others play on
    with socket.create_connection(("127.0.0.1", secure.port)) as refused:
        refused.sendall(b"\x16\x03\x01\x00\x05junk!")
    assert t.request("POST", f"/i?s={sid}", packet())[0] == 200
    secure.event("state")
    other = Client(secure, tls=True)
    other.join("cd")
    assert secure.event("attach", timeout=5) == "attach 1 127.0.0.1"
