// What the parts of the Linux platform layer share (linux.c, linux_sync.c, linux_fs.c, linux_mem.c, linux_sys.c).
#pragma once
#include <stdint.h>

int read_u64_file(const char *path, uint64_t *v);   // linux_sys.c: a decimal number from a sysfs or /proc file
// linux_mem.c: the state of each guest page (pstate), and the write watch over a fresh mapping
#define PS_LIMIT (1ull << 39)
enum { PS_FREE, PS_RESERVED, PS_NONE, PS_READ, PS_RW };   // PS_NONE.. are committed: PLAT_PROT_* + PS_NONE
void ps_set(uint64_t addr, uint64_t n, uint8_t v);
void ww_register(uint64_t addr, uint64_t n);
// linux_share.c: guest pages that are a PlatShared's
int plat_mem_any_shared(uint64_t addr, uint64_t n);
int plat_mem_unshare(uint64_t addr, uint64_t n);   // those of the range become fresh inaccessible pages; 1 on success
void share_forget(uint64_t addr, uint64_t n);      // the range was mapped over
void alt_stack(void);   // linux.c: a signal stack for the calling thread (the fault handler in linux_mem.c runs on it)
