// What a bug report needs about the machine (runtime/sysinfo.c), for the menu's "Copy system info" and "Open log folder".
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Plain text, one item per line: OS and build, CPU, RAM, the GPU and its driver as the Vulkan device reports them, and
// every setting with a value (mkw.ini, the menu, the environment). Technical text for the developers, in English like
// the log. The length written (truncated to n - 1), or 0 on failure.
int sysinfo_text(char *buf, size_t n);
// The CPU's brand string ("AMD Ryzen 7 7800X3D 8-Core Processor"); "" when the CPU has none.
void sysinfo_cpu_name(char *out, size_t n);
// The folder with the log: this run's log folder, else the file stderr goes to, else logs/ in the selected
// installation (also holds setup.log). 0 on failure, including an output buffer too small for the full path.
int sysinfo_log_folder(char *out, size_t n);

#ifdef __cplusplus
}
#endif
