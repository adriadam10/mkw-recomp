"""The Tools page's Add to Steam (tools/prepare/steam.py): the binary shortcuts.vdf read back as it was written, an
existing shortcut kept with its 64-bit field and tags, and an entry with the same executable replaced instead of
doubled. Seconds, on Linux and on Windows (ci.yml): an entry Steam reads and this script does not would drop the
player's own shortcuts from the file.

Usage: uv run pytest tests
"""

import os
import struct
import sys
import zlib
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/prepare"))
import steam  # noqa: E402

NAME = "Mario Kart World"
EXE = "/games/mkw/mkw"


def vdf_with(rows: list[dict]) -> bytes:
    """The file steam.py writes: the shortcuts map, its rows by index."""
    return b"\x00shortcuts\x00" + steam.write_map({str(i): r for i, r in enumerate(rows)}) + b"\x08"


def row(appname: str, exe: str) -> dict:
    return {
        "appid": -1234567890, "AppName": appname, "Exe": f'"{exe}"', "StartDir": '"/games"', "icon": "",
        "ShortcutPath": "", "LaunchOptions": "", "IsHidden": 0, "AllowDesktopConfig": 1, "AllowOverlay": 1,
        "OpenVR": 0, "Devkit": 0, "DevkitGameID": "", "DevkitOverrideAppID": 0, "LastPlayTime": 1700000000,
        "FlatpakAppID": "", "tags": {"1": "played"},
    }  # fmt: skip


def test_write_reads_back(tmp_path: Path) -> None:
    # every type the format has: a string, two ints, a 64-bit one and a map of tags
    rows = [row("Older game", "/games/old/game")]
    rows[0]["u64field"] = ("u64", 18446744073709551615)
    p = tmp_path / "shortcuts.vdf"
    p.write_bytes(vdf_with(rows))
    got = steam.read_map(p.read_bytes(), 0)[0]["shortcuts"]
    assert [v for v in got.values()] == rows


def test_add_keeps_the_others_replaces_the_same_exe(tmp_path: Path) -> None:
    p = tmp_path / "shortcuts.vdf"
    others = [row("Older game", "/games/old/game"), row("Same exe", EXE)]
    others[0]["u64field"] = ("u64", 42)
    p.write_bytes(vdf_with(others))

    steam.add(p, NAME, EXE, "/games/mkw")
    got = list(steam.read_map(p.read_bytes(), 0)[0]["shortcuts"].values())

    names = [r["AppName"] for r in got]
    assert names == ["Older game", NAME]           # the same executable's row replaced, not doubled
    assert got[0]["u64field"] == ("u64", 42)        # an entry we do not write ourselves is kept whole
    assert got[0]["tags"] == {"1": "played"}
    want = zlib.crc32((f'"{EXE}"' + NAME).encode()) | 0x80000000
    assert struct.unpack("<i", struct.pack("<I", want))[0] == got[1]["appid"]   # Steam's own rule for a shortcut
    assert got[1]["Exe"] == f'"{EXE}"' and got[1]["StartDir"] == '"/games/mkw"'

    steam.add(p, NAME, EXE, "/games/mkw")          # a second add is not a second entry either
    assert len(steam.read_map(p.read_bytes(), 0)[0]["shortcuts"]) == 2


def test_add_creates_the_file(tmp_path: Path) -> None:
    p = tmp_path / "config" / "shortcuts.vdf"
    p.parent.mkdir()
    steam.add(p, NAME, EXE, "/games/mkw")
    got = list(steam.read_map(p.read_bytes(), 0)[0]["shortcuts"].values())
    assert [r["AppName"] for r in got] == [NAME]


@pytest.mark.parametrize(
    "data",
    [b"\x00shortcuts\x00", b"\x00shortcuts\x00\x02appid\x00\x01"],
    ids=["truncated-map", "truncated-integer"],
)
def test_add_reports_truncated_shortcuts(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str], data: bytes,
) -> None:
    config = tmp_path / "userdata/123/config"
    config.mkdir(parents=True)
    vdf = config / "shortcuts.vdf"
    vdf.write_bytes(data)
    monkeypatch.setattr(steam, "steam_dir", lambda: tmp_path)
    monkeypatch.setattr(steam, "write_art", lambda *_: config / "icon.png")
    monkeypatch.setattr(sys, "argv", ["steam.py", "add", "--name", NAME, "--exe", EXE, "--dir", "/games/mkw"])

    with pytest.raises(SystemExit) as result:
        steam.main()

    assert result.value.code == 1
    assert capsys.readouterr().out == "error io shortcuts.vdf contains incomplete or invalid Steam shortcut data\n"
    assert vdf.read_bytes() == data


@pytest.mark.parametrize("userdata_kind", ["missing", "file", "empty", "denied"])
def test_add_reports_unavailable_profiles(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str], userdata_kind: str,
) -> None:
    userdata = tmp_path / "userdata"
    if userdata_kind == "file":
        userdata.touch()
    elif userdata_kind in ("empty", "denied"):
        userdata.mkdir()
    if userdata_kind == "denied":
        iterdir = Path.iterdir

        def deny_userdata(path):
            if path == userdata:
                raise PermissionError("userdata access denied")
            return iterdir(path)

        monkeypatch.setattr(Path, "iterdir", deny_userdata)
    monkeypatch.setattr(steam, "steam_dir", lambda: tmp_path)
    monkeypatch.setattr(sys, "argv", ["steam.py", "add", "--name", NAME, "--exe", EXE, "--dir", "/games/mkw"])

    with pytest.raises(SystemExit) as exited:
        steam.main()

    assert exited.value.code == 1
    want = "error io userdata access denied\n" if userdata_kind == "denied" else "error nosteam\n"
    assert capsys.readouterr().out == want


def test_add_uses_only_profile_directories(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str],
) -> None:
    userdata = tmp_path / "userdata"
    for profile in ("0", "123", "other"):
        (userdata / profile).mkdir(parents=True)
    (userdata / "456").touch()
    monkeypatch.setattr(steam, "steam_dir", lambda: tmp_path)
    monkeypatch.setattr(steam, "steam_running", lambda _: False)
    monkeypatch.setattr(steam, "write_art", lambda grid, *_: grid / "icon.png")
    monkeypatch.setattr(sys, "argv", ["steam.py", "add", "--name", NAME, "--exe", EXE, "--dir", "/games/mkw"])

    steam.main()

    assert capsys.readouterr().out == "done 1 0\n"
    assert list(userdata.rglob("shortcuts.vdf")) == [userdata / "123/config/shortcuts.vdf"]


def test_no_steam_pid_is_not_running(tmp_path: Path) -> None:
    # a pid file that is not there (or is not a number) reads as Steam closed, never as an error
    if os.name == "nt":
        pytest.skip("the Windows branch reads the registry instead")
    assert steam.steam_running(tmp_path / "steam") is False
    (tmp_path / "steam.pid").write_text("not a pid\n")
    assert steam.steam_running(tmp_path / "steam") is False


def test_library_art_written_once_and_named_as_the_icon(tmp_path: Path) -> None:
    """Gaming Mode's images (steam.write_art): the five files as PNGs of their sizes, a file already there (the
    player's own) kept, and the shortcut's icon the drawn one, since Steam takes none from a Linux executable."""
    grid, appid = tmp_path / "grid", 2675230854
    grid.mkdir()
    (grid / f"{appid}_hero.png").write_bytes(b"the player's own")
    icon = steam.write_art(grid, appid)
    sizes = {f"{appid}p.png": (600, 900), f"{appid}.png": (920, 430), f"{appid}_logo.png": (320, 320),
             f"{appid}_icon.png": (320, 320)}  # fmt: skip
    for name, (w, h) in sizes.items():
        data = (grid / name).read_bytes()
        assert data[:8] == b"\x89PNG\r\n\x1a\n" and struct.unpack(">II", data[16:24]) == (w, h), name
    assert (grid / f"{appid}_hero.png").read_bytes() == b"the player's own"
    p = tmp_path / "shortcuts.vdf"
    steam.add(p, NAME, EXE, "/games/mkw", str(icon))
    row = steam.read_map(p.read_bytes(), 0)[0]["shortcuts"]["0"]
    assert row["icon"] == str(icon)


def test_art_from_the_start_screen_replaces_ours_only(tmp_path: Path) -> None:
    """--art (launcher.cpp steam_art_draw): its files are used; a later run replaces what this script wrote, never an
    image the player put there after."""
    grid, art, appid = tmp_path / "grid", tmp_path / "art", 2675230854
    art.mkdir()
    for src in steam.ART.values():
        (art / src).write_bytes(b"first " + src.encode())
    steam.write_art(grid, appid, art)
    assert (grid / f"{appid}p.png").read_bytes() == b"first portrait.png"
    (grid / f"{appid}_hero.png").write_bytes(b"the player's own")
    for src in steam.ART.values():
        (art / src).write_bytes(b"second " + src.encode())
    steam.write_art(grid, appid, art)
    assert (grid / f"{appid}p.png").read_bytes() == b"second portrait.png"
    assert (grid / f"{appid}_hero.png").read_bytes() == b"the player's own"


def test_a_second_path_replaces_our_shortcut_not_the_players(tmp_path: Path) -> None:
    """Added again from another program path (the launcher's button after a script, a moved AppImage): the shortcut
    this script added before is replaced, one Mario Kart World stays; a player's own shortcut of that name stays too."""
    vdf = tmp_path / "config/shortcuts.vdf"
    vdf.parent.mkdir()
    steam.add(vdf, NAME, "/games/old/mkw", "/games/old")
    old = zlib.crc32(('"/games/old/mkw"' + NAME).encode()) | 0x80000000
    steam.write_art(tmp_path / "config/grid", old)   # the record that marks it as ours
    rows = steam.read_map(vdf.read_bytes(), 0)[0]["shortcuts"]
    rows["1"] = {"appid": -5, "appname": NAME, "Exe": '"/home/me/my-own-mkw.sh"'}   # the player's own, as Steam writes it
    vdf.write_bytes(b"\x00shortcuts\x00" + steam.write_map(rows) + b"\x08")
    steam.add(vdf, NAME, "/games/new/mkw", "/games/new")
    got = [r.get("Exe", r.get("exe")) for r in steam.read_map(vdf.read_bytes(), 0)[0]["shortcuts"].values()]
    assert got == ['"/home/me/my-own-mkw.sh"', '"/games/new/mkw"']
