"""The moderation bots read gh's JSON as UTF-8 whatever the machine's code page.
Why: `gh api` writes UTF-8, but subprocess decoded it with the locale's encoding, so a dry run by hand on Windows (code
page 1252) stopped with a UnicodeDecodeError on the first issue or discussion with a letter like "č" in it (the CI runs
on Linux in UTF-8 and never saw it). A stand-in for subprocess.run decodes the bytes the way the call asks, under a
legacy code page (no UTF-8 mode, the C locale on POSIX).

Usage: uv run pytest tests/github/test_gh_encoding.py
"""

import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
LEGACY = {"PYTHONUTF8": "0", "PYTHONCOERCECLOCALE": "0", "LC_ALL": "C", "LANG": "C"}
DRIVER = r"""
import json, locale, subprocess, sys
sys.path.insert(0, sys.argv[1] + "/tools/github")
import moderate, moderate_discussions
RAW = json.dumps([[{"title": "Crash \u010d \u0141 \u00e9"}]], ensure_ascii=False).encode("utf-8")
GRAPHQL = json.dumps({"data": {"title": "\u010d"}}, ensure_ascii=False).encode("utf-8")
def fake(args, **kw):
    raw = GRAPHQL if "graphql" in args else RAW
    text = raw.decode(kw.get("encoding") or locale.getpreferredencoding(False))  # what subprocess does with text=True
    return subprocess.CompletedProcess(args, 0, stdout=text, stderr="")
subprocess.run = fake
print(ascii(moderate.GitHub("o/r", False).get("issues")[0]["title"]))
print(ascii(moderate_discussions.GitHub("o/r", False).query("q")["title"]))
"""


def test_gh_json_is_read_as_utf8_on_a_legacy_code_page() -> None:
    done = subprocess.run(
        [sys.executable, "-c", DRIVER, str(ROOT)], env={**os.environ, **LEGACY},
        capture_output=True, encoding="utf-8", errors="replace", timeout=60, check=False,
    )  # fmt: skip
    assert done.returncode == 0, done.stderr[-600:]
    assert done.stdout.splitlines() == [ascii("Crash \u010d \u0141 \u00e9"), ascii("\u010d")]
