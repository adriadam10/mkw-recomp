"""The screen-edge HUD's reviewed layout classes (recomp/hud-classes-180.tsv) against the 1.8.0 layouts' signatures
(recomp/hud-layouts-180.tsv) and the anchors in patches.c: every layout classified, anchors only in hud and menu
layouts, signatures unambiguous where it matters, and the generated header up to date."""

import collections
import re
import sys

import pytest

from test_aspect import ROOT

sys.path.insert(0, str(ROOT / "recomp"))
import hud_layouts  # noqa: E402


def anchors():
    source = (ROOT / "runtime/core/boot/patches.c").read_text()
    block = source[source.index("static const HudAnchor hud_anchors[] = {") : source.index("// The canvas offset of an anchor")]
    return [(layout, pane, parent, ax, ay, kind) for layout, pane, parent, _depth, ax, ay, kind in
            re.findall(r'\{"(\w+)", "(\w+)", "(\w+)", (\d+), (-?\d), (-?\d), (HUD_\w+)\}', block)]


def test_every_1_8_0_layout_has_exactly_one_known_class():
    rows = hud_layouts.table()  # raises on an unclassified layout, a duplicate or an unknown class
    assert len(rows) == 248
    assert {cls for *_, cls in rows} <= set(hud_layouts.CLASS_SET)


def test_the_root_sizes_put_the_split_layouts_in_split():
    for name, size, _, cls in hud_layouts.table():
        if size in ("960x1080", "960x540"):
            assert cls == "split", name


def test_generated_header_has_every_layout_once():
    # build.py writes it to build/obj/rt/hud_layouts.h (generated C stays out of the repo)
    text = hud_layouts.header_text()
    sigs = re.findall(r'\{0x([0-9a-f]{16})ull, "(\w+)", HUD_CLASS_(\w+)\}', text)
    assert [(name, int(sig, 16), cls.lower().replace("_", "-")) for sig, name, cls in sigs] == [
        (name, sig, cls) for name, _, sig, cls in hud_layouts.table()
    ]


def test_anchors_name_only_hud_and_menu_layouts():
    classes = {name: cls for name, _, _, cls in hud_layouts.table()}
    rows = anchors()
    assert len(rows) > 50
    for layout, pane, parent, ax, ay, kind in rows:
        assert layout in classes, layout
        assert classes[layout] in ("hud", "menu"), (layout, classes[layout])


def test_anchors_are_unique_and_move_somewhere():
    rows = anchors()
    keys = [(layout, pane, parent) for layout, pane, parent, *_ in rows]
    assert len(keys) == len(set(keys))
    for layout, pane, parent, ax, ay, kind in rows:
        assert (ax, ay) != ("0", "0") or kind == "HUD_WIDEN", (layout, pane)


@pytest.mark.parametrize(
    "pane",
    ["N_CameraArea_00", "N_CameraArea_01", "L_Ticket_00", "P_Cap_00", "P_Cap_01", "N_Cap_00", "N_Cap_01",
     "N_OverlapAre_00", "N_OverlapAre_01", "N_ReplacePos_00", "N_ReplacePos_01", "N_ButtonSize_00", "N_Outside_00"],
)  # fmt: skip
def test_code_read_panes_and_capture_masks_are_never_anchored(pane):
    assert all(p != pane for _, p, *_ in anchors())


def test_colliding_signatures_share_a_class_and_no_anchor_names_them():
    by = collections.defaultdict(list)
    for name, _, sig, cls in hud_layouts.table():
        by[sig].append((name, cls))
    anchored = {layout for layout, *_ in anchors()}
    for sig, rows in by.items():
        if len(rows) > 1:
            assert len({cls for _, cls in rows}) == 1, rows
            assert not anchored & {name for name, _ in rows}, rows


def test_named_acceptance_case_cup_select():
    rows = {(layout, pane): (parent, ax, ay, kind) for layout, pane, parent, ax, ay, kind in anchors()}
    assert rows[("MenuCupSelect_00", "N_Btn_00")] == ("N_All_00", "0", "-1", "HUD_GROUP")
    assert rows[("MenuCupSelect_00", "P_Base_00")] == ("N_Btn_00", "0", "0", "HUD_WIDEN")
