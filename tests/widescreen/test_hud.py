"""The HUD and the menus' chrome at the screen edges (patches.c, MKW_HUD_POSITION=edges): layout signatures, anchors
scoped by layout, offsets and gating, without a game. The real host_pane_matrix of patches.c is compiled against
authored pane trees (1.8.0's layout and pane names, parents and positions only)."""

import re
import subprocess
import sys

import pytest

from test_aspect import LIBM, ROOT

sys.path.insert(0, str(ROOT / "recomp"))
import hud_layouts  # noqa: E402

HARNESS = r"""
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "cpu/cpu.h"
static float aspect_scale;
static volatile uint64_t split_at;
static int in_race = 1;
static int race_scene_on(void) { return in_race && !(getenv("HUD_NOT_IN_RACE") && *getenv("HUD_NOT_IN_RACE")); }
static uint64_t rt_main_addr(uint64_t offset) { return 0x7000000000ull + offset; }
static void rt_log(const char *fmt, ...) { if (getenv("HUD_LOG")) { va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); fputc(10, stderr); } }
static const char *cfg_get(const char *key) { (void)key; return getenv("HUD_LOG") ? "4" : NULL; }
static const char *cfg_value(const char *key) { (void)key; return getenv("HUD_POSITION") ? getenv("HUD_POSITION") : "edges"; }
static struct { uint64_t pane_matrix_fn; } version = {0x17f89f0};
#define rt_version() (&version)
static uint64_t frame_step_ticks(void) { return 100; }
enum { HUD_CLASS_HUD, HUD_CLASS_HUD_AUTO, HUD_CLASS_MENU, HUD_CLASS_PART, HUD_CLASS_WORLD, HUD_CLASS_CONTENT,
       HUD_CLASS_FULLSCREEN, HUD_CLASS_SPLIT };
static const char *const hud_class_names[] = {"hud", "hud-auto", "menu", "part", "world", "content", "fullscreen",
                                              "split"};
typedef struct { uint64_t sig; const char *name; int cls; } HudLayout;
static HudLayout hud_layouts[2];   // the fixture's layout (from argv) and one that never matches
// The guest builder: global = parent's global composed with the pane's translation and x/y scale (no rotation), as
// main+0x17f89f0 does for these panes.
static uint64_t rt_call(Cpu *c, uint64_t fn, int n, ...) {
    va_list ap; va_start(ap, n); uint64_t pane = va_arg(ap, uint64_t); va_end(ap);
    if (fn != rt_main_addr(0x17f89f0)) exit(9);
    float *g = (float *)(uintptr_t)(pane + 0x70), *t = (float *)(uintptr_t)(pane + 0x30);
    uint64_t parent = *(uint64_t *)(uintptr_t)(pane + 0x18);
    float pg[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    if (parent) memcpy(pg, (const void *)(uintptr_t)(parent + 0x70), sizeof pg);
    memset(g, 0, 48); g[10] = 1;
    g[0] = pg[0] * t[6]; g[1] = pg[1] * t[7]; g[4] = pg[4] * t[6]; g[5] = pg[5] * t[7];
    g[3] = pg[3] + pg[0] * t[0] + pg[1] * t[1]; g[7] = pg[7] + pg[4] * t[0] + pg[5] * t[1];
    c->x[0] = 0; return 0;
}
// Panes with nn::ui2d's links: parent at +0x18, child list head at +0x20 (first, last), own node at +0x08
static unsigned char panes[12][0x100];
static uint64_t P(int i) { return (uint64_t)(uintptr_t)panes[i]; }
static void make(int i, const char *name, int parent, float tx, float ty, float w, float h) {
    memset(panes[i], 0, sizeof panes[i]);
    snprintf((char *)panes[i] + 0xb0, 24, "%s", name);
    uint64_t *self = (uint64_t *)panes[i];
    self[1] = self[2] = P(i) + 8;                       // own node, unlinked
    self[4] = self[5] = P(i) + 0x20;                    // empty child list
    if (parent >= 0) {
        uint64_t *par = (uint64_t *)panes[parent], node = P(i) + 8, head = P(parent) + 0x20;
        self[3] = P(parent);
        uint64_t last = par[5];                         // append: last->next = node, node->next = head
        *(uint64_t *)(uintptr_t)last = node;
        self[1] = head, self[2] = last, par[5] = node;
    }
    float *t = (float *)(panes[i] + 0x30); t[0] = tx; t[1] = ty; t[6] = t[7] = 1;
    float *s = (float *)(panes[i] + 0x50); s[0] = w; s[1] = h;
}
"""


@pytest.fixture(scope="module")
def hud(tmp_path_factory):
    source = (ROOT / "runtime/core/boot/patches.c").read_text()
    section = source[source.index("#define UI_MASK_REST") : source.index("static void host_ui_proj(Cpu *c)")]
    code = HARNESS + section
    code += r"""
static void build(int i) { Cpu c = {0}; c.x[0] = P(i); host_pane_matrix(&c); }
static float *G(int i) { return (float *)(panes[i] + 0x70); }
int main(int argc, char **argv) {
    if (!strcmp(argv[1], "sig")) {   // sig: the fixture "root > a > b > c" and "root > a > d" (twice)
        make(0, "RootPane", -1, 0, 0, 1920, 1080); make(1, "N_All_00", 0, 0, 0, 30, 40);
        make(2, "L_Part_00", 1, 0, 0, 30, 40); make(3, "N_PartTop_00", 2, 0, 0, 30, 40);
        make(4, "P_Deep_00", 3, 0, 0, 30, 40); make(5, "P_Pic_00", 1, 0, 0, 30, 40);
        make(6, "P_Pic_00", 1, 0, 0, 30, 40);   // a clone the game adds at run time: counted once
        printf("%016llx\n", (unsigned long long)hud_layout_sig(P(0))); return 0;
    }
    // LAYOUT CLASS SIG SCALE SPLIT then a tree spec: pane parent-index tx ty w h ...
    hud_layouts[0].name = argv[1]; hud_layouts[0].cls = atoi(argv[2]); hud_layouts[0].sig = strtoull(argv[3], 0, 16);
    hud_layouts[1].name = "Other"; hud_layouts[1].cls = HUD_CLASS_HUD; hud_layouts[1].sig = 1;
    aspect_scale = strtof(argv[4], 0);
    if (atoi(argv[5])) split_at = 95;
    int n = 0;
    for (int a = 6; a + 5 < argc; a += 6, n++)
        make(n, argv[a], atoi(argv[a + 1]), strtof(argv[a + 2], 0), strtof(argv[a + 3], 0), strtof(argv[a + 4], 0),
             strtof(argv[a + 5], 0));
    for (int k = 0; k < 2; k++) for (int i = 0; i < n; i++) build(i);   // twice: no double move
    for (int i = 0; i < n; i++) printf("%.3f %.3f %.4f\n", G(i)[3], G(i)[7], G(i)[0]);
}
"""
    out = tmp_path_factory.mktemp("hud")
    (out / "t.c").write_text(code)
    subprocess.run(
        ["clang", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-I" + str(ROOT / "runtime"),
         "-D_CRT_SECURE_NO_WARNINGS", "-I" + str(ROOT / "runtime/cpu"), str(out / "t.c"), *LIBM, "-o", str(out / "t.exe")],
        check=True,
    )  # fmt: skip
    return out / "t.exe"


CLASS = {c: i for i, c in enumerate(hud_layouts.CLASS_SET)}


def tree_sig(tree):
    """The signature recomp/hud_layouts.py gives a tree spec [(name, parent index, tx, ty, w, h), ...] (root first)."""
    text = "== L %dx%d\n" % (int(tree[0][4]), int(tree[0][5]))
    depth = {0: 0}
    for i, (name, parent, *_rest) in enumerate(tree):
        if i and parent < 0:
            break  # a second root (another layout, as the map icons' own)
        if i:
            depth[i] = depth[parent] + 1
        text += "  " * depth[i] + f"pan {name} <x>\n"
    return f"{hud_layouts.signature(hud_layouts.parse_tree(text), 'L'):016x}"


def run(hud, layout, tree, scale, cls="hud", split=0, register=True):
    sig = tree_sig(tree) if register else "2"
    args = [str(hud), layout, str(CLASS[cls]), sig, str(scale), str(split)]
    for name, parent, tx, ty, w, h in tree:
        args += [name, str(parent), str(tx), str(ty), str(w), str(h)]
    out = subprocess.check_output(args, text=True)
    return [tuple(map(float, line.split())) for line in out.splitlines()]


def anchor_depths():
    source = (ROOT / "runtime/core/boot/patches.c").read_text()
    block = source[source.index("static const HudAnchor hud_anchors[] = {") : source.index("// The canvas offset of an anchor")]
    return {(m[0], m[1], m[2]): int(m[3]) for m in re.findall(r'\{"(\w+)", "(\w+)", "(\w+)", (\d+),', block)}


DEPTHS = anchor_depths()


def simple(pane, parent, tx, ty, w=100, h=100, layout=None, depth=None):
    """root > (fillers) > parent > pane (tx, ty) > T_Num_00 at (10, 5): the digits of a frame. The pane is at its
    anchor's depth in the layout (2 when it has none), so the 3rd and 4th rows are always the pane and its child."""
    depth = depth or DEPTHS.get((layout, pane, parent), 2)
    tree = [("RootPane", -1, 0, 0, 1920, 1080)]
    for k in range(depth - 2):
        tree.append((f"N_Fill_{k:02}", len(tree) - 1, 0, 0, 30, 40))
    tree.append((parent, len(tree) - 1, 0, 0, 30, 40))
    tree += [(pane, len(tree) - 1, tx, ty, w, h), ("T_Num_00", len(tree), 10, 5, 10, 10)]
    return tree


TALL = (2448 / 1848) / (16 / 9)
DY = 540 * (1 / TALL - 1)  # 184.7 at the Fold's shape
W21 = (2560 / 1080) / (16 / 9)
W32 = (5120 / 1440) / (16 / 9)


def test_runtime_signature_matches_the_generator(hud):
    got = subprocess.check_output([str(hud), "sig"], text=True).strip()
    text = "== L 1920x1080\npan RootPane <->\n  pan N_All_00 <RootPane>\n    pan L_Part_00 <N_All_00>\n" \
           "      pan N_PartTop_00 <L_Part_00>\n        pan P_Deep_00 <N_PartTop_00>\n    pan P_Pic_00 <N_All_00>\n"
    want = hud_layouts.signature(hud_layouts.parse_tree(text), "L")
    assert int(got, 16) == want
    deep = "== L 1920x1080\npan RootPane <->\n  pan N_All_00 <RootPane>\n    pan L_Part_00 <N_All_00>\n" \
           "      pan N_PartTop_00 <L_Part_00>\n    pan P_Pic_00 <N_All_00>\n"
    assert want == hud_layouts.signature(hud_layouts.parse_tree(deep), "L")  # depth 4 is not part of it


def test_generator_expands_part_panes_into_their_part_layouts_top_panes():
    text = ("== Host 1920x1080\npan RootPane <->\n  pan N_All_00 <RootPane>\n    prt L_X_00 <N_All_00> part=Part\n"
            "== Part 1920x1080\npan RootPane <->\n  pan N_PartTop_00 <RootPane>\n")
    flat = "== L 1920x1080\npan RootPane <->\n  pan N_All_00 <RootPane>\n    pan L_X_00 <N_All_00>\n" \
           "      pan N_PartTop_00 <L_X_00>\n"
    t = hud_layouts.parse_tree(text)
    assert hud_layouts.signature(t, "Host") == hud_layouts.signature(hud_layouts.parse_tree(flat), "L")


@pytest.mark.parametrize(
    "layout,pane,parent,tx,ty,ex,ey",
    [
        ("RaceInfo_1", "L_ItemSlot_00", "N_Root", -753, 400, 0, DY),
        ("RaceInfo_1", "N_Pers_00", "N_Root", -880, -489, 0, -DY),
        ("RaceInfo_1", "L_Rank_00", "N_Root", 778, -425, 0, -DY),
        ("RaceInfo_1", "L_Rank_00", "N_Root", 778, -358, 0, -DY),  # the other end of RaceInfo_1_Mode's animation
        ("RaceInfo_1", "L_Timer_00", "N_TimeAttack_00", 880, 460, 0, DY),
        ("RaceInfo_1", "L_ItemWarning_00", "N_Root", 0, -500, 0, -DY),
        ("RaceInfo_1", "L_Hit_00", "N_Root", -780, 160, 0, 0),
        ("NowLoading_04", "P_Header_00", "N_Frame_00", 0, 490, 0, DY),
        ("NowLoading_04", "P_Footer_00", "N_Frame_00", 0, -490, 0, -DY),
        ("CharacterSelectBG_00", "P_Base_00", "N_Screen_00", 0, -472, 0, -DY),
        ("CharaSelect_00", "A_alignment_00", "N_Ready_01", 905, -512, 0, -DY),
        ("CharaSelect_00", "A_alignment_00", "N_Ready_01", 438, -254, 0, 0),  # its other state, in the middle
        ("Mission_00", "L_Timer_00", "N_All_00", 731, 436.1, 0, DY),
        ("PassedNum_00", "N_InOut_00", "N_Pos_00", 950, 452, 0, DY),
        ("TimerBattleMode_00", "N_Pos_00", "N_All_00", 805, 460, 0, DY),
        ("PauseMenu_00", "L_SongForward_00", "N_All_00", 636, 476, 0, DY),
        ("MenuSurvivalSelect_00", "N_Title_00", "N_All_00", -960, 475, 0, DY),
        ("TelopFooter_01", "P_Base_00", "N_State_00", 0, -490, 0, -DY),
        ("TelopFooter_01", "P_Base_00", "N_State_00", 0, 10, 0, 0),  # its State animation's other end: the centre
    ],
)
def test_tall_moves_anchored_panes_to_their_edge_and_children_follow(hud, layout, pane, parent, tx, ty, ex, ey):
    (gx, gy, sx), (cx, cy, _) = run(hud, layout, simple(pane, parent, tx, ty, layout=layout), TALL)[-2:]
    assert (gx, gy) == pytest.approx((tx + ex, ty + ey), abs=0.01)
    assert (cx, cy) == pytest.approx((gx + 10, gy + 5), abs=0.01)  # the digits stay with their frame
    assert sx == 1  # nothing widened on a taller output


@pytest.mark.parametrize("scale", [W21, W32])
@pytest.mark.parametrize(
    "layout,pane,parent,tx,ty,ax",
    [("RaceInfo_1", "L_ItemSlot_00", "N_Root", -753, 400, -1), ("RaceInfo_1", "L_Rank_00", "N_Root", 778, -425, 1),
     ("RaceInfo_1", "L_ItemWarning_00", "N_Root", 0, -500, 0),
     ("CharaSelect_00", "A_alignment_00", "N_Ready_00", 200, -512, 0),  # in the middle third: not pushed sideways
     ("Watching_00", "N_Live_00", "N_InOut_00", -891, 452, -1), ("TitleLauncher_00", "N_Load_00", "N_All_00", 880, -468, 1)],
)  # fmt: skip
def test_wide_moves_x_only(hud, scale, layout, pane, parent, tx, ty, ax):
    gx, gy, sx = run(hud, layout, simple(pane, parent, tx, ty, layout=layout), scale)[-2]
    assert (gx, gy) == pytest.approx((tx + ax * 960 * (scale - 1), ty), abs=0.05)
    assert sx == 1


@pytest.mark.parametrize("scale", [W21, W32])
def test_wide_bands_span_the_picture_and_stay_on_their_row(hud, scale):
    tree = simple("P_Footer_00", "N_Frame_00", 0, -490, 1920, 100, layout="NowLoading_04")
    gx, gy, sx = run(hud, "NowLoading_04", tree, scale)[-2]
    assert (gx, gy, sx) == pytest.approx((0, -490, scale), abs=1e-3)


@pytest.mark.parametrize("scale", [W21, W32, TALL])
def test_a_widened_headers_title_keeps_its_width_and_place(hud, scale):
    tree = [("RootPane", -1, 0, 0, 1920, 1080), ("N_All_00", 0, 0, 0, 30, 40), ("P_Header_00", 1, 0, 445, 1920, 192),
            ("T_Text_00", 2, 30, 23, 1100, 80)]  # fmt: skip
    (_, _, _), (_, _, _), (hx, hy, hs), (tx, ty, ts) = run(hud, "FaceSelect_00", tree, scale)
    dy = DY if scale < 1 else 0
    assert (hx, hy, hs) == pytest.approx((0, 445 + dy, max(scale, 1)), abs=1e-3)
    assert (tx, ty, ts) == pytest.approx((30, 468 + dy, 1), abs=1e-3)  # on the band, not stretched


def cup_tree():
    return [("RootPane", -1, 0, 0, 1920, 1080), ("N_CameraArea_00", 0, 500, 119, 650, 650),
            ("N_All_00", 0, 0, 0, 30, 40), ("L_Ticket_00", 2, -427, 75, 1549, 1087.2),
            ("N_Btn_00", 2, 0, 0, 30, 40), ("P_Base_00", 4, 0, -455, 1920, 201), ("N_Cup_00", 4, 0, -390, 30, 40),
            ("L_Cup_00", 6, -742, -5, 413.4, 415.4)]  # fmt: skip


@pytest.mark.parametrize("scale", [TALL, W21, W32])
def test_cup_select_band_docks_to_the_bottom_as_one_group_and_its_base_spans(hud, scale):
    camera, _, ticket, btn, base, cup_row, cup = run(hud, "MenuCupSelect_00", cup_tree(), scale, "menu")[1:]
    dy = -DY if scale < 1 else 0
    assert btn[:2] == pytest.approx((0, dy), abs=1e-3)
    assert base == pytest.approx((0, -455 + dy, max(scale, 1)), abs=1e-3)  # the band, as wide as the picture
    assert cup_row[:2] == pytest.approx((0, -390 + dy), abs=1e-3) and cup == pytest.approx((-742, -395 + dy, 1), abs=1e-3)
    assert camera[:2] == pytest.approx((500, 119)) and ticket[:2] == pytest.approx((-427, 75))  # these stay
    if scale < 1:
        assert -455 + dy - 201 / 2 <= -540 / TALL  # nothing of the backdrop shows below the band


@pytest.mark.parametrize("scale", [W21, W32])
def test_a_widened_container_of_bands_stretches_its_bands_too(hud, scale):
    tree = [("RootPane", -1, 0, 0, 1920, 1080), ("N_All_00", 0, 0, 0, 30, 40), ("N_Btn_00", 1, 0, 0, 30, 40),
            ("N_Base_01", 2, 0, -455, 1920, 201), ("P_Band_00", 3, 0, 0, 1920, 201)]  # fmt: skip
    base, band = run(hud, "MenuSurvivalSelect_00", tree, scale, "menu")[3:]
    assert base == pytest.approx((0, -455, scale), abs=1e-3) and band == pytest.approx((0, -455, scale), abs=1e-3)


@pytest.mark.parametrize("cls", ["content", "part", "world", "split", "fullscreen"])
def test_an_anchor_moves_nothing_in_a_layout_of_another_class(hud, cls):
    for scale in (TALL, W21, W32):
        assert run(hud, "RaceInfo_1", simple("L_ItemSlot_00", "N_Root", -753, 400), scale, cls)[2][:2] == \
            pytest.approx((-753, 400))


@pytest.mark.parametrize(
    "layout,pane,parent",
    [("RaceInfo_1", "L_ItemSlot_00", "N_ChangeLayout_00"), ("RaceInfo_1", "L_Rewind_00", "N_Root"),
     ("MenuSurvivalSelect_00", "P_Cap_00", "N_Btn_00"), ("RaceInfo_1", "L_Name_00", "N_Root"),
     ("MenuCupSelect_00", "N_CameraArea_00", "RootPane"), ("Vote_00", "N_OverlapAre_01", "RootPane"),
     ("RaceInfo_2", "L_ItemSlot_00", "N_ChangeLayout_00"),
     ("MenuCourseSelect_00", "N_Cap_01", "N_CourseSelect_00")],  # fmt: skip
)
def test_split_screen_other_panes_masks_and_code_read_panes_stay(hud, layout, pane, parent):
    for scale in (TALL, W21, W32):
        assert run(hud, layout, simple(pane, parent, -753, 400), scale, "menu")[-2][:2] == pytest.approx([-753, 400])


def mask_tree(pane, tx, w=100, h=1080):
    """CharaSelect_00's capture masks: RootPane > N_Root_02 > P_Cap_0x (depth 2)."""
    return [("RootPane", -1, 0, 0, 1920, 1080), ("N_Root_02", 0, 0, 0, 30, 40), (pane, 1, tx, 0, w, h)]


@pytest.mark.parametrize("scale", [W21, W32])
@pytest.mark.parametrize("layout,tx", [("CharaSelect_00", -858.6), ("MachineSelect_00", -878.1)])
def test_the_lists_outer_mask_reaches_the_screen_edge_and_keeps_its_inner_side(hud, scale, layout, tx):
    gx, _, gs = run(hud, layout, mask_tree("P_Cap_00", tx), scale, "menu")[2]
    # host_ui_proj draws it with the projection widened by s: on the canvas of the narrowed picture it spans
    left, right = (gx - gs * 50) * scale, (gx + gs * 50) * scale
    assert (left, right) == pytest.approx((-960 * scale, tx + 50), abs=1e-2)
    # and it takes the backdrop at u = (x + 960) / 1920 from that place: the backdrop, drawn wide, shows u there
    for x_vertex, x_screen in ((gx - gs * 50, left), (gx + gs * 50, right)):
        assert (x_vertex + 960) / 1920 == pytest.approx((x_screen + 960 * scale) / (1920 * scale), abs=1e-6)


@pytest.mark.parametrize("scale", [W21, W32])
def test_the_lists_inner_mask_stays_in_place_and_samples_under_itself(hud, scale):
    gx, _, gs = run(hud, "CharaSelect_00", mask_tree("P_Cap_01", 29), scale, "menu")[2]
    assert (gx * scale, gs * scale) == pytest.approx((29, 1), abs=1e-3)


@pytest.mark.parametrize("scale", [W21, W32])
def test_a_mask_away_from_the_outer_edge_is_not_extended(hud, scale):
    # CharaSelect_00_State carries the outer mask to -450 (40 wide) with the list in the middle
    gx, _, gs = run(hud, "CharaSelect_00", mask_tree("P_Cap_00", -450, 40, 710), scale, "menu")[2]
    assert (gx * scale, gs * scale) == pytest.approx((-450, 1), abs=1e-3)


@pytest.mark.parametrize("scale", [TALL, 1.0])
def test_masks_keep_their_matrix_on_a_taller_or_16x9_output(hud, scale):
    assert run(hud, "CharaSelect_00", mask_tree("P_Cap_00", -858.6), scale, "menu")[2] == pytest.approx((-858.6, 0, 1))


@pytest.mark.parametrize("scale", [W21, W32, TALL])
@pytest.mark.parametrize("pane,parent,tx,ax", [("L_Key_02", "N_LeftKey_00", -902.7, -1), ("A_Align_00", "N_Key_00", 909.5, 1)])
def test_race_and_pause_prompts_keep_their_shape_in_their_corner(hud, scale, pane, parent, tx, ax):
    # KeyAssign_00 is drawn with the 16:9 projection over the whole picture: canvas x shows at x / 960 of the half
    # width. Squeezed, it shows where and as wide as the narrowed projection would show it docked (HUD_EDGE)
    (gx, gy, gs), (cx, cy, _) = run(hud, "KeyAssign_00", simple(pane, parent, tx, -500, layout="KeyAssign_00"), scale,
                                    "menu")[-2:]  # fmt: skip
    if scale > 1:
        assert (gx / 960, gs) == pytest.approx(((tx + ax * 960 * (scale - 1)) / (960 * scale), 1 / scale), abs=1e-5)
        assert gy == pytest.approx(-500)
        assert abs(gx) < 960  # on screen, in its corner
        assert (cx - gx, cy - gy) == pytest.approx((10 / scale, 5), abs=1e-4)  # its children, squeezed with it
    else:
        assert gy / 540 == pytest.approx((-500 - 540 * (1 / scale - 1)) * scale / 540, abs=1e-5)
        assert gx == pytest.approx(tx) and abs(gy) < 540


def test_an_unknown_layout_moves_nothing(hud):
    for scale in (TALL, W21):
        out = run(hud, "RaceInfo_1", simple("L_ItemSlot_00", "N_Root", -753, 400), scale, register=False)
        assert out[2][:2] == pytest.approx((-753, 400))


def test_the_same_names_in_another_layout_do_not_move(hud):
    # The 3a key-guide anchor (A_alignment_00 under N_Alll_00) belongs to Skip, not to a layout that happens to share it
    for scale in (TALL, W21):
        assert run(hud, "Other", simple("A_alignment_00", "N_Alll_00", 910, -500, depth=3), scale)[-2][:2] == \
            pytest.approx((910, -500))


@pytest.mark.parametrize("depth", [3, 7, 8])
def test_a_part_pane_with_an_anchors_names_at_another_depth_stays(hud, depth):
    # CupBtn_00 (the cup buttons inside MenuCupSelect_00) has its own N_Btn_00 under N_All_00, at depths 7 and 8
    for scale in (TALL, W21):
        tree = simple("N_Btn_00", "N_All_00", 0, -73, layout="MenuCupSelect_00", depth=depth)
        assert run(hud, "MenuCupSelect_00", tree, scale, "menu")[-2][:2] == pytest.approx((0, -73))


def test_a_split_screen_moves_nothing(hud):
    for scale in (TALL, W21):
        assert run(hud, "RaceInfo_1", simple("L_ItemSlot_00", "N_Root", -753, 400), scale, split=1)[2][:2] == \
            pytest.approx((-753, 400))


def test_16x9_offsets_nothing(hud):
    for layout, pane, parent in (("RaceInfo_1", "L_ItemSlot_00", "N_Root"), ("NowLoading_04", "P_Footer_00", "N_Frame_00")):
        out = run(hud, layout, simple(pane, parent, -753, 400, layout=layout), 1.0)
        assert out[-2] == pytest.approx((-753, 400, 1)) and out[-1][:2] == pytest.approx((-743, 405))


def minimap_tree(map_x, map_y, icon_x, icon_y):
    # the map, then a racer's icon: a MapIcon_00 layout attached under a pane of another layout (its root has a parent)
    return [("RootPane", -1, 0, 0, 1920, 1080), ("N_InOut_00", 0, 0, 0, 30, 40), ("N_Mode_00", 1, 0, 0, 30, 40),
            ("N_SunLight_00", 2, 0, 0, 30, 40), ("P_Map_00", 3, map_x, map_y, 391.5, 316.7),
            ("L_Icon_00", -1, 0, 0, 30, 40), ("RootPane", 5, 0, 0, 1920, 1080), ("N_All_00", 6, icon_x, icon_y, 30, 40),
            ("N_All_01", 7, 0, 0, 30, 40)]  # fmt: skip


@pytest.mark.parametrize("scale", [TALL, W21, W32])
def test_minimap_goes_to_the_right_edge_at_its_share_of_the_height(hud, scale):
    out = run(hud, "MiniMap_01", minimap_tree(744.2, -63.7, 866.7, -38.1), scale)
    (mx, my, _), (ix, iy, _) = out[4], out[8]
    if scale < 1:
        assert (mx, my) == pytest.approx((744.2, -63.7 / TALL), abs=0.05)
    else:
        assert (mx, my) == pytest.approx((744.2 + 960 * (scale - 1), -63.7), abs=0.05)
    assert (ix - mx, iy - my) == pytest.approx((866.7 - 744.2, -38.1 + 63.7), abs=0.05)  # registered on the map


@pytest.mark.parametrize("in_race,icon_x,icon_y", [(0, 866.7, -38.1), (1, -500, 200)])
def test_map_icons_outside_a_race_or_the_minimap_stay(hud, monkeypatch, in_race, icon_x, icon_y):
    monkeypatch.setenv("HUD_NOT_IN_RACE", "1" if not in_race else "")
    out = run(hud, "MiniMap_01", minimap_tree(744.2, -63.7, icon_x, icon_y), W21)
    assert out[8][:2] == pytest.approx((icon_x, icon_y), abs=0.01)


def submonitor_tree(map_x, map_y, icon_x, icon_y):
    # free roam's round map (SubMonitor), then the player's icon from another layout, as in minimap_tree
    return [("RootPane", -1, 0, 0, 1920, 1080), ("N_InOut_00", 0, 0, 0, 30, 40),
            ("P_SubMonitor_00", 1, map_x, map_y, 300, 300),
            ("L_Icon_00", -1, 0, 0, 30, 40), ("RootPane", 3, 0, 0, 1920, 1080), ("N_All_00", 4, icon_x, icon_y, 30, 40),
            ("N_All_01", 5, 0, 0, 30, 40)]  # fmt: skip


@pytest.mark.parametrize("scale", [TALL, W21, W32])
def test_free_roam_round_map_goes_to_its_corner_with_its_icon_outside_a_race(hud, monkeypatch, scale):
    monkeypatch.setenv("HUD_NOT_IN_RACE", "1")
    out = run(hud, "SubMonitor", submonitor_tree(760, -390, 770, -380), scale)
    (mx, my, _), (ix, iy, _) = out[2], out[5]
    if scale < 1:
        assert (mx, my) == pytest.approx((760, -390 - 540 * (1 / scale - 1)), abs=0.05)
    else:
        assert (mx, my) == pytest.approx((760 + 960 * (scale - 1), -390), abs=0.05)
    assert (ix - mx, iy - my) == pytest.approx((10, 10), abs=0.05)  # the icon stays on the map


@pytest.mark.parametrize("scale", [TALL, W21, W32])
def test_free_roam_player_icon_placed_through_its_n_all_01_follows_the_round_map(hud, monkeypatch, scale):
    # free roam's player icon: N_All_00 at its layout's origin, N_All_01 at the map (v180p3c-probe1-w32-free)
    monkeypatch.setenv("HUD_NOT_IN_RACE", "1")
    tree = submonitor_tree(760, -390, 0, 0)
    tree[-1] = ("N_All_01", 5, 770, -380, 30, 40)
    out = run(hud, "SubMonitor", tree, scale)
    (mx, my, _), (ox, oy, _), (ix, iy, _) = out[2], out[5], out[6]
    assert (ox, oy) == pytest.approx((0, 0), abs=0.05)  # the icon layout's root stays
    assert (ix - mx, iy - my) == pytest.approx((10, 10), abs=0.05)  # moved once, with the map


@pytest.mark.parametrize("position", ["16:9", "other"])
def test_the_default_position_moves_nothing(hud, monkeypatch, position):
    monkeypatch.setenv("HUD_POSITION", position)
    tree = simple("L_ItemSlot_00", "N_Root", -753, 400, layout="RaceInfo_1")
    assert run(hud, "RaceInfo_1", tree, W21, "hud")[-2][:2] == pytest.approx((-753, 400))
