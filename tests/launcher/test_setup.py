"""The setup's command lines and files without the game: paths with spaces and accents, the resume without --source and
the update's checks. Seconds, on Linux and on Windows (ci.yml), so a break like issue #29 (a compiler path split at its
space, found by a player) shows in the commit that makes it.

Usage: uv run pytest tests
"""

import json
import os
import shutil
import struct
import subprocess
import sys
from collections.abc import Callable
from pathlib import Path
from types import SimpleNamespace

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "sm86/spec"))
sys.path.insert(0, str(ROOT / "tools/prepare"))
import prepare  # noqa: E402
import gamesource  # noqa: E402  (after prepare: it puts recomp/ on the path)
import gamecode  # noqa: E402
import machine  # noqa: E402
import spec  # noqa: E402
import update  # noqa: E402

ODD = "Recomp C ñ"  # a space and an accent, as the players' folders have


def test_cc_survives_spaces(tmp_path: Path) -> None:
    # the shaders step's command line as prepare.py builds it, read back as spec.py's argparse does
    cc = [str(tmp_path / ODD / "zig.exe"), "cc", "-target", "x86_64-windows-gnu"]
    cmd = prepare.spec_command(tmp_path / ODD, tmp_path / ODD / "work", cc, None)
    assert spec.cc_command(cmd[cmd.index("--cc") + 1]) == cc
    assert spec.cc_command("zig cc") == ["zig", "cc"]  # by hand


def run_prepare(*args: str) -> subprocess.CompletedProcess:
    return subprocess.run([sys.executable, "-I", str(ROOT / "tools/prepare/prepare.py"), *args],
                          capture_output=True, text=True, encoding="utf-8", timeout=60)  # fmt: skip


@pytest.mark.parametrize("name", ["Plain", "Café", "玩家"])
def test_moved_source_event_uses_utf8(tmp_path: Path, name: str) -> None:
    gone = tmp_path / name
    state = {"version": 1, "done": {}, "source": str(gone), "romfs_mode": "link"}
    (tmp_path / "setup-state.json").write_text(json.dumps(state), encoding="utf-8")
    # The launcher uses -I, so model the Windows pipe's encoding inside the child.
    driver = ("import runpy,sys; sys.stdout.reconfigure(encoding='cp1252'); "
              "sys.argv=sys.argv[1:]; runpy.run_path(sys.argv[0],run_name='__main__')")
    p = subprocess.run([sys.executable, "-I", "-c", driver, str(ROOT / "tools/prepare/prepare.py"),
                        "--game", str(tmp_path)], capture_output=True, timeout=60)  # fmt: skip
    assert p.returncode == 1
    assert p.stdout.decode("utf-8").splitlines() == [f"error moved {gone}"]
    assert f"error moved {gone}" in (tmp_path / "logs/setup.log").read_text(encoding="utf-8")


@pytest.mark.parametrize("state", [[], {"done": None}, {"done": {"exefs": 42}}])
@pytest.mark.parametrize("location", ["live", "resume", "promote"])
def test_malformed_setup_state(tmp_path: Path, state, location: str) -> None:
    live = tmp_path / "mkwgame.dll"
    live.write_bytes(b"played library")
    staged = tmp_path / "setup-next"
    staged.mkdir()
    (staged / live.name).write_bytes(b"staged library")
    if isinstance(state, dict):
        state = dict(state, version=prepare.PREP_VERSION, ready=True, port="0.1.16")
    folder = tmp_path if location == "live" else staged
    (folder / "setup-state.json").write_text(json.dumps(state), encoding="utf-8")

    args = ["--game", str(tmp_path), "--port", "0.1.16"]
    if location == "resume":
        args.append("--next")
    p = run_prepare(*args)
    assert live.read_bytes() == b"played library"
    assert p.returncode == 2 and p.stdout.strip() == "error source no earlier setup in this folder", p.stdout + p.stderr


@pytest.mark.parametrize("external", [False, True])
def test_resume_without_source(tmp_path: Path, external: bool) -> None:
    game = tmp_path / ODD / "game"
    game.mkdir(parents=True)
    copy = tmp_path / ODD / "copy"
    copy.mkdir()
    state = {"version": 1, "done": {"exefs": str(copy / "exefs"), "romfs": f"link {copy / 'romfs'}"}}
    (game / "setup-state.json").write_text(json.dumps(state))
    p = run_prepare("--game", str(game), *(["--runtime", str(tmp_path / "Launcher ñ")] if external else []))
    # the last setup's copy, whole: check fails on its missing files, naming the folder with its space
    assert "step check" in p.stdout, p.stdout + p.stderr
    assert str(copy) in p.stdout, p.stdout
    saved = json.loads((game / "setup-state.json").read_text())
    assert saved.get("source", str(copy)) == str(copy)


def test_resume_from_moved_folder(tmp_path: Path) -> None:
    # issue #188: the last setup's folder renamed; the start screen names it and asks for it again
    game = tmp_path / ODD / "game"
    game.mkdir(parents=True)
    gone = tmp_path / ODD / "renamed away"
    (game / "setup-state.json").write_text(json.dumps({"version": 1, "source": str(gone), "done": {}}))
    p = run_prepare("--game", str(game))
    assert p.returncode == 1 and f"error moved {gone}" in p.stdout, p.stdout + p.stderr


def test_resume_copy_source_deleted(tmp_path: Path) -> None:
    # issue #501: a copy's original files deleted after the setup; the game folder holds them and is the source now
    game = tmp_path / ODD / "game"
    (game / "exefs").mkdir(parents=True)
    (game / "exefs/main").write_bytes(b"\0")
    (game / "romfs").mkdir()
    gone = tmp_path / ODD / "deleted"
    state = {"version": 1, "source": str(gone), "romfs_mode": "copy", "done": {"romfs": "copy 1.8.0"}}
    (game / "setup-state.json").write_text(json.dumps(state))
    p = run_prepare("--game", str(game))
    assert "error moved" not in p.stdout and "step check" in p.stdout, p.stdout + p.stderr
    assert f"error nogame {game}" in p.stdout or str(game) in p.stdout.split("step check", 1)[1], p.stdout


def test_keys_from_before_kept() -> None:
    # a step done under its key with the source folders (before 0.1.10) is not redone; another key still is
    state = {"done": {"shaders": "/old/exefs /old/romfs t1", "compile": "/old/exefs c1"}}
    prepare.migrate_keys(state, {"shaders": "1.0.0 t1", "compile": "1.0.0 c2"},
                         {"shaders": "/old/exefs /old/romfs t1", "compile": "/old/exefs c2"})  # fmt: skip
    assert state["done"] == {"shaders": "1.0.0 t1", "compile": "/old/exefs c1"}


def test_resume_without_earlier_setup(tmp_path: Path) -> None:
    p = run_prepare("--game", str(tmp_path / ODD))
    assert p.returncode == 2 and "error source" in p.stdout, p.stdout


@pytest.mark.parametrize("failure", ["exit", "timeout"])
def test_warm_failure_does_not_fail_the_setup(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str], failure: str
) -> None:
    # Until 17/11/2026 the prewarm ran the game for a minute and kept the keys of what its title drew.
    def run(argv: list[str], **kwargs: object) -> SimpleNamespace:
        if failure == "timeout":
            raise prepare.subprocess.TimeoutExpired(argv, 1)
        return SimpleNamespace(returncode=70)

    monkeypatch.setattr(prepare.subprocess, "run", run)
    prepare.warm(tmp_path / "game", tmp_path / "setup-work")  # does not raise
    assert "left to the game" in capsys.readouterr().err


@pytest.mark.skipif(os.name == "nt", reason="creating directory symlinks requires Windows privileges")
def test_early_setup_log_preserves_alias(tmp_path: Path) -> None:
    target, alias, source = tmp_path / "real", tmp_path / "Game link ñ", tmp_path / "empty dump"
    target.mkdir()
    source.mkdir()
    alias.symlink_to(target, target_is_directory=True)
    p = run_prepare("--game", str(alias), "--source", str(source))
    assert p.returncode == 1 and "error nogame" in p.stdout, p.stdout + p.stderr
    log = (alias / "logs/setup.log").read_text(encoding="utf-8")
    # main() must keep _early_log's file open under the same alias, retaining startup diagnostics and its timer.
    assert "setup: started, Python" in log
    assert "setup: " in log and str(alias) in log


def test_versions() -> None:
    assert update.version_key("0.1.2-12-g1c15097") == (0, 1, 2)
    assert update.version_key("v0.1.10") > update.version_key("0.1.9")
    assert update.version_key("dev") is None


@pytest.mark.parametrize("os_name", ["linux", "linux-aarch64", "windows"])
@pytest.mark.parametrize("damage", ["none", "size", "digest"])
def test_fetch_update_package(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str], os_name: str, damage: str
) -> None:
    """Both Linux architectures stage an executable AppImage; only Windows extracts a ZIP, after integrity checks."""
    import hashlib
    import zipfile

    payload = b"synthetic update payload; never executed\n"
    source = tmp_path / "download"
    if os_name == "windows":
        with zipfile.ZipFile(source, "w") as archive:
            archive.writestr("mkw-recomp/mkw.exe", payload)
    else:
        source.write_bytes(payload)
    data = source.read_bytes()
    asset = {
        "name": "mkw-recomp-v9.8.7" + update.ASSET_SUFFIX[os_name],
        "size": len(data) + (damage == "size"),
        "digest": "sha256:" + ("0" * 64 if damage == "digest" else hashlib.sha256(data).hexdigest()),
        "browser_download_url": source.as_uri(),
    }
    metadata = tmp_path / "release.json"
    metadata.write_text(json.dumps({
        "tag_name": "v9.8.7", "draft": False, "prerelease": False,
        "assets": [dict(asset, name="mkw-recomp-v9.8.7" + suffix) for suffix in update.ASSET_SUFFIX.values()],
    }))
    real_get = update.get

    def local_get(url: str, timeout: int):
        # Redirect only the release API to a local JSON file; selection, reads and staging use production code.
        if url == f"https://api.github.com/repos/{update.REPO}/releases/latest":
            url = metadata.as_uri()
        else:
            assert url == source.as_uri(), f"unexpected network request: {url}"
        return real_get(url, timeout)

    monkeypatch.setattr(update, "get", local_get)
    dest = (tmp_path / ODD).resolve()
    monkeypatch.setattr(sys, "argv", ["update.py", "fetch", "--os", os_name, "--tag", "v9.8.7", "--dest", str(dest)])
    part = dest / (asset["name"] + ".part")
    if damage != "none":
        with pytest.raises(SystemExit) as exited:
            update.main()
        assert exited.value.code == 1
        assert not part.exists() and not list(dest.iterdir())
        output = capsys.readouterr()
        assert "error fetch " in output.out and "came damaged" in output.out and "ready " not in output.out
        assert not output.err
        return
    update.main()
    out = dest / "new/mkw-recomp/mkw.exe" if os_name == "windows" else dest / asset["name"]
    assert out.read_bytes() == payload and not part.exists()
    ready = out.parent if os_name == "windows" else out
    assert f"ready {ready}\n" in capsys.readouterr().out
    if os_name != "windows":
        assert not (dest / "new").exists()
        if os.name != "nt":
            assert out.stat().st_mode & 0o777 == 0o755


@pytest.mark.parametrize("failure", ["incomplete", "reset"])
def test_fetch_interrupted_download(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str], failure: str,
) -> None:
    import hashlib
    import http.client
    import io

    first, last = b"first chunk", b"last chunk"

    class InterruptedResponse(io.BytesIO):
        def read(self, size: int = -1) -> bytes:
            if self.tell() == 0:
                return super().read(size)
            if failure == "incomplete":
                raise http.client.IncompleteRead(b"", len(last))
            raise ConnectionResetError("connection reset")

    asset = {
        "name": "mkw-recomp-v9.8.7-x86_64.AppImage",
        "size": len(first + last),
        "digest": "sha256:" + hashlib.sha256(first + last).hexdigest(),
        "browser_download_url": "https://example.invalid/download",
    }
    monkeypatch.setattr(update, "latest_asset", lambda _: ("v9.8.7", asset))
    monkeypatch.setattr(update, "get", lambda *_: InterruptedResponse(first))
    monkeypatch.setattr(sys, "argv", ["update.py", "fetch", "--os", "linux", "--tag", "v9.8.7", "--dest", str(tmp_path)])

    with pytest.raises(SystemExit) as exited:
        update.main()

    assert exited.value.code == 1
    lines = capsys.readouterr().out.splitlines()
    assert lines[0] == f"progress {len(first)} {asset['size']}"
    assert len(lines) == 2 and lines[1].startswith("error fetch ")
    assert not list(tmp_path.iterdir())

    monkeypatch.setattr(update, "get", lambda *_: io.BytesIO(first + last))
    update.main()
    out = tmp_path / asset["name"]
    assert out.read_bytes() == first + last
    assert capsys.readouterr().out.splitlines()[-1] == f"ready {out.resolve()}"
    assert list(tmp_path.iterdir()) == [out]


def test_romfs_version_matches_main() -> None:
    # #315: 1.0.0's exefs/main with 1.8.0's RomFS passed the file count and crashed while warming up
    import versions

    v100, v180 = versions.VERSIONS
    res = "System/Resource/ResourceSizeTable."
    r100 = [f"{res}Product.100.Nin_Ounce_NVN2.rsizetable.zs", f"{res}Pilot.100.Nin_Ounce_NVN2.rsizetable.zs"]
    r180 = [f"{res}Product.180.Nin_Ounce_NVN2.rsizetable.zs", "Layout/Title.sarc.zs"]
    assert versions.romfs_mismatch(v100, r100) == "" and versions.romfs_mismatch(v180, r180) == ""
    assert versions.romfs_mismatch(v100, r180) == "RomFS 1.8.0, exefs/main 1.0.0"
    assert versions.romfs_mismatch(v180, r100) == "RomFS 1.0.0, exefs/main 1.8.0"
    assert versions.romfs_mismatch(v100, ["Layout/Title.sarc.zs"]) == ""  # no marker: the file count decides


@pytest.mark.parametrize("product, ok", [(100, True), (180, False)])
def test_check_refuses_other_romfs(tmp_path: Path, monkeypatch, product: int, ok: bool) -> None:
    # check() on a loose dump: 1.0.0's main (its build id in an NSO header) and a RomFS naming `product`
    import versions

    v100 = versions.VERSIONS[0]
    (tmp_path / "exefs").mkdir()
    (tmp_path / "exefs/main").write_bytes(b"NSO0" + bytes(0x3C) + bytes.fromhex(v100.main_build_id))
    res = tmp_path / "romfs/System/Resource"
    res.mkdir(parents=True)
    (res / f"ResourceSizeTable.Product.{product}.Nin_Ounce_NVN2.rsizetable.zs").write_bytes(b"")
    monkeypatch.setattr(gamesource, "count_files", lambda top, enough: enough)  # the file count is not under test
    if ok:
        assert prepare.check(tmp_path, tmp_path)[2] == v100
    else:
        with pytest.raises(prepare.Failed) as e:
            prepare.check(tmp_path, tmp_path)
        assert "RomFS 1.8.0, exefs/main 1.0.0" in str(e.value)


@pytest.mark.parametrize("name", ["stale.old", "setup.old"])
def test_clean_removes_old(tmp_path: Path, name: str) -> None:
    game = tmp_path / ODD
    (game / "update").mkdir(parents=True)
    old = game / name
    old.mkdir() if name == "setup.old" else old.write_text("x")
    (game / "mkw.exe").write_text("keep")
    update.clean(type("A", (), {"game": game, "previous": None})())
    assert not old.exists() and not (game / "update").exists() and (game / "mkw.exe").exists()


def test_clean_keeps_previous(tmp_path: Path) -> None:
    # with --previous the replaced files go to update-previous/ under their own names, for the Tools page's Undo
    game = tmp_path / ODD
    (game / "update-previous").mkdir(parents=True)
    (game / "update-previous/gone.dll").write_text("older still")
    (game / "mkw.exe.old").write_text("0.1.10")
    (game / "lang.old").mkdir()
    update.clean(type("A", (), {"game": game, "previous": "0.1.10"})())
    keep = game / "update-previous"
    assert (keep / "mkw.exe").read_text() == "0.1.10" and (keep / "lang").is_dir()
    assert (keep / "version").read_text().strip() == "0.1.10" and not (keep / "gone.dll").exists()
    assert not list(game.glob("*.old"))


def test_package_sources_import(tmp_path: Path) -> None:
    # setup/src as dist.py copies it: prepare.py and what it imports load without the rest of the repository (build.py
    # imported tools/i18n/catalog.py, which the package lacked)
    sys.path.insert(0, str(ROOT / "tools/build"))
    import dist  # noqa: PLC0415

    dist.copy_sources(tmp_path / "src")
    p = subprocess.run([sys.executable, "-I", str(tmp_path / "src/tools/prepare/prepare.py"), "--help"],
                       capture_output=True, text=True, timeout=60)  # fmt: skip
    assert p.returncode == 0, p.stderr


def test_package_tools_stay_beside_launcher(tmp_path: Path) -> None:
    sys.path.insert(0, str(ROOT / "tools/build"))
    import dist  # noqa: PLC0415

    setup = tmp_path / ODD / "launcher/setup"
    dist.copy_sources(setup / "src")
    code = """
import sys
from pathlib import Path
sys.path.insert(0, sys.argv[1])
import machine
assert machine.package_dir(Path(sys.argv[2])) == Path(sys.argv[3])
"""
    p = subprocess.run([sys.executable, "-I", "-c", code, str(setup / "src/tools/prepare"),
                        str(tmp_path / "Other installation ñ"), str(setup)],
                       capture_output=True, text=True, timeout=60)  # fmt: skip
    assert p.returncode == 0, p.stderr


def test_warm_uses_launcher_with_selected_installation(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    game, runtime = tmp_path / ODD / "game", tmp_path / "launcher"
    calls = []

    def start(argv: list[str], **kwargs: object) -> SimpleNamespace:
        calls.append((argv, kwargs))
        return SimpleNamespace(returncode=0)

    monkeypatch.setattr(prepare.subprocess, "run", start)
    prepare.warm(game, game / "setup-work", runtime)
    argv, kwargs = calls[0]
    assert argv == [str(runtime / ("mkw.exe" if os.name == "nt" else "mkw")), "--prewarm"]
    assert kwargs["env"]["MKW_GAME_DIR"] == str(game)


@pytest.mark.skipif(os.name == "nt", reason="creating directory symlinks requires Windows privileges")
def test_setup_preserves_short_alias_for_warmup(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    target = tmp_path / "real"
    for _ in range(6):
        target /= "deep" * 40
    target.mkdir(parents=True)
    assert len(str(target).encode()) > 768
    alias = tmp_path / "Game link ñ"
    alias.symlink_to(target, target_is_directory=True)
    runtime, source = tmp_path / "launcher", tmp_path / "dump"
    calls = []

    def start(argv: list[str], **kwargs: object) -> SimpleNamespace:
        calls.append((argv, kwargs))
        return SimpleNamespace(returncode=0)

    monkeypatch.setattr(prepare, "own_children", lambda: None)
    monkeypatch.setattr(prepare.update, "ca_file", lambda: None)
    monkeypatch.setattr(prepare, "STEPS", ("check", "warm"))
    monkeypatch.setattr(prepare, "check", lambda *args: (source / "exefs", source / "romfs",
                                                       SimpleNamespace(name="test")))
    monkeypatch.setattr(prepare.subprocess, "run", start)
    monkeypatch.setattr(sys, "argv", ["prepare.py", "--game", str(alias), "--runtime", str(runtime),
                                      "--source", str(source)])
    # main opens its setup log on stderr. Restore pytest's capture stream after exercising the real argument path
    # and warm-up environment; only the game check and game process are synthetic.
    original_stderr = sys.stderr
    monkeypatch.setattr(sys, "stderr", original_stderr)
    try:
        prepare.main()
    finally:
        if sys.stderr is not original_stderr:
            sys.stderr.close()
    argv, kwargs = calls[0]
    assert argv == [str(runtime / "mkw"), "--prewarm"]
    assert kwargs["env"]["MKW_GAME_DIR"] == str(alias)


@pytest.mark.parametrize("driver", ["clang", "lld-link", "zig"])
@pytest.mark.parametrize("name", ["Space and ñ", 'Quote" ñ', "Backslash\\ ñ", 'Both" \\ ñ '])
def test_link_response_preserves_paths(tmp_path: Path, driver: str, name: str) -> None:
    windows = driver == "lld-link"
    if os.name == "nt" and (not windows or any(c in name for c in '\\"') or name.endswith(" ")):
        pytest.skip("POSIX linker or directory name")
    cc = shutil.which("clang")
    linker = shutil.which(driver)
    if not linker and driver == "zig":
        packaged = ROOT / "build/dist/linux/setup/zig/zig"
        linker = str(packaged) if packaged.is_file() else None
    if not cc or not linker:
        pytest.skip("clang and the requested linker are needed (zig may come from the local release package)")
    folder = tmp_path / name
    folder.mkdir()
    src, obj, rsp = folder / "stub.c", folder / "stub.o", folder / "link.rsp"
    src.write_text("int response_file_probe(void) { return 7; }\n")
    flags = ["--target=x86_64-windows-gnu"] if windows else ["-fPIC"]
    subprocess.run([cc, *flags, "-c", str(src), "-o", str(obj)], check=True)
    gamecode.write_link_response(rsp, [obj], windows or driver == "zig")
    lib = folder / ("stub.dll" if windows else "stub.so")
    args = ([linker, "/dll", "/noentry", "/export:response_file_probe", f"/out:{lib}", f"@{rsp}"] if windows
            else [linker, *(["cc"] if driver == "zig" else []), "-shared", "-o", str(lib), f"@{rsp}"])
    # Reuse Zig's compiler runtime across cases; the response file and object paths remain different each time.
    env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(tmp_path.parent / "setup-link-zig-cache"),
               ZIG_LOCAL_CACHE_DIR=str(tmp_path / "zig-local-cache"))
    p = subprocess.run(args, env=env, capture_output=True, text=True, timeout=120)
    assert p.returncode == 0, p.stderr
    assert lib.is_file()


def test_unpack_uses_launcher_not_installation(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    runtime, game = tmp_path / ODD / "launcher", tmp_path / "installation"
    calls = []
    process = SimpleNamespace(stdout=["unpack: 1 files, 64 bytes\n"], wait=lambda: 0)

    def start(argv: list[str], **kwargs: object) -> SimpleNamespace:
        calls.append(argv)
        return process

    monkeypatch.setattr(gamesource.subprocess, "Popen", start)
    package = tmp_path / "Dump ñ.nx"
    assert gamesource.unpack(runtime, package, game, "exefs") == "unpack: 1 files, 64 bytes"
    assert calls == [[str(runtime / ("mkw.exe" if os.name == "nt" else "mkw")),
                      "--unpack", str(package), str(game), "exefs"]]


def test_find_game_packages(tmp_path: Path) -> None:
    # a .nup or .nx package is the source as one file (its exefs and romfs both); any other file is refused
    for name, magic in (("g.nup", b"NUP\x01"), ("g.nx", b"NXAR")):
        (tmp_path / name).write_bytes(magic + bytes(60))
        assert gamesource.find_game(tmp_path / name) == (tmp_path / name, tmp_path / name)
    (tmp_path / "g.zip").write_bytes(b"PK\x03\x04" + bytes(60))
    with pytest.raises(prepare.Failed):
        gamesource.find_game(tmp_path / "g.zip")


def test_regenerate_if_cleaned(tmp_path: Path) -> None:
    # an update that changes only the compile key, after cleanup removed generated/: generate runs again
    keys = {"generate": "g", "compile": "c2"}
    state = {"done": {"generate": "g", "compile": "c1"}}
    prepare.regenerate_if_cleaned(state, keys, tmp_path)
    assert "generate" not in state["done"]
    # generated/ still there, or compile already done: generate stays skipped
    (tmp_path / "generated").mkdir()
    (tmp_path / "generated/layout.json").write_text("[]")
    state = {"done": {"generate": "g", "compile": "c1"}}
    prepare.regenerate_if_cleaned(state, keys, tmp_path)
    assert state["done"]["generate"] == "g"
    state = {"done": {"generate": "g", "compile": "c2"}}
    prepare.regenerate_if_cleaned(state, keys, tmp_path / "none")
    assert state["done"]["generate"] == "g"


def test_fetch_retries_damaged_downloads(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    # issue #207: each try got another checksum; a damaged download is fetched again, and after the last a clear error
    import hashlib

    good = b"nvidia package"
    arrivals = [b"nvidia pa", b"nvidia packagX", good]

    def download(url: str, part: Path) -> str:
        part.write_bytes(arrivals.pop(0))
        return ""

    monkeypatch.setattr(spec, "download", download)
    want = hashlib.sha256(good).hexdigest()
    assert spec.fetch("x/pkg.zip", want, tmp_path).read_bytes() == good
    assert not (tmp_path / "pkg.zip.part").exists()
    arrivals[:] = [b"bad"] * spec.FETCH_TRIES
    (tmp_path / "pkg.zip").unlink()
    with pytest.raises(SystemExit, match="arrived damaged 3 times"):
        spec.fetch("x/pkg.zip", want, tmp_path)


@pytest.mark.parametrize("cut", [8, 0x80 + 8, 0x80 + 0x30 + 0x40 + 3 * 24 + 16])
def test_nup_cut_table_is_reported(tmp_path: Path, cut: int) -> None:
    import nup

    src = tmp_path / "source"
    for name in ("exefs/main", "romfs/file"):
        path = src / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b"synthetic")
    package = tmp_path / "cut.nup"
    nup.pack(src, package, 0x0400C3F00006E000, 0)
    data = package.read_bytes()
    assert nup.Nup(package).files("romfs")
    package.write_bytes(data[:cut])
    with pytest.raises(ValueError, match="cut short"):
        nup.Nup(package).files("romfs")


def test_fastcc_falls_back_when_probe_line_is_incomplete(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    import fastcc

    clang = tmp_path / "clang" / ("clang.exe" if os.name == "nt" else "clang")
    clang.parent.mkdir()
    clang.touch()
    monkeypatch.delenv("MKW_SETUP_CC", raising=False)
    monkeypatch.setattr(fastcc, "version", lambda _: "clang version 22.1.8")
    monkeypatch.setattr(fastcc.subprocess, "run", lambda *args, **kwargs: SimpleNamespace(
        returncode=0, stderr='"-cc1"\n'))
    assert fastcc.setup([str(tmp_path / "zig"), "cc"], tmp_path, tmp_path, [["-O1"]]) is None


def test_nup_pack_reads_back(tmp_path: Path) -> None:
    # a loose dump packed into one .nup (nup.py pack) reads back the same through the module the setup uses
    sys.path.insert(0, str(ROOT / "tools/prepare"))
    import nup

    src = tmp_path / ODD
    files = {"exefs/main": b"NSO0" + bytes(100), "romfs/a.bin": b"x" * 5000, "romfs/Sub Dir/b": b"yz", "romfs/c": b""}
    for name, data in files.items():
        (src / name).parent.mkdir(parents=True, exist_ok=True)
        (src / name).write_bytes(data)
    out = tmp_path / "g.nup"
    nup.pack(src, out, 0x0400C3F00006E000, 0)
    n = nup.Nup(out)
    assert n.title_id == "0400C3F00006E000" and n.version == 0
    got = {f"{part}/{k}": n.read(e) for part in ("exefs", "romfs") for k, e in n.files(part).items()}
    assert got == files


def layered_nup(path: Path, titles: list[tuple[int, int, dict[str, bytes]]]) -> None:
    """A .nup of several titles, (title id, version, {"exefs/x" | "romfs/x": data}) each, laid out as nup.c documents
    and Eglantine's --pack-nup writes it: one root/exefs/romfs dir tree per title, "z:" names stored as one zstd frame.
    A title without exefs files gets no exefs dir (index past the table)."""
    from compression import zstd

    dirs: list[list] = []  # [name, children]; children ("d", i) or ("f", i)
    files: list[tuple[str, bytes, bool]] = []
    recs = []

    def tree(name: str, entries: dict[str, bytes]) -> int:
        d = len(dirs)
        dirs.append([name, []])
        subs: dict[str, dict[str, bytes]] = {}
        for rel, data in entries.items():
            head, _, rest = rel.partition("/")
            if rest:
                subs.setdefault(head, {})[rest] = data
            else:
                z = head.startswith("z:")
                dirs[d][1].append(("f", len(files)))
                files.append((head[2:] if z else head, data, z))
        for sub, inner in sorted(subs.items()):
            dirs[d][1].append(("d", tree(sub, inner)))
        return d

    for tid, version, content in titles:
        parts: dict[str, dict[str, bytes]] = {"exefs": {}, "romfs": {}}
        for k, v in content.items():
            part, _, rel = k.partition("/")
            parts[part][rel] = v
        root = tree("", {})
        ex = tree("exefs", parts["exefs"]) if parts["exefs"] else 0xFFFFFFFF
        ro = tree("romfs", parts["romfs"])
        dirs[root][1] = ([("d", ex)] if ex != 0xFFFFFFFF else []) + [("d", ro)]
        recs.append(struct.pack("<QIIII", tid, root, ex, ro, version).ljust(0x40, b"\0"))
    pool = bytearray()

    def name(s: str) -> tuple[int, int]:
        b = s.encode()
        pool.extend(b)
        return len(pool) - len(b), len(b)

    nd, nf = len(dirs), len(files)
    nchild = sum(len(d[1]) for d in dirs)
    pool_off = 0x30 + 0x40 * len(titles) + nd * 24 + nf * 80 + nchild * 4
    dir_names = [name(d[0]) for d in dirs]
    file_names = [name(f[0]) for f in files]
    data_at = (0x80 + pool_off + len(pool) + 0xFFF) & ~0xFFF
    blobs = [zstd.compress(f[1]) if f[2] else f[1] for f in files]
    toc = (b"TOC\x01" + struct.pack("<IQQQQ", len(titles), nd, nf, pool_off, len(pool))).ljust(0x30, b"\0")
    toc += b"".join(recs)
    first = 0
    for (off, n), d in zip(dir_names, dirs, strict=True):
        toc += struct.pack("<4I8x", off, n, first, len(d[1]))
        first += len(d[1])
    at, offsets = data_at, []
    for (off, n), f, blob in zip(file_names, files, blobs, strict=True):
        codec = bytes([1 if f[2] else 0])  # @0x40 after the 32-byte hash: 1 is one zstd frame
        toc += struct.pack("<IIQQQ", off, n, len(f[1]), at, len(blob)) + bytes(32) + codec + bytes(15)
        offsets.append(at)
        at = (at + len(blob) + 15) & ~15
    for d in dirs:
        toc += b"".join(struct.pack("<I", i if kind == "d" else nd + i) for kind, i in d[1])
    toc += pool
    with open(path, "wb") as o:
        o.write((b"NUP\x01" + struct.pack("<II", 0x10080, 0x80) + struct.pack("<Q", len(toc))).ljust(0x80, b"\0"))
        o.write(toc)
        for blob, off in zip(blobs, offsets, strict=True):
            o.seek(off)
            o.write(blob)


def test_nup_titles_layered(tmp_path: Path) -> None:
    # base game + an update holding only what it changes (Eglantine's --pack-nup): the RomFS read through every title,
    # the newest file kept; the ExeFS the update's whole; a DLC (another title id) and a title's order in the table
    # ignored, by version; one zstd frame read back
    sys.path.insert(0, str(ROOT / "tools/prepare"))
    import nup

    mkw = 0x0400C3F00006E000
    base = {"exefs/main": b"NSO0 1.0.0", "exefs/subsdk0": b"old", "romfs/a.bin": b"a100", "romfs/Sub/b": b"b100",
            "romfs/gone": b"only in 1.0.0"}  # fmt: skip
    update = {"exefs/main": b"NSO0 1.8.0", "exefs/subsdk0": b"movie", "exefs/subsdk1": b"old",
              "romfs/a.bin": b"a180", "romfs/Sub/z:new": b"n" * 4000}  # fmt: skip
    dlc = {"romfs/a.bin": b"dlc", "romfs/dlc": b"d"}
    out = tmp_path / "layered.nup"
    layered_nup(out, [(mkw, 917504, update), (mkw | 0x1001, 65536, dlc), (mkw, 0, base)])
    n = nup.Nup(out)
    assert (n.title_id, n.version) == ("0400C3F00006E000", 917504)
    exefs = {k: n.read(e) for k, e in n.files("exefs").items()}
    romfs = {k: n.read(e) for k, e in n.files("romfs").items()}
    assert exefs == {"main": b"NSO0 1.8.0", "subsdk0": b"movie", "subsdk1": b"old"}
    assert romfs == {"a.bin": b"a180", "Sub/b": b"b100", "Sub/new": b"n" * 4000, "gone": b"only in 1.0.0"}
    assert list(n.files("romfs")) == sorted(romfs)
    # an update without an ExeFS of its own: the base game's
    layered_nup(out, [(mkw, 0, base), (mkw | 0x800, 917504, {"romfs/a.bin": b"a180"})])
    n = nup.Nup(out)
    assert n.version == 917504 and n.read(n.files("exefs")["main"]) == b"NSO0 1.0.0"
    assert n.read(n.files("romfs")["a.bin"]) == b"a180" and set(n.files("romfs")) == {"a.bin", "Sub/b", "gone"}


def test_copy_removes_other_versions_files(tmp_path: Path) -> None:
    # a folder set up from 1.0.0, then copied from 1.8.0: only 1.8.0's files stay (#261), never in a linked folder
    import gamecopy

    def make(root: Path, names: list[str]) -> None:
        for name in names:
            (root / name).parent.mkdir(parents=True, exist_ok=True)
            (root / name).write_text(name)

    src, dst = tmp_path / ODD / "1.8.0", tmp_path / ODD / "game/romfs"
    make(src, ["UI/Product_180.sarc.zs", "Sub Dir/b"])
    make(dst, ["UI/Product_100.sarc.zs", "Old/x/y", "Sub Dir/b"])
    gamecopy.copy_tree(src, dst, 2, "1.8.0")
    assert sorted(p.relative_to(dst).as_posix() for p in dst.rglob("*")) == ["Sub Dir", "Sub Dir/b", "UI",
                                                                             "UI/Product_180.sarc.zs"]  # fmt: skip
    gamecopy.clear_dir(dst, dst / "Sub Dir/b")  # the source inside: kept
    assert (dst / "UI/Product_180.sarc.zs").is_file()


def test_copy_keeps_installed_files_when_source_scan_fails(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch,
) -> None:
    import gamecopy

    src, dst = tmp_path / "dump", tmp_path / "install"
    blocked = src / "sub"
    installed = dst / "sub/file.bin"
    blocked.mkdir(parents=True)
    installed.parent.mkdir(parents=True)
    (blocked / "file.bin").write_bytes(b"new version")
    installed.write_bytes(b"old version")
    stale = dst / "stale.bin"
    stale.write_bytes(b"stale")
    scandir = os.scandir

    def fail_subdirectory(path):
        if Path(path) == blocked:
            raise PermissionError("can't scan source subdirectory")
        return scandir(path)

    with monkeypatch.context() as fault:
        fault.setattr(os, "scandir", fail_subdirectory)
        with pytest.raises(PermissionError, match="can't scan source subdirectory"):
            gamecopy.copy_tree(src, dst, 1, "1.8.0", verify_existing=True)

    assert installed.read_bytes() == b"old version"
    assert stale.is_file()
    gamecopy.copy_tree(src, dst, 1, "1.8.0", verify_existing=True)
    assert installed.read_bytes() == b"new version"
    assert not stale.exists()


@pytest.fixture
def loose_romfs_setup(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch,
) -> tuple[Path, Callable[[str], None]]:
    game = tmp_path / "PC installation"
    game.mkdir()
    for version, data in (("1.0.0", b"old!"), ("1.8.0", b"new!")):
        src = tmp_path / version
        (src / "exefs").mkdir(parents=True)
        (src / "exefs/main").write_bytes(b"synthetic header")
        (src / "romfs").mkdir()
        for name in ("shared.bin", "second.bin", version):
            (src / "romfs" / name).write_bytes(data)

    monkeypatch.setattr(prepare, "STEPS", ("check", "exefs", "romfs"))
    monkeypatch.setattr(prepare, "own_children", lambda: None)
    monkeypatch.setattr(prepare.update, "ca_file", lambda: None)
    monkeypatch.setattr(prepare.build, "game_id", lambda: "synthetic")
    # setup.lock is held until the process ends; here every run shares this process, and a run that exits early leaves
    # its open lock file in the SystemExit traceback pytest.raises keeps, so the next run would wait for it forever
    monkeypatch.setattr(prepare, "hold_lock", lambda _game: None)

    def check(src: Path, *_: Path) -> tuple[Path, Path, SimpleNamespace]:
        return src / "exefs", src / "romfs", SimpleNamespace(name=src.name, romfs_files=3)

    monkeypatch.setattr(prepare, "check", check)

    def run(version: str) -> None:
        log = game / "logs/setup.log"
        log.parent.mkdir(exist_ok=True)
        with log.open("w", encoding="utf-8") as log_stream, monkeypatch.context() as patch:
            patch.setattr(sys, "stderr", log_stream)
            patch.setattr(sys, "argv", ["prepare.py", "--source", str(tmp_path / version), "--game", str(game),
                                        "--romfs", "copy", "--jobs", "1"])
            prepare.main()

    return game / "romfs", run


def test_loose_romfs_replaces_same_size_files_when_switching_versions(loose_romfs_setup) -> None:
    romfs, run = loose_romfs_setup
    run("1.0.0")
    run("1.8.0")

    assert (romfs / "shared.bin").read_bytes() == b"new!"
    assert not (romfs / "1.0.0").exists()
    assert (romfs / "1.8.0").exists()


@pytest.mark.parametrize("recovery", ["first-copy", "retry-switch", "return-to-old"])
def test_interrupted_loose_romfs_copy(loose_romfs_setup, monkeypatch: pytest.MonkeyPatch, recovery: str) -> None:
    romfs, run = loose_romfs_setup
    if recovery != "first-copy":
        run("1.0.0")
    copyfile = shutil.copyfile

    def interrupt(source: Path, target: Path) -> Path:
        if source.name == "second.bin":
            raise OSError("simulated interrupted copy")
        return copyfile(source, target)

    with monkeypatch.context() as patch:
        patch.setattr(shutil, "copyfile", interrupt)
        with pytest.raises(SystemExit) as failed:
            run("1.8.0")
        assert failed.value.code == 1

    shared = romfs / "shared.bin"
    assert shared.read_bytes() == b"new!"
    os.utime(shared, (1_700_000_000, 1_700_000_000))
    timestamp = shared.stat().st_mtime_ns
    if recovery == "return-to-old":
        run("1.0.0")
        assert shared.read_bytes() == (romfs / "second.bin").read_bytes() == b"old!"
        assert not (romfs / "1.8.0").exists()
    else:
        run("1.8.0")
        assert (romfs / "second.bin").read_bytes() == b"new!"
        assert shared.stat().st_mtime_ns == timestamp


def test_package_has_the_launchers_tools() -> None:
    # every script the start screen runs (setup_tool(exe_dir, "mod/import.py", ...)) is copied into the package by
    # tools/build/dist.py's SOURCES: until 13/11/2026 tools/mod was not, and the Mods page said "no setup tools"
    import ast
    import re

    tree = ast.parse((ROOT / "tools/build/dist.py").read_text(encoding="utf-8"))
    sources = next(ast.literal_eval(n.value) for n in tree.body
                   if isinstance(n, ast.Assign) and getattr(n.targets[0], "id", "") == "SOURCES")  # fmt: skip
    wanted = {"prepare/prepare.py"}
    for cpp in (ROOT / "runtime/ui/launcher").glob("*.cpp"):
        wanted |= set(re.findall(r'setup_tool\([^,]+, "([^"]+)"', cpp.read_text(encoding="utf-8")))
    assert wanted - {"prepare/prepare.py"}, "no setup_tool calls found: the pattern is stale"
    # and every tool prepare.py runs by its path (ROOT / "tools/....py"): until 18/11/2026 shaderstep.py ran
    # tools/shaders/pipeline_pairs.py, which no package carried, and every setup from main failed at its shaders
    for py in (ROOT / "tools/prepare").glob("*.py"):
        found = re.findall(r'ROOT / "tools/([^"]+\.py)"', py.read_text(encoding="utf-8"))
        wanted |= set(found)
    for rel in sorted(wanted):
        path = f"tools/{rel}"
        assert any(path == s or path.startswith(s.rstrip("/") + "/") for s in sources), f"{path} not in dist.py SOURCES"
        assert (ROOT / path).is_file(), path


def test_lastrun_reads_how_the_last_game_ended() -> None:
    # tools/prepare/lastrun.py: the start screen's notice; a wrong "known" sends a player away from a new bug
    import lastrun

    sigs = [{"issue": 22, "all": ["instruction pointer invalid"]},
            {"issue": 29, "all": ["spec.py", "args.cc.split"], "fixed": "v0.1.5"}]  # fmt: skip
    assert lastrun.verdict("x\n[host] gfx: window closed; exiting\n", "0.1.11", sigs) == "clean"
    assert lastrun.verdict("vk: device fault: Instruction pointer invalid\n", "0.1.11", sigs) == "known 22"
    assert lastrun.verdict("spec.py ... args.cc.split()\n", "0.1.4", sigs) == "fixed 29 v0.1.5"
    # on the release that fixed it, or later, the same text is a regression: still pointed to its issue
    assert lastrun.verdict("spec.py ... args.cc.split()\n", "0.1.5", sigs) == "known 29"
    assert lastrun.verdict("[host] FATAL: SIGSEGV at host 0x1\n", "0.1.11", sigs) == "crash"
    assert lastrun.verdict("[host] frame 300\n", "0.1.11", sigs) == ""   # killed or power off: nothing to say
    # a hang the player closed the window on is a crash; one that ended ("slow, not hung") before the close is clean
    hang = "vk: the GPU has not finished submission 7 in 5.0 s and the driver reports no device loss\n"
    slow = "vk: the GPU finished submission 7 after 6.2 s: it was slow, not hung\n"
    closed = "[host] gfx: window closed; exiting\n"
    assert lastrun.verdict(hang + closed, "0.1.11", sigs) == "crash"
    assert lastrun.verdict(hang + slow + closed, "0.1.11", sigs) == "clean"
    assert lastrun.verdict(slow + hang + closed, "0.1.11", sigs) == "crash"
    assert lastrun.verdict(closed + "[host] FATAL: SIGSEGV during shutdown\n", "0.1.11", sigs) == "crash"
    assert lastrun.verdict(closed + "vk: device fault: Instruction pointer invalid\n", "0.1.11", sigs) == "known 22"
    assert lastrun.verdict("[host] FATAL: earlier\n" + closed, "0.1.11", sigs) == "clean"
    assert (ROOT / "tools/github/signatures.toml").is_file()


@pytest.mark.parametrize("text", ['{"meta": {', "{}"])
def test_broken_spec_not_reused(tmp_path: Path, text: str) -> None:
    """A spec the decoder rejects is built again, not reused by every later setup (issue #325)."""
    import shaderstep  # noqa: PLC0415

    broken = tmp_path / "instructions.json"
    broken.write_text(text)
    assert not shaderstep.spec_loads(broken)
    good = ROOT / "build/sm86/instructions.json"  # built on this machine, never in the repo
    if good.is_file():
        assert shaderstep.spec_loads(good)


def test_chunk_key_ignores_generated_headers_it_does_not_include(tmp_path: Path) -> None:
    """A new patch set rewrites the generated patches.h, which no chunk includes: it recompiled all 359 (16/11/2026)."""
    gen = tmp_path / "gen"
    (gen / "main").mkdir(parents=True)
    patches, layout = gen / "patches.h", gen / "layout.h"
    patches.write_text("#define RT_PATCH_COUNT 1\n")
    layout.write_text("#define A 1\n")
    chunk, uses = gen / "main/c0.c", gen / "main/c1.c"
    chunk.write_text('#include "cpu.h"\n')
    uses.write_text('#include "cpu.h"\n#include "layout.h"\n')
    keys = lambda: [gamecode.chunk_base(["true"], "cpu", [layout, patches], s) for s in (chunk, uses)]  # noqa: E731
    before = keys()
    patches.write_text("#define RT_PATCH_COUNT 2\n")
    assert keys() == before
    layout.write_text("#define A 2\n")
    after = keys()
    assert after[0] == before[0] and after[1] != before[1]


def test_ca_file_with_an_unreadable_windows_store(monkeypatch, tmp_path):
    """Issue #456: the Windows store holds a certificate Python cannot parse, the context raises OSError (ASN1) and
    the setup died before its first step; the pip bundle is used instead."""
    pem = tmp_path / "Lib/site-packages/pip/_vendor/certifi/cacert.pem"
    pem.parent.mkdir(parents=True)
    pem.write_text("")
    monkeypatch.delenv("SSL_CERT_FILE", raising=False)
    monkeypatch.setattr(sys, "prefix", str(tmp_path))

    def broken(*args, **kwargs):
        raise OSError(-2146881269, "invalid ASN1 tag")

    monkeypatch.setattr(prepare.update.ssl, "create_default_context", broken)
    assert prepare.update.ca_file() == str(pem)


def test_redo_exefs_if_missing(tmp_path: Path) -> None:
    # #419: the exefs step was skipped by its key while game/exefs had no main, and generate stopped on the layout
    state = {"done": {"exefs": "k", "romfs": "r"}}
    prepare.redo_exefs_if_missing(state, tmp_path)
    assert state["done"] == {"romfs": "r"}
    (tmp_path / "exefs").mkdir()
    (tmp_path / "exefs/main").write_text("x")
    state = {"done": {"exefs": "k"}}
    prepare.redo_exefs_if_missing(state, tmp_path)
    assert state["done"] == {"exefs": "k"}


def test_zig_caches_inside_the_game_folder(monkeypatch, tmp_path: Path) -> None:
    # #439: in the user's cache folder zig's tmp/ to o/ rename failed with CrossDevice on one file system
    monkeypatch.delenv("ZIG_GLOBAL_CACHE_DIR", raising=False)
    monkeypatch.delenv("ZIG_LOCAL_CACHE_DIR", raising=False)
    machine.use_own_zig_cache(tmp_path / "game")
    own = str(tmp_path / "game/setup-work/zig-cache")
    assert os.environ["ZIG_GLOBAL_CACHE_DIR"] == own and os.environ["ZIG_LOCAL_CACHE_DIR"] == own


def test_zig_cache_the_players_own(monkeypatch, tmp_path: Path) -> None:
    monkeypatch.setenv("ZIG_GLOBAL_CACHE_DIR", str(tmp_path / "cache"))
    machine.use_own_zig_cache(tmp_path / "game")
    assert os.environ["ZIG_GLOBAL_CACHE_DIR"] == str(tmp_path / "cache")


@pytest.mark.skipif(not shutil.which("cc"), reason="no C compiler")
def test_damaged_generated_c_raises_to_regenerate(tmp_path: Path) -> None:
    # #434: `uint6<NUL>_t` in a generated chunk; generate was skipped by its key, so every retry failed the same
    src, out, stamp = tmp_path / "main_0203.c", tmp_path / "a.o", tmp_path / "a.key"
    src.write_bytes(b"typedef unsigned long long uint64_t;\nuint6\0_t x;\n")
    with pytest.raises(gamecode.DamagedSource, match="NUL bytes"):
        gamecode.compile_one(["cc"], [], src, out, stamp, "k")
    src.write_text("int x = ;\n")  # an ordinary error stays a message
    assert gamecode.compile_one(["cc"], [], src, out, stamp, "k").startswith("main_0203.c: ")
    src.write_text("int x = 1;\n")
    assert gamecode.compile_one(["cc"], [], src, out, stamp, "k") == "" and stamp.read_text() == "k"


def test_space_checked_before_copying(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    """gamecopy.check_space: a drive with less free than the setup needs fails with code "space" (the start screen's
    translated text) and what is needed and free; enough passes; what a cut copy left is not needed again."""
    import gamecopy

    src = tmp_path / "dump/romfs"
    src.mkdir(parents=True)
    (src / "a.bin").write_bytes(b"x" * 3000)
    (src / "b.bin").write_bytes(b"x" * 1000)
    assert gamecopy.romfs_bytes(src) == 4000 and gamecopy.romfs_bytes(tmp_path / "missing") == 0
    done = tmp_path / "game/romfs"
    done.mkdir(parents=True)
    (done / "a.bin").write_bytes(b"x" * 3000)
    assert gamecopy.romfs_bytes(src) - gamecopy.tree_bytes(done) == 1000
    monkeypatch.setattr(gamecopy.shutil, "disk_usage", lambda p: SimpleNamespace(free=2 * gamecopy.GIB))
    gamecopy.check_space(tmp_path / "game/not yet", 2 * gamecopy.GIB)
    with pytest.raises(gamecopy.Failed) as e:
        gamecopy.check_space(tmp_path / "game", 5 * gamecopy.GIB)
    assert e.value.code == "space" and e.value.detail.startswith("5.0 GB, 2.0 GB free in ")


def test_keys_ignore_files_the_package_does_not_ship(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    # a package unzipped by hand over an older one keeps the files it moved (0.1.13's sm86/translate/flow.py): only
    # the listed ones count, so the update does not redo the shaders
    import shaderstep  # noqa: PLC0415

    (tmp_path / "a.py").write_text("new")
    (tmp_path / "stale.py").write_text("old")
    monkeypatch.setattr(shaderstep, "ROOT", tmp_path)
    monkeypatch.setattr(shaderstep, "SHIPPED", tmp_path / "FILES")
    files = [tmp_path / "a.py", tmp_path / "stale.py"]
    assert shaderstep.shipped(files) == files  # a checkout: no list
    (tmp_path / "FILES").write_text("a.py\n")
    assert shaderstep.shipped(files) == [tmp_path / "a.py"]


def test_shipped_follows_the_appimage_setup_symlink(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    # #632: the AppImage's setup/ is a symlink into its mount and ROOT is resolved; a file named through the symlink
    # (prepare.py run as <game>/setup/src/tools/prepare/prepare.py) stopped the setup at check with a ValueError
    import shaderstep  # noqa: PLC0415

    mount = tmp_path / "mount/setup/src"
    mount.mkdir(parents=True)
    (mount / "a.py").write_text("x")
    (mount / "FILES").write_text("a.py\n")
    (tmp_path / "game").mkdir()
    (tmp_path / "game/setup").symlink_to(tmp_path / "mount/setup")
    monkeypatch.setattr(shaderstep, "ROOT", mount.resolve())
    monkeypatch.setattr(shaderstep, "SHIPPED", mount / "FILES")
    via_link = tmp_path / "game/setup/src/a.py"
    assert shaderstep.shipped([via_link]) == [via_link]


@pytest.mark.parametrize("version,index,runs", [(3, True, []), (2, True, ["pipeline_pairs.py"]),
                                                (2, False, ["extract.py", "pipeline_pairs.py"])])  # fmt: skip
def test_ensure_pairs(tmp_path: Path, monkeypatch, version: int, index: bool, runs: list[str]) -> None:
    """The warm step's pipeline-pairs.bin: one of the package's format kept; another version's written again (an
    update whose shaders key did not change kept a v2 file the v3 runtime rejected), with extract.py first when
    setup-work/shaders is gone."""
    import shaderstep

    dest, work = tmp_path / "game", tmp_path / "work"
    (work / "shaders").mkdir(parents=True)
    dest.mkdir()
    if index:
        (work / "shaders/index.csv").write_text("sha1,stage\n")
    (dest / "pipeline-pairs.bin").write_bytes(shaderstep.pipeline_pairs.MAGIC + version.to_bytes(4, "little") + bytes(4))
    calls: list[str] = []
    monkeypatch.setattr(shaderstep, "run", lambda cmd, **_: calls.append(Path(cmd[1]).name))
    shaderstep.ensure_pairs(tmp_path / "exefs", tmp_path / "romfs", work, dest)
    assert shaderstep.PAIRS_VERSION == 3 and calls == runs
