// Exercises KTPShotGeom::stampMiss / readMiss -- the cmd-keyed ray stash that lets a
// shot which resolved against the world report where it went. Built and run by
// ci.yml twice: once with -DNEGATIVE_CONTROL, which must FAIL, and once without,
// which must pass.
//
// It needs no server. What is under test is the stash contract: first-wins inside a
// cmd, destructive and cmd-paired on the way out, independent of the target stash in
// both directions, and a fraction clamped to a bound the wire can prove. The defect
// it exists for is the geometry capture sitting inside the player-hit filter, so a
// trace that hit nobody left nothing behind at all.
#include <math.h>
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

// What DODX_OnTraceLine's shooter-only branch does, including the outer seq compare
// it keeps so an already-stamped cmd does not deref the hit edict.
static void TraceToWorld(KTPShotGeom &sg, float sx, float ex, float frac)
{
    const float start[3]  = { sx, 0.0f, 64.0f };
    const float end[3]    = { ex, 0.0f, 64.0f };
    const float impact[3] = { sx + (ex - sx) * frac, 0.0f, 64.0f };
    if (sg.missSeq != sg.cmdSeq)
        sg.stampMiss(start, end, impact, frac, false, false, 0, false, 7);
}

int main()
{
    int ints[4];
    float start[3], end[3], impact[3];

    // ---------------- the 0 == 0 trap ----------------
    {
        KTPShotGeom sg = Fresh();
        CHECK(sg.missSeq == 0, "reset leaves no ray");
        CHECK(sg.cmdSeq == 0, "reset leaves cmdSeq at 0 -- dead probe");
        // cmdSeq is 0 before the first cmd and so is missSeq. An equality-only key
        // would report the uninitialised stash as this cmd's.
        TraceToWorld(sg, 0.0f, 100.0f, 0.5f);
        CHECK(sg.missSeq == 0, "a trace before the first cmd stamps nothing");
        CHECK(!sg.readMiss(ints, start, end, impact),
            "no cmd has run, so there is no ray to read");
    }

    // ---------------- a shot that hit nobody ----------------
    {
        KTPShotGeom sg = Fresh();
        sg.cmdSeq = 7;
        TraceToWorld(sg, 10.0f, 110.0f, 0.25f);
        // tgtSeq is what the player-hit capture stamps, and this shot had no target.
        CHECK(sg.tgtSeq == 0, "a hit-nobody shot stamps no target sequence");
        CHECK(sg.geomSeq == 0, "and no geometry either -- dead probe");
        CHECK(sg.readMiss(ints, start, end, impact),
            "the ray is readable with no target stamped -- the whole point");
        CHECK(start[0] == 10.0f && end[0] == 110.0f,
            "the ray is the one the trace was given");
        CHECK(impact[0] == 35.0f, "and the impact point is where it stopped");
        CHECK(ints[0] == 2500, "fraction is flFraction x10000");
        CHECK(ints[1] == 0, "a plain world trace sets no flag");
        CHECK(ints[2] == 0, "worldspawn is entity 0, which is a measurement");
    }

    // ---------------- the weapon rides along as data ----------------
    {
        // Two distinct values, because one would pass against a hardcode.
        const float v[3] = { 0, 0, 0 };
        KTPShotGeom a = Fresh(), b = Fresh();
        a.cmdSeq = b.cmdSeq = 7;
        a.stampMiss(v, v, v, 0.5f, false, false, 0, false, 14);
        b.stampMiss(v, v, v, 0.5f, false, false, 0, false, 23);
        int ia[4], ib[4];
        CHECK(a.readMiss(ia, start, end, impact) && ia[3] == 14, "weapon 14 is stored");
        CHECK(b.readMiss(ib, start, end, impact) && ib[3] == 23, "weapon 23 is stored");
    }

    // ---------------- the aimed end is not the impact point ----------------
    {
        // The distinction the counterfactual needs: where the shooter pointed, and
        // where the world stopped him. Folding them would lose the ray's direction
        // on every trace that hit something.
        KTPShotGeom sg = Fresh();
        sg.cmdSeq = 7;
        TraceToWorld(sg, 0.0f, 1000.0f, 0.1f);
        CHECK(sg.readMiss(ints, start, end, impact), "stamped");
        CHECK(end[0] == 1000.0f && impact[0] == 100.0f,
            "aimed end and impact are separate fields");
    }

    // ---------------- first-wins inside the cmd ----------------
    {
        KTPShotGeom sg = Fresh();
        sg.cmdSeq = 7;
        TraceToWorld(sg, 1.0f, 101.0f, 1.0f);
        TraceToWorld(sg, 2.0f, 202.0f, 0.5f);  // a later trace in the same cmd
        CHECK(sg.readMiss(ints, start, end, impact), "stamped");
        CHECK(start[0] == 1.0f, "the first trace of the cmd wins");
    }
    {
        // Through stampMiss DIRECTLY, with no outer compare in front of it. The
        // capture site keeps one as a cost optimisation, and a test that goes only
        // through a helper carrying it would supply the precondition it is meant to
        // be checking -- first-wins has to hold inside the function.
        KTPShotGeom sg = Fresh();
        sg.cmdSeq = 7;
        const float a1[3] = { 1, 0, 0 }, a2[3] = { 2, 0, 0 };
        const float b1[3] = { 90, 0, 0 }, b2[3] = { 91, 0, 0 };
        sg.stampMiss(a1, a2, a2, 0.5f, false, false, 0, false, 11);
        sg.stampMiss(b1, b2, b2, 0.5f, false, false, 0, false, 12);
        CHECK(sg.readMiss(ints, start, end, impact), "stamped");
        CHECK(start[0] == 1.0f && ints[3] == 11,
            "stampMiss itself refuses to replace the cmd's first sample");
    }

    // ---------------- destructive ----------------
    {
        KTPShotGeom sg = Fresh();
        sg.cmdSeq = 7;
        TraceToWorld(sg, 5.0f, 105.0f, 0.5f);
        CHECK(sg.readMiss(ints, start, end, impact), "first read");
        CHECK(!sg.readMiss(ints, start, end, impact),
            "a stash reports at most one shot");
    }

    // ---------------- not inherited across cmds ----------------
    {
        KTPShotGeom sg = Fresh();
        sg.cmdSeq = 7;
        TraceToWorld(sg, 5.0f, 105.0f, 0.5f);
        sg.cmdSeq = 8;  // a cmd in which the shooter owned no trace at all
        CHECK(!sg.readMiss(ints, start, end, impact),
            "a previous cmd's ray is not this cmd's");
        // And the refusal consumes, like readRewind: cmdSeq only grows, so a stash
        // that failed to pair can never pair later.
        sg.cmdSeq = 7;
        CHECK(!sg.readMiss(ints, start, end, impact),
            "the refusal took the stale ray with it");
    }

    // ---------------- independent of the target stash, both ways ----------------
    {
        // A target read must not take the ray: the ray is the record of a shot with
        // no target, so nothing on the target path may own its lifetime.
        KTPShotGeom sg = Fresh();
        sg.cmdSeq = 7;
        TraceToWorld(sg, 5.0f, 105.0f, 0.5f);
        sg.tgtSeq = 7;
        sg.consumeTarget();
        CHECK(sg.tgtSeq == 0, "consumeTarget clears the target stash -- dead probe");
        CHECK(sg.readMiss(ints, start, end, impact),
            "consuming the target must not take the cmd's ray");
    }
    {
        KTPShotGeom sg = Fresh();
        sg.cmdSeq = 7;
        TraceToWorld(sg, 5.0f, 105.0f, 0.5f);
        sg.resetTarget();
        CHECK(sg.readMiss(ints, start, end, impact),
            "resetTarget does not clear the ray either -- it is target-independent");
    }
    {
        // reset() is the full clear and does take it.
        KTPShotGeom sg = Fresh();
        sg.cmdSeq = 7;
        TraceToWorld(sg, 5.0f, 105.0f, 0.5f);
        sg.reset();
        sg.cmdSeq = 7;
        CHECK(!sg.readMiss(ints, start, end, impact), "reset clears the ray");
    }

    // ---------------- the ray does not disturb the hit stash ----------------
    {
        // missFrac and traceFrac carry the same unit and different samples. If the
        // stamp touched the hit stash's fields, one measurement would silently
        // overwrite the other in a column that already has consumers.
        KTPShotGeom sg = Fresh();
        sg.cmdSeq = 7;
        sg.traceFrac = 9999;
        sg.traceFlags = 0xf;
        TraceToWorld(sg, 0.0f, 100.0f, 0.5f);
        CHECK(sg.traceFrac == 9999, "the hit stash's fraction is untouched");
        CHECK(sg.traceFlags == 0xf, "and so are its flags");
        CHECK(sg.readMiss(ints, start, end, impact) && ints[0] == 5000,
            "while the ray reports its own, different fraction");
    }

    // ---------------- the fraction's bound is enforced, not hoped for ----------------
    {
        const float startv[3] = { 0, 0, 0 }, endv[3] = { 1, 0, 0 };
        {
            KTPShotGeom sg = Fresh();
            sg.cmdSeq = 7;
            sg.stampMiss(startv, endv, endv, 1.0f, false, false, 0, false, 1);
            CHECK(sg.readMiss(ints, start, end, impact) && ints[0] == 10000,
                "a full-length trace is 10000");
            CHECK((ints[1] & 0x4) != 0, "and sets the ran-full-length bit");
        }
        {
            KTPShotGeom sg = Fresh();
            sg.cmdSeq = 7;
            sg.stampMiss(startv, endv, endv, 4.0f, false, false, 0, false, 1);
            CHECK(sg.readMiss(ints, start, end, impact) && ints[0] == 10000,
                "an over-range fraction is clamped, not stored");
        }
        {
            KTPShotGeom sg = Fresh();
            sg.cmdSeq = 7;
            sg.stampMiss(startv, endv, startv, -1.0f, false, false, 0, false, 1);
            CHECK(sg.readMiss(ints, start, end, impact) && ints[0] == 0,
                "a negative fraction floors at 0");
        }
        {
            // NaN would become a meaningless int and read as a measurement.
            KTPShotGeom sg = Fresh();
            sg.cmdSeq = 7;
            const float nan = sqrtf(-1.0f);
            sg.stampMiss(startv, endv, startv, nan, false, false, 0, false, 1);
            CHECK(sg.readMiss(ints, start, end, impact) && ints[0] == 0,
                "NaN floors at 0 rather than becoming a number");
        }
    }

    // ---------------- each flag bit means one thing ----------------
    {
        const float v[3] = { 0, 0, 0 };
        {
            KTPShotGeom sg = Fresh();
            sg.cmdSeq = 7;
            sg.stampMiss(v, v, v, 0.0f, true, false, 0, false, 1);
            CHECK(sg.readMiss(ints, start, end, impact) && ints[1] == 0x1,
                "fStartSolid alone is bit0");
        }
        {
            KTPShotGeom sg = Fresh();
            sg.cmdSeq = 7;
            sg.stampMiss(v, v, v, 0.0f, false, true, 0, false, 1);
            CHECK(sg.readMiss(ints, start, end, impact) && ints[1] == 0x2,
                "fAllSolid alone is bit1");
        }
        {
            // The cmd's first trace CAN be a player hit -- this branch runs ahead of
            // the player-hit filter. Bit3 is how a consumer tells that apart from a
            // miss instead of inferring it from the entity index's slot range.
            KTPShotGeom sg = Fresh();
            sg.cmdSeq = 7;
            sg.stampMiss(v, v, v, 0.5f, false, false, 3, true, 1);
            CHECK(sg.readMiss(ints, start, end, impact), "stamped");
            CHECK(ints[1] == 0x8, "a player hit is bit3 and nothing else");
            CHECK(ints[2] == 3, "and the entity index is the player's");
        }
        {
            KTPShotGeom sg = Fresh();
            sg.cmdSeq = 7;
            sg.stampMiss(v, v, v, 0.5f, false, false, -1, false, 1);
            CHECK(sg.readMiss(ints, start, end, impact) && ints[2] == -1,
                "no hit edict at all is -1, not worldspawn's 0");
        }
    }

#ifdef NEGATIVE_CONTROL
    // Proves this harness can fail.
    {
        KTPShotGeom sg = Fresh();
        CHECK(sg.readMiss(ints, start, end, impact), "deliberate failure");
    }
#endif

    printf(fails ? "FAILED %d\n" : "ALL PASS\n", fails);
    return fails ? 1 : 0;
}
