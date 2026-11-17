#!/usr/bin/env python3
"""Draws the icon of mkw.exe by code (our own: a checkered flag on a rounded square, no art of the game) and writes it as
an .ico with PNG images of 256, 48, 32 and 16 pixels. tools/build/build.py --target windows calls it before the
resource (runtime/platform/mkw.rc names "mkw.ico"); the file is made in build/, never committed. With an .svg name it
writes the same drawing as vector shapes instead (the repo's logo, docs/images/logo.svg: text, so it can be versioned); with a
.png name, one 256-pixel image (the Linux AppImage's icon, tools/build/release.py); with a .c name, the C source of
app_icon_set(SDL_Window *), which gives the window a 128-pixel copy (build.py, every target): Wayland compositors with
xdg_toplevel_icon (KWin) show it in the taskbar instead of a generic icon when no desktop file matches the app id.

Usage: uv run tools/build/icon.py OUT.ico | OUT.svg | OUT.png | OUT.c
"""

import math
import struct
import sys
import zlib
from pathlib import Path

BG_TOP, BG_BOTTOM = (36, 84, 196), (18, 38, 110)   # blue, lighter at the top
POLE = (235, 235, 240)


def coverage(x: float, y: float) -> tuple[int, int, int, int]:
    """Colour at (x, y) in a unit square, (0, 0) top left; alpha 0 outside the rounded square."""
    r = 0.18   # corner radius
    cx, cy = min(max(x, r), 1 - r), min(max(y, r), 1 - r)
    if (x - cx) ** 2 + (y - cy) ** 2 > r * r:
        return (0, 0, 0, 0)
    # the flag: 4x3 checks on a waving cloth between the pole and the right side
    fx, fy = (x - 0.30) / 0.52, y - 0.20 - 0.045 * math.sin(x * 2 * math.pi * 1.1)
    if 0 <= fx <= 1 and 0 <= fy <= 0.42:
        light = (int(fx * 4) + int(fy / 0.42 * 3)) % 2 == 0
        shade = 1 - 0.18 * math.cos(x * 2 * math.pi * 1.1)   # folds of the cloth
        c = (250, 250, 250) if light else (20, 20, 24)
        return (*[min(255, int(v * shade)) for v in c], 255)
    if 0.24 <= x <= 0.30 and 0.16 <= y <= 0.86:   # the pole
        return (*POLE, 255)
    t = y
    return (*[int(a + (b - a) * t) for a, b in zip(BG_TOP, BG_BOTTOM)], 255)


def render(size: int, ss: int = 4) -> bytes:
    """RGBA rows, each pixel the mean of ss x ss samples (antialiasing)."""
    rows = bytearray()
    for py in range(size):
        rows.append(0)   # PNG filter: none
        for px in range(size):
            acc = [0, 0, 0, 0]
            for sy in range(ss):
                for sx in range(ss):
                    c = coverage((px + (sx + 0.5) / ss) / size, (py + (sy + 0.5) / ss) / size)
                    a = c[3]
                    acc[0] += c[0] * a
                    acc[1] += c[1] * a
                    acc[2] += c[2] * a
                    acc[3] += a
            a = acc[3]
            rows += bytes([acc[0] // a, acc[1] // a, acc[2] // a, a // (ss * ss)] if a else [0, 0, 0, 0])
    return bytes(rows)


def png(size: int) -> bytes:
    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

    head = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)   # 8-bit RGBA
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", head) + chunk(b"IDAT", zlib.compress(render(size), 9)) + chunk(b"IEND", b"")


def wave(x: float) -> float:
    """The cloth's vertical offset at x, as in coverage()."""
    return 0.045 * math.sin(x * 2 * math.pi * 1.1)


def svg() -> str:
    """The icon as SVG in a 256-unit square: each check a polygon whose edges follow the wave."""
    s, steps = 256, 8

    def pt(fx: float, fy: float) -> str:
        x = 0.30 + 0.52 * fx
        return f"{x * s:.1f},{(0.20 + wave(x) + fy) * s:.1f}"

    top, bottom = "#%02x%02x%02x" % BG_TOP, "#%02x%02x%02x" % BG_BOTTOM
    out = [
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {s} {s}" width="{s}" height="{s}">',
        f'<defs><linearGradient id="bg" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="{top}"/>'
        f'<stop offset="1" stop-color="{bottom}"/></linearGradient></defs>',
        f'<rect width="{s}" height="{s}" rx="{0.18 * s:.1f}" fill="url(#bg)"/>',
        f'<rect x="{0.24 * s:.1f}" y="{0.16 * s:.1f}" width="{0.06 * s:.1f}" height="{0.70 * s:.1f}" '
        'fill="#ebebf0"/>',
    ]
    for row in range(3):
        for col in range(4):
            x0, x1, y0, y1 = col / 4, (col + 1) / 4, row * 0.14, (row + 1) * 0.14
            xs = [x0 + (x1 - x0) * i / steps for i in range(steps + 1)]
            points = [pt(x, y0) for x in xs] + [pt(x, y1) for x in reversed(xs)]
            fill = "#fafafa" if (col + row) % 2 == 0 else "#141418"
            out.append(f'<polygon points="{" ".join(points)}" fill="{fill}"/>')
    return "\n".join([*out, "</svg>", ""])


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    if sys.argv[1].endswith(".svg"):
        Path(sys.argv[1]).write_text(svg())
        return
    if sys.argv[1].endswith(".png"):
        Path(sys.argv[1]).write_bytes(png(256))
        return
    if sys.argv[1].endswith(".c"):
        n = 128
        rows = render(n)
        px = b"".join(rows[y * (n * 4 + 1) + 1 : (y + 1) * (n * 4 + 1)] for y in range(n))   # without the filter bytes
        body = ",".join(str(b) for b in px)
        Path(sys.argv[1]).parent.mkdir(parents=True, exist_ok=True)
        Path(sys.argv[1]).write_text(
            "// Drawn by tools/build/icon.py: the window's icon, RGBA rows. Generated, never committed.\n"
            "#include <SDL3/SDL.h>\n"
            f"static const unsigned char px[{len(px)}] = {{{body}}};\n"
            "void app_icon_set(SDL_Window *w) {\n"
            f"    SDL_Surface *s = SDL_CreateSurfaceFrom({n}, {n}, SDL_PIXELFORMAT_RGBA32, (void *)px, {n * 4});\n"
            "    if (s) SDL_SetWindowIcon(w, s), SDL_DestroySurface(s);   // SDL keeps a copy\n"
            "}\n"
        )
        return
    sizes = (256, 48, 32, 16)
    images = [png(s) for s in sizes]
    out = struct.pack("<HHH", 0, 1, len(images))
    off = 6 + 16 * len(images)
    for s, data in zip(sizes, images):
        out += struct.pack("<BBBBHHII", s % 256, s % 256, 0, 0, 1, 32, len(data), off)
        off += len(data)
    Path(sys.argv[1]).parent.mkdir(parents=True, exist_ok=True)
    Path(sys.argv[1]).write_bytes(out + b"".join(images))


if __name__ == "__main__":
    main()
