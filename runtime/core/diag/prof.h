// Between perf.c (frame timing, always built) and the OS's sampling profiler (prof_win.c on Windows).
#pragma once
#include <stdint.h>

extern volatile uint32_t prof_render_tid;   // the thread that presents (perf_present)
extern volatile uint32_t prof_main_tid;     // the guest main thread (perf_main_thread)
extern volatile uint32_t prof_replay_tid;   // the asynchronous replay (perf_replay_thread): its waits hold up the frame too

// The host thread id of the guest thread with that handle, 0 when there is none (kernel_diag.c; MKW_PROFILE_HANDLE).
uint32_t kern_thread_tid(uint32_t handle);
// The sampler; perf.c's weak versions do nothing.
int prof_sampler_start(void);               // MKW_PROFILE: starts sampling; 1 when it runs
void prof_sampler_write(void);              // writes the profile once (prof_write)
void prof_sampler_frame(uint64_t frame, uint64_t now, uint64_t d);   // every present; d: the interval, 0 on the first

// MKW_FRAMES (frames.c): one row per present, from perf_present; flushed at exit (perf_summary_flush).
void frames_row(uint64_t frame, uint64_t interval_ns, const uint64_t wait_ns[], uint64_t replay_ns);
void frames_flush(void);

// MKW_CPU_COUNTERS=1 (cpuctr.c): the calling thread's CPU time, cycles, instructions, cache misses and time waiting
// for a core (run queue) since its last tick, to a class; cpuctr_add puts a measured span (wall time, a wait) in one.
// frames.c takes the sums per frame. REPLAY is the replay thread's recording of a submission, REPLAY_OUT the rest of
// its time, CHUNKS the chunk threads' recordings (their time between chunks is CHUNKS_OUT).
enum { CPUCTR_REPLAY, CPUCTR_REPLAY_OUT, CPUCTR_CHUNKS, CPUCTR_CHUNKS_OUT, CPUCTR_GAME, CPUCTR_N };
// WAIT: the replay's waits for its chunks (REPLAY), for work (REPLAY_OUT); HELD: its waits for a held submission's work
// (REPLAY_OUT: the queue order, nvn_qorder.c)
enum { CPUCTR_CPU, CPUCTR_CYC, CPUCTR_INS, CPUCTR_MISS, CPUCTR_RQ, CPUCTR_WALL, CPUCTR_WAIT, CPUCTR_HELD, CPUCTR_V };
int cpuctr_on(void);
void cpuctr_tick(int cls);
void cpuctr_add(int cls, int what, uint64_t ns);
void cpuctr_take(uint64_t out[CPUCTR_N][CPUCTR_V]);
