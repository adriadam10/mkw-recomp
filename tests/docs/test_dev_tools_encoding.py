"""The repository's own scripts for contributors read and write its files as UTF-8, whatever the machine's code page.

Why: tools/docs/progress.py read every runtime C file with the locale's encoding and stopped with a UnicodeDecodeError
on runtime/probes/ov_probe.c when its progress bars were regenerated; tools/git/mover.py skipped any file the code page
could not decode, so a path cited in it stayed stale with no word, and wrote every file it changed through the locale
(and, on Windows, with CRLF line endings). The CI runs on Linux in UTF-8 and never saw it.

Usage: uv run pytest tests/docs/test_dev_tools_encoding.py
"""

import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
# A legacy code page everywhere: no UTF-8 mode, no locale coercion, and the C locale on POSIX (Windows ignores it)
LEGACY = {"PYTHONUTF8": "0", "PYTHONCOERCECLOCALE": "0", "LC_ALL": "C", "LANG": "C"}
GIT = {"GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@t", "GIT_COMMITTER_NAME": "t", "GIT_COMMITTER_EMAIL": "t@t"}


def test_progress_reads_the_runtime_sources_as_utf8_on_a_legacy_code_page() -> None:
    driver = "import sys; sys.path.insert(0, sys.argv[1] + '/tools/docs'); import progress; print(len(progress.runtime_source()))"
    done = subprocess.run(
        [sys.executable, "-c", driver, str(ROOT)], env={**os.environ, **LEGACY},
        capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=120, check=False,
    )  # fmt: skip
    assert done.returncode == 0, done.stderr[-600:]
    assert int(done.stdout) > 1000


def git(repo: Path, *args: str) -> None:
    subprocess.run(["git", *args], cwd=repo, env={**os.environ, **GIT}, check=True, capture_output=True)


def test_mover_rewrites_a_path_in_a_utf8_file_and_keeps_its_bytes(tmp_path: Path) -> None:
    repo = tmp_path / "repo"
    (repo / "tools/git").mkdir(parents=True)
    (repo / "runtime/gfx").mkdir(parents=True)
    shutil.copy(ROOT / "tools/git/mover.py", repo / "tools/git/mover.py")
    (repo / "runtime/gfx/a.c").write_text("int a;\n")
    (repo / "runtime/gfx/README.md").write_text("see it\n")
    # UTF-8 text with a letter the Windows code page cannot decode ("č" is bytes C4 8D), and CRLF line endings
    doc = "Začetek: runtime/gfx/a.c\r\nsecond line\r\n".encode()
    (repo / "NOTES.md").write_bytes(doc)
    (repo / "map.txt").write_text("sub a.c\n")
    git(repo, "init", "-q")
    git(repo, "add", "-A")
    git(repo, "commit", "-q", "-m", "x")
    done = subprocess.run(
        [sys.executable, str(repo / "tools/git/mover.py"), "runtime/gfx", str(repo / "map.txt"), "--apply"],
        cwd=repo, env={**os.environ, **LEGACY, **GIT}, capture_output=True, text=True, encoding="utf-8",
        errors="replace", timeout=60, check=False,
    )  # fmt: skip
    assert done.returncode == 0, done.stdout + done.stderr
    assert (repo / "NOTES.md").read_bytes() == doc.replace(b"runtime/gfx/a.c", b"runtime/gfx/sub/a.c")
    assert (repo / "runtime/gfx/sub/a.c").is_file()


def test_mover_says_which_file_it_could_not_read(tmp_path: Path) -> None:
    repo = tmp_path / "repo"
    (repo / "tools/git").mkdir(parents=True)
    (repo / "runtime/gfx").mkdir(parents=True)
    shutil.copy(ROOT / "tools/git/mover.py", repo / "tools/git/mover.py")
    (repo / "runtime/gfx/a.c").write_text("int a;\n")
    (repo / "latin1.md").write_bytes(b"runtime/gfx/a.c caf\xe9\n")  # not UTF-8
    (repo / "map.txt").write_text("sub a.c\n")
    git(repo, "init", "-q")
    git(repo, "add", "-A")
    git(repo, "commit", "-q", "-m", "x")
    done = subprocess.run(
        [sys.executable, str(repo / "tools/git/mover.py"), "runtime/gfx", str(repo / "map.txt")],
        cwd=repo, env={**os.environ, **GIT}, capture_output=True, text=True, encoding="utf-8", errors="replace",
        timeout=60, check=False,
    )  # fmt: skip
    assert done.returncode == 0, done.stdout + done.stderr
    assert "latin1.md" in done.stdout and "not UTF-8" in done.stdout
