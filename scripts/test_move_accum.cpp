// Exercises KTPMoveAccum.h, including the shipped Observe() path that the
// per-usercmd sampler calls. Built and run by ci.yml, twice: once with
// -DNEGATIVE_CONTROL, which must FAIL, and once without, which must pass. A gate
// that only ever passes proves nothing -- same convention as the --selftest mode
// on the Python checks beside this file.
//
// Observe() takes plain values rather than an edict precisely so this file can
// exist: logic that needs a live server can only be verified by deploying it.
#include <stdio.h>
#include <string.h>
#include "KTPMoveAccum.h"

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } } while (0)

static int BucketOfSpeed(float s) { return KTPMoveStats::Bucket(s * s); }
static float Sq(float s) { return s * s; }

static int SumRow(const int *row)
{
    int t = 0;
    for (int i = 0; i < KTPMove::SPEED_BUCKETS; ++i) t += row[i];
    return t;
}

int main()
{
    // ---------------- bucket geometry ----------------
    CHECK(BucketOfSpeed(0.0f)   == 0, "0 -> bucket 0");
    CHECK(BucketOfSpeed(49.9f)  == 0, "49.9 -> bucket 0");
    CHECK(BucketOfSpeed(50.1f)  == 1, "50.1 -> bucket 1");
    CHECK(BucketOfSpeed(100.1f) == 2, "100.1 -> bucket 2");
    CHECK(BucketOfSpeed(249.9f) == 4, "249.9 -> bucket 4");
    CHECK(BucketOfSpeed(250.1f) == 5, "250.1 -> bucket 5");
    // The fleet's stored shot rows reach 582 u/s; a closed top bucket would drop them.
    CHECK(BucketOfSpeed(582.0f) == 5, "582 -> top bucket, not out of range");
    CHECK(BucketOfSpeed(1e6f)   == KTPMove::SPEED_BUCKETS - 1, "absurd speed stays in range");
    for (float s = 0.0f; s < 2000.0f; s += 0.37f)
    {
        int b = BucketOfSpeed(s);
        CHECK(b >= 0 && b < KTPMove::SPEED_BUCKETS, "bucket always indexes inside the arrays");
    }

    // ---------------- wire clamp ----------------
    // The producer's whole line-length budget rests on this bound.
    CHECK(KTPMoveStats::Wire(-5) == 0, "negative clamps to 0");
    CHECK(KTPMoveStats::Wire(12345) == 12345, "ordinary value passes through");
    CHECK(KTPMoveStats::Wire(KTPMove::WIRE_MAX + 1) == KTPMove::WIRE_MAX, "clamped at the ceiling");
    char buf[64];
    snprintf(buf, sizeof(buf), "%d", KTPMoveStats::Wire(2147483647));
    CHECK(strlen(buf) <= 6, "a wired value never renders wider than six digits");

    // ---------------- reset semantics ----------------
    KTPMoveStats st;
    st.Reset();
    CHECK(st.stamAtTapMin == -1, "no tap observed reads -1, not 0");
    CHECK(!st.havePrev, "Reset forgets the sample state");

    // ---------------- the first sample fabricates nothing ----------------
    // This is the one that bit the aim sampler's ground tracking: adopting a
    // baseline we never saw invents an edge and a step at every respawn.
    st.Reset();
    st.Observe(10.000, true, false, Sq(200.0f), /*duckEdge*/true, 80, 400);
    CHECK(st.tapsTotal == 0, "first sample after a reset counts no tap even on an edge");
    CHECK(st.stepTimerFires == 0, "first sample counts no step");
    CHECK(SumRow(st.groundMsStanding) == 0, "first sample accumulates no time");
    CHECK(st.havePrev, "first sample establishes the baseline");

    // ---------------- an ordinary run on the ground ----------------
    st.Reset();
    st.Observe(10.000, true, false, Sq(200.0f), false, 90, 400);   // baseline
    st.Observe(10.010, true, false, Sq(200.0f), false, 90, 390);   // 10ms standing @200
    st.Observe(10.020, true, false, Sq(200.0f), false, 90, 380);
    CHECK(st.groundMsStanding[4] == 20, "standing ground time lands in the 200-250 bucket");
    CHECK(SumRow(st.groundMsDucked) == 0, "no ducked time was recorded");
    CHECK(SumRow(st.airMsDucked) == 0, "no airborne time was recorded");

    // ---------------- crouched on the ground ----------------
    st.Reset();
    st.Observe(20.000, true, true, Sq(60.0f), false, 95, 400);
    st.Observe(20.015, true, true, Sq(60.0f), false, 95, 400);
    CHECK(st.groundMsDucked[1] == 15, "ducked ground time lands in the 50-100 bucket");
    CHECK(SumRow(st.groundMsStanding) == 0, "ducked time is not double-counted as standing");

    // ---------------- a duck-jump is not ground time ----------------
    // Measured on real shot rows: ducking+airborne averages ~276 u/s and
    // ducking+on-ground ~6. Folding them would make the ground row unreadable.
    st.Reset();
    st.Observe(30.000, false, true, Sq(280.0f), false, 70, 400);
    st.Observe(30.012, false, true, Sq(280.0f), false, 70, 400);
    CHECK(st.airMsDucked[5] == 12, "airborne ducked time lands in the top bucket");
    CHECK(SumRow(st.groundMsDucked) == 0, "a duck-jump contributes no GROUND ducked time");
    CHECK(SumRow(st.groundMsStanding) == 0, "a duck-jump contributes no standing time");

    // ---------------- taps split by ground contact ----------------
    st.Reset();
    st.Observe(40.000, true, false, Sq(10.0f), false, 100, 400);   // baseline
    st.Observe(40.010, true, false, Sq(10.0f), true,  100, 400);   // tap, on ground, slow
    st.Observe(40.020, false, false, Sq(300.0f), true, 62, 400);   // tap, airborne, fast
    CHECK(st.tapsTotal == 2, "both taps counted");
    CHECK(st.tapsGround[0] == 1, "ground tap in the slowest bucket");
    CHECK(st.tapsAir[5] == 1, "airborne tap in the top bucket");
    CHECK(st.stamAtTapSum == 162, "stamina summed over taps");
    CHECK(st.stamAtTapMin == 62, "minimum stamina at a tap tracked");

    // A stamina of 0 at a tap must be recorded as 0, not mistaken for "no tap".
    st.Reset();
    st.Observe(41.000, true, false, Sq(10.0f), false, 50, 400);
    st.Observe(41.010, true, false, Sq(10.0f), true, 0, 400);
    CHECK(st.tapsTotal == 1 && st.stamAtTapMin == 0, "a tap at stamina 0 records 0, not -1");

    // ---------------- the step timer is read as a transition ----------------
    st.Reset();
    st.Observe(50.000, true, false, Sq(200.0f), false, 90, 300);
    st.Observe(50.010, true, false, Sq(200.0f), false, 90, 250);   // decaying: no fire
    st.Observe(50.020, true, false, Sq(200.0f), false, 90, 200);
    CHECK(st.stepTimerFires == 0, "a decaying step timer is not a step");
    st.Observe(50.030, true, false, Sq(200.0f), false, 90, 400);   // reset: one fire
    CHECK(st.stepTimerFires == 1, "a rising step timer is one step");
    st.Observe(50.040, true, false, Sq(200.0f), false, 90, 350);
    CHECK(st.stepTimerFires == 1, "the decay after a reset is not a second step");

    // ---------------- a pause or packet gap accumulates no time ----------------
    // The engine zeroes msec during a .tech pause rather than skipping commands,
    // so this bound is what stops a pause being charged as movement.
    st.Reset();
    st.Observe(60.000, true, false, Sq(200.0f), false, 90, 400);
    st.Observe(65.000, true, false, Sq(200.0f), false, 90, 400);   // 5s gap
    CHECK(SumRow(st.groundMsStanding) == 0, "a gap past the bound accumulates no time");
    st.Observe(65.010, true, false, Sq(200.0f), false, 90, 400);
    CHECK(st.groundMsStanding[4] == 10, "sampling resumes normally after the gap");

    // A backwards clock (svtimebase is re-anchored per packet) must not subtract.
    st.Reset();
    st.Observe(70.000, true, false, Sq(200.0f), false, 90, 400);
    st.Observe(69.900, true, false, Sq(200.0f), false, 90, 400);
    CHECK(SumRow(st.groundMsStanding) == 0, "a backwards time delta accumulates nothing");

    // ---------------- ForgetSampleState is what a respawn needs ----------------
    st.Reset();
    st.Observe(80.000, true, false, Sq(200.0f), false, 90, 400);
    st.ForgetSampleState();
    st.Observe(80.010, true, false, Sq(200.0f), true, 90, 999);
    CHECK(st.tapsTotal == 0 && st.stepTimerFires == 0 && SumRow(st.groundMsStanding) == 0,
          "after ForgetSampleState the next sample counts nothing");

    // ---------------- ResetCounters keeps the baseline ----------------
    st.Reset();
    st.Observe(90.000, true, false, Sq(200.0f), false, 90, 400);
    st.Observe(90.010, true, false, Sq(200.0f), true, 90, 400);
    CHECK(st.tapsTotal == 1, "tap before the flush");
    st.ResetCounters();
    CHECK(st.tapsTotal == 0 && st.stamAtTapMin == -1, "flush cleared the counters");
    st.Observe(90.020, true, false, Sq(200.0f), true, 88, 500);
    CHECK(st.tapsTotal == 1, "the first tap AFTER a flush is still counted");
    CHECK(st.stepTimerFires == 1, "the first step after a flush is still counted");
    CHECK(st.groundMsStanding[4] == 10, "time across the flush boundary is still measured");

#ifdef NEGATIVE_CONTROL
    // Proves this harness can fail: without it, everything above passing would be
    // equally consistent with a binary that never ran an assertion.
    CHECK(BucketOfSpeed(0.0f) == 99, "deliberate failure");
#endif

    printf(fails ? "FAILED %d\n" : "ALL PASS\n", fails);
    return fails ? 1 : 0;
}
