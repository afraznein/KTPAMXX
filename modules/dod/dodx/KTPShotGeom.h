// KTP: per-shot aim geometry, captured inside the lag-compensation window.
//
// SENSOR, NOT DETECTOR. It reports the geometry of a shot and applies no threshold,
// computes no ratio, and reaches no conclusion. This repo is public; a threshold here
// would be a published evasion recipe. Judgement lives in the private consumer.
//
// WHY THE CAPTURE POINT IS FORCED. The bullet trace runs inside SV_SetupMove /
// SV_RestoreMove, so enemy origins are only the positions the shooter actually saw
// while lag compensation was applied. `dod_client_weapon_fire` fires OUTSIDE that
// window, reached from the CurWeapon clip-decrement path -- by then the world has been
// restored and the geometry is gone. So the trace captures and the forward only reads.
//
// WHAT SHIPS, AND WHY NOTHING IS REDUCED. Range and target angular velocity travel
// with every sample rather than being folded into any summary: the consumer needs the
// covariates, not a scalar, and reducing them in this layer would bake a judgement
// into a public file.
//
// THE PAIRING GUARD, AND WHY IT IS NOT A GAMETIME COMPARE. The one unforgivable
// failure is a shot whose trace never reached the capture silently inheriting the
// previous shot's numbers -- fabricated evidence attributed to a real person. An
// earlier cut of this struct guarded with gpGlobals->time equality, which fails in
// BOTH directions:
//
//   - It rejects every real shot. During a usercmd the engine sets gpGlobals->time to
//     the client's svtimebase (rehlds sv_user.cpp: SV_PlayerRunPreThink_internal, and
//     again before PostThink), and it advances per command. The fire trace runs in
//     PostThink of cmd N; the CurWeapon message that dispatches the forward is sent
//     from the game's UpdateClientData in HLSDK-lineage DLLs -- the PreThink of cmd
//     N+1. Trace time and read time are one usercmd apart, never equal.
//   - Widening it to a tolerance re-admits aliasing: any stale stash inside the
//     window becomes readable by a shot that missed.
//
// The guard here is a per-player usercmd ordinal instead. DODX's PreThink hook body
// runs AFTER the game's PreThink (it calls the chain first), so for one player the
// order per cmd is: game PreThink (forward dispatch + native read happen here) ->
// hook body (cmdSeq++) -> player Think, pmove and touch handlers -> PostThink (fire
// trace stamps geomSeq = cmdSeq). A read
// therefore sees cmdSeq still equal to the fire cmd's stamp, exactly, whether the
// game DLL sends CurWeapon from the next cmd's PreThink (HLSDK), from the same
// PostThink after firing, or from the send phase before the player's next cmd. The
// counter is per-player on purpose: other players' cmds must not advance it between
// a shooter's trace and his read.
//
// On top of the ordinal: the read is DESTRUCTIVE (a stash reports at most one shot),
// capture is first-wins within a cmd (a later same-cmd trace cannot replace the
// bullet's), and the reader must present the forward's weapon id, which has to match
// the weapon the shooter held at trace time. Every guard failure returns "no
// geometry", never a substitute value.
//
// WHAT THE GUARD DOES NOT EXCLUDE, stated rather than implied: a player-hitting
// TraceLine issued by the game DLL with the SHOOTER as ignore-entity inside his own
// cmd window -- in a cmd where his bullet hit nobody it fills the gap, and between
// the hook body and PostThink (player Think / touch handlers) it can DISPLACE the
// bullet's own capture. dod.so is closed source, so such traces cannot be enumerated
// from here; vanilla HLSDK has none on these paths. See the capture site in
// moduleconfig.cpp for the full residual list.
#ifndef KTP_SHOT_GEOM_H
#define KTP_SHOT_GEOM_H

#include <math.h>

struct KTPShotGeom
{
	// This player's usercmd ordinal. Bumped once per cmd by the PreThink hook body;
	// never reset mid-connection, so a stale stamp can only fall behind, never
	// re-match (wrap needs 2^32 cmds on one connection, and reset() zeroes cmdSeq
	// and geomSeq together, so no reset path can re-match either). 0 means no cmd
	// has run yet, and capture refuses it.
	unsigned int cmdSeq;

	// The stash: one attack trace's geometry. geomSeq is the cmd it was captured
	// in; 0 means empty or already consumed, which is how "no geometry for this
	// shot" and "geometry for this shot" stay impossible to confuse.
	unsigned int geomSeq;
	int geomWeapon;     // shooter's in-hand weapon id at trace time (CPlayer::current)
	int errUdeg;        // angle between the traced ray and the line to target centre, micro-deg
	int rangeUnits;     // trace start to target centre, world units
	int tgtAngVelMdps;  // target bearing rate across the shooter's view, milli-deg/s; -1 = no prior sighting
	int sightGapMs;     // time the bearing rate is averaged over, ms; -1 when tgtAngVelMdps is -1
	int hitgroup;       // free: ReHLDS already traces real studio hitboxes
	// Trace start's distance from the shooter's view origin, world units. Near 0
	// for a fire trace; a penetration continuation starts at the wall exit point,
	// which shrinks range and inflates the angle -- shipped so the consumer can
	// tell those samples apart instead of this layer guessing.
	int startOffUnits;

	// Previous captured sighting of THIS target, for the bearing rate above.
	// Per-target, not global: a shooter switching between two enemies would
	// otherwise read the angle between two different people as one target's
	// movement. prevTime is double so the subtraction does not add its own ulp
	// error on top of the float quantisation gpGlobals->time already arrives with.
	int    prevTarget;
	double prevTime;
	float  prevDir[3];

	void reset()
	{
		cmdSeq = 0;
		geomSeq = 0;
		geomWeapon = 0;
		errUdeg = rangeUnits = hitgroup = 0;
		startOffUnits = 0;
		tgtAngVelMdps = sightGapMs = -1;
		prevTarget = 0;
		prevTime = 0.0;
		prevDir[0] = prevDir[1] = prevDir[2] = 0.0f;
	}

	void consume()
	{
		geomSeq = 0;
	}
};

namespace ktpshot
{
	inline float dot3(const float *a, const float *b)
	{
		return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
	}

	// Returns the length, 0.0f for a degenerate vector rather than normalising to
	// garbage. Zero length means the two points coincide, which is not a shot with
	// an angle -- and the caller wants the length anyway (range).
	inline float normalize3(const float *v, float *out)
	{
		float len = sqrtf(dot3(v, v));
		if (len < 1e-6f) return 0.0f;
		out[0] = v[0]/len; out[1] = v[1]/len; out[2] = v[2]/len;
		return len;
	}

	// Angle between two unit vectors, in micro-degrees.
	//
	// acosf, not the cheaper small-angle approximation. The approximation is excellent
	// near zero and wrong exactly where a human's error lives, and this runs at shot
	// rate, not usercmd rate -- the per-usercmd budget that forbids trigonometry does
	// not apply at 1/1500th the frequency.
	inline int angleUdeg(const float *a, const float *b)
	{
		float c = dot3(a, b);
		// Clamp: rounding can push a legitimate dot product outside [-1,1] and acosf
		// would return NaN, which becomes a meaningless int and reads as a measurement.
		if (c > 1.0f) c = 1.0f;
		else if (c < -1.0f) c = -1.0f;
		double deg = acos((double)c) * (180.0 / 3.14159265358979323846);
		double u = deg * 1e6;
		if (u < 0.0) u = 0.0;
		if (u > 2147483000.0) u = 2147483000.0;  // int32 headroom, not a threshold
		return (int)u;
	}
}

#endif // KTP_SHOT_GEOM_H
