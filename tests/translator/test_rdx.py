"""The RenoDX tone mapper without the game or a GPU: tools/shaders/renodx.py's edit of a final tone curve program
(spirv-cross shaped GLSL made here, with the HDR mode test, the three-channel shoulder and the target written per
channel) and the runtime's constant patch (runtime/gfx/nvn/host/gfx_rdx.c, built with t_rdx.c) on synthetic SPIR-V.
When glslangValidator is on PATH the edited GLSL is compiled too.

Usage: uv run pytest tests/translator/test_rdx.py
"""

import shutil
import struct
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/shaders"))
import renodx  # noqa: E402

# ---- renodx.py


def program(*, mode: bool = True, shared: bool = False, nested: bool = False) -> str:
    """A final tone curve program as spirv-cross writes one: c[7].x flushed and tested against 6 (the HDR mode), each
    exposed channel through exp2(fma(x, A, B)), the HDR mode's block rewriting each channel's bits, then target 0
    written once per channel. shared: two channels from one input; nested: one input declared inside a block."""
    z = "_21" if shared else "_22"
    if nested:  # its shoulder in the same block, into a variable of main's top level
        zpart = ("    float _42 = 0.0;\n    if (_4.x > 0.0)\n    {\n        float _22 = _4.z;\n"
                 "        _42 = 1.0 - exp2(fma(_22, _30, _31));\n    }\n")  # fmt: skip
    else:
        zpart = f"    float _22 = _4.z;\n    float _42 = 1.0 - exp2(fma({z}, _30, _31));\n"
    test = "if (_10 == 6.0)" if mode else "if (_10 == 5.0)"
    return f"""#version 450

layout(set = 0, binding = 3, std140) uniform U_t
{{
    uvec4 _m0[8];
}} _8;

layout(location = 0) in vec3 _4;
layout(location = 0) out vec4 _5;

void main()
{{
    float _9 = uintBitsToFloat(_8._m0[7].x);
    float _10 = (abs(_9) < 1.1754943508222875079687365372222e-38) ? 0.0 : _9;
    float _20 = _4.x;
    float _21 = _4.y;
    float _30 = uintBitsToFloat(_8._m0[1].y);
    float _31 = uintBitsToFloat(_8._m0[1].z);
    float _40 = 1.0 - exp2(fma(_20, _30, _31));
    float _41 = 1.0 - exp2(fma(_21, _30, _31));
{zpart}    uint _50 = floatBitsToUint(_40);
    uint _51 = floatBitsToUint(_41);
    uint _52 = floatBitsToUint(_42);
    {test}
    {{
        float _60 = _40 * 1.5;
        float _61 = _41 * 1.5;
        float _62 = _42 * 1.5;
        _50 = floatBitsToUint(_60);
        _51 = floatBitsToUint(_61);
        _52 = floatBitsToUint(_62);
    }}
    _5.x = uintBitsToFloat(_50);
    _5.y = uintBitsToFloat(_51);
    _5.z = uintBitsToFloat(_52);
    _5.w = 1.0;
}}
"""


def test_rewrite_puts_psychov_in_the_hdr_mode_only() -> None:
    out = renodx.rewrite(program())
    c = "uintBitsToFloat(_8._m0[%s)"
    assert f"vec3 _rdx = rdx_tone(vec3(_20, _21, _22), {c % '6].x'}, {c % '6].z'}, {c % '5].w'});" in out
    # after the game's writes, in the HDR mode only, left of the split the game's
    tail = out[out.index("    _5.z = uintBitsToFloat(_52);") :]
    assert tail.index("if (_10 == 6.0 && gl_FragCoord.x >= RDX_SPLIT)") < tail.index("_5.x = _rdx.x;")
    # the RenoDX code before main, its headroom under the id the runtime patches
    assert out.index("vec3 rdx_psychov35(") < out.index("void main()")
    assert f"layout(constant_id = {renodx.RDX_HEADROOM_ID}) const float RDX_HEADROOM" in out
    assert "RenoDX MIT" in out


def test_rewrite_captures_a_channel_made_inside_a_block() -> None:
    out = renodx.rewrite(program(nested=True))
    assert "    float _rdx_c0 = 0.0;" in out
    assert "_rdx_c0 = _22;" in out
    assert "rdx_tone(vec3(_20, _21, _rdx_c0)," in out


@pytest.mark.parametrize(
    ("kw", "why"), [({"mode": False}, "no HDR mode test"), ({"shared": True}, "no shoulder of three channels")]
)
def test_rewrite_leaves_other_programs(kw: dict, why: str) -> None:
    with pytest.raises(renodx.Skip, match=why):
        renodx.rewrite(program(**kw))


@pytest.mark.parametrize("nested", [False, True])
def test_rewritten_glsl_compiles(tmp_path: Path, nested: bool) -> None:
    glslang = shutil.which("glslangValidator")
    if not glslang:
        pytest.skip("glslangValidator not on PATH")
    src = tmp_path / "p.frag"
    src.write_text(renodx.rewrite(program(nested=nested)), newline="\n")
    r = subprocess.run([glslang, "-V", "--target-env", "vulkan1.3", "-S", "frag", "-o", str(tmp_path / "p.spv"),
                        str(src)], capture_output=True, text=True)  # fmt: skip
    assert r.returncode == 0, r.stdout + r.stderr


def test_candidate_needs_the_curve_constants(tmp_path: Path) -> None:
    consts = (6.0, -1.44269502162933349609375, 0.586600005626678466796875, 0.715200006961822509765625,
              0.2125999927520751953125)  # fmt: skip
    bits = [f32(v) for v in consts]
    p = tmp_path / "a.spv"
    p.write_bytes(struct.pack(f"<{len(bits) + 5}I", 0x07230203, 0x10300, 0, 99, 0, *bits))
    assert renodx.candidate(str(p))
    p.write_bytes(struct.pack(f"<{len(bits) + 4}I", 0x07230203, 0x10300, 0, 99, 0, *bits[1:]))
    assert not renodx.candidate(str(p))


# ---- gfx_rdx.c


def op(code: int, *args: int) -> list[int]:
    return [((len(args) + 1) << 16) | code, *args]


def f32(v: float) -> int:
    return struct.unpack("<I", struct.pack("<f", v))[0]


def module() -> list[int]:
    """Two float specialization constants as renodx.py's programs declare them: %5 SpecId 30000, %6 SpecId 30001."""
    w = [0x07230203, 0x00010300, 0, 8, 0]
    w += op(17, 1) + op(14, 0, 1)  # Shader; Logical GLSL450
    w += op(71, 5, 1, 30000) + op(71, 6, 1, 30001)  # OpDecorate SpecId
    w += op(22, 4, 32)  # %4 float
    w += op(50, 4, 5, f32(0.0)) + op(50, 4, 6, f32(0.0))  # OpSpecConstant
    return w


@pytest.fixture(scope="module")
def patcher(tmp_path_factory: pytest.TempPathFactory) -> Path:
    cc = shutil.which("clang")
    if not cc:
        pytest.skip("Clang is required by the runtime headers")
    exe = tmp_path_factory.mktemp("rdx") / "t_rdx.exe"
    includes = [ROOT / "runtime", ROOT / "runtime/cpu", ROOT / "runtime/core/base",
                ROOT / "runtime/gfx/nvn/host", ROOT / "runtime/gfx/nvn/api", ROOT / "runtime/gfx/nvn/procs"]  # fmt: skip
    subprocess.run([cc, "-O1", *[f"-I{p}" for p in includes], str(ROOT / "tests/translator/t_rdx.c"),
                    str(ROOT / "runtime/gfx/nvn/host/gfx_rdx.c"), "-o", str(exe)], check=True)  # fmt: skip
    return exe


def patch(patcher: Path, tmp_path: Path, words: list[int], sid: int, value: float) -> tuple[int, list[int]]:
    src, dst = tmp_path / "in.spv", tmp_path / "out.spv"
    src.write_bytes(struct.pack(f"<{len(words)}I", *words))
    r = subprocess.run([str(patcher), str(src), str(dst), str(sid), repr(value)])
    data = dst.read_bytes()
    return r.returncode, list(struct.unpack(f"<{len(data) // 4}I", data))


def test_patch_sets_only_that_constant(patcher: Path, tmp_path: Path) -> None:
    w = module()
    first = len(w) - 5  # the first OpSpecConstant's value word
    found, out = patch(patcher, tmp_path, w, 30000, 2.5)
    assert found == 1
    assert [i for i in range(len(w)) if w[i] != out[i]] == [first]
    assert out[first] == f32(2.5) and out[-1] == f32(0.0)
    found, out = patch(patcher, tmp_path, w, 30001, 640.0)
    assert found == 1
    assert [i for i in range(len(w)) if w[i] != out[i]] == [len(w) - 1]
    assert out[-1] == f32(640.0)


def test_patch_without_the_constant_leaves_the_module(patcher: Path, tmp_path: Path) -> None:
    w = module()
    assert patch(patcher, tmp_path, w, 30002, 2.5) == (0, w)


def test_patch_stops_at_a_truncated_instruction(patcher: Path, tmp_path: Path) -> None:
    w = module()[:-1]  # the last OpSpecConstant claims 4 words, 3 are left
    w[len(w) - 4] = op(50, 4, 6, 0)[0]
    first_decoration = 5 + 2 + 3
    w[first_decoration] = (40 << 16) | 71  # and before it, a decoration that claims to run past the end
    assert patch(patcher, tmp_path, w, 30000, 2.5) == (0, w)
