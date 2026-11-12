"""Extract a .nup package's exefs/ and romfs/ into <out>/exefs and <out>/romfs, verifying every file.

Why: the .nup (a55uka/nup format v1; the spec at a55uka/nup SPEC.md, MIT) is the single-file dump that
emulators and dump sites distribute (the Suyin dumps, taodung.com): base, updates and add-ons in one
container, files raw or zstd-compressed, deduplicated by sha256. A player's copy is exefs/ + romfs/
folders; this turns a .nup into that, so the setup (prepare.py) can accept either. The program extracted
is the highest update, else the base (SPEC §3.1.2: an update's files shadow the base's by path; a
standalone update — no base in the container, flag bit 0 — is the whole game, as in update-only dumps).
The structure and the fail-closed validation follow the spec and its reference reader (a55uka/nup
tools/nup, MIT); the resume (through .part, skip by size) and the one-event-per-line progress follow
prepare.py. Free-space check as Recompia's launcher (GPL, idea only).

Progress: one "progress <done> <total>" line per 500 files on stdout (the start screen reads stdout;
prepare.py passes such lines through), the rest on stderr. Each file lands through <name>.part and is
renamed only when its size and sha256 match the package's record; an existing file with the right size is
skipped (resume; --rehash re-hashes it); a file whose sha256 an earlier file of this run already wrote is
copied from that file (the package deduplicates by hash). Exit codes: 0 done, 1 error, 2 bad command line.

The files run in parallel (--jobs, default 8): a destination is usually a NAS, where one create+rename costs
~40 ms (measured on ours, 11/11/2026) while the bytes are cheap, so the per-file metadata latency is the
limit and overlaps across threads. Each worker keeps its own read handle on the package.

Usage: uv run tools/nup/extract.py NUP [--out DIR] [--list] [--only exefs|romfs] [--package N]
                                  [--no-verify] [--rehash] [--jobs N]
--list prints the container's packages (title, version, build id, sizes) and exits. --out defaults to the
game folder (game/, or MKW_GAME=DIR).
"""

import argparse
import contextlib
import hashlib
import os
import shutil
import stat as statmod
import struct
import sys
import threading
import time
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
from compression import zstd
from pathlib import Path

# The v1 layout (SPEC §2-§5), all little-endian
MAGIC = b"NUP\x01"
TOC_MAGIC = b"TOC\x01"
HEADER_SIZE = 0x80
PKG_SIZE, DIR_SIZE, FILE_SIZE = 0x40, 0x18, 0x50
EXTENT_ALIGN = 16
NO_DIR = 0xFFFFFFFF
CODEC_RAW, CODEC_ZSTD = 0, 1
KIND_NAMES = {0: "base", 1: "update", 2: "addon", 3: "addon_update", 4: "system"}
KIND_BASE, KIND_UPDATE = 0, 1
FLAG_STANDALONE = 0x01  # package record flags, bit 0 (SPEC §3.1.2)
TLV_DISPLAY, TLV_TITLE, TLV_PUBLISHER, TLV_BUILD_ID, TLV_SOURCE = 4, 5, 6, 7, 8
MAX_DEPTH = 64  # the deepest romfs path is five levels; more is a broken tree
PROGRESS_EVERY = 500

SUPER = struct.Struct("<4sHHQQ32sQQII16s32s")
TOC_HEAD = struct.Struct("<4sIQQQQQ")
PKG = struct.Struct("<QIIIIBBHIQI20x")
DIR = struct.Struct("<IIIIQ")
FILE = struct.Struct("<IIQQQ32sBBH12x")
TLV = struct.Struct("<HHI")


class NupError(Exception):
    """A .nup that does not conform to the spec (fail closed)."""


class Package:
    __slots__ = ("index", "title_id", "root_dir", "exefs_dir", "romfs_dir", "version", "kind", "flags",
                 "meta_offset", "meta_size")

    def __init__(self, **kw) -> None:
        for k in self.__slots__:
            setattr(self, k, kw[k])

    @property
    def kind_name(self) -> str:
        return KIND_NAMES[self.kind]

    @property
    def version_str(self) -> str:
        v = self.version
        return f"{v >> 16}.{(v >> 8) & 0xFF}.{v & 0xFF}"


class Nup:
    """A validated .nup: the superblock's rules (SPEC §2), the TOC's sha256 before any TOC value is trusted
    (§6.1.1), bounds on every offset and size (§6.1.2), names as labels, never paths (§6.1.5)."""

    def __init__(self, path: Path) -> None:
        self.path = path
        self.fh = open(path, "rb")
        self.closed = False
        try:
            self._load()
        except BaseException:
            self.close()
            raise

    def close(self) -> None:
        if not self.closed:
            self.fh.close()
            self.closed = True

    def __enter__(self) -> "Nup":
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()

    def _load(self) -> None:
        size = self.fh.seek(0, os.SEEK_END)
        self.fh.seek(0)
        sb = self.fh.read(HEADER_SIZE)
        if len(sb) < HEADER_SIZE:
            raise NupError(f"{self.path.name}: smaller than the 128-byte superblock")
        magic, hsize, _hver, toc_off, toc_size, toc_sha, blob_off, file_size, pkg_count, flags, uuid, reserved = \
            SUPER.unpack(sb)
        if magic != MAGIC:
            raise NupError(f"bad magic {magic!r} (expected NUP\\x01)")
        if hsize < HEADER_SIZE or toc_off != hsize:
            raise NupError(f"v1 wants header_size == toc_offset == 0x80 (got {hsize:#x} / {toc_off:#x})")
        if blob_off % EXTENT_ALIGN or blob_off < toc_off + toc_size:
            raise NupError("blob_offset not 16-byte aligned or before the TOC's end")
        if file_size != size:
            raise NupError(f"superblock file_size {file_size:#x} != actual {size:#x}")
        if pkg_count < 1:
            raise NupError("no package")
        if flags:
            raise NupError(f"unknown superblock flags {flags:#x} (v1 has none)")
        if reserved != b"\0" * 32:
            raise NupError("superblock reserved bytes not zero")

        self.fh.seek(toc_off)
        toc = self.fh.read(toc_size)
        if len(toc) < toc_size:
            raise NupError("TOC truncated")
        if hashlib.sha256(toc).digest() != toc_sha:
            raise NupError("TOC sha256 mismatch: the file is corrupt or was edited")
        magic, pkg_count2, dir_count, file_count, pool_off, pool_size, tres = TOC_HEAD.unpack(toc[:48])
        if magic != TOC_MAGIC:
            raise NupError(f"bad TOC magic {magic!r}")
        if pkg_count2 != pkg_count:
            raise NupError("package_count differs between the superblock and the TOC")
        if tres:
            raise NupError("TOC reserved bytes not zero")
        rec_end = TOC_HEAD.size + pkg_count * PKG_SIZE + dir_count * DIR_SIZE + file_count * FILE_SIZE
        if rec_end > len(toc):
            raise NupError("TOC truncated: records past its end")
        if pool_off < rec_end or pool_off + pool_size > len(toc):
            raise NupError("string pool out of the TOC")
        if (pool_off - rec_end) % 4:
            raise NupError("children array not 4-byte aligned")
        self.pool = toc[pool_off:pool_off + pool_size]
        self.size = size

        self.packages: list[Package] = []
        pos = TOC_HEAD.size
        for i in range(pkg_count):
            title_id, root, exefs_d, romfs_d, version, kind, pflags, r1, r2, meta_off, meta_size = \
                PKG.unpack_from(toc, pos)
            pos += PKG_SIZE
            if kind not in KIND_NAMES:
                raise NupError(f"package {i}: unknown kind {kind}")
            if r1 or r2:
                raise NupError(f"package {i}: reserved bytes not zero")
            if pflags & ~FLAG_STANDALONE:
                raise NupError(f"package {i}: unknown package flags {pflags:#x}")
            if root >= dir_count:
                raise NupError(f"package {i}: root_dir out of bounds")
            for d, what in ((exefs_d, "exefs_dir"), (romfs_d, "romfs_dir")):
                if d != NO_DIR and d >= dir_count:
                    raise NupError(f"package {i}: {what} out of bounds")
            if meta_size and meta_off + meta_size > size:
                raise NupError(f"package {i}: meta block past the file's end")
            self.packages.append(Package(index=i, title_id=title_id, root_dir=root, exefs_dir=exefs_d,
                                         romfs_dir=romfs_d, version=version, kind=kind, flags=pflags,
                                         meta_offset=meta_off, meta_size=meta_size))
        # SPEC §3.1.2: an update without a base in the container must be marked standalone (flag bit 0)
        has_base = any(p.kind == KIND_BASE for p in self.packages)
        for p in self.packages:
            if p.kind == KIND_UPDATE and not has_base and not (p.flags & FLAG_STANDALONE):
                raise NupError(f"package {p.index}: an update without a base must be standalone (flags bit 0)")
        # §2.1: the uuid covers (title_id, version, kind) of every package. Not in §6.1's must list, so warn
        h = hashlib.sha256()
        for p in self.packages:
            h.update(struct.pack("<QIB", p.title_id, p.version, p.kind))
        if h.digest()[:16] != uuid:
            print("nup: warning: the superblock's uuid does not match its packages", file=sys.stderr)

        self.dirs: list[tuple[bytes, int, int]] = []  # (name, first_child, child_count)
        for i in range(dir_count):
            off, ln, first, count, r = DIR.unpack_from(toc, pos)
            pos += DIR_SIZE
            if r:
                raise NupError(f"dir {i}: reserved bytes not zero")
            self.dirs.append((self._pool_str(off, ln), first, count))
        self.files: list[tuple[bytes, int, bytes, int, int, int]] = []  # (name, size, sha, ext_off, ext_size, codec)
        for i in range(file_count):
            off, ln, fsize, eoff, esz, sha, codec, cflags, r2 = FILE.unpack_from(toc, pos)
            pos += FILE_SIZE
            if codec not in (CODEC_RAW, CODEC_ZSTD):
                raise NupError(f"file {i}: unknown codec {codec}")
            if cflags or r2:
                raise NupError(f"file {i}: reserved bytes not zero")
            if fsize == 0 and (eoff or esz or codec != CODEC_RAW):
                raise NupError(f"file {i}: zero size with an extent")
            if fsize and esz == 0:
                raise NupError(f"file {i}: nonzero size with an empty extent")
            if fsize and (eoff % EXTENT_ALIGN or eoff < blob_off or eoff + esz > size):
                raise NupError(f"file {i}: extent not 16-byte aligned or past the file")
            self.files.append((self._pool_str(off, ln), fsize, sha, eoff, esz, codec))
        nslots = (pool_off - pos) // 4
        self.children = struct.unpack_from(f"<{nslots}I", toc, pos) if nslots else ()
        used = 0
        for i, (name, first, count) in enumerate(self.dirs):
            if first + count > nslots:
                raise NupError(f"dir {i} ({name!r}): child range out of bounds")
            used += count
        if used != nslots:
            raise NupError(f"children: {used} slots used of {nslots}")

    def _pool_str(self, off: int, ln: int) -> bytes:
        if off + ln > len(self.pool):
            raise NupError("a name reference is past the string pool")
        return self.pool[off:off + ln]

    def meta(self, index: int) -> dict[int, bytes]:
        """The package's meta block (SPEC §5): TLV type -> payload (the first record of a type wins)."""
        p = self.packages[index]
        if not p.meta_size:
            return {}
        self.fh.seek(p.meta_offset)
        block = self.fh.read(p.meta_size)
        out: dict[int, bytes] = {}
        pos = 0
        while pos + TLV.size <= len(block):
            t, r, size = TLV.unpack_from(block, pos)
            if r or pos + TLV.size + size > len(block):
                raise NupError(f"package {index}: a meta TLV overruns the block")
            out.setdefault(t, block[pos + TLV.size:pos + TLV.size + size])
            pos += TLV.size + size
        return out

    def file_data(self, fi: int, fh=None) -> bytes:
        """One file's uncompressed bytes: its extent read, decompressed when the package compressed it.
        fh is the caller's read handle (a worker thread keeps its own; the default is this Nup's)."""
        name, fsize, _sha, eoff, esz, codec = self.files[fi]
        if fsize == 0:
            return b""
        fh = fh or self.fh
        fh.seek(eoff)
        data = fh.read(esz)
        if len(data) != esz:
            raise NupError(f"short read of {name!r}: {len(data)} of {esz} bytes")
        if codec == CODEC_RAW:
            if esz != fsize:
                raise NupError(f"{name!r}: raw extent {esz} bytes, the record says {fsize}")
            return data
        if zstd.get_frame_size(data) != len(data):
            raise NupError(f"{name!r}: a zstd extent with more than one frame (the spec wants one)")
        try:
            info = zstd.get_frame_info(data)
        except zstd.ZstdError as e:
            raise NupError(f"{name!r}: not a zstd frame ({e})")
        if info.decompressed_size is not None and info.decompressed_size != fsize:
            raise NupError(f"{name!r}: the zstd frame says {info.decompressed_size} bytes, the record {fsize}")
        try:
            out = zstd.decompress(data)
        except zstd.ZstdError as e:
            raise NupError(f"{name!r}: zstd failed ({e})")
        if len(out) != fsize:
            raise NupError(f"{name!r}: zstd gave {len(out)} bytes, the record says {fsize}")
        return out


def label(raw: bytes, index: int) -> str:
    """A node's name as a path component (SPEC §6.1.5: names are labels, never paths): UTF-8, not empty,
    not '.' or '..', no NUL, at most 255 bytes (the console's filename limit)."""
    try:
        name = raw.decode("utf-8")
    except UnicodeDecodeError:
        raise NupError(f"node {index}: the name is not UTF-8 ({raw[:24]!r}…)")
    if not name or name in (".", "..") or "\0" in name or len(raw) > 255:
        raise NupError(f"node {index}: unsafe name {name!r}")
    return name


def tree_files(nup: Nup, start: int) -> dict[tuple[str, ...], int]:
    """{relative path: file index} under a package directory, walking the children slots. Every directory
    is reached once (a second reach is a broken tree), child names stay unique per directory (the spec's
    producer rule), and the depth is bounded."""
    n_dirs = len(nup.dirs)
    out: dict[tuple[str, ...], int] = {}
    seen: set[int] = set()

    def rec(d: int, parts: tuple[str, ...]) -> None:
        if d in seen:
            raise NupError(f"directory {'/'.join(parts) or '<root>'} is reached twice: a broken tree")
        seen.add(d)
        _raw, first, count = nup.dirs[d]
        for cid in nup.children[first:first + count]:
            if cid < n_dirs:
                rec(cid, parts + (label(nup.dirs[cid][0], cid),))
            else:
                fi = cid - n_dirs
                key = parts + (label(nup.files[fi][0], fi),)
                if len(key) > MAX_DEPTH:
                    raise NupError(f"path deeper than {MAX_DEPTH} levels: {'/'.join(key)}")
                if key in out:
                    raise NupError(f"two files at {'/'.join(key)}")
                out[key] = fi

    rec(start, ())
    return out


def pick_program(nup: Nup) -> tuple[int, int]:
    """The program's package indices: (base or -1, highest update or -1). Add-ons are not programs."""
    base = next((p.index for p in nup.packages if p.kind == KIND_BASE), -1)
    updates = [p for p in nup.packages if p.kind == KIND_UPDATE]
    if not updates:
        return base, -1
    top = max(p.version for p in updates)
    winners = [p for p in updates if p.version == top]
    if len(winners) > 1:
        raise NupError(f"two update packages at version {top}")
    return base, winners[0].index


def layered(nup: Nup, base: int, upd: int, attr: str) -> dict[tuple[str, ...], int]:
    """The files a side holds once the update shadows the base (SPEC §3.1.2, path shadowing)."""
    out: dict[tuple[str, ...], int] = {}
    for pkg in (base, upd):
        if pkg < 0:
            continue
        d = getattr(nup.packages[pkg], attr)
        if d != NO_DIR:
            out.update(tree_files(nup, d))
    return out


def extract_one(nup: Nup, fi: int, dest: Path, verify: bool, rehash: bool, dup: dict[bytes, Path],
                fh=None, dup_lock=None) -> tuple[str, int]:
    """One file to dest: (state, bytes written) with state extracted / copied / skipped. Through
    <name>.part, renamed only once the size and the sha256 match the record (as prepare.py's copies).
    dup is shared between worker threads and guarded by dup_lock (absent for a single thread): a race on a
    first-write of the same sha only loses the copy, not the bytes."""
    lock = dup_lock or contextlib.nullcontext()
    name, fsize, sha, _eoff, _esz, _codec = nup.files[fi]
    if existing_size(dest) == fsize:
        if not (rehash and verify) or hashlib.sha256(dest.read_bytes()).digest() == sha:
            return "skipped", 0
    with lock:
        src = dup.get(sha)
    if src is not None and src.is_file() and src.stat().st_size == fsize:
        part = dest.with_name(dest.name + ".part")
        shutil.copyfile(src, part)
        part.replace(dest)
        return "copied", fsize
    data = nup.file_data(fi, fh)
    if verify and hashlib.sha256(data).digest() != sha:
        raise NupError(f"{name!r}: sha256 mismatch (the record says {sha.hex()[:16]}…)")
    part = dest.with_name(dest.name + ".part")
    with open(part, "wb") as f:
        f.write(data)
    part.replace(dest)
    with lock:
        dup[sha] = dest
    return "extracted", fsize


def ensure_dir(p: Path, made: set[Path]) -> None:
    if p not in made:
        p.mkdir(parents=True, exist_ok=True)
        made.add(p)


def existing_size(p: Path) -> int:
    """p's size if it is a regular file, else -1. One stat: on a NAS each extra round trip is ~40 ms."""
    try:
        st = p.stat()
    except OSError:
        return -1
    return st.st_size if statmod.S_ISREG(st.st_mode) else -1


def describe(nup: Nup) -> list[str]:
    """--list: one block per package, with the metadata of its meta block (SPEC §5)."""
    lines = [f"{nup.path.name}: {nup.size / 2 ** 30:.2f} GiB, {len(nup.packages)} package(s)"]
    base, upd = pick_program(nup)
    for p in nup.packages:
        meta = nup.meta(p.index)
        display = meta.get(TLV_DISPLAY, b"").decode("utf-8", "replace") or p.version_str
        title = meta.get(TLV_TITLE, b"").decode("utf-8", "replace")
        publisher = meta.get(TLV_PUBLISHER, b"").decode("utf-8", "replace")
        build = meta.get(TLV_BUILD_ID, b"")[:16].hex()
        source = meta.get(TLV_SOURCE, b"").decode("utf-8", "replace")
        flags = " standalone" if p.flags & FLAG_STANDALONE else ""
        lines.append(f"  [{p.index}] {p.kind_name} v{display} (packed {p.version:#x}){flags}  "
                     f"title {p.title_id:016x}")
        if title or publisher or build:
            lines.append(f"      {title} ({publisher})  build id {build}  \"{source}\"")
        for attr, unit, div in (("exefs_dir", "MiB", 20), ("romfs_dir", "GiB", 30)):
            d = getattr(p, attr)
            if d == NO_DIR:
                continue
            t = tree_files(nup, d)
            lines.append(f"      {attr[:-4]}: {len(t)} files, "
                         f"{sum(nup.files[i][1] for i in t.values()) / 2 ** div:.1f} {unit}")
    which = [f"package {i}" for i in (base, upd) if i >= 0]
    lines.append(f"program: {' + '.join(which) if which else 'none (add-ons only)'}")
    return lines


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("nup", type=Path, help="the .nup package")
    ap.add_argument("--out", type=Path, default=Path(os.environ.get("MKW_GAME", "game")),
                    help="exefs/ and romfs/ go to <out> (default: the game folder)")
    ap.add_argument("--list", action="store_true", help="print the container's packages and exit")
    ap.add_argument("--only", choices=("exefs", "romfs"), help="extract one side only")
    ap.add_argument("--package", type=int, help="extract this package (default: the highest update, else the base)")
    ap.add_argument("--no-verify", action="store_true", help="trust the records' sha256 without recomputing")
    ap.add_argument("--rehash", action="store_true", help="re-hash the files a resume would skip")
    ap.add_argument("--jobs", type=int, default=8, help="files extracted at once (default: 8)")
    args = ap.parse_args()

    def die(msg: str, code: int = 1) -> None:
        print(f"nup: {msg}", file=sys.stderr)
        sys.exit(code)

    try:
        nup = Nup(args.nup)
    except NupError as e:
        die(str(e))
    except OSError as e:
        die(str(e))

    try:
        if args.list:
            for line in describe(nup):
                print(line)
            return

        if args.package is not None:
            if not 0 <= args.package < len(nup.packages):
                die(f"--package {args.package}: the container has {len(nup.packages)} package(s)", 2)
            p = nup.packages[args.package]
            base = p.index if p.kind == KIND_BASE else -1
            upd = p.index if p.kind == KIND_UPDATE else -1
        else:
            base, upd = pick_program(nup)
        if base < 0 and upd < 0:
            die("the chosen package is an add-on: it has no program to extract", 2)

        sides: dict[str, dict[tuple[str, ...], int]] = {}
        if args.only != "romfs":
            sides["exefs"] = layered(nup, base, upd, "exefs_dir")
        if args.only != "exefs":
            sides["romfs"] = layered(nup, base, upd, "romfs_dir")
        work = [(side, rel, fi) for side in sides for rel, fi in sorted(sides[side].items())]
        total = len(work)
        if not total:
            die("nothing to extract: the package has no exefs or romfs")

        # Free space for what is not already there (Recompia's launcher checks; 23 GB on a half-full disk is not
        # an error to discover at file 200 000). A fresh destination wants the whole size; a resume scans once.
        args.out.mkdir(parents=True, exist_ok=True)
        if (args.out / "exefs").is_dir() or (args.out / "romfs").is_dir():
            need = 0
            for side, rel, fi in work:
                if existing_size(args.out / side / Path(*rel)) != nup.files[fi][1]:
                    need += nup.files[fi][1]
        else:
            need = sum(nup.files[fi][1] for _s, _r, fi in work)
        free = shutil.disk_usage(args.out).free
        if need > free:
            die(f"not enough space for {args.out}: {need / 2 ** 30:.1f} GiB wanted, {free / 2 ** 30:.1f} GiB free")

        # The destination is usually a NAS: ~40 ms per create+rename measured on ours (11/11/2026) while the
        # bytes are cheap (219 MB/s read, 102 MB/s write) — so the files run in parallel: each worker keeps its
        # own read handle on the package, the dedup map is shared under a lock.
        dup: dict[bytes, Path] = {}
        dup_lock = threading.Lock()
        counts: Counter[str] = Counter()
        written = 0

        def run_slice(items: list[tuple[str, tuple[str, ...], int]]) -> list[tuple[str, int]]:
            fh = open(nup.path, "rb")
            made: set[Path] = {args.out}
            out: list[tuple[str, int]] = []
            try:
                for side, rel, fi in items:
                    dest = args.out / side / Path(*rel)
                    ensure_dir(dest.parent, made)
                    out.append(extract_one(nup, fi, dest, not args.no_verify, args.rehash, dup, fh, dup_lock))
            finally:
                fh.close()
            return out

        jobs = max(1, args.jobs)
        chunk = max(1, total // (jobs * 8))
        slices = [work[i:i + chunk] for i in range(0, total, chunk)]
        t0 = time.monotonic()
        done, next_mark = 0, PROGRESS_EVERY
        with ThreadPoolExecutor(jobs) as pool:
            for results in pool.map(run_slice, slices):
                for state, n in results:
                    counts[state] += 1
                    written += n
                done += len(results)
                while next_mark <= done:
                    print(f"progress {next_mark} {total}", flush=True)
                    next_mark += PROGRESS_EVERY
        if done:
            print(f"progress {done} {total}", flush=True)
        dt = time.monotonic() - t0
        print(f"nup: {total} files in {dt / 60:.1f} min: {counts['extracted']} written, {counts['copied']} copied, "
              f"{counts['skipped']} already there; {written / 2 ** 30:.1f} GiB", file=sys.stderr)
    except NupError as e:
        die(str(e))
    except OSError as e:
        die(str(e))
    finally:
        nup.close()


if __name__ == "__main__":
    main()
