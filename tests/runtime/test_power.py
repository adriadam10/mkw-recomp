"""Xbox pads on Linux's xone driver (runtime/input/linux_power.c), built with the C compiler at hand: the driver exposes
each pad's battery as a power_supply device with a level band but no percentage (UPower marks its synthesized
percentage "should be ignored"), which SDL leaves UNKNOWN. The lookup reads the band and shows High/Medium/Low.

Usage: uv run pytest tests/runtime/test_power.py
"""

import os
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
# INPUT_POWER_* (runtime/input/input.h): NONE 0, UNKNOWN 1, BATTERY 2, CHARGING 3, CHARGED 4, WIRED 5.
BATTERY, CHARGING, CHARGED, WIRED = 2, 3, 4, 5


@pytest.fixture(scope="module")
def power(tmp_path_factory: pytest.TempPathFactory) -> Path:
    if os.name == "nt":
        pytest.skip("sysfs batteries are Linux only")
    cc = shutil.which("clang") or shutil.which("cc") or shutil.which("gcc")
    if not cc:
        pytest.skip("no C compiler")
    exe = tmp_path_factory.mktemp("power") / "t_power.exe"
    subprocess.run([cc, "-O1", "-Wall", "-Wextra", f"-I{ROOT / 'runtime/input'}",
                    str(ROOT / "tests/runtime/t_power.c"), str(ROOT / "runtime/input/linux_power.c"),
                    "-o", str(exe)], check=True)
    return exe


def supply(root: Path, name: str, files: dict[str, str]) -> None:
    d = root / name
    d.mkdir(parents=True, exist_ok=True)
    for file, text in files.items():
        (d / file).write_text(text + "\n", encoding="utf-8")  # sysfs values end in a newline


def xone(level: str, status: str = "Discharging") -> dict[str, str]:
    return {"type": "Battery", "status": status, "capacity_level": level, "scope": "Device",
            "model_name": "Microsoft Xbox Controller"}


def run(exe: Path, root: Path, index: int, name: str) -> tuple[int, int, int, int]:
    out = subprocess.run([str(exe), str(root), str(index), name], capture_output=True, text=True, check=True)
    return tuple(int(x) for x in out.stdout.split())  # found, state, percent, coarse


def test_high_band_shows_high_not_a_percent(power: Path, tmp_path: Path) -> None:
    supply(tmp_path, "gip0.0", xone("High") | {"capacity": "70"})  # a stray percent file is ignored
    assert run(power, tmp_path, 0, "Xbox Series X Controller") == (1, BATTERY, 90, 1)


def test_bands_map_to_low_medium_high(power: Path, tmp_path: Path) -> None:
    supply(tmp_path, "gip0.0", xone("Critical"))
    assert run(power, tmp_path, 0, "Xbox Series X Controller") == (1, BATTERY, 5, 1)
    supply(tmp_path, "gip0.0", xone("Low"))
    assert run(power, tmp_path, 0, "Xbox Series X Controller") == (1, BATTERY, 10, 1)
    supply(tmp_path, "gip0.0", xone("Normal"))
    assert run(power, tmp_path, 0, "Xbox Series X Controller") == (1, BATTERY, 50, 1)


def test_charging_and_full(power: Path, tmp_path: Path) -> None:
    supply(tmp_path, "gip0.0", xone("Normal", "Charging"))
    assert run(power, tmp_path, 0, "Xbox Series X Controller") == (1, CHARGING, 50, 1)
    supply(tmp_path, "gip0.0", xone("Full", "Full"))
    assert run(power, tmp_path, 0, "Xbox Series X Controller") == (1, CHARGED, 100, 1)


def test_no_battery_pack_is_wired(power: Path, tmp_path: Path) -> None:
    supply(tmp_path, "gip0.0", xone("Unknown", "Not charging"))
    assert run(power, tmp_path, 0, "Xbox Series X Controller") == (1, WIRED, -1, 0)


def test_non_xbox_pad_never_takes_an_xbox_level(power: Path, tmp_path: Path) -> None:
    supply(tmp_path, "gip0.0", xone("High"))
    assert run(power, tmp_path, 0, "DualSense Wireless Controller")[0] == 0


def test_second_pad_takes_the_second_supply(power: Path, tmp_path: Path) -> None:
    supply(tmp_path, "gip0.0", xone("High"))
    supply(tmp_path, "gip0.1", xone("Low"))
    assert run(power, tmp_path, 1, "Xbox Series X Controller") == (1, BATTERY, 10, 1)
    assert run(power, tmp_path, 2, "Xbox Series X Controller")[0] == 0


def test_laptop_battery_and_empty_tree_are_ignored(power: Path, tmp_path: Path) -> None:
    empty = tmp_path / "empty"
    empty.mkdir()
    assert run(power, empty, 0, "Xbox Series X Controller")[0] == 0
    supply(tmp_path, "BAT0", {"type": "Battery", "status": "Discharging", "capacity": "42", "scope": "System",
                              "model_name": "Laptop Battery"})
    supply(tmp_path, "gip0.0", xone("Normal"))
    assert run(power, tmp_path, 0, "Xbox Series X Controller") == (1, BATTERY, 50, 1)


def test_model_match_without_gip_name(power: Path, tmp_path: Path) -> None:
    supply(tmp_path, "hid-11-22-33", {"type": "Battery", "status": "Discharging", "capacity_level": "Normal",
                                      "scope": "Device", "model_name": "Xbox Wireless Controller"})
    assert run(power, tmp_path, 0, "Xbox Wireless Controller") == (1, BATTERY, 50, 1)


def test_xbox_gate(power: Path) -> None:
    def isxbox(name: str) -> int:
        out = subprocess.run([str(power), "isxbox", name], capture_output=True, text=True, check=True)
        return int(out.stdout)

    assert isxbox("Xbox Series X Controller") == 1
    assert isxbox("Microsoft Xbox Controller") == 1
    assert isxbox("") == 1  # a name that tells nothing passes, as in the lookup
    assert isxbox("DualSense Wireless Controller") == 0
    assert isxbox("Switch Pro Controller") == 0
