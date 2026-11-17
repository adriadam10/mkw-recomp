"""The system work list: MKW's SDK imports the runtime does not replace, by namespace, service-backed ones first.

Why: an import without a host replacement runs the SDK's own code. For pure library namespaces (nn::util, nn::os on
SVCs, std...) that is right. For a namespace whose code talks to a console service (account, hid, applet, time...)
it stops the run when it reaches an IPC session the host does not serve (it serves audren and sm only), one import at
a time. Static reachability does not find them: the SDK opens a session once and later calls go through a held proxy
object. So this lists them by namespace, from the service map below.

With --census (the file of a run with MKW_IMPORT_CENSUS, runtime/core/diag/impcensus.c) it also says which of them the
game called in that run and how often; SDK functions reached from main only through a function pointer are listed
apart.

Usage: uv run tools/analisis/runtime/sdk_gaps.py [--db build/bindb.sqlite] [--all] [--namespace nn::account]
       [--census build/run/NAME.imports]
"""

import argparse
import re
import sqlite3
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
OVERRIDE = re.compile(r'HOST_(?:OVERRIDE|OBSERVER)\("([^"]+)"')
# Namespaces whose SDK code ends in a console service session (service names from main.npdm).
SERVICE_NS = {
    "nn::account": "acc:u0", "nn::hid": "hid", "nn::oe": "appletOE", "nn::am": "appletOE", "nn::friends": "friend:u",
    "nn::nifm": "nifm:u", "nn::time": "time:u", "nn::prepo": "prepo:u", "nn::ldn": "ldn:u", "nn::nfp": "nfp:user",
    "nn::nfc": "nfc:user", "nn::bcat": "bcat:u", "nn::pctl": "pctl", "nn::aoc": "aoc:u", "nn::socket": "bsd:u",
    "nn::ssl": "ssl", "nn::err": "ectx:aw", "nn::fs": "fsp-srv", "nn::codec": "hwopus", "nn::pl": "pl:u",
    "nn::mii": "mii:u", "nn::album": "caps:u", "nn::capsrv": "caps:su", "nn::nsd": "nsd:u", "nn::ngc": "ngc:u",
    "nn::notification": "notif:a", "nn::settings": "set", "nn::apm": "apm", "nn::irsensor": "irs", "nn::lm": "lm",
    "nn::swkbd": "appletOE", "nn::web": "appletOE", "nn::err::detail": "ectx:aw", "nn::ec": "nim:eca",
    "nn::pia": "bsd:u", "nn::movie": "hwopus", "nn::audio": "aud:u",
}  # fmt: skip


def overrides() -> set[str]:
    names = set()
    for src in (ROOT / "runtime").rglob("*.c"):
        names.update(OVERRIDE.findall(src.read_text(errors="replace")))
    return names


def namespace(demangled: str) -> str:
    m = re.match(r"(?:\w[\w:<>, *&]* )?((?:nn::)?\w+)", demangled.replace("non-virtual thunk to ", ""))
    return m.group(1) if m else demangled


def read_census(path: Path) -> dict[str, tuple[int, int]]:
    """symbol -> (calls, first frame) of the functions entered at their start (offset +0)."""
    out = {}
    for line in path.read_text().splitlines():
        if line.startswith("#"):
            continue
        _mod, _off, calls, first, sym = line.split("\t")
        name, _, off = sym.rpartition("+")
        if name and int(off, 16) == 0:
            out[name] = (int(calls), int(first))
    return out


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--db", type=Path, default=ROOT / "build/bindb.sqlite")
    ap.add_argument("--all", action="store_true", help="also list namespaces with no service behind them")
    ap.add_argument("--namespace", help="print the imports of one namespace")
    ap.add_argument("--census", type=Path, help="MKW_IMPORT_CENSUS file of a run")
    args = ap.parse_args()
    db = sqlite3.connect(args.db)
    replaced = overrides()
    census = read_census(args.census) if args.census else {}
    gaps: dict[str, list[tuple[str, str]]] = defaultdict(list)
    imports = set()
    total = 0
    for name, dem in db.execute("SELECT name, demangled FROM sym WHERE module='main' AND kind='import'"):
        total += 1
        imports.add(name)
        if name not in replaced:
            gaps[namespace(dem)].append((name, dem))
    covered = total - sum(len(v) for v in gaps.values())
    if args.namespace:
        rows = sorted(gaps.get(args.namespace, []), key=lambda r: (-census.get(r[0], (0, 0))[0], r[1]))
        for name, dem in rows:
            calls, first = census.get(name, (0, 0))
            print(f"{calls:>10} {first:>6}  {dem}" if args.census else dem)
        return
    print(f"main imports {total}, replaced by the host {covered}")
    if args.census:
        called = sum(1 for items in gaps.values() for n, _ in items if n in census)
        print(f"called in the run without replacement: {called} (calls / first frame per namespace below)")
    head = "imports without replacement" + (": called, calls" if args.census else "")
    print(f"{'namespace':<18} {'service':<10} {head}")
    for ns, items in sorted(gaps.items(), key=lambda kv: (kv[0] not in SERVICE_NS, -len(kv[1]))):
        if ns in SERVICE_NS or args.all:
            hit = [census[n] for n, _ in items if n in census]
            extra = f": {len(hit)}, {sum(c for c, _ in hit)}" if args.census else ""
            print(f"{ns:<18} {SERVICE_NS.get(ns, '-'):<10} {len(items)}{extra}")
    if args.census:
        others = sorted((c, n) for n, (c, _) in census.items() if n not in imports)
        print(f"SDK functions entered from main that are not imports (function pointers): {len(others)}")
        for c, n in reversed(others[-15:]):
            print(f"  {c:>10}  {n}")


if __name__ == "__main__":
    main()
