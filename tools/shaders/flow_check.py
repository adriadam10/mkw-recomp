"""Check that sm86/translate/frontend/flow.py keeps each program's control flow: run the original branch graph and the
structured GLSL it builds side by side, off the GPU, and compare the instructions they execute.

Why: a wrongly structured loop runs to LOOP_CAP in every pixel and hangs the GPU (run93, 31/10/2026: an amdgpu ring
timeout and a reset of the whole desktop's GPU). This catches it before any run. Each instruction becomes a marker
statement and each branch condition a named oracle; both forms see the same pseudo-random answer for the n-th
evaluation of a branch, so equal programs give equal traces. Predicated EXITs are oracles too.

Usage: uv run tools/shaders/flow_check.py [--shaders build/shaders] [--sample N] [--seeds 16]
Prints the programs whose unverified structuring (flow._structure) runs other instructions: flow.structure refuses
those and the translator keeps their switch. The simulation itself is sm86/translate/frontend/flow_sim.py.
"""

import argparse
import csv
import os
import sys
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT / "sm86/translate"), str(ROOT / "sm86/translate/frontend")]
sys.path.insert(0, str(ROOT / "tools/shaders"))

import flow
import glsl
from extract import trim
from flow_sim import diff, markers, named
from program import Decoder
from translate import translator_hash

_dec = None
_captured: list = []
_orig_structure = flow._structure  # unverified: the census of what flow.structure refuses
_verified = flow.structure


def capture(stmts, branches):
    _captured.append((list(stmts), dict(branches)))
    return _verified(stmts, branches)


def init() -> None:
    global _dec
    _dec = Decoder(ROOT / "build/sm86/instructions.json")
    flow.structure = capture


def check(job: tuple[str, str, str, int]) -> tuple[str, str]:
    sha, stage, shaders, seeds = job
    code = trim(Path(shaders, f"{sha}.code").read_bytes(), stage)
    sph, insns = _dec.program(code, stage)
    _captured.clear()
    try:
        glsl.translate(sph, insns, stage)
    except glsl.Unsupported:
        return sha, "skip"
    except Exception as e:  # noqa: BLE001 - one program must not stop the check
        return sha, f"error {type(e).__name__}"
    if not _captured:
        return sha, "no-branches"
    stmts, branches = _captured[-1]
    mk, nb = markers(stmts), named(branches)
    try:
        lines = _orig_structure(mk, nb)
    except flow.Fail:
        return sha, "switch"
    why = diff(mk, nb, lines, seeds)
    return sha, f"DIFF {why}" if why else "ok"


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--shaders", type=Path, default=ROOT / "build/shaders")
    ap.add_argument("--sample", type=int, default=0)
    ap.add_argument("--seeds", type=int, default=16)
    ap.add_argument("--only", nargs="*", help="sha1s to check")
    args = ap.parse_args()
    with (args.shaders / "index.csv").open() as f:
        rows = list(csv.DictReader(f))
    if args.only:
        rows = [r for r in rows if r["sha1"] in set(args.only)]
    elif args.sample:
        rows = rows[:: max(1, len(rows) // args.sample)]
    jobs = [(r["sha1"], r["stage"], str(args.shaders), args.seeds) for r in rows]
    counts: dict[str, int] = {}
    th = translator_hash()
    with ProcessPoolExecutor(os.cpu_count(), initializer=init) as pool:
        for sha, res in pool.map(check, jobs, chunksize=8):
            key = res.split(" ")[0]
            counts[key] = counts.get(key, 0) + 1
            if key in ("DIFF", "error"):
                print(sha, res, flush=True)
    print(" ".join(f"{k} {v}" for k, v in sorted(counts.items())))
    if translator_hash() != th:  # as glsl_hash.py: workers started after an edit checked other code
        sys.exit("flow_check: the translator changed during the check: these counts are not valid, run it again")


if __name__ == "__main__":
    main()
