"""Off-GPU simulation of a program's control flow: the original branch graph against the structured GLSL that flow.py
builds from it, on the same pseudo-random branch outcomes. Used by flow.structure to refuse a structuring that runs
other instructions (it then keeps the switch) and by tools/shaders/flow_check.py over the census.

Why: a wrongly structured loop runs to LOOP_CAP in every pixel and hangs the GPU; run93 (31/10/2026) reset the GPU of the
whole desktop, and flow_check then found 57 of 7 643 structured programs whose structured form ran other instructions
(a flagged jump into the else-part of an if / else). Each instruction becomes a marker statement and each branch
condition a named oracle; both forms see the same answer for the n-th evaluation of a branch, so equal programs give
equal traces. Predicated EXITs are oracles too.
"""

import hashlib
import re

STEPS = 20000
def oracle(seed: int, name: str, occ: int) -> bool:
    return hashlib.blake2b(f"{seed}:{name}:{occ}".encode(), digest_size=1).digest()[0] & 1 == 1


def _balanced(s: str) -> bool:
    depth = 0
    for ch in s:
        depth += (ch == "(") - (ch == ")")
        if depth < 0:
            return False
    return depth == 0


def _top_or(c: str) -> list[str]:
    """c split at its " || " outside parentheses."""
    parts, depth, start = [], 0, 0
    for k, ch in enumerate(c):
        depth += (ch == "(") - (ch == ")")
        if depth == 0 and c.startswith(" || ", k):
            parts.append(c[start:k])
            start = k + 4
    return parts + [c[start:]]


class Run:
    def __init__(self, seed: int):
        self.seed, self.occ, self.trace, self.done = seed, {}, [], False

    def ask(self, name: str) -> bool:
        n = self.occ.get(name, 0)
        self.occ[name] = n + 1
        return oracle(self.seed, name, n)

    def cond(self, c: str, flags: dict) -> bool:
        c = c.strip()
        if c == "true":
            return True
        parts = _top_or(c)
        if len(parts) > 1:  # a guarded if (flow.py): left to right, stopping at the first true one, as GLSL does
            return any(self.cond(p, flags) for p in parts)
        if c.startswith("(") and c.endswith(")") and len(_top_or(c[1:-1])) == 1 and _balanced(c[1:-1]):
            return self.cond(c[1:-1], flags)
        if c.startswith("!(") and c.endswith(")"):
            return not self.cond(c[2:-1], flags)
        if c in flags or re.fullmatch(r"g\d+", c):   # a guard flag no jump has set yet is false, as declared
            return flags.get(c, False)
        return self.ask(c)

    def stmt(self, s: str) -> None:
        """S<k>: executed; R<k>: return; X<k>: return if its oracle says so."""
        kind, k = s[0], s[1:]
        self.trace.append(int(k))
        if kind == "R" or (kind == "X" and self.ask(f"X{k}")):
            self.done = True


def markers(stmts: list[str]) -> list[str]:
    out = []
    for k, s in enumerate(stmts):
        if not s:
            out.append("")
        elif "return;" in s and s.lstrip().startswith("if ("):
            out.append(f"X{k};")  # a predicated EXIT
        elif "return;" in s:
            out.append(f"R{k};")
        else:
            out.append(f"S{k};")
    return out


def run_original(stmts: list[str], branches: dict, seed: int) -> list[int]:
    r, i, n, steps = Run(seed), 0, len(stmts), 0
    while 0 <= i < n and not r.done and steps < STEPS:
        steps += 1
        if i in branches:
            t, c = branches[i]
            i = t if c is None or r.ask(c) else i + 1
            continue
        if stmts[i]:
            r.stmt(stmts[i].rstrip(";"))
        i += 1
    return r.trace


def parse(lines: list[str]):
    """Nested blocks: ('stmt', s, guard) | ('set', flag, cond, guard) | ('if', cond, then, else) | ('do', body, cond)
    | ('reset', [flags])."""
    pos = 0

    def block(stop: tuple[str, ...]):
        nonlocal pos
        out = []
        while pos < len(lines):
            s = lines[pos].strip()
            if any(s.startswith(x) for x in stop):
                return out
            pos += 1
            if not s or s.startswith("bool "):
                continue
            if s == "do {":
                body = block(("} while",))
                end = lines[pos].strip()
                pos += 1
                out.append(("do", body, re.match(r"\} while \(\((.*)\) && \+\+loops < LOOP_CAP\);", end).group(1)))
            elif s.startswith("if (!(") and s.endswith(") {"):
                cond = s[len("if (!(") : -len(")) {")]
                then = block(("} else {", "}"))
                other = []
                if lines[pos].strip() == "} else {":
                    pos += 1
                    other = block(("}",))
                pos += 1
                out.append(("if", cond, then, other))
            elif re.fullmatch(r"(g\d+ = false; ?)+", s):
                out.append(("reset", re.findall(r"(g\d+) = false", s)))
            else:
                guard = None
                m = re.fullmatch(r"if \(!\((.*?)\)\) \{ (.*) \}", s)
                if m:
                    guard, s = m.group(1).split(" || "), m.group(2)
                m = re.fullmatch(r"(g\d+) = (.*);", s)
                if m:
                    out.append(("set", m.group(1), m.group(2), guard))
                else:
                    out.append(("stmt", s.rstrip(";"), guard))
        return out

    return block(("\0",))


def run_structured(tree, seed: int) -> list[int]:
    r, flags, steps = Run(seed), {}, [0]

    def ex(nodes) -> None:
        for node in nodes:
            if r.done or steps[0] >= STEPS:
                return
            steps[0] += 1
            kind = node[0]
            if kind in ("stmt", "set") and node[-1] and any(flags.get(g, False) for g in node[-1]):
                continue
            if kind == "stmt":
                r.stmt(node[1])
            elif kind == "set":
                flags[node[1]] = r.cond(node[2], flags)
            elif kind == "reset":
                for g in node[1]:
                    flags[g] = False
            elif kind == "if":
                ex(node[3] if r.cond(node[1], flags) else node[2])
            elif kind == "do":
                while True:
                    ex(node[1])
                    if r.done or steps[0] >= STEPS or not r.cond(node[2], flags):
                        break

    ex(tree)
    return r.trace


def named(branches: dict) -> dict:
    """Branch conditions as oracles named after their instruction."""
    return {k: (t, None if c is None else f"C{k}") for k, (t, c) in branches.items()}


def diff(mk: list[str], nb: dict, lines: list[str], seeds: int) -> str | None:
    """None when the structured lines run the same instructions as the branch graph for every seed."""
    tree = parse(lines)
    for seed in range(seeds):
        a, b = run_original(mk, nb, seed), run_structured(tree, seed)
        if a != b:
            at = next((k for k, (x, y) in enumerate(zip(a, b)) if x != y), min(len(a), len(b)))
            return f"seed {seed} at step {at}: original {a[at:at + 4]} structured {b[at:at + 4]}"
    return None
