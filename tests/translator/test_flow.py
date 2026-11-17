"""sm86/translate/frontend/flow.py: shapes ptxas leaves that the structured form must keep, checked with flow_sim against the
branch graph."""

import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT / "sm86/translate"), str(ROOT / "sm86/translate/frontend")]
import flow  # noqa: E402
import flow_sim  # noqa: E402
from glsl import subroutines  # noqa: E402


def program(n: int, branches: dict) -> list[str]:
    """n instructions, each a statement except the branches."""
    return ["" if k in branches else f"x{k};" for k in range(n)]


def test_jump_into_a_shared_tail():
    # FS 63a69b46 (12/11/2026), its block at 806-832 cut down. The then-part (1-5) jumps from 2 into the tail [9, 10)
    # of the else-part (6-10); without the copy no nesting expresses it and the translator kept the switch.
    #  0 @!P0 BRA 6 | 1 a | 2 @P1 BRA 9 | 3 b | 4 c | 5 BRA 11
    #  6 d | 7 @!P1 BRA 10 | 8 e | 9 f | 10 g | 11 join
    branches = {0: (6, "!p0"), 2: (9, "p1"), 5: (11, None), 7: (10, "!p1")}
    branches[10] = (11, None)
    stmts = program(12, branches)
    stmts[9], stmts[10] = "f;", ""
    with pytest.raises(flow.Fail):  # the plain structuring runs other instructions
        mk, nb = flow_sim.markers(stmts), flow_sim.named(branches)
        if flow_sim.diff(mk, nb, flow._structure(mk, nb), 16):
            raise flow.Fail
    lines = flow.structure(stmts, branches)
    assert not any("switch" in x for x in lines)
    mk, nb = flow_sim.markers(stmts), flow_sim.named(branches)
    d = flow._dup_tails(stmts, branches, mk, nb, True)
    assert flow_sim.diff(mk, nb, flow._structure(d[2], d[3]), 64) is None


def test_no_tail_to_copy():
    # every jump goes straight on to a branch: nothing is copied, and the program keeps its plain structuring
    branches = {0: (3, "p0"), 2: (4, None)}
    stmts = program(5, branches)
    with pytest.raises(flow.Fail):
        flow._dup_tails(stmts, branches, flow_sim.markers(stmts), flow_sim.named(branches), False)
    assert flow.structure(stmts, branches)


def test_subroutine_copied_into_each_call():
    # VS c4b1e702 (12/11/2026), cut down: a helper after the EXIT called twice, with jumps inside it to its last
    # instruction and to its RET. Each call gets a copy whose jumps stay in it, and the RET falls out after the call.
    #  0 a | 1 CALL 6 | 2 b | 3 CALL 6 | 4 c | 5 EXIT
    #  6 d | 7 @P BRA 9 | 8 @Q BRA 10 | 9 f | 10 RET
    stmts = ["a;", "{ pc = 96u; continue; }", "b;", "{ pc = 96u; continue; }", "c;", "return;", "d;", "", "", "f;", ""]
    branches = {7: (9, "p"), 8: (10, "q")}
    s2, b2 = subroutines.splice(stmts, branches, {1: (6, 10), 3: (6, 10)})
    assert s2 == ["a;", "", "d;", "", "", "f;", "b;", "", "d;", "", "", "f;", "c;", "return;", *[""] * 5]
    assert b2 == {3: (5, "p"), 4: (6, "q"), 9: (11, "p"), 10: (12, "q")}
    assert not any("switch" in x for x in flow.structure(s2, b2))
