"""1.8.0's display size (set display): the table's sites and the real hooks of patches.c, without an owned game.

1.8.0 sizes its render targets and window crop from the display object's requested size, so the hooks give that size
the output's shape: rows on a taller output, columns on a wider one, shrunk to fit the output (the window textures).
"""

import re
import subprocess
import sys
from types import SimpleNamespace

import pytest

from test_aspect import LIBM, ROOT, function

sys.path.insert(0, str(ROOT / "recomp"))
import patches  # noqa: E402

# site: (guarded word, another word the guard refuses, host); main's patch page at 0x37f0000 (generated layout)
DISPLAY_SITES = {
    0x3723B4: (0xF900092C, 0xF9000909, "DISPLAY_SIZE_HOST"),
    0x372840: (0x9429CE08, 0x94000142, "DISPLAY_SET_HOST"),
    0xDE5B58: (0x94000142, 0x9429CE08, "DISPLAY_SET_HOST"),
}


def test_display_table_guards_its_own_instructions_and_calls_the_registered_hooks():
    table = patches.load(
        ROOT / "recomp/patches-mkw-180.txt",
        [("main", 0, 0x37F0000), ("subsdk0", 0x5000000, 0), ("subsdk1", 0x6000000, 0), ("sdk", 0x7000000, 0)],
    )
    index = next(i for i, row in enumerate(table.sets) if row[0] == "display")
    sites = {at: row for at, row in table.sites["main"].items() if row[0] == index}
    assert sorted(sites) == sorted(DISPLAY_SITES)
    source = (ROOT / "runtime/core/boot/patches.c").read_text()
    used = {int(m, 16) for m in re.findall(r"#define \w+_HOST (0x[0-9a-f]+)", source)}
    for site, (word, other_word, host_name) in DISPLAY_SITES.items():
        _, replacement, guard = sites[site]
        assert guard == word and len(replacement) == 1
        host = int(re.search(rf"#define {host_name} (0x[0-9a-f]+)", source)[1], 16)
        assert host in used and re.search(rf"rt_register_host\(g->patch_page \+ {host_name},", source)
        assert replacement[0] >> 26 == 0x25  # BL, not a branch that loses the return address
        assert patches.branch_target(replacement[0] ^ 0x80000000, site) == table.page + host
        one = patches.Table(table.sets, {"main": {site: sites[site]}})
        module = SimpleNamespace(base=0, in_text=lambda _: True, word=lambda _, w=word: w)
        patches.check(one, module, "main")
        module.word = lambda _, w=other_word: w
        with pytest.raises(patches.PatchError, match="module has"):
            patches.check(one, module, "main")
    hosts = re.findall(r"#define \w+_HOST (0x[0-9a-f]+)", source)
    assert len(hosts) == len(set(hosts)), "two hosts share a patch page slot"


@pytest.fixture(scope="module")
def display_hook(tmp_path_factory):
    source = (ROOT / "runtime/core/boot/patches.c").read_text()
    code = r"""
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu/cpu.h"
static float aspect_out, aspect_scale;
static struct { uint64_t display_set_fn; uint8_t display_value_reg, display_base_reg; } version = {0xde6060, 12, 9};
#define rt_version() (&version)
#define RtVersion __typeof__(version)
static uint64_t rt_main_addr(uint64_t offset) { return 0x7000000000ull + offset; }
static void rt_log(const char *fmt, ...) { (void)fmt; }
static uint64_t called_fn, called_args[3];
// The guest setter: stores *size at display + 0x10 + 8 * slot and its mode index, as main+0xde6060 does.
static uint64_t rt_call(Cpu *c, uint64_t fn, int n, ...) {
    va_list ap; va_start(ap, n);
    for (int i = 0; i < n; i++) called_args[i] = va_arg(ap, uint64_t);
    va_end(ap);
    called_fn = fn;
    uint64_t wh; memcpy(&wh, (void *)(uintptr_t)called_args[1], 8);
    memcpy((void *)(uintptr_t)(called_args[0] + 0x10 + 8 * called_args[2]), &wh, 8);
    *(uint8_t *)(uintptr_t)(called_args[0] + 0x264) = (uint32_t)wh == 2560 && wh >> 32 == 1440 ? 5 : 7;
    c->x[0] = 0xdead; return 0;
}
static uint32_t output_wh[2];
"""
    code += "".join(function(source, n) for n in ("display_fit", "display_note", "host_display_size", "host_display_set"))
    code += r"""
int main(int argc, char **argv) {
    if (argc != 8) return 2;
    aspect_out = strtof(argv[1], 0); aspect_scale = aspect_out / (16.0f / 9);
    uint64_t wh = strtoull(argv[2], 0, 0) | strtoull(argv[3], 0, 0) << 32;
    int slot = atoi(argv[5]);
    output_wh[0] = (uint32_t)atoi(argv[6]), output_wh[1] = (uint32_t)atoi(argv[7]);
    static uint8_t display[0x300];
    Cpu c = {0};
    if (!strcmp(argv[4], "store")) {
        c.x[12] = wh;
        c.x[9] = (uint64_t)(uintptr_t)display;
        host_display_size(&c);
        if (c.x[12] != wh) return 3;   // registers keep the original size
    } else {
        c.x[0] = (uint64_t)(uintptr_t)display; c.x[1] = (uint64_t)(uintptr_t)&wh; c.x[2] = (uint32_t)slot;
        host_display_set(&c);
        if (called_fn != rt_main_addr(0xde6060)) return 4;
        if (display[0x264] != ((uint32_t)wh == 2560 && wh >> 32 == 1440 ? 5 : 7)) return 5;   // mode of the original
    }
    uint64_t out; memcpy(&out, display + 0x10 + 8 * slot, 8);
    printf("%u %u\n", (unsigned)out, (unsigned)(out >> 32));
}
"""
    out = tmp_path_factory.mktemp("display")
    (out / "test.c").write_text(code)
    subprocess.run(
        ["clang", "-std=c11", "-Wall", "-Wextra", "-Werror", "-D_CRT_SECURE_NO_WARNINGS",
         "-I" + str(ROOT / "runtime"), "-I" + str(ROOT / "runtime/cpu"), str(out / "test.c"), *LIBM,
         "-o", str(out / "test.exe")],
        check=True,
    )  # fmt: skip
    return out / "test.exe"


def run(hook, aspect, w, h, kind, slot, output=(3840, 2160)):
    out = subprocess.check_output([str(hook), str(aspect), str(w), str(h), kind, str(slot), *map(str, output)])
    return list(map(int, out.split()))


@pytest.mark.parametrize("kind,slot", [("store", 0), ("set", 0), ("set", 1)])
@pytest.mark.parametrize("w,h,rows", [(2560, 1440, 1934), (1920, 1080, 1450), (1280, 720, 966)])
def test_tall_display_keeps_width_and_takes_the_output_rows(display_hook, kind, slot, w, h, rows):
    assert run(display_hook, 2448 / 1848, w, h, kind, slot) == [w, rows]


@pytest.mark.parametrize("kind,slot", [("store", 0), ("set", 0), ("set", 1)])
@pytest.mark.parametrize(
    "aspect,w,h,output,cols",
    [
        (2560 / 1080, 2560, 1440, (3414, 1440), 3414),
        (5120 / 1440, 2560, 1440, (5120, 1440), 5120),
        (2560 / 1080, 1920, 1080, (2560, 1080), 2560),
    ],
)
def test_wide_display_keeps_rows_and_takes_the_output_columns(display_hook, kind, slot, aspect, w, h, output, cols):
    got = run(display_hook, aspect, w, h, kind, slot, output)
    assert got == [cols, h]


@pytest.mark.parametrize("kind", ["store", "set"])
def test_a_display_larger_than_the_output_is_shrunk_with_its_shape(display_hook, kind):
    # 32:9 at 1440 lines in 3840x2160 window textures: 5120 columns do not fit, the shape stays
    got = run(display_hook, 5120 / 1440, 2560, 1440, kind, 0, (3840, 2160))
    assert got[0] <= 3840 and got[1] <= 2160
    assert got[0] / got[1] == pytest.approx(5120 / 1440, rel=2e-3)


@pytest.mark.parametrize("kind", ["store", "set"])
@pytest.mark.parametrize(
    "aspect,w,h",
    [(0, 2560, 1440), (16 / 9, 2560, 1440), (2560 / 1080, 640, 480), (2448 / 1848, 640, 480), (2448 / 1848, 0, 0)],
)
def test_other_outputs_and_sizes_keep_the_display_size(display_hook, kind, aspect, w, h):
    assert run(display_hook, aspect, w, h, kind, 0) == [w, h]
