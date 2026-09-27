// KTP: per-usercmd crouch-input and footstep-emission counters.
//
// SENSOR, NOT DETECTOR. It counts crouch button edges, time spent in movement
// states, and footstep emissions. It applies no threshold, computes no ratio and
// reaches no conclusion -- this repo is public, so a bound written here is a bound a
// reader can sit outside. Judgement lives in the private consumer.
//
// NOTHING IS REDUCED TO A RATIO HERE. Emitted footsteps and the time that could have
// produced them ship as separate quantities, and the time ships as a HISTOGRAM over
// speed rather than as "time above a threshold". A speed cut-point is the whole
// judgement in this question, so choosing one here would publish it.
//
// WHY THE BUTTON EDGE IS TRUSTWORTHY AT PreThink. Inside one SV_RunCmd the engine
// assigns v.button from this command's buttons BEFORE calling SV_PlayerRunPreThink,
// and writes v.oldbuttons from pmove's buttons only AFTER the pmove phase. So at our
// sample point v.button is this command and v.oldbuttons is the previous one, and
// (button & ~oldbuttons) is a true rising edge with no state of our own to keep.
// That ordering is the entire basis for this file; re-check it before trusting an
// edge count if SV_RunCmd is ever reordered.
//
// WHAT THE SAMPLE POINT MEANS FOR THE OTHER FIELDS, stated rather than implied:
// velocity, flags and fuser4 are all written back from pmove at the END of a command,
// so at PreThink of command N they describe the state command N acts ON, not the state
// it produces. A tap's recorded stamina is the stamina the tap was applied to.
//
// WHY MILLISECONDS AND NOT COMMAND COUNTS. A count of usercmds is a function of the
// client's own cl_cmdrate, so a legal cvar change would move every number here. Time
// is the same for everyone. Same reasoning as the ground-contact counters next door.
#ifndef KTP_MOVE_ACCUM_H
#define KTP_MOVE_ACCUM_H

namespace KTPMove
{
	// Horizontal-speed histogram. RESOLUTION, not a threshold: every bucket ships, so
	// a consumer picks its own cut-point and this file states none. The top bucket is
	// open because DoD produces speeds above any on-foot maximum (falls, boosts), and
	// a closed top would discard them instead of bucketing them.
	const int SPEED_BUCKETS = 6;
	// Integer on purpose: this value crosses into Pawn and is STORED beside every
	// row so the histogram keeps its meaning. A float would narrow silently on the
	// way out, and a stored 12 for a real 12.5 misdescribes every row it labels.
	const int BUCKET_WIDTH_UNITS = 50;   // world units/sec

	// How long sampling may lapse and still be treated as continuous time. STRUCTURAL:
	// it has to clear any legal frame time, and KTPCvarChecker permits fps_max down to
	// 60, a 16.7ms frame. Without it a .tech pause or a packet gap would dump its whole
	// span into whichever bucket the player was last seen in.
	//
	// It is also the only bound that holds during a pause, where msec is zeroed rather
	// than skipped and the clock does not advance -- the elapsed-time test simply never
	// accumulates, which is the correct answer there.
	const double SAMPLE_GAP_MS = 150.0;

	// Ceiling applied when a counter crosses into Pawn. The producer's line is rendered
	// as fixed-width fields, and an unbounded counter would overflow that and malform
	// the batch silently. Reached only if a flush is missed entirely, which the consumer
	// prevents by resetting unconditionally -- this is the backstop for when it doesn't.
	const int    WIRE_MAX = 999999;
}

// Crouch input and step emission for one player, accumulated between flushes.
//
// Two ledgers, deliberately in ONE struct with ONE reader: a footstep count read
// without the crouch census beside it invites the reading this instrument must not
// support -- a crouch-walker legitimately emits few steps, and that is not an offence.
// Keeping them inseparable at the source is cheaper than remembering the rule.
struct KTPMoveStats
{
	// Rising IN_DUCK edges by horizontal-speed bucket, split by ground contact.
	// Airborne is a separate row because a duck-jump is ducking at running speed by
	// construction, so folding the two makes the ground row unreadable.
	int tapsGround[KTPMove::SPEED_BUCKETS];
	int tapsAir[KTPMove::SPEED_BUCKETS];

	// Milliseconds on the ground by speed bucket, split by crouch state. This is the
	// footstep denominator and the crouch-at-speed measure at once; the engine's step
	// cadence differs between the two stances, so they cannot share a row.
	int groundMsStanding[KTPMove::SPEED_BUCKETS];
	int groundMsDucked[KTPMove::SPEED_BUCKETS];

	// Milliseconds airborne while crouched, by speed bucket. Not derivable from the
	// ground rows, and it is where crouch-at-speed legitimately concentrates.
	int airMsDucked[KTPMove::SPEED_BUCKETS];

	int tapsTotal;
	// fuser4 at each tap, summed and floored. Sum plus count rather than a mean so the
	// consumer can re-derive either; min because exhaustion is the tail, not the centre.
	int stamAtTapSum;
	int stamAtTapMin;        // -1 until a tap has been observed

	// Footsteps as the server actually emitted them, from the sound hook. Split by
	// family rather than filtered, because which families count as a footstep is a
	// judgement and the families are cheap to carry separately.
	int stepsGround;         // surface step samples
	int stepsLadder;
	int soundsWater;         // wade/swim -- not step-timer driven
	int pmoveSoundsTotal;    // every player sound the movement code emitted

	// The engine's own step timer, sampled at PreThink: a rise means the timer was
	// reset, which is what happens when a step fires. An INDEPENDENT observation of the
	// same event as stepsGround, and the only control that separates "this player made
	// no noise" from "this server emitted no footsteps at all" -- the second reads as
	// the first in every per-player figure, and would do so fleet-wide and silently.
	int stepTimerFires;

	// In-flight sampling state. `havePrev` distinguishes "was not crouched" from "have
	// not looked yet": adopting a state without it fabricates an edge at every respawn,
	// and adopting a timer value without it fabricates a step.
	double lastTime;
	int    prevStepTimer;
	bool   havePrev;

	void Reset()
	{
		ResetCounters();
		ForgetSampleState();
	}

	// Clears only what a flush has just shipped. Sampling state is left alone: a
	// respawn is what invalidates it, not a flush, and clearing it here would drop the
	// first step and the first edge of every window.
	void ResetCounters()
	{
		for (int i = 0; i < KTPMove::SPEED_BUCKETS; ++i)
		{
			tapsGround[i] = tapsAir[i] = 0;
			groundMsStanding[i] = groundMsDucked[i] = 0;
			airMsDucked[i] = 0;
		}
		tapsTotal = 0;
		stamAtTapSum = 0;
		stamAtTapMin = -1;
		stepsGround = stepsLadder = soundsWater = pmoveSoundsTotal = 0;
		stepTimerFires = 0;
	}

	// Unusable across a death or a respawn: the player teleports, the step timer is
	// reset by someone else, and the button state we would diff against belongs to a
	// command we were not watching.
	void ForgetSampleState()
	{
		lastTime = 0.0;
		prevStepTimer = 0;
		havePrev = false;
	}

	// One usercmd's worth of observation, taking plain values rather than an edict.
	//
	// The arithmetic lives HERE, not in the sampler, for one reason: the sampler needs a
	// live edict and a running server, so logic left there can only be verified by
	// deploying it. Everything below is exercisable on a workstation, and the part that
	// reads the edict is then small enough to check by eye.
	//
	// `duckPressEdge` is passed in already resolved. The caller owns that because the
	// reason it is trustworthy is an engine ordering fact (see the file header), and
	// burying the button read in here would hide which fields it depends on.
	void Observe(double t, bool onGround, bool ducking, float speedSq,
	             bool duckPressEdge, int stamina, int stepTimer)
	{
		const int bucket = Bucket(speedSq);

		// Nothing is counted on the first sample after a reset or a respawn. Without
		// this, adopting a baseline we never saw fabricates a button edge and a step at
		// every spawn -- and they would look exactly like real ones.
		if (havePrev)
		{
			if (duckPressEdge)
			{
				if (onGround) tapsGround[bucket]++;
				else          tapsAir[bucket]++;
				tapsTotal++;

				stamAtTapSum += stamina;
				if (stamAtTapMin < 0 || stamina < stamAtTapMin)
					stamAtTapMin = stamina;
			}

			// A rise in the engine's step timer is a step having just been played. Read as
			// a TRANSITION and never against a value: DoD owns the reset constant and this
			// stack cannot see it, but a rise means the same thing whatever it is.
			if (stepTimer > prevStepTimer)
				stepTimerFires++;

			// The interval that just elapsed is charged to the state being read now,
			// because pmove writes these fields back at the END of the command that ran in
			// it -- so this is that interval's outcome, not the next one's.
			const double gapMs = (t - lastTime) * 1000.0;
			if (gapMs >= 0.0 && gapMs <= KTPMove::SAMPLE_GAP_MS)
			{
				const int ms = (int)(gapMs + 0.5);
				if (onGround)
				{
					if (ducking) groundMsDucked[bucket] += ms;
					else         groundMsStanding[bucket] += ms;
				}
				else if (ducking)
				{
					airMsDucked[bucket] += ms;
				}
			}
		}

		lastTime = t;
		prevStepTimer = stepTimer;
		havePrev = true;
	}

	// Bucket for a squared horizontal speed. Squared so the sampler needs no sqrt in a
	// path that runs once per usercmd per player.
	static int Bucket(float speedSq)
	{
		const float w = (float)KTPMove::BUCKET_WIDTH_UNITS;
		float hi = w;
		for (int i = 0; i < KTPMove::SPEED_BUCKETS - 1; ++i, hi += w)
		{
			if (speedSq < hi * hi) return i;
		}
		return KTPMove::SPEED_BUCKETS - 1;
	}

	static int Wire(int v)
	{
		if (v < 0) return 0;
		return (v > KTPMove::WIRE_MAX) ? KTPMove::WIRE_MAX : v;
	}
};

#endif // KTP_MOVE_ACCUM_H
