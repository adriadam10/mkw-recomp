"""Differential test of every instruction shape in the game's code: the lifter's C (as the x86 build emits it) against
Unicorn, on random registers, flags, vector registers and memory.

Why: on x86 the game's FP and Advanced SIMD are the portable C lowering (simd='c': tools/simd_c.py, runtime/cpu/simd.h),
written by hand from the Arm pseudocode, and integer instructions go through recomp/lifter.py; lift_diff.py checks only a
few newer classes on synthetic words. A wrong lowering of one form does not crash: it computes a wrong number every time
the game runs it (a transform, a collision, a timer). This takes every distinct word of the game's .text, groups them by
shape (the mnemonic and operands with registers and immediates abstracted), and checks up to --per words of each shape
against QEMU's AArch64 (Unicorn, CPU model "max"), comparing X0-X30, SP, NZCV, V0-V31 and the memory around the access.
FPSR is not compared (simd.h does not model its cumulative bits). FPCR is 0 on both sides, as the game runs it.

Skipped: branches and calls (the lifter does not make them plain C), system instructions (MRS/MSR/SYS/barriers/hints,
SVC), exclusives, PC-relative loads, and anything whose C calls into the runtime (rt_*). Each skip is counted.

Usage (needs clang, so inside the build container):
  uv run --with unicorn --with capstone tools/cpu/lift_game_diff.py [--exefs game/exefs] [--modules main]
      [--per 3] [--runs 4] [--seed 1] [--only REGEX] [--json OUT]
Prints the mismatching shapes with an example each (word, disassembly, what differs) and a summary.
"""

from __future__ import annotations

import argparse
import ctypes
import json
import os
import random
import re
import struct
import subprocess
import sys
import tempfile
from collections import defaultdict
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

import numpy as np
from capstone import CS_ARCH_ARM64, CS_MODE_ARM, Cs
from capstone.arm64 import ARM64_OP_MEM
from unicorn import Uc, UcError
from unicorn import arm64_const as uca
from unicorn.unicorn_const import UC_ARCH_ARM64, UC_MODE_ARM

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "recomp"))

from lifter import Lifter, Unsupported  # noqa: E402
from nso import Nso  # noqa: E402

DEFAULT_EXEFS = Path(os.environ.get("MKW_GAME", "game")) / "exefs"
MEM = 1 << 16  # guest memory around the access, the same on both sides; the access is in its middle (scaled
# unsigned offsets reach +32 KiB)
CODE = 0x1000  # where the instruction sits for Unicorn and for the lifter's PC-relative operands
BATCH = 1500  # cases per compiled library
# The game code's own flags (tools/build/build.py GEN_FLAGS on x86_64)
CFLAGS = ["-O1", "-fno-strict-aliasing", "-march=x86-64-v3", "-shared", "-fPIC", "-w"]

XREG = [getattr(uca, f"UC_ARM64_REG_X{i}") for i in range(31)]
VREG = [getattr(uca, f"UC_ARM64_REG_V{i}") for i in range(32)]
SYSTEM = re.compile(r"^(mrs|msr|sys|sysl|dc|ic|tlbi|at|hint|nop|yield|wfe|wfi|sev|sevl|isb|dsb|dmb|clrex|svc|hvc|smc|"
                    r"brk|hlt|udf|bti|pac|aut|xpac|paci|auti|esb|psb|csdb|ld[a]?xr|st[l]?xr|ldaxp|ldxp|stl?xp|"
                    r"prfm|prfum|cfinv|axflag|xaflag|setf)")  # fmt: skip
REG_TOKEN = re.compile(r"\b(?:[xwvqdshbz]\d{1,2}|[xw]zr|sp|wsp)\b")
NUM_TOKEN = re.compile(r"#?-?(?:0x[0-9a-f]+|\d+(?:\.\d+)?(?:e[+-]?\d+)?)\b")

HARNESS = """
#include <setjmp.h>
#include <signal.h>
#include "cpu.h"
#include "simd.h"
_Alignas(4096) uint8_t lift_mem[%d];
size_t cpu_size(void) { return sizeof(Cpu); }
%s
void (*const cases[])(Cpu *) = { %s };
/* A case that faults (an address outside lift_mem) returns 1 instead of taking the process down. */
static sigjmp_buf jb;
static void on_fault(int sig) { (void)sig; siglongjmp(jb, 1); }
int run_case(int k, Cpu *c) {
    struct sigaction sa = {0}, old_segv, old_bus;
    sa.sa_handler = on_fault;
    sigaction(SIGSEGV, &sa, &old_segv);
    sigaction(SIGBUS, &sa, &old_bus);
    int faulted = sigsetjmp(jb, 1);
    if (!faulted) cases[k](c);
    sigaction(SIGSEGV, &old_segv, NULL);
    sigaction(SIGBUS, &old_bus, NULL);
    return faulted;
}
"""


def shape(text: str) -> str:
    """The disassembly with registers and numbers abstracted: one key per form of an instruction."""
    return NUM_TOKEN.sub("#", REG_TOKEN.sub(lambda m: m.group(0)[0] if m.group(0)[0] in "xwvqdshbz" else m.group(0),
                                           text))  # fmt: skip


def words_of(exefs: Path, modules: list[str]) -> list[int]:
    out: set[int] = set()
    for m in modules:
        nso = Nso(str(exefs / m))
        off, size = nso.text
        out.update(np.unique(np.frombuffer(bytes(nso.image), dtype="<u4", count=size // 4, offset=off)).tolist())
    return sorted(out)


def classify(words: list[int]) -> tuple[list[tuple[str, int, str, str]], dict[str, int]]:
    """(shape, word, disassembly, C) of every liftable plain op, and the skip counts by reason."""
    cs = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    cs.detail = True
    lift = Lifter(0, "c")
    out, skip = [], defaultdict(int)
    for w in words:
        ins = next(cs.disasm(w.to_bytes(4, "little"), CODE), None)
        if ins is None:
            skip["not an instruction (data)"] += 1
            continue
        if SYSTEM.match(ins.mnemonic):
            skip["system, exclusive or hint"] += 1
            continue
        if ins.mnemonic in ("ldr", "ldrsw", "prfm") and not any(o.type == ARM64_OP_MEM for o in ins.operands):
            skip["PC-relative load"] += 1
            continue
        try:
            li = lift.lift(w, CODE)
        except Unsupported:
            skip["unsupported by the lifter"] += 1
            continue
        except Exception:  # noqa: BLE001 -- counted, lift_coverage.py lists them
            skip["lifter error"] += 1
            continue
        if li.kind != "op":
            skip[f"not a plain op ({li.kind})"] += 1
            continue
        if "rt_" in li.code:
            skip["calls the runtime"] += 1
            continue
        out.append((shape(f"{ins.mnemonic} {ins.op_str}"), w, f"{ins.mnemonic} {ins.op_str}", li.code))
    return out, dict(skip)


def pick(cases: list[tuple[str, int, str, str]], per: int, rng: random.Random) -> list[tuple[str, int, str, str]]:
    by = defaultdict(list)
    for c in cases:
        by[c[0]].append(c)
    out = []
    for k in sorted(by):
        v = by[k]
        out.extend(rng.sample(v, min(per, len(v))))
    return out


def build(batch: list[tuple[str, int, str, str]], tmp: Path, tag: str) -> ctypes.CDLL:
    fns = "\n".join(f"static void t{k}(Cpu *c) {{ {code} }}" for k, (_, _, _, code) in enumerate(batch))
    src, lib = tmp / f"{tag}.c", tmp / f"{tag}.so"
    src.write_text(HARNESS % (MEM, fns, ", ".join(f"t{k}" for k in range(len(batch)))))
    subprocess.run(["clang", "-std=gnu11", *CFLAGS, f"-I{ROOT / 'runtime/cpu'}", f"-I{ROOT / 'runtime'}",
                    f"-I{ROOT / 'runtime/core'}", "-o", str(lib), str(src)], check=True)  # fmt: skip
    return ctypes.CDLL(str(lib))


NICE_F32 = [0.0, -0.0, 1.0, -1.0, 0.5, 2.0, 1.5, -3.25, 1e-3, 65504.0, 3.4e38, 1.17549435e-38, 1e-40, float("inf"),
            float("-inf"), float("nan")]  # fmt: skip
NICE_F64 = [0.0, -0.0, 1.0, -1.0, 0.5, 1e-300, 1e300, 2.2250738585072014e-308, 5e-324, float("inf"), float("nan")]
NICE_X = [0, 1, 2, 0x7F, 0x80, 0xFF, 0xFFFF, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 1 << 63, (1 << 63) - 1, (1 << 64) - 1]


def rand_v(rng: random.Random) -> int:
    r = rng.random()
    if r < 0.4:
        return rng.getrandbits(128)
    if r < 0.7:  # four nice floats
        return int.from_bytes(b"".join(struct.pack("<f", rng.choice(NICE_F32)) for _ in range(4)), "little")
    if r < 0.85:  # two nice doubles
        return int.from_bytes(b"".join(struct.pack("<d", rng.choice(NICE_F64)) for _ in range(2)), "little")
    # moderate floats: what transforms carry
    return int.from_bytes(b"".join(struct.pack("<f", rng.uniform(-1e4, 1e4)) for _ in range(4)), "little")


_CS = None


def mem_regs(w: int) -> tuple[int | None, int | None]:
    """(base, index) register numbers of the memory operand (31 = SP for a base), or None."""
    global _CS
    if _CS is None:
        _CS = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
        _CS.detail = True
    ins = next(_CS.disasm(w.to_bytes(4, "little"), CODE))
    for o in ins.operands:
        if o.type == ARM64_OP_MEM:
            name = lambda r: ins.reg_name(r) if r else None  # noqa: E731
            b, i = name(o.mem.base), name(o.mem.index)
            alias = {"sp": 31, "wsp": 31, "fp": 29, "lr": 30, "x29": 29, "x30": 30}  # capstone's names for x29/x30
            num = lambda n: None if n is None else alias[n] if n in alias else int(n[1:]) if n[1:].isdigit() else None  # noqa: E731
            return num(b), num(i)
    return None, None


def run_one(lib: ctypes.CDLL, k: int, w: int, rng: random.Random) -> list[str]:
    host_mem = ctypes.addressof(ctypes.c_uint8.in_dll(lib, "lift_mem"))
    x = [rng.choice(NICE_X) if rng.random() < 0.3 else rng.getrandbits(64) for _ in range(31)]
    sp = host_mem + MEM // 2
    base, index = mem_regs(w)
    if base is not None:
        if base == 31:
            sp = host_mem + MEM // 2 + 16 * rng.randrange(64)
        else:
            x[base] = host_mem + MEM // 2 + 16 * rng.randrange(64)
    if index is not None and index != base:
        x[index] = rng.randrange(256)
    v = [rand_v(rng) for _ in range(32)]
    nzcv = rng.randrange(16) << 28
    mem = rng.randbytes(MEM)

    ctypes.memmove(host_mem, mem, MEM)
    cpu = (ctypes.c_uint64 * (lib.cpu_size() // 8))()
    for i in range(31):
        cpu[i] = x[i]
    cpu[32], cpu[34] = sp, nzcv  # sp (256), nzcv (272); fpcr (320) stays 0
    for i in range(32):
        cpu[64 + 2 * i], cpu[64 + 2 * i + 1] = v[i] & ((1 << 64) - 1), v[i] >> 64
    if lib.run_case(k, ctypes.addressof(cpu)):
        return ["the lifted C faulted (an address outside the test memory: base or index not as the shape needs)"]
    got_x = [cpu[i] for i in range(31)]
    got_sp, got_nzcv = cpu[32], cpu[34] & 0xF0000000
    got_v = [cpu[64 + 2 * i] | cpu[64 + 2 * i + 1] << 64 for i in range(32)]
    got_mem = ctypes.string_at(host_mem, MEM)

    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    uc.ctl_set_cpu_model(uca.UC_CPU_ARM64_MAX)
    uc.mem_map(CODE, 0x1000)
    uc.mem_write(CODE, struct.pack("<I", w))
    uc.mem_map(host_mem, MEM)
    uc.mem_write(host_mem, mem)
    for i in range(31):
        uc.reg_write(XREG[i], x[i])
    for i in range(32):
        uc.reg_write(VREG[i], v[i])
    uc.reg_write(uca.UC_ARM64_REG_SP, sp)
    uc.reg_write(uca.UC_ARM64_REG_NZCV, nzcv)
    uc.reg_write(uca.UC_ARM64_REG_FPCR, 0)
    try:
        uc.emu_start(CODE, CODE + 4)
    except UcError as e:
        return [f"unicorn: {e}"]
    bad = [f"x{i} {got_x[i]:#x} != {uc.reg_read(XREG[i]):#x}" for i in range(31) if got_x[i] != uc.reg_read(XREG[i])]
    if got_sp != uc.reg_read(uca.UC_ARM64_REG_SP):
        bad.append(f"sp {got_sp:#x} != {uc.reg_read(uca.UC_ARM64_REG_SP):#x}")
    if got_nzcv != uc.reg_read(uca.UC_ARM64_REG_NZCV) & 0xF0000000:
        bad.append(f"nzcv {got_nzcv >> 28:04b} != {uc.reg_read(uca.UC_ARM64_REG_NZCV) >> 28 & 15:04b}")
    for i in range(32):
        ref = uc.reg_read(VREG[i])
        if got_v[i] != ref:
            bad.append(f"v{i} {got_v[i]:032x} != {ref:032x}")
    ref_mem = bytes(uc.mem_read(host_mem, MEM))
    if got_mem != ref_mem:
        j = next(j for j in range(MEM) if got_mem[j] != ref_mem[j])
        bad.append(f"mem+{j - MEM // 2:#x} {got_mem[j]:#04x} != {ref_mem[j]:#04x}")
    if uc.reg_read(uca.UC_ARM64_REG_PC) != CODE + 4:
        bad.append("unicorn did not run past the instruction")
    return bad


def check_batch(args: tuple[int, list[tuple[str, int, str, str]], int, int]) -> list[tuple[str, int, str, list[str]]]:
    n, batch, runs, seed = args
    rng = random.Random(seed * 1000003 + n)
    with tempfile.TemporaryDirectory() as tmp:
        try:
            lib = build(batch, Path(tmp), f"b{n}")
        except subprocess.CalledProcessError:
            if len(batch) == 1:
                return [(batch[0][0], batch[0][1], batch[0][2], ["the lifted C does not compile"])]
            half = len(batch) // 2  # find the case that breaks the build
            return check_batch((n * 2, batch[:half], runs, seed)) + check_batch((n * 2 + 1, batch[half:], runs, seed))
        out = []
        for k, (sh, w, text, _) in enumerate(batch):
            for _ in range(runs):
                bad = run_one(lib, k, w, rng)
                if bad:
                    out.append((sh, w, text, bad))
                    break
        return out


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--exefs", type=Path, default=DEFAULT_EXEFS)
    ap.add_argument("--modules", default="main")
    ap.add_argument("--per", type=int, default=3, help="words checked per shape")
    ap.add_argument("--runs", type=int, default=4, help="random states per word")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--only", help="regex on the shape: check only those")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--json", type=Path)
    a = ap.parse_args()

    words = words_of(a.exefs, a.modules.split(","))
    chunks = [words[i :: a.jobs] for i in range(a.jobs)]
    with ProcessPoolExecutor(a.jobs) as pool:
        parts = list(pool.map(classify, chunks))
    cases = [c for p, _ in parts for c in p]
    skip: dict[str, int] = defaultdict(int)
    for _, s in parts:
        for k, v in s.items():
            skip[k] += v
    if a.only:
        cases = [c for c in cases if re.search(a.only, c[0])]
    rng = random.Random(a.seed)
    chosen = pick(cases, a.per, rng)
    nshapes = len({c[0] for c in chosen})
    print(f"{len(words)} distinct words; {len(cases)} plain ops in {nshapes} shapes; checking {len(chosen)} words, "
          f"{a.runs} random states each", flush=True)  # fmt: skip
    for k, v in sorted(skip.items(), key=lambda kv: -kv[1]):
        print(f"  skipped: {v} {k}")
    batches = [(n, chosen[i : i + BATCH], a.runs, a.seed) for n, i in enumerate(range(0, len(chosen), BATCH))]
    with ProcessPoolExecutor(a.jobs) as pool:
        bad = [b for part in pool.map(check_batch, batches) for b in part]
    by_shape = defaultdict(list)
    for sh, w, text, why in bad:
        by_shape[sh].append((w, text, why))
    for sh in sorted(by_shape, key=lambda s: -len(by_shape[s])):
        w, text, why = by_shape[sh][0]
        print(f"MISMATCH {sh}  ({len(by_shape[sh])} of its words)  e.g. {w:#010x} {text}: {'; '.join(why[:3])}")
    print(f"{len(by_shape)} of {nshapes} shapes mismatch ({len(bad)} of {len(chosen)} words)")
    if a.json:
        a.json.write_text(json.dumps({sh: [[f"{w:#010x}", t, why] for w, t, why in v] for sh, v in by_shape.items()},
                                     indent=1))  # fmt: skip


if __name__ == "__main__":
    main()
