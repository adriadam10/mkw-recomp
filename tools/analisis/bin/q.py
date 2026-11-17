"""Short answers from build/bindb.sqlite (built by tools/analisis/bin/bindb.py).

Why: a question about the executable should cost a few lines of output, not a disassembly listing. Every address is
printed as module+offset with the function's name when it has one.

Addresses: absolute hex (0x7000123456), module+offset (main+0x19e5df0) or a symbol name (exact or demangled).

Usage: uv run tools/analisis/bin/q.py <command> <arg> [--limit N]
  at ADDR         function containing ADDR, its size and name
  fn ADDR         function summary: callers, callees (named first), strings and data it references
  callers ADDR    who calls the function at ADDR (BL and tail B)
  callees ADDR    what the function at ADDR calls
  str TEXT        strings containing TEXT and the functions that reference them
  sym PATTERN     symbols whose name or demangled name contains PATTERN (exports, imports, objects)
  imports PATTERN imports of main whose demangled name contains PATTERN, with how many call sites each
  refs ADDR       functions that reference the data address ADDR
  vptrs ADDR      data slots that point to the function at ADDR (indirect calls: vtables, callbacks), each with its
                  vtable column, the slot index and the functions that load that column (constructors)
  vtable ADDR     the function pointers of the vtable column holding the data address ADDR
"""

import argparse
import sqlite3
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
DB = ROOT / "build/bindb.sqlite"


class Bin:
    def __init__(self, path: Path) -> None:
        if not path.exists():
            sys.exit(f"{path} missing: run uv run tools/analisis/bin/bindb.py first")
        self.db = sqlite3.connect(path)
        self.mods = self.db.execute("SELECT name, base, size FROM module ORDER BY base").fetchall()

    def rel(self, a: int | None) -> str:
        if a is None:
            return "?"
        for name, base, size in self.mods:
            if base <= a < base + size:
                return f"{name}+{a - base:#x}"
        return f"{a:#x}"

    def name(self, a: int) -> str:
        """module+offset, then the function or PLT import name when there is one."""
        row = self.db.execute("SELECT name FROM plt WHERE addr=?", (a,)).fetchone()
        if row:
            return f"{self.rel(a)} [plt] {row[0]}"
        row = self.db.execute("SELECT addr, name FROM func WHERE addr<=? ORDER BY addr DESC LIMIT 1", (a,)).fetchone()
        if row and row[1]:
            return f"{self.rel(a)} {row[1]}" + (f" +{a - row[0]:#x}" if a != row[0] else "")
        return self.rel(a)

    def addr(self, text: str) -> int:
        if "+" in text:
            mod, off = text.split("+", 1)
            for name, base, _ in self.mods:
                if name == mod:
                    return base + int(off, 16)
            sys.exit(f"unknown module {mod}")
        try:
            return int(text, 16)
        except ValueError:
            pass
        row = self.db.execute(
            "SELECT addr FROM sym WHERE addr IS NOT NULL AND (name=? OR demangled=?) LIMIT 1", (text, text)
        ).fetchone()
        row = row or self.db.execute("SELECT addr FROM func WHERE name=? LIMIT 1", (text,)).fetchone()
        if not row:
            sys.exit(f"no symbol {text}")
        return row[0]

    def func(self, a: int) -> tuple[int, int, str | None]:
        row = self.db.execute("SELECT addr, end, name FROM func WHERE addr<=? ORDER BY addr DESC LIMIT 1", (a,))
        found = row.fetchone()
        if not found:
            sys.exit(f"{a:#x} is in no function")
        return found


def cmd_at(b: Bin, arg: str, limit: int) -> None:
    a = b.addr(arg)
    start, end, name = b.func(a)
    print(f"{b.rel(a)} in {b.rel(start)} ({end - start:#x} bytes) {name or ''}")


def callees(b: Bin, start: int, end: int) -> list[tuple[int, int]]:
    return b.db.execute(
        "SELECT target, count(*) FROM call WHERE site>=? AND site<? AND NOT (target>=? AND target<?) "
        "GROUP BY target ORDER BY count(*) DESC",
        (start, end, start, end),
    ).fetchall()


def cmd_fn(b: Bin, arg: str, limit: int) -> None:
    start, end, name = b.func(b.addr(arg))
    ncallers = b.db.execute(
        "SELECT count(DISTINCT caller) FROM call WHERE target=? OR target IN (SELECT addr FROM plt WHERE target=?)",
        (start, start),
    ).fetchone()[0]
    out = callees(b, start, end)
    print(f"{b.rel(start)} {name or ''} ({end - start:#x} bytes, {ncallers} callers, {len(out)} callees)")
    named = sorted(out, key=lambda t: "[plt]" not in b.name(t[0]) and not b.name(t[0]).count(" "))
    for t, n in named[:limit]:
        print(f"  -> {b.name(t)}" + (f" x{n}" if n > 1 else ""))
    strs = b.db.execute(
        "SELECT DISTINCT s.text FROM ref r JOIN str s ON s.addr=r.target WHERE r.site>=? AND r.site<?", (start, end)
    ).fetchall()
    for (s,) in strs[:limit]:
        print(f'  "{s[:100]}"')
    data = b.db.execute(
        "SELECT count(DISTINCT target) FROM ref WHERE site>=? AND site<? AND target NOT IN (SELECT addr FROM str)",
        (start, end),
    ).fetchone()[0]
    print(f"  {len(strs)} strings, {data} other data addresses")


def cmd_callers(b: Bin, arg: str, limit: int) -> None:
    start, _, _ = b.func(b.addr(arg))
    # Calls from other modules land on their PLT stub for this function, not on it.
    rows = b.db.execute(
        "SELECT caller, count(*), group_concat(DISTINCT kind) FROM call "
        "WHERE target=? OR target IN (SELECT addr FROM plt WHERE target=?) GROUP BY caller ORDER BY count(*) DESC",
        (start, start),
    ).fetchall()
    print(f"{len(rows)} callers of {b.name(start)}")
    for caller, n, kinds in rows[:limit]:
        print(f"  {b.name(caller)}" + (f" x{n}" if n > 1 else "") + (" (tail)" if kinds == "b" else ""))


def cmd_callees(b: Bin, arg: str, limit: int) -> None:
    start, end, _ = b.func(b.addr(arg))
    rows = callees(b, start, end)
    print(f"{len(rows)} callees of {b.name(start)}")
    for t, n in rows[:limit]:
        print(f"  {b.name(t)}" + (f" x{n}" if n > 1 else ""))


def cmd_str(b: Bin, arg: str, limit: int) -> None:
    rows = b.db.execute("SELECT addr, text FROM str WHERE text LIKE ? LIMIT ?", (f"%{arg}%", limit)).fetchall()
    for a, text in rows:
        users = b.db.execute("SELECT DISTINCT func FROM ref WHERE target=? LIMIT 5", (a,)).fetchall()
        print(f'{b.rel(a)} "{text[:100]}"')
        for (f,) in users:
            print(f"  <- {b.name(f)}")


def cmd_sym(b: Bin, arg: str, limit: int) -> None:
    rows = b.db.execute(
        "SELECT module, addr, kind, demangled FROM sym WHERE name LIKE ? OR demangled LIKE ? LIMIT ?",
        (f"%{arg}%", f"%{arg}%", limit),
    ).fetchall()
    for mod, a, kind, dem in rows:
        print(f"{kind:<6} {b.rel(a) if a else mod:<22} {dem}")


def cmd_imports(b: Bin, arg: str, limit: int) -> None:
    rows = b.db.execute(
        "SELECT p.name, count(c.site) FROM plt p LEFT JOIN call c ON c.target=p.addr "
        "WHERE p.module='main' AND p.name LIKE ? GROUP BY p.addr ORDER BY count(c.site) DESC LIMIT ?",
        (f"%{arg}%", limit),
    ).fetchall()
    for name, n in rows:
        print(f"{n:>6}  {name}")


def cmd_refs(b: Bin, arg: str, limit: int) -> None:
    a = b.addr(arg)
    rows = b.db.execute(
        "SELECT func, count(*) FROM ref WHERE target=? GROUP BY func ORDER BY count(*) DESC LIMIT ?", (a, limit)
    ).fetchall()
    print(f"{len(rows)} functions reference {b.rel(a)}")
    for f, n in rows:
        print(f"  {b.name(f)}" + (f" x{n}" if n > 1 else ""))


def column_users(b: Bin, vt: int) -> list[int]:
    """Functions that reference a vtable column: the address point itself or the two words before it."""
    rows = b.db.execute("SELECT DISTINCT func FROM ref WHERE target BETWEEN ? AND ?", (vt - 16, vt)).fetchall()
    return [f for (f,) in rows]


def cmd_vptrs(b: Bin, arg: str, limit: int) -> None:
    start, _, _ = b.func(b.addr(arg))
    rows = b.db.execute("SELECT slot, vtable FROM fptr WHERE target=? ORDER BY slot LIMIT ?", (start, limit)).fetchall()
    total = b.db.execute("SELECT count(*) FROM fptr WHERE target=?", (start,)).fetchone()[0]
    print(f"{total} data slots point to {b.name(start)}")
    for slot, vt in rows:
        users = column_users(b, vt)
        who = ", ".join(b.name(f) for f in users[:3]) + (f" +{len(users) - 3}" if len(users) > 3 else "")
        print(f"  {b.rel(slot)}: column {b.rel(vt)} [{(slot - vt) // 8}]" + (f" <- {who}" if who else ""))


def cmd_vtable(b: Bin, arg: str, limit: int) -> None:
    a = b.addr(arg)
    row = b.db.execute("SELECT vtable FROM fptr WHERE slot<=? ORDER BY slot DESC LIMIT 1", (a,)).fetchone()
    if not row:
        sys.exit(f"no function pointer at or below {b.rel(a)}")
    vt = row[0]
    rows = b.db.execute("SELECT slot, target FROM fptr WHERE vtable=? ORDER BY slot", (vt,)).fetchall()
    if a > rows[-1][0] + 8:
        sys.exit(f"{b.rel(a)} is past the column at {b.rel(vt)}")
    users = column_users(b, vt)
    print(f"column {b.rel(vt)}: {len(rows)} slots, loaded by {len(users)} functions")
    for f in users[:5]:
        print(f"  <- {b.name(f)}")
    for slot, t in rows[:limit]:
        print(f"  [{(slot - vt) // 8}] {b.name(t)}")


COMMANDS = {
    "at": cmd_at,
    "fn": cmd_fn,
    "callers": cmd_callers,
    "callees": cmd_callees,
    "str": cmd_str,
    "sym": cmd_sym,
    "imports": cmd_imports,
    "refs": cmd_refs,
    "vptrs": cmd_vptrs,
    "vtable": cmd_vtable,
}


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0], usage=__doc__.split("Usage: ")[1])
    ap.add_argument("command", choices=COMMANDS)
    ap.add_argument("arg")
    ap.add_argument("--limit", type=int, default=20)
    ap.add_argument("--db", type=Path, default=DB)
    args = ap.parse_args()
    COMMANDS[args.command](Bin(args.db), args.arg, args.limit)


if __name__ == "__main__":
    main()
