"""SM86 -> GLSL, glsl.core.Builder methods for memory: constant banks, global (storage buffers), shared, local, attributes
and IPA. A mixin: it uses the Builder's operand helpers (src_u, dst...) and state."""

from typing import ClassVar

from program import Insn

from glsl.common import PT, RZ, URZ, Unsupported, multi_load, reg


class MemOps:
    def fold_set(self, i: Insn, state: tuple) -> None:
        n = i.v("Rd")
        if n != RZ and not self.predicated and i.sym("sat") != "SAT":
            self.fold[n] = state
            self.fold_new.add(n)

    def fold_ipa_w(self, i: Insn) -> str | None:
        """(attr * w) * (1 / w) as attr: the console hands a perspective varying times w and the shader multiplies by
        the reciprocal of w (IPA.PASS a[0x7c], MUFU.RCP), two multiplies the driver cannot fold since w * (1 / w) is
        not 1 in floats; the varying is then the value Vulkan interpolated, as on the console up to rounding. The fold
        of smo-recomp mx_translate.c fold_ipa_w and SavageWindRecomp glsl_fold_ipa_w (-8 % ACO instructions there)."""
        if not (i.has("Ra") and i.has("Rb")) or any(
            i.v(f"{r}@negate", 0) or i.v(f"{r}@absolute", 0) for r in ("Ra", "Rb")
        ):
            return None
        a, b = self.fold.get(i.v("Ra")), self.fold.get(i.v("Rb"))
        for x, y in ((a, b), (b, a)):
            if x and y and x[0] == "aw" and y == ("rw",):
                return x[1]
        return None

    def ipa(self, i: Insn) -> str:
        """IPA as Ryujinx InstEmitAttribute.cs (MIT) does it: a perspective input is the interpolated value times
        gl_FragCoord.w (the hardware hands the shader value * w); MULTIPLY then multiplies by Rb."""
        if not i.has("attr"):
            return self.ipa_indexed(i)
        addr = i.v("attr") * 4
        if 0x70 <= addr < 0x80:
            v = f"gl_FragCoord[{(addr - 0x70) >> 2}]"
        elif 0x80 <= addr < 0x280:
            attr, comp = (addr - 0x80) // 16, ((addr - 0x80) >> 2) & 3
            self.in_attrs.add(attr)
            v = plain = f"in_attr{attr}[{comp}]"
            persp = bool(self.sph and self.sph_perspective(attr))
            if persp:
                v = f"({v} * gl_FragCoord.w)"
        elif addr in (0x2E0, 0x2E4):  # PointCoord (Ryujinx AttributeMap.cs)
            v = f"gl_PointCoord[{(addr - 0x2E0) >> 2}]"
        elif addr == 0x3FC:  # FrontFacing, a bool: all ones when true, as Ryujinx stores booleans
            v = "uintBitsToFloat(gl_FrontFacing ? 0xffffffffu : 0u)"
        else:
            raise Unsupported(f"IPA a[{addr:#x}] (system value)")
        op = i.sym("ipaop")
        if op == "MUL" and 0x80 <= addr < 0x280 and persp and self.fold.get(i.v("Rb")) == ("rw",):
            return self.dst_f(i, plain)  # fold_ipa_w
        if op == "MUL":
            v = f"({v} * {self.src_f(i, 'b')})"
        elif i.sym("ipaop") == "CONSTANT" and 0x80 <= addr < 0x280:
            v = f"in_attr{(addr - 0x80) // 16}[{((addr - 0x80) >> 2) & 3}]"  # flat: no perspective factor
        elif i.sym("ipaop") not in ("PASS", "", "CONSTANT"):  # CONSTANT on a system value: the value itself
            raise Unsupported(f"IPA.{i.sym('ipaop')}")
        out = self.dst_f(i, v)
        if op in ("PASS", "") and addr == 0x7C:
            self.fold_set(i, ("w",))
        elif op in ("PASS", "") and 0x80 <= addr < 0x280 and persp:
            self.fold_set(i, ("aw", plain))
        return out

    def ipa_indexed(self, i: Insn) -> str:
        """IPA.PASS a[URa + offset]: an input picked at run time (a loop over a material's inputs: agl_common 230 and
        three more). ipa_dyn, made by ipa_dyn_fns, reads any generic input the SPH marks used, with the same
        perspective factor as a fixed IPA, and FragCoord; any other address reads 0."""
        if not i.has("URa") or i.sym("ipaop") not in ("PASS", "") or not self.sph:
            raise Unsupported(f"{i.cls}: attribute address in a register")
        self.ipa_dyn = True
        off = i.v("URa_offset") * 4
        ua = i.v("URa")
        if ua != URZ:
            self.uregs.add(ua)
        addr = f"(ur{ua} + {off:#x}u)" if ua != URZ else f"{off:#x}u"
        return self.dst_f(i, f"ipa_dyn({addr})")

    def ipa_dyn_fns(self) -> list[str]:
        if not getattr(self, "ipa_dyn", False):
            return []
        cases = [f"    case {0x70 + 4 * c:#x}u: return gl_FragCoord[{c}];" for c in range(4)]
        for a in range(32):
            if not any(self.sph.imap_used(a, c) for c in range(4)):
                continue
            self.in_attrs.add(a)
            w = " * gl_FragCoord.w" if self.sph_perspective(a) else ""
            cases += [f"    case {0x80 + 16 * a + 4 * c:#x}u: return in_attr{a}[{c}]{w};" for c in range(4)]
        return ["float ipa_dyn(uint a) {", "    switch (a) {", *cases, "    }", "    return 0.0;", "}"]

    def sph_perspective(self, attr: int) -> bool:
        """PS input map: the first used component of a generic input says its interpolation (2 = perspective)."""
        byte = (self.sph.words[6 + (attr >> 2)] >> ((attr & 3) * 8)) & 0xFF
        for comp in range(4):
            t = (byte >> (comp * 2)) & 3
            if t:
                return t == 2
        return False

    def uldc(self, i: Insn) -> str:
        size = {"32": 1, "": 1, "64": 2}.get(i.sym("sz"))
        if size is None:
            raise Unsupported(f"ULDC.{i.sym('sz')}")
        n = i.v("URd")
        if n == URZ:
            return ""
        iv = next(v for k, v in i.f.items() if k.startswith("ConstBankAddress"))
        bank, addr = (v for _, v in iv.slot_values)
        out = []
        for k in range(size):   # a new value: no longer what was traced (track_ssbo leaves ULDC to this)
            self.ur_ssbo.pop(n + k, None), self.ur_derived.pop(n + k, None)
        if i.has("URa") and i.v("URa") != URZ:
            # ULDC URd, c[bank][URa + offset]: a dynamic address. The index was dropped and every read took word
            # `offset`: the race's colour-grading LUT read the same table entry for all 32 slices (no blue, 31/10/2026).
            self.cbufs.add(bank)
            loads = []
            for k in range(size):
                self.uregs.add(n + k)
                loads.append(self.cb_word(bank, self.ureg(i.v("URa")), addr + 4 * k))
            tmp = ", ".join(f"lw{k} = {w}" for k, w in enumerate(loads))
            return f"{{ uint {tmp}; {' '.join(f'ur{n + k} = lw{k};' for k in range(size))} }}"
        for k in range(size):
            self.uregs.add(n + k)
            out.append(f"ur{n + k} = {self.const(bank, addr + 4 * k, spec=not (size == 2 and bank == 0))};")
        if size == 2 and bank == 0:
            self.ur_ssbo[n] = addr  # a storage buffer address: the driver keeps one per binding in bank 0
        return " ".join(out)

    # ---- global memory as storage buffers
    def global_mem(self, i: Insn) -> str:
        """LDG/STG through a storage buffer's address, which the driver keeps in bank 0 (one slot per binding, as
        Ryujinx's GlobalMemoryToStorageBufferPass finds them on Maxwell): a 64-bit uniform base loaded from it, or a
        register pair computed from it (reg_ssbo). The access becomes sb<offset>.d[] at its byte offset in the buffer:
        for a register base, Ra minus the buffer's address (the low words; buffers stay under 4 GiB). A register base the
        tracking lost goes through global_search."""
        try:
            slot, rel = self.global_addr(i)
        except Unsupported:
            if not ("sImmOffset" in i.cls or "RaNonRZ" in i.cls) or i.v("Ra") == RZ:
                raise
            return self.global_search(i)
        if i.op == "STG":
            self.ssbos_written.add(slot)
        sz = i.sym("sz")
        sub = {"U8": (8, False), "S8": (8, True), "U16": (16, False), "S16": (16, True)}.get(sz)
        words = {"32": 1, "64": 2, "128": 4}.get(sz, 1 if sub else None)
        if words is None:
            raise Unsupported(f"{i.op}.{sz}")
        if sub and i.op == "STG":
            # A byte or halfword store into the word holding it: clear its bits, then set them, both atomic, so the
            # other bytes of the word (other invocations may store them) stay as they are.
            bits = sub[0]
            sh, n = f"((({rel}) & 3u) * 8u)", i.v("Rb")
            if n != RZ:
                self.regs.add(n)
            m = f"({(1 << bits) - 1:#x}u << {sh})"
            w = f"sb{slot:x}.d[({rel}) >> 2]"
            return f"atomicAnd({w}, ~{m}); atomicOr({w}, ({reg(n)} << {sh}) & {m});"
        if sub:
            bits, signed = sub
            n = i.v("Rd")
            if n == RZ:
                return ""
            self.regs.add(n)
            w, sh = f"sb{slot:x}.d[{rel} >> 2]", f"(({rel} & 3u) * 8u)"
            e = (
                f"uint(bitfieldExtract(int({w}), int({sh}), {bits}))"
                if signed
                else f"bitfieldExtract({w}, int({sh}), {bits})"
            )
            return f"r{n} = {e};"
        out = []
        if i.op == "LDG":
            n, loads = i.v("Rd"), []
            for k in range(words):
                if n + k != RZ:
                    self.regs.add(n + k)
                    loads.append((n + k, f"sb{slot:x}.d[({rel} >> 2) + {k}u]"))
            return multi_load(loads)
        else:
            n = i.v("Rb")
            for k in range(words):
                self.regs.add(n + k) if n != RZ else None
                out.append(f"sb{slot:x}.d[({rel} >> 2) + {k}u] = {reg(n + k if n != RZ else RZ)};")
        return " ".join(out)

    def global_addr(self, i: Insn) -> tuple[int, str]:
        """(storage buffer's c[0] offset, byte offset in it) of a global access; see global_mem."""
        base = next((i.v(k) for k in ("Ra_URb", "Ra_URc") if i.has(k)), None)
        off = i.sv("Ra_offset") & 0xFFFFFFFF
        ra = i.v("Ra") if i.has("Ra") else RZ
        if base is not None and base in self.ur_ssbo:
            slot = self.ur_ssbo[base]
            rel = f"({self.src_u(i, 'a')} + {off:#x}u)" if i.cls.endswith("Ra32") and ra != RZ else f"{off:#x}u"
        elif base is not None and base in self.ur_derived:
            slot = self.ur_derived[base]
            self.uregs.add(base)
            extra = f"{self.src_u(i, 'a')} + " if i.cls.endswith("Ra32") and ra != RZ else ""
            rel = f"({extra}ur{base} - {self.const(0, slot, spec=False)} + {off:#x}u)"
        elif base is None and ("sImmOffset" in i.cls or "RaNonRZ" in i.cls) and ra in self.reg_ssbo:
            slot = self.reg_ssbo[ra]
            self.regs.add(ra)
            rel = f"(r{ra} - {self.const(0, slot, spec=False)} + {off:#x}u)"
        else:
            raise Unsupported(f"{i.cls}: base not a traced storage buffer")
        self.ssbos.add(slot)
        return slot, rel

    def global_search(self, i: Insn) -> str:
        """LDG/STG of 32, 64 or 128 bits whose register base the linear tracking lost (a BRX or another path rewrote
        the register in program order: DrivePassShaderDefault 91, missing in 121 runs). The address is looked up at run
        time among the program's storage buffers (gld/gst, made by global_search_fns once they are all known): the
        nearest base below it whose bound range holds it. The idea is eden-nx2's global memory functions
        (src/shader_recompiler/backend/spirv/spirv_emit_context.cpp:942-1000, GPL-3.0: idea only)."""
        words = {"32": 1, "64": 2, "128": 4}.get(i.sym("sz"))
        if words is None:
            raise Unsupported(f"{i.op}.{i.sym('sz')}: base not a traced storage buffer")
        ra, off = i.v("Ra"), i.sv("Ra_offset") & 0xFFFFFFFF
        self.regs.update((ra, ra + 1))
        addr = f"uint gc, gl = uaddCarry(r{ra}, {off:#x}u, gc), gh = r{ra + 1} + gc;"
        if i.op == "LDG":
            self.gsearch_load = True
            n = i.v("Rd")
            if n == RZ:
                return ""
            self.regs.update(range(n, n + words))
            tmp = ", ".join(f"gw{k} = gld(gl, gh, {k}u)" for k in range(words))
            return f"{{ {addr} uint {tmp}; {' '.join(f'r{n + k} = gw{k};' for k in range(words))} }}"
        self.gsearch_store = True
        n = i.v("Rb")
        if n != RZ:
            self.regs.update(range(n, n + words))
        stores = " ".join(f"gst(gl, gh, {k}u, {reg(n + k if n != RZ else RZ)});" for k in range(words))
        return f"{{ {addr} {stores} }}"

    def global_search_fns(self) -> list[str]:
        """gld/gst over the storage buffers the program names (bank 0 base, bound length); every one may be stored
        into when gst is used. No buffer at all: nothing to search."""
        if not (self.gsearch_load or self.gsearch_store):
            return []
        if not self.ssbos:
            raise Unsupported("global access with no storage buffer to search")
        if self.gsearch_store:
            self.ssbos_written |= self.ssbos
        find = []
        for k, o in enumerate(sorted(self.ssbos)):
            lo, hi = self.const(0, o, spec=False), self.const(0, o + 4, spec=False)
            find.append(f"    r = usubBorrow(lo, {lo}, b); if (hi - {hi} - b == 0u && r < uint(sb{o:x}.d.length()) * 4u "
                        f"&& r < w) {{ w = r; s = {k}; }}")  # fmt: skip
        head = ["uint gfind(uint lo, uint hi, out int s) {", "    uint w = 0xffffffffu, r, b; s = -1;", *find,
                "    return w;", "}"]  # fmt: skip
        pick = " else ".join(f"if (s == {k}) {{X(sb{o:x})}}" for k, o in enumerate(sorted(self.ssbos)))
        out = head
        if self.gsearch_load:
            ld = pick.replace("{X(", "v = ").replace(")}", ".d[(w >> 2) + k];")
            out += ["uint gld(uint lo, uint hi, uint k) {", "    int s; uint w = gfind(lo, hi, s), v = 0u;",
                    f"    {ld}", "    return v;", "}"]  # fmt: skip
        if self.gsearch_store:
            st = pick.replace("{X(", "").replace(")}", ".d[(w >> 2) + k] = v;")
            out += ["void gst(uint lo, uint hi, uint k, uint v) {", "    int s; uint w = gfind(lo, hi, s);",
                    f"    {st}", "}"]  # fmt: skip
        return out

    ATOM: ClassVar[dict[str, str]] = {"ADD": "atomicAdd", "MIN": "atomicMin", "MAX": "atomicMax", "AND": "atomicAnd",
                                      "OR": "atomicOr", "XOR": "atomicXor", "EXCH": "atomicExchange"}  # fmt: skip

    def atom_global(self, i: Insn) -> str:
        """ATOMG / RED on a storage buffer word: GLSL atomics on its uint array, 32-bit unsigned or ADD/EXCH of any
        32-bit kind (signed MIN/MAX would need an int view: not handled). RED keeps no result."""
        op, sz = i.sym("op"), i.sym("sz")
        if op not in self.ATOM or sz not in ("U32", "32", "S32") or (sz == "S32" and op in ("MIN", "MAX")):
            raise Unsupported(f"{i.op}.{op}.{sz}")
        if i.has("Pu") and i.v("Pu") != PT:
            raise Unsupported(f"{i.op} with predicate out")
        slot, rel = self.global_addr(i)
        self.ssbos_written.add(slot)
        e = f"{self.ATOM[op]}(sb{slot:x}.d[{rel} >> 2], {self.src_u(i, 'b')})"
        return f"{e};" if i.op == "RED" else self.dst(i, e)

    def track_ssbo(self, i: Insn) -> None:
        """reg_ssbo after i: a register written from a bank-0 word that is a storage buffer's address (LEA/IADD3 of
        an index with it, LDC.64 of it), or from a register already holding one, keeps that buffer; any other write
        drops it. Straight-line approximation of Ryujinx's search, enough for the address arithmetic ptxas emits."""
        if i.has("URd") and i.op != "ULDC":   # ULDC keeps uldc's record
            # uniform: a register derived from a ULDC.64 base (ur_ssbo) or from one derived already (ur_derived).
            # Any other write ends both: a base kept after `UIADD3 UR4, UR4, 0x100` lost the offset of the accesses
            # through it (115 programs, among them the cacti's draw arguments; tools/shaders/track_audit.py).
            n = i.v("URd")
            src = None
            if i.op in ("UIADD3", "ULEA", "UMOV", "UIMAD"):
                src = next((self.ur_ssbo.get(i.v(r), self.ur_derived.get(i.v(r))) for r in ("URa", "URb", "URc")
                            if i.has(r) and (i.v(r) in self.ur_ssbo or i.v(r) in self.ur_derived)), None)  # fmt: skip
            self.ur_ssbo.pop(n, None), self.ur_derived.pop(n, None)
            if src is not None and n != URZ:
                self.ur_derived[n] = src
            return
        if not i.has("Rd") or i.op == "STG":
            return   # an LDG's destination is a loaded value, a pointer at most: dropped below
        n = i.v("Rd")
        if n == RZ:
            return
        slot = None
        for k, iv in i.f.items():
            if k.startswith("ConstBankAddress"):
                bank, word = (v for _, v in iv.slot_values)
                if bank == 0 and word % 2 == 0 and word * 4 >= 0x10:  # CS bases from 0x10 (dumped cb84cbdf)
                    slot = word * 4
        if slot is None and i.op in ("IADD3", "LEA", "IMAD", "MOV", "SEL"):
            slot = next((self.reg_ssbo[i.v(r)] for r in ("Ra", "Rb", "Rc") if i.has(r) and i.v(r) in self.reg_ssbo),
                        None)  # fmt: skip
        self.reg_ssbo.pop(n, None)
        # a 32-bit write leaves the next register alone (agl_common: `SHF R7` between `IADD3 R8, c[0x0][0x510]` and
        # `RED [R8]` dropped R8's buffer: 71 programs, 31/10/2026); pairs and quads clear what they cover
        wide = {i.sym(k) for k in ("sz", "wide", "fmt")} & {"64", "128", "WIDE", "U64", "S64"}
        for k in range(1, 4 if "128" in wide else 2 if wide else 1):
            self.reg_ssbo.pop(n + k, None)
        if slot is not None:
            self.reg_ssbo[n] = slot

    def ldc(self, i: Insn) -> str:
        """LDC c[bank][Ra + offset]: a constant read at a dynamic address; .U8/.S8/.U16/.S16 take the byte or halfword
        from the word holding it, zero- or sign-extended."""
        sz = i.sym("sz")
        sub = {"U8": (8, False), "S8": (8, True), "U16": (16, False), "S16": (16, True)}.get(sz)
        words = {"32": 1, "64": 2}.get(sz, 1 if sub else None)
        if words is None:
            raise Unsupported(f"LDC.{sz}")
        iv = next(v for k, v in i.f.items() if k.startswith("ConstBankAddress"))
        bank, off = (v for _, v in iv.slot_values)
        self.cbufs.add(bank)
        addr = f"({self.src_u(i, 'a')} + {off}u)"
        out, n = [], i.v("Rd")
        if sub:
            if n == RZ:
                return ""
            self.regs.add(n)
            word, sh = f"cb{bank}.d[{addr} >> 4][({addr} >> 2) & 3u]", f"int(({addr} & 3u) * 8u)"
            bits, signed = sub
            v = f"uint(bitfieldExtract(int({word}), {sh}, {bits}))" if signed else f"bitfieldExtract({word}, {sh}, {bits})"
            return f"r{n} = {v};"
        for k in range(words):
            if n + k != RZ:
                self.regs.add(n + k)
                out.append((n + k, self.cb_word(bank, self.src_u(i, "a"), off + 4 * k)))
        return multi_load(out)

    def cb_word(self, bank: int, base: str, off: int) -> str:
        """The word at byte base + off of a constant bank, base dynamic and off a constant multiple of 4: through the
        bank's scalar-layout view (head.py), as (base >> 2) + off / 4 with an arithmetic shift, so a negative base
        still lands where base + off does. Through the std140 `.d[A >> 4][(A >> 2) & 3]` RADV paid ~4 VALU per load
        (add, and -16, bfe, lshl_add) that it never folds into A & ~3; this way the shift is shared by the loads with
        one base and the offset goes into the load's immediate (pt4)."""
        if off & 3:
            a = f"({base} + {off}u)"
            return f"cb{bank}.d[{a} >> 4][({a} >> 2) & 3u]"
        self.cbufs_w.add(bank)
        return f"cb{bank}w.w[uint(int({base}) >> 2) + {off >> 2}u]"

    def shared_mem(self, i: Insn) -> str:
        """LDS/STS: byte address Ra * stride (X1/X4/X8/X16) [+ URb] + offset into shared memory (words); LDS of 8/16
        bits takes them from the word holding them, zero- or sign-extended."""
        sz = i.sym("sz")
        sub = {"U8": (8, False), "S8": (8, True), "U16": (16, False), "S16": (16, True)}.get(sz)
        words = {"32": 1, "64": 2, "128": 4}.get(sz, 1 if sub and i.op == "LDS" else None)
        if words is None:
            raise Unsupported(f"{i.op}.{sz}")
        self.uses_shared = True
        stride = {"X1": 1, "X4": 4, "X8": 8, "X16": 16, "": 1}.get(i.sym("stride"), 1)
        addr = f"({self.src_u(i, 'a')} * {stride}u + {i.sv('Ra_offset') & 0xFFFFFFFF:#x}u"
        if i.has("Ra_URb"):
            addr += f" + {self.ureg(i.v('Ra_URb'))}"
        addr += ")"
        out = []
        if sub:
            n = i.v("Rd")
            if n == RZ:
                return ""
            self.regs.add(n)
            w, sh, (bits, signed) = f"SMEM({addr} >> 2)", f"int(({addr} & 3u) * 8u)", sub
            v = f"uint(bitfieldExtract(int({w}), {sh}, {bits}))" if signed else f"bitfieldExtract({w}, {sh}, {bits})"
            return f"r{n} = {v};"
        for k in range(words):
            w = f"SMEM(({addr} >> 2) + {k}u)"
            if i.op == "LDS":
                n = i.v("Rd")
                if n != RZ:
                    self.regs.add(n + k)
                    out.append((n + k, w))
            else:
                n = i.v("Rb")
                if n != RZ:
                    self.regs.add(n + k)
                out.append(f"{w} = {reg(n + k if n != RZ else RZ)};")
        return multi_load(out) if i.op == "LDS" else " ".join(out)

    def local_mem(self, i: Insn) -> str:
        """LDL/STL: thread-private memory (register spills, indexed arrays) as a uint array of the size the SPH gives
        (word 1, low 24 bits: 0x10-0x200 bytes in the 22 programs that use it, 31/10/2026, each at least the furthest
        constant access). Byte address Ra + offset; a register index is clamped to the array."""
        words = {"32": 1, "64": 2, "128": 4}.get(i.sym("sz"))
        if words is None:
            raise Unsupported(f"{i.op}.{i.sym('sz')}")
        off = i.sv("Ra_offset") if "sImm" in i.cls else i.v("Ra_offset")
        self.lmem_bytes = max(self.lmem_bytes, off + 4 * words, 4)
        ra, out = i.v("Ra"), []
        for k in range(words):
            if ra == RZ:
                w = f"lmem[{off // 4 + k}]"
            else:
                w = f"lmem[min((({self.src_u(i, 'a')} + {off & 0xFFFFFFFF:#x}u) >> 2) + {k}u, LMEM_WORDS - 1u)]"
            if i.op == "LDL":
                n = i.v("Rd")
                if n != RZ:
                    self.regs.add(n + k)
                    out.append((n + k, w))
            else:
                n = i.v("Rb")
                if n != RZ:
                    self.regs.add(n + k)
                out.append(f"{w} = {reg(n + k if n != RZ else RZ)};")
        return multi_load(out) if i.op == "LDL" else " ".join(out)

    # Attribute addresses (Ryujinx AttributeConsts.cs, MIT): 0x70 position, 0x80 + 16 i generic i, 0x6c point size,
    # 0x2c0 clip distances, 0x2f8 instance id, 0x2fc vertex id.
    def attr_word(self, addr: int, out: bool) -> str:
        if 0x70 <= addr < 0x80:
            if not out:
                raise Unsupported("ALD of the position")
            return f"gl_Position[{(addr - 0x70) >> 2}]"
        if 0x80 <= addr < 0x280:
            g, c = (addr - 0x80) // 16, ((addr - 0x80) >> 2) & 3
            (self.out_attrs if out else self.in_attrs).add(g)
            return f"{'out_attr' if out else 'in_attr'}{g}[{c}]"
        if addr == 0x6C and out:
            return "gl_PointSize"
        if addr == 0x5C and out:
            # Not in Maxwell's map (Ryujinx); eden-nx2 meets it as "reserved attribute 23". Hypothesis: Ampere's
            # per-primitive shading rate, which goes with MKW's VRS calls; VRS is off, so the write is dropped.
            return ""
        if 0x2C0 <= addr < 0x2E0 and out:
            self.clip = max(getattr(self, "clip", 0), ((addr - 0x2C0) >> 2) + 1)
            return f"gl_ClipDistance[{(addr - 0x2C0) >> 2}]"
        if addr == 0x2F8 and not out:
            # Maxwell's InstanceId does not include the draw's base instance; Vulkan's InstanceIndex does (eden-nx2
            # emit_spirv_context_get_set.cpp:346-352, an idea: index minus base). The HUD's text VS indexes its glyph
            # records with it and read past them on draws with a base instance.
            return "intBitsToFloat(gl_InstanceIndex - gl_BaseInstance)"
        if addr == 0x2FC and not out:
            return "intBitsToFloat(gl_VertexIndex)"
        raise Unsupported(f"{'AST' if out else 'ALD'} a[{addr:#x}]")

    def attr_mem(self, i: Insn) -> str:
        """ALD/AST with a constant address (Ra = RZ): consecutive words from Rd / Rb."""
        if i.v("Ra") != RZ or (i.op == "ALD" and i.sym("io") == "O"):
            raise Unsupported(f"{i.op} indexed or of outputs")
        n = {"32": 1, "64": 2, "96": 3, "128": 4}.get(i.sym("sz"))
        if n is None:
            raise Unsupported(f"{i.op}.{i.sym('sz')}")
        base, out = i.v("Ra_offset"), i.op == "AST"
        stmts = []
        for k in range(n):
            target = self.attr_word(base + 4 * k, out)
            if out and not target:
                continue
            if out:
                r = i.v("Rb") + k if i.v("Rb") != RZ else RZ
                if r != RZ:
                    self.regs.add(r)
                stmts.append(f"{target} = uintBitsToFloat({reg(r)});")
            else:
                d = i.v("Rd") + k
                if i.v("Rd") != RZ:
                    self.regs.add(d)
                    stmts.append(f"r{d} = floatBitsToUint({target});")
        return " ".join(stmts)
