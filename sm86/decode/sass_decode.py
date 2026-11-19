"""Decode a 128-bit SASS instruction word against an architecture's spec.

The decoding rule, and the evidence for each part of it, is written out in
`docs/decoding_rule.md`.
"""

import argparse

from sass_spec import WORD_BITS


class ItemValue:
    """What one `ENCODING` assignment holds in a particular word."""

    __slots__ = ("item", "slot_values", "symbols", "table_inputs",
                 "table_ambiguous", "notes")

    def __init__(self, item):
        self.item = item
        self.slot_values = []      # one (slot, value) per bit slot
        self.symbols = None        # enum symbol names for the value, if any
        self.table_inputs = None   # input tuple recovered through a TABLES row
        self.table_ambiguous = False
        self.notes = ()            # a list once read_values has one to add

    @property
    def value(self):
        return self.slot_values[0][1] if self.slot_values else None

    def render(self):
        parts = []
        for slot, value in self.slot_values:
            parts.append(f"{value}" if slot.width else "-")
        text = ",".join(parts)
        if self.symbols:
            text += " (" + "|".join(self.symbols) + ")"
        if self.table_inputs is not None:
            text += " <- " + self.table_inputs
            if self.table_ambiguous:
                text += " [+more]"
        return text


class Decoded:
    def __init__(self, word, opcode):
        self.word = word
        self.opcode = opcode
        self.status = "unmatched"
        self.cls = None
        self.matches = []          # classes that satisfied every constraint
        self.alternates = []       # ALTERNATE CLASS blocks that also matched
        self.rejections = {}       # class name -> list of failed constraints
        self.values = []           # ItemValue, for the single match
        self.unused_residue = 0    # bits set outside anything the class assigns

    @property
    def mnemonic(self):
        if self.cls is None:
            return None
        return self.cls.mnemonic[-1] if self.cls.mnemonic else self.cls.name


def constraint_failures(cls, word, use_starred=True, use_tables=True):
    """List the pinned values this word contradicts, empty when the class fits.

    A failure is `(item name, slot name, observed, allowed sample)`.
    """
    bad = []
    for item in cls.items:
        if item.allowed is None:
            continue
        if item.is_starred and not use_starred:
            continue
        if item.kind == "call" and not use_tables:
            continue
        for slot in item.slots:
            if slot.width == 0:
                # `BITS_0_Sb` pins nothing; it carries no bits at all.
                continue
            value = slot.extract(word)
            if value not in item.allowed:
                sample = sorted(item.allowed)
                if len(sample) > 6:
                    sample = sample[:6] + ["..."]
                bad.append((item.name, slot.name, value, sample))
    return bad


_ENUM_NAMES = {}  # (spec, type) -> {value: sorted names}: one pass over each table, not one per read (mkw-recomp)


def _enum_symbols(spec, type_name, value):
    by_value = _ENUM_NAMES.get((id(spec), type_name))
    if by_value is None:
        table = spec.enum_tables.get(type_name)
        if not table:
            return None
        by_value = {}
        for n, v in table["values"].items():
            by_value.setdefault(v, []).append(n)
        by_value = {v: sorted(ns) for v, ns in by_value.items()}
        _ENUM_NAMES[(id(spec), type_name)] = by_value
    names = by_value.get(value)
    return list(names) if names else None


def _read_plan(spec, cls):
    """Per item of `cls`, what read_values needs, resolved once instead of per word: (item, (slot, runs) of each
    slot, value -> symbols map or None, its note for an unnamed value, the call table's decode rows or None, its
    note). The decoder's hottest path: 58 % of a shader update's decoding (mkw-recomp, 18/11/2026)."""
    plan = []
    for item in cls.items:
        names = unnamed = rows = no_row = None
        if item.kind == "field" and item.format_type:
            _enum_symbols(spec, item.format_type, 0)  # builds the type's value -> names map
            names = _ENUM_NAMES.get((id(spec), item.format_type), {})
            unnamed = "value not named by its type" if item.format_type in spec.enum_tables else None
        elif item.kind == "call":
            table = spec.tables.get(item.call_name)
            if table and table.get("decode") and len(item.slots) == 1:
                rows, no_row = table["decode"], "no table row produces this value"
            elif len(item.slots) > 1:
                # `A,B = Call(x,y)` writes two slots; the table
                # model is one output per row, so the pair is not inverted.
                no_row = "pair-form call, slots read raw"
        plan.append((item, [(s, s.runs) for s in item.slots], names, unnamed, rows, no_row))
    return plan


def read_values(spec, cls, word):
    """Read every assignment of `cls` out of `word`."""
    plan = cls.__dict__.get("read_plan")
    if plan is None or plan[0] is not spec:
        plan = cls.read_plan = (spec, _read_plan(spec, cls))
    out = []
    for item, slots, names, unnamed, rows, no_row in plan[1]:
        iv = ItemValue(item)
        sv = iv.slot_values
        for slot, runs in slots:
            v = 0
            for p, m, i in runs:
                v |= ((word >> p) & m) << i
            sv.append((slot, v))
        value = sv[0][1] if sv else None
        if names is not None:
            symbols = names.get(value)
            if symbols:
                iv.symbols = list(symbols)
            elif unnamed:
                iv.notes = [unnamed]
        elif rows is not None:
            found = rows.get(str(value))
            if found:
                iv.table_inputs = found[0]
                iv.table_ambiguous = len(found) > 1
            else:
                iv.notes = [no_row]
        elif no_row:
            iv.notes = [no_row]
        out.append(iv)
    return out


def decode(spec, word, use_starred=True, use_tables=True):
    opcode = spec.opcode_of(word)
    result = Decoded(word, opcode)
    candidates = spec.by_opcode.get(opcode, [])
    if not candidates:
        result.status = "unknown-opcode"
        return result
    for cls in candidates:
        bad = constraint_failures(cls, word, use_starred, use_tables)
        if bad:
            result.rejections[cls.name] = bad
        else:
            result.matches.append(cls)
    primaries = [c for c in result.matches if not c.is_alternate]
    result.alternates = [c for c in result.matches if c.is_alternate]
    if len(primaries) == 1:
        # The primary class is the decode; the alternates are listed with it.
        # See docs/decoding_rule.md for why the primary is preferred.
        result.status = "ok"
        result.cls = primaries[0]
        result.values = read_values(spec, result.cls, word)
        result.unused_residue = word & result.cls.unused_mask
    elif len(primaries) > 1:
        result.status = "ambiguous"
    elif result.alternates:
        # No primary fits but an alternate does. Not seen in any test cubin;
        # reported rather than silently promoted, since the word would then be
        # spelled only by a block the description marks as an alternative.
        result.status = "alternate-only"
        result.cls = result.alternates[0]
        result.values = read_values(spec, result.cls, word)
        result.unused_residue = word & result.cls.unused_mask
    else:
        result.status = "unmatched"
    return result


# ---------------------------------------------------------------------------
# Re-encoding, for the self-consistency check.

class ReencodeError(Exception):
    pass


def reencode(spec, decoded, stats=None):
    """Rebuild a 128-bit word from a decode result, going back through the
    specification rather than copying the input.

    Constants come from the specification, not from the word. Enumerated fields
    go out through their type's name -> value map, and table-driven slots go out
    through the table's `encode` map, so both round trips are exercised.
    Bits the class does not assign are written as zero; the caller compares
    those separately, because the class says nothing about them.
    """
    if decoded.cls is None:
        raise ReencodeError("no single matching class")
    word = 0
    written = 0
    for iv in decoded.values:
        item = iv.item
        for slot, observed in iv.slot_values:
            if slot.width == 0:
                continue
            if written & slot.mask:
                raise ReencodeError(
                    f"slot {slot.name} overlaps a slot already written")
            value = _value_to_write(spec, item, iv, observed, stats)
            if value >> slot.width:
                raise ReencodeError(
                    f"{item.name}: value {value} does not fit slot "
                    f"{slot.name} ({slot.width} bits)")
            word = slot.deposit(word, value)
            written |= slot.mask
    return word


def _value_to_write(spec, item, iv, observed, stats=None):
    def bump(key):
        if stats is not None:
            stats[key] += 1

    if item.kind == "constant":
        bump("constant written from the specification")
        return int(item.value)
    if item.kind == "field":
        if iv.symbols:
            # Back through the type's symbol table.
            bump("enum symbol round trip")
            table = spec.enum_tables[item.format_type]["values"]
            return int(table[iv.symbols[0]])
        bump("field written back raw")
        return observed
    if item.kind == "call" and iv.table_inputs is not None:
        bump("TABLES row round trip")
        table = spec.tables[item.call_name]
        return int(table["encode"][iv.table_inputs])
    bump("value written back raw")
    return observed


def selfcheck(spec, decoded, stats=None):
    """Compare a re-encoded word with the original, bit for bit.

    Returns `(ok, differing bit positions, note)`. Bits outside the class's
    coverage are reported separately: the class assigns nothing to them, so
    they are not a decoder error. `stats`, when given a counter, records which
    re-encode path each slot took.
    """
    try:
        rebuilt = reencode(spec, decoded, stats)
    except (ReencodeError, KeyError) as exc:
        return False, [], f"re-encode failed: {exc}"
    covered = decoded.cls.covered_mask
    diff = (rebuilt ^ decoded.word) & covered
    bits = [b for b in range(WORD_BITS) if (diff >> b) & 1]
    return (not bits), bits, ""


# ---------------------------------------------------------------------------

def format_decode(decoded, verbose=False):
    lines = []
    head = f"0x{decoded.word:032x}  opcode={decoded.opcode}"
    if decoded.status in ("ok", "alternate-only"):
        head += f"  {decoded.mnemonic}  [{decoded.cls.name}]"
        if decoded.status == "alternate-only":
            head += " <alternate class, no primary matched>"
        if decoded.alternates and decoded.status == "ok":
            head += "  also spelled " + ",".join(
                c.name for c in decoded.alternates)
    else:
        head += f"  <{decoded.status}>"
    lines.append(head)
    if decoded.status in ("ok", "alternate-only"):
        if decoded.unused_residue:
            lines.append(f"    unassigned bits set: 0x{decoded.unused_residue:032x}")
        if verbose:
            for iv in decoded.values:
                slots = ",".join(s.name for s, _ in iv.slot_values)
                note = ("  # " + "; ".join(iv.notes)) if iv.notes else ""
                lines.append(f"    {iv.item.name:<44} = {iv.render():<28} {slots}{note}")
    elif decoded.status in ("ambiguous", "unmatched"):
        if decoded.status == "ambiguous":
            lines.append("    matches: " + ", ".join(c.name for c in decoded.matches))
        for name, bad in decoded.rejections.items():
            reason = "; ".join(f"{i}@{s}={v} not in {a}" for i, s, v, a in bad[:3])
            lines.append(f"    rejected {name}: {reason}")
    elif decoded.status == "unknown-opcode":
        lines.append("    no class carries this opcode value")
    return "\n".join(lines)


def main(argv=None):
    import sass_spec
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("arch", help="architecture whose specification to decode against")
    ap.add_argument("word", help="one 128-bit instruction word, in hexadecimal")
    ap.add_argument("--verbose", action="store_true", help="print every field")
    args = ap.parse_args(argv)

    try:
        spec = sass_spec.load(args.arch)
    except FileNotFoundError:
        ap.error(f"no specification for architecture {args.arch!r}; "
                 f"generate one with parse_sidl.py")
    except ValueError as exc:
        ap.error(str(exc))
    try:
        word = int(args.word, 16)
    except ValueError:
        ap.error(f"{args.word!r} is not a hexadecimal word")
    if word >> WORD_BITS:
        ap.error(f"word is wider than {WORD_BITS} bits")

    decoded = decode(spec, word)
    print(format_decode(decoded, args.verbose))
    if decoded.status != "ok":
        return 1
    ok, bits, note = selfcheck(spec, decoded)
    print(f"    self-check: {'ok' if ok else 'MISMATCH at bits ' + str(bits)}"
          + (f" ({note})" if note else ""))
    return 0 if ok and not note and not decoded.unused_residue else 1


if __name__ == "__main__":
    raise SystemExit(main())
