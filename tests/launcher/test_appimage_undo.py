"""Exercise the launcher's AppImage Undo with real renames and injected I/O failures, without a packaged game."""

import shutil
import subprocess
from pathlib import Path

import probe_build
import pytest

ROOT = Path(__file__).resolve().parents[2]

STUBS = r"""
#include <cstdio>
#include <cstdlib>
#include <cstring>
static int count, fail_at, fail_recovery_at, metadata_writes;
static const char *rt_port_version = "new";
int update_previous(const char *, char *tag, unsigned) { strcpy(tag, "old"); return 1; }
const char *SDL_GetError() { return "injected I/O failure"; }
bool SDL_RenamePath(const char *from, const char *to) {
    ++count;
    return count != fail_at && count != fail_recovery_at && rename(from, to) == 0;
}
bool plat_file_exists(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    fclose(file);
    return true;
}
bool write_lines(const char *, const char *, const char *) { ++metadata_writes; return true; }
bool write_mark(const char *, const char *, const char *) { ++metadata_writes; return true; }
void rt_log(const char *, ...) {}
"""

DRIVER = r"""
int main(int argc, char **argv) {
    fail_at = atoi(argv[1]);
    fail_recovery_at = atoi(argv[2]);
    char why[1200] = "";
    int result = update_revert(".", why, sizeof why);
    printf("%d %d\n%s\n", result, metadata_writes, why);
}
"""


@pytest.fixture(scope="module")
def undo_probe(tmp_path_factory):
    cxx = shutil.which("g++") or shutil.which("c++") or shutil.which("clang++")
    if not cxx:
        pytest.skip("C++ compiler needed")
    path = tmp_path_factory.mktemp("appimage-undo")
    text = (ROOT / "runtime/ui/launcher/launcher_update.cpp").read_text(encoding="utf-8")
    start = text.index("static void kept_appimage(")
    kept = text[start:text.index("#endif", start)]
    # Build the production Linux branch even when the test runs on Windows.
    revert = text[text.index("int update_revert("):text.index("void update_relaunch(")]
    source, exe = path / "undo.cpp", path / "undo.exe"
    source.write_text(STUBS + kept + revert.replace("#ifdef _WIN32", "#if 0", 1) + DRIVER, encoding="utf-8")
    probe_build.build([cxx, str(source), "-o", str(exe)])
    return exe


@pytest.mark.parametrize("first_failure", [0, 1, 2, 3])
def test_undo_restores_both_original_paths_on_error(tmp_path, undo_probe, first_failure, monkeypatch):
    app, kept = tmp_path / "MKW.AppImage", tmp_path / "MKW.AppImage.previous"
    app.write_bytes(b"current")
    kept.write_bytes(b"previous")
    monkeypatch.setenv("APPIMAGE", app.name)
    p = subprocess.run([str(undo_probe), str(first_failure), "0"], cwd=tmp_path, check=True,
                       capture_output=True, text=True, timeout=30)
    result, reason = p.stdout.splitlines()
    if first_failure:
        assert result == "0 0" and "I/O failure" in reason
        assert app.read_bytes() == b"current"
        assert kept.read_bytes() == b"previous"
    else:
        assert result == "1 2"
        assert app.read_bytes() == b"previous"
        assert kept.read_bytes() == b"current"
    assert not (tmp_path / "MKW.AppImage.next").exists()


@pytest.mark.parametrize("recovery_failure", [4, 5])
def test_undo_reports_failed_recovery_and_preserves_copies(tmp_path, undo_probe, recovery_failure, monkeypatch):
    app = tmp_path / "MKW.AppImage"
    app.write_bytes(b"current")
    (tmp_path / "MKW.AppImage.previous").write_bytes(b"previous")
    monkeypatch.setenv("APPIMAGE", app.name)
    p = subprocess.run([str(undo_probe), "3", str(recovery_failure)], cwd=tmp_path, check=True,
                       capture_output=True, text=True, timeout=30)
    result, reason = p.stdout.splitlines()
    assert result == "0 0"
    assert "rollback failed" in reason and ".next" in reason
    assert sorted(path.read_bytes() for path in tmp_path.iterdir()) == [b"current", b"previous"]


def test_undo_preserves_existing_recovery_file(tmp_path, undo_probe, monkeypatch):
    app = tmp_path / "MKW.AppImage"
    app.write_bytes(b"current")
    (tmp_path / "MKW.AppImage.previous").write_bytes(b"previous")
    next_path = tmp_path / "MKW.AppImage.next"
    next_path.write_bytes(b"earlier recovery")
    monkeypatch.setenv("APPIMAGE", app.name)
    p = subprocess.run([str(undo_probe), "0", "0"], cwd=tmp_path, check=True,
                       capture_output=True, text=True, timeout=30)
    result, reason = p.stdout.splitlines()
    assert result == "0 0" and "already exists" in reason
    assert next_path.read_bytes() == b"earlier recovery"
    assert app.read_bytes() == b"current"
