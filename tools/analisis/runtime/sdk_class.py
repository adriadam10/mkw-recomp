"""What each SDK import of main ends in when it runs as the SDK's recompiled code: a console service, the kernel, or
nothing of the console (pure library code, right as it is).

Why: the SDK progress bar counted only the imports replaced by name, as if the other 1 100 were all missing; most are
library code (nn::util, libc, math) that needs nothing from the console (Adrià, 05/11/2026: split the bar in three).
An import is
- "service" if the game did not call it in the census run and its direct calls reach an IPC system call
  (ConnectToNamedPort, SendSyncRequest...) or its namespace talks to a console service (SERVICE_NS of sdk_gaps.py):
  the SDK reaches most services through proxy objects held from an earlier call, which a static walk does not follow.
  The runtime serves no IPC since 05/11/2026 (an IPC system call is a named fatal), so an import the game called in a
  run that did not stop reaches no service (nn::movie's WebM demuxer, nn::hid's vibration mixer, and
  nv::GetGraphicsMemoryInfo, whose IPC branch sits behind a callback the host's NVN layer sets);
- "kernel" if its direct calls reach another system call (threads, waits, memory, time: the runtime's kernel.c);
- "pure" otherwise.
The walk follows BL and tail B, through PLT stubs into the other modules, and stops at functions the host replaces
by name (their system calls never run) and at the error paths (STOP: abort,
log, unexpected result, C++ throw), which reach Break, OutputDebugString, the lm service and a kernel mutex from
nearly every function (VSNPrintf through nn::detail::UnexpectedDefaultImpl, std::__next_prime through __cxa_throw).

Usage: uv run tools/analisis/runtime/sdk_class.py [--db build/bindb.sqlite] [--census build/run/NAME.imports] [--class kernel]
       [--namespace nn::os]
       (needs the svc table of tools/analisis/bin/bindb.py)
"""

import argparse
import sqlite3
from collections import Counter, defaultdict
from pathlib import Path

from sdk_gaps import SERVICE_NS, namespace, overrides, read_census

ROOT = Path(__file__).resolve().parents[3]
IPC_SVC = {0x1F, 0x21, 0x22, 0x23}  # ConnectToNamedPort, SendSyncRequest(WithUserBuffer), SendAsyncRequest...
STOP = ("nn::diag::", "nn::detail::UnexpectedDefault", "__cxa_throw", "abort", "std::terminate",
        "std::__1::__libcpp_verbose_abort")  # fmt: skip


def classify(db: sqlite3.Connection, called: set[str] = frozenset()) -> dict[str, str]:
    """import name of main -> "service" | "kernel" | "pure"; called: the imports a census run called."""
    replaced = overrides()
    plt = dict(db.execute("SELECT addr, target FROM plt WHERE target IS NOT NULL"))
    names = dict(db.execute("SELECT addr, name FROM func WHERE name IS NOT NULL"))
    exported = dict(db.execute("SELECT name, addr FROM sym WHERE module != 'main' AND kind = 'func'"))
    host = {exported[n] for n in replaced if n in exported}  # replaced by name: their bodies never run
    callers: dict[int, set[int]] = defaultdict(set)
    for caller, target in db.execute("SELECT caller, target FROM call c JOIN func f ON f.addr = c.caller "
                                     "WHERE f.module != 'main'"):  # fmt: skip
        callers[plt.get(target, target)].add(caller)
    reach: dict[int, set[int]] = defaultdict(set)  # function -> system calls its direct calls reach
    for func, num in db.execute("SELECT func, num FROM svc"):
        if func in host:
            continue
        todo, seen = [func], {func}
        while todo:
            f = todo.pop()
            reach[f].add(num)
            for c in callers.get(f, ()):
                if c not in seen and c not in host and not (names.get(c) or "").startswith(STOP):
                    seen.add(c)
                    todo.append(c)
    out = {}
    for name, dem in db.execute("SELECT name, demangled FROM sym WHERE module = 'main' AND kind = 'import'"):
        svcs = reach.get(exported.get(name, 0), set())
        if name not in called and (svcs & IPC_SVC or namespace(dem) in SERVICE_NS):
            out[name] = "service"
        else:
            out[name] = "kernel" if svcs else "pure"
    return out


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--db", type=Path, default=ROOT / "build/bindb.sqlite")
    ap.add_argument("--census", type=Path, help="MKW_IMPORT_CENSUS file of a run (its calls reach no service)")
    ap.add_argument("--class", dest="cls", help="list the imports of one class")
    ap.add_argument("--namespace", help="only this namespace")
    args = ap.parse_args()
    db = sqlite3.connect(args.db)
    cls = classify(db, set(read_census(args.census)) if args.census else set())
    dem = dict(db.execute("SELECT name, demangled FROM sym WHERE module = 'main' AND kind = 'import'"))
    rows = [(n, c) for n, c in cls.items() if not args.namespace or namespace(dem[n]) == args.namespace]
    if args.cls:
        print("\n".join(sorted(dem[n] for n, c in rows if c == args.cls)))
        return
    print(dict(Counter(c for _, c in rows)))
    by_ns: dict[str, Counter] = defaultdict(Counter)
    for n, c in rows:
        by_ns[namespace(dem[n])][c] += 1
    for ns, cnt in sorted(by_ns.items(), key=lambda kv: -sum(kv[1].values()))[:30]:
        print(f"{ns:<22} " + "  ".join(f"{k} {cnt[k]}" for k in ("service", "kernel", "pure") if cnt[k]))


if __name__ == "__main__":
    main()
