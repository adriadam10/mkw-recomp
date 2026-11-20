"""Profile-guided game code: merges the training runs' profiles and checks a profile still fits the code it was made on.

Why: the generated C is built at -O1 with no idea which of its ~400 000 functions run every frame. clang compiles a
profiled build at -O2 with hot paths laid out straight and hot calls inlined (totk-pc-recomp docs/pgo.md: 11-12 % a
frame on a trained scene, ~6 % on an untrained one). A profile only fits the code it was trained on: clang matches each
function by name and control-flow hash, and a function whose generated C changed loses its counts silently. The
record next to the profile keeps what decides that (the git tree of recomp/, the digests of the runtime/cpu headers
the generated C includes, the game version), and `check` compares them with the checkout.

Steps (Linux; tools/build/README.md):
  uv run tools/build/build.py --pgo gen                 # instrumented build/mkw (objects in build/obj-pgogen)
  tools/run/race.sh ...                                  # training runs; each writes build/pgo/raw/mkw-<pid>.profraw
  uv run tools/build/pgo.py merge                        # build/pgo/mkw-<ver>.profdata and .json
  uv run tools/build/build.py --pgo build/pgo/mkw-<ver>.profdata   # -O2 with it (objects in build/obj-pgo)
  uv run tools/build/pgo.py check build/pgo/mkw-<ver>.profdata     # does it still fit this checkout?
  uv run tools/build/pgo.py top build/pgo/mkw-<ver>.profdata       # its hottest functions
llvm-profdata comes from the distribution's LLVM (MKW_LLVM_PROFDATA overrides).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RAW = ROOT / "build/pgo/raw"
sys.path.insert(0, str(ROOT / "recomp"))
import versions  # noqa: E402


def profdata() -> str:
    tool = os.environ.get("MKW_LLVM_PROFDATA") or shutil.which("llvm-profdata")
    if not tool:
        sys.exit("llvm-profdata not found (the distribution's llvm package; MKW_LLVM_PROFDATA to name it)")
    return tool


def fit_record() -> dict[str, str]:
    """What a profile's fit depends on: the generator's tree, the headers every generated function inlines."""
    tree = subprocess.run(["git", "rev-parse", "HEAD:recomp"], cwd=ROOT, capture_output=True, text=True, check=True)
    dirty = subprocess.run(["git", "status", "--porcelain", "--", "recomp"], cwd=ROOT, capture_output=True, text=True,
                           check=True).stdout.strip()  # fmt: skip
    heads = {p.name: hashlib.sha256(p.read_bytes().replace(b"\r\n", b"\n")).hexdigest()[:16]
             for p in sorted((ROOT / "runtime/cpu").glob("*.h"))}  # fmt: skip
    return {
        "recomp_tree": tree.stdout.strip() + ("-dirty" if dirty else ""),
        **{f"header {k}": v for k, v in heads.items()},
    }


def merge(args: argparse.Namespace) -> None:
    raws = sorted(RAW.glob("*.profraw"))
    if not raws:
        sys.exit(f"no profiles in {RAW}: run the instrumented build (build.py --pgo gen) first")
    ver = args.version or str(versions.detect(args.exefs).number)  # 100, 180
    out = Path(args.out) if args.out else ROOT / f"build/pgo/mkw-{ver}.profdata"
    out.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run([profdata(), "merge", "-o", str(out), *map(str, raws)], check=True)
    rec = {"version": ver, "runs": [r.name for r in raws], **fit_record()}
    out.with_suffix(".json").write_text(json.dumps(rec, indent=1) + "\n")
    size = sum(r.stat().st_size for r in raws) >> 20
    print(f"{out}: {len(raws)} runs ({size} MiB raw), record {out.with_suffix('.json').name}")


def check(args: argparse.Namespace) -> None:
    rec_path = Path(args.profile).with_suffix(".json")
    if not rec_path.is_file():
        sys.exit(f"{rec_path} missing: the profile's fit is unknown")
    rec, now = json.loads(rec_path.read_text()), fit_record()
    stale = [k for k in now if rec.get(k) != now[k]]
    if not stale:
        print(f"{args.profile}: fits this checkout")
        return
    for k in stale:
        print(f"{args.profile}: {k} was {rec.get(k)}, now {now[k]}")
    print("retrain: the functions whose generated C changed lost their counts (clang does not say which)")
    sys.exit(1)


def top(args: argparse.Namespace) -> None:
    subprocess.run([profdata(), "show", "--topn", str(args.n), str(args.profile)], check=True)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    m = sub.add_parser("merge", help="build/pgo/raw/*.profraw into one profile and its record")
    m.add_argument("--version", help="game version tag in the name (default: the dump's)")
    m.add_argument("--exefs", type=Path, default=ROOT / "game/exefs")
    m.add_argument("--out", help="the profile's path (default build/pgo/mkw-<version>.profdata)")
    c = sub.add_parser("check", help="whether a profile's record matches this checkout")
    c.add_argument("profile")
    t = sub.add_parser("top", help="the profile's hottest functions")
    t.add_argument("profile")
    t.add_argument("-n", type=int, default=30)
    args = ap.parse_args()
    {"merge": merge, "check": check, "top": top}[args.cmd](args)


if __name__ == "__main__":
    main()
