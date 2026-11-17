"""Audit of the translator's per-register tracking (ur_ssbo, ur_derived, reg_ssbo, fold) over every extracted program:
a use of a tracked fact whose register an instruction rewrote after the fact was recorded is a stale use, shown with
the program, pc and instruction. Uses that only cross a jump target (another path may arrive with another value) are
counted apart.

Why: a stale ur_ssbo entry dropped the offset of an ATOMG (UR4 rewritten by ULDC + UIADD3 after a ULDC.64 of a storage
buffer address): the cacti's draw arguments (08/11/2026). This finds every case of the pattern
without running the game, and proves a fix (0 stale uses after it).

Usage: uv run tools/shaders/track_audit.py [--examples N] [--brx-any]   (~1 min, by job)
"""

import argparse
import csv
import os
import sys
from collections import Counter
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT / "sm86/translate"), str(ROOT / "sm86/translate/frontend")]
sys.path.insert(0, str(ROOT / "tools/shaders"))

import glsl
from extract import trim
from glsl import core
from program import Decoder

RZ, URZ = 255, 63
DICTS = ("ur_ssbo", "ur_derived", "reg_ssbo", "fold")
KINDS = {"ur_ssbo": "u", "ur_derived": "u", "reg_ssbo": "r", "fold": "r"}
BRX_ANY = "--brx-any" in sys.argv   # read by the worker processes too
_dec = None


class TrackDict(dict):
    """A tracking dict that tells the audit which keys the translator (re)set and which it consulted."""

    def __init__(self, b, name, kind):
        super().__init__()
        self.b, self.name, self.kind = b, name, kind

    def __setitem__(self, k, v):
        super().__setitem__(k, v)
        self.b._set_now.add((self.name, k))

    def clear(self):
        super().clear()
        self.b._joins += 1

    def get(self, k, default=None):
        if self.name == "fold" and k in self:
            self.b._use(self.name, k)
        return super().get(k, default)


def dest_regs(i):
    """(kind, register) pairs an instruction writes: Rd/Rd2 and URd, with the width its size names."""
    wide = {i.sym(k) for k in ("sz", "wide", "fmt")} & {"64", "128", "WIDE", "U64", "S64"}
    width = 4 if "128" in wide else 2 if wide else 1
    out = []
    for field, kind, zero in (("Rd", "r", RZ), ("Rd2", "r", RZ), ("URd", "u", URZ)):
        if i.has(field) and i.v(field) != zero:
            out += [(kind, i.v(field) + k) for k in range(width)]
    return out


def patch() -> None:
    init0, insn0, track0, gaddr0 = core.Builder.__init__, core.Builder.insn, core.Builder.track_ssbo, core.Builder.global_addr
    kinds = KINDS

    def init(self, *a, **k):
        init0(self, *a, **k)
        self._ver, self._setver, self._setjoin, self._set_now, self._joins = Counter(), {}, {}, set(), 0
        self._found, self._pc, self._op, self._lastw, self._setpcs = [], 0, "", {}, {}
        for n in DICTS:
            setattr(self, n, TrackDict(self, n, kinds[n]))

    def use(self, name, reg):
        key, r = (name, reg), (kinds[name], reg)
        if self._ver[r] != self._setver.get(key, -1):
            self._found.append(("stale", name, self._pc, self._op, f"{reg} rewritten by {self._lastw.get(r)}"))
        elif self._setjoin.get(key) != self._joins:
            self._found.append(("join", name, self._pc, self._op, reg))
        else:
            self._found.append(("ok", name, self._pc, self._op, reg))

    def insn(self, i):
        self._pc, self._op, self._set_now = i.pc, i.cls, set()
        return insn0(self, i)

    def track(self, i):
        track0(self, i)
        for r in dest_regs(i):
            self._ver[r] += 1
            if not any(n == r[1] and (kinds[d], n) == r for d, n in self._set_now):
                self._lastw[r] = i.cls.split("_")[0]
        for name, k in self._set_now:  # a fact recorded by this instruction holds from its write on
            self._setver[(name, k)] = self._ver[(kinds[name], k)]
            self._setjoin[(name, k)] = self._joins
            self._setpcs.setdefault((kinds[name], k), set()).add(i.pc)

    def gaddr(self, i):
        base = next((i.v(k) for k in ("Ra_URb", "Ra_URc") if i.has(k)), None)
        ra = i.v("Ra") if i.has("Ra") else RZ
        if base is not None and base in self.ur_ssbo:
            self._use("ur_ssbo", base)
        elif base is not None and base in self.ur_derived:
            self._use("ur_derived", base)
        elif base is None and ra in self.reg_ssbo:
            self._use("reg_ssbo", ra)
        return gaddr0(self, i)

    core.Builder.__init__, core.Builder.insn, core.Builder.track_ssbo = init, insn, track
    core.Builder.global_addr, core.Builder._use = gaddr, use


def init() -> None:
    global _dec
    _dec = Decoder(ROOT / "build/sm86/instructions.json")
    patch()


def one(row):
    sha, stage = row
    shaders = ROOT / "build/shaders"
    code = trim((shaders / f"{sha}.code").read_bytes(), stage)
    ctl = (shaders / f"{sha}.ctl").read_bytes()
    found = []
    orig = core.Builder.__init__

    def keep(self, *a, **k):  # the audit's Builder of this program
        orig(self, *a, **k)
        found.append(self)

    core.Builder.__init__ = keep
    try:
        sph, insns = _dec.program(code, stage)
        glsl.translate(sph, insns, stage, glsl.compute_shape(ctl) if stage == "cs" else None)
    except (glsl.Unsupported, KeyError, ValueError, IndexError):
        pass
    finally:
        core.Builder.__init__ = orig
    if not found:
        return sha, []
    b, out = found[0], []
    preds = predecessors(insns)
    index = {i.pc: k for k, i in enumerate(insns)}
    for kind, name, pc, op, reg in b._found:
        if kind == "join":
            r = (KINDS[name], reg)
            if preds is None:
                kind = "join-brx"   # BRX/RET targets come from run time: no paths to follow
            else:
                bad = [d for d in reaching(insns, preds, index[pc], r) if d not in b._setpcs.get(r, set())]
                kind = "join-bug" if bad else "join-safe"
                if bad:
                    reg = f"{reg}, also written at {', '.join(d if isinstance(d, str) else hex(d) for d in bad[:3])}"
        out.append((kind, name, pc, op, reg))
    return sha, out


def predicated(i) -> bool:
    return i.v("Pg", 7) != 7 or bool(i.v("Pg@not", 0)) or i.v("UPg", 7) != 7


def predecessors(insns):
    """Per instruction index, the indices that can run right before it. BRX and RET take their target at run time:
    with --brx-any any instruction may follow them (the translator's own assumption), else None (not followed)."""
    if any(i.op in ("BRX", "BRXU", "RET") for i in insns) and not BRX_ANY:
        return None
    index = {i.pc: k for k, i in enumerate(insns)}
    preds = [[] for _ in insns]
    for k, i in enumerate(insns):
        succ = []
        if i.op in ("BRA", "CALL"):
            t = index.get(i.pc + 16 + 4 * i.sv("sImm" if i.op == "BRA" else "Sa"))
            if t is not None and t != k:
                succ.append(t)
            if predicated(i):
                succ.append(k + 1)
        elif i.op in ("BRX", "BRXU", "RET"):
            succ.extend(range(len(insns)))
        elif i.op == "EXIT":
            if predicated(i):
                succ.append(k + 1)
        else:
            succ.append(k + 1)
        for s in succ:
            if s < len(insns):
                preds[s].append(k)
    return preds


def reaching(insns, preds, use, r):
    """pcs of the writes of register r that reach instruction index use along some path ("entry": none on a path)."""
    out, seen, work = set(), set(), list(preds[use])
    if not work:
        out.add("entry")
    while work:
        k = work.pop()
        if k in seen:
            continue
        seen.add(k)
        if r in dest_regs(insns[k]):
            out.add(insns[k].pc)
            if not predicated(insns[k]):
                continue
        if not preds[k]:
            out.add("entry")
        work.extend(preds[k])
    return sorted(out, key=str)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--examples", type=int, default=5)
    ap.add_argument("--brx-any", action="store_true", help="any instruction may follow a BRX or RET")
    a = ap.parse_args()
    with (ROOT / "build/shaders/index.csv").open() as f:
        rows = [(r["sha1"], r["stage"]) for r in csv.DictReader(f)]
    with ProcessPoolExecutor(os.cpu_count(), initializer=init) as pool:
        res = list(pool.map(one, rows, chunksize=16))
    uses, progs, ex, writers = Counter(), {}, {}, Counter()
    for sha, found in res:
        for kind, name, pc, op, reg in found:
            uses[(name, kind)] += 1
            progs.setdefault((name, kind), set()).add(sha)
            ex.setdefault((name, kind), []).append(f"{sha[:12]} pc {pc:#x} {op} reg {reg}")
            if kind == "stale":
                writers[(name, str(reg).split("by ")[-1])] += 1
    print(f"{len(res)} programs")
    for name in DICTS:
        for kind in ("ok", "join-safe", "join-brx", "join-bug", "stale"):
            if uses[(name, kind)]:
                print(f"{name:10s} {kind:5s} {uses[(name, kind)]:7d} uses in {len(progs[(name, kind)]):5d} programs")
                if kind not in ("ok", "join-safe", "join-brx"):
                    for e in ex[(name, kind)][: a.examples]:
                        print(f"    {e}")
    for (name, w), n in writers.most_common():
        print(f"stale {name}: register last rewritten by {w}: {n} uses")


if __name__ == "__main__":
    main()
