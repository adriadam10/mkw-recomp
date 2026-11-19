"""prepare.py --next: the update set up beside the played version (tools/prepare/nextsetup.py)."""

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools/prepare"))
import nextsetup


def played(game: Path) -> None:
    (game / "spirv").mkdir(parents=True)
    (game / "spirv/a.spv").write_bytes(b"old a")
    (game / "spirv/b.spv").write_bytes(b"old b")
    (game / "mkwgame.so").write_bytes(b"old lib")
    (game / "setup-state.json").write_text(json.dumps({"version": 1, "done": {"compile": "k1"}, "port": "0.1.12"}))


def test_an_update_without_shaders_keeps_the_played_spirv(tmp_path: Path) -> None:
    # spirv/ is translate.py's (--base): seed makes none, and promote leaves the played one in place
    played(tmp_path)
    out = nextsetup.seed(tmp_path)
    assert not (out / "spirv").exists()
    (out / "setup-state.json").write_text(json.dumps({"version": 1, "done": {}, "port": "0.1.13", "ready": True}))
    assert nextsetup.promote(tmp_path, "0.1.13")
    assert (tmp_path / "spirv/a.spv").read_bytes() == b"old a"


def test_load_starts_from_the_played_state_and_resumes_its_own(tmp_path: Path) -> None:
    played(tmp_path)
    state = json.loads((tmp_path / "setup-state.json").read_text())
    s = nextsetup.load(tmp_path, state)
    s["done"]["compile"] = "k2"
    assert state["done"]["compile"] == "k1"  # a copy, not the played one
    nextsetup.seed(tmp_path)
    (tmp_path / "setup-next/setup-state.json").write_text(json.dumps(s))
    assert nextsetup.load(tmp_path, state)["done"]["compile"] == "k2"


def test_promote_moves_a_finished_update_in(tmp_path: Path) -> None:
    played(tmp_path)
    out = nextsetup.seed(tmp_path)
    (out / "mkwgame.so").write_bytes(b"new lib")
    (out / "spirv").mkdir()
    (out / "spirv/a.spv").write_bytes(b"new a")
    (out / "setup-state.json").write_text(json.dumps({"version": 1, "done": {}, "port": "0.1.13", "ready": True}))
    assert nextsetup.promote(tmp_path, "0.1.13")
    assert (tmp_path / "mkwgame.so").read_bytes() == b"new lib"
    assert json.loads((tmp_path / "setup-state.json").read_text())["port"] == "0.1.13"
    assert not out.exists() and (tmp_path / "spirv.old").is_dir()  # removed beside the swap's steps
    assert (tmp_path / "spirv/a.spv").read_bytes() == b"new a"
    nextsetup.remove_old(tmp_path)
    assert not (tmp_path / "spirv.old").exists() and not (tmp_path / "mkwgame.so.old").exists()


def test_promote_drops_an_unfinished_or_other_version(tmp_path: Path) -> None:
    played(tmp_path)
    out = nextsetup.seed(tmp_path)
    (out / "mkwgame.so").write_bytes(b"half")
    (out / "setup-state.json").write_text(json.dumps({"version": 1, "done": {}, "port": "0.1.13"}))
    assert not nextsetup.promote(tmp_path, "0.1.13")
    assert (tmp_path / "mkwgame.so").read_bytes() == b"old lib" and not out.exists()


def test_warm_caches_are_copies_and_move_in(tmp_path: Path) -> None:
    # the prewarm appends to the keys: a link would change the played version's file
    played(tmp_path)
    (tmp_path / "pipeline-keys.bin").write_bytes(b"keys")
    out = nextsetup.seed(tmp_path)
    with open(out / "pipeline-keys.bin", "ab") as f:
        f.write(b" more")
    assert (tmp_path / "pipeline-keys.bin").read_bytes() == b"keys"
    (out / "pipeline-cache.bin").write_bytes(b"cache")
    (out / "setup-state.json").write_text(json.dumps({"version": 1, "done": {}, "port": "0.1.13", "ready": True}))
    assert nextsetup.promote(tmp_path, "0.1.13")
    assert (tmp_path / "pipeline-keys.bin").read_bytes() == b"keys more"
    assert (tmp_path / "pipeline-cache.bin").read_bytes() == b"cache"
