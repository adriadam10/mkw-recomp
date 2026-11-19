"""Check Original conflict choices against real synthetic SARC data and the launcher's conflict reader."""

import json
import shutil
import subprocess
import sys
from pathlib import Path

import probe_build
import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/mod"))
import merge  # noqa: E402


@pytest.fixture(scope="module")
def conflict_probe(tmp_path_factory):
    compiler = shutil.which("g++") or shutil.which("c++") or shutil.which("clang++")
    if not compiler:
        pytest.skip("C++ compiler needed")
    tmp = tmp_path_factory.mktemp("conflict-reader")
    header = (ROOT / "runtime/ui/launcher/mods.h").read_text(encoding="utf-8")
    end = header.index("} ModConflict;") + len("} ModConflict;")
    start = header.rindex("typedef struct {", 0, end)
    row = header[start:end]
    source = (ROOT / "runtime/ui/launcher/mods.cpp").read_text(encoding="utf-8")
    reader = source[source.index("static int json_string("):source.index("int mods_conflict_count(")]
    cxx = r"""
#include <cstdio>
#include <cstdint>
#include <cstring>
#define PLAT_PATH_MAX 1024
#define MOD_NAME_MAX 64
using PlatFile = FILE;
const char *cfg_game_dir() { return "."; }
PlatFile *plat_fs_open(const char *path, int, int *) { return fopen(path, "rb"); }
int64_t plat_file_pread(PlatFile *f, char *out, uint32_t size, uint64_t) {
    return fread(out, 1, size, f);
}
void plat_file_close(PlatFile *f) { fclose(f); }
void rt_log(const char *, ...) {}
"""
    driver = r"""
static struct { int nconflicts; ModConflict conflicts[64]; } G;
"""
    report = r"""
int main() {
    conflicts_load();
    for (int i = 0; i < G.nconflicts; ++i) {
        const auto &c = G.conflicts[i];
        printf("%s %d %d\n", c.inner, c.resolved, c.has_original);
    }
}
"""
    cpp, exe = tmp / "conflicts.cpp", tmp / "conflicts.exe"
    cpp.write_text(cxx + row + driver + reader + report, encoding="utf-8")
    probe_build.build([compiler, str(cpp), "-o", str(exe)])
    return exe


def test_conflict_reader_keeps_flags_in_their_own_row(conflict_probe, tmp_path):
    merged = tmp_path / "mods/.merged"
    merged.mkdir(parents=True)
    rows = [
        dict(archive="shared.sarc", inner="new.bin", mods=["A", "B"], winner="B",
             resolved=False, has_original=False),
        dict(archive="shared.sarc", inner="original.bin", mods=["A", "B"], winner="vanilla",
             resolved=True, has_original=True),
    ]
    (merged / "conflicts.json").write_text(json.dumps(rows, indent=2), encoding="utf-8")
    result = subprocess.run([str(conflict_probe)], cwd=tmp_path, check=True, capture_output=True,
                            text=True, timeout=30)
    assert result.stdout.splitlines() == ["new.bin 0 0", "original.bin 1 1"]


def test_legacy_original_pick_for_an_added_file_gets_a_default(tmp_path):
    game = tmp_path / "game"
    original = tmp_path / "base romfs"
    sources = [
        (original, [("original.bin", b"original")]),
        (game / "mods/A/romfs", [("new.bin", b"A added"), ("original.bin", b"A edit")]),
        (game / "mods/B/romfs", [("new.bin", b"B added"), ("original.bin", b"B edit")]),
    ]
    for path, files in sources:
        path.mkdir(parents=True)
        (path / "shared.sarc").write_bytes(merge.build_sarc(files))
    choices = game / "mods/choices.json"
    rows = [dict(archive="shared.sarc", inner=name, winner="vanilla", mods=["A", "B"])
            for name in ("new.bin", "original.bin")]
    choices.write_text(json.dumps(rows), encoding="utf-8")

    args = [sys.executable, "-I", str(ROOT / "tools/mod/merge.py"), "--game", str(game), "--romfs",
            str(original), "--enable", "A,B", "--choices", str(choices)]
    merged = game / "mods/.merged"

    def check_merge():
        p = subprocess.run(args, capture_output=True, text=True, encoding="utf-8", timeout=30)
        assert p.returncode == 0, p.stdout + p.stderr
        assert p.stdout.splitlines()[-1].startswith("done ")
        data = merge.parse_sarc_ordered((merged / "romfs/shared.sarc").read_bytes())[0]
        assert dict(data) == {"new.bin": b"B added", "original.bin": b"original"}
        conflicts = json.loads((merged / "conflicts.json").read_text(encoding="utf-8"))
        assert {row["inner"]: (row["winner"], row["resolved"], row["has_original"]) for row in conflicts} == {
            "new.bin": ("B", False, False), "original.bin": ("vanilla", True, True),
        }

    check_merge()
    # Cached conflict data from the previous schema must be regenerated even with the same mods and choices.
    state_path = merged / "state.json"
    state = json.loads(state_path.read_text(encoding="utf-8"))
    state["version"] = 2
    state_path.write_text(json.dumps(state), encoding="utf-8")
    check_merge()
