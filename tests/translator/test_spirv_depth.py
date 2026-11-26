"""The runtime's -1..1 depth rewrite (runtime/gfx/vk/pipe/vk_spirv_depth.c, through t_spirv_depth.c) on shaders shaped
as the translator writes them: a vertex shader that writes gl_Position one component at a time from a function of its
own and returns early (sm86 glsl/mem.py, core.py), the mesh-as-compute vertex shader (sm86 MESH_CS_VS, imported), a
mesh shader whose invocation writes its own vertex (sm86 MESH_MAIN) and a module whose Position variable is not in a
block. Each rewritten module must pass spirv-val, and before every OpReturn of main the position is read whole, its z
made (z + w) * 0.5 and stored back. A fragment shader is left alone. Needs glslangValidator and spirv-val (skipped
without them); no game, no GPU.

Usage: uv run pytest tests/translator/test_spirv_depth.py
"""

import re
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
TOOLS = {t: shutil.which(t) for t in ("glslangValidator", "spirv-val", "spirv-dis", "spirv-as")}
CC = shutil.which("clang") or shutil.which("cc")
pytestmark = pytest.mark.skipif(not all(TOOLS.values()) or not CC, reason="needs glslang, SPIRV-Tools and a C compiler")

VS = """#version 460
layout(location = 0) in vec4 a;
layout(location = 0) out vec4 o;
void sm_main() { gl_Position[0] = a.x; gl_Position[1] = a.y; gl_Position[2] = a.z; gl_Position[3] = a.w; o = a; }
void main() { gl_Position = vec4(0.0, 0.0, 0.0, 1.0); sm_main(); if (a.x > 5.0) return; o.x += 1.0; }
"""

MESH = """#version 460
#extension GL_EXT_mesh_shader : require
layout(local_size_x = 32) in;
layout(triangles, max_vertices = 32, max_primitives = 64) out;
layout(location = 0) out vec4 out_attr0[];
shared uint mv[128];
void main() {
    uint v = gl_LocalInvocationIndex;
    mv[v] = v;
    barrier();
    SetMeshOutputsEXT(32u, 1u);
    gl_MeshVerticesEXT[v].gl_Position = uintBitsToFloat(uvec4(mv[v], mv[32u + v], mv[64u + v], mv[96u + v]));
    out_attr0[v] = vec4(1.0);
    if (v == 0u) gl_PrimitiveTriangleIndicesEXT[0] = uvec3(0u, 1u, 2u);
}
"""

FS = """#version 460
layout(location = 0) out vec4 c;
void main() { c = vec4(1.0); }
"""

# A vertex shader whose Position is a plain Output variable, not a member of gl_PerVertex
LOOSE = """OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint Vertex %main "main" %pos %a
OpDecorate %pos BuiltIn Position
OpDecorate %a Location 0
%void = OpTypeVoid
%fn = OpTypeFunction %void
%float = OpTypeFloat 32
%v4 = OpTypeVector %float 4
%pv4o = OpTypePointer Output %v4
%pv4i = OpTypePointer Input %v4
%pos = OpVariable %pv4o Output
%a = OpVariable %pv4i Input
%main = OpFunction %void None %fn
%l = OpLabel
%x = OpLoad %v4 %a
OpStore %pos %x
OpReturn
OpFunctionEnd
"""


@pytest.fixture(scope="module")
def patcher(tmp_path_factory: pytest.TempPathFactory) -> Path:
    exe = tmp_path_factory.mktemp("spirv-depth") / "t_spirv_depth"
    pipe = ROOT / "runtime/gfx/vk/pipe"
    includes = {ROOT / "runtime"} | {p.parent for p in (ROOT / "runtime").rglob("*.h")}
    subprocess.run([CC, "-O1", "-Wall", "-Werror", *[f"-I{p}" for p in sorted(includes)],
                    str(ROOT / "tests/translator/t_spirv_depth.c"), str(pipe / "vk_spirv_depth.c"), "-o", str(exe)],
                   check=True)  # fmt: skip
    return exe


def compile_glsl(tmp: Path, name: str, src: str) -> Path:
    path = tmp / name
    path.write_text(src)
    subprocess.run([TOOLS["glslangValidator"], "-V", "--target-env", "vulkan1.3", str(path), "-o", f"{path}.spv"],
                   check=True, capture_output=True)  # fmt: skip
    return Path(f"{path}.spv")


def validate(path: Path) -> None:
    p = subprocess.run([TOOLS["spirv-val"], "--target-env", "vulkan1.3", str(path)], capture_output=True, text=True)
    assert p.returncode == 0, p.stderr


def rewrite(patcher: Path, src: Path) -> tuple[int, Path]:
    dst = src.with_suffix(".depth.spv")
    return subprocess.run([str(patcher), str(src), str(dst)]).returncode, dst


def main_body(path: Path) -> list[str]:
    """The disassembled lines of main, from OpFunction to OpFunctionEnd."""
    dis = subprocess.run([TOOLS["spirv-dis"], "--raw-id", str(path)], capture_output=True, text=True, check=True).stdout
    lines = [ln.strip() for ln in dis.splitlines()]
    name = next(re.search(r'OpEntryPoint \w+ (%\d+) "main"', ln).group(1) for ln in lines if "OpEntryPoint" in ln)
    start = next(i for i, ln in enumerate(lines) if ln.startswith(f"{name} = OpFunction "))
    end = next(i for i in range(start, len(lines)) if lines[i] == "OpFunctionEnd")
    return lines[start : end + 1]


CONVERT = ["OpCompositeExtract", "OpCompositeExtract", "OpFAdd", "OpFMul", "OpCompositeInsert"]


def ops_of(lines: list[str]) -> list[str]:
    return [re.sub(r"^%\d+ = ", "", ln).split()[0] for ln in lines]


def halves(path: Path) -> set[str]:
    """The ids of the module's float constants 0.5."""
    dis = subprocess.run([TOOLS["spirv-dis"], "--raw-id", str(path)], capture_output=True, text=True, check=True).stdout
    return set(re.findall(r"^\s*(%\d+) = OpConstant %\d+ 0\.5$", dis, re.M))


def check_convert(seq: list[str], half: set[str]) -> None:
    """z (2) and w (3) of one vec4, their sum times 0.5, inserted back at z of that vec4."""
    ex_z, ex_w, add, mul, ins = (ln.split() for ln in seq)
    assert ex_z[-1] == "2" and ex_w[-1] == "3" and ex_z[-2] == ex_w[-2], seq
    assert add[-2:] == [ex_z[0], ex_w[0]], seq
    assert mul[-2] == add[0] and mul[-1] in half, seq
    assert ins[-3:] == [mul[0], ex_z[-2], "2"], seq


def check_vertex(path: Path, block: bool = True) -> None:
    """Before every OpReturn of main: load the position, convert its z, store it back."""
    body, half = main_body(path), halves(path)
    returns = [i for i, ln in enumerate(body) if ln == "OpReturn"]
    assert returns and half
    want = ["OpAccessChain", "OpLoad", *CONVERT, "OpStore"] if block else ["OpLoad", *CONVERT, "OpStore"]
    for r in returns:
        seq = body[r - len(want) : r]
        assert ops_of(seq) == want, seq
        check_convert(seq[-6:-1], half)
        load, store = seq[-7].split(), seq[-1].split()
        assert load[-1] == store[1] and store[2] == seq[-2].split()[0], seq  # stored back where it was read


def test_vertex_shader(patcher: Path, tmp_path: Path) -> None:
    src = compile_glsl(tmp_path, "vs.vert", VS)
    code, dst = rewrite(patcher, src)
    assert code == 0
    validate(dst)
    check_vertex(dst)
    assert sum(ln == "OpReturn" for ln in main_body(dst)) == 2  # the early return too


def test_mesh_as_compute_vertex_shader(patcher: Path, tmp_path: Path) -> None:
    sys.path[:0] = [str(ROOT / "sm86/translate"), str(ROOT / "sm86/translate/frontend")]
    from glsl.head import MESH_CS_VS

    src = compile_glsl(tmp_path, "mesh_cs.vert", MESH_CS_VS)
    code, dst = rewrite(patcher, src)
    assert code == 0
    validate(dst)
    check_vertex(dst)


def test_mesh_shader(patcher: Path, tmp_path: Path) -> None:
    """Mesh outputs must not be read: the store of a vertex's position stores the converted value instead."""
    src = compile_glsl(tmp_path, "ms.mesh", MESH)
    code, dst = rewrite(patcher, src)
    assert code == 0
    validate(dst)
    before, body = main_body(src), main_body(dst)
    assert "OpReturn" in body and len(body) == len(before) + len(CONVERT)
    stores = [
        i
        for i, ln in enumerate(body)
        if ln.startswith("OpStore") and i >= len(CONVERT) and ops_of(body[i - len(CONVERT) : i]) == CONVERT
    ]
    assert len(stores) == 1, body
    i = stores[0]
    check_convert(body[i - len(CONVERT) : i], halves(dst))
    value = body[i].split()[2]
    assert body[i - 1].startswith(f"{value} = OpCompositeInsert")  # the converted value is what is stored


MESH_COMPONENT = MESH.replace(
    "gl_MeshVerticesEXT[v].gl_Position = ",
    "gl_MeshVerticesEXT[v].gl_Position.z = 0.0; gl_MeshVerticesEXT[v].gl_Position.xyw = vec3(0.0); out_attr0[v] = ",
)


def test_mesh_component_store_refused(patcher: Path, tmp_path: Path) -> None:
    """One component of a mesh vertex's position cannot be converted alone."""
    code, dst = rewrite(patcher, compile_glsl(tmp_path, "msc.mesh", MESH_COMPONENT))
    assert code == 1 and not dst.exists()


def test_position_outside_a_block(patcher: Path, tmp_path: Path) -> None:
    src = tmp_path / "loose.spvasm"
    src.write_text(LOOSE)
    spv = tmp_path / "loose.spv"
    subprocess.run([TOOLS["spirv-as"], "--target-env", "vulkan1.3", str(src), "-o", str(spv)], check=True)
    code, dst = rewrite(patcher, spv)
    assert code == 0
    validate(dst)
    check_vertex(dst, block=False)


def test_fragment_shader_left_alone(patcher: Path, tmp_path: Path) -> None:
    code, dst = rewrite(patcher, compile_glsl(tmp_path, "fs.frag", FS))
    assert code == 1 and not dst.exists()


@pytest.mark.parametrize("words", [b"", b"\x03\x02\x23\x07" + b"\0" * 12 + b"\xff\xff\xff\xff"])
def test_bad_modules(patcher: Path, tmp_path: Path, words: bytes) -> None:
    src = tmp_path / "bad.spv"
    src.write_bytes(words)
    code, _ = rewrite(patcher, src)
    assert code == 1
