// MKW_FPS=auto: the present interval (1-4 at 60 Hz: 60, 30, 20, 15 fps; in ticks of FpsAuto.hz otherwise, up to the
// same 15 fps: 8 at 120 Hz) from the time the game measured for each frame (its
// frame object's [+0x128], game.c). The game's own choice stops at 2: below 30 fps the race ran in slow motion
// (0.75-0.83x on the Radeon 660M at 36 ms a frame). Its rule (acc += measured - interval * 16.7 ms, capped at
// +-66.7 ms, interval = acc / 16.7 + 1) taken to 4 would cycle 2, 3, 4 on a steady 36 ms frame, each step undoing the
// last; so this one has hysteresis: up as soon as the median of the last 8 frames is 8 % past the interval, to the
// one it fits that way; down only when the median of the last two seconds fits a lower interval with 15 % to spare.
// Below 30 fps, a timed trial of the next faster interval as well: where the CPU's clock follows its load, a slower
// interval makes the frames slower and nothing would ever fit the faster one again (#655, below).
#include <string.h>

#include "host.h"

#define UP_FRAMES 8
#define DOWN_MARGIN 0.85   // lat4: race frames of 25-40 ms moved it 2 <-> 3 every few seconds with 0.92 over 1 s
#define DOWN_SECONDS 2
#define HOLD_MIN_S 5
#define HOLD_MAX_S 60
#define PINGPONG_S 10
#define UP_MARGIN 1.08   // gp15b: 50-54 ms spikes at interval 3 moved it to 4 one or two times every 5 s
// A stall: frames past twice what the slowest interval holds (133 ms), which no interval fits (a CPU held at a low
// clock, a load), after frames that fitted the interval. It moves nothing for up to STALL_MAX_S: mp1/mp2 (Steam Deck,
// 2 players, interval 3) went to 4 on each stall of 300 ms frames and held it, doubled on each return, so the race ran
// at 15 fps between them where 20 held at full speed. Longer than that, it is the machine's speed and counts as any
// frame.
#define STALL_FACTOR 2.0
#define STALL_MAX_S 20
// Trials. #655 (BC-250, RADV, 0.1.15, 1.8.0): with MKW_FPS=30 a race held 28.4-29.8 fps for 8 minutes, the big
// submission recorded in 17 ms (median); with auto, a heavy stretch took it to 4 and it stayed at 15 fps for minutes,
// the same submission recorded in 28.6 ms: the CPU, idle most of each 66.7 ms, ran at a lower clock, so no frame
// fitted 20 fps to come down. Below 30 fps (interval past 2 at 60 Hz), and only once an interval at least as fast as
// the one tried held HELD_S in this session, the next faster one is tried for TRIAL_S each time the interval has been
// kept TRIAL_FIRST_S: it stays if its last frames fit it (the rule that moves up), else back, and the wait doubles up
// to TRIAL_MAX_S; one that held counts as failed too if an up follows within HOLD_MIN_S. A machine that slow from the
// start never held the faster interval and gets no trials, nor do frames slower than the interval they are at (the
// CPU busy all of each frame: its clock as high as the load takes it).
// Measured on --probe-fps's model of #655: 0 -> 100 % of the frames at 30 fps after a heavy stretch; where the faster
// interval no longer holds, 3 % of the frames on trials in 2 minutes.
#define HELD_S 5
#define TRIAL_FIRST_S 10
#define TRIAL_MAX_S 60
#define TRIAL_S 1.0   // the clock follows the load within tens of ms; the verdict is on its last 8 frames
// Game time. A faster interval is kept while the game runs at full speed: fa886-169-60-1 (26/11/2026, Windows, 16:9
// at MKW_FPS=60, a 4-race Grand Prix with MKW_FRAME_TRACE) went up on bursts of 18-25 ms frames that the pacer catches
// up (nvn_api_obj.c pace(): a frame late by under a period keeps its slot chain), so auto would have run at 30 for 30 %
// of a race whose game time ran at 1.0000x at 60; the race that 60 did not hold ran at 0.970x, its frames mostly
// fitting 60 and a few a whole period late. So an up needs game time lost (the frames' real time past the game time
// they advanced, decayed over LOSS_TAU_S) past LOSS_SHARE of it, and that alone moves up one step, counting only the
// frames run at this interval that a stall does not explain (a hitch of 200 ms frames loses time no interval keeps;
// with frame generation off the game picks 1 or 2 itself, game.c auto_want, and its own choice's loss is no reason
// for 20 fps). One frame's part is at most LOSS_FRAME_MS: a single hitch (a shader built, a 1 s freeze, which kept
// the sum past the threshold for 6 s) does not open it alone, two do. A trial (below) holds only if it loses no game
// time either and ends at once when it does: a failed one costs about LOSS_FRAME_MS, not a second of frames that do
// not fit (45 ms frames at 2: 350 ms). median_up (game.c, MKW_FPS_AUTO_LOSS=0 or its A blocks) moves up on the
// median alone, as before, for an A/B inside one run.
#define LOSS_TAU_S 2.0
#define LOSS_SHARE 0.02
#define LOSS_FRAME_MS 30.0

static double median(const FpsAuto *a, unsigned count) {
    double v[FPS_AUTO_HIST];
    unsigned n = count < a->n ? count : a->n;
    for (unsigned i = 0; i < n; i++) v[i] = a->hist[(a->head + FPS_AUTO_HIST - 1 - i) % FPS_AUTO_HIST];
    for (unsigned i = 1; i < n; i++)   // insertion sort: at most FPS_AUTO_HIST (120) values
        for (unsigned j = i; j > 0 && v[j - 1] > v[j]; j--) {
            double t = v[j];
            v[j] = v[j - 1], v[j - 1] = t;
        }
    return n ? v[n / 2] : 0.0;
}

// The vsync rate the interval counts in (MKW_LOGIC_HZ), one tick of it, and the slowest interval: 15 fps at any rate
// (120 Hz: 8), not 4 ticks, which at 120 Hz stopped at 30 fps and ran a 19 fps race at 0.65x (lhz120r)
static int rate(const FpsAuto *a) { return a->hz > 0 ? a->hz : 60; }
static double period_ms(const FpsAuto *a) { return 1000.0 / rate(a); }
static int max_iv(const FpsAuto *a) { return (FPS_AUTO_MAX * rate(a) + 30) / 60; }

static int fitting(const FpsAuto *a, double ms, double margin) {   // the smallest interval a frame of ms fits in
    int top = max_iv(a);
    for (int iv = 1; iv < top; iv++)
        if (ms <= iv * period_ms(a) * margin) return iv;
    return top;
}

void fps_auto_reset(FpsAuto *a) {
    memset(a, 0, sizeof *a);
    a->iv = 1;
}

static unsigned frames_of(const FpsAuto *a, double s, int iv) { return (unsigned)(s * rate(a) / iv); }

// The frame's game time lost into both sums (none below zero: a frame the pacer caught up only pays back the one
// before it), and whether each is past LOSS_SHARE of its window
static void lose(FpsAuto *a, double real_ms, double game_ms) {
    if (real_ms <= 0) return;   // the first frame: no period yet
    if (game_ms < 0) game_ms = a->iv * period_ms(a);   // not known (the probe's model): its interval's
    double k = real_ms / (LOSS_TAU_S * 1000.0), keep = k < 1 ? 1 - k : 0, lost = real_ms - game_ms;
    if (lost > LOSS_FRAME_MS) lost = LOSS_FRAME_MS;
    // a frame no interval holds counts where it is the machine's speed (frames that did not fit before it: 200 ms ones
    // from the start; or a stall past STALL_MAX_S), not as a stall's first frames: two of 300 ms after runs of 18 ms
    // frames the pacer catches up opened the threshold before the median of 8 showed the stall
    int stall = real_ms > max_iv(a) * period_ms(a) * STALL_FACTOR;
    int here = game_ms > (a->iv - 0.5) * period_ms(a) && game_ms < (a->iv + 0.5) * period_ms(a);
    a->lost = a->lost * keep + (!stall || !a->fits || a->stall_ms >= STALL_MAX_S * 1000.0 ? lost : 0);
    a->lost_held = a->lost_held * keep + (here && !stall ? lost : 0);
    if (a->lost < 0) a->lost = 0;
    if (a->lost_held < 0) a->lost_held = 0;
}

// Past LOSS_SHARE of s seconds of game time (LOSS_TAU_S: the steady state; less from zero, a trial: the decayed sum of
// a steady loss reaches about s (1 - s / 2 tau)), and past one period of the interval: a frame late by less keeps the
// pacer's slot chain and the next ones can catch it up (at 15 fps a period is 67 ms, past the 40 of the share)
static int losing_in(const FpsAuto *a, double lost, double s) {
    double share = LOSS_SHARE * 1000.0 * s * (s < LOSS_TAU_S ? 1 - s / (2 * LOSS_TAU_S) : 1);
    return lost > share && lost > a->iv * period_ms(a);
}

static int losing(const FpsAuto *a, double lost) { return losing_in(a, lost, LOSS_TAU_S); }

static void moved(FpsAuto *a) { a->lost = a->lost_held = 0; }   // a new interval: its own game time from here

// The slowest interval the caller uses now (game.c: 30 fps out of a race; 0 none). Over it the interval goes back to
// it: game.c capped only what it wrote, so a load's long frames took the controller to 3 and the race began at 20 fps
// (fal-219-2, 26/11/2026). A change starts the game time lost over: a load's frames are not the race's.
void fps_auto_cap(FpsAuto *a, int cap) {
    if (cap == a->cap) return;
    a->cap = cap;
    moved(a);
    if (cap && a->iv > cap) a->iv = cap, a->since = 0, a->hold = 0, a->downed = 0, a->trial = 0, a->trial_wait = 0;
}

static int top(const FpsAuto *a) { return a->cap && a->cap < max_iv(a) ? a->cap : max_iv(a); }

// The frame's part of a trial: 1 while one is under way (the rules that move the interval wait for its end).
static int trial_step(FpsAuto *a, double v) {
    int slow = 2 * rate(a) / 60;   // 30 fps in ticks: trials only below it
    if (a->since >= frames_of(a, HELD_S, a->iv) && a->iv < 32) a->held |= 1u << a->iv;
    if (a->trial) {
        int lost = losing_in(a, a->lost, TRIAL_S);   // past what the whole trial may lose: it failed, at once
        if (--a->trial && !lost) return 1;
        if (!lost && a->trial == 0 && fitting(a, v, UP_MARGIN) <= a->iv) {
            a->trial_ok = 1;   // it holds, unless an up undoes it within HOLD_MIN_S (fps_auto_step)
        } else {
            a->iv = a->trial_from, a->trial = 0;
            a->trial_s = a->trial_s * 2 > TRIAL_MAX_S ? TRIAL_MAX_S : a->trial_s * 2;
            moved(a);
        }
        a->since = 0, a->downed = 0, a->hold = 0;
        a->trial_wait = frames_of(a, a->trial_s, a->iv);
        return 1;
    }
    // (nothing as fast as iv - 1 ever held; or frames past this interval too: the CPU is busy all of each frame, so its
    // clock is already as high as the load takes it). Trials of 60 from 30 as well, with frame generation on, left the
    // race's time at 30 as it was (fta-219-1/2, 26/11/2026, two Grand Prix with the A/B blocks one way and the other:
    // -0.05 points, the scenes' part 7.4) for 0.9 more changes a minute: in the game the rule that comes down left 30
    // within 10 s in 6 of 11 stretches, sooner than a closed loop on frames recorded at 60 had it
    if (a->iv <= slow || a->iv >= 32 || !(a->held & ((1u << a->iv) - 1)) || v > a->iv * period_ms(a) * UP_MARGIN)
        return 0;
    if (!a->trial_s) a->trial_s = TRIAL_FIRST_S;
    if (!a->trial_wait) a->trial_wait = frames_of(a, a->trial_s, a->iv);
    if (--a->trial_wait) return 0;
    a->trial_from = a->iv--, a->since = 0, a->downed = 0;
    moved(a);
    a->trial = frames_of(a, TRIAL_S, a->iv);
    return 1;
}

static double median_real(const FpsAuto *a) {   // the last UP_FRAMES real periods
    double v[UP_FRAMES];
    for (unsigned i = 0; i < UP_FRAMES; i++) v[i] = a->real[i];
    for (unsigned i = 1; i < UP_FRAMES; i++)
        for (unsigned j = i; j > 0 && v[j - 1] > v[j]; j--) {
            double t = v[j];
            v[j] = v[j - 1], v[j - 1] = t;
        }
    return v[UP_FRAMES / 2];
}

// measured_ms: the game's own measure of the frame; real_ms: the time since the last frame began (0 unknown); game_ms:
// the game time it advanced (0: its interval's). Up on the larger of the first two, once game time is lost: the game's
// measure can stay under the real period (lat5: 3 -> 1 on 8.7 ms frames before the start, then the race at 0.79x at
// interval 1). Down one step at a time; after an up, no down for hold_s seconds, doubled (to 60) when the up undid a
// down of the last 10 s.
int fps_auto_step(FpsAuto *a, double measured_ms, double real_ms, double game_ms) {
    lose(a, real_ms, game_ms);   // at the interval the frame ran at, before anything moves it
    a->hist[a->head] = measured_ms;
    a->head = (a->head + 1) % FPS_AUTO_HIST;
    a->real[a->n_real++ % UP_FRAMES] = real_ms;
    if (a->n < FPS_AUTO_HIST) a->n++;
    a->since++;
    if (a->hold) a->hold--;
    if (!a->hold_s) a->hold_s = HOLD_MIN_S;
    double v = 0;
    if (a->n >= UP_FRAMES) {
        double m = median(a, UP_FRAMES), r = a->n_real >= UP_FRAMES ? median_real(a) : 0;
        v = m > r ? m : r;
    }
    // a trial that held HOLD_MIN_S without an up: the next one from here starts over at the first wait. An up before
    // counts it failed: 34 +- 3 ms frames passed a 1 s trial of 30 and lost time 2 s later, and with each trial held
    // the next came 10 s on (7.7 changes a minute; 3.7 so). PINGPONG_S was too long: a scene 10 s at 29 ms, 10 at 38
    // took the up into the slower stretch for the trial's failure and held 20 fps 73 % of the time (50 so).
    if (a->trial_ok && a->since >= frames_of(a, HOLD_MIN_S, a->iv)) a->trial_ok = 0, a->trial_s = TRIAL_FIRST_S;
    int at = a->iv;
    if (trial_step(a, v)) return a->iv;
    if (a->iv != at) return a->iv;
    if (a->since >= UP_FRAMES && a->n >= UP_FRAMES) {
        int stalled = a->fits && v > max_iv(a) * period_ms(a) * STALL_FACTOR;
        a->stall_ms = stalled ? a->stall_ms + (real_ms > measured_ms ? real_ms : measured_ms) : 0;
        int up = stalled && a->stall_ms < STALL_MAX_S * 1000.0 ? a->iv : fitting(a, v, UP_MARGIN);
        if (up > top(a)) up = top(a);
        // fitted: by the median, or by game time where a median past the interval loses none (the pacer's bursts),
        // so a stall then still gets its allowance instead of going to the slowest interval at once
        if (!stalled) a->fits = up <= a->iv || (!a->median_up && !losing(a, a->lost_held));
        // frames that fit, a few a whole period late (fa886-169-60-1's slowest race): one step
        if (!a->median_up && !stalled && up <= a->iv && a->iv < top(a) && losing(a, a->lost_held)) up = a->iv + 1;
        if (up > a->iv && (a->median_up || losing(a, a->lost))) {
            int undid_down = a->downed && a->since < (unsigned)(PINGPONG_S * rate(a) / a->iv);
            if (a->trial_ok) {   // the trial that held did not: as one that failed, and the hold doubles
                a->trial_ok = 0, undid_down = 1;
                a->trial_s = a->trial_s * 2 > TRIAL_MAX_S ? TRIAL_MAX_S : a->trial_s * 2;
            }
            a->hold_s = undid_down ? (a->hold_s * 2 > HOLD_MAX_S ? HOLD_MAX_S : a->hold_s * 2) : HOLD_MIN_S;
            a->iv = up, a->since = 0, a->downed = 0, a->trial_wait = 0;
            a->hold = (unsigned)(a->hold_s * rate(a) / a->iv);
            moved(a);
            return a->iv;
        }
    }
    // two seconds of frames at this interval, as many as the history keeps: past it (144 Hz at 2, 240 Hz under 4) a
    // window the history never fills held the interval for good
    unsigned window = DOWN_SECONDS * (unsigned)rate(a) / (unsigned)a->iv;
    if (window > FPS_AUTO_HIST) window = FPS_AUTO_HIST;
    if (a->iv > 1 && !a->hold && a->since >= window && a->n >= window) {
        // one step at a time: the game's measure can be far under the frame's real cost (lat6: 9 ms at interval 4,
        // 66 ms frames), so a step down is a trial the real period confirms or undoes within 8 frames
        if (fitting(a, median(a, window), DOWN_MARGIN) < a->iv) a->iv--, a->since = 0, a->downed = 1, moved(a);
    }
    return a->iv;
}
