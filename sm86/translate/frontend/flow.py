"""Structured control flow for a program's GLSL: if / else and do / while from its BRA instructions, in place of the
switch on the pc that glsl.translate falls back to.

Why: drivers compile the structured form much faster than a state machine. SavageWindRecomp measured it on Maxwell
shaders on the same Radeon 660M (runtime/nvn/maxwell/translate.c structure_flow @ 663cff16, MIT: RADV compile time of
a sample 42.9 -> 14.6 s of CPU), and glslc gains the same way. The idea and the cases are theirs; SM86 needs less of
it: BSSY/BSYNC are scheduling hints the translator drops, and EXIT is a GLSL return, valid anywhere in main.

A forward conditional BRA over [n, t) becomes `if (!cond) { [n, t) }`, or if / else when the code before t ends with
an unconditional jump further on; a backward BRA at `back` to `i` becomes `do { [i, back) } while (cond);`; an
unconditional forward jump skips dead code. Anything else (a jump into or out of a range other than its own end,
CALL, RET, BRX) raises Fail and the caller keeps the switch.
"""

import flow_sim


class Fail(Exception):
    """The program's branches do not nest."""


def structure(stmts: list[str], branches: dict[int, tuple[int, str | None]]) -> list[str]:
    """_structure, refused (Fail: the caller keeps the switch) unless flow_sim finds that it runs the same instructions
    as the branch graph on 4 sets of branch outcomes. When that fails, once more with the short tails copied to the
    jumps into them (_dup_tails), checked against the original graph."""
    mk, nb = flow_sim.markers(stmts), flow_sim.named(branches)
    try:
        lines = _structure(stmts, branches)
        if not flow_sim.diff(mk, nb, _structure(mk, nb), 4):
            return lines
    except Fail:
        pass
    # the tails fall-through also reaches first, then every one, then also the longer ones that fall into a join
    for joins_only, falls in ((True, False), (False, False), (False, True)):
        try:
            d_stmts, d_branches, d_mk, d_nb = _dup_tails(stmts, branches, mk, nb, joins_only, falls)
            lines = _structure(d_stmts, d_branches)
            if not flow_sim.diff(mk, nb, _structure(d_mk, d_nb), 4):
                return lines
        except Fail:
            pass
    raise Fail


TAIL_MAX = 16  # instructions copied per jump at most
FALL_MAX = 32  # the same for a tail that falls into a join (third pass)


def _dup_tails(
    stmts: list[str],
    branches: dict[int, tuple[int, str | None]],
    mk: list[str],
    nb: dict,
    joins_only: bool,
    falls: bool = False,
):
    """A forward jump into a join that runs a few instructions and jumps on, `@P BRA t ... t: x; BRA j`, gets its own
    copy of [t, BRA j): `@!P skip; x; BRA j`. ptxas merges equal tails of if / else chains so, and the jump then lands
    in the middle of a sibling's else-part, which no nesting expresses (FS 63a69b46…, the draw that gave up on AMD's
    Windows driver after 30 s in 10 logs of #111's group, 12/11/2026). The copies keep their instruction's marker and
    each jump its condition's name, so flow_sim compares the result with the original graph. Fail when nothing applies.
    joins_only: only the tails the instruction before also falls into (a jump that lands only where other jumps do,
    as in FS 9f0b96fd…, needs the second pass). falls: also a tail of up to FALL_MAX that ends by falling into a join
    (`t: x; j:` with j another jump's target), copied with a jump to j: VS 06040b9f…, whose then-part jumps 20
    instructions into its else-part, one of the 21 programs that took AMD's compiler 2-6 s (12/11/2026). Returns the
    new stmts and branches, and the same for flow_sim (mk, nb)."""
    targets = {t for t, _ in branches.values()}
    most = FALL_MAX if falls else TAIL_MAX
    tails: dict[int, tuple[int, int, int]] = {}  # jump -> (t, u, j): copy [t, u), then jump to j
    for k, (t, c) in branches.items():
        if t <= k + 1 or t >= len(stmts) or joins_only and t - 1 in branches and branches[t - 1][1] is None:
            continue  # backward, next, past the end, or (first pass) no fall-through into t
        u = t
        while u < len(stmts) and u not in branches and not (falls and u > t and u in targets) and u - t <= most:
            u += 1
        if u in branches and branches[u][1] is None and branches[u][0] > u and u > t and u - t <= TAIL_MAX:
            tails[k] = (t, u, branches[u][0])
        elif falls and u in targets and u > t and u - t <= most:
            tails[k] = (t, u, u)
    if not tails:
        raise Fail
    pos: dict[int, int] = {}  # original index -> new index
    s2, m2, jumps = [], [], []  # jumps: (new index, original target)
    for k, s in enumerate(stmts):
        pos[k] = len(s2)
        s2.append(s)
        m2.append(mk[k])
        if k in tails:
            t, u, j = tails[k]
            s2 += stmts[t:u]
            m2 += mk[t:u]
            jumps.append((len(s2), j))
            s2.append("")
            m2.append("")
    pos[len(stmts)] = len(s2)
    b2, n2 = {}, {}
    for k, (t, c) in branches.items():
        if k in tails:  # into its copy when it jumps, past it when not
            if c is not None:
                b2[pos[k]], n2[pos[k]] = (pos[k + 1], f"!({c})"), (pos[k + 1], f"!({nb[k][1]})")
        else:
            b2[pos[k]], n2[pos[k]] = (pos.get(t, pos[len(stmts)]), c), (pos.get(t, pos[len(stmts)]), nb[k][1])
    for at, j in jumps:
        b2[at] = n2[at] = (pos.get(j, pos[len(stmts)]), None)
    return s2, b2, m2, n2


def _structure(stmts: list[str], branches: dict[int, tuple[int, str | None]]) -> list[str]:
    """Lines of main's body. stmts[k]: GLSL of instruction k ('' for none); branches[k] = (target index, condition or
    None when unconditional) for each BRA, a target past the end meaning the end."""
    n = len(stmts)
    branches = dict(branches)
    # `@P jump past the back edge; jump back` is a loop that runs while !P (ptxas leaves loops so, also with a CALL
    # that never returns: DrivePassShaderDefault 294 at 0x3d6a0)
    for k, (t, c) in sorted(branches.items()):
        nxt = branches.get(k + 1)
        if c is not None and t == k + 2 and nxt and nxt[1] is None and nxt[0] <= k:
            branches[k + 1] = (nxt[0], f"!({c})")
            del branches[k]
    loop_end: dict[int, int] = {}
    for k, (t, _) in branches.items():
        if t <= k:
            loop_end[t] = max(loop_end.get(t, k), k)
    targets = {t for t, _ in branches.values()}
    before: dict[int, list[str]] = {}
    consumed: set[int] = set()
    dead: set[int] = set()
    opened: set[int] = set()
    gotos: list[tuple[int, int, str]] = []  # (jump, target, flag): forward jumps out of their range
    sets: dict[int, str] = {}  # jump -> the statement that sets its flag

    def no_loop_between(a: int, b: int) -> bool:
        return not any(a < h < b or a < e < b for h, e in loop_end.items())

    def put(at: int, line: str) -> None:
        before.setdefault(at, []).append(line)

    def run(lo: int, hi: int, latch: int | None, depth: int, outs: frozenset[int] = frozenset()) -> None:
        """outs: targets that are the end of this range in effect: the join of an enclosing if / else whose then- or
        else-part this range ends (an if / else-if chain whose branches all jump to one far join)."""
        if depth > 64:
            raise Fail
        i = lo
        while i < hi:
            back = loop_end.get(i)
            if back is not None and i not in opened:
                if back >= hi:
                    raise Fail
                cond = branches[back][1]
                if cond is None:
                    raise Fail  # an endless loop left only by breaks
                opened.add(i)
                consumed.add(back)
                put(i, "    do {")
                run(i, back, back, depth + 1)
                put(back + 1, f"    }} while (({cond}) && ++loops < LOOP_CAP);")
                i = back + 1
                continue
            if i not in branches:
                i += 1
                continue
            t, cond = branches[i]
            if t in outs:
                t = hi
            if t == hi + 1 and latch == hi:
                raise Fail  # a break out of the loop
            if t == latch:
                t = hi  # continue: on to the loop's condition
            if t > hi and t <= n and no_loop_between(i, t):
                # A jump out of its range to a later join (an if / else chain's end): a flag set at the jump guards
                # every instruction up to the target. Flags are assigned at the jump and cleared at each loop pass.
                flag = f"g{len(gotos)}"
                gotos.append((i, t, flag))
                consumed.add(i)
                sets[i] = f"{flag} = {cond or 'true'};"
                i += 1
                continue
            if t <= i or t > hi:
                raise Fail
            consumed.add(i)
            if cond is None:  # what lies between is dead, unless something else jumps into it
                if any(i < x < t for x in targets):
                    raise Fail
                dead.update(range(i + 1, t))
                i = t
                continue
            if t == i + 1:
                i += 1
                continue
            last = t - 1
            other = branches.get(last)
            join = None
            far = other[0] if other else None
            if far is not None and far in outs:
                far = hi
            if last > i and other and other[1] is None and loop_end.get(t) is None and t < far <= hi:
                join = far
                # the far join counts as the end only for a then- or else-part that ends where this range ends
                inner = (outs if join == hi else frozenset()) | {other[0]}
            put(i + 1, f"    if (!({cond})) {{")
            if join is not None:
                run(i + 1, last, None, depth + 1, inner)
                consumed.add(last)
                put(t, "    } else {")
                run(t, join, None, depth + 1, inner)
                put(join, "    }")
                i = join
            else:
                run(i + 1, t, None, depth + 1)
                put(t, "    }")
                i = t

    run(0, n, None, 0)
    if set(branches) - consumed:
        raise Fail
    flags = [f for _, _, f in gotos]
    # Where each if's block (both parts) closes: an if that opens and closes inside a guarded stretch tests the guard
    # first, as every statement there does, so its condition is not read on the way to the join (harmless on the GPU,
    # but flow_sim counts every read of a branch condition, and the extra ones shifted the next loop pass's answers:
    # PS a3908a92…, 12/11/2026). One that closes past the jump's target is left alone: guarding it would skip what
    # follows the target too.
    ends, stack = {}, []
    for k in range(n + 1):
        for x, line in enumerate(before.get(k, [])):
            if line.startswith("    if (") or line == "    do {":
                stack.append((k, x))
            elif line == "    }" or line.startswith("    } while"):
                ends[stack.pop()] = k
    out = [f"    bool {f} = false;" for f in flags]
    for k in range(n):
        for x, line in enumerate(before.get(k, [])):
            guard = [f for j, t, f in gotos if j < k and ends.get((k, x), n + 1) <= t]
            if guard and line.startswith("    if (!(") and line.endswith(")) {"):
                line = f"    if (!({' || '.join(guard)} || ({line[len('    if (!(') : -len(')) {')]}))) {{"
            out.append(line)
            if line == "    do {" and flags:  # a new pass: no jump of this pass has been taken yet
                out.append("        " + " ".join(f"{f} = false;" for f in flags))
        guard = [f for j, t, f in gotos if j < k < t]
        stmt = sets.get(k) or (stmts[k] if k not in dead and k not in consumed else "")
        if stmt:
            out.append(f"        if (!({' || '.join(guard)})) {{ {stmt} }}" if guard else f"        {stmt}")
    return out + before.get(n, [])
