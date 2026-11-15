// Exercises KTPApplyCurWeapon -- CurWeapon applied to the in-hand weapon and clip.
// Built and run by ci.yml twice: once with -DNEGATIVE_CONTROL, which must FAIL, and
// once without, which must pass.
//
// The negative control is the pre-fix handler: a CurWeapon that arrives while stats
// are paused is dropped whole. The pairs below are the three Lane B 11-12 kills whose
// hook read disagreed with the engine (held bar against garand and thompson, held mp40
// against mp44): a bot respawned into a new class during the round freeze, so its
// spawn CurWeapon was dropped and `current` stayed on the previous life's gun.
//
// The driver mirrors Client_CurWeapon's case 2 in modules/dod/dodx/usermsg.cpp.
#include <stdio.h>
#include <string.h>
#include "KTPCurWeapon.h"

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } } while (0)

// weaponData indices (modules/dod/dodx/Utils.cpp).
enum { W_GARAND = 5, W_THOMPSON = 7, W_MP44 = 8, W_BAR = 11, W_MP40 = 12, W_MG42 = 17,
       W_GARANDBUTT = 38 };

struct FakePlayer
{
	struct { int clip; int shots; } weapons[47];
	int current, old, totalShots;
	bool attack2;

	void saveShot(int weapon) { weapons[weapon].shots++; totalShots++; }
};

static bool paused = false;

// needcheck mirrors weaponData[].needcheck for the ids used here; refine mirrors
// get_weaponid's garand case (secondary attack is the butt).
static void CurWeapon(FakePlayer &p, int id, int clip)
{
#ifdef NEGATIVE_CONTROL
	if (paused)
		return;
#endif
	KTPApplyCurWeapon(p, id, clip, id == W_GARAND, !paused,
		[](FakePlayer &fp) { return fp.attack2 ? (int)W_GARANDBUTT : fp.current; });
}

static void Fresh(FakePlayer &p)
{
	memset(&p, 0, sizeof(p));
	paused = false;
}

// A life on `held`, death, a round freeze in which the bot respawns on `spawned`,
// then the freeze ends and the bot fires once.
static void RespawnDuringFreeze(int held, int heldClip, int spawned, int spawnClip)
{
	char msg[160];
	FakePlayer p;
	Fresh(p);

	CurWeapon(p, held, heldClip);
	CurWeapon(p, held, heldClip - 1);
	CHECK(p.weapons[held].shots == 1, "live shot on the held weapon is counted once");

	paused = true;
	CurWeapon(p, spawned, spawnClip);
	snprintf(msg, sizeof(msg), "spawn during the freeze moves current %d -> %d", held, spawned);
	CHECK(p.current == spawned, msg);
	CHECK(p.old == held, "old is the previous life's weapon, for the switch forward");
	CHECK(p.totalShots == 1, "nothing is counted while paused");

	paused = false;
	CurWeapon(p, spawned, spawnClip - 1);
	snprintf(msg, sizeof(msg), "first live shot after the freeze is counted on %d", spawned);
	CHECK(p.weapons[spawned].shots == 1, msg);
	CHECK(p.weapons[held].shots == 1, "no shot leaks onto the held weapon");
	CHECK(p.totalShots == 2, "two trigger pulls, two shots");
}

int main()
{
	RespawnDuringFreeze(W_BAR, 20, W_GARAND, 8);
	RespawnDuringFreeze(W_BAR, 20, W_THOMPSON, 30);
	RespawnDuringFreeze(W_MP40, 32, W_MP44, 30);

	FakePlayer p;

	// Shots fired while paused are not counted, and do not leave a stale clip that
	// double-counts or swallows the first live shot.
	Fresh(p);
	CurWeapon(p, W_MP44, 30);
	paused = true;
	CurWeapon(p, W_MP44, 29);
	CurWeapon(p, W_MP44, 28);
	CHECK(p.totalShots == 0, "paused shots are not counted");
	paused = false;
	CurWeapon(p, W_MP44, 27);
	CurWeapon(p, W_MP44, 27);
	CurWeapon(p, W_MP44, 26);
	CHECK(p.weapons[W_MP44].shots == 2, "two live shots after the pause, counted once each");

	// A reload during the pause: the first live shot compares against the full clip.
	Fresh(p);
	CurWeapon(p, W_THOMPSON, 3);
	paused = true;
	CurWeapon(p, W_THOMPSON, 30);
	paused = false;
	CurWeapon(p, W_THOMPSON, 29);
	CHECK(p.weapons[W_THOMPSON].shots == 1, "reload during the pause, first live shot counted");

	// Unpaused behaviour is unchanged: the needcheck refinement, the MG42 two-round
	// match, and a switch that is not a shot.
	Fresh(p);
	CurWeapon(p, W_GARAND, 8);
	p.attack2 = true;
	CurWeapon(p, W_GARAND, -1);
	CHECK(p.current == W_GARANDBUTT, "garand secondary attack refines to the butt");
	CHECK(p.totalShots == 0, "a melee CurWeapon with clip -1 is not a shot");

	Fresh(p);
	CurWeapon(p, W_MG42, 150);
	CurWeapon(p, W_MG42, 149);
	CHECK(p.weapons[W_MG42].shots == 0, "MG42 one-round drop is not a shot");
	CurWeapon(p, W_MG42, 147);
	CHECK(p.weapons[W_MG42].shots == 1, "MG42 two-round drop is one shot");

	Fresh(p);
	CurWeapon(p, W_BAR, 20);
	CurWeapon(p, W_MP40, 32);
	CurWeapon(p, W_BAR, 20);
	CHECK(p.totalShots == 0, "switching away and back is not a shot");
	CHECK(p.current == W_BAR && p.old == W_MP40, "current and old follow the switch");

	if (fails)
	{
		printf("%d check(s) failed\n", fails);
		return 1;
	}
	printf("all cur-weapon checks passed\n");
	return 0;
}
