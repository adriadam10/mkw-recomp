"""Hash the GLSL and reflection that sm86/translate gives for every extracted program, one line per program.

Why: a refactor of the translator must not change its output; two hash files compared with `cmp` say it in ~1 min,
without glslc (the census recompiles every program whose GLSL changed and does not show where).

Usage: uv run tools/shaders/glsl_hash.py OUT.tsv   (before and after the change, then `cmp before.tsv after.tsv`)
"""

import csv
import hashlib
import json
import os
import sys
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT / "sm86/translate"), str(ROOT / "sm86/translate/frontend")]
sys.path.insert(0, str(ROOT / "tools/shaders"))

import glsl
from extract import trim
from program import Decoder
from translate import translator_hash

_dec = None


def init() -> None:
    global _dec
    _dec = Decoder(ROOT / "build/sm86/instructions.json")


def one(row: tuple[str, str]) -> tuple[str, str]:
    sha, stage = row
    shaders = ROOT / "build/shaders"
    code = trim((shaders / f"{sha}.code").read_bytes(), stage)
    ctl = (shaders / f"{sha}.ctl").read_bytes()
    try:
        sph, insns = _dec.program(code, stage)
        src, refl = glsl.translate(sph, insns, stage, glsl.compute_shape(ctl) if stage == "cs" else None)
        out = src + json.dumps(refl, sort_keys=True)
    except (glsl.Unsupported, KeyError, ValueError, IndexError) as e:  # the failure is the output to compare
        out = f"{type(e).__name__}: {e}"
    return sha, hashlib.sha256(out.encode()).hexdigest()


def main() -> None:
    with (ROOT / "build/shaders/index.csv").open() as f:
        rows = [(r["sha1"], r["stage"]) for r in csv.DictReader(f)]
    th = translator_hash()
    with ProcessPoolExecutor(os.cpu_count(), initializer=init) as pool:
        res = sorted(pool.map(one, rows, chunksize=16))
    # workers that started after an edit translate with other code than the first ones: the file would mix both
    # (12/11/2026: two censuses of the subroutine change taken while the translator was being edited)
    if translator_hash() != th:
        sys.exit("glsl_hash: the translator changed during the census: nothing written, run it again")
    Path(sys.argv[1]).write_text("".join(f"{sha}\t{h}\n" for sha, h in res))
    print(f"{len(res)} programs")


if __name__ == "__main__":
    main()
