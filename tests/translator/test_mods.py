"""Code patches shipped by mods (recomp/patches.py load_mods): a mod.json's spans and sites merged into the table."""

import json
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "recomp"))
import patches  # noqa: E402

BASES = {"main": (0x7000000000, 0x3410000)}


def mod(root: Path, name: str, p: dict) -> None:
    (root / name).mkdir()
    (root / name / "mod.json").write_text(json.dumps({"name": name, "patches": p}))


def test_span_only_mod(tmp_path):
    # a mod with spans and no sites (Timer alone) used where/index/clash before they were set
    mod(tmp_path, "Timer", {"spans": [{"set": "timer", "env": "MKW_TIMER", "module": "main", "off": "48e728",
                                        "words": ["52800028", "39004328"]}]})
    t = patches.Table()
    patches.load_mods(t, str(tmp_path), BASES)
    assert t.sets == [("timer", "MKW_TIMER", 0)]
    assert t.spans["main"][0x48E728] == (0, [0x52800028, 0x39004328])


def test_two_mods_on_one_address(tmp_path):
    for name in ("A", "B"):
        mod(tmp_path, name, {"sites": [{"set": name.lower(), "env": f"MKW_{name}", "module": "main", "off": "7d201c",
                                        "orig": "51001D4A", "repl": ["5100214A"]}]})
    with pytest.raises(patches.PatchError):
        patches.load_mods(patches.Table(), str(tmp_path), BASES)


def test_no_mods_folder(tmp_path):
    t = patches.Table()
    patches.load_mods(t, str(tmp_path / "missing"), BASES)
    assert t.sets == [] and not t.spans
