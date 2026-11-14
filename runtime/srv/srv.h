// The online server built into the executable: `mkw.exe --server [--host ADDR]... [--port P] [--data DIR]
// [--seconds S]`, a process of its own (the start screen or the game starts it, and it ends with them), so a fault in it
// never takes the game down.
#pragma once

// argv after "--server". Returns the process's exit code: 0 after --seconds, 1 when nothing could be bound or the
// certificates could not be made, 2 on a bad argument.
int srv_main(int argc, char **argv);

// A line in the server's log (<data>/logs/server.log, and stderr), with the time in front.
void srv_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// A numeric address's bytes in network order: 4 (IPv4), 16 (IPv6), 0 when ip is not one.
int srv_ip_bytes(const char *ip, unsigned char out[16]);

// Milliseconds of a clock that only goes forward (timers), and the server's data folder (--data).
#include <stddef.h>
#include <stdint.h>
uint64_t srv_ms(void);
const char *srv_data_dir(void);
// Cryptographically random bytes (ids, room numbers, tokens); 1 on success.
int srv_random(void *buf, size_t n);
