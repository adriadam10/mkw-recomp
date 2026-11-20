"""GameBanana's connections race every resolved address (Happy Eyeballs).

CPython tries the addresses one after another with the full timeout each, which stalls for tens of seconds when
IPv6 is blackholed but IPv4 answers (or the reverse). No network: uv run pytest tests/launcher/test_gamebanana_eyeballs.py
"""

import http.client
import socket
import sys
import threading
import time
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/mod"))
import gamebanana  # noqa: E402


def test_race_connect_skips_dead_address(monkeypatch) -> None:
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(("127.0.0.1", 0))
    server.listen(1)
    port = server.getsockname()[1]
    accepted = threading.Event()

    def serve():
        conn, _ = server.accept()
        accepted.set()
        conn.close()

    worker = threading.Thread(target=serve, daemon=True)
    worker.start()
    dead = (socket.AF_INET, socket.SOCK_STREAM, socket.IPPROTO_TCP, "", ("127.0.0.1", 1))
    live = (socket.AF_INET, socket.SOCK_STREAM, socket.IPPROTO_TCP, "", ("127.0.0.1", port))
    monkeypatch.setattr(socket, "getaddrinfo", lambda *_, **__: [dead, live])

    conn = http.client.HTTPConnection("example.invalid", timeout=10)
    assert conn._create_connection is gamebanana._race_create_connection   # connect() itself stays CPython's
    conn.connect()
    try:
        assert accepted.wait(10)
        assert conn.sock.getpeername()[1] == port
    finally:
        conn.close()
        server.close()


def test_all_dead_fails_at_once(monkeypatch) -> None:
    dead = (socket.AF_INET, socket.SOCK_STREAM, socket.IPPROTO_TCP, "", ("127.0.0.1", 1))
    monkeypatch.setattr(socket, "getaddrinfo", lambda *_, **__: [dead, dead])
    start = time.monotonic()
    with pytest.raises(OSError):
        gamebanana._race_create_connection(("example.invalid", 80), 20)
    assert time.monotonic() - start < 5   # not the 20 s timeout


def test_timeout_covers_stagger_and_closes_late_connections(monkeypatch) -> None:
    release = threading.Event()
    sockets = []

    class SlowSocket:
        def __init__(self, *args):
            self.closed = threading.Event()
            sockets.append(self)

        def settimeout(self, timeout):
            pass

        def connect(self, address):
            release.wait(5)

        def close(self):
            self.closed.set()

    addr = (socket.AF_INET, socket.SOCK_STREAM, socket.IPPROTO_TCP, "", ("127.0.0.1", 1))
    monkeypatch.setattr(socket, "getaddrinfo", lambda *_, **__: [addr] * 4)
    monkeypatch.setattr(socket, "socket", SlowSocket)
    start = time.monotonic()
    try:
        with pytest.raises(TimeoutError):
            gamebanana._race_create_connection(("example.invalid", 80), 0.05)
        elapsed = time.monotonic() - start
    finally:
        release.set()
    assert all(s.closed.wait(2) for s in sockets)
    assert elapsed < 0.5  # the old stagger alone took 0.75 s for four addresses
