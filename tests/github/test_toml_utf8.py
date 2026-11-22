"""The bot's signatures.toml and topics.toml are read as UTF-8 whatever the machine's code page.
Why: Path.read_text() used the locale's encoding, so a run by hand on Windows (code page 1252) turned the "…" of a
signature and the "–" and "→" of an FAQ reply into "â€¦", "â€"" and "â†'", and `--apply` posted that in the reply.
The CI runs on Linux in UTF-8 and never saw it. The loaders run in a child process under a legacy code page.

Usage: uv run pytest tests/github/test_toml_utf8.py
"""

import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
LEGACY = {"PYTHONUTF8": "0", "PYTHONCOERCECLOCALE": "0", "LC_ALL": "C", "LANG": "C"}
DRIVER = r"""
import sys
sys.path.insert(0, sys.argv[1] + "/tools/github")
import classify, discuss
sigs = [s.what for s in classify.load_signatures()]
replies = [t.reply for t in discuss.load_topics()]
print(any("…" in w for w in sigs), any("–" in r for r in replies), any("â€" in r for r in replies + sigs))
"""


def test_toml_files_are_read_as_utf8_on_a_legacy_code_page() -> None:
    done = subprocess.run(
        [sys.executable, "-c", DRIVER, str(ROOT)], env={**os.environ, **LEGACY},
        capture_output=True, encoding="utf-8", errors="replace", timeout=60, check=False,
    )  # fmt: skip
    assert done.returncode == 0, done.stderr[-600:]
    assert done.stdout.split() == ["True", "True", "False"]
