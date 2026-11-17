"""Why sm86/translate/frontend/flow.py keeps one program as a `switch`: its branch graph, whether flow._structure builds a
structured form, and where flow_sim finds that form running other instructions than the graph (seed, step, the
instructions of each around it).

Why: PS a3908a92… (12/11/2026) was structured right but refused; this showed the extra reads of a branch condition
behind a guard in two minutes, where reading the 821-line switch had not.
Usage: uv run tools/shaders/flow_why.py SHA1 [--stage ps] [--seeds 4]   (SHA1: a program of build/shaders/)
"""

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT / "sm86/translate"), str(ROOT / "sm86/translate/frontend")]
sys.path.insert(0, str(ROOT / "tools/shaders"))

import flow  # noqa: E402
import flow_sim  # noqa: E402
import glsl  # noqa: E402
from extract import trim  # noqa: E402
from program import Decoder  # noqa: E402


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("sha1")
    ap.add_argument("--stage", default="ps")
    ap.add_argument("--seeds", type=int, default=4)
    a = ap.parse_args()
    base = ROOT / "build/shaders" / a.sha1
    code, ctl = base.with_suffix(".code").read_bytes(), base.with_suffix(".ctl").read_bytes()
    seen: dict = {}
    real = flow.structure

    def spy(stmts, branches):  # the statements and branches glsl.translate hands flow
        seen["s"], seen["b"] = list(stmts), dict(branches)
        return real(stmts, branches)

    flow.structure = spy
    sph, insns = Decoder(ROOT / "build/sm86/instructions.json").program(trim(code, a.stage), a.stage)
    glsl.translate(sph, insns, a.stage, glsl.compute_shape(ctl) if a.stage == "cs" else None, b"")
    if "s" not in seen:
        sys.exit("flow.structure was not called: the program has no branches or is not structurable (BRX, RET)")
    s, b = seen["s"], seen["b"]
    print(f"{len(s)} statements, {len(b)} branches")
    for k in sorted(b):
        t, c = b[k]
        print(f"  {k:4d} -> {t:4d}{' (back)' if t <= k else ''} {c or 'always'}")
    mk, nb = flow_sim.markers(s), flow_sim.named(b)
    try:
        lines = flow._structure(mk, nb)
    except flow.Fail:
        sys.exit("flow._structure: Fail (the branches do not nest)")
    for seed in range(a.seeds):
        x, y = flow_sim.run_original(mk, nb, seed), flow_sim.run_structured(flow_sim.parse(lines), seed)
        if x != y:
            at = next((k for k, (p, q) in enumerate(zip(x, y)) if p != q), min(len(x), len(y)))
            print(
                f"seed {seed}, step {at}:\n  graph      {x[max(0, at - 10) : at + 6]}\n  structured {y[max(0, at - 10) : at + 6]}"
            )
            return
    print(f"flow._structure runs the same instructions as the graph on {a.seeds} seeds")


if __name__ == "__main__":
    main()
