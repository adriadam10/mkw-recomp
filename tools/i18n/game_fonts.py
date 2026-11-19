# /// script
# requires-python = ">=3.14"
# dependencies = ["fonttools>=4.60"]
# ///
"""The game's fonts with the letters a translation needs and they lack, composed from their own pieces.

Why: the fonts of the game's European text (FOT-RodinNTLGPro EB and UB, which the race font sets fall back to for
every letter, and VDL-LogoG) have ç ö ü ı but not ğ Ğ ş Ş İ, so a Turkish text would show gaps (discussion #593). Each
missing letter is drawn here from the same font: the base letter plus the breve, dot or cedilla the font already has,
placed where that font places the same kind of mark (the diaeresis of ö/Ö, the cedilla of ç/Ç), so it keeps the font's
weight and style. Nothing of Nintendo's is shipped: the setup composes them from the player's own copy.

Font/<name>.bfotf.zs: zstd of a "bfotf", an OpenType font after an 8-byte header (the magic d9 9b 87 1a, then the
font's size, big-endian; padding may follow) with size and font XORed with the key a6 01 85 02 (measured 18/11/2026).

Usage:
  uv run tools/i18n/game_fonts.py missing tr           the letters of i18n/game/base/tr.tsv each game font lacks
  uv run tools/i18n/game_fonts.py build tr -o DIR      the composed fonts and their manifest lines (game_texts.py build)
The game copy is game/ in the repository (a link to your copy) or MKW_GAME.
"""

import argparse
import hashlib
import struct
import sys
from compression import zstd
from pathlib import Path

from fontTools.pens.boundsPen import BoundsPen
from fontTools.pens.recordingPen import DecomposingRecordingPen
from fontTools.pens.t2CharStringPen import T2CharStringPen
from fontTools.pens.transformPen import TransformPen
from fontTools.ttLib import TTFont

KEY = bytes.fromhex("a6018502")
MAGIC = bytes.fromhex("d99b871a")
# The fonts the European texts are drawn with (Font/*.bfcpx, the layouts' fnl1 lists). NinDev-RaceFontMoveR and
# nintendoP_RodinNTLG-EB already have every Latin letter.
FONTS = ("FOT-RodinNTLGPro-EB", "FOT-RodinNTLGPro-UB", "VDL-LogoG-Heavy", "VDL-LOGOG-ULTRA")
# letter -> (base, mark, reference letter whose mark sets the height, the reference's base)
ABOVE, BELOW = "above", "below"
RECIPES = {
    "ğ": ("g", "̆", ABOVE, "ö", "o"),
    "Ğ": ("G", "̆", ABOVE, "Ö", "O"),
    "İ": ("I", "̇", ABOVE, "Ö", "O"),
    "ş": ("s", "¸", BELOW, "ç", "c"),
    "Ş": ("S", "¸", BELOW, "Ç", "C"),
}


def decrypt(data: bytes) -> bytes:
    if data[:4] != MAGIC:
        raise ValueError("not a bfotf")
    (size,) = struct.unpack(">I", xor(data[4:8], 4))
    if size > len(data) - 8:  # VDL-LogoG-Heavy has one byte of padding after its font
        raise ValueError(f"bfotf of {len(data)} bytes says {size} after its header")
    return xor(data[8 : 8 + size], 8)


def encrypt(font: bytes) -> bytes:
    return MAGIC + xor(struct.pack(">I", len(font)), 4) + xor(font, 8)


def xor(data: bytes, start: int) -> bytes:
    """data XORed with the key as it falls at offset start of the file (the key restarts at offset 0)."""
    n = len(data)
    key = (KEY * (n // 4 + 2))[start % 4 : start % 4 + n]
    return (int.from_bytes(data, "little") ^ int.from_bytes(key, "little")).to_bytes(n, "little")


def bounds(glyphs, name: str) -> tuple[float, float, float, float]:
    pen = BoundsPen(glyphs)
    glyphs[name].draw(pen)
    return pen.bounds


def contours(glyphs, name: str) -> list[tuple[float, float, float, float]]:
    """The control-point box of each contour of a glyph."""
    pen = DecomposingRecordingPen(glyphs)
    glyphs[name].draw(pen)
    out, pts = [], []
    for op, args in pen.value:
        pts += args
        if op in ("closePath", "endPath") and pts:
            xs, ys = [p[0] for p in pts], [p[1] for p in pts]
            out.append((min(xs), min(ys), max(xs), max(ys)))
            pts = []
    return out


def mark_shift(glyphs, cmap: dict[int, str], recipe: tuple[str, str, str, str, str]) -> tuple[float, float]:
    """How far the mark glyph moves: centred on the base letter, at the height the reference letter's own mark has."""
    base, mark, where, ref, ref_base = recipe
    bx0, _, bx1, _ = bounds(glyphs, cmap[ord(base)])
    mx0, my0, mx1, my1 = bounds(glyphs, cmap[ord(mark)])
    dx = (bx0 + bx1) / 2 - (mx0 + mx1) / 2
    if where == BELOW:
        # the cedilla can be one contour with the c: the reference's lowest point is the mark's
        return dx, bounds(glyphs, cmap[ord(ref)])[1] - my0
    top = bounds(glyphs, cmap[ord(ref_base)])[3]
    above = [c for c in contours(glyphs, cmap[ord(ref)]) if c[1] > top - 5]
    if not above:
        raise ValueError(f"no mark above {ref}")
    return dx, min(c[1] for c in above) - my0


def compose(font: TTFont, letters: str) -> list[str]:
    """Add each of letters the font lacks and has a recipe for; returns the letters added."""
    cmap = font.getBestCmap()
    glyphs = font.getGlyphSet()
    top = font["CFF "].cff.topDictIndex[0]
    if not hasattr(top, "ROS"):
        raise ValueError("not a CID-keyed CFF font")
    added = []
    for ch in letters:
        recipe = RECIPES.get(ch)
        if ord(ch) in cmap or not recipe or any(ord(c) not in cmap for c in recipe[:2] + recipe[3:]):
            continue
        base, mark = cmap[ord(recipe[0])], cmap[ord(recipe[1])]
        dx, dy = mark_shift(glyphs, cmap, recipe)
        fd = top.FDArray[top.FDSelect[font.getGlyphID(base)]]
        width = glyphs[base].width
        pen = T2CharStringPen(None if width == fd.Private.defaultWidthX else width - fd.Private.nominalWidthX, glyphs)
        glyphs[base].draw(pen)
        glyphs[mark].draw(TransformPen(pen, (1, 0, 0, 1, dx, dy)))
        name = f"cid{len(font.getGlyphOrder()):05d}"
        add_glyph(font, top, name, pen.getCharString(private=fd.Private, globalSubrs=top.CharStrings.globalSubrs),
                  top.FDSelect[font.getGlyphID(base)], base)  # fmt: skip
        for table in font["cmap"].tables:
            if table.isUnicode():
                table.cmap[ord(ch)] = name
        added.append(ch)
    return added


def add_glyph(font: TTFont, top, name: str, charstring, fd: int, like: str) -> None:
    """A new last glyph: its charstring, CID and font dict, and the metrics of the glyph it is drawn on."""
    cs = top.CharStrings
    cs.charStrings[name] = len(cs.charStringsIndex)
    cs.charStringsIndex.append(charstring)
    top.FDSelect.gidArray.append(fd)
    order = font.getGlyphOrder() + [name]  # a new list: the CFF charset may be the font's glyph order list itself
    top.charset = order
    font.setGlyphOrder(order)
    font["hmtx"].metrics[name] = font["hmtx"].metrics[like]
    if "vmtx" in font:
        font["vmtx"].metrics[name] = font["vmtx"].metrics[like]
    if "VORG" in font and like in font["VORG"].VOriginRecords:
        font["VORG"].VOriginRecords[name] = font["VORG"].VOriginRecords[like]


def read_font(font_dir: Path, name: str) -> bytes:
    """Font/<name>.bfotf(.zs) of the copy, packed as it is in the RomFS."""
    path = font_dir / f"{name}.bfotf.zs"
    return path.read_bytes() if path.exists() else zstd.compress((font_dir / f"{name}.bfotf").read_bytes())


def missing(font_dir: Path, text: str) -> dict[str, str]:
    """For each font, the letters of text it lacks (spaces and control characters aside)."""
    import io

    need = {c for c in text if c.isprintable() and not c.isspace()}
    out = {}
    for name in FONTS:
        font = TTFont(io.BytesIO(decrypt(zstd.decompress(read_font(font_dir, name)))), lazy=True)
        out[name] = "".join(sorted(c for c in need if ord(c) not in font.getBestCmap()))
    return out


def build(font_dir: Path, text: str, out: Path) -> list[str]:
    """The fonts lacking a letter of text, with it composed, into out/Font/; their overlay manifest lines. The letters
    no recipe covers are reported and left as the game draws them."""
    import io

    lines = []
    sha = hashlib.sha256
    tool = sha(Path(__file__).read_bytes()).hexdigest()[:16]
    for name, lack in missing(font_dir, text).items():
        if not lack:
            continue
        orig = read_font(font_dir, name)
        rel = f"Font/{name}.bfotf.zs"
        # the setup runs this every time (~6 s a font): the last result kept while the font, letters and tool match
        key = f"{sha(orig).hexdigest()} {lack} {tool}\n"
        key_path = out / f"{rel}.key"
        if (out / rel).is_file() and key_path.is_file() and key_path.read_text(encoding="utf-8") == key:
            data = (out / rel).read_bytes()
            lines.append(f"{rel}\t{len(data)}\t{sha(data).hexdigest()}\t{sha(orig).hexdigest()}")
            continue
        font = TTFont(io.BytesIO(decrypt(zstd.decompress(orig))))
        added = compose(font, lack)
        left = "".join(c for c in lack if c not in added)
        if left:
            print(f"{name}: no recipe for {left}", file=sys.stderr)
        if not added:
            continue
        buf = io.BytesIO()
        font.save(buf)
        data = zstd.compress(encrypt(buf.getvalue()))
        (out / rel).parent.mkdir(parents=True, exist_ok=True)
        (out / rel).write_bytes(data)
        key_path.write_text(key, encoding="utf-8")
        lines.append(f"{rel}\t{len(data)}\t{sha(data).hexdigest()}\t{sha(orig).hexdigest()}")
        print(f"{name}: {''.join(added)} composed", file=sys.stderr)
    return lines


def main() -> int:
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import game_texts

    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("cmd", choices=("missing", "build"))
    ap.add_argument("lang", help="a translation in i18n/game/base, e.g. tr")
    ap.add_argument("-o", "--out", type=Path, help="build: the overlay folder (default build/lang/<lang>)")
    args = ap.parse_args()
    font_dir = game_texts.romfs_folder("Font")
    text = "".join(game_texts.read_translation(args.lang, 1 << 30).values())
    if args.cmd == "missing":
        for name, lack in missing(font_dir, text).items():
            print(f"{name}: {lack or '-'}")
        return 0
    out = args.out or game_texts.ROOT / "build" / "lang" / args.lang
    print("\n".join(build(font_dir, text, out)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
