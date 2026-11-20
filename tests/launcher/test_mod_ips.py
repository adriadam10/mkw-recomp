"""tools/mod/ips.py's readers of a mod's exefs patches: good files parse, a damaged one is a PatchFailed that the import
reports, never a traceback (import.py catches only PatchFailed and OSError).

Usage: uv run pytest tests/launcher/test_mod_ips.py
"""

import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/mod"))
import ips  # noqa: E402


def record(offset: int, data: bytes, width: int = 3) -> bytes:
    return offset.to_bytes(width, "big") + len(data).to_bytes(2, "big") + data


def test_ips_records_and_rle() -> None:
    patch = b"PATCH" + record(0x100, b"\x1f\x20\x03\xd5") + (0x200).to_bytes(3, "big") + b"\x00\x00\x00\x03A" + b"EOF"
    assert ips.parse_ips(patch) == [(0x100, b"\x1f\x20\x03\xd5"), (0x200, b"AAA")]


def test_ips32_records() -> None:
    assert ips.parse_ips(b"IPS32" + record(0x1_0000_00, b"ab", 4) + b"EEOF") == [(0x1_0000_00, b"ab")]


@pytest.mark.parametrize(
    "tail",
    [
        b"\x00\x00",  # an RLE record cut before its count
        b"\x00\x00\x00",  # cut inside the count
        b"\x00\x00\x00\x03",  # cut before the byte
        b"\x00\x09ab",  # data shorter than its size
        b"\x00\x02ab",  # no end mark
    ],
)
def test_damaged_ips_is_a_patch_failure(tail: bytes) -> None:
    with pytest.raises(ips.PatchFailed):
        ips.parse_ips(b"PATCH" + (0x100).to_bytes(3, "big") + tail)


def test_not_an_ips_is_a_patch_failure() -> None:
    with pytest.raises(ips.PatchFailed):
        ips.parse_ips(b"nothing")


def test_pchtxt_applies_the_offset_shift_to_enabled_lines() -> None:
    text = "@nsobid-AB12\n@flag offset_shift 0x100\n@enabled\n100 1F2003D5 // nop\n@disabled\n200 00\n@stop\n"
    assert ips.parse_pchtxt(text) == ("ab12", [(0x200, b"\x1f\x20\x03\xd5")])


@pytest.mark.parametrize("line", ["@flag offset_shift zz", "@flag offset_shift", "100 1F2", "not a record"])
def test_damaged_pchtxt_is_a_patch_failure(line: str) -> None:
    with pytest.raises(ips.PatchFailed):
        ips.parse_pchtxt(f"@nsobid-AB12\n@enabled\n{line}\n")
