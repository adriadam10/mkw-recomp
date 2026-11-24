"""Builds docs/i18n/README.<lang>.md from a translated body and the language's strings, or checks they are current.

Why: the README is read in the languages of most of the players who report here; a translation is written by hand once
(docs/i18n/<lang>.body.md) and this keeps the parts every language repeats in step: the centred title, the download
buttons, the link row and the language bar (the same as README.md's, its own language in bold), the body's relative
links moved two folders up, and a "translated from" line with the hash of the README.md it follows and of the body.
A rebuild keeps a language's README.md hash while its body is unchanged, so that adding a language or changing the
bar does not mark every old translation as current; an edited body takes the current one. check.py runs the
check and warns, without refusing the commit, when README.md changed after a translation (Adrià 13/11/2026).

Usage: uv run tools/docs/i18n_readme.py            (writes every docs/i18n/README.<lang>.md)
       uv run tools/docs/i18n_readme.py --check    (lists the translations older than README.md; exit 0 either way)
"""

import argparse
import hashlib
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
I18N = ROOT / "docs/i18n"
REPO = "https://github.com/adriadam10/mkw-recomp"
# (code, name in its own language, right to left)
LANGS = [("zh-CN", "简体中文", False), ("es", "Español", False), ("ar", "العربية", True), ("hi", "हिन्दी", False),
         ("pt-BR", "Português (Brasil)", False), ("pt-PT", "Português (Portugal)", False),
         ("de", "Deutsch", False), ("vi", "Tiếng Việt", False)]  # fmt: skip
# per language: tagline, download, latest for Windows / Linux (AppImage) / Linux ARM, and the link row's names
# fmt: off
STRINGS = {
    "zh-CN": ("《马力欧赛车 世界》（Nintendo Switch 2）的重编译版本，可在 Linux 和 Windows 上原生运行。", "下载",
              "Windows 最新版", "Linux 最新版 (AppImage)", "Linux ARM 最新版",
              ["发布", "帮助 Wiki", "Discord", "更新日志", "参与贡献", "来源", "路线图", "性能", "问题", "讨论", "存档"]),
    "es": ("Mario Kart World (Nintendo Switch 2) recompilado para funcionar de forma nativa en Linux y Windows.",
           "Descargar", "última para Windows", "última para Linux (AppImage)", "última para Linux ARM",
           ["Versiones", "Wiki de ayuda", "Discord", "Cambios", "Contribuir", "Fuentes", "Hoja de ruta", "Rendimiento", "Issues", "Discussions",
            "Archivo"]),
    "ar": ("ماريو كارت وورلد (Nintendo Switch 2) معاد تجميعها لتعمل أصليًا على Linux وWindows.", "تنزيل",
           "أحدث إصدار لـ Windows", "أحدث إصدار لـ Linux (AppImage)", "أحدث إصدار لـ Linux ARM",
           ["الإصدارات", "الويكي", "Discord", "سجل التغييرات", "المساهمة", "المصادر", "خارطة الطريق", "الأداء", "المشكلات", "النقاشات", "الأرشيف"]),
    "hi": ("Mario Kart World (Nintendo Switch 2), Linux और Windows पर नेटिव रूप से चलने के लिए रीकम्पाइल किया गया।",
           "डाउनलोड", "Windows के लिए नवीनतम", "Linux के लिए नवीनतम (AppImage)", "Linux ARM के लिए नवीनतम",
           ["रिलीज़", "विकी", "Discord", "बदलाव", "योगदान", "स्रोत", "रोडमैप", "प्रदर्शन", "इश्यू", "चर्चाएँ", "आर्काइव"]),
    "pt-BR": ("Mario Kart World (Nintendo Switch 2) recompilado para rodar nativamente no Linux e no Windows.",
              "Baixar", "mais recente para Windows", "mais recente para Linux (AppImage)",
              "mais recente para Linux ARM",
              ["Versões", "Wiki de ajuda", "Discord", "Mudanças", "Contribuir", "Fontes", "Roteiro", "Desempenho", "Issues", "Discussões",
               "Arquivo"]),
    "pt-PT": ("Mario Kart World (Nintendo Switch 2) recompilado para correr nativamente em Linux e Windows.",
              "Transferir", "mais recente para Windows", "mais recente para Linux (AppImage)",
              "mais recente para Linux ARM",
              ["Versões", "Wiki de ajuda", "Discord", "Alterações", "Contribuir", "Fontes", "Roteiro", "Desempenho", "Issues", "Discussões",
               "Arquivo"]),
    "de": ("Mario Kart World (Nintendo Switch 2), rekompiliert, um nativ unter Linux und Windows zu laufen.",
           "Download", "neueste für Windows", "neueste für Linux (AppImage)", "neueste für Linux ARM",
           ["Releases", "Hilfe-Wiki", "Discord", "Änderungen", "Mitwirken", "Quellen", "Roadmap", "Leistung", "Issues", "Discussions",
            "Archiv"]),
    "vi": ("Mario Kart World (Nintendo Switch 2) được biên dịch lại để chạy nguyên bản trên Linux và Windows.",
           "Tải xuống", "bản mới nhất cho Windows", "bản mới nhất cho Linux (AppImage)", "bản mới nhất cho Linux ARM",
           ["Bản phát hành", "Wiki trợ giúp", "Discord", "Nhật ký thay đổi", "Đóng góp", "Nguồn", "Lộ trình", "Hiệu năng", "Vấn đề", "Thảo luận",
            "Lưu trữ"]),
}
# fmt: on
LINKS = [
    f"{REPO}/releases",
    # Help for players: installation, settings, controls, known errors, FAQ (its source is the repository's wiki)
    f"{REPO}/wiki",
    # Permanent invite to the Discord server (#rules); declared with prevent_destroy in the server repo
    "https://discord.gg/8uYBkjQkEg",
    "../../CHANGELOG.md",
    "../../CONTRIBUTING.md",
    "../../docs/project/research.md#sources",
    "../../docs/project/roadmap.md",
    "../../docs/project/performance.md",
    f"{REPO}/issues",
    f"{REPO}/discussions",
    # Daily copy of the repository, issues, discussions and releases (tools/github/archive.py)
    "https://archive.org/search?query=subject%3A%22mkw-recomp%22",
]  # fmt: skip
# Scripts whose letters join (Arabic) or stack (Devanagari): their buttons stay in English (13/11/2026, seen on GitHub)
LATIN_BUTTONS = {"ar", "hi"}
SOURCE = re.compile(r"<!-- translated from README.md ([0-9a-f]{12})(?: body ([0-9a-f]{12}))? -->")
REL_MD = re.compile(r"(\]\()(?!https?:|#|mailto:|\.\./)([^)\s]+)")
REL_HTML = re.compile(r'((?:href|src)=")(?!https?:|#|mailto:|\.\./)([^"]+)')


def readme_hash() -> str:
    return hashlib.sha256((ROOT / "README.md").read_bytes()).hexdigest()[:12]


def source_stamp(code: str, body: str) -> str:
    """The "translated from" line: the README.md hash the built file already names while its body is unchanged (a
    file stamped before the body hash existed counts as unchanged), the current one for a new or edited body."""
    body_hash = hashlib.sha256(body.encode("utf-8")).hexdigest()[:12]
    built = I18N / f"README.{code}.md"
    m = SOURCE.search(built.read_text(encoding="utf-8")) if built.is_file() else None
    source = m.group(1) if m and m.group(2) in (None, body_hash) else readme_hash()
    return f"<!-- translated from README.md {source} body {body_hash} -->"


def badge(text: str) -> str:
    return text.replace("-", "--").replace("_", "__").replace(" ", "_")


def bar(code: str) -> str:
    items = ['<a href="../../README.md">English</a>']
    for c, name, _ in LANGS:
        items.append(f"<b>{name}</b>" if c == code else f'<a href="README.{c}.md">{name}</a>')
    return '<!-- i18n-bar:start -->\n<p align="center">\n  ' + " ·\n  ".join(items) + "\n</p>\n<!-- i18n-bar:end -->"


def build(code: str, rtl: bool) -> str:
    tag, dl, win, lin, arm, names = STRINGS[code]
    if code in LATIN_BUTTONS:  # shields.io's for-the-badge spaces and capitalises letters one by one: it breaks
        dl, win, lin, arm = "Download", "latest for Windows", "latest for Linux (AppImage)", "latest for Linux ARM"
    from urllib.parse import quote

    def button(what: str, color: str) -> str:
        url = "https://img.shields.io/badge/" + quote(badge("⬇ " + dl)) + "-" + quote(badge(what)) + f"-{color}"
        return (f'  <a href="{REPO}/releases/latest"><img src="{url}?style=for-the-badge&labelColor=1b2740" '
                f'alt="{dl}: {what}"></a>')  # fmt: skip

    links = " ·\n  ".join(f'<a href="{u}">{n}</a>' if i > 2 else f'<b><a href="{u}">{n}</a></b>'
                          for i, (u, n) in enumerate(zip(LINKS, names)))  # fmt: skip
    body = (I18N / f"{code}.body.md").read_text(encoding="utf-8").strip()
    stamp = source_stamp(code, body)
    body = REL_HTML.sub(r"\1../../\2", REL_MD.sub(r"\1../../\2", body))
    if rtl:
        body = f'<div dir="rtl">\n\n{body}\n\n</div>'
    head = "\n\n".join([
        stamp,
        '<p align="center"><img src="../images/logo.svg" width="128" alt="mkw-recomp logo"></p>',
        '<h1 align="center">mkw-recomp</h1>',
        f'<p align="center">\n  <b>{tag}</b>\n</p>',
        '<p align="center">\n' + "\n".join([button(win, "0078d6"), button(lin, "e95420"), button(arm, "8957e5")])
        + "\n</p>",
        f'<p align="center">\n  {links}\n</p>',
        bar(code),
    ])  # fmt: skip
    return head + "\n\n" + body + "\n"


def stale() -> list[str]:
    now = readme_hash()
    out = []
    for code, _, _ in LANGS:
        p = I18N / f"README.{code}.md"
        m = SOURCE.search(p.read_text(encoding="utf-8")) if p.is_file() else None
        if not m or m.group(1) != now:
            out.append(f"docs/i18n/README.{code}.md")
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()
    if args.check:
        for p in stale():
            print(
                f"note: {p} was translated from an older README.md: update docs/i18n/<lang>.body.md and run "
                "tools/docs/i18n_readme.py"
            )
        return 0
    for code, _, rtl in LANGS:
        (I18N / f"README.{code}.md").write_text(build(code, rtl), encoding="utf-8")
        print(f"docs/i18n/README.{code}.md")
    return 0


if __name__ == "__main__":
    sys.exit(main())
