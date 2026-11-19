// Controller batteries the kernel reports but SDL does not: on Linux the xone driver for Xbox One and Series
// controllers (medusalix/xone, GPL-2.0) publishes each pad's battery as a power_supply device (driver/common.c:
// STATUS, CAPACITY_LEVEL, SCOPE and MODEL_NAME, and no percentage), which UPower shows but SDL_GetGamepadPowerInfo
// leaves UNKNOWN. UPower synthesizes a percentage from the band and marks it "should be ignored", so this does
// ignore it: the level band maps to a coarse Low/Medium/High like the Switch 2 voltage bands in input_power.c,
// never to a percent. SDL covers DualShock/DualSense, Switch and XInput pads itself, so this only runs when SDL
// reports UNKNOWN. Matching is by order among Xbox-named pads: the caller passes the pad's Xbox order and reads
// the Nth Xbox supply, so a non-Xbox pad in an earlier slot never shifts the match; mixed sets can still mismatch,
// and then the row keeps SDL's answer.
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "linux_power.h"

static int read_file(const char *root, const char *dev, const char *file, char *buf, size_t size) {
    char path[256];
    if (snprintf(path, sizeof path, "%s/%s/%s", root, dev, file) >= (int)sizeof path) return 0;
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    size_t n = fread(buf, 1, size - 1, f);
    fclose(f);
    buf[n] = 0;
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' ' || buf[n - 1] == '\t')) buf[--n] = 0;
    return 1;
}

// 1 when hay holds needle, ASCII case-insensitive.
static int contains_ci(const char *hay, const char *needle) {
    size_t n = strlen(needle);
    if (!n) return 1;
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < n && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == n) return 1;
    }
    return 0;
}

// 1 when the SDL pad name may be an Xbox pad (an empty name tells nothing, so it passes).
int linux_power_is_xbox(const char *sdl_name) {
    return !sdl_name || !*sdl_name || contains_ci(sdl_name, "xbox");
}

// Sorted names of the Xbox battery supplies under root: SCOPE must be Device (not a laptop BAT0 or a mains
// adapter) with a CAPACITY_LEVEL file (xone's coarse band; its percentage is synthesized downstream and marked to
// be ignored, so it is never read), and the model must mention Xbox or the name start with gip (xone's prefix).
static int collect(const char *root, char names[][64], int max) {
    DIR *d = opendir(root);
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    char scope[32], level[32], model[64];
    while (n < max && (e = readdir(d))) {
        size_t len = strlen(e->d_name);
        if (e->d_name[0] == '.' || len >= sizeof names[0]) continue;
        if (!read_file(root, e->d_name, "scope", scope, sizeof scope) || strcmp(scope, "Device")) continue;
        if (!read_file(root, e->d_name, "capacity_level", level, sizeof level)) continue;
        if (!read_file(root, e->d_name, "model_name", model, sizeof model)) model[0] = 0;
        if (strncmp(e->d_name, "gip", 3) && !contains_ci(model, "xbox")) continue;
        memcpy(names[n], e->d_name, len + 1);
        n++;
    }
    closedir(d);
    for (int i = 1; i < n; i++) {   // readdir order is filesystem order; connection order reads stabler sorted
        char tmp[64];
        strcpy(tmp, names[i]);
        int j = i - 1;
        while (j >= 0 && strcmp(names[j], tmp) > 0) strcpy(names[j + 1], names[j]), j--;
        strcpy(names[j + 1], tmp);
    }
    return n;
}

// The band to the coarse percent input_power_text shows as Low/Medium/High (10/50/90, as for the Switch 2 bands, so
// Low still trips the 15 % low-battery notice; Critical sits below it, Full reads as High).
static int level_pct(const char *level) {
    if (!strcasecmp(level, "critical")) return 5;
    if (!strcasecmp(level, "low")) return 10;
    if (!strcasecmp(level, "normal")) return 50;
    if (!strcasecmp(level, "high")) return 90;
    if (!strcasecmp(level, "full")) return 100;
    return -1;
}

int linux_power_lookup_at(const char *root, int index, const char *sdl_name, InputPower *out) {
    if (!root || !out || index < 0 || !linux_power_is_xbox(sdl_name)) return 0;
    char names[INPUT_MAX_PLAYERS][64];
    if (index >= collect(root, names, INPUT_MAX_PLAYERS)) return 0;
    const char *dev = names[index];
    char status[32] = "", level[32] = "";
    read_file(root, dev, "status", status, sizeof status);
    read_file(root, dev, "capacity_level", level, sizeof level);
    int pct = level[0] ? level_pct(level) : -1;
    if (!strcasecmp(status, "Charging")) {
        out->state = INPUT_POWER_CHARGING;   // percent stays -1 without a band: the row shows "Charging"
    } else if (!strcasecmp(status, "Full")) {
        out->state = INPUT_POWER_CHARGED, out->percent = pct < 0 ? 100 : pct, out->coarse = 1;
        return 1;
    } else if (!strcasecmp(status, "Discharging")) {
        if (pct < 0) return 0;   // discharging but no band: nothing to show, keep SDL's answer
        out->state = INPUT_POWER_BATTERY;
    } else if (!strcasecmp(status, "Not charging")) {
        out->state = INPUT_POWER_WIRED;   // xone reports this with no band when no battery pack is fitted
        out->percent = -1, out->coarse = 0;
        return 1;
    } else {
        return 0;
    }
    out->percent = pct, out->coarse = pct >= 0;
    return 1;
}

int linux_power_lookup(int index, const char *sdl_name, InputPower *out) {
    return linux_power_lookup_at("/sys/class/power_supply", index, sdl_name, out);
}
