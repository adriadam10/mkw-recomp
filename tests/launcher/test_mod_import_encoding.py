"""The mod importer's pipe protocol must use UTF-8, independently of Windows' active code page.

Why: a Unicode name could install and then fail while printing success. Run the actual CLI with cp1252 streams,
as Windows Python uses for redirected output, without a game. Usage: uv run pytest tests/launcher/test_mod_import_encoding.py
"""

import json
import subprocess
import sys
import zipfile
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tools/mod/import.py"
DRIVER = (
    "import runpy,sys; sys.stdout.reconfigure(encoding='cp1252'); "
    "sys.stderr.reconfigure(encoding='cp1252', errors=sys.stderr.errors); sys.argv=sys.argv[1:]; "
    "runpy.run_path(sys.argv[0],run_name='__main__')"
)


def cli(*args):
    # -I ignores PYTHONIOENCODING, just as the launcher does. Set the initial stream explicitly instead of
    # claiming that a Linux platform label exercises Windows' encoding defaults.
    return subprocess.run([sys.executable, "-I", "-c", DRIVER, str(SCRIPT), *map(str, args)],
                          capture_output=True, timeout=30)


@pytest.fixture
def folders(tmp_path):
    vanilla, source = tmp_path / "原始 café", tmp_path / "source"
    vanilla.mkdir()
    (source / "romfs").mkdir(parents=True)
    (source / "romfs/invented.txt").write_bytes(b"invented bytes, no game content")
    return tmp_path / "installation", vanilla, source


@pytest.mark.parametrize("name", ["Plain", "Café", "玩家"])
@pytest.mark.parametrize("archive", [False, True])
def test_success_is_utf8_after_install(folders, name, archive):
    game, vanilla, source = folders
    kind = "--folder"
    if archive:
        package = source.parent / "模组.zip"
        with zipfile.ZipFile(package, "w") as z:
            z.write(source / "romfs/invented.txt", "romfs/invented.txt")
        kind, source = "--zip", package
    result = cli("--game", game, "--romfs", vanilla, kind, source,
                 "--build-id", "0123456789abcdef", "--name", name)
    assert result.returncode == 0, result.stderr.decode("utf-8", "replace")
    assert result.stdout.decode("utf-8").strip() == f"done {name} romfs=1 exefs=-"
    assert not result.stderr
    assert json.loads((game / "mods" / name / "mod.json").read_text(encoding="utf-8"))["name"] == name


def test_unicode_error_path_is_reported(folders):
    game, vanilla, source = folders
    missing = vanilla / "不存在"
    result = cli("--game", game, "--romfs", missing, "--folder", source)
    assert result.returncode == 1
    assert result.stdout.decode("utf-8").startswith(f"error romfs {missing} ")
    assert not result.stderr


def test_unicode_variants_are_reported(folders):
    game, vanilla, source = folders
    # Both variants are invented overlays; the importer must ask which one, without copying either.
    (source / "romfs").rename(source / "临时")
    for name in ["Café", "玩家"]:
        (source / name / "romfs").mkdir(parents=True)
    result = cli("--game", game, "--romfs", vanilla, "--folder", source, "--build-id", "0123456789abcdef")
    assert result.returncode == 1
    assert result.stdout.decode("utf-8").strip() == "error variants Café\t玩家"
    assert not result.stderr
    assert not (game / "mods").exists()


def test_argument_errors_keep_utf8_details():
    result = cli("--game", "x", "--romfs", "x", "--folder", "x", "--未知参数")
    assert result.returncode == 2
    assert "--未知参数" in result.stderr.decode("utf-8")


def test_argument_errors_escape_lone_surrogates():
    result = cli("--game", "x", "--romfs", "x", "--folder", "x", "--bad-\udcff")
    assert result.returncode == 2
    assert b"unrecognized arguments: --bad-\\udcff" in result.stderr
