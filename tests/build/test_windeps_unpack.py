"""tools/build/windeps.py unpack_deps: an MSYS2 package member never writes outside build/win/deps.

Why: each package's sha256 is checked before it is unpacked, but the member's path was joined to the destination
unchecked, and the include/lib/bin filter only looked at its first segment. A member such as
ucrt64/include/../../../x therefore passed the filter and was written outside the deps folder.

Usage: uv run pytest tests/build
"""

import io
import sys
import tarfile
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/build"))

import windeps  # noqa: E402


def make_archive(path: Path, members: list[tuple[str, bytes]]) -> Path:
    with tarfile.open(path, "w") as tar:
        for name, data in members:
            info = tarfile.TarInfo(name)
            info.size = len(data)
            tar.addfile(info, io.BytesIO(data))
    return path


def test_member_that_climbs_out_of_deps_is_refused(tmp_path, monkeypatch):
    monkeypatch.setattr(windeps, "WIN", tmp_path / "win")
    archive = make_archive(
        tmp_path / "pkg.tar",
        [("ucrt64/include/../../../escaped.txt", b"x")],
    )

    with pytest.raises(SystemExit):
        windeps.unpack_deps({"pkg": archive})

    assert not (tmp_path / "escaped.txt").exists()
    assert not (tmp_path / "win" / "escaped.txt").exists()


def test_ordinary_members_still_unpack_under_deps(tmp_path, monkeypatch):
    monkeypatch.setattr(windeps, "WIN", tmp_path / "win")
    archive = make_archive(
        tmp_path / "pkg.tar",
        [
            ("ucrt64/include/vulkan/vulkan.h", b"#pragma once\n"),
            ("ucrt64/lib/libfoo.a", b"!<arch>\n"),
        ],
    )

    windeps.unpack_deps({"pkg": archive})

    deps = tmp_path / "win" / "deps"
    assert (deps / "include/vulkan/vulkan.h").read_bytes() == b"#pragma once\n"
    assert (deps / "lib/libfoo.a").read_bytes() == b"!<arch>\n"
