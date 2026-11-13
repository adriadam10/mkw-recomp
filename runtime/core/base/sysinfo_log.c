// Locate the folder for "Open log folder" independently of the Vulkan system-info report, so launcher paths can
// be checked without a GPU or graphics headers (tests/test_game_dir.py).
#include <stdio.h>

#include "cfg.h"
#include "host.h"
#include "platform/platform.h"
#include "sysinfo.h"

int sysinfo_log_folder(char *out, size_t n) {
    if (!out || !n) return 0;
    const char *log = rt_log_path();
    if (log) {
        int written = snprintf(out, n, "%s", log);
        if (written < 0 || (size_t)written >= n) return 0;
    } else if (!plat_stderr_path(out, n)) {
        // The launcher opens no game log itself. Setup failures and previous game runs still leave their logs in
        // the selected installation, even when its directory is on another drive.
        const char *dir = cfg_game_dir();
        if (!*dir) return 0;
        int written = snprintf(out, n, "%s/logs", dir);
        return written >= 0 && (size_t)written < n;
    }
    char *s = plat_last_sep(out);
    if (!s) return 0;
    if (s == out || (s - out == 2 && out[1] == ':')) s[1] = 0;   // preserve POSIX and Windows drive roots
    else *s = 0;
    return 1;
}
