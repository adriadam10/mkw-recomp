// The optional third-party libraries (DLSS, FSR 3.1, RenHDrum) downloaded when the settings pick them
// (ui/launcher/libs_fetch.cpp): tools/prepare/libs.py in a child process, as launcher_update.cpp runs update.py.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int running;
    long long done, total;   // bytes of the files being downloaded; total 0 before the first file
    int fetched;             // files the last fetch downloaded: they load at the next start
    int finished;            // 1 done, -1 failed
    char features[64];       // what the last fetch was for ("dlss,dlssg")
    char error[256];         // libs.py's message, or SDL's, when it failed
} LibsState;

// libs.py fetch for these features (comma-separated, libs.tsv's names) beside the executable, off the UI thread; a
// fetch running finishes first and then this one starts. Present files are kept, so a repeat costs a Python start.
void libs_fetch_start(const char *features);
LibsState libs_state(void);   // a copy, any time

#ifdef __cplusplus
}
#endif
