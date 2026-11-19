#!/usr/bin/env python3
"""HDR tone mapping after RenoDX (MKW_HDR_TONEMAP, gfx_shader.c): replacement SPIR-V for the programs that hold the
game's final tone curve, written to <spirv>-renodx/<key>.spv (beside spirv/, never in it: a write in spirv/ makes its
shaders.pack stale); the runtime loads them in place of <spirv>/<key>.spv and gives them the display's headroom
(specialization constant RDX_HEADROOM_ID, gfx_rdx_patch in gfx_rdx.c).

How MKW draws HDR (frame dumps of 18/11/2026, title screen and free roam, HDR10, peak 1200, paper white 480):
  - the scene is linear Display-P3 (the PQ program's matrix is P3 to BT.2020 times the nits of 1.0);
  - a LUT pass grades it (a 32^3 rgba16f LUT read through the bits of a half float); its output is still
    scene-linear (up to 68 in the dump), so it stays the game's and the course's grade with it;
  - the final curve program (one of 24 variants, the post-process uber programs among them) multiplies by an
    exposure, puts each channel through a shoulder (linear to c[1].x, then 1 - 2^(x c[1].y + c[1].z), towards 1),
    and in its HDR mode (c[7].x = 6) expands that by up to c[6].x = 1.5 (P / (1 + (P - 1) (1 - s)^c[6].z), the
    channels' factors pulled together by c[6].w), times c[5].w = 1/1.5. Output 1.0 is 1.5 times paper white: the
    game's HDR never goes above 720 nits on a 1200 nit display (measured: 723 at most, vanilla).
Here, in the HDR mode only, the final colour becomes PsychoV35 of the exposed scene: its mid grey where the game's
expansion puts it, highlights rolled off to the display's peak over paper white (the headroom the runtime gives),
times c[5].w as the game's own. The other modes (SDR) stay as they are. PsychoV35 works in BT.709 (the input
converted from P3) and gives P3, fitted under the peak in BT.2020.

PsychoV35: RenoDX, github.com/clshortfuse/renodx (Copyright (c) 2025 Carlos Lopez Jr., MIT), its
src/shaders/tonemap/psychov/test35.hlsl as src/games/zelda-oot-oz/scene_tonemap.slangi calls it, at that mod's
defaults; written here in GLSL and checked against RenoDX's path to 1.5e-6 (relative) on 2000 random inputs,
18/11/2026.

Programs are found by their constants, optimized with spirv-opt (the setup's SPIR-V is O0), decompiled with spirv-cross, edited as GLSL, compiled with glslangValidator
and checked with spirv-val. The setup runs it after the shaders step, under its own key (tools/prepare/shaderstep.py
tonemap: a change here redoes only this, seconds, not the translation), with the tools the package carries beside
glslc (conda-forge's SPIRV-Cross, glslang, SPIRV-Tools; tools/build/dist.py); a Vulkan SDK's Bin works too. A
program that does not take the edit stays the game's. Usage:
    python tools/shaders/renodx.py --spirv "<game>/spirv" [--out <folder>] [--tools <folder>] [--keep-glsl] [--clean]
"""

import argparse
import glob
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

RDX_HEADROOM_ID = 30000  # gfx_shader.c RDX_HEADROOM_ID: peak over paper white (set by gfx_rdx.c)

RDX = r"""
// ---- RenoDX PsychoV35 for the game's HDR tone curve (tools/shaders/renodx.py; RenoDX MIT)
layout(constant_id = 30000) const float RDX_HEADROOM = 0.0;   // peak / paper white, from the runtime; 0: the game's
layout(constant_id = 30001) const float RDX_SPLIT = 0.0;      // MKW_RDX_SPLIT: pixels left of it stay the game's
const float RDX_REF_EXPOSURE = 1.4827717799416489;   // ACES(0.18) / 0.18
const float RDX_CONE_EXPONENT = 1.3355037565264822;
const float RDX_HUE_CLIP = 0.5;
// (rows written as columns: v * M is M times v)
const mat3 RDX_P3_TO_709 = mat3(1.22493971, -0.22493853, -0.00000152,
                                -0.04205663, 1.04205709, 0.00000141,
                                -0.01963776, -0.07863596, 1.09827377);
const mat3 RDX_TO_RATIO = mat3(1.51324785, 3.64325256, 0.39905514,
                               0.55073221, 4.31962362, 0.68519973,
                               0.14901612, 0.51444296, 4.89209648);
const mat3 RDX_TO_2020 = mat3(3.09283412, -2.19752558, 0.10469146,
                              -0.24194003, 1.40626676, -0.16432673,
                              -0.05344957, 0.03725273, 1.01619683);
const mat3 RDX_TO_P3 = mat3(4.22701706, -3.35165471, 0.12463762,
                            -0.46166943, 1.65594667, -0.19427713,
                            -0.04087748, 0.00411622, 1.03676126);
const vec3 RDX_Y709 = vec3(0.2126390059, 0.7151686788, 0.0721923154);

float rdx_min3(vec3 v) { return min(min(v.x, v.y), v.z); }
float rdx_max3(vec3 v) { return max(max(v.x, v.y), v.z); }
float rdx_mean3(vec3 v) { return (v.x + v.y + v.z) * (1.0 / 3.0); }

// PsychoV35 of u (linear BT.709); the result in linear Display-P3, fitted under peak in BT.2020
vec3 rdx_psychov35(vec3 u, float gain, float peak) {
    vec3 m = abs(u);
    vec3 fit = clamp(m * (m * 2.51 + 0.03) / (m * (m * 2.43 + 0.59) + 0.14), 0.0, 1.0);
    float fmin = rdx_min3(fit), frange = rdx_max3(fit) - fmin;
    bool neutral = frange == 0.0;
    float span = neutral ? 1.0 : frange, low = rdx_min3(m), wide = rdx_max3(m) - low;
    vec3 moved = (fit - fmin) / span * wide + low;
    moved = mix(moved, -moved, lessThan(u, vec3(0.0)));
    if (neutral) moved = u;
    vec3 bent = (moved - u) * RDX_HUE_CLIP + u;
    float y = dot(RDX_Y709, u), by = dot(RDX_Y709, bent);
    if (y > 0.0 && by > 0.0) u = bent * (y / by);

    float anchor = clamp(gain * (0.18 * RDX_REF_EXPOSURE) / peak, 1e-6, 0.95);
    float rest = 1.0 - anchor;
    float expo = RDX_CONE_EXPONENT / rest, offs = log2(anchor / rest);
    vec3 ratio = max(u * RDX_TO_RATIO, vec3(0.0));
    vec3 latent = expo * log2(ratio) + offs;
    vec3 response = 1.0 / (1.0 + exp2(-latent));
    vec3 deficit = 1.0 / (1.0 + exp2(latent));
    float rmean = rdx_mean3(response), dmean = rdx_mean3(deficit);
    vec3 residual = rmean > 0.5 ? dmean - deficit : response - rmean;
    float radius = length(residual);
    vec3 dir = radius > 0.0 ? residual / radius : vec3(0.0);
    vec3 source = ratio - rdx_mean3(ratio);
    float sradius = length(source);
    vec3 middle = source / sradius + dir;
    float mlen = length(middle);
    if (sradius > 0.0 && radius > 0.0 && mlen > 0.0) dir = middle / mlen;

    float level = peak * rmean, headroom = peak * dmean;
    vec3 off = peak * radius * dir;
    vec3 t = off * RDX_TO_2020;
    float s = 1.0;
    for (int i = 0; i < 3; i++)
        s = min(s, t[i] > 0.0 ? headroom / t[i] : t[i] < 0.0 ? level / -t[i] : 1.0);
    return max(level + (off * s) * RDX_TO_P3, vec3(0.0));
}

// x: the exposed scene (P3, 1.0 = paper white) as the game's shoulder takes it; P, k: its HDR expansion's c[6].x and
// c[6].z; scale: c[5].w. Mid grey (0.18) lands where the game's expansion puts it.
vec3 rdx_tone(vec3 x, float P, float k, float scale) {
    float game_mid = 0.18 * P / (1.0 + (P - 1.0) * pow(0.82, k));
    float peak = max(RDX_HEADROOM > 0.0 ? RDX_HEADROOM : P, 1.0);
    return rdx_psychov35(x * RDX_P3_TO_709, game_mid / (0.18 * RDX_REF_EXPOSURE), peak) * scale;
}
"""


class Skip(Exception):
    pass


def rewrite(glsl):
    # the HDR mode's test: if (M == 6.0), M the flushed c[7].x of a constant bank U
    mode = None
    for m in re.finditer(r"if \((_\d+) == 6\.0\)", glsl):
        v = m.group(1)
        d = re.search(r"float " + v + r" = \(abs\((_\d+)\) < [^;]*\? 0\.0 : \1;", glsl)
        src = d.group(1) if d else v
        u = re.search(r"float " + src + r" = uintBitsToFloat\((_\d+)\._m0\[7\]\.x\);", glsl)
        if u:
            mode = (v, u.group(1))
            break
    if not mode:
        raise Skip("no HDR mode test")
    ubo = mode[1]
    # the shoulder: exp2(fma(X, A, B)) for the three exposed channels X (a float, or a uint's bits), one A and B
    sh = re.findall(
        r"exp2\(fma\((_\d+|uintBitsToFloat\(_\d+\)), (_\d+|uintBitsToFloat\([^()]*\)), "
        r"(_\d+|uintBitsToFloat\([^()]*\))\)\)",
        glsl,
    )
    groups = {}
    for x, a, b in sh:
        groups.setdefault((a, b), set()).add(x)
    xs = next((v for v in groups.values() if len(v) == 3), None)
    if not xs:
        raise Skip("no shoulder of three channels")
    xvar = {x: re.search(r"_\d+", x).group(0) for x in xs}
    # one declared inside a block is copied, where it is made, to a variable of main's top level
    captured = {}
    for x, v in xvar.items():
        if re.search(r"^    (?:precise )?(?:float|uint) " + v + r"\b", glsl, re.M):
            continue
        d = list(re.finditer(r"^(\s+)(?:precise )?(?:float|uint) " + v + r" = .*;$", glsl, re.M))
        if len(d) != 1:
            raise Skip("an exposed channel not made once")
        captured[x] = (d[0].end(), d[0].group(1), "_rdx_c%d" % len(captured))
    outs = re.findall(r"^    (_\d+)\.([xyzw]) = (.*);$", glsl, re.M)
    out = {k: (o, rhs) for o, k, rhs in outs}
    if not set("xyz") <= set(out) or len({out[k][0] for k in "xyz"}) != 1:
        raise Skip("target 0 not written once per channel at the end")
    o = out["x"][0]
    # each channel's exposed value: from its write back to the HDR mode's block, then to the nearest shoulder input
    m6 = re.search(r"if \(" + mode[0] + r" == 6\.0\)\s*\{", glsl)
    i, depth = m6.end(), 1
    while depth:
        depth += {"{": 1, "}": -1}.get(glsl[i], 0)
        i += 1
    block = glsl[m6.end() : i]
    assigns = {}
    for v, rhs in re.findall(r"^\s*(?:precise )?(?:float |uint |int |bool )?(_\d+) = (.*);$", glsl, re.M):
        assigns.setdefault(v, []).append(rhs)
    in_block = dict(re.findall(r"^\s*(_\d+) = floatBitsToUint\((_\d+)\);$", block, re.M))

    def nearest(start, goal, limit):
        seen, level = set(start), list(start)
        for _ in range(limit):
            hit = {g for g in goal for v in level if goal[g] == v}
            if hit:
                return hit
            nxt = []
            for v in level:
                for rhs in assigns.get(v, []):
                    for t in re.findall(r"_\d+", rhs):
                        if t not in seen:
                            seen.add(t)
                            nxt.append(t)
            level = nxt
        return set()

    chan = {}
    for c in "xyz":
        start = re.findall(r"_\d+", out[c][1])
        q = nearest(start, {u: u for u in in_block}, 10)
        if len(q) != 1:
            raise Skip("channel %s not traced to the HDR mode's block" % c)
        x = nearest([in_block[q.pop()]], xvar, 6)
        if len(x) != 1:
            raise Skip("channel %s not traced to one shoulder input" % c)
        chan[c] = x.pop()
    if len(set(chan.values())) != 3:
        raise Skip("the channels share a shoulder input")
    # the HDR mode's test value too, when it is made inside a block (the race's program, f07750bf…)
    mode_var = mode[0]
    if not re.search(r"^    (?:precise )?float " + mode_var + r"\b", glsl, re.M):
        d = list(re.finditer(r"^(\s+)(?:precise )?float " + mode_var + r" = .*;$", glsl, re.M))
        if len(d) != 1:
            raise Skip("the HDR mode's test not made once")
        captured[mode_var] = (d[0].end(), d[0].group(1), "_rdx_mode")
        mode = ("_rdx_mode", mode[1])
    for x, (pos, ind, name) in sorted(captured.items(), key=lambda kv: -kv[1][0]):
        glsl = glsl[:pos] + "\n%s%s = %s;" % (ind, name, x) + glsl[pos:]
    for c in "xyz":
        if chan[c] in captured:
            chan[c] = captured[chan[c]][2]
    if captured:
        i = glsl.index("void main()\n{\n") + len("void main()\n{\n")
        glsl = glsl[:i] + "".join("    float %s = 0.0;\n" % v[2] for v in captured.values()) + glsl[i:]
    lines = glsl.split("\n")
    last = max(i for i, l in enumerate(lines) if re.match(r"^    " + o + r"\.[xyz] = ", l))
    f = lambda c: "uintBitsToFloat(%s._m0[%s)" % (ubo, c)
    lines[last + 1 : last + 1] = [
        "    if (%s == 6.0 && gl_FragCoord.x >= RDX_SPLIT)   // RenoDX: the HDR mode's tone curve" % mode[0],
        "    {",
        "        vec3 _rdx = rdx_tone(vec3(%s, %s, %s), %s, %s, %s);"
        % (chan["x"], chan["y"], chan["z"], f("6].x"), f("6].z"), f("5].w")),
        "        %s.x = _rdx.x;" % o,
        "        %s.y = _rdx.y;" % o,
        "        %s.z = _rdx.z;" % o,
        "    }",
    ]
    text = "\n".join(lines)
    i = text.index("void main()")
    return text[:i] + RDX.strip() + "\n\n" + text[i:]


def tool(dirs, name):
    exe = name + (".exe" if os.name == "nt" else "")
    for d in dirs:
        p = os.path.join(d, exe)
        if os.path.isfile(p):
            return p
    p = shutil.which(name)
    if not p:
        sys.exit("renodx.py: %s not found (the package's setup/glslc, or a Vulkan SDK's Bin: --tools)" % name)
    return p


def candidate(path):
    d = open(path, "rb").read()
    ws = set(struct.unpack("<%dI" % (len(d) // 4), d[: len(d) // 4 * 4]))
    bits = lambda v: struct.unpack("<I", struct.pack("<f", v))[0]
    need = [
        6.0,
        -1.44269502162933349609375,
        0.586600005626678466796875,
        0.715200006961822509765625,
        0.2125999927520751953125,
    ]
    return all(bits(v) in ws for v in need)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--spirv", required=True, help="the game's spirv folder")
    ap.add_argument("--tools", action="append", default=[], help="folder of spirv-cross, glslangValidator, spirv-val")
    ap.add_argument("--out", help="where the programs go (default <spirv>-renodx, where the runtime reads them)")
    ap.add_argument("--clean", action="store_true", help="remove the generated folder and stop")
    ap.add_argument("--keep-glsl", action="store_true", help="also write the edited GLSL beside the SPIR-V")
    a = ap.parse_args()
    dirs = a.tools + ([os.path.join(os.environ["VULKAN_SDK"], "Bin")] if os.environ.get("VULKAN_SDK") else [])
    out = a.out or os.path.normpath(a.spirv) + "-renodx"
    shutil.rmtree(out, ignore_errors=True)
    if a.clean:
        return
    cross, glslang, val = tool(dirs, "spirv-cross"), tool(dirs, "glslangValidator"), tool(dirs, "spirv-val")
    opt = tool(dirs, "spirv-opt")
    os.makedirs(out)
    made, skipped = 0, []
    with tempfile.TemporaryDirectory() as tmp:
        for spv in sorted(glob.glob(os.path.join(a.spirv, "*.spv"))):
            key = os.path.basename(spv)[:-4]
            if not candidate(spv):
                continue
            # optimized first: the setup's SPIR-V is O0 with the translator's registers as variables (r0.., p0..,
            # reused), where the dataflow below cannot be followed; -O gives SSA values again, and keeps the bindings
            # and locations the runtime's reflection of the game's program names (11801bf9 on main 7e49ba8: 0 of 24
            # edited without it, 19/11/2026)
            ospv = os.path.join(tmp, key + ".opt.spv")
            r = subprocess.run([opt, "-O", "--strip-debug", spv, "-o", ospv], capture_output=True, text=True)
            if r.returncode:
                skipped.append((key, "spirv-opt: " + r.stderr.strip()[:80]))
                continue
            r = subprocess.run([cross, ospv], capture_output=True, text=True)
            if r.returncode:
                skipped.append((key, "spirv-cross: " + r.stderr.strip()[:80]))
                continue
            try:
                text = rewrite(r.stdout)
            except Skip as e:
                skipped.append((key, str(e)))
                if "no HDR mode test" not in str(e):  # (the runtime logs these as left the game's)
                    open(os.path.join(out, key + ".none"), "w").close()
                continue
            src = os.path.join(tmp, key + ".frag")
            open(src, "w", newline="\n").write(text)
            dst = os.path.join(out, key + ".spv")
            c = subprocess.run(
                [glslang, "-V", "--target-env", "vulkan1.3", "-S", "frag", "-o", dst, src],
                capture_output=True,
                text=True,
            )
            if c.returncode == 0:
                c = subprocess.run([val, "--target-env", "vulkan1.3", dst], capture_output=True, text=True)
            if c.returncode:
                msg = [l for l in (c.stdout + c.stderr).splitlines() if "ERROR" in l or "error" in l]
                skipped.append((key, "build: " + (msg[0] if msg else "failed")[:140]))
                if os.path.exists(dst):
                    os.remove(dst)
                open(os.path.join(out, key + ".none"), "w").close()
                continue
            if a.keep_glsl:
                shutil.copy(src, os.path.join(out, key + ".frag"))
            made += 1
            print("%s: HDR tone curve replaced" % key[:16])
    for k, why in skipped:
        print("skip %s: %s" % (k[:16], why))
    print("renodx.py: %d programs, %d left as the game's" % (made, len(skipped)))
    if skipped and not made:
        # every tone curve found and none edited: the translator's output changed shape (main 7e49ba8's O0 SPIR-V did,
        # 19/11/2026). An error, so the setup does not record the tone mapper as made and tries again next time
        sys.exit("renodx.py: no tone curve program could be edited")


if __name__ == "__main__":
    main()
