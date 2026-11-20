"""Read a Switch 2 .nup package: the dump format that packs a title's exefs/ and romfs/ into one file.

Why: players' dumps come out as .nup; the setup (prepare.py) and the shader extractor (tools/shaders/extract.py) read
them in place, and the runtime reads the RomFS from one too (runtime/sdk/fs/nup.c, which documents the layout).

Usage: uv run tools/prepare/nup.py list GAME.nup
       uv run tools/prepare/nup.py extract GAME.nup DIR [--part exefs|romfs]   (DIR/exefs, DIR/romfs)
       uv run tools/prepare/nup.py pack DIR GAME.nup --title 0400C3F00006E000 [--version 0]
            DIR/exefs and DIR/romfs into one package, stored raw (no zstd, so the runtime reads the RomFS in place):
            one file instead of ~206 000 for a dump kept loose (Adrià 13/11/2026)
As a module: Nup(path).files("romfs") -> {path: Entry}, .read(entry) -> bytes, .extract("exefs", dir).
"""

import argparse
import struct
import sys
from compression import zstd
from dataclasses import dataclass
from pathlib import Path

MAGIC = b"NUP\x01"
CHUNK = 8 << 20
ZSTD_OPTIONS = {zstd.DecompressionParameter.window_log_max: 31}  # frames written with zstd --long


@dataclass(frozen=True)
class Entry:
    offset: int  # of the stored bytes, in the package
    size: int
    stored: int
    zstd: bool


def is_nup(path: Path) -> bool:
    try:
        with open(path, "rb") as f:
            return f.read(4) == MAGIC
    except OSError:
        return False


def game_titles(titles: list[tuple]) -> list[tuple]:
    """The game's (title id, ...) records of a package, oldest version first (the table's order breaks a tie): a base
    game's and its update's ids differ only in the low 12 bits (0x800 on the console), a DLC's is higher (base +
    0x1000 and up), so the lowest id names the game and DLC is not layered into its RomFS."""
    game = min(t[0] & ~0xFFF for t in titles)
    return sorted((t for t in titles if t[0] & ~0xFFF == game), key=lambda t: t[4])


class Nup:
    """The package's game, its titles layered: a package can hold the base game and its updates as separate titles,
    an update's RomFS only what it adds or changes (Eglantine's --pack-nup writes it so), so a RomFS path is read from
    the newest title that has it, by version; the ExeFS is the newest one whole (an update's replaces all modules, as
    1.8.0's subsdk0 is another module). title_id and version are the newest's. Titles of another game (DLC) are left
    out. A package of one title (a dump, nup.py pack) reads as before."""

    def __init__(self, path: Path):
        self.path = Path(path)
        with open(self.path, "rb") as f:
            head = f.read(12)
            if head[:4] != MAGIC:
                raise ValueError(f"{path}: not a NUP package")
            if len(head) < 12:
                raise ValueError(f"{path}: NUP header is cut short")
            toc = struct.unpack_from("<I", head, 8)[0]
            f.seek(toc)
            th = f.read(0x30)
            if th[:4] != b"TOC\x01":
                raise ValueError(f"{path}: no table of contents")
            if len(th) < 0x30:
                raise ValueError(f"{path}: table of contents is cut short")
            nt = struct.unpack_from("<I", th, 4)[0]
            self._nd, nf, pool, pool_size = struct.unpack_from("<QQQQ", th, 8)
            f.seek(toc)
            self._t = f.read(pool + pool_size)
            if len(self._t) < pool + pool_size:
                raise ValueError(f"{path}: table of contents is cut short")
        self._pool = pool
        self._dirs = 0x30 + nt * 0x40
        self._files = self._dirs + self._nd * 24
        self._children = self._files + nf * 80
        self._titles = game_titles([struct.unpack_from("<QIIII", self._t, 0x30 + i * 0x40) for i in range(nt)])
        tid, _, _, _, self.version = self._titles[-1]
        self.title_id = f"{tid:016X}"
        self._parts: dict[str, dict[str, Entry]] = {}

    def _name(self, rec: int) -> str:
        off, n = struct.unpack_from("<II", self._t, rec)
        return self._t[self._pool + off : self._pool + off + n].decode("utf-8")

    def _walk(self, top: int) -> dict[str, Entry]:
        """The files under one title's part dir; none when the title has no such part (a dir index past the table)."""
        out: dict[str, Entry] = {}
        stack = [(top, "")] if top < self._nd else []
        while stack:
            d, prefix = stack.pop()
            _, _, first, count = struct.unpack_from("<4I", self._t, self._dirs + d * 24)
            for k in range(first, first + count):
                c = struct.unpack_from("<I", self._t, self._children + k * 4)[0]
                if c < self._nd:
                    stack.append((c, prefix + self._name(self._dirs + c * 24) + "/"))
                    continue
                rec = self._files + (c - self._nd) * 80
                size, off, stored = struct.unpack_from("<QQQ", self._t, rec + 8)
                codec = self._t[rec + 0x40]
                if codec > 1:
                    raise ValueError(f"{self.path}: unknown codec {codec} for {prefix + self._name(rec)}")
                out[prefix + self._name(rec)] = Entry(off, size, stored, codec == 1)
        return out

    def files(self, part: str) -> dict[str, Entry]:
        """{path under the part: Entry} of "exefs" or "romfs", sorted by path (the layering: see the class)."""
        if part not in self._parts:
            out: dict[str, Entry] = {}
            if part == "exefs":
                for t in reversed(self._titles):
                    if out := self._walk(t[2]):
                        break
            else:
                for t in self._titles:
                    out.update(self._walk(t[3]))
            self._parts[part] = dict(sorted(out.items()))
        return self._parts[part]

    def read(self, e: Entry) -> bytes:
        with open(self.path, "rb") as f:
            f.seek(e.offset)
            data = f.read(e.stored)
        if len(data) != e.stored:
            raise ValueError(f"{self.path}: short read at {e.offset:#x}")
        if e.zstd:
            data = zstd.decompress(data, options=ZSTD_OPTIONS)
        if len(data) != e.size:
            raise ValueError(f"{self.path}: a file at {e.offset:#x} has {len(data)} bytes, its table says {e.size}")
        return data

    def extract(self, part: str, dst: Path, progress=None, prefix: str = "") -> None:
        """The part's files (those under prefix) into dst; files already there with the right size are kept (resume).
        progress(done, total)."""
        files = {k: e for k, e in self.files(part).items() if k.startswith(prefix)}
        with open(self.path, "rb") as f:
            for n, (name, e) in enumerate(files.items(), 1):
                out = Path(dst, name)
                if not (out.is_file() and out.stat().st_size == e.size):
                    out.parent.mkdir(parents=True, exist_ok=True)
                    tmp = out.with_name(out.name + ".part")
                    with open(tmp, "wb") as o:
                        f.seek(e.offset)
                        left = e.stored
                        dec = zstd.ZstdDecompressor(options=ZSTD_OPTIONS) if e.zstd else None
                        while left:
                            buf = f.read(min(left, CHUNK))
                            if not buf:
                                raise ValueError(f"{self.path}: short read in {name}")
                            left -= len(buf)
                            o.write(dec.decompress(buf) if dec else buf)
                    if tmp.stat().st_size != e.size:
                        raise ValueError(f"{self.path}: {name} has {tmp.stat().st_size} bytes, its table says {e.size}")
                    tmp.replace(out)
                if progress and (n % 500 == 0 or n == len(files)):
                    progress(n, len(files))


def romfs_read(romfs: Path, rel: str) -> bytes:
    """One RomFS file of a game copy whose romfs is a folder or a .nup (game/romfs links to the package since the
    copies were packed, 13/11/2026)."""
    if not is_nup(romfs):
        return (romfs / rel).read_bytes()
    n = Nup(romfs)
    return n.read(n.files("romfs")[rel])


def romfs_dir(romfs: Path, sub: str, cache: Path) -> Path:
    """romfs as a folder holding sub: itself when loose, else sub unpacked from the package once into cache/sub."""
    if not is_nup(romfs):
        return romfs
    Nup(romfs).extract("romfs", cache, prefix=sub.rstrip("/") + "/")
    return cache


def pack(src: Path, dst: Path, title_id: int, version: int, progress=None) -> None:
    """src/exefs and src/romfs into dst as a package both readers take (this module and runtime/sdk/fs/nup.c), laid out
    as a dumped one: a 0x80-byte header, the TOC at 0x80 (its offset is a u32, so before the data), the dirs breadth
    first from the root (""), exefs and romfs, each dir's children contiguous. Files raw; the 32-byte hash is left zero
    (neither reader checks it). Written to dst.part, renamed when whole."""
    dirs: list[tuple[str, Path]] = [("", src), ("exefs", src / "exefs"), ("romfs", src / "romfs")]
    files: list[Path] = []
    kids: list[list[tuple[str, int]]] = []  # per dir: ("d" | "f", index)
    for d, (_, path) in enumerate(dirs):
        if d == 0:
            kids.append([("d", 1), ("d", 2)])
            continue
        entries = sorted(path.iterdir(), key=lambda p: p.name)
        mine = []
        for p in entries:
            if p.is_dir():
                dirs.append((p.name, p))
                mine.append(("d", len(dirs) - 1))
        for p in entries:
            if p.is_file():
                files.append(p)
                mine.append(("f", len(files) - 1))
        kids.append(mine)
    pool = bytearray()

    def name(s: str) -> tuple[int, int]:
        b = s.encode("utf-8")
        pool.extend(b)
        return len(pool) - len(b), len(b)

    nd, nf, nchild = len(dirs), len(files), sum(len(k) for k in kids)
    dir_names = [name(n) for n, _ in dirs]
    file_names = [name(p.name) for p in files]
    pool_off = 0x30 + 0x40 + nd * 24 + nf * 80 + nchild * 4
    toc_size = pool_off + len(pool)
    data = (0x80 + toc_size + 0xFFF) & ~0xFFF
    offsets, at = [], data
    for p in files:
        offsets.append(at)
        at = (at + p.stat().st_size + 15) & ~15
    toc = bytearray(b"TOC\x01")
    toc = (toc + struct.pack("<IQQQQ", 1, nd, nf, pool_off, len(pool))).ljust(0x30, b"\0")
    toc += struct.pack("<QIIII", title_id, 0, 1, 2, version).ljust(0x40, b"\0")
    first = 0
    for (off, n), k in zip(dir_names, kids, strict=True):
        toc += struct.pack("<4I8x", off, n, first, len(k))
        first += len(k)
    for (off, n), p, at_ in zip(file_names, files, offsets, strict=True):
        size = p.stat().st_size
        toc += struct.pack("<IIQQQ", off, n, size, at_, size) + bytes(32) + b"\0" * 16
    for k in kids:
        toc += b"".join(struct.pack("<I", i if kind == "d" else nd + i) for kind, i in k)
    toc += pool
    part = dst.with_name(dst.name + ".part")
    with open(part, "wb") as o:
        o.write((MAGIC + struct.pack("<II", 0x10080, 0x80) + struct.pack("<Q", toc_size)).ljust(0x80, b"\0"))
        o.write(toc)
        for n, (p, at_) in enumerate(zip(files, offsets, strict=True), 1):
            o.seek(at_)
            with open(p, "rb") as f:
                while buf := f.read(CHUNK):
                    o.write(buf)
            if progress and (n % 2000 == 0 or n == nf):
                progress(n, nf)
        o.truncate(at)
    part.replace(dst)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("list").add_argument("nup", type=Path)
    ex = sub.add_parser("extract")
    ex.add_argument("nup", type=Path)
    ex.add_argument("out", type=Path)
    ex.add_argument("--part", choices=("exefs", "romfs"), action="append")
    pk = sub.add_parser("pack")
    pk.add_argument("src", type=Path)
    pk.add_argument("out", type=Path)
    pk.add_argument("--title", required=True, help="title id, hex (Mario Kart World: 0400C3F00006E000)")
    pk.add_argument("--version", type=int, default=0, help="title version (1.0.0: 0; 1.8.0: 917504)")
    a = ap.parse_args()
    if a.cmd == "pack":
        pack(a.src, a.out, int(a.title, 16), a.version, lambda d, t: print(f"  {d}/{t}", end="\r", file=sys.stderr))
        print(f"{a.out}: done{' ' * 20}")
        return
    n = Nup(a.nup)
    print(f"title {n.title_id} v{n.version}: {len(n.files('exefs'))} exefs and {len(n.files('romfs'))} romfs files")
    if a.cmd == "extract":
        for part in a.part or ("exefs", "romfs"):
            n.extract(part, a.out / part, lambda d, t: print(f"  {part} {d}/{t}", end="\r", file=sys.stderr))
            print(f"{part}: done{' ' * 20}")


if __name__ == "__main__":
    main()
