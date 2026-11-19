"""translate.py --base: an update's pass beside the played version's spirv/ (tools/prepare/shaderstep.py, --next).

Usage: uv run pytest tests/translator/test_translate_base.py
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/shaders"))
import translate  # noqa: E402


def test_link_base_links_a_program_and_a_rewrite_leaves_the_played_file(tmp_path: Path) -> None:
    base, out = tmp_path / "played", tmp_path / "next"
    base.mkdir(), out.mkdir()
    (base / "k.spv").write_bytes(b"old spv")
    (base / "k.done").write_text("th\nok\nsum")
    (base / "other.spv").write_bytes(b"another program")
    translate.BASE = str(base)
    translate.link_base("k", str(out))
    assert sorted(p.name for p in out.iterdir()) == ["k.done", "k.spv"]  # only this program's, none missing
    translate.put(out / "k.spv", b"new spv")
    assert (base / "k.spv").read_bytes() == b"old spv" and (out / "k.spv").read_bytes() == b"new spv"
    translate.link_base("k", str(out))  # a resumed pass: what is there stays
    assert (out / "k.spv").read_bytes() == b"new spv"
