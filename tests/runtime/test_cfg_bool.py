"""cfg_bool (runtime/core/base/cfg.c) reads the words a player writes for a yes/no setting, not only "0" and "1".
Why: every yes/no row is "0|1", and only "0" was false: MKW_DISCORD=off switched Rich Presence on, MKW_LAN=off the LAN
sockets, MKW_UPDATE_CHECK=off the update check. The production function is compiled with stubs for the table lookups,
without the game or a GPU.
Usage: uv run pytest tests/runtime/test_cfg_bool.py
"""

import shutil
import subprocess
from pathlib import Path

import pytest

SOURCE = Path(__file__).resolve().parents[2] / "runtime/core/base/cfg.c"
HARNESS = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _MSC_VER
#define strcasecmp _stricmp
#else
#include <strings.h>
#endif
typedef struct { const char *key, *def; } CfgMeta;
static const char *value, *def;
static const char *cfg_value(const char *name) { (void)name; return value; }
static const CfgMeta *cfg_meta(const char *name) {
    static CfgMeta m;
    m.key = name, m.def = def;
    return def ? &m : NULL;
}
#include "boolean.inc"
int main(int argc, char **argv) {
    (void)argc;
    value = argv[1];
    def = argc > 2 ? argv[2] : NULL;
    printf("%d\n", cfg_bool("MKW_X"));
    return 0;
}
"""


@pytest.fixture(scope="module")
def probe(tmp_path_factory: pytest.TempPathFactory):
    cc = shutil.which("clang") or shutil.which("cc") or shutil.which("gcc")
    if not cc:
        pytest.skip("no C compiler")
    tmp = tmp_path_factory.mktemp("cfg_bool")
    source = SOURCE.read_text(encoding="utf-8")
    start = source.index("int cfg_bool(const char *name) {")
    end = source.index("\n}", start) + len("\n}")
    (tmp / "boolean.inc").write_text(source[start:end], encoding="utf-8")
    (tmp / "main.c").write_text(HARNESS, encoding="utf-8")
    exe = tmp / "cfg_bool.exe"
    subprocess.run([cc, "-std=gnu11", str(tmp / "main.c"), "-I", str(tmp), "-o", str(exe)], check=True, timeout=60)

    def run(value: str, default: str | None = "0") -> int:
        args = [str(exe), value, *([default] if default is not None else [])]
        return int(subprocess.run(args, capture_output=True, text=True, check=True, timeout=10).stdout)

    return run


@pytest.mark.parametrize("word", ["0", "off", "OFF", "Off", "false", "False", "no", "NO"])
def test_words_for_no_are_false(probe, word: str) -> None:
    assert probe(word, "1") == 0


@pytest.mark.parametrize("word", ["1", "on", "ON", "true", "True", "yes", "YES", "2", "-1"])
def test_words_for_yes_and_other_numbers_are_true(probe, word: str) -> None:
    assert probe(word, "0") == 1


@pytest.mark.parametrize(("default", "expected"), [("0", 0), ("1", 1)])
def test_an_unknown_word_gives_the_rows_default(probe, default: str, expected: int) -> None:
    assert probe("banana", default) == expected
    assert probe("1x", default) == expected


def test_a_row_without_a_default_keeps_the_old_reading(probe) -> None:
    assert probe("banana", None) == 1
    assert probe("off", None) == 0
