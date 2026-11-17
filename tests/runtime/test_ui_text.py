"""The text catalogs of the menu, the start screen, the setup page and the overlay (i18n/ui/mkw.pot and i18n/ui/<lang>.po,
compiled by tools/i18n/catalog.py): the English template has exactly the ids of ui_text.h, every .po has the same
entries as the template with the same English, and each translation has the same printf conversions in the same
order. A translation that turned "%s ... %d" into "%d ... %s" would make printf read its arguments wrong.

Usage: uv run pytest tests
"""

import re
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/i18n"))
import catalog  # noqa: E402

CONV = re.compile(r"%[-+ #0]*\d*(?:\.\d+)?[a-zA-Z%]")
SWKBD_MAX = 9  # the software keyboard's DEFAULT_NAME, in UTF-16 units


def enum_ids() -> set[str]:
    header = (ROOT / "runtime/gfx/nvn/host/ui_text.h").read_text(encoding="utf-8")
    body = re.sub(r"//.*", "", header[header.index("typedef enum") : header.index("UI_TEXT_COUNT")])
    return set(re.findall(r"\b(UI_(?!MENU_FIRST\b|MENU_LAST_ID\b|MENU_LANGNAME_)[A-Z0-9_]+)\b", body))


def test_template_matches_the_enum() -> None:
    _, pot = catalog.parse(catalog.POT)
    ids = [e.ctx for e in pot]
    assert len(ids) == len(set(ids)), "an id twice"
    assert set(ids) == enum_ids()


@pytest.mark.parametrize("lang", catalog.LANGS)
def test_translation(lang: str) -> None:
    path = catalog.po_path(lang)
    if not path.exists():
        pytest.skip("no translation yet")
    english = {e.ctx: e.msgid for e in catalog.parse(catalog.POT)[1]}
    entries = catalog.parse(path)[1]
    assert {e.ctx: e.msgid for e in entries} == english, "out of date: uv run tools/i18n/catalog.py update"
    wrong = {e.ctx: e.msgstr for e in entries if e.msgstr and CONV.findall(e.msgid) != CONV.findall(e.msgstr)}
    assert wrong == {}, "printf conversions differ from the English"
    name = next((e.msgstr for e in entries if e.ctx == "UI_SWKBD_DEFAULT_NAME"), "")
    assert len(name.encode("utf-16-le")) // 2 <= SWKBD_MAX and not set(name) & set("@%\\")
