"""Save backups must retain earlier snapshots, including simultaneous calls in one second.

Why: the Tools page's timestamp was also its overwrite target. Exercise saves.backup and its CLI with invented
profiles, a fixed clock and real ZIPs, without a launcher or game. Usage: uv run pytest tests/launcher/test_save_backups.py
"""

import importlib.util
import subprocess
import sys
import zipfile
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tools/prepare/saves.py"
STAMP = "20261117-120000"
spec = importlib.util.spec_from_file_location("save_backups", SCRIPT)
saves = importlib.util.module_from_spec(spec)
spec.loader.exec_module(saves)


@pytest.fixture
def profile(tmp_path, monkeypatch):
    monkeypatch.setattr(saves.time, "strftime", lambda _: STAMP)
    folder = tmp_path / "Player profile"
    (folder / "save").mkdir(parents=True)
    (folder / "save/slot.txt").write_bytes(b"first snapshot")
    return folder


def contents(path):
    with zipfile.ZipFile(path) as archive:
        assert archive.testzip() is None
        return {name: archive.read(name) for name in archive.namelist()}


def test_repeated_backup_retains_both_snapshots(profile, tmp_path):
    dest = tmp_path / "backups"
    first = Path(saves.backup(profile, dest, "Player"))
    original = first.read_bytes()
    (profile / "save/slot.txt").write_bytes(b"second snapshot")
    second = Path(saves.backup(profile, dest, "Player"))
    assert first != second
    assert first.name == f"Player-{STAMP}.zip"
    assert first.read_bytes() == original
    assert contents(first) == {"save/slot.txt": b"first snapshot"}
    assert contents(second) == {"save/slot.txt": b"second snapshot"}
    assert len(list(dest.glob("*.zip"))) == 2
    assert not list(dest.glob("*.part"))


def test_concurrent_processes_keep_each_profile(tmp_path):
    dest = tmp_path / "backups"
    driver = (
        "import importlib.util, sys; from pathlib import Path; "
        "s=importlib.util.spec_from_file_location('backup',sys.argv[1]); "
        "m=importlib.util.module_from_spec(s); s.loader.exec_module(m); "
        f"m.time.strftime=lambda _: '{STAMP}'; "
        "print(m.backup(Path(sys.argv[2]),Path(sys.argv[3]),'Player'))"
    )
    jobs = []
    try:
        for i in range(6):
            folder = tmp_path / f"profile{i}"
            folder.mkdir()
            (folder / "save.txt").write_text(str(i), encoding="utf-8")
            jobs.append(subprocess.Popen([sys.executable, "-I", "-c", driver, str(SCRIPT), str(folder), str(dest)],
                                         stdout=subprocess.PIPE, stderr=subprocess.PIPE))
        paths = []
        for job in jobs:
            out, err = job.communicate(timeout=30)
            assert job.returncode == 0, err
            paths.append(Path(out.decode().strip()))
        assert len(set(paths)) == len(jobs)
        assert {contents(p)["save.txt"] for p in paths} == {str(i).encode() for i in range(6)}
        assert not list(dest.glob("*.part"))
    finally:
        for job in jobs:
            if job.poll() is None:
                job.kill()
            job.communicate()


def test_existing_archive_and_stale_partial_are_kept(profile, tmp_path):
    dest = tmp_path / "backups"
    dest.mkdir()
    archive = dest / f"Player-{STAMP}.zip"
    partial = dest / f"Player-{STAMP}-1.part"
    archive.write_bytes(b"an earlier backup")
    partial.write_bytes(b"another process or an interrupted copy")
    result = Path(saves.backup(profile, dest, "Player"))
    assert result not in (archive, partial)
    assert archive.read_bytes() == b"an earlier backup"
    assert partial.read_bytes() == b"another process or an interrupted copy"
    assert contents(result)["save/slot.txt"] == b"first snapshot"


def test_write_failure_removes_only_its_partial(profile, tmp_path, monkeypatch):
    dest = tmp_path / "backups"
    earlier = Path(saves.backup(profile, dest, "Player"))
    original = earlier.read_bytes()

    def fail(*args, **kwargs):
        raise OSError("synthetic write failure")

    monkeypatch.setattr(zipfile.ZipFile, "write", fail)
    with pytest.raises(OSError, match="synthetic write failure"):
        saves.backup(profile, dest, "Player")
    assert earlier.read_bytes() == original
    assert list(dest.iterdir()) == [earlier]


def test_names_that_clean_the_same_do_not_collide(profile, tmp_path):
    first = Path(saves.backup(profile, tmp_path / "backups", "A/B"))
    second = Path(saves.backup(profile, tmp_path / "backups", "A?B"))
    assert first != second
    assert contents(first) == contents(second)


def test_cli_and_missing_profile(profile, tmp_path):
    args = [sys.executable, "-I", str(SCRIPT), "backup", "--profile", str(profile),
            "--dest", str(tmp_path / "backups"), "--name", "Player"]
    result = subprocess.run(args, capture_output=True, check=True)
    assert result.stdout.startswith(b"done ")
    assert contents(Path(result.stdout.decode().strip()[5:]))["save/slot.txt"] == b"first snapshot"
    args[args.index(str(profile))] = str(tmp_path / "absent")
    result = subprocess.run(args, capture_output=True)
    assert result.returncode == 1 and result.stdout.startswith(b"error noprofile ")
