"""The pre-commit hook's checks read the repository's UTF-8 files whatever the machine's code page.

On Windows (code page 1252) tools/docs/check.py stopped on the Hindi link in README.md with a UnicodeDecodeError, so
the hook refused every commit there; tools/git/cfg_rows.py read runtime/core/base/cfg.c the same way.

Usage: uv run pytest tests/docs/test_hook_encoding.py
"""

import os
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
# A legacy code page everywhere: no UTF-8 mode, no locale coercion, and the C locale on POSIX (Windows ignores it)
LEGACY = {"PYTHONUTF8": "0", "PYTHONCOERCECLOCALE": "0", "LC_ALL": "C", "LANG": "C"}


@pytest.mark.parametrize("script", ["tools/docs/check.py", "tools/git/cfg_rows.py"])
def test_hook_check_reads_utf8_on_a_legacy_code_page(script: str) -> None:
    done = subprocess.run(
        [sys.executable, str(ROOT / script)], cwd=ROOT, env={**os.environ, **LEGACY},
        capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=120, check=False,
    )  # fmt: skip
    assert "UnicodeDecodeError" not in done.stderr, done.stderr[-600:]
