"""tools/prepare/libs.py against a local HTTP server: a good file lands beside the program, a damaged one is refused
and leaves nothing behind, and a file the player already put there is kept."""

import hashlib
import http.server
import importlib.util
import sys
import threading
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
GOOD = b"MZ" + bytes(range(256)) * 40


def load_libs():
    sys.path.insert(0, str(ROOT / "tools/prepare"))
    spec = importlib.util.spec_from_file_location("libs", ROOT / "tools/prepare/libs.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


@pytest.fixture
def server(tmp_path):
    served = tmp_path / "served"
    served.mkdir()
    (served / "good.dll").write_bytes(GOOD)
    (served / "bad.dll").write_bytes(GOOD[:-1] + b"X")

    class Quiet(http.server.SimpleHTTPRequestHandler):
        def __init__(self, *a, **k):
            super().__init__(*a, directory=str(served), **k)

        def log_message(self, *a):
            pass

    httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Quiet)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    yield f"http://127.0.0.1:{httpd.server_address[1]}"
    httpd.shutdown()


def run(libs, monkeypatch, tmp_path, base, capsys, *argv):
    table = tmp_path / "libs.tsv"
    digest = hashlib.sha256(GOOD).hexdigest()
    table.write_text(
        "feature\tos\tfile\tbytes\tsha256\turl\n"
        f"good\twindows\tgood.dll\t{len(GOOD)}\t{digest}\t{base}/good.dll\n"
        f"bad\twindows\tbad.dll\t{len(GOOD)}\t{digest}\t{base}/bad.dll\n"
    )
    monkeypatch.setattr(libs, "TABLE", table)
    monkeypatch.setattr(libs, "ssl_context", lambda: None)
    monkeypatch.setattr(sys, "argv", ["libs.py", *argv, "--os", "windows"])
    code = libs.main()
    return code, capsys.readouterr().out.splitlines()


def test_fetch_good_bad_and_kept(server, monkeypatch, tmp_path, capsys):
    libs = load_libs()
    dest = tmp_path / "game"
    code, out = run(libs, monkeypatch, tmp_path, server, capsys, "fetch", "--features", "good", "--dir", str(dest))
    assert code == 0 and out[-2:] == ["ready good.dll", "done"]
    assert (dest / "good.dll").read_bytes() == GOOD

    code, out = run(libs, monkeypatch, tmp_path, server, capsys, "fetch", "--features", "bad", "--dir", str(dest))
    assert code == 1 and out[-1].startswith("error fetch bad.dll came damaged")
    assert sorted(p.name for p in dest.iterdir()) == ["good.dll"]

    (dest / "good.dll").write_bytes(b"newer, put there by the player")
    code, out = run(
        libs, monkeypatch, tmp_path, server, capsys, "missing", "--features", "good,bad", "--dir", str(dest)
    )
    assert out == [f"need bad bad.dll {len(GOOD)}", "done"]
    code, out = run(libs, monkeypatch, tmp_path, server, capsys, "fetch", "--features", "good", "--dir", str(dest))
    assert out == ["done"] and (dest / "good.dll").read_bytes() == b"newer, put there by the player"


def test_shipped_table_is_well_formed():
    libs = load_libs()
    rows = libs.table()
    assert {r.feature for r in rows} == {"dlss", "dlssg", "fsr3", "fsr3fg", "renhdrum"}
    for r in rows:
        assert r.url.startswith("https://") and len(r.sha256) == 64 and r.size > 0


def test_fetch_retries_transient_network_error(server, monkeypatch, tmp_path, capsys):
    libs = load_libs()
    real_urlopen = libs.urllib.request.urlopen
    attempts = 0

    def flaky_urlopen(*a, **k):
        nonlocal attempts
        attempts += 1
        if attempts == 1:
            raise OSError("temporary DNS failure")
        return real_urlopen(*a, **k)

    monkeypatch.setattr(libs.urllib.request, "urlopen", flaky_urlopen)
    monkeypatch.setattr(libs.time, "sleep", lambda _: None)
    dest = tmp_path / "game"
    code, out = run(libs, monkeypatch, tmp_path, server, capsys, "fetch", "--features", "good", "--dir", str(dest))
    assert code == 0 and attempts == 2 and out[-2:] == ["ready good.dll", "done"]
    assert (dest / "good.dll").read_bytes() == GOOD
