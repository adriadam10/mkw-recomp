"""tools/i18n/game_fonts.py: the bfotf wrapping of the game's fonts, which the setup undoes and redoes around the letters
it composes (a wrong key or header gives the game a font it cannot read)."""

import struct
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools/i18n"))
import game_fonts  # noqa: E402


def test_round_trip_and_header() -> None:
    font = b"OTTO" + bytes(range(256)) * 3 + b"\x01"
    data = game_fonts.encrypt(font)
    assert data[:4] == game_fonts.MAGIC
    # the font's size, big-endian, XORed with the key as it falls at offset 4
    assert struct.unpack(">I", bytes(a ^ b for a, b in zip(data[4:8], game_fonts.KEY)))[0] == len(font)
    assert game_fonts.decrypt(data) == font


def test_padding_after_the_font_is_ignored() -> None:
    font = b"OTTO" + b"\x55" * 101
    assert game_fonts.decrypt(game_fonts.encrypt(font) + b"\0") == font


def test_truncated_file_stops() -> None:
    with pytest.raises(ValueError):
        game_fonts.decrypt(game_fonts.encrypt(b"OTTO" * 10)[:-1])
