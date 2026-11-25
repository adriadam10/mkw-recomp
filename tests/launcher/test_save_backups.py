"""Save backups must retain earlier snapshots, including simultaneous calls in one second.

Why: the Tools page's timestamp was also its overwrite target. Exercise saves.backup and its CLI with invented
profiles, a fixed clock and real ZIPs, without a launcher or game. Usage: uv run pytest tests/launcher/test_save_backups.py
"""

import importlib.util
import os
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


def test_linked_save_directory_does_not_produce_incomplete_backup(tmp_path):
    profile = tmp_path / "profile"
    profile.mkdir()
    target = tmp_path / "actual-saves"
    target.mkdir()
    (target / "slot.dat").write_bytes(b"save data")
    dest = tmp_path / "backups"
    earlier = Path(saves.backup(profile, dest, "Player"))
    original = earlier.read_bytes()
    link = profile / "save"
    if os.name == "nt":
        result = subprocess.run(["cmd", "/c", "mklink", "/J", str(link), str(target)], capture_output=True)
        if result.returncode:
            pytest.skip("cannot create a junction")
    else:
        link.symlink_to(target, target_is_directory=True)
    with pytest.raises(OSError, match="linked directory"):
        saves.backup(profile, dest, "Player")
    assert list(dest.glob("*.zip")) == [earlier]
    assert earlier.read_bytes() == original
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


def test_unreadable_save_directory_is_reported(profile, tmp_path, monkeypatch, capsys):
    dest = tmp_path / "backups"
    earlier = Path(saves.backup(profile, dest, "Player"))
    original = earlier.read_bytes()
    (profile / "nickname.txt").write_text("Player", encoding="utf-8")
    scandir = os.scandir

    def deny_save_dir(path="."):
        if not isinstance(path, int) and Path(path) == profile / "save":
            raise PermissionError("save directory is unreadable")
        return scandir(path)

    monkeypatch.setattr(os, "scandir", deny_save_dir)
    monkeypatch.setattr(sys, "argv", [str(SCRIPT), "backup", "--profile", str(profile), "--dest", str(dest)])
    with pytest.raises(SystemExit) as result:
        saves.main()
    assert result.value.code == 1
    assert capsys.readouterr().out == "error io save directory is unreadable\n"
    assert earlier.read_bytes() == original
    assert list(dest.iterdir()) == [earlier]


def test_names_that_clean_the_same_do_not_collide(profile, tmp_path):
    first = Path(saves.backup(profile, tmp_path / "backups", "A/B"))
    second = Path(saves.backup(profile, tmp_path / "backups", "A?B"))
    assert first != second
    assert contents(first) == contents(second)


@pytest.mark.parametrize("name", ["Player", "Café", "玩家"])
def test_cli_and_missing_profile(profile, tmp_path, name):
    # The launcher uses -I. Model a legacy Windows pipe explicitly since -I ignores PYTHONIOENCODING.
    driver = ("import runpy,sys; sys.stdout.reconfigure(encoding='cp1252'); "
              "sys.argv=sys.argv[1:]; runpy.run_path(sys.argv[0],run_name='__main__')")
    args = [sys.executable, "-I", "-c", driver, str(SCRIPT), "backup", "--profile", str(profile),
            "--dest", str(tmp_path / "backups"), "--name", name]
    result = subprocess.run(args, capture_output=True, timeout=30)
    assert result.returncode == 0, result.stderr
    line = result.stdout.decode("utf-8").strip()
    assert line.startswith("done ")
    archive = Path(line.removeprefix("done "))
    assert archive.name.startswith(f"{name}-")
    assert contents(archive)["save/slot.txt"] == b"first snapshot"

    missing = tmp_path / f"absent {name}"
    args[args.index(str(profile))] = str(missing)
    result = subprocess.run(args, capture_output=True, timeout=30)
    assert result.returncode == 1 and result.stdout.decode("utf-8").strip() == f"error noprofile {missing}"
