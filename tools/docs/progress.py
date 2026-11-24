"""How much of MKW's libraries and shader instructions is done: the progress bars of the root README (cog block).

Why: AnyPS5 shows the share of system library functions and GPU shader instructions it implements on top of its
README; the same was wanted here (05/11/2026). Three measures, each "done" counted in the code at every commit:
- NVN2: of the 697 functions of the driver's proc table, those the runtime defines (NVN_API, or NVN2_IGNORED for the
  optional features that are off) or runs as the SDK's own code (nvn2_use_sdk_proc); the rest stop with their name. Also: of those the game reads (nvn_usage.py).
- SDK: of the imports of main, those replaced by name (HOST_OVERRIDE) plus those that are pure library code and run
  right as the SDK's recompiled code (tools/analisis/runtime/sdk_class.py); apart, those on the runtime's kernel and those
  that go to a console service without a replacement, with how many of these the game called in a race
  (MKW_IMPORT_CENSUS). Counting only the replaced ones read as 1 100 missing functions.
- Shaders: of the SM86 opcodes in MKW's programs, those translated in every form they take; an opcode with a form the
  translator rejects (build/spirv/failed.tsv) is partial.
What comes from the game and its measurements (names, imports, census, failures) is kept in tools/docs/progress.json,
because a checkout without build/ has none of it; --update refreshes it from build/.

Usage: uv run tools/docs/progress.py --update --census build/run/NAME.imports   (needs build/bindb.sqlite with its
       svc table, build/nvn_usage.csv, build/shader_census, build/spirv and build/sm86/instructions.json; the census
       is the file of a race run with MKW_IMPORT_CENSUS)
       uv run cog -r -I tools/docs README.md      regenerates the bars; tools/docs/check.py checks them
"""

import argparse
import csv
import json
import re
import sqlite3
import sys
import time
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/analisis/runtime"))
DATA = Path(__file__).resolve().parent / "progress.json"
BUILD = ROOT / "build"
WIDTH = 20  # characters of a bar
NVN_DEF = re.compile(r"^\s*NVN(?:_API|2_IGNORED)\((nvn\w+)\)", re.MULTILINE)
OVERRIDE = re.compile(r'HOST_OVERRIDE\(\s*"(\w+)"')
# nvn2_use_sdk_proc (runtime/gfx/nvn/procs/nvn2.c): procs whose driver function runs as the SDK's own recompiled code.
SDK_PROC = re.compile(r'nvn2_use_sdk_proc\(const char \*name\) \{ return !strncmp\(name, "(\w+)"')


def runtime_source() -> str:
    return "\n".join(
        p.read_text(encoding="utf-8") for p in sorted((ROOT / "runtime").rglob("*.c")) if "third_party" not in p.parts
    )


# ---- --update: the measured lists, from build/
def measure(census: Path) -> dict:
    from sdk_class import classify
    from sdk_gaps import read_census

    nvn2 = {r["name"]: int(r["functions"]) for r in csv.DictReader((BUILD / "nvn_usage.csv").open())}
    db = sqlite3.connect(BUILD / "bindb.sqlite")
    imports = sorted(db.execute("SELECT name, demangled FROM sym WHERE module = 'main' AND kind = 'import'"))
    called = read_census(census)
    sdk_cls = classify(db, set(called))
    opcodes = {r["opcode"]: int(r["count"]) for r in csv.DictReader((BUILD / "shader_census/opcodes.csv").open())}
    spec = json.loads((BUILD / "sm86/instructions.json").read_text())["instructions"]
    failing: dict[str, list[str]] = {}
    failed_programs = set()
    for line in (BUILD / "spirv/failed.tsv").read_text().splitlines():
        reason, programs = line.split("\t", 1)
        failed_programs |= set(programs.split())
        cls = reason.removeprefix("unsupported: ").split(":")[0].split()[0]
        op = spec[cls]["mnemonic"][-1] if cls in spec else cls.upper()
        failing.setdefault(op, []).append(reason.removeprefix("unsupported: "))
    ok = sum(1 for p in (BUILD / "spirv").glob("*.result") if p.read_text().strip() == "ok")
    return {
        "date": time.strftime("%d/%m/%Y"),
        "nvn2": nvn2,
        "imports": [[name, namespace(demangled), sdk_cls[name], name in called] for name, demangled in imports],
        "census": census.name,
        "opcodes": opcodes,
        "failing": failing,
        "programs": {"ok": ok, "total": ok + len(failed_programs)},
    }


# ---- the README block
def num(n: int) -> str:
    """1289 -> "1 289", as the docs write numbers."""
    return f"{n:,}".replace(",", " ")


def bar(done: int, total: int) -> str:
    n = round(WIDTH * done / total) if total else 0
    return f"`{'█' * n}{'░' * (WIDTH - n)}` {100 * done / total:.0f} %" if total else "—"


def stacked(parts: list[int], total: int) -> str:
    """A bar of several parts, each with its own fill (done █, kernel ▒, missing ░); the % is the first part."""
    cells, acc = [], 0
    for n, ch in zip(parts, "█▒░", strict=False):
        end = round(WIDTH * (acc + n) / total)
        cells.append(ch * (end - round(WIDTH * acc / total)))
        acc += n
    return f"`{''.join(cells)}` {100 * parts[0] / total:.0f} %"


def sdk_state(imp: list, overrides: set[str]) -> str:
    """ "replaced" | "pure" | "kernel" | "service" for one row of progress.json's imports."""
    return "replaced" if imp[0] in overrides else imp[2]


def sdk_table(imports: list, overrides: set[str], limit: int = 25) -> list[str]:
    rows: dict[str, Counter] = {}
    for imp in imports:
        c = rows.setdefault(imp[1], Counter())
        c[sdk_state(imp, overrides)] += 1
        c["called"] += sdk_state(imp, overrides) == "service" and imp[3]
    out = [
        "<details><summary>SDK by namespace</summary>",
        "",
        "| Group | Done | Replaced | Library | On the kernel | To a service (called) |",
        "|---|---|---|---|---|---|",
    ]
    ordered = sorted(rows.items(), key=lambda kv: (-kv[1].total() + kv[1]["called"], kv[0]))

    def line(name: str, c: Counter) -> str:
        done, total = c["replaced"] + c["pure"], c.total() - c["called"]
        svc = f"{num(c['service'])} ({num(c['called'])})" if c["service"] else ""
        cells = [num(c[k]) if c[k] else "" for k in ("replaced", "pure", "kernel")]
        return f"| {name} | {stacked([done, c['kernel'], c['service']], total)} | {' | '.join(cells)} | {svc} |"

    out += [line(n, c) for n, c in ordered[:limit]]
    if len(ordered) > limit:
        rest = sum((c for _, c in ordered[limit:]), Counter())
        out.append(line(f"rest ({len(ordered) - limit} groups)", rest))
    return [*out, "", "</details>", ""]


def nvn_object(name: str, objects: list[str]) -> str:
    """The NVN object a function belongs to: the longest object name it starts with ("nvnTextureBuilderSetSize" ->
    TextureBuilder); objects are those with an Initialize or SetDefaults function."""
    rest = name.removeprefix("nvn")
    return next((o for o in objects if rest.startswith(o)), "others")


def namespace(demangled: str) -> str:
    m = re.match(r"(?:[\w:<>]+ )?((?:nn|nv|nvn)::\w+)", demangled)
    return m.group(1) if m else "C and C++ (libc, cxa)"


def group_table(title: str, rows: dict[str, list[int]], limit: int = 25) -> list[str]:
    out = [f"<details><summary>{title}</summary>", "", "| Group | Done | |", "|---|---|---|"]
    ordered = sorted(rows.items(), key=lambda kv: (-kv[1][1], kv[0]))
    for name, (done, total) in ordered[:limit]:
        out.append(f"| {name} | {bar(done, total)} | {num(done)} of {num(total)} |")
    if len(ordered) > limit:
        done, total = (sum(v[i] for _, v in ordered[limit:]) for i in (0, 1))
        out.append(f"| rest ({len(ordered) - limit} groups) | {bar(done, total)} | {num(done)} of {num(total)} |")
    return [*out, "", "</details>", ""]


def emit(cog) -> None:
    data = json.loads(DATA.read_text(encoding="utf-8"))
    src = runtime_source()
    impl = set(NVN_DEF.findall(src)) | {"nvnDeviceGetProcAddress"}  # (nvn_procs.c defines that one by hand)
    overrides = set(OVERRIDE.findall(src))

    nvn2 = data["nvn2"]
    sdk_procs = SDK_PROC.findall(src)
    impl |= {n for n in nvn2 for p in sdk_procs if n.startswith(p)}
    nvn_done = [n for n in nvn2 if n in impl]
    used = [n for n, k in nvn2.items() if k > 0]
    used_done = [n for n in used if n in impl]
    objects = sorted(
        {n[3 : -len(s)] for n in nvn2 for s in ("Initialize", "SetDefaults") if n.endswith(s)}, key=len, reverse=True
    )
    by_object: dict[str, list[int]] = {}
    for n in nvn2:
        g = by_object.setdefault(nvn_object(n, objects), [0, 0])
        g[0] += n in impl
        g[1] += 1

    imports = data["imports"]
    sdk = Counter(sdk_state(imp, overrides) for imp in imports)
    sdk_called = sum(1 for imp in imports if sdk_state(imp, overrides) == "service" and imp[3])

    ops = set(data["opcodes"]) | set(data["failing"])
    partial = sorted(set(data["failing"]))
    ops_done = len(ops) - len(partial)
    progs = data["programs"]

    lines = [
        "| | Done | |",
        "|---|---|---|",
        (
            f"| NVN2 (graphics API) | {bar(len(nvn_done), len(nvn2))} | {len(nvn_done)} of {len(nvn2)} functions; of "
            f"the {len(used)} the game reads, {len(used_done)} |"
        ),
        (
            f"| SDK `nn::*` | {stacked([sdk['replaced'] + sdk['pure'], sdk['kernel'], sdk['service']], len(imports))} | "
            f"of the {num(len(imports))} imports of `main`: {sdk['replaced']} replaced by name and "
            f"{sdk['pure']} library code that runs as is (█); {sdk['kernel']} on the runtime's kernel (▒); "
            f"{sdk['service']} go to a console service with no replacement (░), {sdk_called} of them called in "
            "a race |"
        ),
        (
            f"| SM86 opcodes | {bar(ops_done, len(ops))} | {ops_done} of the {len(ops)} MKW uses, translated in every "
            f"form; {num(progs['ok'])} of {num(progs['total'])} programs to SPIR-V |"
        ),
        "",
        (
            f"What is done is counted in the code at every commit; the game's totals are the measurements of "
            f"{data['date']} (`tools/docs/progress.py`)."
        ),
        "",
        *group_table("NVN2 by object", by_object),
        *sdk_table(imports, overrides),
        "<details><summary>Opcodes with forms not translated</summary>",
        "",
        "| Opcode | Instructions in MKW | Failing forms |",
        "|---|---|---|",
        *(
            f"| {op} | {num(data['opcodes'][op]) if op in data['opcodes'] else 'outside the nvdisasm census'} | {'; '.join(f'`{r}`' for r in data['failing'][op])} |"
            for op in partial
        ),
        "",
        "</details>",
    ]
    cog.outl("\n".join(lines))


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--update", action="store_true", help="refresh tools/docs/progress.json from build/")
    ap.add_argument("--census", type=Path, help="MKW_IMPORT_CENSUS file of a race run (needed by --update)")
    args = ap.parse_args()
    if not args.update or not args.census:
        ap.error("nothing to do: --update --census FILE refreshes the measurements; cog renders the README block")
    data = measure(args.census)
    DATA.write_text(json.dumps(data, indent=1, ensure_ascii=False, sort_keys=True) + "\n", encoding="utf-8", newline="\n")
    counts = Counter({k: len(v) for k, v in data.items() if isinstance(v, (list, dict))})
    print(f"{DATA.relative_to(ROOT)}: {dict(counts)}")


if __name__ == "__main__":
    main()
