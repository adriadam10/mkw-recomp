"""prepare.py --next: the update set up beside the played version (tools/prepare/nextsetup.py)."""

import json
import sys
from pathlib import Path

import pytest

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


def test_promote_requires_a_boolean_ready_marker(tmp_path: Path) -> None:
    played(tmp_path)
    out = nextsetup.seed(tmp_path)
    (out / "mkwgame.so").write_bytes(b"unfinished lib")
    state = {"version": 1, "done": {}, "port": "0.1.13", "ready": "false"}
    (out / "setup-state.json").write_text(json.dumps(state), encoding="utf-8")

    assert not nextsetup.promote(tmp_path, "0.1.13")
    assert (tmp_path / "mkwgame.so").read_bytes() == b"old lib"


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


@pytest.mark.parametrize("interrupted", [False, True])
def test_promotion_restores_outputs_after_a_failed_or_interrupted_rename(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, interrupted: bool
) -> None:
    played(tmp_path)
    (tmp_path / "pipeline-keys.bin").write_bytes(b"old keys")
    out = nextsetup.seed(tmp_path)
    (out / "mkwgame.so").write_bytes(b"new lib")
    (out / "mkwgame.id").write_bytes(b"new id")
    (out / "pipeline-keys.bin").write_bytes(b"new keys")
    (out / nextsetup.STATE).write_text(json.dumps({"version": 1, "done": {}, "port": "0.1.13", "ready": True}))
    rename = Path.rename

    def fail_at_keys(src: Path, dst: Path) -> Path:
        if src == out / "pipeline-keys.bin":
            if interrupted:
                raise KeyboardInterrupt
            raise PermissionError("keys locked")
        return rename(src, dst)

    with monkeypatch.context() as patch:
        patch.setattr(Path, "rename", fail_at_keys)
        with pytest.raises(KeyboardInterrupt if interrupted else PermissionError):
            nextsetup.promote(tmp_path, "0.1.13")
    if interrupted:
        assert not (tmp_path / "pipeline-keys.bin").exists()
        nextsetup.recover(tmp_path)
    assert (tmp_path / "mkwgame.so").read_bytes() == b"old lib"
    assert (tmp_path / "pipeline-keys.bin").read_bytes() == b"old keys"
    assert not (tmp_path / "mkwgame.id").exists()
    assert (out / "mkwgame.id").read_bytes() == b"new id"
    assert not (out / nextsetup.JOURNAL).exists()
    assert nextsetup.promote(tmp_path, "0.1.13")
    assert (tmp_path / "pipeline-keys.bin").read_bytes() == b"new keys"
    assert (tmp_path / "pipeline-keys.bin.old").read_bytes() == b"old keys"
    nextsetup.remove_old(tmp_path)
    assert not (tmp_path / "pipeline-keys.bin.old").exists()


def test_failed_rollback_preserves_both_versions_until_retry(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    played(tmp_path)
    out = nextsetup.seed(tmp_path)
    (out / "mkwgame.so").write_bytes(b"new lib")
    (out / nextsetup.STATE).write_text(json.dumps({"version": 1, "done": {}, "port": "0.1.13", "ready": True}))
    rename = Path.rename
    live = tmp_path / "mkwgame.so"
    old = tmp_path / "mkwgame.so.old"

    def locked(src: Path, dst: Path) -> Path:
        if dst == live:
            raise PermissionError("library locked")
        return rename(src, dst)

    with monkeypatch.context() as patch:
        patch.setattr(Path, "rename", locked)
        with pytest.raises(OSError, match="Rollback failed"):
            nextsetup.promote(tmp_path, "0.1.13")
    assert (out / nextsetup.JOURNAL).is_file()
    assert old.read_bytes() == b"old lib"
    assert (out / "mkwgame.so").read_bytes() == b"new lib"
    assert nextsetup.promote(tmp_path, "0.1.13")
    assert live.read_bytes() == b"new lib"
    assert old.read_bytes() == b"old lib"


def test_prepare_reports_promotion_failure(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    import prepare

    def fail(game: Path, port: str) -> bool:
        assert game == tmp_path and port == "0.1.13"
        raise PermissionError("staged output locked")

    monkeypatch.setattr(sys, "argv", ["prepare.py", "--game", str(tmp_path), "--port", "0.1.13"])
    monkeypatch.setattr(prepare, "own_children", lambda: None)
    monkeypatch.setattr(prepare, "hold_lock", lambda _: None)
    monkeypatch.setattr(prepare.update, "ca_file", lambda: None)
    monkeypatch.setattr(nextsetup, "promote", fail)
    with pytest.raises(SystemExit) as error:
        prepare.main()
    assert error.value.code == 1
    assert capsys.readouterr().out == "error io staged output locked\n"
