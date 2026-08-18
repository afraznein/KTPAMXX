// KTP: per-usercmd trigger-actuation timing relative to aim/target crossings, fed
// from SV_PlayerRunPreThink beside the aim sampler.
//
// THIS IS A SENSOR, NOT A DETECTOR. It records WHEN the fire button was pressed
// relative to when the aim ray entered a target's bounding box, and WHOSE motion
// caused that entry. It holds no threshold, no ratio and no conclusion -- this
// repository is public, so a bound written here is a bound a reader can sit outside.
// Whatever consumes these records decides what they mean.
//
// WHY NO ENGINE TRACE. The obvious build -- re-tracing the aim ray through
// pfnTraceLine every usercmd -- was assessed and rejected before this file existed:
// it needs an engine export, it re-enters the module's own TraceLine hook (which
// attributes traces as shots for HLStatsX), and pfnMakeVectors/TraceLine write
// engine globals mid-PreThink. Everything here is instead computed from edict state
// the module already reads: a ray/box slab test per enemy, and no engine call that
// writes engine state (the sole engine read is the userid lookup, argued below).
// The view ray is built with local sincos rather than pfnMakeVectors for the same
// reason -- MAKE_VECTORS writes gpGlobals->v_forward/right/up, and clobbering those
// under a closed-source game DLL is a bug we cannot even test for.
//
// WHAT A RECORD IS. A press (IN_ATTACK rising edge in the usercmd stream -- the
// actuation the client generated, independent of whether a bullet resulted) is
// reported with: signed time since the aim ray last entered the chosen target's box,
// the shooter's view angular speed and the line-of-sight angular speed at that entry,
// aim error and range at the press, weapon, and the client frame spacing. The two
// angular speeds exist so a consumer can tell a crossing the shooter's own hand
// produced from one the target's movement produced -- the press timing of those two
// cases means different things, and reducing them to one number here would destroy
// exactly the distinction a consumer needs.
//
// A press with the aim ray outside every enemy is held briefly and, if a crossing
// follows, reported with NEGATIVE latency. Firing ahead of a crossing is a thing
// humans do; dropping those presses would discard the evidence that most strongly
// distinguishes anticipation from reaction, and it is not this file's job to decide
// which of the two it is looking at.
//
// KNOWN FRAME SKEW, deliberately not corrected. Enemy boxes here are the CURRENT
// server positions; the hit trace the game runs at fire time sees lag-compensated
// ones, and the client acted on its own interpolated view of both. So a latency
// reported here contains a per-client offset that depends on route latency, interp
// settings and which side's motion caused the crossing. Correcting for that needs
// per-client network state this module does not own; the onset angular speeds and
// frame spacing ship precisely so the consumer can model it instead of trusting a
// number that silently pretends the three clocks agree.
//
// KNOWN OCCLUSION BLINDNESS, deliberately not corrected either -- and the larger of
// the two contaminants. The ray is infinite and untraced (tracing is the rejected
// design above), so a crossing fires for an enemy behind a wall or across the map,
// and the crossings counter is dominated by entries no one could see. Bias direction
// differs per path and is established for only one of them. On the INSIDE-PRESS path
// a latency anchored on an occluded entry runs LONG (the target crossed before it
// became visible), which reads more human. On the PENDING path it is UNESTABLISHED:
// a through-wall crossing can consume the pending record earlier than the visible one
// would, SHORTENING the reported anticipation window. Do not read "never less human"
// as covering both -- a consumer correcting for this bias needs the distinction, and
// any consumer dividing by crossings is dividing by a mixed population.
//
// ⚠️ THE BACKWARD-CLOCK GUARD IS NOT A MAP-IDENTITY GUARD. It cannot fire when the
// old map's final time is LESS than the new map's time at the first post-change
// sample -- a changelevel within a few seconds of map start. That makes the
// DODX_OnSV_ActivateServer reset LOAD-BEARING, not belt-and-braces: do not drop it as
// redundant on the strength of the in-file guard.
//
// Requires dodx.h (CPlayer, players[], engine globals). Include from exactly one
// translation unit: state lives in file-scope statics, and a second inclusion would
// silently give that unit its own private copy of every counter.
#ifndef KTP_TRIGGER_GEOM_H
#define KTP_TRIGGER_GEOM_H

#include <math.h>
#include "KTPShotGeom.h"   // ktpshot vector helpers; angles in micro-degrees

namespace KTPTrig
{
	// Press records retained per player between flushes. Same payload reasoning as
	// the aim sampler's retained windows: enough slots that a consumer computes a
	// proportion rather than reading an extreme, with the observed-event count
	// shipped alongside so the sampling fraction stays recoverable.
	// ⚠️ Raising this raises the flush payload linearly and the consumer's buffer
	// must be re-derived from ITS loop bound, or truncation returns.
	const int    KEEP_PRESSES = 16;

	// How long an off-target press stays completable by a later crossing. This
	// bounds STATE LIFETIME, nothing else: it is far above any press-to-crossing
	// gap a consumer would treat as associated, so it cannot function as a gate,
	// and a press it expires is still counted -- only its pairing is lost.
	const double PEND_MAX_S   = 1.0;

	// Below this a ray direction component is treated as parallel to the slab.
	// Degenerate-input guard, same role as the normalize floor in KTPShotGeom.
	const float  DIR_EPS      = 1e-8f;

	const int    MAX_TRACK    = 33;    // engine client slots, index 0 unused

	// Record flags. A field that is not known is zero AND unflagged, so a consumer
	// can never mistake "unmeasured" for "measured as zero" -- the distinction the
	// AC's own rules require ("unknown, never clean").
	const int    FL_INSIDE      = 1;   // aim ray inside the chosen target's box at press
	const int    FL_SCOPED      = 2;   // shooter was scoped at press
	const int    FL_ONSET_KNOWN = 4;   // latMs is a real interval, not a placeholder
	const int    FL_RATES_KNOWN = 8;   // onset angular speeds were computable
	// The press-time geometry (errUdeg, rangeUnits) and the crossing timing describe
	// the SAME target. A completed off-target press is anchored on the enemy nearest
	// at actuation but completed by whichever enemy crosses first; when those differ
	// the record mixes two people's geometry, and a consumer must be able to see that
	// rather than correlate a latency with another target's range.
	const int    FL_CTX_MATCH   = 16;
}

// One press event with its crossing context. Reported as-is.
struct KTPPressRec
{
	int latMs;      // signed ms from box entry to press; negative = press came first.
	                // Unbounded within a map by design: a crosshair resting on a
	                // target accrues for as long as it rests, so a large value is a
	                // measurement, not an overflow
	int errUdeg;    // angle between aim ray and target-box centre at press
	int rangeUnits; // shooter eye to target-box centre at press
	int viewMdps;   // shooter view angular speed at the box entry, milli-deg/s
	int losMdps;    // line-of-sight angular speed at the box entry, milli-deg/s
	int weapon;     // DODX weapon id; consumer must filter -- melee and grenades
	                // actuate through the same button
	int flags;      // KTPTrig::FL_*
	int frameMs;    // client frame spacing at the anchoring sample -- the timing
	                // quantum every interval above is measured in. -1 until measured
	                // (same sentinel discipline as the aim sampler's ground stat):
	                // zero would read as a real quantum and invite a divide by it
};

// Shooter x target crossing state. Per pair, not per shooter: a shooter with two
// enemies in view has two independent entry clocks, and collapsing them would read
// the angle between two different people as one target's movement -- the same
// failure mode KTPShotGeom guards against with per-target sighting.
struct KTPTrigPair
{
	bool  seen;          // pair has a previous sample; without one, no transition
	                     // and no rate is inferable, only adopted (never fabricated)
	bool  inside;
	bool  onsetKnown;
	bool  onsetRates;
	float prevLos[3];    // unit line-of-sight at the previous sample
	float onsetTime;
	int   onsetViewMdps;
	int   onsetLosMdps;
};

struct KTPTrigShooter
{
	// Engine userid of the slot's occupant when state was last reset. Slots are
	// reused mid-map, and inherited counters would attribute one player's presses
	// to the next occupant -- checked every sample so the guard holds even if the
	// connect/disconnect wiring is ever lost in a refactor.
	int   userid;

	bool  viewSeen;
	bool  buttonsSeen;
	float prevView[3];
	float prevTime;
	int   prevButtons;

	// Pending off-target press, held until a crossing completes it or PEND_MAX_S
	// expires it. One slot: a newer off-target press replaces an older one, because
	// the press nearest the eventual crossing is the one whose timing describes it.
	bool  pendOpen;
	float pendTime;
	int   pendTarget;    // enemy the press-time geometry describes, for FL_CTX_MATCH
	int   pendErrUdeg;
	int   pendRange;
	int   pendWeapon;
	int   pendScoped;
	int   pendFrameMs;

	// Denominators. Retained records are a sample; these are the population sizes a
	// consumer needs to read that sample honestly -- crossings without a press and
	// presses without a crossing are both signal, and only the counts carry them.
	int   crossings;      // aim ray entered some enemy's box
	int   presses;        // IN_ATTACK rising edges observed
	int   pressesInside;  // presses with the ray inside some enemy's box
	int   poolSeen;       // press events eligible for retention (reservoir denominator)

	unsigned    rng;
	int         keptCount;
	KTPPressRec kept[KTPTrig::KEEP_PRESSES];
	KTPTrigPair pair[KTPTrig::MAX_TRACK];

	// xorshift32 -- uniformity is the only requirement here. The reservoir keeps
	// every press equally likely to be retained, so no press can be deterministically
	// crowded out of the sample by producing others around it; a chronological
	// first-N would allow exactly that.
	unsigned NextRand()
	{
		unsigned x = rng;
		x ^= x << 13; x ^= x >> 17; x ^= x << 5;
		rng = x ? x : 0x9E3779B9u;
		return rng;
	}

	// In-flight state only. A death or spectate mid-approach is not a crossing that
	// resolved, and scoring any part of it would mix populations; the shipped
	// counters and records describe only what completed.
	//
	// buttonsSeen goes false with the rest: the edge detector must re-adopt the held
	// buttons before judging a rising edge, or a player who respawns holding +attack
	// (the death-cam next-player bind) ships a fabricated press whose actuation
	// happened while dead -- and one who died mid-burst has his first real press
	// eaten by a stale held bit. Adopt-first, like the ground tracker's groundKnown.
	void DropInFlight()
	{
		viewSeen = false;
		buttonsSeen = false;
		pendOpen = false;
		for (int i = 0; i < KTPTrig::MAX_TRACK; i++)
		{
			pair[i].seen = false;
			pair[i].inside = false;
			pair[i].onsetKnown = false;
			pair[i].onsetRates = false;
		}
	}

	// Clears only what a flush has just SHIPPED. The pending press and the pair
	// clocks straddling a flush are left alone so an interval that ends mid-approach
	// reports the press once, when it resolves, with its true latency.
	void ResetCounters()
	{
		crossings = 0;
		presses = 0;
		pressesInside = 0;
		poolSeen = 0;
		keptCount = 0;
	}

	void Reset(int newUserid, unsigned seed)
	{
		userid = newUserid;
		prevTime = 0.0f;
		prevButtons = 0;
		prevView[0] = prevView[1] = prevView[2] = 0.0f;
		rng = seed ? seed : 0x9E3779B9u;
		ResetCounters();
		DropInFlight();
	}

	void KeepPress(const KTPPressRec &r)
	{
		poolSeen++;
		if (keptCount < KTPTrig::KEEP_PRESSES)
		{
			kept[keptCount++] = r;
			return;
		}
		unsigned j = NextRand() % (unsigned)poolSeen;
		if (j < (unsigned)KTPTrig::KEEP_PRESSES)
			kept[j] = r;
	}
};

static KTPTrigShooter g_ktpTrig[KTPTrig::MAX_TRACK];

namespace KTPTrig
{
	// Forward vector from GoldSrc view angles. Local sincos, never pfnMakeVectors:
	// see the file header for why touching gpGlobals->v_forward here is off-limits.
	// This is the only transcendental work on the per-usercmd path, fixed-cost and
	// branchless -- the trig this file keeps OFF that path is the acos in the angle
	// helpers, which runs only at press and crossing events.
	inline void ViewForward(const edict_t *pEntity, float *out)
	{
		const double d2r = 3.14159265358979323846 / 180.0;
		double p = (double)pEntity->v.v_angle.x * d2r;
		double y = (double)pEntity->v.v_angle.y * d2r;
		double cp = cos(p);
		out[0] = (float)(cp * cos(y));
		out[1] = (float)(cp * sin(y));
		out[2] = (float)(-sin(p));
	}

	// Ray/AABB slab test. The box is the target entity's OWN v.mins/v.maxs -- taken
	// as the closed-source game DLL maintains them for that stance, adopted rather
	// than re-derived: whether dod.so resizes the hull for prone is its property,
	// not ours to assert, and hand-entered dimensions here would be worse than
	// inherited ones. A near-parallel axis is handled by an explicit containment
	// branch rather than infinity arithmetic, whose 0*inf NaN would fail closed on
	// a boundary-exact eye position.
	inline bool RayHitsBox(const float *eye, const float *dir,
	                       const float *bmin, const float *bmax)
	{
		float tNear = -3.4e38f, tFar = 3.4e38f;
		for (int a = 0; a < 3; a++)
		{
			float d = dir[a];
			if (d > -DIR_EPS && d < DIR_EPS)
			{
				if (eye[a] < bmin[a] || eye[a] > bmax[a]) return false;
				continue;
			}
			float inv = 1.0f / d;
			float t1 = (bmin[a] - eye[a]) * inv;
			float t2 = (bmax[a] - eye[a]) * inv;
			if (t1 > t2) { float tmp = t1; t1 = t2; t2 = tmp; }
			if (t1 > tNear) tNear = t1;
			if (t2 < tFar)  tFar  = t2;
			if (tNear > tFar) return false;
		}
		// tFar >= 0 admits an eye already inside the box -- point blank is a real
		// aim state, not a degenerate one.
		return tFar >= 0.0f;
	}

	// Angular speed between two unit directions, milli-deg/s. Event-rate only
	// (crossings and presses); the acos cost argument lives in KTPShotGeom.
	inline int RateMdps(const float *a, const float *b, double dt)
	{
		if (dt <= 0.0) return 0;
		double deg = (double)ktpshot::angleUdeg(a, b) * 1e-6;
		double m = deg * 1000.0 / dt;
		if (m > 2147483000.0) m = 2147483000.0;   // int32 headroom, not a threshold
		return (int)(m + 0.5);
	}

	inline int ClampMs(double s)
	{
		double m = s * 1000.0;
		if (m > 2147483000.0) m = 2147483000.0;
		if (m < -2147483000.0) m = -2147483000.0;
		return (int)(m + (m < 0.0 ? -0.5 : 0.5));
	}
}

// One usercmd's worth of trigger/crossing state, called beside KTPSampleAim. All
// per-usercmd work is slab tests and running geometry -- no allocation, no I/O,
// trig only at press/crossing events plus the fixed view-ray sincos argued above,
// and no engine call that writes engine state. The one engine READ is the userid
// lookup below: a bounded walk of the client array that touches nothing.
static void KTPTriggerSample(CPlayer *pPlayer, edict_t *pEntity)
{
	int index = pPlayer->index;
	if (index < 1 || index >= KTPTrig::MAX_TRACK)
		return;

	KTPTrigShooter &st = g_ktpTrig[index];

	// Slot-reuse tripwire. Also seeds the reservoir; the seed only needs to differ
	// between occupants, not be unpredictable -- retention uniformity is the
	// property, secrecy is not.
	//
	// The engine answers with a negative userid when it cannot match the edict to a
	// client. That is "identity unknown", not an identity: sampling under it would
	// attribute blindly, and storing it would make the next sample reset again and
	// silently discard measured presses. Refuse to do either.
	int uid = (*g_engfuncs.pfnGetPlayerUserId)(pEntity);
	if (uid < 1)
		return;
	if (uid != st.userid)
	{
		union { float f; unsigned u; } tbits;
		tbits.f = (float)gpGlobals->time;
		st.Reset(uid, tbits.u ^ ((unsigned)uid << 16) ^ (unsigned)index);
	}

	// Same gate, same reasoning as the aim sampler: dead players and spectators
	// keep sending usercmds, and in DoD +attack is the spectator next-player bind --
	// a spectator clicking through players would otherwise be the busiest trigger
	// finger on the server. Bots excluded outright: nothing here describes a person.
	const int team = pEntity->v.team;
	if (!pPlayer->IsAlive() || (team != 1 && team != 2)
	    || (pEntity->v.flags & FL_FAKECLIENT))
	{
		st.DropInFlight();
		return;
	}

	const double t = (double)gpGlobals->time;

	// A clock that stepped backward invalidates EVERY held timestamp, not just the
	// rates: g_ktpTrig outlives a map change, the userid tripwire does not fire on
	// one (GoldSrc clients keep their userid across changelevel), and an onsetTime
	// from the old map's clock would ship as a huge negative latency flagged as a
	// real interval -- fabricated evidence, the worst output this file can produce.
	// Small backward svtimebase re-anchors trip this too; losing a pair clock to
	// one is measurement loss in the safe direction, never fabrication.
	if (st.viewSeen && (double)st.prevTime > t)
		st.DropInFlight();

	// svtimebase is re-anchored per packet, so dt can be zero or step backward; a
	// rate over such an interval is not a measurement and is skipped, never faked.
	// A frozen clock (msec = 0 flood: buttons and angles still client-controlled)
	// degrades but cannot fabricate: dt stays zero, so rates and frame spacing ship
	// unflagged, and its records are recognisable by exactly that absence.
	const double dt = st.viewSeen ? (t - (double)st.prevTime) : 0.0;
	const bool   dtValid = st.viewSeen && dt > 0.0;

	// Expire (or drop, on a backward step) the pending press. Expiry loses only the
	// pairing -- the press itself was already counted when it happened.
	if (st.pendOpen &&
	    ((t - (double)st.pendTime) > KTPTrig::PEND_MAX_S || (double)st.pendTime > t))
		st.pendOpen = false;

	float eye[3];
	eye[0] = pEntity->v.origin.x + pEntity->v.view_ofs.x;
	eye[1] = pEntity->v.origin.y + pEntity->v.view_ofs.y;
	eye[2] = pEntity->v.origin.z + pEntity->v.view_ofs.z;

	float viewDir[3];
	KTPTrig::ViewForward(pEntity, viewDir);

	// Per-target working set for this sample, so the press logic below reads the
	// same geometry the transition logic saw rather than recomputing it.
	bool  tgtValid[KTPTrig::MAX_TRACK];
	bool  tgtInside[KTPTrig::MAX_TRACK];
	float tgtLos[KTPTrig::MAX_TRACK][3];
	int   tgtRange[KTPTrig::MAX_TRACK];

	const int maxc = gpGlobals->maxClients < (KTPTrig::MAX_TRACK - 1)
	               ? gpGlobals->maxClients : (KTPTrig::MAX_TRACK - 1);

	for (int i = 1; i <= maxc; i++)
	{
		tgtValid[i] = false;

		if (i == index)
			continue;

		CPlayer *pTgt = GET_PLAYER_POINTER_I(i);
		edict_t *pTe  = pTgt->pEdict;
		KTPTrigPair &pr = st.pair[i];

		const int tteam = pTe ? (int)pTe->v.team : 0;
		if (!pTgt->ingame || !pTe || pTe->free || !pTgt->IsAlive()
		    || (pTe->v.flags & FL_FAKECLIENT)
		    || (tteam != 1 && tteam != 2) || tteam == team)
		{
			// The pair's clock is meaningless across a death or leave: the target
			// teleports on respawn, so the contact neither continues nor ended
			// where we last saw it.
			pr.seen = false;
			pr.inside = false;
			pr.onsetKnown = false;
			pr.onsetRates = false;
			continue;
		}

		float bmin[3], bmax[3], centre[3], losVec[3];
		bmin[0] = pTe->v.origin.x + pTe->v.mins.x;
		bmin[1] = pTe->v.origin.y + pTe->v.mins.y;
		bmin[2] = pTe->v.origin.z + pTe->v.mins.z;
		bmax[0] = pTe->v.origin.x + pTe->v.maxs.x;
		bmax[1] = pTe->v.origin.y + pTe->v.maxs.y;
		bmax[2] = pTe->v.origin.z + pTe->v.maxs.z;
		centre[0] = (bmin[0] + bmax[0]) * 0.5f;
		centre[1] = (bmin[1] + bmax[1]) * 0.5f;
		centre[2] = (bmin[2] + bmax[2]) * 0.5f;
		losVec[0] = centre[0] - eye[0];
		losVec[1] = centre[1] - eye[1];
		losVec[2] = centre[2] - eye[2];

		// One square root serves both the unit direction and the range -- the
		// costliest arithmetic on this path, paid once per pair. The floor matches
		// the normalize floor in KTPShotGeom, squared.
		float len2 = ktpshot::dot3(losVec, losVec);
		if (len2 < 1e-12f)
		{
			// Coincident points have no direction; treat like an invalid target
			// rather than inventing one.
			pr.seen = false;
			pr.inside = false;
			pr.onsetKnown = false;
			pr.onsetRates = false;
			continue;
		}
		float len = sqrtf(len2);
		float linv = 1.0f / len;
		float losDir[3];
		losDir[0] = losVec[0] * linv;
		losDir[1] = losVec[1] * linv;
		losDir[2] = losVec[2] * linv;

		const bool insideNow = KTPTrig::RayHitsBox(eye, viewDir, bmin, bmax);

		tgtValid[i]  = true;
		tgtInside[i] = insideNow;
		tgtLos[i][0] = losDir[0]; tgtLos[i][1] = losDir[1]; tgtLos[i][2] = losDir[2];
		tgtRange[i]  = len > 2147483000.0f ? 2147483000 : (int)(len + 0.5f);

		// A transition only exists relative to a previous sample of the SAME pair;
		// a target first seen already inside gets an adopted state and no onset,
		// so its eventual press ships unflagged rather than with an invented clock.
		if (pr.seen && insideNow && !pr.inside)
		{
			st.crossings++;
			pr.onsetTime  = (float)t;
			pr.onsetKnown = true;
			pr.onsetRates = dtValid;
			pr.onsetViewMdps = dtValid ? KTPTrig::RateMdps(st.prevView, viewDir, dt) : 0;
			pr.onsetLosMdps  = dtValid ? KTPTrig::RateMdps(pr.prevLos, losDir, dt) : 0;

			// Complete a held off-target press against the first crossing that
			// follows it. The record's error and range describe the aim situation
			// at the ACTUATION; the crossing supplies the timing and motion
			// context, and when the crossing enemy is not the one the actuation
			// geometry describes, the absent FL_CTX_MATCH says so.
			if (st.pendOpen)
			{
				KTPPressRec r;
				r.latMs      = -KTPTrig::ClampMs(t - (double)st.pendTime);
				r.errUdeg    = st.pendErrUdeg;
				r.rangeUnits = st.pendRange;
				r.viewMdps   = pr.onsetViewMdps;
				r.losMdps    = pr.onsetLosMdps;
				r.weapon     = st.pendWeapon;
				r.flags      = KTPTrig::FL_ONSET_KNOWN
				             | (st.pendScoped ? KTPTrig::FL_SCOPED : 0)
				             | (pr.onsetRates ? KTPTrig::FL_RATES_KNOWN : 0)
				             | (i == st.pendTarget ? KTPTrig::FL_CTX_MATCH : 0);
				r.frameMs    = st.pendFrameMs;
				st.KeepPress(r);
				st.pendOpen = false;
			}
		}

		pr.inside = insideNow;
		pr.prevLos[0] = losDir[0]; pr.prevLos[1] = losDir[1]; pr.prevLos[2] = losDir[2];
		pr.seen = true;
	}

	// Press = rising edge. Discharge is deliberately not required: an actuation
	// into an empty clip or a cycling bolt is still the client deciding to fire,
	// and the discharge stream already exists separately for a consumer to join.
	// The first sample after a reset or a respawn only ADOPTS the held buttons --
	// an edge needs two samples of the same life to exist.
	const int buttons = pEntity->v.button;
	const bool press = st.buttonsSeen
	                && (buttons & IN_ATTACK) && !(st.prevButtons & IN_ATTACK);

	if (press)
	{
		st.presses++;

		// Choose among inside targets by aim error; among the rest, the nearest by
		// aim error anchors the off-target press. acos here is press-rate.
		int bestIn = 0, bestInErr = 0;
		int bestAny = 0, bestAnyErr = 0;
		for (int i = 1; i <= maxc; i++)
		{
			if (!tgtValid[i])
				continue;
			int e = ktpshot::angleUdeg(viewDir, tgtLos[i]);
			if (!bestAny || e < bestAnyErr) { bestAny = i; bestAnyErr = e; }
			if (tgtInside[i] && (!bestIn || e < bestInErr)) { bestIn = i; bestInErr = e; }
		}

		const int frameMs = dtValid ? KTPTrig::ClampMs(dt) : -1;

		if (bestIn)
		{
			st.pressesInside++;
			KTPTrigPair &pr = st.pair[bestIn];
			KTPPressRec r;
			r.latMs      = pr.onsetKnown ? KTPTrig::ClampMs(t - (double)pr.onsetTime) : 0;
			r.errUdeg    = bestInErr;
			r.rangeUnits = tgtRange[bestIn];
			r.viewMdps   = pr.onsetRates ? pr.onsetViewMdps : 0;
			r.losMdps    = pr.onsetRates ? pr.onsetLosMdps : 0;
			r.weapon     = pPlayer->current;
			// Geometry and timing describe the same enemy by construction here.
			r.flags      = KTPTrig::FL_INSIDE | KTPTrig::FL_CTX_MATCH
			             | (pPlayer->is_scoped ? KTPTrig::FL_SCOPED : 0)
			             | (pr.onsetKnown ? KTPTrig::FL_ONSET_KNOWN : 0)
			             | (pr.onsetRates ? KTPTrig::FL_RATES_KNOWN : 0);
			r.frameMs    = frameMs;
			st.KeepPress(r);
			st.pendOpen = false;
		}
		else if (bestAny)
		{
			// Held rather than recorded: only a later crossing proves this press
			// was aimed AT something.
			st.pendOpen    = true;
			st.pendTime    = (float)t;
			st.pendTarget  = bestAny;
			st.pendErrUdeg = bestAnyErr;
			st.pendRange   = tgtRange[bestAny];
			st.pendWeapon  = pPlayer->current;
			st.pendScoped  = pPlayer->is_scoped ? 1 : 0;
			st.pendFrameMs = frameMs;
		}
		else
		{
			// No live enemy: nothing a crossing could associate, so the press
			// stays a bare count -- and it still supersedes any older pending,
			// or that one would outlive the newer actuation and claim a crossing
			// that followed a different press.
			st.pendOpen = false;
		}
	}

	st.prevButtons = buttons;
	st.buttonsSeen = true;
	st.prevView[0] = viewDir[0]; st.prevView[1] = viewDir[1]; st.prevView[2] = viewDir[2];
	st.prevTime = (float)t;
	st.viewSeen = true;
}

// KTP: read a player's trigger/crossing summary. Measurements only -- no threshold
// is applied here and none should be added; the consumer decides what they mean.
//
// The counters are not one population within a flush interval: a press near the
// boundary lands in this interval while the crossing that completes it lands in the
// next, so presses and poolSeen can disagree by the events in flight. A consumer
// comparing them must aggregate across intervals, not within one.
//
// out[] = { crossings, presses, pressesInside, poolSeen, keptCount }
static cell AMX_NATIVE_CALL dodx_get_trigger_stats(AMX *amx, cell *params)
{
	int index = params[1];
	CHECK_PLAYER(index);

	CPlayer *pPlayer = GET_PLAYER_POINTER_I(index);
	if (!pPlayer->ingame || !pPlayer->pEdict || pPlayer->pEdict->free)
		return 0;

	const KTPTrigShooter &st = g_ktpTrig[index];

	// A slot can be reoccupied between the new player's PutInServer and his first
	// usercmd; in that window the stored state is still the previous occupant's.
	// Refusing the read here is what keeps one player's presses from ever being
	// reported under another's name.
	if ((*g_engfuncs.pfnGetPlayerUserId)(pPlayer->pEdict) != st.userid)
		return 0;

	cell *out = MF_GetAmxAddr(amx, params[2]);

	out[0] = st.crossings;
	out[1] = st.presses;
	out[2] = st.pressesInside;
	out[3] = st.poolSeen;
	out[4] = st.keptCount;

	return 1;
}

// KTP: read one retained press record. Scaled integers for the same reason the aim
// windows ship them: these cross into Pawn and then JSON, and a float round-trip
// through both is where a comparison silently shifts.
//
// Reservoir slots are not chronological, and a slot's content can change between
// reads within one flush interval -- read once per flush, after play, before reset.
//
// out[] = { lat_ms, err_udeg, range_units, view_mdps, los_mdps, weapon, flags, frame_ms }
static cell AMX_NATIVE_CALL dodx_get_trigger_press(AMX *amx, cell *params)
{
	int index = params[1];
	CHECK_PLAYER(index);

	CPlayer *pPlayer = GET_PLAYER_POINTER_I(index);
	if (!pPlayer->ingame || !pPlayer->pEdict || pPlayer->pEdict->free)
		return 0;

	const KTPTrigShooter &st = g_ktpTrig[index];

	// Same reoccupied-slot guard as the stats read; see the note there.
	if ((*g_engfuncs.pfnGetPlayerUserId)(pPlayer->pEdict) != st.userid)
		return 0;

	int slot = params[2];
	if (slot < 0 || slot >= st.keptCount)
		return 0;

	const KTPPressRec &r = st.kept[slot];
	cell *out = MF_GetAmxAddr(amx, params[3]);

	out[0] = r.latMs;
	out[1] = r.errUdeg;
	out[2] = r.rangeUnits;
	out[3] = r.viewMdps;
	out[4] = r.losMdps;
	out[5] = r.weapon;
	out[6] = r.flags;
	out[7] = r.frameMs;

	return 1;
}

// KTP: clear a player's counters after a successful flush. Separate from the read
// so a failed POST does not silently discard the records that justified it.
static cell AMX_NATIVE_CALL dodx_reset_trigger_stats(AMX *amx, cell *params)
{
	int index = params[1];
	CHECK_PLAYER(index);

	CPlayer *pPlayer = GET_PLAYER_POINTER_I(index);
	if (!pPlayer->ingame || !pPlayer->pEdict || pPlayer->pEdict->free)
		return 0;

	// Same reoccupied-slot guard as the reads -- a guard shared by N entry points
	// must hold at all N, or the asymmetry is where it rots. Wiping the previous
	// occupant's stale counters would be harmless, but refusing keeps the three
	// natives one behaviour.
	if ((*g_engfuncs.pfnGetPlayerUserId)(pPlayer->pEdict) != g_ktpTrig[index].userid)
		return 0;

	// ResetCounters, not Reset: the pending press and pair clocks straddling the
	// flush will be reported once they resolve, and wiping them would make the
	// "reported once, with its true extent" property above false.
	g_ktpTrig[index].ResetCounters();
	return 1;
}

static AMX_NATIVE_INFO KTPTrigger_Natives[] =
{
	{ "dodx_get_trigger_stats",   dodx_get_trigger_stats },
	{ "dodx_get_trigger_press",   dodx_get_trigger_press },
	{ "dodx_reset_trigger_stats", dodx_reset_trigger_stats },
	{ NULL, NULL }
};

#endif // KTP_TRIGGER_GEOM_H
