"""tools/shaders/pack.py: spirv/shaders.pack, the one file the boot prewarm reads every program from.

Usage: uv run pytest tests/translator/test_pack.py
"""

import os
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/shaders"))
import pack  # noqa: E402


def test_pack_indexes_each_program_by_key_and_a_later_write_makes_it_stale(tmp_path: Path) -> None:
    a, b = "aa" * 32, "0b" * 32
    (tmp_path / f"{a}.res.json").write_text('{"stage": "vs"}')
    (tmp_path / f"{a}.spv").write_bytes(b"\x03\x02\x23\x07spv a")
    (tmp_path / f"{b}.res.json").write_text('{"stage": "cs"}')
    (tmp_path / f"{b}.spv").write_bytes(b"spv b")
    (tmp_path / f"{b}.cs.spv").write_bytes(b"cs b")
    (tmp_path / f"{b}.done").write_text("not packed")
    assert pack.pack(tmp_path)[0] == 2 and pack.current(tmp_path)
    data = (tmp_path / pack.NAME).read_bytes()
    assert data[:8] == pack.MAGIC and struct.unpack_from("<II", data, 8) == (pack.VERSION, 2)
    entries = [pack.ENTRY.unpack_from(data, 16 + i * pack.ENTRY.size) for i in range(2)]
    assert [e[0].hex() for e in entries] == [b, a]  # sorted by key bytes, the runtime's bsearch
    key, *spans = entries[0]
    files = [data[off : off + n] for off, n in zip(spans[::2], spans[1::2])]
    assert files == [b"spv b", b'{"stage": "cs"}', b"cs b"]
    assert entries[1][5:7] == (0, 0)  # a program without a .cs.spv
    st = (tmp_path / pack.NAME).stat()
    os.utime(tmp_path / pack.NAME, ns=(st.st_atime_ns, st.st_mtime_ns - 10**9))  # a later pass wrote the folder
    assert not pack.current(tmp_path)
