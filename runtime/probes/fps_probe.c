// --probe-fps: MKW_FPS=auto's controller (core/boot/fps_auto.c) on synthetic frame times. Each case feeds frames of
// a fixed length and checks where the interval settles and how often it changed: a steady frame must give one
// interval and no back and forth (the game's own rule taken to 4 would cycle 2, 3, 4 on 36 ms frames). Needs no game.
#include <stdio.h>

#include "host.h"

static int fails;

// The real period of a frame of ms at interval iv: the pacer holds it to the interval's period.
static double real_of(double ms, int iv) { return ms > iv * 1000.0 / 60 ? ms : iv * 1000.0 / 60; }

// frames of ms each (plus jitter of +-j alternating); returns the interval after them, counting the changes. real:
// the real frame time when the game's measure (ms) falls short of it, else 0 (the measure is the frame time)
static int feed_real(FpsAuto *a, double ms, double j, double real, int frames, int *changes) {
    for (int i = 0; i < frames; i++) {
        int prev = a->iv;
        double m = ms + (i & 1 ? j : -j);
        if (fps_auto_step(a, m, real_of(real > m ? real : m, prev)) != prev) (*changes)++;
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

    fps_auto_reset(&a);   // lat4: race frames spread over 25-40 ms, around 30 fps: one interval, no back and forth
    changes = 0;
    static const double spread[] = {25, 31, 38, 27, 40, 29, 33, 26, 36, 30, 28, 39, 32, 34, 27, 31};
    for (int i = 0; i < 300; i++) {
        int prev = a.iv;
        if (fps_auto_step(&a, spread[i % 16], real_of(spread[i % 16], prev)) != prev) changes++;
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
            if (fps_auto_step(&a, 300.0, 300.0) != prev) changes++;
        }
        for (int i = 0; i < 240; i++) {
            int prev = a.iv;
            double m = 38.0 + (i & 1 ? 2.0 : -2.0);
            if (fps_auto_step(&a, m, real_of(45.0, prev)) != prev) changes++;
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
            if (fps_auto_step(&a, m, m > held ? m : held) != prev && f >= 120) changes++;
        }
        expect(at120[i].what, a.iv, at120[i].want, changes, 0);
    }

    // dk20 (Steam Deck, race at interval 3): the game's measure follows the interval, at 20 fps the CPU and GPU idle
    // half the frame at lower clocks and frames measure ~35 ms, at 30 fps they measure ~27 ms and hold. The down rule
    // (28.3 ms) never left 3; the probe goes to 2 and stays.
    fps_auto_reset(&a);
    changes = 0;
    for (int i = 0; i < 1800; i++) {   // 30 s, from interval 3
        if (i == 0) a.iv = 3;
        int prev = a.iv;
        double m = (prev >= 3 ? 35.0 : 27.0) + (i & 1 ? 1.5 : -1.5);
        if (fps_auto_step(&a, m, real_of(m, prev)) != prev) changes++;
    }
    expect("Steam Deck: 35 ms at 20 fps, 27 ms at 30 fps", a.iv, 2, changes, 1);

    // fan_auto (Steam Deck GP, working fan): at 30 fps the game measures 31-41 ms, the real period holds 33.3 ms with a
    // 50 ms frame now and then (fixed 30 ran at 0.99x). From 20 fps the probe goes to 30 and it stays there.
    fps_auto_reset(&a);
    changes = 0;
    static const double deck30[] = {33, 36, 31, 38, 35, 41, 34, 37, 32, 39, 35, 36};
    for (int i = 0; i < 3600; i++) {   // 60 s, from interval 3
        if (i == 0) a.iv = 3;
        int prev = a.iv;
        double m = prev >= 3 ? 35.0 + (i & 1 ? 1.5 : -1.5) : deck30[i % 12];
        double r = prev >= 3 ? 50.0 : i % 15 ? 1000.0 / 30 : 50.0;
        if (fps_auto_step(&a, m, r) != prev) changes++;
    }
    expect("Steam Deck: 31-41 ms measured at 30 fps", a.iv, 2, changes, 1);

    // Radeon 660M (#597, adriadam10): a steady ~35 ms frame, whose real period at 30 fps is ~36 ms. A probe to 30 is
    // undone and judged on the real period (past 1.05 x 33.3 ms): no further probe in this race, at most one in 10
    // minutes; the next race scene (fps_auto_race_start) may try once more.
    fps_auto_reset(&a);
    changes = 0;
    for (int i = 0; i < 12000; i++) {   // 10 minutes at 20 fps, from interval 3
        if (i == 0) a.iv = 3;
        int prev = a.iv;
        double m = 35.0 + (i & 1 ? 0.8 : -0.8);
        double r = prev >= 3 ? 50.0 : 36.0 + (i & 1 ? 0.5 : -0.5);
        if (fps_auto_step(&a, m, r) != prev) changes++;
    }
    expect("660M: 35 ms, 36 ms real at 30 (probes)", a.iv, 3, (int)a.probes, 1);
    unsigned before = a.probes;
    fps_auto_race_start(&a);
    for (int i = 0; i < 1200; i++) {   // a new race: one more probe at most
        int prev = a.iv;
        double m = 35.0 + (i & 1 ? 0.8 : -0.8), r = prev >= 3 ? 50.0 : 36.0;
        fps_auto_step(&a, m, r);
    }
    expect("660M: next race scene (probes)", a.iv, 3, (int)(a.probes - before), 1);

    printf("fps: %s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
