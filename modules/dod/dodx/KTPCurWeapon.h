// KTP: one CurWeapon message applied to a player's in-hand weapon and clip.
//
// The in-hand weapon and the tracked clip follow every message, paused or not. Only
// the shot count waits for stats to be live. A spawn during the round freeze sends
// its CurWeapon while stats are paused, and dropping it left `current` on the last
// life's gun, so hits, damage and the frag-context clip went to a weapon the player
// no longer held.
//
// Templated on the player so scripts/test_cur_weapon.cpp can drive it with a fake.

#ifndef KTP_CUR_WEAPON_H
#define KTP_CUR_WEAPON_H

// The MG42 shot is matched two rounds down, as upstream DODX always has.
enum { KTP_CURWEAPON_MG42 = 17 };

template <typename Player, typename Refine>
inline void KTPApplyCurWeapon(Player &p, int iId, int iClip, bool needcheck, bool countShots,
	Refine refine)
{
	p.old = p.current;
	p.current = iId;

	if (needcheck)
	{
		iId = refine(p);
		p.current = iId;
	}

	if (countShots && iClip > -1)
	{
		int perShot = (p.current == KTP_CURWEAPON_MG42) ? 2 : 1;
		if (iClip + perShot == p.weapons[iId].clip)
			p.saveShot(iId);
	}

	// Tracked even while paused, so the first live shot compares against the clip the
	// player actually holds rather than one from before the pause.
	p.weapons[iId].clip = iClip;
}

#endif // KTP_CUR_WEAPON_H
