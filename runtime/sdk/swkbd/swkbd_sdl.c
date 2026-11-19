// The software keyboard's front end for SDL windows (swkbd.h). A request for a number or a code (numeric mode, or
// only download-code characters in at most 12 units) opens the overlay's number pad (ui/menu/text_entry.h), which a
// controller, the mouse or the keyboard works. Any other request, and one no overlay picks up (no window: probes),
// takes the starting text at once, as MKW_SWKBD_ANSWER=<initial> does: the game's initial text, else the default name
// from the catalog. MKW_SWKBD_TEST=cancel stands for Cancel pressed.
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "swkbd.h"
#include "ui/menu/text_entry.h"

#define DISABLED_OUTSIDE_DOWNLOAD_CODE (1u << 7)   // the applet's InvalidCharFlag bit: a code of digits and letters

static int wants_keypad(const SwkbdRequest *req) {
    return req->numeric || ((req->disabled & DISABLED_OUTSIDE_DOWNLOAD_CODE) && req->max_len <= 12);
}

int text_entry_run(const SwkbdRequest *req, uint16_t *out, size_t cap, size_t *n) {
    const char *test = cfg_get("MKW_SWKBD_TEST");
    if (test && !strcmp(test, "cancel")) { *n = 0; return 0; }
    if (wants_keypad(req)) {
        char prompt[0x101 * 3 + 1], initial[KB_LIMIT * 3 + 1], digits[KB_LIMIT + 1];
        size_t k = 0;
        while (k < 0x100 && req->prompt[k]) k++;
        swkbd_utf16_to_utf8(req->prompt, k, prompt, sizeof prompt);
        for (k = 0; k < KB_LIMIT && req->initial[k]; k++) {}
        swkbd_utf16_to_utf8(req->initial, req->game_initial ? k : 0, initial, sizeof initial);
        int r = text_entry_keypad(prompt, initial, req->min_len, req->max_len, req->password, digits, sizeof digits);
        if (r >= 0) {
            *n = r ? swkbd_utf8_to_utf16(digits, out, cap) : 0;
            return r;
        }
        rt_log("swkbd: no overlay drew the number pad within 3 s");
    }
    size_t k = 0;
    while (k < cap && req->initial[k]) k++;
    memcpy(out, req->initial, k * 2);
    *n = k;
    rt_log("swkbd: no text entry window for this request: the %s text is taken", req->game_initial ? "game's" : "default");
    return k > 0;
}
