#!/usr/bin/env python3
"""Every name the runtime passes as a literal to cfg_value, cfg_int, cfg_double or cfg_bool has its row in the settings
table (runtime/core/base/cfg.c metas[]): without one those functions stop the game (rt_fatal), and only when the line runs.
A diagnostic knob reads cfg_get instead. 06/11/2026: MKW_STANDIN_MAGENTA through cfg_bool stopped every run without it.
Run by tools/git/pre-commit; exit 1 with the offending names.

Usage: uv run tools/git/cfg_rows.py
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def main() -> None:
    cfg = (ROOT / "runtime/core/base/cfg.c").read_text(encoding="utf-8")
    table = cfg[cfg.index("static const CfgMeta metas[]") :]
    table = table[: table.index("};")]
    rows = set(re.findall(r'\{"([A-Z0-9_]+)"', table))
    bad = []
    for path in sorted((ROOT / "runtime").rglob("*.[ch]*")):
        if "third_party" in path.parts:
            continue
        for n, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
            for name in re.findall(r'\bcfg_(?:value|int|double|bool)\("MKW_([A-Z0-9_]+)"\)', line):
                # cfg_value stops only when the name is unset; a line that checks cfg_get first is safe
                if name not in rows and f'cfg_get("MKW_{name}")' not in line:
                    bad.append(f"{path.relative_to(ROOT)}:{n}: MKW_{name}")
    if bad:
        print("cfg_rows: names read with cfg_value/int/double/bool that have no row in runtime/core/base/cfg.c "
              "(rt_fatal at run time; a diagnostic knob reads cfg_get):", file=sys.stderr)
        print("\n".join(bad), file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
