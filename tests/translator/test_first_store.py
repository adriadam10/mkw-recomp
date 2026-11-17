"""The first-invocation store split (sm86/translate/glsl/first_store.py) on the preamble shapes MKW's programs have.

Usage: uv run pytest tests/translator/test_first_store.py
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT / "sm86/translate"), str(ROOT / "sm86/translate/frontend")]
from glsl import first_store  # noqa: E402

PRE_1D = [  # 0fde431b...'s preamble (32x1)
    "        r1 = gl_WorkGroupID.x;",
    "        ur4 = cb1.d[0][0]; ur5 = cb1.d[0][1];",
    "        r0 = gl_LocalInvocationID.x;",
    "        r1 = (r1 * 0x20u + r0);",
    "        p0 = ((r1) != (0u)) && true;",
    "        p1 = ((r1) >= (cb6.d[4][0])) && true;",
    "        if (!p0) { sb60.d[((ur4 - cb0.d[6][0] + 0x0u) >> 2) + 0u] = 0u; }",
    "        if (p1) { return; }",
    "        r2 = atomicAdd(sb60.d[0], 1u);",
]

PRE_2D = [  # ded3fdf9...'s preamble (8x4)
    "        r12 = gl_WorkGroupID.x;",
    "        r1 = gl_LocalInvocationID.x;",
    "        r13 = gl_WorkGroupID.y;",
    "        r2 = gl_LocalInvocationID.y;",
    "        r4 = (r12 * 0x8u + r1);",
    "        r5 = ((r13 << 2u) + r2);",
    "        { uint lr416 = ((r4) | (r5)); p0 = (lr416 != 0u) || !true; }",
    "        if (!p0) { r1 = cb0.d[2][1]; }",
    "        if (!p0) { sb20.d[((r0 - cb0.d[2][0] + 0x0u) >> 2) + 0u] = 0u; }",
    "        r3 = atomicAdd(sb20.d[1], 1u);",
]


def test_one_dimension() -> None:
    out, form = first_store.split(PRE_1D, (32, 1, 1, 0))
    assert form == 1
    assert out[6] == "        if (!p0 && FIRST_STORE != 2u) { sb60.d[((ur4 - cb0.d[6][0] + 0x0u) >> 2) + 0u] = 0u; }"
    assert out[7] == "        if (FIRST_STORE == 1u) return;"
    assert out[8:] == PRE_1D[7:]


def test_two_dimensions() -> None:
    out, form = first_store.split(PRE_2D, (8, 4, 1, 0))
    assert form == 2
    assert out[7] == PRE_2D[7]  # a guarded assignment stays in every variant
    assert "FIRST_STORE != 2u" in out[8] and out[9] == "        if (FIRST_STORE == 1u) return;"


def test_other_workgroup_size_is_left_alone() -> None:
    assert first_store.split(PRE_1D, (64, 1, 1, 0)) == (PRE_1D, 0)  # 0x20 is not the workgroup's width


def test_index_rewritten_before_the_test_is_left_alone() -> None:
    lines = PRE_1D[:4] + ["        r1 = (r1 + 0x1u);"] + PRE_1D[4:]
    assert first_store.split(lines, (32, 1, 1, 0)) == (lines, 0)
