"""The "env:" line of logs/setup.log (tools/prepare/machine.py environment): the system, Wine or Windows, the kind of drive
and its free space. Why: a setup under Proton (#406), on a network share (#439) or on a full disk fails differently, and
the log had only the thread count and the RAM, so a report could not say which one it was.

Usage: uv run pytest tests/launcher/test_setup_environment.py
"""

import os
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools/prepare"))
import machine

PREPARE = Path(__file__).resolve().parents[2] / "tools/prepare/prepare.py"


def test_a_native_windows_game_folder(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(os, "name", "nt")
    monkeypatch.setattr(machine, "wine_version", lambda: None)
    monkeypatch.setattr(machine, "drive_kind", lambda p: "fixed")
    monkeypatch.delenv("ZIG_GLOBAL_CACHE_DIR", raising=False)
    line = machine.environment(tmp_path)
    assert "native Windows" in line
    assert "fixed drive" in line
    assert " GB free" in line
    assert "Wine" not in line
    assert "ZIG_GLOBAL_CACHE_DIR" not in line


def test_proton_on_a_network_share_says_so(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(os, "name", "nt")
    monkeypatch.setattr(machine, "wine_version", lambda: "10.0")
    monkeypatch.setattr(machine, "drive_kind", lambda p: "network")
    line = machine.environment(tmp_path)
    assert "Wine 10.0" in line
    assert "network drive" in line
    assert "native Windows" not in line


def test_the_players_zig_cache_folder_is_named(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setenv("ZIG_GLOBAL_CACHE_DIR", "D:/zig")
    assert "ZIG_GLOBAL_CACHE_DIR=D:/zig" in machine.environment(tmp_path)


def test_an_unreadable_folder_does_not_stop_the_setup(tmp_path: Path) -> None:
    line = machine.environment(tmp_path / "does" / "not" / "exist")
    assert "free space unknown" in line


def test_off_windows_there_is_no_wine_and_no_drive_kind(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(os, "name", "posix")
    assert machine.wine_version() is None
    assert machine.drive_kind(tmp_path) == "unknown"


def test_the_setup_writes_the_line_to_its_log() -> None:
    assert 'print(f"env: {environment(game)}", file=sys.stderr)' in PREPARE.read_text(encoding="utf-8")
