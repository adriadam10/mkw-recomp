"""One SM86 program as the translator sees it: its program header (SPH) and its decoded instructions.

Graphics stages start with a 0x80-byte SPH (Maxwell layout, kept by Ampere); compute programs are bare code. The code
ends at the first all-zero word or at data placed after it (tools/shaders/extract.py cuts it). SPH fields follow
Ryujinx's ShaderHeader.cs (MIT): word 0 type/version/stage, PS input map words 6..13 (2 bits per attribute
component), PS output targets word 18 (4 enable bits per render target), sample mask and depth word 19.
"""

import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "decode"))

import sass_decode
import sass_spec

SPH_SIZE = 0x80
STAGES = {1: "vs", 2: "tcs", 3: "tes", 4: "gs", 5: "ps"}


@dataclass
class Sph:
    stage: str
    words: tuple[int, ...]

    @property
    def omap_targets(self) -> int:
        """PS: 4 bits (RGBA) per render target, 8 targets."""
        return self.words[18]

    @property
    def omap_depth(self) -> bool:
        return bool(self.words[19] >> 1 & 1)

    def imap_used(self, attr: int, comp: int) -> bool:
        """PS: generic input attr (0..31) component comp read (any interpolation mode)."""
        bit = (attr * 4 + comp) * 2
        return bool(self.words[6 + bit // 32] >> (bit % 32) & 3)


@dataclass
class Insn:
    pc: int
    word: int
    d: object  # sass_decode.Decoded
    f: dict = field(default_factory=dict)  # item name -> ItemValue

    @property
    def op(self) -> str:
        return self.d.mnemonic

    @property
    def cls(self) -> str:
        return self.d.cls.name

    def v(self, name: str, default: int | None = None) -> int:
        iv = self.f.get(name)
        if iv is None:
            if default is None:
                raise KeyError(f"{self.cls}: no field {name}")
            return default
        return iv.value

    def sv(self, name: str) -> int:
        """A field as a signed integer of its encoded width (branch offsets)."""
        iv = self.f[name]
        width = sum(slot.width for slot, _ in iv.slot_values)
        v = iv.value
        return v - (1 << width) if v >> (width - 1) & 1 else v

    def sym(self, name: str) -> str:
        iv = self.f.get(name)
        return iv.symbols[0] if iv is not None and iv.symbols else ""

    def has(self, name: str) -> bool:
        return name in self.f


CACHE_WORDS = 20_000  # per process (~2 KB each: 200 000 x 16 processes ran out of 6.7 GB); cleared when full


class Decoder:
    def __init__(self, spec_path: Path) -> None:
        self.spec = sass_spec.Spec(str(spec_path))
        self.cache: dict[int, object] = {}  # word -> decoded: 1.8 M distinct words in 4.7 M (30/10/2026)

    def program(self, code: bytes, stage: str) -> tuple[Sph | None, list[Insn]]:
        sph = None
        if stage != "cs":
            words = struct.unpack_from("<32I", code)
            sph = Sph(STAGES.get((words[0] >> 10) & 0xF, stage), words)
            code = code[SPH_SIZE:]
        out = []
        for pc in range(0, len(code) - 15, 16):
            word = int.from_bytes(code[pc : pc + 16], "little")
            d = self.cache.get(word)
            if d is None:
                d = sass_decode.decode(self.spec, word)
                # the fields by name once per word, not per instruction (the translator only reads them)
                d.fields = {getattr(v.item, "name", str(v.item)): v for v in d.values}
                if len(self.cache) >= CACHE_WORDS:
                    self.cache.clear()
                self.cache[word] = d
            if d.cls is None:
                break  # data after the code (constants), never executed
            out.append(Insn(pc, word, d, d.fields))
        return sph, out
