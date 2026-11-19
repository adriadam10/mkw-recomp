"""Exercise the launcher's fatal-log startup gate with UTF-8 paths and bounded reads, without SDL or the game."""

import shutil
import subprocess
from pathlib import Path

import probe_build
import pytest

ROOT = Path(__file__).resolve().parents[2]

STUBS = r"""
#include <cstdio>
#include <cstdint>
#include <cstring>
#ifdef _WIN32
#include <windows.h>
#endif

using PlatFile = FILE;
struct PlatFileId { uint64_t size; };
static const char *game;
#define PLAT_PATH_MAX 1024
static const char *cfg_game_dir() { return game; }
static PlatFile *plat_file_open(const char *path) {
#ifdef _WIN32
    wchar_t wide[PLAT_PATH_MAX * 2];
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, sizeof(wide) / sizeof(*wide)))
        return nullptr;
    return _wfopen(wide, L"rb");
#else
    return fopen(path, "rb");
#endif
}
static int plat_file_id(PlatFile *file, PlatFileId *id) {
    if (fseek(file, 0, SEEK_END)) return 0;
    long size = ftell(file);
    if (size < 0) return 0;
    id->size = size;
    return 1;
}
static int64_t plat_file_pread(PlatFile *file, void *buf, uint32_t size, uint64_t offset) {
    if (fseek(file, (long)offset, SEEK_SET)) return -1;
    return fread(buf, 1, size, file);
}
static void plat_file_close(PlatFile *file) { fclose(file); }
"""

DRIVER = r"""
int main(int argc, char **argv) {
    game = !strcmp(argv[1], "accent") ? "caf\xc3\xa9" : "plain";
    printf("%d\n", last_run_fatal());
}
"""


@pytest.fixture(scope="module")
def last_run_probe(tmp_path_factory):
    cxx = shutil.which("g++") or shutil.which("c++") or shutil.which("clang++")
    if not cxx:
        pytest.skip("C++ compiler needed")
    path = tmp_path_factory.mktemp("last-run-path")
    launcher = (ROOT / "runtime/ui/launcher/launcher.cpp").read_text(encoding="utf-8")
    start = launcher.index("static int last_run_fatal(")
    end = launcher.index("// View / Select", start)
    source, exe = path / "last_run.cpp", path / "last_run.exe"
    source.write_text(STUBS + launcher[start:end] + DRIVER, encoding="utf-8")
    probe_build.build([cxx, str(source), "-o", str(exe)])
    return exe


@pytest.mark.parametrize(("name", "mode"), [("plain", "plain"), ("café", "accent")])
@pytest.mark.parametrize(("log", "fatal"), [
    (b"FATAL startup stopped\n", True),
    (b"fatal signal\n", True),
    (b"window closed\n", False),
    (b"FATAL old entry\n" + b"x" * 65536, False),
    (None, False),
])
def test_last_run_log_in_selected_game_folder(tmp_path, last_run_probe, name, mode, log, fatal):
    folder = tmp_path / name / "logs"
    folder.mkdir(parents=True)
    if log is not None:
        (folder / "mkw.log").write_bytes(log)
    p = subprocess.run([str(last_run_probe), mode], cwd=tmp_path, check=True,
                       capture_output=True, text=True, timeout=30)
    assert p.stdout.strip() == str(int(fatal))
