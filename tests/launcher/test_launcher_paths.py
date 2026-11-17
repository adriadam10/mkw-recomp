"""Setup's saved RomFS must reach mod imports in the same launcher session.

Why: the game reexecs and reads the new ini, while the launcher used its startup cache. Build the actual config,
boot resolver, mod resolver and completion handler with real file stubs; import synthetic NUP/ZIP data through the
real CLI and check original hashes. No game, SDL window or GPU. Usage: uv run pytest tests/launcher/test_launcher_paths.py
"""

import hashlib
import importlib.util
import os
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/prepare"))
from nup import Nup, pack  # noqa: E402
from prepare import ini_set  # noqa: E402


def cut(text: str, start: str, end: str) -> str:
    return text[text.index(start):text.index(end, text.index(start))]


@pytest.fixture(scope="module")
def paths_probe(tmp_path_factory: pytest.TempPathFactory) -> Path:
    cc = shutil.which("clang") or shutil.which("cc")
    if not cc:
        pytest.skip("C compiler needed")
    folder = tmp_path_factory.mktemp("launcher-paths")
    stubs = (ROOT / "tests/launcher/t_game_dir.c").read_text(encoding="utf-8")
    (folder / "installation_stubs.inc").write_text(stubs[:stubs.index("static int run(")], encoding="utf-8")
    (folder / "installation_entry.inc").write_text(stubs[stubs.index("#ifdef _WIN32\nint wmain"):], encoding="utf-8")
    boot = (ROOT / "runtime/core/boot/main.c").read_text(encoding="utf-8")
    mods = (ROOT / "runtime/ui/launcher/mods.cpp").read_text(encoding="utf-8")
    launcher = (ROOT / "runtime/ui/launcher/launcher.cpp").read_text(encoding="utf-8")
    setup = (ROOT / "runtime/ui/launcher/launcher_setup.cpp").read_text(encoding="utf-8")
    # As in test_adpcm.py, cut the actual functions, not a second implementation. Only the C++ aggregate's
    # spelling becomes a C compound literal; the same production fields and generation assignment are compiled.
    snippets = [
        'static const char *rt_default_romfs = "";\n',
        'static char pp_romfs[PLAT_PATH_MAX * 2];\n',
        'int plat_dir_exists(const char *p) { struct stat s; return !stat(p, &s) && S_ISDIR(s.st_mode); }\n',
        cut(boot, "static const char *portable_path(", "static void portable_paths("),
        cut(mods, "static char g_romfs_override", "void mods_toggle("),
        # launcher_set_romfs also keeps the path for the start screen's "game files gone" check (#486)
        cut(launcher, "static char g_romfs[", "\n") + "\n",
        cut(launcher, 'extern "C" void launcher_set_romfs(', 'extern "C" void launcher_set_exefs(')
            .replace('extern "C" ', ""),
        cut(boot, "static const char *launcher_romfs_arg;", "static int run_launcher("),
        cut(launcher, "static void setup_paths_finished(", 'extern "C" void launcher_gb_install('),
        "static SetupState g_state;\nstatic void start_setup(void) {\n",
        cut(setup, "    unsigned generation =", "    SDL_UnlockMutex(g_lock);")
            .replace("SetupState{", "(SetupState){"),
        "}\nstatic void boot_paths(int argc, char **argv) {\n",
        cut(boot, "    cfg_init();   // mkw.ini: the settings pages", "    // the players (Profiles page)"),
        "}\n",
    ]
    (folder / "launcher_paths.inc").write_text("".join(snippets), encoding="utf-8")
    includes = [ROOT / p for p in ("runtime", "runtime/cpu", "runtime/core/base", "runtime/core/boot",
                                  "runtime/ui/launcher")]
    flags = []
    if os.name == "nt":
        mingw = shutil.which("gcc")
        for p in ("C:/msys64/mingw64/bin/gcc.exe", "C:/msys64/ucrt64/bin/gcc.exe"):
            if not mingw and Path(p).is_file():
                mingw = p
        if not mingw:
            pytest.skip("MinGW libraries needed for the native Windows probe")
        libgcc = Path(subprocess.check_output([mingw, "-print-libgcc-file-name"], text=True).strip()).resolve()
        kernel = Path(subprocess.check_output([mingw, "-print-file-name=libkernel32.a"], text=True).strip()).resolve()
        flags = ["--target=x86_64-w64-windows-gnu", f"--sysroot={kernel.parent.parent}", f"-L{libgcc.parent}",
                 "-fuse-ld=lld", "-municode"]
    output = folder / ("probe.exe" if os.name == "nt" else "probe")
    sources = [ROOT / p for p in ("tests/launcher/t_launcher_paths.c", "runtime/core/base/cfg.c",
                                 "runtime/core/base/sysinfo_log.c", "runtime/core/boot/gamelib.c")]
    subprocess.run([cc, *flags, *(f"-I{p}" for p in [*includes, folder]), *(str(p) for p in sources),
                    "-o", str(output)], check=True, timeout=60)
    return output


class Session:
    def __init__(self, executable: Path, args: tuple[str, ...], env: dict[str, str]):
        self.process = subprocess.Popen([str(executable), *args], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE, text=True, encoding="utf-8", env=env)
        self.initial = self.read()

    def read(self) -> dict[str, str]:
        result = {}
        while True:
            line = self.process.stdout.readline()
            assert line, f"probe exited: {self.process.poll()} {self.process.stderr.read()}"
            if line.strip() == ".":
                return result
            key, value = line.rstrip("\r\n").split("=", 1)
            result[key] = value

    def command(self, value: str) -> dict[str, str]:
        self.process.stdin.write(value + "\n")
        self.process.stdin.flush()
        return self.read()

    def close(self):
        self.process.stdin.close()
        try:
            assert self.process.wait(timeout=10) == 0, self.process.stderr.read()
        finally:
            if self.process.poll() is None:
                self.process.kill()
                self.process.wait()
            self.process.stdout.close()
            self.process.stderr.close()


@pytest.fixture
def session(paths_probe: Path):
    # Keep native Windows installation roots short; the paths themselves still contain spaces and Unicode.
    with tempfile.TemporaryDirectory(prefix="mkw-lp-") as directory:
        tmp_path = Path(directory)
        launcher = tmp_path / "Launcher ñ"
        launcher.mkdir()
        executable = Path(shutil.copy2(paths_probe, launcher / paths_probe.name))
        env = {k: v for k, v in os.environ.items() if not k.startswith(("MKW_", "TEST_LOG_", "TEST_STDERR_"))}
        opened = []

        def start(initial: str = "", args: tuple[str, ...] = (), **overrides: str) -> tuple[Session, Path]:
            game = tmp_path / "Game ñ"
            game.mkdir(exist_ok=True)
            (launcher / "mkw.ini").write_text(f'game_dir = "{game}"\n', encoding="utf-8")
            (game / "mkw.ini").write_text(initial, encoding="utf-8")
            s = Session(executable, args, env | overrides)
            opened.append(s)
            return s, game

        yield start
        for s in opened:
            s.close()


def package(folder: Path, original: bytes = b"synthetic original") -> Path:
    source = folder / "source"
    (source / "exefs").mkdir(parents=True)
    (source / "exefs/main").write_bytes(b"synthetic header".ljust(0x40, b"\0") + bytes.fromhex("0123456789abcdef"))
    (source / "romfs/Example").mkdir(parents=True)
    (source / "romfs/Example/file.txt").write_bytes(original)
    path = folder / "External package ñ.nup"
    pack(source, path, 0x0400C3F00006E000, 0)
    return path


def import_mod(game: Path, romfs: str, archive: Path) -> subprocess.CompletedProcess:
    return subprocess.run([sys.executable, "-I", str(ROOT / "tools/mod/import.py"), "--game", str(game),
                           "--romfs", romfs, "--exefs", str(game / "exefs"), "--zip", str(archive)],
                          capture_output=True, text=True, encoding="utf-8", timeout=30)


def test_linked_setup_reaches_real_import(session, tmp_path: Path) -> None:
    nup = package(tmp_path / "Other folder ñ")
    archive = tmp_path / "Synthetic Mod.zip"
    with zipfile.ZipFile(archive, "w") as z:
        z.writestr("romfs/Example/file.txt", b"synthetic modded content")
    s, game = session()
    Nup(nup).extract("exefs", game / "exefs")
    before = import_mod(game, s.initial["romfs"], archive)
    assert before.returncode == 1 and "error romfs " in before.stdout
    s.command("start")
    ini_set(game / "mkw.ini", "romfs", str(nup))
    after = s.command("finish 1")
    assert after["romfs"] == str(nup)
    imported = import_mod(game, after["romfs"], archive)
    assert imported.returncode == 0, imported.stdout + imported.stderr
    manifest = (game / "mods/Synthetic Mod/romfs/manifest.tsv").read_text(encoding="utf-8")
    assert hashlib.sha256(b"synthetic original").hexdigest() in manifest


def test_switching_nups_changes_import_hashes(session, tmp_path: Path) -> None:
    first = package(tmp_path / "First", b"first original")
    second = package(tmp_path / "Second", b"second original")
    s, game = session(f"romfs = {first}\n")
    Nup(first).extract("exefs", game / "exefs")
    s.command("start")
    ini_set(game / "mkw.ini", "romfs", str(second))
    path = s.command("finish 1")["romfs"]
    archive = tmp_path / "Changed source.zip"
    with zipfile.ZipFile(archive, "w") as z:
        z.writestr("romfs/Example/file.txt", b"modded")
    imported = import_mod(game, path, archive)
    assert imported.returncode == 0, imported.stdout + imported.stderr
    manifest = (game / "mods/Changed source/romfs/manifest.tsv").read_text(encoding="utf-8")
    assert hashlib.sha256(b"second original").hexdigest() in manifest
    assert hashlib.sha256(b"first original").hexdigest() not in manifest


def test_refreshed_nup_is_merge_baseline(session, tmp_path: Path) -> None:
    spec = importlib.util.spec_from_file_location("mod_merge", ROOT / "tools/mod/merge.py")
    merge = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(merge)
    nup = package(tmp_path / "Archive source")
    source = nup.parent / "source"
    relative = "Example/files.sarc"
    vanilla = merge.build_sarc([("a.txt", b"original A"), ("b.txt", b"original B")])
    (source / "romfs" / relative).write_bytes(vanilla)
    pack(source, nup, 0x0400C3F00006E000, 0)
    s, game = session()
    s.command("start")
    ini_set(game / "mkw.ini", "romfs", str(nup))
    chosen = s.command("finish 1")["romfs"]
    for name, files in [("First", [("a.txt", b"changed A"), ("b.txt", b"original B")]),
                        ("Second", [("a.txt", b"original A"), ("b.txt", b"changed B")])]:
        dest = game / "mods" / name / "romfs" / relative
        dest.parent.mkdir(parents=True)
        dest.write_bytes(merge.build_sarc(files))
    p = subprocess.run([sys.executable, "-I", str(ROOT / "tools/mod/merge.py"), "--game", str(game),
                        "--romfs", chosen, "--enable", "First,Second"],
                       capture_output=True, text=True, encoding="utf-8", timeout=30)
    assert p.returncode == 0 and "conflicts=0" in p.stdout, p.stdout + p.stderr
    merged = game / "mods/.merged/romfs" / relative
    inners, _ = merge.parse_sarc_ordered(merged.read_bytes())
    assert dict(inners) == {"a.txt": b"changed A", "b.txt": b"changed B"}
    manifest = (game / "mods/.merged/romfs/manifest.tsv").read_text(encoding="utf-8")
    assert hashlib.sha256(vanilla).hexdigest() in manifest


def test_missing_linked_package_reports_selected_path(session, tmp_path: Path) -> None:
    s, game = session()
    missing = tmp_path / "External folder" / "Missing.nup"
    s.command("start")
    ini_set(game / "mkw.ini", "romfs", str(missing))
    chosen = s.command("finish 1")["romfs"]
    p = import_mod(game, chosen, tmp_path / "not-read.zip")
    assert p.returncode == 1 and f"error romfs {missing} " in p.stdout


@pytest.mark.parametrize("old,new", [("/old.nup", "/new.nup"), ("romfs", "/linked.nup"),
                                     ("/linked.nup", "romfs"), ("/old", "relative ñ/Game.nup"),
                                     ("/old", r"D:\Other folder\Game.nup"),
                                     ("/old", r"\\server\share\Game.nup")])
def test_source_changes(session, old: str, new: str) -> None:
    s, game = session(f"romfs = {old}\n")
    s.command("start")
    ini_set(game / "mkw.ini", "romfs", new)
    after = s.command("finish 1")
    expected = new if new[0] in "/\\" or new[1:2] == ":" else f"{game}/{new}"
    assert after["romfs"] == expected
    assert after["held"] == old  # cfg_get pointers held before refresh remain valid


@pytest.mark.parametrize("flag", ["--romfs", "--nup"])
def test_argv_override_survives_setup(session, flag: str) -> None:
    s, game = session("romfs = /old.nup\n", args=(flag, "/explicit.nup"), MKW_ROMFS="/environment.nup")
    s.command("start")
    ini_set(game / "mkw.ini", "romfs", "/new.nup")
    assert s.command("finish 1")["romfs"] == "/explicit.nup"


def test_environment_override_survives_setup(session) -> None:
    s, game = session("romfs = /old.nup\n", MKW_ROMFS="/environment.nup")
    s.command("start")
    ini_set(game / "mkw.ini", "romfs", "/new.nup")
    assert s.command("finish 1")["romfs"] == "/environment.nup"


def test_failed_and_repeated_completion(session) -> None:
    s, game = session("romfs = /old.nup\n")
    s.command("start")
    ini_set(game / "mkw.ini", "romfs", "/failed.nup")
    assert s.command("finish -1")["romfs"] == "/old.nup"
    s.command("start")
    ini_set(game / "mkw.ini", "romfs", "/good.nup")
    good = s.command("finish 1")
    assert good["romfs"] == "/good.nup"
    ini_set(game / "mkw.ini", "romfs", "/not-another-setup.nup")
    polled = s.command("poll")
    assert polled["romfs"] == good["romfs"] and polled["version"] == good["version"]
    s.command("start")  # start and finish entirely between polls still advances the production generation
    assert s.command("finish 1")["romfs"] == "/not-another-setup.nup"


def test_reload_preserves_dirty_settings_and_disk_path(session) -> None:
    s, game = session("romfs = /old.nup\nvolume = 80\nmods = Old\n")
    s.command("volume 37")
    s.command("mods First,Second")
    s.command("start")
    ini_set(game / "mkw.ini", "romfs", "/new.nup")
    after = s.command("finish 1")
    assert after["volume"] == "37" and after["mods"] == "First,Second"
    # The reloaded key stays clean: a later external change must not be overwritten by an unrelated menu save.
    ini_set(game / "mkw.ini", "romfs", "/later.nup")
    assert s.command("save")["result"] == "1"
    saved = (game / "mkw.ini").read_text(encoding="utf-8")
    assert "romfs = /later.nup" in saved and "volume = 37" in saved and "mods = First,Second" in saved


@pytest.mark.parametrize("content", [None, "volume = 50\n", 'romfs = ""\n'])
def test_missing_file_or_nonempty_key_keeps_previous_source(session, content: str | None) -> None:
    s, game = session("romfs = /old.nup\n")
    s.command("start")
    if content is None:
        (game / "mkw.ini").unlink()
    else:
        (game / "mkw.ini").write_text(content, encoding="utf-8")
    assert s.command("finish 1")["romfs"] == "/old.nup"


def test_reload_matches_startup_ini_rules(session) -> None:
    s, game = session("romfs = /old.nup\n")
    s.command("start")
    (game / "mkw.ini").write_bytes(b'\xef\xbb\xbf[files]\r\n# romfs = ignored\r\nRoMfS = /earlier\r\n'
                                  b' MkW_rOmFs = "/final path.nup" \r\nromfs = \r\n; comment\r\n')
    after = s.command("finish 1")
    assert after["romfs"] == "/final path.nup" and after["cfg"] == "/final path.nup"
