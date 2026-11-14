"""Protobuf wire format by hand for tools/npln: decode any message to fields without a schema, and build messages from
(field number, value) pairs.

Why: the NPLN messages are known only by field number (no .proto from Nintendo is used), and a schema-less decode lets
the server log a request it does not understand yet.

Usage (from tools/npln/services.py):
  fields = pb.decode(data)            # {number: [value, ...]}, values as int or bytes
  pb.msg((1, "text"), (2, 5), (3, pb.msg(...)))   # a message: str and bytes length-delimited, int varint
  pb.dump(data)                       # a readable tree for the log
"""


def varint(n: int) -> bytes:
    n &= (1 << 64) - 1
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        out.append(b | (0x80 if n else 0))
        if not n:
            return bytes(out)


def read_varint(b: bytes, i: int) -> tuple[int, int]:
    n = shift = 0
    while True:
        c = b[i]
        i += 1
        n |= (c & 0x7F) << shift
        shift += 7
        if not c & 0x80:
            return n, i


def decode(b: bytes) -> dict[int, list]:
    """{field: [values]}: varints and fixed as int, length-delimited as bytes. Raises ValueError on bad input."""
    out: dict[int, list] = {}
    i = 0
    while i < len(b):
        key, i = read_varint(b, i)
        f, wt = key >> 3, key & 7
        if wt == 0:
            v, i = read_varint(b, i)
        elif wt == 1:
            v, i = int.from_bytes(b[i : i + 8], "little"), i + 8
        elif wt == 2:
            n, i = read_varint(b, i)
            v, i = b[i : i + n], i + n
        elif wt == 5:
            v, i = int.from_bytes(b[i : i + 4], "little"), i + 4
        else:
            raise ValueError(f"wire type {wt}")
        if i > len(b) or f == 0:
            raise ValueError("truncated")
        out.setdefault(f, []).append(v)
    return out


def get(fields: dict, n: int, default=None):
    v = fields.get(n)
    return v[0] if v else default


def text(fields: dict, n: int, default: str = "") -> str:
    v = get(fields, n)
    return v.decode("utf-8", "replace") if isinstance(v, bytes) else default


def sub(fields: dict, n: int) -> dict:
    v = get(fields, n)
    try:
        return decode(v) if isinstance(v, bytes) else {}
    except (ValueError, IndexError):
        return {}


def field(n: int, v) -> bytes:
    if v is None:
        return b""
    if isinstance(v, bool):
        v = int(v)
    if isinstance(v, int):
        return varint(n << 3) + varint(v)
    if isinstance(v, str):
        v = v.encode()
    if isinstance(v, (bytes, bytearray)):
        return varint(n << 3 | 2) + varint(len(v)) + bytes(v)
    if isinstance(v, (list, tuple)):
        return b"".join(field(n, x) for x in v)
    raise TypeError(type(v))


def msg(*pairs) -> bytes:
    """A message from (field, value) pairs; None values are left out, lists repeat the field."""
    return b"".join(field(n, v) for n, v in pairs)


def fixed64(n: int, v: int) -> bytes:
    return varint(n << 3 | 1) + (v & ((1 << 64) - 1)).to_bytes(8, "little")


def timestamp(seconds: float) -> bytes:
    """google.protobuf.Timestamp / Duration: seconds 1, nanos 2."""
    s = int(seconds)
    return msg((1, s), (2, int((seconds - s) * 1e9) or None))


def dump(b: bytes, depth: int = 0, limit: int = 6) -> str:
    """A readable tree: nested messages decoded where they parse, printable strings as text, the rest as hex."""
    try:
        fields = decode(b)
    except (ValueError, IndexError):
        return repr(b[:64])
    pad = "  " * depth
    lines = []
    for f, vals in fields.items():
        for v in vals:
            if isinstance(v, bytes):
                printable = v and all(32 <= c < 127 for c in v)
                inner = None
                if not printable and depth < limit and v:
                    try:
                        decode(v)
                        inner = dump(v, depth + 1, limit)
                    except (ValueError, IndexError):
                        pass
                if printable:
                    lines.append(f"{pad}{f}: {v.decode()!r}")
                elif inner is not None:
                    lines.append(f"{pad}{f}: {{\n{inner}\n{pad}}}")
                else:
                    lines.append(f"{pad}{f}: <{len(v)} bytes {v[:24].hex()}>")
            else:
                lines.append(f"{pad}{f}: {v}")
    return "\n".join(lines)
