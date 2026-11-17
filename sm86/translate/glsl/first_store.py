"""SM86 -> GLSL: a store only a dispatch's first invocation makes, split off so the runtime can run it first.

Some compute programs reset an append counter in their preamble, `if (global invocation == 0) sb[k] = 0`, while every
workgroup adds to it with atomics later on. Nothing in the API orders the first workgroup ahead of the others: where a
later workgroup appends before the reset, its entries are lost and the list it builds (MKW's scenery: trees and
bushes) loses objects at random from frame to frame. nxnative (MIT) found it in MKW's scenery lists and moved the
stores to the host before the dispatch (docs/reference/shaders.md, "A store only a dispatch's first invocation
makes", dm/gfx/dmsh_build.py first_store).

Here the program gets a specialization constant FIRST_STORE (constant_id FIRST_STORE_ID): 0, the default, runs it as
it was; 1 makes the guarded stores and returns right after the last one; 2 runs it without them. The runtime
(vk_exec_ops.c op_dispatch) dispatches one workgroup with 1, a barrier, then the dispatch with 2. Recognised only in
straight-line code at the top of main (nothing before the last store may branch, loop, store or use atomics), for the
exact first-invocation tests: `wg.x * LX + lx != 0`, or `(wg.x * LX + lx) | (wg.y * LY + ly) != 0` with LZ = 1. The
reflection's "first_store" says which: 1 needs a dispatch of y = z = 1, 2 one of z = 1, as only then the test is the
dispatch's first invocation alone.
"""

import re

FIRST_STORE_ID = 0x7FF0  # above the SCU/SCV ids (2k, 2k + 1) a program can have

_ASSIGN = re.compile(r"^(u?r\d+) = ([^;]+);$")
_PRED_NE = re.compile(r"^(p\d+) = \(\((r\d+)\) != \(0u\)\) && true;$")
_PRED_OR = re.compile(r"^\{ uint (lr\d+) = \(\((r\d+)\) \| \((r\d+)\)\); (p\d+) = \(\1 != 0u\) \|\| !true; \}$")
_GUARDED = re.compile(r"^if \(!(p\d+)\) \{ (.*) \}$")
_STORE = re.compile(r"^sb\d+\.d\[.*\] = (\d+u|0x[0-9a-f]+u);$")
_LINEAR = re.compile(r"^\((r\d+) \* (0x[0-9a-f]+u|\d+u) \+ (r\d+)\)$")
_SHIFTED = re.compile(r"^\(\((r\d+) << (\d+)u\) \+ (r\d+)\)$")
_WRITES = re.compile(r"\b(u?r\d+|p\d+) = ")
_MEMORY = re.compile(r"\bsb\d+\.d\[.*\] = |atomic|imageStore|\bSMEM\(|barrier\(|\bgst|\blmem\[")


def _uint(s: str) -> int:
    return int(s[:-1], 0)


def _value(defs: dict[str, str], expr: str) -> str:
    """What an assignment's expression holds, in the registers' values at that point: an ID as GLSL names it, the
    global index `index.<axis>.<workgroup size>`, or the expression itself."""
    m = _LINEAR.match(expr)
    if m:
        w, k, loc = m.group(1), _uint(m.group(2)), m.group(3)
    elif (m := _SHIFTED.match(expr)) and int(m.group(2)) < 32:
        w, k, loc = m.group(1), 1 << int(m.group(2)), m.group(3)
    else:
        return expr
    for axis in "xy":
        if defs.get(w) == f"gl_WorkGroupID.{axis}" and defs.get(loc) == f"gl_LocalInvocationID.{axis}":
            return f"index.{axis}.{k}"
    return expr


def _index_of(defs: dict[str, str], reg: str, axis: str, size: int) -> bool:
    return defs.get(reg) == f"index.{axis}.{size}"


def split(lines: list[str], shape: tuple[int, int, int, int]) -> tuple[list[str], int]:
    """The structured body with the first-invocation stores under FIRST_STORE, and the reflection's "first_store" (0:
    none found, the body unchanged)."""
    lx, ly, lz, _ = shape
    defs: dict[str, str] = {}
    first: dict[str, int] = {}  # predicate -> 1 or 2 (the form): false only in the first invocation
    stores: list[int] = []
    form = 0
    for n, raw in enumerate(lines):
        if not raw.startswith("        ") or raw.startswith("         "):
            break  # nested: past the straight-line preamble
        s = raw.strip()
        g = _GUARDED.match(s)
        if g and g.group(1) in first and _STORE.match(g.group(2)) and form in (0, first[g.group(1)]):
            stores.append(n)
            form = first[g.group(1)]
            continue
        if _MEMORY.search(s) or "return" in s or "continue" in s or "break" in s or s.endswith("{") or s == "}":
            break
        if g:  # a guarded assignment: what it writes is no longer known
            for w in _WRITES.findall(g.group(2)):
                defs.pop(w, None)
                first.pop(w, None)
            continue
        if m := _PRED_NE.match(s):
            first.pop(m.group(1), None)
            if ly == 1 and lz == 1 and _index_of(defs, m.group(2), "x", lx):
                first[m.group(1)] = 1
            continue
        if m := _PRED_OR.match(s):
            first.pop(m.group(4), None)
            if lz == 1 and _index_of(defs, m.group(2), "x", lx) and _index_of(defs, m.group(3), "y", ly):
                first[m.group(4)] = 2
            continue
        if m := _ASSIGN.match(s):
            defs[m.group(1)] = _value(defs, m.group(2))
            first.pop(m.group(1), None)
            continue
        for w in _WRITES.findall(s):  # pairs, vectors, blocks: written another way
            defs.pop(w, None)
            first.pop(w, None)
    if not stores:
        return lines, 0
    out = list(lines)
    for n in stores:
        g = _GUARDED.match(out[n].strip())
        out[n] = f"        if (!{g.group(1)} && FIRST_STORE != 2u) {{ {g.group(2)} }}"
    out.insert(stores[-1] + 1, "        if (FIRST_STORE == 1u) return;")
    return out, form


def declaration() -> str:
    return f"layout(constant_id = {FIRST_STORE_ID}) const uint FIRST_STORE = 0u;"
