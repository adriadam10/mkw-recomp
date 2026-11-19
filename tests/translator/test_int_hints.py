"""The integer input and output hints (sm86/translate/glsl/int_hints.py) on the shapes of MKW's programs.

Usage: uv run pytest tests/translator/test_int_hints.py
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT / "sm86/translate"), str(ROOT / "sm86/translate/frontend")]
from glsl import int_hints  # noqa: E402

VS_TEXEL = """layout(location = 0) in vec4 in_attr0;
void main() {
        r21 = floatBitsToUint(in_attr0[0]);
        r9 = uint(int(clamp(floor(uintBitsToFloat(r18)), -2147483648.0, 2147483647.0)));
        { vec4 tv = texelFetch(t2_12, ivec2(int(r21), int(r9)), 0); r8 = floatBitsToUint(tv[0]); }
        r21 = floatBitsToUint(fma((-uintBitsToFloat(r14)), uintBitsToFloat(r14), uintBitsToFloat(0x3f800000u)));
        r15 = floatBitsToUint(sqrt(uintBitsToFloat(r21)));
}"""  # DriveRefreshUber.Procedural's vertex program 3 (RGBA16I at 0)

VS_SIGN_TEST = """layout(location = 2) in vec4 in_attr2;
void main() {
        r0 = floatBitsToUint(in_attr2[3]);
        p0 = ((int(r0)) >= (int(0x1u))) && true;
        if (p0) { r1 = floatBitsToUint(float(int(r0))); }
        { vec4 tv = texelFetch(t2_1, ivec2(int(r0), 0), 0); r2 = floatBitsToUint(tv[0]); }
}"""  # a float attribute whose bits are also compared as an integer: not an integer one

FS_RAW = """layout(location = 0) flat in vec4 in_attr0;
layout(location = 0) out vec4 out_color0;
void main() {
        r0 = floatBitsToUint(in_attr0[0]);
        out_color0[0] = uintBitsToFloat(r0); return;
}"""  # DrivePassShaderDefault's fragment program 664 (into R8UI)


def test_texel_coordinate_input_is_integer():
    assert int_hints.hints(VS_TEXEL, "vs", {}) == {"int_inputs": 1}


def test_input_with_other_uses_is_not():
    assert int_hints.hints(VS_SIGN_TEST, "vs", {}) == {}


def test_flat_copy_fragment_outputs_are_raw():
    assert int_hints.hints(FS_RAW, "ps", {"textures": []}) == {"raw_outputs": 1}


def test_fragment_with_smooth_input_or_texture_is_not():
    assert int_hints.hints(FS_RAW.replace(" flat in", " in"), "ps", {}) == {}
    assert int_hints.hints(FS_RAW, "ps", {"textures": [{"binding": 0}]}) == {}
