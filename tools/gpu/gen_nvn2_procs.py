"""Write runtime/gfx/nvn/procs/nvn2_procs.inc: every NVN2 proc name NVN1 lacks, as NVN_PROC rows for the runtime's registry.

Why: MKW's proc loader asks for all ~700 NVN2 names. A name the front does not know used to come back as NULL, and a
NULL that the game later calls (or stores and tests) fails far from the cause. Listed here, each name gets a weak stub
that stops with its own name when called (runtime/gfx/nvn/procs/nvn_procs.c); runtime/gfx/nvn/procs/nvn2.c gives the strong
definitions. ProgramBuilder accessors are left out: the runtime hands out the driver's own functions for them.

Names come from the sdk's proc table (nvn2_procs.py) minus NVN1's (nvn1_procs.txt). API names only, no code. Each
game version has its own sdk build with more procs (1.0.0: 698, 1.8.0: 731), and one runtime serves them all: the
names already in the file are kept, so running this on each version's dump leaves the union.

Usage: uv run tools/gpu/gen_nvn2_procs.py
"""

import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "recomp"))
sys.path.insert(0, str(Path(__file__).parent))

from nso import Nso
from nvn2_procs import procs

OUT = ROOT / "runtime/gfx/nvn/procs/nvn2_procs.inc"


def main() -> None:
    nvn2 = set(procs(Nso(str(Path(os.environ.get("MKW_GAME", "game")) / "exefs/sdk"))))
    nvn1 = {n for n in (Path(__file__).with_name("nvn1_procs.txt")).read_text().split() if n.startswith("nvn")}
    names = sorted(n for n in nvn2 - nvn1 if not n.startswith("nvnProgramBuilder"))
    before = set(OUT.read_text().split("NVN_PROC(")[1:]) if OUT.is_file() else set()
    names += [n.split(")")[0] for n in before]
    names.append("nvnProgramInitializeFromBuilder")  # NVN2 only, but on a program: host function (nvn2.c)
    head = (
        "// NVN2 procs NVN1 lacks (tools/gpu/gen_nvn2_procs.py; do not edit): weak stubs unless nvn2.c defines them.\n"
    )
    OUT.write_text(head + "".join(f"NVN_PROC({n})\n" for n in sorted(set(names))))
    print(f"{OUT.relative_to(ROOT)}: {len(set(names))} procs")


if __name__ == "__main__":
    main()
