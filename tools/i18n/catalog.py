# /// script
# requires-python = ">=3.12"
# ///
"""The text catalogs of the menu, the start screen, the setup page and the overlay as gettext files: i18n/ui/mkw.pot holds
the English (each entry's msgctxt is its UiTextId of runtime/gfx/nvn/host/ui_text.h, its msgid the English text) and
i18n/ui/<lang>.po each translation. build.py turns them into C (one const char *text_<lang>[UI_TEXT_COUNT] array per
language, in build/), so the ids stay checked by the compiler and nothing is read at run time.

Why .po and not C by hand: translators fix a text in Poedit, Weblate or Crowdin, or in any editor, without C quoting
or a build (Adrià 12/11/2026). An entry left empty or marked fuzzy shows the English text.

Usage:
  uv run tools/i18n/catalog.py update      after editing mkw.pot: every .po gets the new ids empty, loses the removed
                                           ones, and an entry whose English changed is marked fuzzy (its old English
                                           kept as "#| msgid") until a translator checks it
  uv run tools/i18n/catalog.py status      translated, fuzzy and empty entries per language
  uv run tools/i18n/catalog.py gen DIR     the C files build.py compiles (ui_text_<lang>.c)
"""

import argparse
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
I18N = ROOT / "i18n" / "ui"
POT = I18N / "mkw.pot"
# The catalog languages after English, as ui_text.c's rows name them (Switch language codes: ui_text_lang): the game's,
# then menu languages it does not ship
LANGS = ["ja", "fr", "de", "it", "es", "nl", "pt", "pt-BR", "ru", "zh-Hans", "zh-Hant", "ko"]
LANGS += ["es-419", "ca", "gl", "eu", "pl", "tr", "uk", "th"]
WRAP = 100


@dataclass
class Entry:
    ctx: str
    msgid: str
    msgstr: str = ""
    notes: list[str] = field(default_factory=list)  # "#." lines: for translators
    fuzzy: bool = False
    prev: str | None = None  # "#| msgid": the English a fuzzy translation was made from


def unquote(lines: list[str]) -> str:
    return "".join(re.fullmatch(r'\s*"(.*)"\s*', s, re.S)[1] for s in lines)  # type: ignore[index]


def parse(path: Path) -> tuple[str, list[Entry]]:
    """The header's msgstr and the entries of a .po or .pot (escapes kept as written: they are C's too)."""
    header, entries = "", []
    for block in re.split(r"\n\s*\n", path.read_text(encoding="utf-8").strip()):
        notes, fuzzy, prev, fields, key = [], False, [], {}, None
        for line in block.splitlines():
            if line.startswith("#."):
                notes.append(line[2:].strip())
            elif line.startswith("#,"):
                fuzzy = "fuzzy" in line
            elif line.startswith("#| msgid"):
                prev.append(line[len("#| msgid") :])
            elif line.startswith("#|"):
                prev.append(line[2:])
            elif line.startswith("#"):
                continue
            elif m := re.match(r"(msgctxt|msgid|msgstr)\s+(.*)", line):
                key = m[1]
                fields[key] = [m[2]]
            elif key:
                fields[key].append(line)
        if "msgid" not in fields:
            continue
        text = {k: unquote(v) for k, v in fields.items()}
        if not text["msgid"] and "msgctxt" not in text:
            header = text.get("msgstr", "")
            continue
        entries.append(
            Entry(text["msgctxt"], text["msgid"], text.get("msgstr", ""), notes, fuzzy, unquote(prev) if prev else None)
        )
    return header, entries


def quoted(key: str, text: str) -> list[str]:
    """key "text", wrapped at spaces into continuation lines as msgmerge does when it is long."""
    if len(text) + len(key) + 3 <= WRAP:
        return [f'{key} "{text}"']
    parts, cur = [], ""
    for word in re.findall(r"\S+\s*|\s+", text):
        if cur and len(cur) + len(word) > WRAP - 2:
            parts.append(cur)
            cur = ""
        cur += word
    return [f'{key} ""'] + [f'"{p}"' for p in parts + [cur] if p]


def write(path: Path, header: str, entries: list[Entry]) -> None:
    out = [*quoted("msgid", ""), *quoted("msgstr", header), ""]
    for e in entries:
        out += [f"#. {n}" for n in e.notes]
        if e.fuzzy:
            out.append("#, fuzzy")
        if e.prev is not None:
            out += ["#| " + s for s in quoted("msgid", e.prev)]
        out += quoted("msgctxt", e.ctx) + quoted("msgid", e.msgid) + quoted("msgstr", e.msgstr) + [""]
    path.write_text("\n".join(out), encoding="utf-8")


def po_path(lang: str) -> Path:
    return I18N / f"{lang}.po"


def header_for(lang: str) -> str:
    return f"Project-Id-Version: mkw-recomp\\nLanguage: {lang.replace('-', '_')}\\nMIME-Version: 1.0\\n" \
           "Content-Type: text/plain; charset=UTF-8\\nContent-Transfer-Encoding: 8bit\\n"  # fmt: skip


def update() -> None:
    _, pot = parse(POT)
    for lang in LANGS:
        path = po_path(lang)
        old = {e.ctx: e for e in parse(path)[1]} if path.exists() else {}
        merged = []
        for t in pot:
            o = old.get(t.ctx)
            e = Entry(t.ctx, t.msgid, notes=t.notes)
            if o and o.msgstr:
                e.msgstr, e.fuzzy, e.prev = o.msgstr, o.fuzzy, o.prev
                if o.msgid != t.msgid:  # the English changed: kept, but shown in English until checked
                    e.fuzzy, e.prev = True, o.prev if o.fuzzy and o.prev is not None else o.msgid
            merged.append(e)
        write(path, header_for(lang), merged)


def status() -> None:
    _, pot = parse(POT)
    for lang in LANGS:
        entries = {e.ctx: e for e in parse(po_path(lang))[1]} if po_path(lang).exists() else {}
        done = sum(1 for t in pot if (e := entries.get(t.ctx)) and e.msgstr and not e.fuzzy)
        fuzzy = sum(1 for t in pot if (e := entries.get(t.ctx)) and e.msgstr and e.fuzzy)
        print(f"{lang:8} {done:4}/{len(pot)} translated, {fuzzy} fuzzy, {len(pot) - done - fuzzy} empty")


def c_array(symbol: str, comment: str, texts: dict[str, str]) -> str:
    out = [f"// {comment} Generated by tools/i18n/catalog.py from i18n/; do not edit.", '#include "ui_text_cat.h"', ""]
    out.append(f"const char *const {symbol}[UI_TEXT_COUNT] = {{")
    out.append("    LANG_NAMES,")
    out += [f'    [{ctx}] = "{text}",' for ctx, text in texts.items()]
    out += ["};", ""]
    return "\n".join(out)


def symbol(lang: str) -> str:
    return "text_" + lang.lower().replace("-", "_")


def generate(out: Path) -> list[Path]:
    """ui_text_<lang>.c for English and every language, rewritten only when they change (no needless rebuild)."""
    _, pot = parse(POT)
    files = {"en": c_array("text_en", "English, the fallback of every language.", {t.ctx: t.msgid for t in pot})}
    english = {t.ctx: t.msgid for t in pot}
    for lang in LANGS:
        entries = parse(po_path(lang))[1] if po_path(lang).exists() else []
        texts = {e.ctx: e.msgstr for e in entries if e.msgstr and not e.fuzzy and english.get(e.ctx) == e.msgid}
        files[lang] = c_array(symbol(lang), f"{lang}.", texts)
    out.mkdir(parents=True, exist_ok=True)
    paths = []
    for lang, text in files.items():
        path = out / f"ui_text_{lang.lower().replace('-', '_')}.c"
        if not path.exists() or path.read_text(encoding="utf-8") != text:
            path.write_text(text, encoding="utf-8")
        paths.append(path)
    return paths


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("update")
    sub.add_parser("status")
    sub.add_parser("gen").add_argument("dir", type=Path)
    args = ap.parse_args()
    if args.cmd == "update":
        update()
    elif args.cmd == "status":
        status()
    else:
        for p in generate(args.dir):
            print(p)


if __name__ == "__main__":
    sys.exit(main())
