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

	// The target's own state at trace time, in a SEPARATE one-shot stash from the
	// geometry above. Two stashes, not one widened payload, because
	// dodx_get_shot_geom's read is destructive and has exactly one consumer
	// (KTPMatchHandler, feeding ktp_ac_weapon_fires). A second consumer sharing
	// that stash would silently starve whichever one read second; widening its
	// out[] array instead would overflow the caller's fixed geom[6]. An
	// independent seq/consume pair leaves that contract byte-identical.
	//
	// WHY THIS EXISTS. A trace that hit a real studio hitbox but produced no
	// damage row has three candidate explanations that only the target's own
	// state can separate: it was already dead (another shot resolved first in the
	// same instant), it was a teammate (friendly fire the game DLL zeroed before
	// the damage hook), or neither -- which is the only one that means damage is
	// genuinely going missing. Inferring that after the fact from two
	// independently-ingested tables cannot distinguish them; reading v.health /
	// v.deadflag / v.team here, at the instant the trace resolved, can.
	unsigned int tgtSeq;   // cmd this target sample belongs to; 0 = empty/consumed
	int tgtEntIndex;       // ENTINDEX of the player the trace hit
	int tgtHealth;         // v.health at trace time; <=0 with deadflag clear is a same-tick kill
	int tgtDead;           // 1 when v.deadflag != DEAD_NO at trace time
	int tgtTeam;           // v.team at trace time; compare against the shooter's
	int tgtShooterTeam;    // shipped alongside so the consumer needs no roster join

	// The shooter's measured ping and loss at trace time. The one field that can
	// separate a network-caused miss from a server-side one, and it has to be read
	// HERE: by the time any consumer runs, the shooter's ping has moved, and a
	// session-average ping (which is all hlstats_Events_StatsmeLatency keeps)
	// cannot say what it was for one shot.
	int tgtPing;
	int tgtLoss;

	// How many player-hitting traces this shooter's cmd produced, and whether the
	// captured one was the first.
	//
	// WHY THIS IS NOT OPTIONAL. The stash is first-wins within a cmd, and the
	// header above admits what that cannot exclude: a shooter-owned player-hitting
	// trace that is NOT the bullet -- fired from player Think or a touch handler
	// between the PreThink hook body and PostThink -- reaches the capture first and
	// DISPLACES the bullet's own. Such a sample reports a hitgroup, applies no
	// damage, and leaves the target's health untouched, which is exactly the
	// signature of the case we are trying to count as "damage went missing".
	//
	// Without this counter the two are indistinguishable and every ratio built on
	// "confirmed hit" is uninterpretable. With it, a consumer can ask whether the
	// unexplained samples are the ones from cmds that fired more than one
	// player-hitting trace -- measured, not assumed. Measured on a bot match
	// 2026-09-12: 41.7% of confirmed live-enemy hits had no damage row and 100% of
	// those left health flat, which both hypotheses predict equally.
	//
	// The count is NOT stamped at capture time: under first-wins the captured
	// sample is always the cmd's first, so a "was I first" flag would be
	// tautologically 1 and measure nothing. It is accumulated across the whole cmd
	// and read afterwards -- the read runs in the next cmd's PreThink, before the
	// hook body advances cmdSeq, so the counter is still the capture cmd's when a
	// consumer asks. traceSeq keys it to that cmd so a stale count can never be
	// reported against a newer sample.
	unsigned int traceSeq; // cmd traceCount belongs to
	int traceCount;        // player-hitting traces this shooter produced that cmd

	// Mechanics of the trace itself, and the victim's capacity to be damaged at
	// all. Both answer "was this a hit that COULD have applied damage" without
	// inferring it from whether damage later appeared.
	//
	// traceFrac is flFraction x10000: how far along the ray the trace stopped.
	// traceFlags is a bitfield rather than four columns because the wire line is
	// already near its budget and these are all booleans:
	//   bit0 fStartSolid -- the trace STARTED inside solid geometry. A known
	//        GoldSrc failure mode that silently eats hits, and nothing in this
	//        stack has ever recorded it.
	//   bit1 fAllSolid   -- the whole trace was in solid.
	//   bit2 target was SOLID_NOT at trace time.
	//   bit3 target had takedamage == DAMAGE_NO at trace time -- an invulnerable
	//        or not-yet-damageable victim (spawn protection, warmup) explains a
	//        hit with no damage outright, and is invisible after the fact.
	int traceFrac;
	int traceFlags;

	// allTraceCount counts EVERY trace this shooter owned in the cmd, not just
	// the player-hitting ones traceCount sees. The difference is the whole point:
	// a wall-hitting trace never reaches the capture, so traceCount cannot see
	// the initial trace of a penetration chain, only its continuation.
	//
	// WHY IT MATTERS. fStartSolid on its own is ambiguous. DoD penetrates walls
	// by re-tracing from the wall, so a continuation trace legitimately starts
	// inside solid and legitimately applies no damage when the bullet fails to
	// exit -- benign, and indistinguishable from the shooter's own eye being
	// stuck in geometry, which is not. Two things separate them: startOffUnits
	// (below) is ~0 for an eye-origin trace and large for a continuation, and a
	// continuation implies the cmd carried an earlier trace this counter can see.
	unsigned int allTraceSeq;
	int allTraceCount;

	// Distance from the shooter's view origin to where the captured trace
	// STARTED. Already computed for the geometry stash; carried here too so the
	// shot stream can tell an eye-origin trace from a penetration continuation
	// without depending on the other stash's single consumer.
	int tgtStartOff;

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
		resetTarget();
	}

	void resetTarget()
	{
		tgtSeq = 0;
		tgtEntIndex = 0;
		tgtHealth = 0;
		tgtDead = 0;
		tgtTeam = 0;
		tgtShooterTeam = 0;
		tgtPing = 0;
		tgtLoss = 0;
		traceSeq = 0;
		traceCount = 0;
		traceFrac = 0;
		traceFlags = 0;
		allTraceSeq = 0;
		allTraceCount = 0;
		tgtStartOff = 0;
	}

	void consume()
	{
		geomSeq = 0;
	}

	void consumeTarget()
	{
		tgtSeq = 0;
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
