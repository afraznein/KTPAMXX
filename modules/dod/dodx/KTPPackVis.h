// KTP: aim-vs-transmission sampling (blind audit tier 2.7).
//
// SENSOR, NOT DETECTOR. Counters only: no threshold, no ratio, no conclusion is
// computed here, and none should be added -- this repo is public, and judgement
// lives in the private consumer.
//
// WHAT IS MEASURED. When a player's own aim trace lands on a live enemy, the
// sample asks one question of the pack recorder in moduleconfig.cpp: was that
// enemy present in ANY entity pack the server sent this player within a
// per-sample window? The window is the engine's default client interpolation
// depth plus the player's measured ping -- how far in the past that player's
// screen legitimately runs -- so it is a forgiveness bound, not a judgement.
// A single-frame check would fire hardest on peeking, the most common
// legitimate act in the game, and hardest on high-ping players; the per-player
// window exists to absorb exactly those.
//
// ONE DIRECTION ONLY. "Not packed within the window" means the server never
// transmitted that entity to that client in that span -- the client had nothing
// to render. "Packed" means NOTHING about visibility: PVS culling is leaf-based
// and generous, and the game DLL's own AddToFullPack (established by reading
// its binary, not assumed) keeps an entity packed for a short grace after its
// last passed visibility test, so entities are routinely packed while fully
// occluded. A consumer reading packed-within-window as "legitimately seen" is
// measuring map topology, not behaviour.
//
// UNKNOWN IS A THIRD STATE, NOT A DEFAULT. When recording has not yet covered a
// full window for both slots involved (fresh map, fresh connect, recorder
// inactive, clock boundary), the sample counts as unknown. Folding unknown into
// either side fabricates a rate; the counters keep it separate so no reader has
// to remember to.
#ifndef KTP_PACK_VIS_H
#define KTP_PACK_VIS_H

namespace ktppackvis
{
	// All counters saturate together: a consumer dividing one by another must
	// never see one wrap while the other holds. (Unreachable under nightly
	// restarts; the symmetry is the point.)
	inline void satAdd(int &c, int by)
	{
		if (c <= 2147483000 - by)
			c += by;
	}
}

struct KTPPackVis
{
	// Per-cmd latch, compared against KTPShotGeom::cmdSeq so at most one sample
	// is taken per usercmd however many player-hitting traces the cmd contains.
	unsigned int lastSampleSeq;

	int samplesKnown;     // recorder answered: packed-within-window or not
	int samplesUnpacked;  // subset of samplesKnown
	int samplesUnknown;   // recorder could not answer -- countable as NEITHER side
	int windowMsSum;      // windows used across known samples, saturating add
	int windowMsMax;      // widest window used since the last reset

	void reset()
	{
		lastSampleSeq = 0;
		samplesKnown = 0;
		samplesUnpacked = 0;
		samplesUnknown = 0;
		windowMsSum = 0;
		windowMsMax = 0;
	}
};

#endif // KTP_PACK_VIS_H
