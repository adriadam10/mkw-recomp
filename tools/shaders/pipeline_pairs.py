"""Write pipeline-pairs.bin: every vertex + fragment program pair of the player's game copy, by program key.

Why: the boot prewarm (runtime/gfx/vk/pipe/vk_pipe_pairs.c) builds the shader libraries of every pair before the game
draws, so no draw is skipped while its vertex or fragment library compiles; the pairs come from the copy (extract.py's
pairs.csv), not from runs. A program key is sha256 of a stage's control section and code (the prepared SPIR-V's name,
gfx_shader_load_input).

Every vertex or fragment stage in no pair goes in alone, its partner all zero: agl's full-screen passes are programs
with a fragment stage only, drawn with another program's vertex stage (a replay of build/cap/race, 17/11/2026: 7 of
its 9 fragment libraries not built were such), and a fragment library does not depend on the vertex stage.

A record's fragment stage from exefs/main or a Layout archive (nn::ui2d's shaders) is marked as one the game may draw
with NVN's alpha test, and the prewarm also builds its fragment library with the test's functions the game uses: in
6 165 recorded pipelines (17/11/2026) the test was on in 32, all with GREATER or GEQUAL and all with one of 4 fragment
programs, three of exefs/main and one of a Layout archive (91 fragment programs in those sources).

Every compute program goes in too, flagged, in the vertex slot with no fragment stage: the prewarm builds its compute
pipeline, which the game otherwise made on the drawing thread at its first dispatch, waiting for it (18/11/2026); so do
the mesh programs ("tes?" in index.csv), which the runtime runs as compute too.

Usage: python tools/shaders/pipeline_pairs.py [--shaders build/shaders] [--out build/pipeline-pairs.bin]
Layout: "MKW_PAIR", u32 version 3, u32 record count, then per record the vertex and the fragment program key (32 bytes
each; all zero: none) and u32 flags (bit 0: the fragment stage may draw with the alpha test; bit 1: the vertex slot
holds a compute program).
"""

import argparse
import csv
import hashlib
import re
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MAGIC, VERSION = b"MKW_PAIR", 3
FLAG_ALPHA, FLAG_CS = 1, 2
# extract.py's source column: "<romfs>::Layout/..." from a .nup, "<romfs>/Layout/..." from a folder ("\" on Windows),
# "<exefs>/main@0x..." for exefs/main
ALPHA_SOURCE = re.compile(r"(^|[/\\:])Layout[/\\]|(^|[/\\])main@0x")


def program_key(shaders: Path, sha1: str) -> bytes:
    return hashlib.sha256((shaders / f"{sha1}.ctl").read_bytes() + (shaders / f"{sha1}.code").read_bytes()).digest()


def write(shaders: Path, out: Path) -> int:
    """Writes out from shaders/pairs.csv atomically; returns the pair count."""
    with (shaders / "pairs.csv").open() as f:
        rows = [(r["vs"], r["ps"]) for r in csv.DictReader(f)]
    with (shaders / "index.csv").open() as f:
        everything = list(csv.DictReader(f))
    index = [r for r in everything if r["stage"] in ("vs", "ps")]
    # extract.py names a mesh program "tes?" (SPH type 3): the runtime runs it as compute, so it goes in flagged too
    compute = [r["sha1"] for r in everything if r["stage"] in ("cs", "tes?")]
    stages = [(r["sha1"], r["stage"]) for r in index]
    alpha = {r["sha1"] for r in index if r["stage"] == "ps" and ALPHA_SOURCE.search(r["source"])}
    paired = {s for r in rows for s in r}
    rows += [(s, "") if st == "vs" else ("", s) for s, st in stages if s not in paired]
    keys = {s: program_key(shaders, s) for r in rows for s in r if s} | {"": bytes(32)}
    body = b"".join(keys[v] + keys[f] + struct.pack("<I", FLAG_ALPHA if f in alpha else 0) for v, f in rows)
    body += b"".join(program_key(shaders, c) + bytes(32) + struct.pack("<I", FLAG_CS) for c in compute)
    tmp = out.with_suffix(".tmp")
    tmp.write_bytes(MAGIC + struct.pack("<II", VERSION, len(rows) + len(compute)) + body)
    tmp.replace(out)
    return len(rows) + len(compute)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--shaders", type=Path, default=ROOT / "build/shaders")
    ap.add_argument("--out", type=Path, default=ROOT / "build/pipeline-pairs.bin")
    a = ap.parse_args()
    print(f"{write(a.shaders, a.out)} pairs in {a.out}")


if __name__ == "__main__":
    main()
