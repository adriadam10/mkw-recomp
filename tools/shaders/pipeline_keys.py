"""Census of a pipeline-keys.bin: which parts of a pipeline key the game's own files could give.

Why: the boot prewarm only builds the pipelines a player's runs recorded; building all of them at setup needs their keys
from the game copy itself. This measures how far that can go: distinct programs, pairs and states, how often one
program pair keeps a single value of each state field, and which pairs are one program of the game's shader archives
(build/shaders/index.csv from extract.py; a stage's key is sha256 of its .ctl and .code, the record's program key).

Usage: python3 tools/shaders/pipeline_keys.py [build/pipeline-keys.bin] [--shaders build/shaders]
Record layout: KeyRec in runtime/gfx/vk/pipe/vk_pipe_internal.h (vs[32] fs[32] has_fs pad, then VkPipeState of vk.h);
the field offsets below are VkPipeState's (vs and fs pointers first), checked with offsetof on 17/11/2026.
"""

import argparse
import collections
import csv
import hashlib
import struct
from pathlib import Path

HEAD = 72  # vs[32] fs[32] has_fs pad
FIELDS = {"rt": (16, 52), "misc": (52, 64), "blend": (64, 128), "colchm": (128, 136), "ms": (136, 146),
          "poly": (148, 152), "ds": (152, 160), "vattr": (160, 224), "vstream": (224, 352)}


def records(path: Path) -> list[bytes]:
    b = path.read_bytes()
    magic, _version, size = struct.unpack_from("<8sII", b)
    if magic != b"MKW_PKEY":
        raise SystemExit(f"{path}: not a pipeline key file")
    return [b[16 + i * size : 16 + (i + 1) * size] for i in range((len(b) - 16) // size)]


def field(r: bytes, name: str) -> bytes:
    lo, hi = FIELDS[name]
    return r[HEAD + lo : HEAD + hi]


def single_share(recs: list[bytes], group, name: str) -> str:
    """'a/b': of b groups, a keep one value of the field."""
    seen = collections.defaultdict(set)
    for r in recs:
        seen[group(r)].add(field(r, name))
    return f"{sum(len(v) == 1 for v in seen.values())}/{len(seen)}"


def archive_pairs(shaders: Path) -> tuple[set[bytes], set[tuple[bytes, bytes]]]:
    """Program keys of every extracted stage, and the (vs, ps) pairs of each archive program."""
    rows = list(csv.DictReader((shaders / "index.csv").open()))
    key = {r["sha1"]: hashlib.sha256((shaders / f"{r['sha1']}.ctl").read_bytes() +
                                     (shaders / f"{r['sha1']}.code").read_bytes()).digest() for r in rows}
    prog = collections.defaultdict(dict)
    for r in rows:
        prog[(r["source"], r["program"])][r["stage"]] = key[r["sha1"]]
    return set(key.values()), {(p["vs"], p["ps"]) for p in prog.values() if "vs" in p and "ps" in p}


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("keys", nargs="?", type=Path, default=Path("build/pipeline-keys.bin"))
    ap.add_argument("--shaders", type=Path, default=Path("build/shaders"))
    a = ap.parse_args()
    recs = records(a.keys)
    pairs = {r[:64] for r in recs}
    print(f"{len(recs)} records, {len({r[:32] for r in recs})} vs, {len({r[32:64] for r in recs})} fs, {len(pairs)} pairs")
    print(f"{'field':8} {'distinct':>8}  {'one per vs':>11} {'one per fs':>11} {'one per pair':>12}")
    for name in FIELDS:
        print(f"{name:8} {len({field(r, name) for r in recs}):8}  {single_share(recs, lambda r: r[:32], name):>11} "
              f"{single_share(recs, lambda r: r[32:64], name):>11} {single_share(recs, lambda r: r[:64], name):>12}")
    if (a.shaders / "index.csv").exists():
        known, static = archive_pairs(a.shaders)
        p2 = {(p[:32], p[32:]) for p in pairs}
        print(f"archives: {len(known)} stages, {len(static)} programs with vs and ps; record pairs: vs in archives "
              f"{sum(p[0] in known for p in p2)}, fs {sum(p[1] in known for p in p2)}, one archive program "
              f"{sum(p in static for p in p2)} of {len(p2)}")


if __name__ == "__main__":
    main()
