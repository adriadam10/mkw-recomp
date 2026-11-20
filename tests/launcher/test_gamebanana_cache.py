"""The Mods page can recover a truncated GameBanana cache entry without waiting for it to expire.

Run the CLI entry point with synthetic cached responses and no network: uv run pytest tests/launcher/test_gamebanana_cache.py
"""

import hashlib
import io
import json
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/mod"))
import gamebanana  # noqa: E402


@pytest.fixture
def profile_request(tmp_path: Path, monkeypatch: pytest.MonkeyPatch):
    mod_id = "123"
    url = f"{gamebanana.API}/Mod/{mod_id}/ProfilePage"
    key = tmp_path / (hashlib.sha256(url.encode()).hexdigest()[:24] + ".json")
    data = {"_aGame": {"_idRow": gamebanana.GAME}, "_sName": "Example mod"}
    monkeypatch.setattr(sys, "argv", ["gamebanana.py", "--cache", str(tmp_path), "info", mod_id])
    return url, key, data


@pytest.mark.parametrize("broken", [b'{"_aGame":', b"\xff"], ids=["truncated-json", "invalid-utf8"])
def test_info_replaces_unreadable_json_cache(
    profile_request, monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str], broken: bytes,
) -> None:
    url, key, data = profile_request
    key.write_bytes(broken)
    requests = []

    def fetch(request, *, timeout):
        requests.append(request.full_url)
        return io.BytesIO(json.dumps(data).encode())

    monkeypatch.setattr(gamebanana.urllib.request, "urlopen", fetch)

    assert gamebanana.main() == 0
    assert capsys.readouterr().out.startswith("info\t123\tExample mod\t")
    assert requests == [url]
    assert json.loads(key.read_bytes()) == data

    assert gamebanana.main() == 0
    assert capsys.readouterr().out.startswith("info\t123\tExample mod\t")
    assert requests == [url]


def test_failed_cache_replace_preserves_old_entry(
    profile_request, monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str],
) -> None:
    _, key, data = profile_request
    previous = b'{"_aGame":'
    key.write_bytes(previous)
    monkeypatch.setattr(gamebanana.urllib.request, "urlopen", lambda *_, **__: io.BytesIO(json.dumps(data).encode()))
    replace = Path.replace

    def fail_cache_replace(temp, target):
        if target == key:
            assert temp.parent == key.parent
            assert json.loads(temp.read_bytes()) == data
            assert key.read_bytes() == previous
            raise PermissionError("cache entry is locked")
        return replace(temp, target)

    monkeypatch.setattr(Path, "replace", fail_cache_replace)

    assert gamebanana.main() == 1
    assert capsys.readouterr().out == "error\tnet\tcache entry is locked\n"
    assert key.read_bytes() == previous
    assert {p.name for p in key.parent.iterdir()} == {key.name, "requests"}


def test_image_cache_recovers_from_failed_write(tmp_path, monkeypatch, capsys):
    url = "https://images.gamebanana.com/img/synthetic.png"
    calls = []

    def fetch(*args, **kwargs):
        calls.append(1)
        return io.BytesIO(b"complete image")

    monkeypatch.setattr(gamebanana, "throttle", lambda _: None)
    monkeypatch.setattr(gamebanana.urllib.request, "urlopen", fetch)
    write = Path.write_bytes

    def fail_once(path, data):
        monkeypatch.setattr(Path, "write_bytes", write)
        write(path, data[:3])
        raise OSError("interrupted write")

    monkeypatch.setattr(Path, "write_bytes", fail_once)
    assert gamebanana.images(tmp_path, [url]) == 0
    assert capsys.readouterr().out == ""
    assert gamebanana.images(tmp_path, [url]) == 0
    assert len(calls) == 2
    cached = list((tmp_path / "img").glob("*.png"))
    assert len(cached) == 1 and cached[0].read_bytes() == b"complete image"
