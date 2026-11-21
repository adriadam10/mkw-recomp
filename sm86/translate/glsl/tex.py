"""SM86 -> GLSL, glsl.core.Builder methods for textures and images. A mixin: it uses the Builder's operand helpers (src_u,
dst...) and state."""

from typing import ClassVar

from program import Insn

from glsl.common import PT, RZ, Unsupported, reg


class TexOps:
    def txq(self, i: Insn) -> str:
        """TXQ TEX_HEADER_DIMENSION: width, height, depth/layers, levels of the texture at lod Ra."""
        if i.sym("query") != "TEX_HEADER_DIMENSION":
            raise Unsupported(f"TXQ {i.sym('query')}")
        key = (i.v("bank5"), i.v("tsPtrIdx"))
        self.textures.setdefault(key, "sampler2D")
        t = f"t{key[0]}_{key[1]}"
        # the sampler's type is final only after the whole program (a later TEX may make it an array): the size comes
        # from txqsz_<bank>_<idx>, written with the declarations, as an ivec3 padded with 1
        self.txq_tex.add(key)
        size = f"txqsz_{key[0]}_{key[1]}(int({self.src_u(i, 'a')}))"
        comps = [f"uint(sz[{k}])" for k in range(3)] + [f"uint(textureQueryLevels({t}))"]
        rd, rd2 = i.v("Rd"), i.v("Rd2")
        dests = [rd, rd + 1 if rd != RZ else RZ, rd2, rd2 + 1 if rd2 != RZ else RZ]
        outs, comp, mask = [], 0, i.v("wmsk", 0xF)
        for c in range(4):
            if mask >> c & 1:
                d = dests[comp]
                comp += 1
                if d != RZ:
                    self.regs.add(d)
                    outs.append(f"r{d} = {comps[c]};")
        return f"{{ ivec3 sz = {size}; {' '.join(outs)} }}" if outs else ""

    # Images: bound like textures (handle at c[bank5][tsPtrIdx * 4], named i<bank>_<idx>), declared without a format
    # (shaderStorageImageRead/WriteWithoutFormat) and as float images (hypothesis: an integer image would need its
    # format from the descriptor). Coordinates are integers from Ra, the array layer after the coordinates.
    IMG: ClassVar[dict[str, tuple[str, int]]] = {"1D": ("1D", 1), "2D": ("2D", 2), "3D": ("3D", 3),
                                                 "1D_ARRAY": ("1DArray", 2), "2D_ARRAY": ("2DArray", 3)}  # fmt: skip

    def image(self, i: Insn) -> tuple[str, str, int]:
        dim = i.sym("dim")
        if dim not in self.IMG or i.sym("clamp") not in ("IGN", ""):
            raise Unsupported(f"{i.op} {dim} {i.sym('clamp')}")
        name, n = self.IMG[dim]
        a = i.v("Ra")
        if a != RZ:
            self.regs.update(range(a, a + n))
        coords = [f"int({reg(a + k if a != RZ else RZ)})" for k in range(n)]
        vec = coords[0] if n == 1 else f"ivec{n}({', '.join(coords)})"
        key = (i.v("bank5"), i.v("tsPtrIdx"))
        kind = "uimage" if i.op in ("SURED", "SUATOM") else "image"  # atomics need an integer image with a declared format
        if self.images.get(key, f"{kind}{name}") != f"{kind}{name}":
            raise Unsupported(f"{i.op}: image {key} used both as float and for atomics")
        self.images[key] = f"{kind}{name}"
        return f"i{key[0]}_{key[1]}", vec, {"R": 1, "RG": 2, "RGBA": 4}.get(i.sym("rgba"), 0)

    def sust(self, i: Insn) -> str:
        img, vec, n = self.image(i)
        if not n:
            raise Unsupported(f"SUST.{i.sym('rgba')}")
        b = i.v("Rb")
        vals = [f"uintBitsToFloat({reg(b + k if b != RZ else RZ)})" for k in range(n)] + ["0.0"] * (4 - n)
        if b != RZ:
            self.regs.update(range(b, b + n))
        return f"imageStore({img}, {vec}, vec4({', '.join(vals)}));"

    IMG_ATOM: ClassVar[dict[str, str]] = {"ADD": "imageAtomicAdd", "MIN": "imageAtomicMin", "MAX": "imageAtomicMax",
                                          "AND": "imageAtomicAnd", "OR": "imageAtomicOr", "XOR": "imageAtomicXor"}  # fmt: skip

    def sured(self, i: Insn) -> str:
        """SURED: an atomic on a 32-bit texel with no result (Ryujinx SuRed); the image as r32ui (hypothesis: the
        texture is a 32-bit integer one; a float one would need its bits)."""
        op = self.IMG_ATOM.get(i.sym("op"))
        if op is None or i.sym("sz") not in ("32", "U32"):
            raise Unsupported(f"{i.op}.{i.sym('op')}.{i.sym('sz')}")
        img, vec, _ = self.image(i)
        b = i.v("Rb")
        if b != RZ:
            self.regs.add(b)
        return f"{op}({img}, {vec}, {reg(b)});"

    def suatom(self, i: Insn) -> str:
        """SUATOM: SURED that also writes the texel's old value to Rd (Ryujinx SuAtom), 32-bit only."""
        if i.v("Pu", PT) != PT:
            raise Unsupported("SUATOM with predicate out")
        call = self.sured(i).removesuffix(";")
        d = i.v("Rd")
        if d == RZ:
            return f"{call};"
        self.regs.add(d)
        return f"r{d} = {call};"

    def suld(self, i: Insn) -> str:
        if i.v("Pu", PT) != PT:
            raise Unsupported("SULD with predicate")
        img, vec, n = self.image(i)
        if not n:
            raise Unsupported(f"SULD.{i.sym('rgba')}")
        d, out = i.v("Rd"), []
        for k in range(n):
            if d != RZ:
                self.regs.add(d + k)
                out.append(f"r{d + k} = floatBitsToUint(iv[{k}]);")
        return f"{{ vec4 iv = imageLoad({img}, {vec}); {' '.join(out)} }}" if out else ""

    # ---- textures
    DIMS: ClassVar[dict[str, tuple[str, int, bool]]] = {
        "1D": ("1D", 1, False), "2D": ("2D", 2, False), "3D": ("3D", 3, False), "CUBE": ("Cube", 3, False),
        "ARRAY_1D": ("1DArray", 1, True), "ARRAY_2D": ("2DArray", 2, True), "ARRAY_CUBE": ("CubeArray", 3, True),
    }  # fmt: skip

    def tex(self, i: Insn) -> str:
        """TEX on a bound texture (handle at c[bank5][tsPtrIdx * 4], named t<bank>_<idx>). Source registers as Mesa
        NAK's nak_nir_lower_tex.c (MIT) packs them for SM70+: group 0 = [array index (u32)] + coordinates in Ra...,
        group 1 = [lod or bias] + [depth reference] in Rb...; the scalar form (.SCR) puts the whole list (at most 4)
        as 1 -> Ra; 2 -> Ra, Rb; 3 -> Ra, Ra+1, Rb; 4 -> Ra, Ra+1, Rb, Rb+1. For .SCR with a lod NAK has no case; the
        same split is used (hypothesis until checked on a picture). Results: the components set in wmsk, in order, to
        Rd, Rd+1, Rd2, Rd2+1."""
        dim = i.sym("paramA")
        aoffi = i.sym("aoffi") == "AOFFI"
        gather = i.op == "TLD4"
        ptp = gather and i.sym("toff") == "PTP"
        if dim not in self.DIMS or i.sym("toff") not in ("notoff", "", "PTP" if gather else "") or (
            (aoffi or ptp) and dim.startswith("CUBE")
        ):
            raise Unsupported(f"{i.op} {dim} {i.sym('aoffi')}{i.sym('toff')}")
        name, ncoord, is_array = self.DIMS[dim]
        lod, dc = i.sym("lodlc"), i.sym("dc") == "DC"
        g0 = (["idx"] if is_array else []) + [f"c{k}" for k in range(ncoord)]
        # the second group: lod or bias, the packed offsets (AOFFI), the depth reference (Ryujinx's TEX order)
        g1 = (["lod"] if lod in ("LL", "LB") else []) + (["off"] if aoffi else []) + (["p0", "p1"] if ptp else [])
        g1 += ["ref"] if dc else []
        regs: dict[str, str] = {}
        a, b = i.v("Ra"), i.v("Rb")
        if i.sym("scr") == "SCR":
            seq = g0 + g1
            if len(seq) > 4:
                raise Unsupported(f"TEX.SCR with {len(seq)} sources")
            spots = {1: [a], 2: [a, b], 3: [a, a + 1, b], 4: [a, a + 1, b, b + 1]}[len(seq)]
            regs = dict(zip(seq, spots, strict=True))
        else:
            regs = {n: a + k for k, n in enumerate(g0)} | {n: b + k for k, n in enumerate(g1)}
        for r in regs.values():
            if r != RZ:
                self.regs.add(r)

        def f(n: str) -> str:
            return f"uintBitsToFloat({reg(regs[n])})"

        coords = [f(f"c{k}") for k in range(ncoord)]
        key = (i.v("bank5"), i.v("tsPtrIdx"))
        t = f"t{key[0]}_{key[1]}"
        tb = f"{t}b"
        idxf = f"float({reg(regs['idx'])})" if is_array else None
        if is_array:
            coords.append(idxf)
        if dc:
            coords.append(f("ref"))
        sampler = f"sampler{name}{'Shadow' if dc else ''}"
        self.textures[key] = sampler
        if dc and lod == "LL":
            self.uses_shadow_lod = True  # textureLod on array and cube shadow samplers

        def build(tname: str, cds: list[str]) -> str:
            cs = list(cds)
            if aoffi:  # 4-bit signed texel offsets per axis; GLSL wants them constant, so they move the coordinates
                lvl = f"int({f('lod')})" if lod == "LL" else "0"
                for k in range(ncoord):
                    off = f"float(bitfieldExtract(int({reg(regs['off'])}), {4 * k}, 4))"
                    cs[k] = f"({cs[k]} + {off} / float(textureSize({tname}, {lvl}).{'xyz'[k]}))"
            v = cs[0] if len(cs) == 1 else f"vec{len(cs)}({', '.join(cs)})"
            if ptp:
                # One offset per texel: 4 signed bytes x, y per texel in two registers (Mesa NAK nak_nir_lower_tex.c:
                # 309-316). Texel k is the (i0, j0) texel (.w) of a gather at offset k, as GLSL's textureGatherOffsets
                # defines it; one gather each, since the offsets are not constant here.
                ch = "" if dc else f", {({'R': 0, 'G': 1, 'B': 2, 'A': 3}[i.sym('comp') or 'R'])}"
                args = self.split_ref(cs) if dc else v
                offs = [
                    f"ivec2(bitfieldExtract(int({reg(regs[f'p{k // 2}'])}), {16 * (k % 2)}, 8), "
                    f"bitfieldExtract(int({reg(regs[f'p{k // 2}'])}), {16 * (k % 2) + 8}, 8))"
                    for k in range(4)
                ]
                return f"vec4({', '.join(f'textureGatherOffset({tname}, {args}, {o}{ch}).w' for o in offs)})"
            if gather:
                ch = {"R": 0, "G": 1, "B": 2, "A": 3}[i.sym("comp") or "R"]
                call = f"textureGather({tname}, {v}, {ch})" if not dc else f"textureGather({tname}, {self.split_ref(cs)})"
            elif lod == "LZ":
                call = f"textureLod({tname}, {v}, 0.0)" if not dc else f"textureGrad({tname}, {v}, {self.zero_grad(name)})"
            elif lod == "LL":
                call = f"textureLod({tname}, {v}, {f('lod')})"
            elif lod == "LB":
                call = f"texture({tname}, {v}, {f('lod')})"
            elif lod in ("nolodlc", ""):
                call = f"texture({tname}, {v})"
            else:
                raise Unsupported(f"TEX lod mode {lod}")
            if dc and not gather:
                call = f"vec4({call}, 0.0, 0.0, 1.0)"
            return call

        if is_array:
            coords[ncoord] = f"min({idxf}, 63.0)"
        call = build(t, coords)
        if is_array:
            coords[ncoord] = f"clamp({idxf} - 64.0, 0.0, 3.0)"
            call = f"({idxf} < 64.0 ? {call} : {build(tb, coords)})"
        outs = self.tex_outs(i)
        return f"{{ vec4 tv = {call}; {' '.join(outs)} }}" if outs else ""

    def tex_outs(self, i: Insn) -> list[str]:
        """The components set in wmsk, in order, from tv to Rd, Rd+1, Rd2, Rd2+1. With .F16 (f16rm) two components
        share a register as packed halves (the first in the low half): the menus' text sampled the font's alpha with
        TEX.F16 and read it with HADD2, and a 32-bit float there discarded every glyph (31/10/2026)."""
        rd, rd2 = i.v("Rd"), i.v("Rd2")
        dests = [rd, rd + 1 if rd != RZ else RZ, rd2, rd2 + 1 if rd2 != RZ else RZ]
        comps = [c for c in range(4) if i.v("wmsk", 0xF) >> c & 1]
        if i.v("f16rm", 0):
            # (c0, c1) to Rd, (c2, c3) to Rd2, or Rd+1 without one (eden-nx2 lower_to_maxwell.h:1532-1543): Rd+1 for
            # both dropped the alpha of the button icons next to the title's «Pulsa», read from Rd2 (01/11/2026).
            dests = [rd, rd2 if rd2 != RZ else dests[1]]
            pairs = [comps[k : k + 2] for k in range(0, len(comps), 2)]
            vals = [f"packHalf2x16(vec2(tv[{p[0]}], {f'tv[{p[1]}]' if len(p) > 1 else '0.0'}))" for p in pairs]
        else:
            vals = [f"floatBitsToUint(tv[{c}])" for c in comps]
        outs = []
        for d, v in zip(dests, vals, strict=False):
            if d != RZ:
                self.regs.add(d)
                outs.append(f"r{d} = {v};")
        return outs

    @staticmethod
    def split_ref(coords: list[str]) -> str:
        """textureGather on a shadow sampler takes the coordinates and the reference as separate arguments."""
        p = coords[:-1]
        return f"{p[0] if len(p) == 1 else f'vec{len(p)}({chr(44).join(p)})'}, {coords[-1]}"

    def tld(self, i: Insn) -> str:
        """TLD (texelFetch): integer coordinates and array index, same register packing as TEX; .LZ lod 0, .LL lod
        in group 1. .MS: the sample index last in group 1 (Mesa NAK nak_nir_lower_tex.c:377-378), on a 2D or 2D array
        multisample texture, whose fetch takes the sample instead of a lod. Offsets are not handled."""
        dim = i.sym("paramA")
        ms = i.sym("ms") == "MS"
        if dim not in self.DIMS or i.sym("aoffi") == "AOFFI" or "Cube" in self.DIMS[dim][0] or (ms and "2D" not in dim):
            raise Unsupported(f"TLD {dim} {i.sym('ms')} {i.sym('aoffi')}")
        name, ncoord, is_array = self.DIMS[dim]
        lod = i.sym("lodlc")
        if ms:
            name = name.replace("2D", "2DMS")
            if lod == "LL":
                raise Unsupported("TLD.MS with a lod")
        g0 = (["idx"] if is_array else []) + [f"c{k}" for k in range(ncoord)]
        g1 = (["lod"] if lod == "LL" else []) + (["ms"] if ms else [])
        a, b = i.v("Ra"), i.v("Rb")
        if i.sym("scr") == "SCR":
            seq = g0 + g1
            if len(seq) > 4:
                raise Unsupported("TLD.SCR with more than 4 sources")
            regs = dict(zip(seq, {1: [a], 2: [a, b], 3: [a, a + 1, b], 4: [a, a + 1, b, b + 1]}[len(seq)], strict=True))
        else:
            regs = {n: a + k for k, n in enumerate(g0)} | {n: b + k for k, n in enumerate(g1)}
        self.regs.update(r for r in regs.values() if r != RZ)
        idx = f"int({reg(regs['idx'])})" if is_array else None
        ints = [f"int({reg(regs[f'c{k}'])})" for k in range(ncoord)] + ([idx] if is_array else [])
        if is_array:
            ints[-1] = f"min({idx}, 63)"
        vec = ints[0] if len(ints) == 1 else f"ivec{len(ints)}({', '.join(ints)})"
        key = (i.v("bank5"), i.v("tsPtrIdx"))
        self.textures.setdefault(key, f"sampler{name}")
        lv = f"int({reg(regs['ms' if ms else 'lod'])})" if ms or lod == "LL" else "0"
        t = f"t{key[0]}_{key[1]}"
        call = f"texelFetch({t}, {vec}, {lv})"
        if is_array:
            ints[-1] = f"clamp({idx} - 64, 0, 3)"
            vec_b = ints[0] if len(ints) == 1 else f"ivec{len(ints)}({', '.join(ints)})"
            call = f"({idx} < 64 ? {call} : texelFetch({t}b, {vec_b}, {lv}))"
        outs = self.tex_outs(i)
        return f"{{ vec4 tv = {call}; {' '.join(outs)} }}" if outs else ""

    def tmml(self, i: Insn) -> str:
        """TMML as Ryujinx InstEmitTexture.cs EmitTmml (MIT): sources [array index] + coordinates from Ra; component
        0 is textureQueryLod().y and 1 is .x (the instruction's order is GLSL's reversed), each * 256 as an integer."""
        dim = i.sym("paramA")
        if dim not in self.DIMS:
            raise Unsupported(f"TMML {dim}")
        name, ncoord, is_array = self.DIMS[dim]
        a = i.v("Ra")
        idx = 1 if is_array else 0
        coords = [f"uintBitsToFloat({reg(a + idx + k)})" for k in range(ncoord)]
        self.regs.update(r for r in range(a, a + idx + ncoord) if r != RZ)
        vec = coords[0] if ncoord == 1 else f"vec{ncoord}({', '.join(coords)})"
        key = (i.v("bank5"), i.v("tsPtrIdx"))
        self.textures.setdefault(key, f"sampler{name}")
        t = f"t{key[0]}_{key[1]}"
        rd, rd2 = i.v("Rd"), i.v("Rd2")
        dests = [rd, rd + 1 if rd != RZ else RZ, rd2, rd2 + 1 if rd2 != RZ else RZ]
        outs, comp, mask = [], 0, i.v("wmsk", 0xF)
        for c in range(4):
            if mask >> c & 1:
                d = dests[comp]
                comp += 1
                if d == RZ:
                    continue
                self.regs.add(d)
                outs.append(f"r{d} = {'uint(int(lq[' + str(c ^ 1) + '] * 256.0))' if c < 2 else '0u'};")
        return f"{{ vec2 lq = textureQueryLod({t}, {vec}); {' '.join(outs)} }}" if outs else ""

    @staticmethod
    def zero_grad(name: str) -> str:
        n = {"1D": 1, "2D": 2, "3D": 3, "Cube": 3, "1DArray": 1, "2DArray": 2, "CubeArray": 3}[name]
        z = "0.0" if n == 1 else f"vec{n}(0.0)"
        return f"{z}, {z}"
