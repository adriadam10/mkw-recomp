"""The game code's chunks on LLVM's clang with zig's -cc1 line (tools/prepare/fastcc.py), with a stand-in zig and clang."""

import os
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools/prepare"))
import fastcc

pytestmark = pytest.mark.skipif(os.name == "nt", reason="shell scripts stand in for zig and clang")


def stand_ins(tmp: Path, clang_version: str, windows: bool = False) -> tuple[list[str], Path]:
    """A zig whose -### prints a -cc1 line naming its input and output, and a package with a clang. windows: as
    zig.exe, the output is a temporary in its cache (also the CodeView name) and it exits 1 on renaming it."""
    zig = tmp / "zig"
    tmp_out = '"-object-file-name=/c/tmp/ab12-\'"$(basename "$out" .o)"\'.obj" ' if windows else ""
    out = '/c/tmp/ab12-\'"$(basename "$out" .o)"\'.obj' if windows else "'\"$out\"'"
    zig.write_text(
        "#!/bin/sh\n"
        'case "$*" in *--version*) echo "clang version 22.1.8"; exit 0;; esac\n'
        'for a; do [ "$prev" = -o ] && out=$a; case $a in *.c) src=$a;; esac; prev=$a; done\n'
        'echo \'"/z/zig" "-cc1" "-main-file-name" "\'"$(basename "$src")"\'" "-D" "ID=\\"x\\"" '
        f'{tmp_out}"-o" "{out}" "-x" "c" "\'"$src"\'"\' >&2\n'
        + ("echo \"error: failed to rename 'tmp' into 'o': FileNotFound\" >&2; exit 1\n" if windows else "")
    )
    pkg = tmp / "setup"
    (pkg / "clang").mkdir(parents=True)
    clang = fastcc.clang_path(pkg)
    clang.write_text(f'#!/bin/sh\necho "clang version {clang_version} (https://github.com/llvm/llvm-project abc)"\n')
    for p in (zig, clang):
        p.chmod(0o755)
    return [str(zig), "cc", "-target", "x86_64-linux-gnu"], pkg


def test_a_chunk_runs_zigs_line_on_clang_with_its_paths(tmp_path: Path) -> None:
    cc, pkg = stand_ins(tmp_path, "22.1.8")
    obj = tmp_path / "obj"
    obj.mkdir()
    cc1 = fastcc.setup(cc, pkg, obj, [["-O1"]])
    assert cc1 is not None
    src, out = tmp_path / "gen/main_0001.c", obj / "main_main_0001.o"
    cmd = cc1.cmd(["-O1"], src, out)
    assert cmd[0] == str(fastcc.clang_path(pkg))
    assert cmd[1:] == ["-cc1", "-main-file-name", "main_0001.c", "-D", 'ID="x"', "-o", str(out), "-x", "c", str(src)]
    assert not (obj / f"{fastcc.PROBE}.c").exists()


def test_zig_exe_line_writes_the_chunk_object_not_its_cache_temporary(tmp_path: Path) -> None:
    cc, pkg = stand_ins(tmp_path, "22.1.8", windows=True)
    obj = tmp_path / "obj"
    obj.mkdir()
    cc1 = fastcc.setup(cc, pkg, obj, [["-O1"]])
    assert cc1 is not None
    src, out = tmp_path / "gen/main_0001.c", obj / "main_main_0001.o"
    assert cc1.cmd(["-O1"], src, out)[1:] == [
        "-cc1", "-main-file-name", "main_0001.c", "-D", 'ID="x"', f"-object-file-name={out}", "-o", str(out),
        "-x", "c", str(src),
    ]  # fmt: skip


def test_zig_compiles_when_clang_is_another_version(tmp_path: Path) -> None:
    cc, pkg = stand_ins(tmp_path, "22.1.7")
    assert fastcc.setup(cc, pkg, tmp_path, [["-O1"]]) is None


def test_zig_compiles_when_asked(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    cc, pkg = stand_ins(tmp_path, "22.1.8")
    monkeypatch.setenv("MKW_SETUP_CC", "zig")
    assert fastcc.setup(cc, pkg, tmp_path, [["-O1"]]) is None
