// KTP: Damage-hook kills held for the engine's weapon name.
//
// The victim's Health usually arrives before DeathMsg, so the Damage hook sees most
// kills first, when the only weapon it can read is the attacker's held one. That read
// can name a weapon the engine never logged (a switch, a respawn into a new class, a
// butt or bayonet hit), and HLStatsX joins the frag marker to the stock kill line on
// the weapon. So the hook's report waits here for DeathMsg, which carries the engine's
// own logname.
//
// Plain values in and out, no engine types, so scripts/test_kill_hold.cpp can drive it.

#ifndef KTP_KILL_HOLD_H
#define KTP_KILL_HOLD_H

struct KTPHeldKill
{
	bool active;
	int attacker;
	int weapon;
	int aim;
	int TA;
	float time;
};

class KTPKillHold
{
public:
	enum { kSlots = 33 };

	static bool validSlot(int victim) { return victim >= 1 && victim < kSlots; }

	bool holding(int victim) const { return validSlot(victim) && slot[victim].active; }

	// Overwrites: the caller must expire() the slot with force first, or a held kill is lost.
	void hold(int victim, int attacker, int weapon, int aim, int TA, float now)
	{
		if (!validSlot(victim))
			return;
		KTPHeldKill &k = slot[victim];
		k.active = true;
		k.attacker = attacker;
		k.weapon = weapon;
		k.aim = aim;
		k.TA = TA;
		k.time = now;
	}

	// DeathMsg for this victim. Its name is the engine's view of the killer it names,
	// so a different killer keeps the hook's read, and 0 is DODX's unknown bucket
	// rather than a weapon.
	bool resolve(int victim, int killer, int engineWeapon, KTPHeldKill &out)
	{
		if (!take(victim, out))
			return false;
		if (engineWeapon > 0 && killer == out.attacker)
			out.weapon = engineWeapon;
		return true;
	}

	// Hands back a kill whose DeathMsg can no longer be coming. One stamped later than
	// now predates a clock restart (map change) and is dropped, never handed back.
	bool expire(int victim, float now, bool force, float window, KTPHeldKill &out)
	{
		if (!holding(victim))
			return false;
		float age = now - slot[victim].time;
		if (age < 0.0f)
		{
			slot[victim].active = false;
			return false;
		}
		if (!force && age < window)
			return false;
		return take(victim, out);
	}

private:
	bool take(int victim, KTPHeldKill &out)
	{
		if (!holding(victim))
			return false;
		out = slot[victim];
		slot[victim].active = false;
		return true;
	}

	KTPHeldKill slot[kSlots];
};

#endif // KTP_KILL_HOLD_H
