// Exercises KTPShotGeom::readAllTraces -- the cmd-keyed read that lets a shot
// which hit nobody report the trace count it already collected. Built and run by
// ci.yml twice: once with -DNEGATIVE_CONTROL, which must FAIL, and once without,
// which must pass.
//
// What is under test is the PAIRING KEY, and it needs no server: the defect it
// exists for is a read that routes through the target stash, which a shot with no
// target never stamps.
#include <stdio.h>
#include <string.h>
#include "KTPShotGeom.h"

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } } while (0)

static KTPShotGeom Fresh()
{
    KTPShotGeom sg;
    memset(&sg, 0x5a, sizeof(sg));  // garbage first, so reset() has to do the work
    sg.reset();
    return sg;
}

// What DODX_OnTraceLine's shooter-only branch does, ahead of the player-hit filter.
static void CountOneTrace(KTPShotGeom &sg)
{
    if (sg.cmdSeq == 0)
        return;
    if (sg.allTraceSeq != sg.cmdSeq)
    {
        sg.allTraceSeq = sg.cmdSeq;
        sg.allTraceCount = 0;
    }
    if (sg.allTraceCount < 999)
        sg.allTraceCount++;
}

int main()
{
    int count;

    // ---------------- the 0 == 0 trap ----------------
    {
        KTPShotGeom sg = Fresh();
        CHECK(sg.allTraceSeq == 0 && sg.allTraceCount == 0, "reset leaves no count");
        // cmdSeq is 0 before the first cmd and so is allTraceSeq. An equality-only
        // key would report the uninitialised counter as this cmd's.
        CHECK(sg.cmdSeq == 0, "reset leaves cmdSeq at 0 -- dead probe");
        CHECK(!sg.readAllTraces(count), "no cmd has run, so there is no count to read");
    }

    // ---------------- a shot that hit nobody ----------------
    {
        KTPShotGeom sg = Fresh();
        sg.cmdSeq = 7;
        CountOneTrace(sg);
        CountOneTrace(sg);
        // tgtSeq is what the player-hit capture stamps, and this shot had no target.
        CHECK(sg.tgtSeq == 0, "a hit-nobody shot stamps no target sequence");
        CHECK(sg.readAllTraces(count) && count == 2,
            "the count is readable with no target stamped -- the whole point");
    }

    // ---------------- non-destructive ----------------
    {
        KTPShotGeom sg = Fresh();
        sg.cmdSeq = 7;
        CountOneTrace(sg);
        CHECK(sg.readAllTraces(count) && count == 1, "first read");
        CHECK(sg.readAllTraces(count) && count == 1, "the read does not consume");
        // The counter is still live inside the cmd: a consuming read would make the
        // next trace restart the count instead of continuing it.
        CountOneTrace(sg);
        CHECK(sg.readAllTraces(count) && count == 2, "the count continues after a read");
    }

    // ---------------- not inherited across cmds ----------------
    {
        KTPShotGeom sg = Fresh();
        sg.cmdSeq = 7;
        CountOneTrace(sg);
        sg.cmdSeq = 8;  // a cmd in which the shooter owned no trace at all
        CHECK(!sg.readAllTraces(count), "a previous cmd's count is not this cmd's");
        // And the refusal does not consume, so the earlier cmd's own read still works.
        sg.cmdSeq = 7;
        CHECK(sg.readAllTraces(count) && count == 1, "the refusal left the count alone");
    }

    // ---------------- a target read does not destroy it ----------------
    {
        KTPShotGeom sg = Fresh();
        sg.cmdSeq = 7;
        CountOneTrace(sg);
        sg.tgtSeq = 7;
        sg.consumeTarget();  // what every dodx_get_shot_target* does on the way out
        CHECK(sg.tgtSeq == 0, "consumeTarget clears the target stash -- dead probe");
        CHECK(sg.readAllTraces(count) && count == 1,
            "consuming the target must not take the cmd's trace count with it");
    }
    {
        // resetTarget is the full clear, and it does take the count -- so a new
        // target stash cannot carry a stale cmd's traces.
        KTPShotGeom sg = Fresh();
        sg.cmdSeq = 7;
        CountOneTrace(sg);
        sg.resetTarget();
        CHECK(!sg.readAllTraces(count), "resetTarget clears the count");
    }

    // ---------------- zero is a real count, with a sentinel for absent ----------------
    {
        // The sentinel is what target[11] cannot express: it reports 0 both for
        // "no count for this cmd" and for a count that is genuinely 0.
        KTPShotGeom sg = Fresh();
        sg.cmdSeq = 7;
        sg.allTraceSeq = 7;
        sg.allTraceCount = 0;
        CHECK(sg.readAllTraces(count) && count == 0,
            "a stamped count of 0 is a measurement, not an absence");
    }

    // ---------------- the bound the wire needs ----------------
    {
        KTPShotGeom sg = Fresh();
        sg.cmdSeq = 7;
        for (int i = 0; i < 1100; i++)
            CountOneTrace(sg);
        CHECK(sg.readAllTraces(count) && count == 999, "the SMALLINT bound holds");
    }

#ifdef NEGATIVE_CONTROL
    // Proves this harness can fail.
    {
        KTPShotGeom sg = Fresh();
        CHECK(sg.readAllTraces(count), "deliberate failure");
    }
#endif

    printf(fails ? "FAILED %d\n" : "ALL PASS\n", fails);
    return fails ? 1 : 0;
}
