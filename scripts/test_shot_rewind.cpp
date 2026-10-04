// Exercises KTPShotGeom's schema-26 additions: the per-cmd rewind stash and the
// hitgroup clamp. Built and run by ci.yml twice: once with -DNEGATIVE_CONTROL,
// which must FAIL, and once without, which must pass.
//
// The stash takes plain values rather than the engine record so this file can
// exist: what it guards is the pairing, and the pairing needs no server.
#include <stdio.h>
#include <string.h>
#include <math.h>
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

int main()
{
    int out[3];

    // ---------------- pairing ----------------
    {
        KTPShotGeom sg = Fresh();
        CHECK(sg.rwSeq == 0, "reset leaves no record");
        CHECK(sg.tgtHitgroup == -1, "reset leaves hitgroup at the sentinel");
        CHECK(!sg.readRewind(out), "a fresh stash reads nothing");

        sg.stampRewind(true, 0x7u, 300.0f, 412.0f);
        CHECK(sg.rwSeq == 0, "cmdSeq 0 (no cmd has run) stamps nothing");

        sg.cmdSeq = 5;
        sg.stampRewind(true, 0x7u, 300.0f, 412.0f);
        CHECK(sg.readRewind(out), "the fire cmd reads its own record");
        CHECK(out[0] == 7 && out[1] == 300 && out[2] == 412, "values as stamped");
        CHECK(!sg.readRewind(out), "the read is destructive");
    }
    {
        // Read one cmd late: the next packet's rewind must not be attributed.
        KTPShotGeom sg = Fresh();
        sg.cmdSeq = 5;
        sg.stampRewind(true, 0x3u, 120.0f, 120.0f);
        sg.cmdSeq = 6;
        CHECK(!sg.readRewind(out), "a stale cmd's record is refused");
        sg.cmdSeq = 5;
        CHECK(!sg.readRewind(out), "and consumed by the refusal");
    }
    {
        // A cmd with no record (bot, old engine) must not inherit the previous cmd's.
        KTPShotGeom sg = Fresh();
        sg.cmdSeq = 5;
        sg.stampRewind(true, 0x3u, 120.0f, 120.0f);
        sg.cmdSeq = 6;
        sg.stampRewind(false, 0x3u, 120.0f, 120.0f);
        CHECK(!sg.readRewind(out), "no record for this cmd reads as none");
    }
    {
        // A not-attempted packet is still a record: flags 0, depth/want 0.
        KTPShotGeom sg = Fresh();
        sg.cmdSeq = 9;
        sg.stampRewind(true, 0u, 0.0f, 0.0f);
        CHECK(sg.readRewind(out) && out[0] == 0 && out[1] == 0 && out[2] == 0,
            "flags 0 is a real record, not an absent one");
    }

    // ---------------- clamps ----------------
    CHECK(KTPShotGeom::clampMs(-3.0f) == 0, "negative depth clamps to 0");
    CHECK(KTPShotGeom::clampMs(NAN) == 0, "NaN clamps to 0, not to garbage");
    CHECK(KTPShotGeom::clampMs(299.6f) == 300, "rounded to the nearest ms");
    CHECK(KTPShotGeom::clampMs(1600.0f) == 1600, "the real maximum passes");
    CHECK(KTPShotGeom::clampMs(12000.0f) == 9999, "the wire bound holds");
    CHECK(KTPShotGeom::clampMs(INFINITY) == 9999, "infinity holds the bound");
    {
        KTPShotGeom sg = Fresh();
        sg.cmdSeq = 3;
        sg.stampRewind(true, 0xffu, 0.0f, 0.0f);
        CHECK(sg.readRewind(out) && out[0] == 0x7f, "flags masked to seven bits");
    }
    CHECK(KTPShotGeom::clampHitgroup(0) == 0, "hitgroup 0 (generic) is real");
    CHECK(KTPShotGeom::clampHitgroup(7) == 7, "a limb hitgroup passes");
    CHECK(KTPShotGeom::clampHitgroup(99) == 99, "the wire bound passes");
    CHECK(KTPShotGeom::clampHitgroup(100) == -1, "outside 0-99 is the sentinel");
    CHECK(KTPShotGeom::clampHitgroup(-2) == -1, "negative is the sentinel");
    {
        KTPShotGeom sg = Fresh();
        sg.tgtHitgroup = 3;
        sg.resetTarget();
        CHECK(sg.tgtHitgroup == -1, "resetTarget clears the hitgroup");
    }

#ifdef NEGATIVE_CONTROL
    // Proves this harness can fail.
    CHECK(KTPShotGeom::clampHitgroup(0) == 99, "deliberate failure");
#endif

    printf(fails ? "FAILED %d\n" : "ALL PASS\n", fails);
    return fails ? 1 : 0;
}
