"""GameBanana's list and info lines carry the content rating's visibility.

Run the CLI entry point with synthetic responses and no network: uv run pytest tests/launcher/test_gamebanana_nsfw.py
"""

import io
import json
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/mod"))
import gamebanana  # noqa: E402


def record(mod_id, vis, rated):
    return {
        "_idRow": mod_id,
        "_sModelName": "Mod",
        "_sName": f"Mod {mod_id}",
        "_aSubmitter": {"_sName": "Author"},
        "_nLikeCount": 1,
        "_nViewCount": 2,
        "_tsDateAdded": 1,
        "_aRootCategory": {"_sName": "Driver Skins"},
        "_sProfileUrl": f"https://gamebanana.com/mods/{mod_id}",
        "_sInitialVisibility": vis,
        "_bHasContentRatings": rated,
        "_aPreviewMedia": {"_aImages": []},
    }


def test_browse_row_carries_visibility(monkeypatch: pytest.MonkeyPatch, tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    data = {"_aRecords": [record(1, "show", False), record(736713, "warn", True)], "_aMetadata": {"_nRecordCount": 2}}
    monkeypatch.setattr(sys, "argv", ["gamebanana.py", "--cache", str(tmp_path), "browse", "--page", "1"])
    monkeypatch.setattr(gamebanana.urllib.request, "urlopen", lambda *_, **__: io.BytesIO(json.dumps(data).encode()))
    assert gamebanana.main() == 0
    lines = capsys.readouterr().out.splitlines()
    assert lines[0].split("\t")[-2:] == ["show", "0"]
    assert lines[1].split("\t")[-2:] == ["warn", "1"]


def test_hidden_row_is_marked_hide(monkeypatch: pytest.MonkeyPatch, tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    data = {"_aRecords": [record(736713, "hide", True)], "_aMetadata": {"_nRecordCount": 1}}
    monkeypatch.setattr(sys, "argv", ["gamebanana.py", "--cache", str(tmp_path), "browse", "--page", "1"])
    monkeypatch.setattr(gamebanana.urllib.request, "urlopen", lambda *_, **__: io.BytesIO(json.dumps(data).encode()))
    assert gamebanana.main() == 0
    assert capsys.readouterr().out.splitlines()[0].split("\t")[-2:] == ["hide", "1"]


@pytest.mark.parametrize(("vis", "ratings"), [("warn", {"st": "Sexual Themes"}), ("show", {})])
def test_info_carries_ratings(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path, capsys: pytest.CaptureFixture[str], vis: str, ratings: dict,
) -> None:
    data = {
        "_aGame": {"_idRow": gamebanana.GAME},
        "_sName": "Example mod",
        "_sInitialVisibility": vis,
        "_aContentRatings": ratings,
        "_aFiles": [],
        "_aPreviewMedia": {"_aImages": []},
    }
    monkeypatch.setattr(sys, "argv", ["gamebanana.py", "--cache", str(tmp_path), "info", "123"])
    monkeypatch.setattr(gamebanana.urllib.request, "urlopen", lambda *_, **__: io.BytesIO(json.dumps(data).encode()))
    assert gamebanana.main() == 0
    fields = capsys.readouterr().out.splitlines()[0].split("\t")
    assert fields[0] == "info"
    assert fields[-2:] == [vis, ", ".join(ratings.values())]
