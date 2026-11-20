"""A mod's exefs patches (.ips / IPS32 or .pchtxt, the emulators' format) as the generator's code patches.

Why: the port has no executable to byte-patch at run time: a code change has to be lifted into the generated C, as a
`site` (one instruction replaced, switchable) or a `span` (a region skipped, for a branch over code), the shapes of
recomp/patches.py. GameBanana's code mods (200cc, Classic Grand Prix, 30 FPS...) ship .ips files named by the build
id of the module they patch; this turns them into a mod.json "patches" block, so a player installs them as they come.

What converts: in .text, a changed word that is not a branch becomes a site (a bl too: the lifter calls its target),
and an unconditional forward b to a later instruction becomes a span over the region it skips. What does not (the
mod is refused, saying where): a conditional, backward or register branch, a b out of reach of the code, a write
outside .text (data), or a build id of no module of this game.

Usage: ips.py --exefs DIR --name NAME FILE...    prints the mod.json "patches" block (JSON) and "switches"
As a module: convert(exefs, name, files) -> (patches, switches) or raises PatchFailed(reason).
"""

import argparse
import json
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "recomp"))
from layout import layout  # noqa: E402
from nso import Nso  # noqa: E402

NSO_HEADER = 0x100   # IPS offsets count the NSO header: module offset = IPS offset - 0x100 (pchtxt's offset_shift)


class PatchFailed(ValueError):
    pass


def parse_ips(data: bytes) -> list[tuple[int, bytes]]:
    """(offset, bytes) records of an IPS (24-bit offsets) or IPS32 (32-bit) patch, RLE records expanded."""
    if data.startswith(b"IPS32"):
        pos, width, eof = 5, 4, b"EEOF"
    elif data.startswith(b"PATCH"):
        pos, width, eof = 5, 3, b"EOF"
    else:
        raise PatchFailed("not an IPS patch")
    out = []
    while data[pos:pos + len(eof)] != eof:   # the end mark where a record would start (IPS may add 3 bytes after it)
        if pos + width + 2 > len(data):
            raise PatchFailed("IPS ends early")
        off = int.from_bytes(data[pos:pos + width], "big")
        size = struct.unpack_from(">H", data, pos + width)[0]
        pos += width + 2
        if size:
            out.append((off, data[pos:pos + size]))
            pos += size
        else:   # RLE: a 2-byte count and the byte
            if pos + 3 > len(data):   # a cut download: struct.error would end the import with a traceback
                raise PatchFailed("IPS ends early")
            n = struct.unpack_from(">H", data, pos)[0]
            out.append((off, data[pos + 2:pos + 3] * n))
            pos += 3
    return out


def parse_pchtxt(text: str) -> tuple[str, list[tuple[int, bytes]]]:
    """(build id, (offset, bytes) records) of a .pchtxt: the enabled sections' lines "OFFSET BYTES", offsets shifted
    by @flag offset_shift (0x100 when it says so, as for IPS)."""
    bid, shift, on, out = "", 0, True, []
    for raw in text.splitlines():
        line = raw.split("//")[0].strip()
        if not line:
            continue
        if line.startswith("@nsobid-"):
            bid = line[8:].strip().lower()
        elif line.startswith("@flag offset_shift"):
            try:
                shift = int(line.split()[-1], 0)
            except ValueError:   # import.py refuses a PatchFailed with a reason; a bare ValueError was a traceback
                raise PatchFailed(f"pchtxt line not understood: {line}") from None
        elif line in ("@enabled", "@disabled"):
            on = line == "@enabled"
        elif line == "@stop":
            break
        elif on and not line.startswith("@"):
            m = re.fullmatch(r"([0-9A-Fa-f]+)\s+([0-9A-Fa-f]+)", line)
            if not m or len(m.group(2)) % 2:
                raise PatchFailed(f"pchtxt line not understood: {line}")
            out.append((int(m.group(1), 16) + shift, bytes.fromhex(m.group(2))))
    return bid, out


def is_branch(w: int) -> bool:
    return (w >> 26) & 0x1F == 0x05 or w & 0xFF000010 == 0x54000000 or w & 0x7E000000 in (0x34000000, 0x36000000) \
        or w & 0xFE1FFC1F == 0xD61F0000   # b/bl, b.cond, cbz/cbnz, tbz/tbnz, br/blr/ret


def module_words(exefs: Path) -> dict[str, tuple[str, Nso]]:
    """{build id (16 hex): (module name, its NSO)} of the game's modules."""
    out = {}
    for name, _, _ in layout(exefs):
        n = Nso(str(Path(exefs) / name))
        out[n.build_id[:16]] = (name, n)
    return out


def word_at(n: Nso, off: int) -> int:
    return struct.unpack_from("<I", n.image, off)[0]


def records_to_patches(module: str, n: Nso, records, set_name: str, env: str) -> tuple[list, list]:
    text_lo, text_hi = n.text[0], n.text[0] + n.text[1]
    new: dict[int, int] = {}   # word offset in the module -> its patched word
    for off, data in records:
        off -= NSO_HEADER
        for i, b in enumerate(data):
            a = off + i
            if not text_lo <= a < text_hi:
                raise PatchFailed(f"{module}+{a:#x} is outside the code (.text): data patches are not supported")
            w = a & ~3
            cur = new.get(w, word_at(n, w))
            shift = (a - w) * 8
            new[w] = (cur & ~(0xFF << shift)) | (b << shift)
    sites, spans = [], []
    for w in sorted(new):
        orig, repl = word_at(n, w), new[w]
        if orig == repl:
            continue
        if not is_branch(repl) or (repl >> 26) == 0x25:   # bl: the lifter calls the target and comes back
            sites.append({"set": set_name, "env": env, "module": module, "off": f"{w:x}", "orig": f"{orig:08X}",
                          "repl": [f"{repl:08X}"]})
            continue
        if (repl >> 26) != 0x05:
            raise PatchFailed(f"{module}+{w:#x}: a conditional or register branch ({repl:08x}) cannot be converted")
        delta = (((repl & 0x3FFFFFF) ^ 0x2000000) - 0x2000000) * 4
        target = w + delta
        if delta <= 0 or target > text_hi:
            raise PatchFailed(f"{module}+{w:#x}: a backward or out-of-code b ({repl:08x}) cannot be converted")
        spans.append({"set": set_name, "env": env, "module": module, "off": f"{w:x}",
                      "words": [f"{word_at(n, x):08X}" for x in range(w, target, 4)]})
    # a site inside a span would be skipped anyway, and the generator refuses the overlap: drop it
    covered = [(int(s["off"], 16), int(s["off"], 16) + 4 * len(s["words"])) for s in spans]
    sites = [s for s in sites if not any(lo <= int(s["off"], 16) < hi for lo, hi in covered)]
    return sites, spans


def convert(exefs: Path, name: str, files: list[Path]) -> tuple[dict, dict]:
    """The mod.json "patches" and "switches" for the exefs files of mod name (.ips named by build id, .pchtxt)."""
    mods = module_words(exefs)
    key = re.sub(r"[^A-Z0-9]+", "_", name.upper()).strip("_") or "MOD"
    set_name, env = key.lower(), f"MKW_{key}"
    by_build: dict[str, list] = {}
    ips_builds = {f.stem.lower()[:16] for f in files if f.suffix.lower() == ".ips"}
    for f in sorted(files):
        if f.suffix.lower() == ".ips":
            by_build.setdefault(f.stem.lower()[:16], []).extend(parse_ips(f.read_bytes()))
        elif f.suffix.lower() == ".pchtxt":
            bid, recs = parse_pchtxt(f.read_text(encoding="utf-8", errors="replace"))
            if bid[:16] not in ips_builds:   # the same patch as an .ips beside it is usual: the .ips wins
                by_build.setdefault(bid[:16], []).extend(recs)
    if not by_build:
        raise PatchFailed("no .ips or .pchtxt in exefs/")
    sites, spans = [], []
    for bid, recs in by_build.items():
        if bid not in mods:
            raise PatchFailed(f"built for {bid}, which is no module of this game copy (another game version?)")
        module, nso = mods[bid]
        s, p = records_to_patches(module, nso, recs, set_name, env)
        sites += s
        spans += p
    if not sites and not spans:
        raise PatchFailed("its patches change nothing in this game copy")
    return {"sites": sites, "spans": spans}, {key: "1"}


def image_patches(exefs: Path, files: list[Path]) -> list[dict]:
    """The exefs files' bytes as {"module", "off" (hex, module offset), "bytes" (hex)} patches of the game's own code,
    applied to the module image before the generator lifts it (recomp/gen.py --image): every kind of code change goes
    (new functions in .text's padding, branches, returns), not data. Refuses another build or a write outside .text."""
    mods = module_words(exefs)
    ips_builds = {f.stem.lower()[:16] for f in files if f.suffix.lower() == ".ips"}
    by_build: dict[str, list] = {}
    for f in sorted(files):
        if f.suffix.lower() == ".ips":
            by_build.setdefault(f.stem.lower()[:16], []).extend(parse_ips(f.read_bytes()))
        elif f.suffix.lower() == ".pchtxt":
            bid, recs = parse_pchtxt(f.read_text(encoding="utf-8", errors="replace"))
            if bid[:16] not in ips_builds:   # usually the same patch as the .ips beside it: the .ips wins
                by_build.setdefault(bid[:16], []).extend(recs)
    if not by_build:
        raise PatchFailed("no .ips or .pchtxt in exefs/")
    out = []
    for bid, recs in by_build.items():
        if bid not in mods:
            raise PatchFailed(f"built for {bid}, which is no module of this game copy (another game version?)")
        module, n = mods[bid]
        lo, hi = n.text[0], n.text[0] + n.text[1]
        for off, data in recs:
            off -= NSO_HEADER
            if off < lo or off + len(data) > hi:
                raise PatchFailed(f"{module}+{off:#x} is outside the code (.text): data patches are not supported")
            if n.image[off:off + len(data)] != data:
                out.append({"module": module, "off": f"{off:x}", "bytes": data.hex()})
    if not out:
        raise PatchFailed("its patches change nothing in this game copy")
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--exefs", type=Path, required=True)
    ap.add_argument("--name", required=True)
    ap.add_argument("files", type=Path, nargs="+")
    args = ap.parse_args()
    try:
        patches, switches = convert(args.exefs, args.name, args.files)
    except PatchFailed as e:
        print(f"error {e}")
        return 1
    print(json.dumps({"switches": switches, "patches": patches}, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
