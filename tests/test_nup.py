"""The .nup reader without a game or a GPU: a synthetic container packed against the spec (base + updates,
raw and zstd files, deduplicated extents, meta blocks), extracted and checked, plus the validation that must
fail closed. The packer is written independently of extract.py so a layout slip shows in both.

Usage: uv run pytest tests
"""

import hashlib
import importlib.util
import struct
import sys
from pathlib import Path

import pytest
from compression import zstd

ROOT = Path(__file__).resolve().parents[1]

_spec = importlib.util.spec_from_file_location("nup_extract", ROOT / "tools/nup/extract.py")
nup = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(nup)  # noqa: E402


# ---- an independent packer (SPEC §2-§5) ------------------------------------

MAGIC = b"NUP\x01"
TOC_MAGIC = b"TOC\x01"
SUPER = struct.Struct("<4sHHQQ32sQQII16s32s")
TOC_HEAD = struct.Struct("<4sIQQQQQ")
PKG = struct.Struct("<QIIIIBBHIQI20x")
DIR = struct.Struct("<IIIIQ")
FILE = struct.Struct("<IIQQQ32sBBH12x")


def pack_nup(packages):
    """packages: list of dict(kind, version, flags, title_id, exefs, romfs, meta);
    exefs/romfs are lists of (name, data, codec) — a dict key repeats are allowed (a duplicate-name test)."""
    pool = bytearray()
    dirs, files = [], []

    def intern(name: str) -> tuple[int, int]:
        off = len(pool)
        pool.extend(name.encode())
        return off, len(name)

    def add_dir(name: str) -> int:
        off, ln = intern(name)
        dirs.append([off, ln, []])
        return len(dirs) - 1

    def add_file(name: str, data: bytes, codec: int) -> int:
        off, ln = intern(name)
        payload = data if codec == 0 else zstd.compress(data, level=3)
        files.append([off, ln, len(data), hashlib.sha256(data).digest(), codec, payload])
        return len(files) - 1

    layout = [(add_dir("root"), add_dir("exefs"), add_dir("romfs")) for _ in packages]
    file_ids = [
        (
            [add_file(n, d, c) for n, d, c in spec["exefs"]],
            [add_file(n, d, c) for n, d, c in spec["romfs"]],
        )
        for spec in packages
    ]
    for i, (root, exefs_d, romfs_d) in enumerate(layout):
        exf, rff = file_ids[i]
        dirs[root][2] = [exefs_d, romfs_d]
        dirs[exefs_d][2] = [len(dirs) + f for f in exf]
        dirs[romfs_d][2] = [len(dirs) + f for f in rff]

    ndir, nfile = len(dirs), len(files)
    children = [c for d in dirs for c in d[2]]
    toc_size = (TOC_HEAD.size + len(packages) * 0x40 + ndir * 0x18 + nfile * 0x50
                + len(children) * 4 + len(pool))
    blob_offset = (0x80 + toc_size + 15) & ~15

    blob = bytearray()
    ext_off = [0] * nfile
    seen: dict[bytes, int] = {}
    for idx, f in enumerate(files):
        if f[2] == 0:
            continue
        if f[3] in seen:  # dedup: equal sha256 shares one extent (SPEC §3.6)
            ext_off[idx] = seen[f[3]]
            continue
        blob += b"\0" * ((16 - len(blob) % 16) % 16)
        seen[f[3]] = ext_off[idx] = len(blob)
        blob += f[5]

    metas, meta_off, o = [], [], blob_offset + len(blob)
    for spec in packages:
        out = b"".join(struct.pack("<HHI", t, 0, len(p)) + p for t, p in sorted((spec.get("meta") or {}).items()))
        metas.append(out)
        meta_off.append(o if out else 0)
        o += len(out)
    file_size = o

    h = hashlib.sha256()
    for spec in packages:
        h.update(struct.pack("<QIB", spec["title_id"], spec["version"], spec["kind"]))
    uuid = h.digest()[:16]

    first_child, pos = [], 0
    for d in dirs:
        first_child.append(pos)
        pos += len(d[2])
    toc = bytearray(TOC_HEAD.pack(TOC_MAGIC, len(packages), ndir, nfile, 0, 0, 0))
    for i, spec in enumerate(packages):
        root, exefs_d, romfs_d = layout[i]
        toc += PKG.pack(spec["title_id"], root, exefs_d, romfs_d, spec["version"], spec["kind"], spec["flags"],
                        0, 0, meta_off[i], len(metas[i]))
    for i, d in enumerate(dirs):
        toc += DIR.pack(d[0], d[1], first_child[i], len(d[2]), 0)
    for idx, f in enumerate(files):
        toc += FILE.pack(f[0], f[1], f[2], blob_offset + ext_off[idx], len(f[5]), f[3], f[4], 0, 0)
    if children:
        toc += struct.pack(f"<{len(children)}I", *children)
    pool_off = len(toc)
    assert pool_off == toc_size - len(pool), "toc size drift"
    struct.pack_into("<QQ", toc, 0x18, pool_off, len(pool))
    toc += pool

    sb = SUPER.pack(MAGIC, 0x80, 1, 0x80, len(toc), hashlib.sha256(bytes(toc)).digest(), blob_offset, file_size,
                    len(packages), 0, uuid, b"\0" * 32)
    return sb + bytes(toc) + b"\0" * (blob_offset - (0x80 + len(toc))) + bytes(blob) + b"".join(metas)


def pkg(kind, version, title_id, exefs=(), romfs=(), flags=0, meta=None):
    return dict(kind=kind, version=version, flags=flags, title_id=title_id, exefs=list(exefs),
                romfs=list(romfs), meta=meta)


def write_nup(tmp_path: Path, packages) -> Path:
    p = tmp_path / "test.nup"
    p.write_bytes(pack_nup(packages))
    return p


def extract_all(nup_path: Path, out: Path) -> None:
    """Run the same plan main() runs: pick the program, layer, extract every side."""
    with nup.Nup(nup_path) as n:
        base, upd = nup.pick_program(n)
        for side, attr in (("exefs", "exefs_dir"), ("romfs", "romfs_dir")):
            tree = nup.layered(n, base, upd, attr)
            dup: dict[bytes, Path] = {}
            for rel in sorted(tree):
                dest = out / side / Path(*rel)
                dest.parent.mkdir(parents=True, exist_ok=True)
                nup.extract_one(n, tree[rel], dest, True, False, dup)


# ---- tests ------------------------------------------------------------------

def test_base_and_update_shadow(tmp_path):
    """The update's files shadow the base's by path; the base keeps the rest; exefs comes from the update."""
    p = write_nup(tmp_path, [
        pkg(0, 0x10000, 0x4000000000000000,
            exefs=[("main", b"base-main", 0)],
            romfs=[("common.txt", b"base-common", 0), ("a.txt", b"A", 0), ("dir/b.txt", b"B", 0)]),
        pkg(1, 0x20000, 0x4000000000000000,
            exefs=[("main", b"upd-main", 0)],
            romfs=[("common.txt", b"upd-common", 0), ("c.txt", b"C", 0)]),
    ])
    out = tmp_path / "out"
    extract_all(p, out)
    assert (out / "exefs" / "main").read_bytes() == b"upd-main"
    assert (out / "romfs" / "common.txt").read_bytes() == b"upd-common"  # shadowed
    assert (out / "romfs" / "a.txt").read_bytes() == b"A"  # base only
    assert (out / "romfs" / "dir" / "b.txt").read_bytes() == b"B"
    assert (out / "romfs" / "c.txt").read_bytes() == b"C"  # update only


def test_base_only(tmp_path):
    p = write_nup(tmp_path, [pkg(0, 0x10000, 1, exefs=[("main", b"M", 0)], romfs=[("x", b"X", 0)])])
    out = tmp_path / "out"
    extract_all(p, out)
    assert (out / "exefs" / "main").read_bytes() == b"M"
    assert (out / "romfs" / "x").read_bytes() == b"X"


def test_standalone_update_is_the_whole_game(tmp_path):
    p = write_nup(tmp_path, [pkg(1, 0x10000, 1, flags=1, exefs=[("main", b"M", 0)], romfs=[("x", b"X", 0)])])
    out = tmp_path / "out"
    extract_all(p, out)
    assert (out / "exefs" / "main").read_bytes() == b"M"
    assert (out / "romfs" / "x").read_bytes() == b"X"


def test_zstd_file(tmp_path):
    data = b"hello " * 1000
    p = write_nup(tmp_path, [pkg(0, 0x10000, 1, romfs=[("z", data, 1)])])
    out = tmp_path / "out"
    extract_all(p, out)
    assert (out / "romfs" / "z").read_bytes() == data


def test_dedup_shared_extent(tmp_path):
    """Two files with equal content share one extent; both extract."""
    data = b"shared" * 100
    p = write_nup(tmp_path, [pkg(0, 0x10000, 1, romfs=[("one", data, 0), ("two", data, 0)])])
    with nup.Nup(p) as n:
        # the packer gave them one extent
        e1 = n.files[0][3]
        e2 = n.files[1][3]
        assert e1 == e2
    out = tmp_path / "out"
    extract_all(p, out)
    assert (out / "romfs" / "one").read_bytes() == data
    assert (out / "romfs" / "two").read_bytes() == data


def test_resume_skips_same_size(tmp_path):
    p = write_nup(tmp_path, [pkg(0, 0x10000, 1, romfs=[("x", b"AAAA", 0)])])
    out = tmp_path / "out"
    extract_all(p, out)
    # a second run must skip the file that is already there with the right size
    with nup.Nup(p) as n:
        base, upd = nup.pick_program(n)
        tree = nup.layered(n, base, upd, "romfs_dir")
        dest = out / "romfs" / "x"
        assert nup.extract_one(n, tree[("x",)], dest, True, False, {}) == ("skipped", 0)


def test_resume_rewrites_wrong_size(tmp_path):
    p = write_nup(tmp_path, [pkg(0, 0x10000, 1, romfs=[("x", b"AAAA", 0)])])
    out = tmp_path / "out"
    out.joinpath("romfs").mkdir(parents=True)
    (out / "romfs" / "x").write_bytes(b"BB")  # wrong size
    with nup.Nup(p) as n:
        base, upd = nup.pick_program(n)
        tree = nup.layered(n, base, upd, "romfs_dir")
        state, _ = nup.extract_one(n, tree[("x",)], out / "romfs" / "x", True, False, {})
    assert state == "extracted"
    assert (out / "romfs" / "x").read_bytes() == b"AAAA"


def test_sha_mismatch_fails(tmp_path):
    packages = [pkg(0, 0x10000, 1, romfs=[("x", b"AAAA", 0)])]
    blob = bytearray(pack_nup(packages))
    # corrupt the one romfs byte in the blob (the record's sha no longer matches)
    i = blob.find(b"AAAA")
    blob[i] = ord("B")
    p = tmp_path / "corrupt.nup"
    p.write_bytes(bytes(blob))
    out = tmp_path / "out"
    with nup.Nup(p) as n:
        base, upd = nup.pick_program(n)
        tree = nup.layered(n, base, upd, "romfs_dir")
        with pytest.raises(nup.NupError, match="sha256 mismatch"):
            nup.extract_one(n, tree[("x",)], out / "romfs" / "x", True, False, {})


def test_pick_program_highest_update(tmp_path):
    p = write_nup(tmp_path, [
        pkg(0, 0x10000, 1),
        pkg(1, 0x10000, 1),
        pkg(1, 0x20000, 1, romfs=[("top", b"T", 0)]),
    ])
    with nup.Nup(p) as n:
        base, upd = nup.pick_program(n)
        assert base == 0
        assert upd == 2


def test_bad_magic(tmp_path):
    p = tmp_path / "x.nup"
    p.write_bytes(b"XXXX" + pack_nup([pkg(0, 0x10000, 1)])[4:])
    with pytest.raises(nup.NupError, match="bad magic"):
        nup.Nup(p)


def test_toc_sha_mismatch(tmp_path):
    blob = bytearray(pack_nup([pkg(0, 0x10000, 1, romfs=[("x", b"X", 0)])]))
    blob[0x100] ^= 0xFF  # a TOC byte (the TOC starts at 0x80)
    p = tmp_path / "x.nup"
    p.write_bytes(bytes(blob))
    with pytest.raises(nup.NupError, match="TOC sha256 mismatch"):
        nup.Nup(p)


def test_package_count_mismatch(tmp_path):
    blob = bytearray(pack_nup([pkg(0, 0x10000, 1)]))
    struct.pack_into("<I", blob, 0x48, 2)  # superblock package_count
    p = tmp_path / "x.nup"
    p.write_bytes(bytes(blob))
    with pytest.raises(nup.NupError, match="package_count"):
        nup.Nup(p)


def test_update_without_base_must_be_standalone(tmp_path):
    p = write_nup(tmp_path, [pkg(1, 0x10000, 1, flags=0)])
    with pytest.raises(nup.NupError, match="standalone"):
        nup.Nup(p)


def test_unsafe_name(tmp_path):
    p = write_nup(tmp_path, [pkg(0, 0x10000, 1, romfs=[("..", b"X", 0)])])
    with nup.Nup(p) as n:
        base, upd = nup.pick_program(n)
        with pytest.raises(nup.NupError, match="unsafe name"):
            nup.layered(n, base, upd, "romfs_dir")


def test_duplicate_name(tmp_path):
    p = write_nup(tmp_path, [pkg(0, 0x10000, 1, romfs=[("x", b"1", 0), ("x", b"2", 0)])])
    with nup.Nup(p) as n:
        base, upd = nup.pick_program(n)
        with pytest.raises(nup.NupError, match="two files at"):
            nup.layered(n, base, upd, "romfs_dir")


def test_unknown_codec(tmp_path):
    p = write_nup(tmp_path, [pkg(0, 0x10000, 1, romfs=[("x", b"X", 3)])])
    with pytest.raises(nup.NupError, match="unknown codec"):
        nup.Nup(p)


def test_meta_and_list(tmp_path):
    build_id = bytes(range(16))
    p = write_nup(tmp_path, [
        pkg(0, 0x10000, 1, exefs=[("main", b"M", 0)], meta={4: b"1.8.0", 5: b"MKW", 7: build_id, 8: b"test"})])
    with nup.Nup(p) as n:
        meta = n.meta(0)
        assert meta[4] == b"1.8.0"
        assert meta[5] == b"MKW"
        assert meta[7][:16] == build_id
        text = "\n".join(nup.describe(n))
    assert "v1.8.0" in text
    assert "build id " + build_id.hex() in text
    assert "MKW" in text
    assert "program: package 0" in text


def test_no_verify_trusts_record(tmp_path):
    packages = [pkg(0, 0x10000, 1, romfs=[("x", b"AAAA", 0)])]
    blob = bytearray(pack_nup(packages))
    i = blob.find(b"AAAA")
    blob[i] = ord("B")  # corrupt; --no-verify must not recompute
    p = tmp_path / "x.nup"
    p.write_bytes(bytes(blob))
    out = tmp_path / "out"
    out.joinpath("romfs").mkdir(parents=True)
    with nup.Nup(p) as n:
        base, upd = nup.pick_program(n)
        tree = nup.layered(n, base, upd, "romfs_dir")
        state, _ = nup.extract_one(n, tree[("x",)], out / "romfs" / "x", False, False, {})
    assert state == "extracted"
    assert (out / "romfs" / "x").read_bytes() == b"BAAA"


def test_cli_list(tmp_path):
    p = write_nup(tmp_path, [pkg(0, 0x10000, 1, exefs=[("main", b"M", 0)],
                                 meta={4: b"1.0.0", 5: b"MKW"})])
    import subprocess
    r = subprocess.run([sys.executable, str(ROOT / "tools" / "nup" / "extract.py"), str(p), "--list"],
                       capture_output=True, text=True, timeout=60)
    assert r.returncode == 0, r.stderr
    assert "v1.0.0" in r.stdout
    assert "program: package 0" in r.stdout


def test_cli_bad_file_exit_code(tmp_path):
    p = tmp_path / "x.nup"
    p.write_bytes(b"not a nup" + b"\0" * 119)
    import subprocess
    r = subprocess.run([sys.executable, str(ROOT / "tools" / "nup" / "extract.py"), str(p)],
                       capture_output=True, text=True, timeout=60)
    assert r.returncode == 1
    assert "bad magic" in r.stderr
