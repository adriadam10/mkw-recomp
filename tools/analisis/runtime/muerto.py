"""Dead-code sweeps of the runtime: what came with the totk-pc-recomp copy and MKW never runs or never reads.

Why: the runtime is totk-pc-recomp's, adapted. Four sweeps found ~5,600 dead lines
on 05/11/2026; this repeats them after more code is brought over. Each prints a table; nothing is changed.
- funcs: global functions of build/obj/rt that no object of the binary asks for (nm: defined T, never U) and whose
  name appears once in the runtime's .c/.cpp/.inc (its definition).
- keys: names of cfg.c's known[] list that no call outside the probes takes as a literal ("MKW_X", "X" or
  PREFIX "X"), writers and descriptions aside (opt_*, row_*, cfg_set, cfg_meta, plat_setenv), that no file but cfg.c
  and the menu names as "MKW_X" (helpers such as portable_path take the name), nor a "BIND_%s"-style
  name built from a prefix, nor the generated patches.h's switches: a menu option among them does nothing. A menu row
  that reads its own key to show it counts as a read (not caught).
- ifdefs: #if/#ifdef/#ifndef in the runtime on a macro tools/build/build.py fixes (-D flags, #define in its texts).
- files: runtime .c/.cpp that build.py compiles for neither target.
- texts: ui_text.h ids no file but the catalog and the probes names (a text nobody shows); range markers
  (UI_MENU_FIRST...) and families read by offset from their first member (UI_BIND_A + action) are left out.
Static functions and variables come from the compiler's -Wunused-* warnings instead (the build prints them).

Usage: uv run tools/analisis/runtime/muerto.py [funcs|keys|ifdefs|files|texts]...   (all without arguments; funcs needs a
build, keys its build/generated/patches.h, or it lists the patch switches too)
"""

import importlib.util
import re
import subprocess
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
RT = ROOT / "runtime"
OBJ = ROOT / "build/obj"
SOURCES = [p for ext in ("*.c", "*.cpp", "*.inc") for p in RT.rglob(ext) if "third_party" not in p.parts]


def text_of(paths: list[Path]) -> str:
    return "\n".join(p.read_text(errors="replace") for p in paths)


def nm(args: list[str]) -> str:
    return subprocess.run(["nm", *args], capture_output=True, text=True, check=False).stdout


def funcs() -> None:
    objs = sorted(str(p) for p in (OBJ / "rt").rglob("*.o"))
    if not objs:
        sys.exit("muerto: no build/obj/rt objects (build first: tools/build/build.py)")
    defined: dict[str, str] = {}
    for line in nm(["-A", "--defined-only", *objs]).splitlines():
        f = line.split()
        if len(f) == 3 and f[1] == "T":
            defined[f[2]] = f[0].split(":")[0]
    libs = [str(p) for p in OBJ.glob("*.a")]
    wanted = {f[1] for f in (line.split() for line in nm(["-u", *objs, *libs]).splitlines()) if len(f) == 2}
    words = Counter(re.findall(r"\w+", text_of(SOURCES)))
    print("| Function | Object |\n|---|---|")
    for name in sorted(set(defined) - wanted):
        if words[name] == 1:
            print(f"| `{name}` | `{Path(defined[name]).relative_to(OBJ / 'rt')}` |")


def keys() -> None:
    cfg = (RT / "core/base/cfg.c").read_text()
    known = re.search(r"known\[\] = \{(.*?)\};", cfg, re.DOTALL)
    if not known:
        sys.exit("muerto: no known[] list in runtime/core/base/cfg.c")
    src = text_of([p for p in SOURCES if "probes" not in p.parts])
    calls = re.findall(r'\b(\w+)\(\s*(?:PREFIX\s*)?"(?:MKW_)?(\w+)"', src)
    read = {key for fn, key in calls if not re.match(r"opt_|row_|cfg_set$|cfg_meta$|plat_setenv$", fn)}
    rest = [p for p in SOURCES if "probes" not in p.parts and p.name not in ("cfg.c", "menu.cpp")]
    read |= set(re.findall(r'"MKW_(\w+)"', text_of(rest)))  # a name handed to a helper (portable_path, env_u...)
    prefixes = tuple(re.findall(r'"(?:MKW_)?([A-Z0-9_]+_)%s', src))
    patches = ROOT / "build/generated/patches.h"
    switches = set(re.findall(r'"MKW_(\w+)"', patches.read_text())) if patches.exists() else set()
    in_menu = (RT / "ui/menu/menu.cpp").read_text()
    print("| Key | In the menu |\n|---|---|")
    for key in re.findall(r'"([A-Z0-9_]+)"', known.group(1)):
        if key not in read and key not in switches and not key.startswith(prefixes):
            print(f"| `MKW_{key}` | {'yes' if f'MKW_{key}' in in_menu else 'no'} |")


def build_module():
    spec = importlib.util.spec_from_file_location("build", ROOT / "tools/build/build.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def ifdefs() -> None:
    text = (ROOT / "tools/build/build.py").read_text()
    fixed = set(re.findall(r'"-D(\w+)', text)) | set(re.findall(r"#define (\w+)", text))
    print("| Macro | Where |\n|---|---|")
    for p in SOURCES + list(RT.rglob("*.h")):
        if "third_party" in p.parts:
            continue
        for n, line in enumerate(p.read_text(errors="replace").splitlines(), 1):
            m = re.match(r"\s*#\s*(?:if|ifdef|ifndef|elif)\b(.*)", line)
            hit = m and fixed & set(re.findall(r"\w+", m.group(1)))
            if hit:
                print(f"| `{', '.join(sorted(hit))}` | `{p.relative_to(ROOT)}:{n}` |")


def files() -> None:
    build = build_module()
    compiled = set(build.runtime_sources(build.LINUX)) | set(build.runtime_sources(build.WINDOWS))
    print("| Fichero |\n|---|")
    for p in sorted(set(RT.rglob("*.c")) | set(RT.rglob("*.cpp"))):
        if p not in compiled:
            print(f"| `{p.relative_to(ROOT)}` |")


def texts() -> None:
    ids = re.findall(r"\b(UI_[A-Z0-9_]+)\b", (RT / "gfx/nvn/host/ui_text.h").read_text())
    others = text_of(
        [p for p in SOURCES + list(RT.rglob("*.h")) if not p.name.startswith("ui_text.") and "probes" not in p.parts]
    )
    used = Counter(re.findall(r"\bUI_[A-Z0-9_]+\b", others))
    by_offset = {m.rsplit("_", 1)[0] + "_" for m in re.findall(r"\b(UI_[A-Z0-9_]+)\s*\+", others)}
    print("| Texto |\n|---|")
    for name in dict.fromkeys(ids):
        marker = name.endswith(("_FIRST", "_LAST", "_LAST_ID", "_COUNT"))
        if not used[name] and not marker and not name.startswith(tuple(by_offset)):
            print(f"| `{name}` |")


SWEEPS = {"funcs": funcs, "keys": keys, "ifdefs": ifdefs, "files": files, "texts": texts}

if __name__ == "__main__":
    asked = sys.argv[1:] or list(SWEEPS)
    for name in asked:
        if name not in SWEEPS:
            sys.exit(__doc__)
        print(f"\n## {name}\n")
        SWEEPS[name]()
