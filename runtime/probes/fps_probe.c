// --probe-fps: MKW_FPS=auto's controller (core/boot/fps_auto.c) on synthetic frame times. Each case feeds frames of
// a fixed length and checks where the interval settles and how often it changed: a steady frame must give one
// interval and no back and forth (the game's own rule taken to 4 would cycle 2, 3, 4 on 36 ms frames). Needs no game.
#include <stdio.h>

#include "host.h"

static int fails;

// The real period of a frame of ms at interval iv: the pacer holds it to the interval's period.
static double real_of(double ms, int iv) { return ms > iv * 1000.0 / 60 ? ms : iv * 1000.0 / 60; }

// The same with the pacer's slot chain (nvn_api_obj.c pace()), for frames whose time varies from one to the next: one
// late by under a period keeps the chain, so the next can be shorter than the interval and no game time is lost;
// later than that, the chain starts over at it. late: how far the last present was past its slot.
static double paced(double *late, double ms, int iv) {
    double period = iv * 1000.0 / 60, real = ms > period - *late ? ms : period - *late;
    *late += real - period;
    if (*late >= period) *late = 0;   // started over: the time past the slot was game time lost
    return real;
}

// frames of ms each (plus jitter of +-j alternating); returns the interval after them, counting the changes. real:
// the real frame time when the game's measure (ms) falls short of it, else 0 (the measure is the frame time)
static int feed_real(FpsAuto *a, double ms, double j, double real, int frames, int *changes) {
    for (int i = 0; i < frames; i++) {
        int prev = a->iv;
        double m = ms + (i & 1 ? j : -j);
        if (fps_auto_step(a, m, real_of(real > m ? real : m, prev), -1) != prev) (*changes)++;
    }
    return a->iv;
}

static int feed(FpsAuto *a, double ms, double j, int frames, int *changes) {
    return feed_real(a, ms, j, 0, frames, changes);
}

static void expect(const char *what, int iv, int want, int changes, int max_changes) {
    int ok = iv == want && changes <= max_changes;
    if (!ok) fails++;
    printf("%s fps: %-38s interval %d (want %d), %d changes (at most %d)\n", ok ? "ok  " : "FAIL", what, iv, want,
           changes, max_changes);
}

int probe_fps(void) {
    static const struct { const char *what; double ms, jitter; int want; } steady[] = {
        {"15 ms frames (fits 60)", 15.0, 1.0, 1},       {"16 ms frames (just fits 60)", 16.0, 0.3, 1},
        {"20 ms frames", 20.0, 2.0, 2},                 {"36 ms frames (Radeon 660M race)", 36.0, 3.0, 3},
        {"45 ms frames", 45.0, 4.0, 3},                 {"55 ms frames", 55.0, 4.0, 4},
        {"120 ms frames (slower than 15)", 120.0, 10.0, 4},
    };
    for (unsigned i = 0; i < sizeof steady / sizeof steady[0]; i++) {
        FpsAuto a;
        fps_auto_reset(&a);
        int changes = 0;
        feed(&a, steady[i].ms, steady[i].jitter, 120, &changes);   // settle
        int settled = changes;
        changes = 0;
        int iv = feed(&a, steady[i].ms, steady[i].jitter, 1200, &changes);   // 20 s at 60 Hz: no more changes
        expect(steady[i].what, iv, steady[i].want, changes, 0);
        if (settled > 2) fails++, printf("FAIL fps: %s took %d changes to settle\n", steady[i].what, settled);
    }

    FpsAuto a;   // a loading hitch of 4 frames at 60 fps: the median of 8 does not move
    fps_auto_reset(&a);
    int changes = 0;
    feed(&a, 15.0, 1.0, 120, &changes);
    feed(&a, 200.0, 0.0, 3, &changes);
    int iv = feed(&a, 15.0, 1.0, 120, &changes);
    expect("15 ms with a 3-frame 200 ms hitch", iv, 1, changes, 0);

    fps_auto_reset(&a);   // a load (36 ms race, then 12 ms menu): back to 60 within about two seconds
    changes = 0;
    feed(&a, 36.0, 3.0, 300, &changes);
    iv = feed(&a, 12.0, 1.0, 150, &changes);   // 15 +- 1 would not: its median, 16, is in the hysteresis gap
    expect("36 ms then 12 ms, 150 frames later", iv, 1, changes, 3);

    // lat6: the game measures 9 ms while frames take 60: a step down is undone and the hold doubles, so in 60 s at
    // most a handful of trials
    fps_auto_reset(&a);
    changes = 0;
    feed_real(&a, 9.0, 0.5, 60.0, 60, &changes);
    int c0 = changes;
    changes = 0;
    iv = feed_real(&a, 9.0, 0.5, 60.0, 900, &changes);   // 60 s at 15 fps
    expect("game measures 9 ms, frames take 60", iv, 4, changes + (c0 > 3) * 99, 8);

    // lat4: race frames spread over 25-40 ms, around 30 fps: one interval, no back and forth. Paced: at 30 the shorter
    // frames catch up the longer ones (at most 7 ms past a slot), so 30 loses no game time
    fps_auto_reset(&a);
    changes = 0;
    static const double spread[] = {25, 31, 38, 27, 40, 29, 33, 26, 36, 30, 28, 39, 32, 34, 27, 31};
    double late = 0;
    for (int i = 0; i < 300; i++) {
        int prev = a.iv;
        if (fps_auto_step(&a, spread[i % 16], paced(&late, spread[i % 16], prev), -1) != prev) changes++;
    }
    expect("frames spread over 25-40 ms", a.iv, a.iv == 2 ? 2 : 3, changes, 1);

    // lat5: the game measured 14 ms while its frames took 21 (the race at 0.79x at interval 1): up on the real period,
    // and back down at most a couple of times in 40 s (the hold doubles)
    fps_auto_reset(&a);
    changes = 0;
    feed_real(&a, 14.0, 0.5, 21.0, 30, &changes);
    int after = a.iv;
    feed_real(&a, 14.0, 0.5, 21.0, 2400, &changes);
    expect("game measures 14 ms, frames take 21", after, 2, changes, 5);

    // fa886-169-60-1 (26/11/2026, race 1, game time 1.0000x at 60): runs of 8-12 frames of 17-20 ms, each caught up by
    // the 15 ms frames after it (the pacer keeps a frame late by under a period on its slot chain), no game time lost;
    // the median of 8 went past 60's 18 ms and auto would have held 30 for 30 % of the race. 8 frames of 18.3 ms (13 ms
    // late at the end), then 6 of 14.45: interval 1 throughout.
    fps_auto_reset(&a);
    changes = 0;
    for (int f = 0; f < 1400; f++) {   // 100 runs, 23 s
        int prev = a.iv;
        double real = f % 14 < 8 ? 18.3 : 14.45;
        if (fps_auto_step(&a, real - 1.0, real, -1) != prev) changes++;
    }
    expect("runs of 18 ms frames the pacer catches up", a.iv, 1, changes, 0);

    // fa886-169-60-1's race 3 (0.970x at 60): frames that fit 60 and now and then one a whole period late (the pacer
    // starts its chain over: game time lost). A 36 ms frame (20 measured) every 30 ticks, 13 ms ones between: 30 holds
    // it at 1.00x, so up to 2 and there nearly all the time (each step down loses time again and doubles the hold)
    fps_auto_reset(&a);
    changes = 0;
    double at2t = 0, all_t = 0, game_t = 0;
    late = 0;
    for (int t = 0; t < 7200;) {   // 120 s of game time
        int prev = a.iv, slow = t / 30 != (t + prev) / 30;   // this frame's ticks cross a 30th
        double real = paced(&late, slow ? 36.0 : 13.0, prev);
        if (fps_auto_step(&a, slow ? 20.0 : 13.0, real, -1) != prev) changes++;
        t += prev, game_t += prev * 1000.0 / 60, all_t += real, at2t += prev == 2 ? real : 0;
    }
    int share2 = (int)(at2t * 100 / all_t);
    printf("     fps: a frame a period late every 30 ticks: interval 2 in %d %% of the time, game time %.3fx\n", share2,
           game_t / all_t);
    expect("one frame a period late every 30 ticks", a.iv, 2, changes + (share2 < 90) * 99, 10);

    // mp1/mp2 (Steam Deck, 2 players): 38 ms measured, 45 ms frames (interval 3) and, every ~20 s, stalls of ~300 ms
    // frames for ~8 s (the CPU held at 400 MHz). No interval holds a 300 ms frame: going to 4 for each one, then
    // holding it (doubled on each return) kept the race at 15 fps where it runs 20 between the stalls.
    fps_auto_reset(&a);
    changes = 0;
    feed_real(&a, 38.0, 2.0, 45.0, 120, &changes);
    int at3 = 0, normal = 0;
    for (int cycle = 0; cycle < 8; cycle++) {
        for (int i = 0; i < 27; i++) {
            int prev = a.iv;
            if (fps_auto_step(&a, 300.0, 300.0, -1) != prev) changes++;
        }
        for (int i = 0; i < 240; i++) {
            int prev = a.iv;
            double m = 38.0 + (i & 1 ? 2.0 : -2.0);
            if (fps_auto_step(&a, m, real_of(45.0, prev), -1) != prev) changes++;
            normal++, at3 += a.iv == 3;
        }
    }
    int pct = normal ? at3 * 100 / normal : 0;
    printf("     fps: stalls of 300 ms between 45 ms frames: interval 3 in %d %% of the frames between them\n", pct);
    expect("45 ms frames with 8 s stalls of 300 ms", a.iv, 3, changes + (pct < 90) * 99, 4);

    // a machine that is that slow from the start: to 4 at once, no stall allowance (nothing fitted before)
    fps_auto_reset(&a);
    changes = 0;
    iv = feed(&a, 200.0, 5.0, 10, &changes);   // 2 s of 200 ms frames
    expect("200 ms frames from the start, 10 frames", iv, 4, changes, 1);

    // fitting 45 ms frames, then 200 ms frames for good: after the stall allowance it goes to 4
    fps_auto_reset(&a);
    changes = 0;
    feed_real(&a, 38.0, 2.0, 45.0, 120, &changes);
    changes = 0;
    iv = feed(&a, 200.0, 5.0, 300, &changes);   // 60 s
    expect("45 ms, then 200 ms frames for 60 s", iv, 4, changes, 1);

    // #655 (BC-250, RADV, 0.1.15): the CPU's clock follows its load, so a slower interval makes every frame slower. The
    // race held 30 fps with MKW_FPS=30 (28.4 at worst over 8 minutes, the big submission recorded in 17 ms), but auto,
    // once a heavy stretch took it to 4, stayed at 15 fps for minutes: there the same submission took 28.6 ms, and
    // no measure fitted 20 fps to come down. Modelled: 30 ms of work at full clock; the clock goes from 1 at 85 % busy
    // down to 0.5 at 60 % (linear between), following the load within ~10 frames. At interval 2 that is 30 ms (30
    // fps), at 3 ~37 ms, at 4 ~46 ms: neither of the two fits the interval below it. After 4 s of 70 ms work it should
    // come back to 30 fps and stay there most of the time.
    for (int with_held = 1; with_held >= 0; with_held--) {
        fps_auto_reset(&a);
        changes = 0;
        double clk = 1.0;
        int at2 = 0, frames = 0;
        for (int f = 0; f < 4800; f++) {   // 30 s at 2 (if it holds), 4 s heavy, then ~60 s
            double base = f >= 1800 && f < 2040 ? 70.0 : 30.0;
            if (!with_held && f < 1800) base = 70.0;   // never held 30 fps or 20 before: no trials
            double cost = base / clk, period = a.iv * 1000.0 / 60, real = cost > period ? cost : period;
            double u = cost / real, want = u >= 0.85 ? 1.0 : u <= 0.6 ? 0.5 : 0.5 + 0.5 * (u - 0.6) / 0.25;
            clk += (want - clk) * 0.1;
            int prev = a.iv;
            if (fps_auto_step(&a, cost, real, -1) != prev && f >= 2040) changes++;
            if (f >= 2640) frames++, at2 += a.iv == 2;   // from 10 s after the heavy stretch
        }
        int pct = frames ? at2 * 100 / frames : 0;
        if (with_held) {
            printf("     fps: clock following the load: interval 2 in %d %% of the frames from 10 s after the heavy "
                   "stretch\n", pct);
            expect("clock following the load (#655)", a.iv, 2, changes + (pct < 75) * 99, 12);
        } else {   // 70 ms work throughout: 4 is right, and with no faster interval ever held, no trials
            changes = 0;
            for (int f = 0; f < 3600; f++) {
                double cost = 70.0 / clk, period = a.iv * 1000.0 / 60, real = cost > period ? cost : period;
                double u = cost / real, want = u >= 0.85 ? 1.0 : u <= 0.6 ? 0.5 : 0.5 + 0.5 * (u - 0.6) / 0.25;
                clk += (want - clk) * 0.1;
                int prev = a.iv;
                if (fps_auto_step(&a, 70.0, real, -1) != prev) changes++;
            }
            expect("70 ms work, clock following the load", a.iv, 4, changes, 0);
        }
    }

    // What the trials cost where the faster interval no longer holds (no clock effect): 30 s of 28 ms frames, then 45
    // ms for good. Each trial of 2 is 1 s the machine cannot hold; after 10 s at 3, then 20, 40 and 60: in 2 minutes
    // three of them, under 5 % of the frames at 2.
    fps_auto_reset(&a);
    changes = 0;
    feed(&a, 28.0, 1.0, 900, &changes);
    changes = 0;
    int at2b = 0, n45 = 0;
    for (int f = 0; f < 2400; f++) {   // 120 s of 45 ms frames (at 20 fps)
        int prev = a.iv;
        double m = 45.0 + (f & 1 ? 1.0 : -1.0);
        if (fps_auto_step(&a, m, real_of(m, prev), -1) != prev) changes++;
        n45++, at2b += a.iv == 2;
    }
    int pct2 = at2b * 100 / n45;
    printf("     fps: 45 ms frames after 30 fps held: interval 2 (trials) in %d %% of the frames\n", pct2);
    expect("45 ms frames after 30 fps held, 120 s", a.iv, 3, changes + (pct2 >= 5) * 99, 7);

    // Hitches and freezes at 60 with game time counted: one frame's part of the loss is capped, so a lone 80 ms frame
    // (63 ms lost) does not move it, and a 1 s freeze does not leave the game time lost past the threshold for the
    // runs of 18 ms frames the pacer catches up (it kept it there for 6 s: the median alone moved it)
    static const struct { const char *what; double hitch; } hitches[] = {
        {"14 ms with an 80 ms hitch every 5 s", 80.0}, {"a 1 s freeze, then runs of 18 ms frames", 1000.0}};
    for (unsigned h = 0; h < sizeof hitches / sizeof hitches[0]; h++) {
        fps_auto_reset(&a);
        changes = 0;
        late = 0;
        for (int f = 0; f < 1800; f++) {   // 30 s
            int prev = a.iv, once = hitches[h].hitch > 100;
            double ms = (once ? f == 300 : f % 300 == 299) ? hitches[h].hitch
                        : once && f > 300 && f % 14 < 8 ? 18.3
                                                         : 14.0;
            double real = paced(&late, ms, prev);
            if (fps_auto_step(&a, ms - 1.0, real, prev * 1000.0 / 60) != prev) changes++;
        }
        expect(hitches[h].what, a.iv, 1, changes, 0);
    }

    // Bursts the pacer catches up keep the frames fitting by game time, so a stall then gets its allowance: 300 ms
    // frames for 3 s went straight to 4 (the median past 60 without loss had cleared "fitted")
    fps_auto_reset(&a);
    changes = 0;
    late = 0;
    for (int f = 0; f < 2400; f++) {
        int prev = a.iv, stall = f >= 1000 && f < 1010;
        double ms = stall ? 300.0 : f % 14 < 8 ? 18.3 : 14.45, real = stall ? 300.0 : paced(&late, ms, prev);
        if (fps_auto_step(&a, ms - 1.0, real, prev * 1000.0 / 60) != prev) changes++;
    }
    expect("a 3 s stall among bursts the pacer absorbs", a.iv, 1, changes, 0);

    // Just past 30 fps: 35 ms frames run the game at 0.95x at 2, so 3. Its trials of 2 fit by the median (35 < 36) but
    // lose game time: they fail on it at once, and one undone by an up counts as failed (the wait doubles). Before,
    // each held, the next started over at 10 s: 10 changes a minute. Now the up and four trials of a few frames each
    // (after 10, 20, 40 and 60 s): 9 changes in 3 minutes.
    fps_auto_reset(&a);
    changes = 0;
    late = 0;
    double game35 = 0, real35 = 0;
    feed(&a, 28.0, 1.0, 600, &changes);   // 30 fps held first, so trials are allowed
    changes = 0;
    for (int f = 0; f < 3600; f++) {   // about 3 minutes
        int prev = a.iv;
        double ms = 35.0 + (f & 1 ? 1.0 : -1.0), real = paced(&late, ms, prev);
        if (fps_auto_step(&a, ms * 0.97, real, prev * 1000.0 / 60) != prev) changes++;
        if (f >= 600) game35 += prev * 1000.0 / 60, real35 += real;
    }
    printf("     fps: 35 ms frames: game time %.4fx from 20 s on\n", game35 / real35);
    expect("35 ms frames (0.95x at 30)", a.iv, 3, changes + (game35 / real35 < 0.995) * 99, 9);

    // Out of a race (game.c: the slowest interval 2) a load's 88 ms frames leave it at 2, and the race starts there
    fps_auto_reset(&a);
    changes = 0;
    fps_auto_cap(&a, 2);
    feed(&a, 88.0, 4.0, 60, &changes);
    fps_auto_cap(&a, 0);
    int start_iv = a.iv;
    iv = feed(&a, 28.0, 1.0, 300, &changes);
    expect("a load out of a race, then 28 ms frames", start_iv, 2, changes + (iv != 2) * 99, 1);

    // MKW_LOGIC_HZ=120: the interval counts 120 Hz ticks, up to 8 (15 fps); lhz120c topped out at 30 fps with 4
    static const struct { const char *what; double ms; int want; } at120[] = {
        {"120 Hz, 7 ms frames", 7.0, 1},   {"120 Hz, 15 ms frames", 15.0, 2}, {"120 Hz, 36 ms frames", 36.0, 5},
        {"120 Hz, 50 ms frames", 50.0, 6}, {"120 Hz, 120 ms frames", 120.0, 8},
    };
    for (unsigned i = 0; i < sizeof at120 / sizeof at120[0]; i++) {
        fps_auto_reset(&a);
        a.hz = 120;
        changes = 0;
        for (int f = 0; f < 1320; f++) {
            int prev = a.iv;
            double m = at120[i].ms + (f & 1 ? 0.5 : -0.5), held = prev * 1000.0 / 120;
            if (fps_auto_step(&a, m, m > held ? m : held, -1) != prev && f >= 120) changes++;
        }
        expect(at120[i].what, a.iv, at120[i].want, changes, 0);
    }

    printf("fps: %s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
