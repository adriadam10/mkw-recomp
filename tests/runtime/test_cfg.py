"""Every setting the in-game menu writes to mkw.ini is a key the runtime reads back (runtime/core/base/cfg.c).

Why: HDR was in the menu's table but not in the list of known keys, so `HDR = off` written by the menu was dropped at
the next start with "unknown key 'HDR'" (#168, 13/11/2026).

Usage: uv run pytest tests/runtime/test_cfg.py
"""

import re
from pathlib import Path

SRC = (Path(__file__).resolve().parents[2] / "runtime/core/base/cfg.c").read_text()


def test_menu_settings_are_known_keys() -> None:
    known = set(
        re.findall(r'"([A-Z0-9_]+)"', SRC[SRC.index("known[] = {") : SRC.index("};", SRC.index("known[] = {"))])
    )
    menu = set(re.findall(r'\{"([A-Z0-9_]+)", "[^"]*", "[^"]*", (?:LIVE|RESTART)', SRC))
    assert menu, "the menu's settings table was not found"
    assert not menu - known, f"menu settings missing from known[]: {sorted(menu - known)}"
