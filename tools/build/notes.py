"""A release's notes: that version's section of CHANGELOG.md (summary, changes, what was tested, extra known issues) put
into docs/project/release-template.md (install, updating, known issues, reporting: the same in every release).

Why: v0.1.0-v0.1.2 were written by hand, each with its own shape and the same install text copied and drifting. One
CHANGELOG.md, the common convention, keeps every version's changes in one file; GitHub's own generated notes list
merged pull requests, and this repository commits to main without them.

A version's section is "## X.Y.Z": a summary paragraph, "### Changes", "### Tested" and optionally "### Known issues"
(lines added under the template's). Each change names its issue (#N) when it has one.

Usage: uv run tools/build/notes.py --version 0.1.3 [--out FILE]   (default: stdout; gh release create --notes-file FILE)
       --changelog FILE reads another CHANGELOG.md (tools/git/pre-push: the tagged commit's, before the tag leaves)
"""

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SECTIONS = {"changes": "Changes", "tested": "Tested", "known": "Known issues"}


def version_section(changelog: str, version: str) -> str:
    parts = re.split(r"^## (\S+)\s*$", changelog, flags=re.M)
    for name, body in zip(parts[1::2], parts[2::2], strict=True):
        if name == version:
            return body
    sys.exit(f"CHANGELOG.md: no '## {version}' section")


def parse(text: str, version: str) -> dict[str, str]:
    """The summary and the subsections of a version's section."""
    parts = re.split(r"^### (.+)$", text.strip(), flags=re.M)
    got = {"summary": parts[0].strip()}
    titles = {v: k for k, v in SECTIONS.items()}
    for title, body in zip(parts[1::2], parts[2::2], strict=True):
        if title.strip() not in titles:
            sys.exit(
                f"CHANGELOG.md {version}: unknown '### {title.strip()}' (expected: {', '.join(SECTIONS.values())})"
            )
        got[titles[title.strip()]] = body.strip()
    for key in ("summary", "changes", "tested"):
        if not got.get(key):
            sys.exit(
                f"CHANGELOG.md {version}: no {'summary paragraph' if key == 'summary' else '### ' + SECTIONS[key]}"
            )
    if "PENDING" in got["tested"]:
        sys.exit(f"CHANGELOG.md {version}: ### Tested still says PENDING")
    return got


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--version", required=True, help="e.g. 0.1.3 (the tag without its v)")
    ap.add_argument("--out", type=Path, help="write here instead of stdout")
    ap.add_argument("--changelog", type=Path, default=ROOT / "CHANGELOG.md", help="default: the repository's")
    args = ap.parse_args()
    got = parse(version_section(args.changelog.read_text(encoding="utf-8"), args.version), args.version)
    template = (ROOT / "docs/project/release-template.md").read_text(encoding="utf-8")
    notes = template.format(
        summary=got["summary"], changes=got["changes"], tested=got["tested"], known=got.get("known", "")
    )
    notes = re.sub(r"\n{3,}", "\n\n", notes).strip() + "\n"
    if args.out:
        args.out.write_text(notes, encoding="utf-8")
    else:
        sys.stdout.write(notes)


if __name__ == "__main__":
    main()
