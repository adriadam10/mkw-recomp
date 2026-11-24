"""The character and kart lists' capture masks on a wider output: the pane-matrix hook's sites and the real
host_pane_matrix / host_ui_proj of patches.c over authored pane fixtures (names and numbers of 1.8.0's CharaSelect_00
and MachineSelect_00 layouts only); no game required."""

import re
import subprocess
import sys
from types import SimpleNamespace

import pytest

from test_aspect import LIBM, ROOT, function

sys.path.insert(0, str(ROOT / "recomp"))
import hud_layouts  # noqa: E402
import patches  # noqa: E402

PANE_SITES = {0x2F65C4: 0x9454090B, 0x17EC8E8: 0x94003042, 0x17F9724: 0x97FFFCB3}  # bl main+0x17f89f0


def test_pane_matrix_sites_call_the_builder_and_the_registered_hook():
    table = patches.load(
        ROOT / "recomp/patches-mkw-180.txt",
        [("main", 0, 0x37F0000), ("subsdk0", 0x5000000, 0), ("subsdk1", 0x6000000, 0), ("sdk", 0x7000000, 0)],
    )
    source = (ROOT / "runtime/core/boot/patches.c").read_text()
    host = int(re.search(r"#define PANE_MATRIX_HOST (0x[0-9a-f]+)", source)[1], 16)
    assert re.search(r"rt_register_host\(g->patch_page \+ PANE_MATRIX_HOST, host_pane_matrix\)", source)
    hosts = re.findall(r"#define \w+_HOST (0x[0-9a-f]+)", source)
    assert len(hosts) == len(set(hosts)) and host < 0xE00  # race_scene.c's hosts start at 0xe00
    version = (ROOT / "runtime/core/boot/version.c").read_text()
    builder = int(re.search(r"\.pane_matrix_fn = (0x[0-9a-f]+)", version)[1], 16)
    for site, word in PANE_SITES.items():
        k, replacement, guard = table.sites["main"][site]
        assert table.sets[k][0] == "aspect" and guard == word and len(replacement) == 1
        assert patches.branch_target(word ^ 0x80000000, site) == builder  # the original calls the builder
        assert replacement[0] >> 26 == 0x25 and patches.branch_target(replacement[0] ^ 0x80000000, site) == 0x37F0000 + host
        module = SimpleNamespace(base=0, in_text=lambda _: True, word=lambda _, w=word: w)
        patches.check(patches.Table(table.sets, {"main": {site: table.sites["main"][site]}}), module, "main")


@pytest.fixture(scope="module")
def masks(tmp_path_factory):
    source = (ROOT / "runtime/core/boot/patches.c").read_text()
    code = r"""
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu/cpu.h"
static float aspect_scale, aspect_out;
static uint64_t split_at, tick = 100;
static int movie;
static struct { uint64_t ui_text_lr, pane_matrix_fn; } version = {0, 0x17f89f0};
#define rt_version() (&version)
static uint64_t rt_main_addr(uint64_t p) { return p; }
static uint64_t frame_step_ticks(void) { return tick; }
static int ov_movie_playing(void) { return movie; }
static int seqwatch_probe(void) { return 0; }
static void rt_log(const char *fmt, ...) { (void)fmt; }
static unsigned char anc[4][0xe0], parent[0xe0], pane[0xe0], icon[0xe0];
static int pane_readable(uint64_t p, uint64_t n) {
    return p >= (uint64_t)(uintptr_t)pane && p + n <= (uint64_t)(uintptr_t)(pane + sizeof pane);
}
static float built[12];
// The guest builder: writes the built global matrix (the fixture's) at the pane's +0x70, as main+0x17f89f0 does
static uint64_t rt_call(Cpu *c, uint64_t fn, int n, ...) {
    (void)c; (void)n;
    if (fn != 0x17f89f0) exit(9);
    va_list ap; va_start(ap, n);
    uint64_t at = va_arg(ap, uint64_t);
    va_end(ap);
    memcpy((void *)(uintptr_t)(at + 0x70), built, sizeof built);
    return 0;
}
static const char *cfg_value(const char *key) { (void)key; return getenv("HUD") ? getenv("HUD") : "16:9"; }
static int race_scene_on(void) { return getenv("RACE") != 0; }
"""
    # the pane-matrix hook with everything it calls: the masks, the prompts and the screen-edge HUD
    # inline here; build.py writes it to build/obj/rt/hud_layouts.h for the runtime
    code += hud_layouts.header_text().replace("#pragma once\n", "")
    code += 'static const char *cfg_get(const char *key) { (void)key; return NULL; }\n'
    code += source[source.index("#define UI_MASK_REST") : source.index("static void host_ui_proj(Cpu *c)")]
    code += "\n".join(function(source, name) for name in ("is_16x9", "ui_full_screen", "aspect_ui_fit", "ui_tall_backdrop", "host_ui_proj"))
    code += r"""
// argv: scale name parent_name above x width split [y]; above: the panes over the parent up to the RootPane (0: the
// parent is a root). Env ICON=x,y: then a MapIcon root's N_All_00 at x,y is built too and its translation printed.
int main(int argc, char **argv) {
    if (argc != 8 && argc != 9) return 2;
    aspect_scale = strtof(argv[1], 0); aspect_out = aspect_scale * (16.0f / 9);
    snprintf((char *)pane + 0xb0, 24, "%s", argv[2]);
    snprintf((char *)parent + 0xb0, 24, "%s", argv[3]);
    int above = atoi(argv[4]);
    unsigned char *up = parent;
    for (int i = 0; i < above && i < 4; i++) {
        uint64_t a = (uint64_t)(uintptr_t)anc[i];
        memcpy(up + 0x18, &a, 8);
        snprintf((char *)anc[i] + 0xb0, 24, "%s", i == above - 1 ? "RootPane" : "N_All_00");
        up = anc[i];
    }
    uint64_t p = (uint64_t)(uintptr_t)parent;
    memcpy(pane + 0x18, &p, 8);
    float size[2] = {strtof(argv[6], 0), 1080};
    memcpy(pane + 0x50, size, sizeof size);
    float m[12] = {1,0,0,strtof(argv[5], 0), 0,1,0,argc == 9 ? strtof(argv[8], 0) : 0, 0,0,1,0};
    memcpy(built, m, sizeof m);
    if (atoi(argv[7])) split_at = tick;
    Cpu c = {0}; c.x[0] = (uint64_t)(uintptr_t)pane;
    host_pane_matrix(&c);
    float g[12]; memcpy(g, pane + 0x70, sizeof g);
    // then its draw: the projection the pane gets (x row), as host_ui_proj sets it up
    float info[32] = {0}, out[16] = {0};
    info[4] = 1; info[9] = 16.0f / 9; info[19] = 1;
    Cpu d = {0}; d.x[0] = (uint64_t)(uintptr_t)info; d.x[1] = (uint64_t)(uintptr_t)out;
    d.x[23] = (uint64_t)(uintptr_t)(pane + 0x70); d.x[22] = d.x[23] - 0x20;
    host_ui_proj(&d);
    // where its left and right edges land on screen, in units of the fitted 16:9 canvas (the picture's edges at
    // -960 s and 960 s on a wider output s): x times the drawn x scale (1 / s narrowed, 1 wide) times s
    float k = out[0] * (aspect_scale > 1 ? aspect_scale : 1);
    float l = (g[3] - g[0] * size[0] / 2) * k, r2 = (g[3] + g[0] * size[0] / 2) * k;
    printf("%.3f %.3f %.6g %.6g %.6g %.6g %.6g", l, r2, g[0], out[0], g[3], g[5], g[7]);
    if (getenv("ICON")) {   // a racer's map icon: N_All_00 under its MapIcon layout's RootPane
        uint64_t r = (uint64_t)(uintptr_t)anc[3];
        memset(anc[3], 0, sizeof anc[3]);
        snprintf((char *)anc[3] + 0xb0, 24, "RootPane");
        snprintf((char *)icon + 0xb0, 24, "N_All_00");
        memcpy(icon + 0x18, &r, 8);
        float im[12] = {1,0,0,strtof(getenv("ICON"), 0), 0,1,0,strtof(strchr(getenv("ICON"), ',') + 1, 0), 0,0,1,0};
        memcpy(built, im, sizeof im);
        Cpu e = {0}; e.x[0] = (uint64_t)(uintptr_t)icon;
        host_pane_matrix(&e);
        memcpy(g, icon + 0x70, sizeof g);
        printf(" %.6g %.6g", g[3], g[7]);
    }
    printf("\n");
}
"""
    root = tmp_path_factory.mktemp("list-masks")
    (root / "test.c").write_text(code)
    subprocess.run(
        ["clang", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-D_CRT_SECURE_NO_WARNINGS",
         "-I" + str(ROOT / "runtime"), str(root / "test.c"), *LIBM, "-o", str(root / "test.exe")],
        check=True,
    )  # fmt: skip
    return root / "test.exe"


def run(exe, scale, name, parent, depth_ok, x, width=100, split=0, y=None):
    args = [str(exe), str(scale), name, parent, str(depth_ok), str(x), str(width), str(split)]
    out = subprocess.check_output(args + ([str(y)] if y is not None else []))
    return list(map(float, out.split()))[:4] if y is None else list(map(float, out.split()))


@pytest.mark.parametrize("scale", [2560 / 1080 / (16 / 9), 3840 / 1080 / (16 / 9)])
@pytest.mark.parametrize("x", [-858.6, -878.1])  # character and kart: the outer mask at rest
def test_outer_mask_reaches_the_screen_edge_and_keeps_its_inner_side(masks, scale, x):
    left, right, gx, px = run(masks, scale, "P_Cap_00", "N_Root_02", 1, x)
    # on screen, in units of the narrowed canvas: from the picture's left edge to where its inner side was
    assert left == pytest.approx(-960 * scale, abs=0.05) and right == pytest.approx(x + 50, abs=0.05)
    assert px == pytest.approx(1)  # drawn with the wide projection: it samples the backdrop under itself


@pytest.mark.parametrize("scale", [2560 / 1080 / (16 / 9), 3840 / 1080 / (16 / 9)])
@pytest.mark.parametrize("name,x", [("P_Cap_01", 29), ("P_Cap_00", -450)])  # inner mask; outer one mid-animation
def test_other_masks_stay_where_they_were_on_screen(masks, scale, name, x):
    left, right, gx, px = run(masks, scale, name, "N_Root_02", 1, x)
    assert (left, right) == pytest.approx((x - 50, x + 50), abs=0.05)
    assert px == pytest.approx(1) and gx == pytest.approx(1 / scale)


@pytest.mark.parametrize(
    "scale,name,parent,depth_ok,split",
    [
        (1, "P_Cap_00", "N_Root_02", 1, 0),  # 16:9
        (2448 / 1848 / (16 / 9), "P_Cap_00", "N_Root_02", 1, 0),  # taller: ui_tall_backdrop's case
        (1.333, "P_Cap_00", "N_Cup_00", 1, 0),  # another layout's mask
        (1.333, "P_Cap_00", "N_Root_02", 0, 0),  # N_Root_02 not under a root
        (1.333, "P_Cap_02", "N_Root_02", 1, 0),
        (1.333, "P_Base_00", "N_Root_02", 1, 0),
        (1.333, "P_Cap_00", "N_Root_02", 1, 1),  # a split screen
    ],
)
def test_other_panes_outputs_and_split_screens_are_left_alone(masks, scale, name, parent, depth_ok, split):
    left, right, gx, px = run(masks, scale, name, parent, depth_ok, -858.6, split=split)
    assert gx == 1
    if scale > 1:
        assert px == pytest.approx(1 / scale)  # the ordinary narrowed projection


# KeyAssign_00's two prompt groups: (pane, parent, side, rest x and y of their global translation in 1.8.0's layout)
PROMPTS = [("L_Key_02", "N_LeftKey_00", -1, -902.75, -500.0), ("A_Align_00", "N_Key_00", 1, 909.55, -500.0)]


@pytest.mark.parametrize("scale", [2560 / 1080 / (16 / 9), 3840 / 1080 / (16 / 9)])
@pytest.mark.parametrize("name,parent,side,x,y", PROMPTS)
def test_prompts_keep_their_corner_and_their_16x9_shape_on_wider_outputs(masks, scale, name, parent, side, x, y):
    *_, gx, px, tx, gy, ty = run(masks, scale, name, parent, 0, x, split=0, y=y)
    # drawn over the whole picture by the 16:9 projection: canvas x shows at x * s in fitted units, so the group shows
    # at its own x moved to its side (x + 960 (s - 1)) and one unit wide per unit
    assert gx * scale == pytest.approx(1) and tx * scale == pytest.approx(x + side * 960 * (scale - 1))
    assert (gy, ty) == (1, y)


@pytest.mark.parametrize("name,parent,side,x,y", PROMPTS)
def test_prompts_keep_their_corner_and_their_16x9_shape_on_taller_outputs(masks, name, parent, side, x, y):
    scale = 2448 / 1848 / (16 / 9)
    *_, gx, px, tx, gy, ty = run(masks, scale, name, parent, 0, x, split=0, y=y)
    assert (gx, tx) == (1, pytest.approx(x))
    assert gy / scale == pytest.approx(1) and ty / scale == pytest.approx(y - 540 * (1 / scale - 1))


@pytest.mark.parametrize("name,parent", [("L_Key_02", "A_alignment_00"), ("A_Align_00", "N_All_00"), ("L_Key_00", "N_Key_00")])
def test_other_key_guides_keep_their_matrix(masks, name, parent):
    *_, gx, px, tx, gy, ty = run(masks, 1.333, name, parent, 0, 500, split=0, y=-500)
    assert (gx, tx, gy, ty) == (1, 500, 1, -500)


def test_prompts_in_a_split_screen_keep_their_matrix(masks):
    *_, gx, px, tx, gy, ty = run(masks, 1.333, "L_Key_02", "N_LeftKey_00", 0, -902.75, split=1, y=-500)
    assert (gx, tx) == (1, -902.75)
