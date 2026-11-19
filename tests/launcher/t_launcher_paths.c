// Real config, boot resolution and launcher completion, driven without SDL or a game by test_launcher_paths.py.
// The includes are cut from production functions and t_game_dir.c's file stubs/UTF-8 entry point by that test.
#include "installation_stubs.inc"
#include "launcher_setup.h"
#include "launcher_paths.inc"

static const char *held;

static void show(int result) {
    char path[PLAT_PATH_MAX * 2];
    romfs_root(path, sizeof path);
    printf("romfs=%s\ncfg=%s\nheld=%s\nvolume=%s\nmods=%s\nversion=%u\ngeneration=%u\nresult=%d\n.\n",
           path, cfg_get("MKW_ROMFS") ? cfg_get("MKW_ROMFS") : "", held ? held : "",
           cfg_value("MKW_VOLUME"), cfg_get("MKW_MODS") ? cfg_get("MKW_MODS") : "",
           cfg_version(), g_state.generation, result);
    fflush(stdout);
}

static int run(int argc, char **argv) {
    exe = argv[0];
    boot_paths(argc, argv);
    const char *env_log_name = getenv("MKW_LOG_NAME");
    printf("log=%s\nlog_env=%s\n", probe_log_name ? probe_log_name : "", env_log_name ? env_log_name : "");
    held = cfg_get("MKW_ROMFS");
    show(0);
    char line[PLAT_PATH_MAX * 3];
    while (fgets(line, sizeof line, stdin)) {
        line[strcspn(line, "\r\n")] = 0;
        char *arg = strchr(line, ' ');
        if (arg) *arg++ = 0;
        int result = 0;
        if (!strcmp(line, "start")) start_setup();
        else if (!strcmp(line, "finish") && arg) {
            g_state.running = 0;
            g_state.finished = atoi(arg);
            setup_paths_finished(&g_state);
        } else if (!strcmp(line, "poll")) setup_paths_finished(&g_state);
        else if (!strcmp(line, "reload")) result = cfg_reload_key("MKW_ROMFS");
        else if (!strcmp(line, "save")) result = cfg_save();
        else if (!strcmp(line, "volume") && arg) cfg_set("MKW_VOLUME", arg);
        else if (!strcmp(line, "mods") && arg) cfg_set("MKW_MODS", arg);
        else if (strcmp(line, "show")) return 2;
        show(result);
    }
    return 0;
}

#include "installation_entry.inc"
