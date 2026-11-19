"""SM86 -> GLSL: which vertex inputs and fragment outputs of a program carry integers, read from its GLSL.

The registers are uint and every input arrives as float bits, so a program's GLSL does not say which vertex attributes
have integer formats or which outputs go to integer targets: a draw tells the runtime (vk_pipeline_answers), which then
builds an integer variant of the shader (gfx_shader.c). The setup's prewarm (runtime/gfx/vk/pipe/vk_pipe_pairs.c)
builds those variants before any draw, so it needs a hint from the program itself. A census of 6 165 pipelines our
runs recorded (17/11/2026) had:

- integer vertex attributes at locations 6 and 7 (skinning indices), which the prewarm takes as integer in any program
  that reads them, and RGBA16I at location 0 in two programs of DriveRefreshUber.Procedural, whose GLSL uses those
  bits only as texel coordinates. int_inputs finds locations used like that: every component read is used only as an
  integer texel coordinate or buffer index, never as a float. Over the recorded programs it names those two and no
  other; over the copy, 3 programs. Wider rules (any integer conversion or comparison) named 1 600 float attributes:
  NVIDIA's compiler compares float bits as integers and the game keeps integers in RGBA32F attributes.
- one fragment program drawing into an integer target (R8UI): DrivePassShaderDefault's 664, which copies a flat input
  to its output. raw_outputs names the outputs of programs whose inputs are all flat and that sample nothing: in the
  copy, that one and 224 of the same archive.

The reflection gets "int_inputs" (vertex: Location bit mask) and "raw_outputs" (fragment: output bit mask) when not 0.
"""

import re

_LOAD = re.compile(r"^\s*(r\d+) = floatBitsToUint\(in_attr(\d+)\[\d\]\);")
_INPUT = re.compile(r"^layout\(location = \d+\)( flat)? in ", re.M)
_OUTPUT = re.compile(r"\bout_color(\d)\[")


def int_inputs(src: str) -> int:
    """Locations whose components are all used only as integer texel coordinates or buffer indices."""
    lines = src.split("\n")
    as_int = as_other = 0
    for i, line in enumerate(lines):
        m = _LOAD.match(line)
        if not m:
            continue
        reg, bit = m.group(1), 1 << int(m.group(2))
        int_use = re.compile(r"(texelFetch|imageLoad|imageStore)\([^;]*\bint\(" + reg + r"\)|\.d\[\(\(" + reg + r"\b")
        assign = re.compile(r"(^\s*|[{;]\s*)" + reg + r" = ")
        use = re.compile(r"\b" + reg + r"\b")
        for later in lines[i + 1 :]:
            if int_use.search(later):
                as_int |= bit
            elif use.search(assign.sub(r"\1", later)):
                as_other |= bit
            if assign.search(later):  # overwritten: later uses are of another value
                break
    return as_int & ~as_other


def raw_outputs(src: str, refl: dict) -> int:
    """Outputs of a fragment program that only moves flat inputs and constants: no texture or image, all inputs flat."""
    flat = _INPUT.findall(src)
    if not flat or not all(flat) or refl.get("textures") or refl.get("images"):
        return 0
    return sum({1 << int(t) for t in _OUTPUT.findall(src)})


def hints(src: str, stage: str, refl: dict) -> dict:
    """The reflection entries for src, a program of stage ("vs", "ps", ...); empty when it has none."""
    if stage == "vs" and (m := int_inputs(src)):
        return {"int_inputs": m}
    if stage == "ps" and (m := raw_outputs(src, refl)):
        return {"raw_outputs": m}
    return {}
