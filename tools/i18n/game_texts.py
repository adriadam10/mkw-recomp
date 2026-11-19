# /// script
# requires-python = ">=3.14"
# dependencies = ["fonttools>=4.60"]
# ///
"""The game's own texts (romfs/Mals/<region><lang>/*.msarc: SARC archives of MSBT files, 1 328 texts per language):
dumped as "label<TAB>text" lines, and rebuilt in a language the game does not ship.

The dump is the vocabulary a menu translation reuses (Time Trials, Free Roam, the cups...). A game translation
(i18n/game/base/<lang>.tsv, same lines) holds only our text, never Nintendo's: build reads the player's own copy, replaces
each translated label (the rest stays in the base language: a partial translation works) and writes the archives with
the manifest.tsv of a RomFS overlay (runtime/sdk/fs/romfs.c), which the runtime serves in place of the originals. A
letter the game's fonts lack (Turkish ğ ş İ) is composed into a copy of them in the same overlay (game_fonts.py).

Labels are "<archive>/<msbt>/<label>", e.g. MsgMain/Menu/2203, the same in every language. A text's control tags
(colours, the player's name, plurals) are written {0E gggg tttt llll ...} and {0F gggg tttt}, in UTF-16 units as
hex: a translation keeps them as the dump with --tags gives them. \\n is a line break.

Usage:
  uv run tools/i18n/game_texts.py dump EUde [JPja ...] [-o DIR] [--tags]   every text of those folders
  uv run tools/i18n/game_texts.py todo ca            the base language's lines (with tags) ca.tsv still lacks
  uv run tools/i18n/game_texts.py build ca [-o DIR]  the overlay (default build/lang/ca): MKW_ROMFS_MODS=DIR
  uv run tools/i18n/game_texts.py list               the folders the game has
The game copy is game/ in the repository (a link to your copy) or MKW_GAME.
"""

import argparse
import hashlib
import os
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TRANSLATIONS = ROOT / "i18n" / "game"
BASE_TSV = TRANSLATIONS / "base"  # <lang>.tsv; the <version>/ folders beside it hold what each update reworded
# The game language each translation replaces (the one the game is told): Spain's languages over its Spanish, Turkish
# over the British English (discussion #593)
BASE = {"ca": "EUes", "gl": "EUes", "eu": "EUes", "tr": "EUen"}
TAG = re.compile(r"\{(0[EF](?: [0-9A-F]{4})*)\}")


def game_dir() -> Path:
    return Path(os.environ.get("MKW_GAME", ROOT / "game"))


def romfs_folder(sub: str) -> Path:
    """romfs/<sub> of the game copy; from a packed copy (game/romfs a .nup), unpacked once under build/ (Mals ~200 MB,
    Font ~35 MB)."""
    sys.path.insert(0, str(ROOT / "tools/prepare"))
    from nup import romfs_dir

    return romfs_dir(game_dir() / "romfs", sub, ROOT / "build/romfs-mals" / game_dir().resolve().name) / sub


# ---- SARC
def sarc_read(data: bytes) -> tuple[int, list[tuple[int, str]], dict[str, bytes]]:
    """A little-endian SARC: its data offset, its SFAT nodes (offset of the node, name) in order, its files by name."""
    if data[:4] != b"SARC" or data[6:8] != b"\xff\xfe":
        raise ValueError("not a little-endian SARC")
    (hdr_len,) = struct.unpack_from("<H", data, 4)
    (data_start,) = struct.unpack_from("<I", data, 12)
    sfat = hdr_len
    if data[sfat : sfat + 4] != b"SFAT":
        raise ValueError("no SFAT")
    sfat_len, count = struct.unpack_from("<HH", data, sfat + 4)
    sfnt = sfat + sfat_len + count * 16
    if data[sfnt : sfnt + 4] != b"SFNT":
        raise ValueError("no SFNT")
    nodes, files = [], {}
    for i in range(count):
        node = sfat + sfat_len + i * 16
        _hash, attr, start, end = struct.unpack_from("<IIII", data, node)
        name = f"#{i}"
        if attr & 0x01000000:
            off = sfnt + 8 + (attr & 0xFFFF) * 4
            name = data[off : data.index(b"\0", off)].decode()
        nodes.append((node, name))
        files[name] = data[data_start + start : data_start + end]
    return data_start, nodes, files


def sarc_files(data: bytes) -> dict[str, bytes]:
    return sarc_read(data)[2]


def sarc_replace(data: bytes, new: dict[str, bytes]) -> bytes:
    """The same SARC with some files' contents replaced: names, hashes and order kept, data packed again in node order
    with each file 8-aligned as the game's are."""
    data_start, nodes, files = sarc_read(data)
    out = bytearray(data[:data_start])
    for node, name in nodes:
        blob = new.get(name, files[name])
        while (len(out) - data_start) % 8:
            out.append(0)
        struct.pack_into("<II", out, node + 8, len(out) - data_start, len(out) - data_start + len(blob))
        out += blob
    struct.pack_into("<I", out, 8, len(out))
    return bytes(out)


# ---- MSBT
def msbt_sections(data: bytes) -> list[tuple[bytes, bytes]]:
    if data[:8] != b"MsgStdBn" or data[8:10] != b"\xff\xfe" or data[0x0C] != 1:
        raise ValueError("not a little-endian UTF-16 MSBT")
    (count,) = struct.unpack_from("<H", data, 0x0E)
    out, off = [], 0x20
    for _ in range(count):
        (size,) = struct.unpack_from("<I", data, off + 4)
        out.append((data[off : off + 4], data[off + 16 : off + 16 + size]))
        off += 16 + ((size + 15) & ~15)
    return out


def msbt_labels(body: bytes) -> dict[int, str]:
    (groups,) = struct.unpack_from("<I", body, 0)
    labels = {}
    for g in range(groups):
        n, q = struct.unpack_from("<II", body, 4 + g * 8)
        for _ in range(n):
            ln = body[q]
            (idx,) = struct.unpack_from("<I", body, q + 1 + ln)
            labels[idx] = body[q + 1 : q + 1 + ln].decode()
            q += 1 + ln + 4
    return labels


def txt2_raw(body: bytes) -> list[bytes]:
    (n,) = struct.unpack_from("<I", body, 0)
    offs = [struct.unpack_from("<I", body, 4 + i * 4)[0] for i in range(n)] + [len(body)]
    return [body[offs[i] : offs[i + 1]] for i in range(n)]


def msbt_texts(data: bytes, tags: bool = False) -> list[tuple[str, str]]:
    """(label, text) of an MSBT, in text order."""
    secs = dict(msbt_sections(data))
    labels = msbt_labels(secs[b"LBL1"])
    return [(labels.get(i, f"#{i}"), decode(raw, tags)) for i, raw in enumerate(txt2_raw(secs[b"TXT2"]))]


def msbt_replace(data: bytes, texts: dict[str, str]) -> bytes:
    """The same MSBT with the texts of some labels replaced (encode)."""
    secs = msbt_sections(data)
    labels = msbt_labels(dict(secs)[b"LBL1"])
    out = bytearray(data[:0x20])
    for magic, body in secs:
        if magic == b"TXT2":
            raws = txt2_raw(body)
            raws = [encode(texts[labels[i]]) if labels.get(i) in texts else r for i, r in enumerate(raws)]
            body = struct.pack("<I", len(raws))
            off = 4 + 4 * len(raws)
            for r in raws:
                body += struct.pack("<I", off)
                off += len(r)
            body += b"".join(raws)
        out += magic + struct.pack("<I", len(body)) + bytes(8) + body
        out += b"\xab" * (-len(out) % 16)
    struct.pack_into("<I", out, 0x12, len(out))
    return bytes(out)


def decode(raw: bytes, tags: bool) -> str:
    """UTF-16LE text without its final NUL; control tags (0x0E group type size args, 0x0F group type) as {0E ...}
    tokens, or dropped."""
    units = struct.unpack(f"<{len(raw) // 2}H", raw[: len(raw) // 2 * 2])
    out, run, i = [], [], 0

    def flush() -> None:
        text = struct.pack(f"<{len(run)}H", *run).decode("utf-16-le", "replace")
        out.append(text.replace("\\", "\\\\").replace("\n", "\\n").replace("\t", "\\t"))
        run.clear()

    while i < len(units) and units[i]:
        u = units[i]
        if u in (0x0E, 0x0F):
            n = 4 + units[i + 3] // 2 if u == 0x0E else 3
            flush()
            if tags:
                out.append("{" + " ".join([f"{u:02X}"] + [f"{x:04X}" for x in units[i + 1 : i + n]]) + "}")
            i += n
            continue
        run.append(u)
        i += 1
    flush()
    return "".join(out)


def encode(text: str) -> bytes:
    """decode's text with tags back to UTF-16LE with its NUL."""
    out = bytearray()
    for i, part in enumerate(TAG.split(text)):
        if i % 2:
            out += struct.pack(f"<{len(part.split())}H", *(int(x, 16) for x in part.split()))
        else:
            plain = re.sub(r"\\(.)", lambda m: {"n": "\n", "t": "\t"}.get(m[1], m[1]), part)
            out += plain.encode("utf-16-le")
    return bytes(out) + b"\0\0"


# ---- folders and translations
def folder_lines(folder: Path, tags: bool = False) -> list[tuple[str, str]]:
    lines = []
    for arc in sorted(folder.glob("*.msarc")):
        for name, blob in sorted(sarc_files(arc.read_bytes()).items()):
            if name.endswith(".msbt"):
                stem = f"{arc.stem}/{name.removesuffix('.msbt')}"
                lines += [(f"{stem}/{label}", text) for label, text in msbt_texts(blob, tags) if text]
    return lines


def game_version(exefs: Path) -> int:
    """RtGame.version of the copy (100: 1.0.0, 180: 1.8.0), 0 when its exefs is missing or of a build not listed."""
    sys.path.insert(0, str(ROOT / "recomp"))
    from versions import detect

    try:
        return detect(exefs).number
    except (OSError, ValueError):
        return 0


def read_translation(lang: str, version: int = 0) -> dict[str, str]:
    """<lang>.tsv, then each <version>/<lang>.tsv up to the copy's version (i18n/game/1.8.0/ca.tsv: the texts 1.8.0
    reworded or retagged, which keep their 1.0.0 translation in <lang>.tsv for a 1.0.0 copy)."""
    sys.path.insert(0, str(ROOT / "recomp"))
    from versions import VERSIONS

    numbers = {v.name: v.number for v in VERSIONS}
    out = read_tsv(BASE_TSV / f"{lang}.tsv")
    for sub in sorted((d for d in TRANSLATIONS.iterdir() if d.name in numbers), key=lambda d: numbers[d.name]):
        if numbers[sub.name] <= version and (sub / f"{lang}.tsv").is_file():
            out |= read_tsv(sub / f"{lang}.tsv")
    return out


def read_tsv(path: Path) -> dict[str, str]:
    out = {}
    for n, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not line or line.startswith("#"):
            continue
        label, sep, text = line.partition("\t")
        if not sep:
            raise SystemExit(f"{path}:{n}: no tab between the label and the text")
        out[label] = text
    return out


def mismatched(source: dict[str, str], texts: dict[str, str]) -> list[str]:
    """The translated labels the base language lacks or whose tags differ from it (tags compared in order)."""
    bad = [k for k in texts if k not in source]
    return bad + [k for k in texts if k in source and TAG.findall(texts[k]) != TAG.findall(source[k])]


def build(lang: str, out: Path, romfs: Path | None = None, strict: bool = True, version: int = 0) -> None:
    import game_fonts

    """strict (the build command, on the copy the translation is made from): a mismatch stops. Not strict (the
    setup): the translation was made on one game version and the player may have another (1.8.0 drops or retags
    labels of 1.0.0, so the setup stopped at its last step for every 1.8.0 dump); those texts stay in the base
    language, as a label the translation leaves out does."""
    base = BASE[lang]
    folder = (romfs or game_dir() / "romfs") / "Mals" / base
    texts = read_translation(lang, version)
    source = dict(folder_lines(folder, tags=True))
    bad = mismatched(source, texts)
    # labels of another version (1.8.0 dropped some of 1.0.0, and has new ones) are expected: only changed tags stop
    retagged = [k for k in bad if k in source]
    if retagged and strict:
        raise SystemExit(f"{lang}: changed tags: {', '.join(retagged[:10])}")
    if bad:
        print(
            f"{lang}: {len(bad)} of {len(texts)} texts do not match this copy of the game, left in {base}",
            file=sys.stderr,
        )
        drop = set(bad)
        texts = {k: v for k, v in texts.items() if k not in drop}
    manifest = []
    for arc in sorted(folder.glob("*.msarc")):
        orig = arc.read_bytes()
        new = {}
        for name, blob in sarc_files(orig).items():
            stem = f"{arc.stem}/{name.removesuffix('.msbt')}/"
            mine = {k.removeprefix(stem): v for k, v in texts.items() if k.startswith(stem)}
            if mine:
                new[name] = msbt_replace(blob, mine)
        if not new:
            continue
        data = sarc_replace(orig, new)
        rel = f"Mals/{base}/{arc.name}"
        (out / rel).parent.mkdir(parents=True, exist_ok=True)
        (out / rel).write_bytes(data)
        sha = hashlib.sha256
        manifest.append(f"{rel}\t{len(data)}\t{sha(data).hexdigest()}\t{sha(orig).hexdigest()}")
    font_dir = romfs / "Font" if romfs else romfs_folder("Font")
    manifest += game_fonts.build(font_dir, "".join(texts.values()), out)
    out.mkdir(parents=True, exist_ok=True)
    (out / "manifest.tsv").write_text(f"# {lang} over {base}, from i18n/game/base/{lang}.tsv\n" + "\n".join(manifest) + "\n")
    n = len(folder_lines(folder))
    print(f"{lang}: {len(texts)} of {n} texts translated, {len(manifest)} archives in {out}", file=sys.stderr)


def build_all(game: Path, romfs: Path) -> None:
    """Every translation into game/lang/<lang>/ (the setup's last step, tools/prepare/prepare.py)."""
    version = game_version(game / "exefs")
    for path in sorted(BASE_TSV.glob("*.tsv")):
        build(path.stem, game / "lang" / path.stem, romfs, strict=False, version=version)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    d = sub.add_parser("dump", help="every text of some romfs/Mals folders")
    d.add_argument("folders", nargs="+", help="e.g. EUde JPja")
    d.add_argument("-o", "--out", type=Path, help="write <folder>.tsv files here instead of stdout")
    d.add_argument("--tags", action="store_true", help="keep the control tags as {0E ...} tokens")
    t = sub.add_parser("todo", help="the base language's lines a translation still lacks")
    t.add_argument("lang", choices=sorted(BASE))
    b = sub.add_parser("build", help="a translation's RomFS overlay")
    b.add_argument("lang", choices=sorted(BASE))
    b.add_argument("-o", "--out", type=Path)
    sub.add_parser("list", help="the folders the game has")
    args = ap.parse_args()
    mals = romfs_folder("Mals")
    if args.cmd == "list":
        print(" ".join(sorted(p.name for p in mals.iterdir() if p.is_dir())))
    elif args.cmd == "dump":
        for f in args.folders:
            text = "".join(f"{k}\t{v}\n" for k, v in folder_lines(mals / f, args.tags))
            if args.out:
                args.out.mkdir(parents=True, exist_ok=True)
                (args.out / f"{f}.tsv").write_text(text, encoding="utf-8")
            else:
                sys.stdout.write(text)
    elif args.cmd == "todo":
        path = BASE_TSV / f"{args.lang}.tsv"
        done = read_translation(args.lang, game_version(game_dir() / "exefs")) if path.exists() else {}
        sys.stdout.write("".join(f"{k}\t{v}\n" for k, v in folder_lines(mals / BASE[args.lang], True) if k not in done))
    else:
        romfs_folder("Font")
        build(args.lang, args.out or ROOT / "build" / "lang" / args.lang, mals.parent,
              version=game_version(game_dir() / "exefs"))  # fmt: skip
    return 0


if __name__ == "__main__":
    sys.exit(main())
