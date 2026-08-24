// vim: set ts=4 sw=4 tw=99 noet:
//
// AMX Mod X, based on AMX Mod by Aleksander Naszko ("OLO").
// Copyright (C) The AMX Mod X Development Team.
// Copyright (C) 2004 Lukasz Wlasinski.
//
// This software is licensed under the GNU General Public License, version 3 or higher.
// Additional exceptions apply. For full license details, see LICENSE.txt or visit:
//     https://alliedmods.net/amxmodx-license

//
// DODX Module
//

#include "amxxmodule.h"
#include "dodx.h"
#include <string.h>

// KTP: Per-victim "client_death already fired this frame" gate.
// The Damage hook fires iFDeath when a tracked attacker damages a victim to <=0 HP.
// DeathMsg always fires (incl. suicide / world-kill / fall). We need DeathMsg to
// cover the no-Damage cases without double-firing for the normal kill flow.
static float g_lastDeathReportTime[33] = {0};

// KTP 2026-05-21: per-player observed deaths counter for the score/deaths
// offset validation gate. Ticks once per death event (any cause — frags,
// suicides, world damage, kill console command). Reset in player Init,
// Disconnect, and dodx_reset_all_stats (the Connect path is unreachable in
// extension mode). Exposed via dodx_get_observed_deaths native in NBase.cpp.
//
// NOT a replacement for life.deaths / round.deaths — those are stats counters
// driven through saveKill() and have different semantics (reset on round
// start, only count damage-flow deaths). This counter is purely for the
// pdata-offset validation gate.
int g_observedDeaths[33] = {0};

// Per-life gate for the counter above.
// INVARIANT: a victim dies at most once per life, so g_observedDeaths[i]
// increments at most once between consecutive life starts. The flag is set
// by the first counted death of a life and cleared only at points that
// begin a new life or a new tracking epoch: spawn (PStatus / ResetHUD),
// PreThink observing the player alive, CPlayer Init/Connect/Disconnect,
// and dodx_reset_all_stats. This makes the counter immune to the
// Damage-hook/DeathMsg double-report race regardless of timing; the 33ms
// g_lastDeathReportTime window above stays as the dedup for saveKill()
// and the iFDeath forward (HLStatsX kill-log semantics unchanged).
bool g_deathCountedThisLife[33] = {false};

// Count a death exactly once per life. Both death-report paths call this
// instead of touching g_observedDeaths directly.
static void countObservedDeath(int victimIdx)
{
	if (victimIdx < 1 || victimIdx >= 33)
		return;
	if (g_deathCountedThisLife[victimIdx])
		return;
	g_deathCountedThisLife[victimIdx] = true;
	g_observedDeaths[victimIdx]++;
}

void Client_ResetHUD_End(void* mValue)
{
	if (!mPlayer)
		return;
	mPlayer->clearStats = gpGlobals->time + 0.25f;
	// ResetHUD also marks a spawn — second re-arm point for the
	// observed-deaths per-life gate (see g_deathCountedThisLife).
	if (mPlayer->index >= 1 && mPlayer->index < 33)
		g_deathCountedThisLife[mPlayer->index] = false;
}

void Client_RoundState(void* mValue)
{
	if ( mPlayer ) return;

	// KTP: Safety check - gpGlobals must be valid
	if (!gpGlobals)
		return;

	int result = *(int*)mValue;
	if ( result == 1 )
	{
		// KTP: Use cached maxClients value for safety
		int maxClients = gpGlobals->maxClients;
		if (maxClients < 1 || maxClients > 32)
			return;  // Sanity check

		for (int i=1;i<=maxClients;i++)
		{
			CPlayer *pPlayer = GET_PLAYER_POINTER_I(i);
			if (pPlayer && pPlayer->ingame)
			{
				pPlayer->clearRound = gpGlobals->time + 0.25f;
			}
		}
	}
}

void Client_TeamScore(void* mValue)
{
	static int index;

	switch(mState++)
	{
	case 0:
		index = *(int*)mValue;
		break;
	case 1:
		switch (index)
		{
		case 1:
			AlliesScore = *(int*)mValue;
			break;
		case 2:
			AxisScore = *(int*)mValue;
			break;
		}
		break;
	}
}

void Client_ObjScore(void* mValue)
{
	static CPlayer *pPlayer;
	static int score;
	static int playerIdx;

	switch(mState++)
	{
	case 0:
		// KTP: Safety check - gpGlobals must be valid
		if (!gpGlobals)
		{
			pPlayer = NULL;
			break;
		}
		playerIdx = *(int*)mValue;
		if (playerIdx < 1 || playerIdx > gpGlobals->maxClients)
			pPlayer = NULL;
		else
			pPlayer = GET_PLAYER_POINTER_I(playerIdx);
		break;
	case 1:
		// KTP fix: revalidate player pointer - edict could have been freed
		// between case 0 and case 1 if a callback triggered during message dispatch
		if (!pPlayer || !pPlayer->ingame || !pPlayer->pEdict || pPlayer->pEdict->free)
			break;
		score = *(int*)mValue;
		if ( (pPlayer->lastScore = score - (int)(pPlayer->savedScore)) && isModuleActive() )
		{
			pPlayer->updateScore(pPlayer->current,pPlayer->lastScore);
			pPlayer->sendScore = gpGlobals->time + 0.25f;
			// KTP: Mark as pending CP resolution. ObjScore fires BEFORE SetObj in DoD,
			// so g_lastCapturedCP isn't set yet. The CP index will be resolved in
			// PreThink when sendScore fires (~0.2s later, after SetObj has arrived).
			pPlayer->lastScoreCP = -2;  // -2 = pending resolution
		}
		pPlayer->savedScore = score;
		break;
	}
}

void Client_CurWeapon(void* mValue)
{
	static int iState;
	static int iId;

	switch (mState++)
	{
		case 0:
			iState = *(int*)mValue;
			break;

		case 1:
			if (!iState)
				break;

			iId = *(int*)mValue;
			break;

		case 2:
			if(!iState || !isModuleActive() || !mPlayer)
				break;

			// KTP: Safety check - mPlayer->pEdict must be valid
			if (!mPlayer->pEdict || mPlayer->pEdict->free)
				break;

			// KTP: Bounds check - iId must be a valid weapon index
			if (iId < 0 || iId >= DODMAX_WEAPONS)
				break;

			int iClip = *(int*)mValue;

			mPlayer->old = mPlayer->current;
			mPlayer->current = iId;

			if(weaponData[iId].needcheck)
			{
				iId = get_weaponid(mPlayer);
				mPlayer->current = iId;
			}

			if(iClip > -1)
			{
				if(mPlayer->current == 17)
				{
					if(iClip+2 == mPlayer->weapons[iId].clip)
						mPlayer->saveShot(iId);
				}
				else
				{
					if (iClip+1 == mPlayer->weapons[iId].clip)
						mPlayer->saveShot(iId);
				}
			}

			mPlayer->weapons[iId].clip = iClip;
			mCurWpnEnd = 1;
			break;
	};
}

void Client_CurWeapon_End(void*)
{
	if(mCurWpnEnd == 1 && mPlayer && mPlayer->index && mPlayer->current && mPlayer->old && (mPlayer->current != mPlayer->old))
		MF_ExecuteForward(iFCurWpnForward, mPlayer->index, mPlayer->current, mPlayer->old);

	mCurWpnEnd = 0;
}


/*
Nie ma damage event ...
*/
void Client_Health_End(void* mValue)
{
    if ( !isModuleActive() )
		return;

	if (!mPlayer)
		return;

	// KTP: Check pEdict is valid before accessing
	if (!mPlayer->pEdict || mPlayer->pEdict->free)
		return;

	edict_t *enemy = mPlayer->pEdict->v.dmg_inflictor;
	int damage = (int)mPlayer->pEdict->v.dmg_take;

	// KTP: Check if we have valid damage and enemy pointer
	// NOTE: We do NOT check enemy->free here because grenade entities can be freed
	// after damaging the first victim but before processing subsequent victims.
	// We'll handle freed entities by falling back to self-damage below.
	if (!damage || !enemy)
		return;

	int weapon = 0;
	int aim = 0;

	mPlayer->pEdict->v.dmg_take = 0.0;

	CPlayer* pAttacker = NULL;

	// KTP: If enemy entity is freed, skip directly to grenade lookup
	// This happens when a grenade damages multiple victims - the entity
	// may be freed after the first victim but before subsequent ones
	if (enemy->free)
	{
		g_grenades.find(enemy, &pAttacker, weapon);
	}
	// KTP: Extra safety check for enemy edict validity before accessing flags
	else if((enemy->v.flags & (FL_CLIENT | FL_FAKECLIENT)))
	{
		// KTP: Validate enemy player index is in range
		int enemyIdx = ENTINDEX_SAFE(enemy);
		if (enemyIdx < 1 || enemyIdx > gpGlobals->maxClients)
		{
			pAttacker = mPlayer;  // Fall back to self-damage
		}
		else
		{
			pAttacker = GET_PLAYER_POINTER_I(enemyIdx);

			// KTP: Check attacker is valid and ingame
			if (!pAttacker->ingame || !pAttacker->pEdict || pAttacker->pEdict->free)
			{
				pAttacker = mPlayer;
			}
			else
			{
				weapon = pAttacker->current;

				// KTP: Bounds check for weapon array
				if (weapon >= 0 && weapon < DODMAX_WEAPONS)
				{
					if ( weaponData[weapon].needcheck )
						weapon = get_weaponid(pAttacker);

					aim = pAttacker->aiming;

					if ( weaponData[weapon].melee )
						pAttacker->saveShot(weapon);
				}
			}
		}
	}
	else
	{
		g_grenades.find(enemy, &pAttacker, weapon);
	}

	int TA = 0;

	if ( !pAttacker )
		pAttacker = mPlayer;

	if ( pAttacker->index != mPlayer->index )
	{
		// KTP: Check pEdict is valid before accessing for team comparison
		if ( pAttacker->pEdict && !pAttacker->pEdict->free &&
		     mPlayer->pEdict->v.team == pAttacker->pEdict->v.team )
			TA = 1;
	}

	// KTP: Fire pre-damage forward to allow damage modification
	// Plugins can return a modified damage value (lower to reduce, 0 to block)
	int effectiveDamage = damage;
	if (iFDamagePre >= 0)
	{
		cell result = MF_ExecuteForward(iFDamagePre, pAttacker->index, mPlayer->index, damage, weapon, aim, TA);

		// If plugin returned a different damage value, apply the reduction
		if (result >= 0 && result < damage)
		{
			int reduction = damage - result;
			effectiveDamage = result;

			// Heal the player by the reduction amount (effectively reducing the damage taken)
			// Only heal if player is still alive and reduction is meaningful
			if (mPlayer->IsAlive() && reduction > 0)
			{
				float newHealth = mPlayer->pEdict->v.health + (float)reduction;
				// Cap at max health (100 for DoD, could be higher with other mods)
				if (newHealth > 100.0f)
					newHealth = 100.0f;
				mPlayer->pEdict->v.health = newHealth;

				// KTP: Send Health message to update client HUD
				// The game already sent the Health message with the reduced value,
				// so we need to send another one with the correct value after heal-back
				// gmsgHealth is 0 until captured; type-0 is Sys_Error. Skipping
				// only loses the heal-back HUD correction, never the health change.
				if (gmsgHealth > 0)
				{
				MESSAGE_BEGIN(MSG_ONE, gmsgHealth, NULL, mPlayer->pEdict);
				WRITE_BYTE((int)newHealth);
				MESSAGE_END();
				}
			}
		}
	}

	// Use effective damage for stats tracking
	if ( pAttacker->index != mPlayer->index )
	{
		pAttacker->saveHit( mPlayer , weapon , effectiveDamage, aim );
	}

	MF_ExecuteForward( iFDamage, pAttacker->index, mPlayer->index, effectiveDamage, weapon, aim, TA );

	if ( !mPlayer->IsAlive() )
	{
		// Symmetric side of the DeathMsg dedup: if DeathMsg already reported
		// this victim's death inside the window, firing here again would
		// double-log the kill in HLStatsX. (The observed-deaths counter has
		// its own per-life gate and doesn't rely on this window.)
		// Negative delta = server time restarted (map change), not a dup.
		if (mPlayer->index >= 1 && mPlayer->index < 33 && gpGlobals)
		{
			float delta = gpGlobals->time - g_lastDeathReportTime[mPlayer->index];
			if (delta >= 0.0f && delta < 0.033f)
				return;
		}

		pAttacker->saveKill(mPlayer,weapon,( aim == 1 ) ? 1:0 ,TA);
		MF_ExecuteForward( iFDeath, pAttacker->index, mPlayer->index, weapon, aim, TA );
		if (mPlayer->index >= 1 && mPlayer->index < 33)
			g_lastDeathReportTime[mPlayer->index] = gpGlobals ? gpGlobals->time : 0.0f;
		countObservedDeath(mPlayer->index);
	}
}

// KTP: DoD's WeaponList carries no weapon-name string. Fields, read out of
// CBasePlayer::UpdateClientData in dod_i386.so md5 4f4727b2...: ammo1 index,
// max ammo1, ammo2 index, max ammo2, slot, position, SHORT weapon id, flags,
// clip. Field 0 is the DLL's own GetAmmoIndex(pszAmmo1) — this map's live
// m_rgAmmo slot — and field 6 is the DODW_* id.
void Client_WeaponList(void* mValue)
{
  static int iAmmoIndex = -1;

  int field = mState++;

  // mState is only ours if DODX_OnMsgBegin actually ran for this message; when
  // it bails the field counter is the previous message's, and misreading it here
  // would file a bogus slot for a bogus weapon.
  if (!g_bServerActive)
    return;

  switch (field)
  {
  case 0:
    iAmmoIndex = *(int*)mValue;
    break;

  case 6:
    {
      int wpnId = *(int*)mValue;
      if (wpnId > 0 && wpnId < DODMAX_WEAPONS &&
          iAmmoIndex >= 0 && iAmmoIndex < DODX_MAX_AMMO_SLOTS)
      {
        g_ammoIndexByWeapon[wpnId] = iAmmoIndex;
        DODX_CheckAmmoIndexDrift(wpnId, iAmmoIndex);
      }
      // Consumed: a later id must not pair with this weapon's index.
      iAmmoIndex = -1;
    }
    break;
  }
}

void Client_AmmoX(void* mValue)
{
  static int iAmmo;

  switch (mState++)
  {
  case 0:
    iAmmo = *(int*)mValue;
    break;
  case 1:
	if (!mPlayer ) 
		break;
    for(int i = 1; i < DODMAX_WEAPONS ; ++i)
	{
      if (iAmmo == weaponData[i].ammoSlot)
        mPlayer->weapons[i].ammo = *(int*)mValue;
	}
  }
}

void Client_AmmoShort(void* mValue)
{
  static int iAmmo;

  switch (mState++)
  {
  case 0:
    iAmmo = *(int*)mValue;
    break;

  case 1:
	if(!mPlayer ) 
		break;

    for(int i = 1; i < DODMAX_WEAPONS ; ++i) 
	{
      if (iAmmo == weaponData[i].ammoSlot)
		  mPlayer->weapons[i].ammo = *(int*)mValue;
	}
  }
}

// Called with a value of 90 at start 20 when someone scopes in and 0 when they scope out
void Client_SetFOV(void* mValue)
{
	if(!mPlayer)
		return;

	mPlayer->Scoping(*(int*)mValue);
}

void Client_SetFOV_End(void* mValue)
{
	if(!mPlayer)
		return;

	mPlayer->ScopingCheck();
}

void Client_Object(void* mValue)
{
	if(!mPlayer)
		return;

	// KTP: Check pEdict is valid before accessing
	if (!mPlayer->pEdict || mPlayer->pEdict->free)
		return;

	// First need to find out what was picked up
	const char *classname;
	edict_t* pObject = NULL;

	//const char* value;

	//if(mValue)
	//{
	//	value = (char*)mValue;
	//}

	if(!mPlayer->object.carrying)
	{
		// We grab the first object within the sphere of our player
		pObject = FindEntityInSphere(mPlayer->pEdict, mPlayer->pEdict, 50.0);

		// The loop through all the objects within the sphere
		while(pObject && !FNullEnt(pObject))
		{
			classname = STRING(pObject->v.classname);

			if(strcmp(classname, "dod_object") == 0)
			{
				mPlayer->object.pEdict = pObject;
				mPlayer->object.do_forward = true;
				return;
			}

			pObject = FindEntityInSphere(pObject, mPlayer->pEdict, 50.0);
		}
	}

	else
	{
		mPlayer->object.do_forward = true;
	}
}

void Client_Object_End(void* mValue)
{
	if(!mPlayer)
		return;

	float fposition[3];

	if(mPlayer->object.do_forward)
	{
		mPlayer->object.do_forward = (mPlayer->object.do_forward) ? false : true;
		mPlayer->object.carrying = (mPlayer->object.carrying) ? false : true;

		if (mPlayer->object.pEdict && !FNullEnt(mPlayer->object.pEdict))
		{
			mPlayer->object.pEdict->v.origin.CopyToArray(fposition);
			cell position[3];
			position[0] = amx_ftoc(fposition[0]);
			position[1] = amx_ftoc(fposition[1]);
			position[2] = amx_ftoc(fposition[2]);
			cell pos = MF_PrepareCellArray(position, 3);
			// KTP: Use ENTINDEX_SAFE to avoid crash in extension mode
			MF_ExecuteForward(iFObjectTouched, mPlayer->index, ENTINDEX_SAFE(mPlayer->object.pEdict), pos, mPlayer->object.carrying);
		}

		if(!mPlayer->object.carrying)
			mPlayer->object.pEdict = NULL;
	}
}

// KTP: Control Point tracking - ported from dodfun module
// Parses InitObj message containing all CP data for the map.
//
// Two modes of operation, decided in case 0:
//   1. mObjects.count == 0 — Metamod path. Use InitObj as the sole source.
//   2. mObjects.count > 0  — Extension-mode path. Entity scan ran first and
//      populated mObjects (and resolved pAreaEdict pairings via the lazy
//      GET_CAPTURE_AREA macro). InitObj from the DLL carries the *correct*
//      cp ordering — entity-scan order isn't guaranteed to match SetObj id.
//      First matching InitObj (newCount == mObjects.count) reorders mObjects
//      to DLL order while preserving each CP's resolved pAreaEdict, then
//      re-fires iFInitCP so the SMA rebuilds its name cache in the new order.
static objinfo_t s_initObjScanSnapshot[12];  // Saved entity-scan entries for area pairing
static int s_initObjScanSnapshotCount = 0;
static bool s_initObjReorderMode = false;    // True while consuming InitObj to reorder

void Client_InitObj(void* mValue)
{
	static int num;

	// No mDest filter — accept InitObj from any source:
	// MSG_BROADCAST (0) during ServerActivate, MSG_ONE (1) on player connect,
	// MSG_ALL (2) in some configurations. InitObj is global CP data that
	// doesn't depend on the target player context.

	switch (mState++)
	{
	case 0:
		num = 0;
		s_initObjReorderMode = false;
		{
			int newCount = *(int*)mValue;
#ifdef DODX_DEBUG_CP_INIT
			MF_Log("[DODX] InitObj case 0: newCount=%d mObjects.count=%d finalized=%d",
				newCount, mObjects.count, g_cpOrderingFinalized ? 1 : 0);
#endif

			if (mObjects.count > 0)
			{
				// Already finalized OR partial/stale message — skip.
				if (g_cpOrderingFinalized || newCount != mObjects.count)
				{
					MF_Log("[DODX] InitObj: skipped (finalized=%d, newCount=%d, existing=%d)",
						g_cpOrderingFinalized ? 1 : 0, newCount, mObjects.count);
					mState = 999;
					return;
				}

				// Snapshot entity-scan results so we can rebuild pAreaEdict
				// pairings after reordering.
				s_initObjScanSnapshotCount = mObjects.count;
				for (int i = 0; i < s_initObjScanSnapshotCount && i < 12; i++)
					s_initObjScanSnapshot[i] = mObjects.obj[i];
				s_initObjReorderMode = true;
				mObjects.Clear();
			}

			mObjects.count = newCount;
		}
		if (mObjects.count == 0)
			mObjects.Clear();
		if (mObjects.count > 12)
			mObjects.count = 12;
		break;
	case 1:
		if (num < 12)
			mObjects.obj[num].pEdict = GETEDICT(*(int*)mValue);
		break;
	case 2:
		if (num < 12)
			mObjects.obj[num].index = *(int*)mValue;
		break;
	case 3:
		if (num < 12)
		{
			// KTP: field 3 is the LIVE owner, not the default. dodfun's
			// CObjective::InitObj (CMisc.cpp) writes obj[i].owner here, and its
			// nine WRITE_* calls line up one-for-one with cases 0-9 below; this
			// was the only field whose name disagreed, and the name was wrong.
			//
			// Writing it into default_owner destroys the BSP seed that
			// DODX_InitCPFromEntities() applies at ServerActivate. That used to
			// be invisible because the entity scan runs AFTER the map-load
			// InitObj and re-Clear()s everything, so the bad write was always
			// overwritten a moment later. It only bites on a LATER InitObj —
			// reorder or refresh — and the scan never runs again, so from that
			// point default_owner reports whoever happens to hold the flag.
			//
			// A corrupted default_owner fails silently rather than loudly —
			// downstream readers take it at face value and never cross-check it.
			//
			// A FIRST parse still seeds default_owner from it because mObjects was
			// empty and nothing better exists yet — NOT because live equals default
			// here, which a pdata read on jagd contradicts. Also keeps non-extension
			// mode
			// working, where DODX_InitCPFromEntities() returns at the
			// g_bExtensionMode gate and never provides a seed at all.
			mObjects.obj[num].owner = *(int*)mValue;
			if (!s_initObjReorderMode)
				mObjects.obj[num].default_owner = mObjects.obj[num].owner;
		}
		break;
	case 4:
		if (num < 12)
			mObjects.obj[num].visible = *(int*)mValue;
		break;
	case 5:
		if (num < 12)
			mObjects.obj[num].icon_neutral = *(int*)mValue;
		break;
	case 6:
		if (num < 12)
			mObjects.obj[num].icon_allies = *(int*)mValue;
		break;
	case 7:
		if (num < 12)
			mObjects.obj[num].icon_axis = *(int*)mValue;
		break;
	case 8:
		if (num < 12)
			mObjects.obj[num].origin_x = *(float*)mValue;
		break;
	case 9:
		if (num < 12)
			mObjects.obj[num].origin_y = *(float*)mValue;
		mState = 1;
		num++;
		if (num == mObjects.count)
		{
			if (s_initObjReorderMode)
			{
				// Carry forward each CP's pAreaEdict pairing from the snapshot
				// (matched by edict pointer — the only stable identifier).
				//
				// KTP: default_owner rides along for the same reason. A reorder
				// Clear()s mObjects, and the message carries no default — field 3
				// is the live owner (see case 3). The snapshot holds the BSP seed
				// written by DODX_InitCPFromEntities(), which never runs again,
				// so without this a reorder zeroes every CP's default for the
				// rest of the map.
				for (int i = 0; i < mObjects.count; i++)
				{
					for (int j = 0; j < s_initObjScanSnapshotCount; j++)
					{
						if (mObjects.obj[i].pEdict &&
						    mObjects.obj[i].pEdict == s_initObjScanSnapshot[j].pEdict)
						{
							mObjects.obj[i].pAreaEdict    = s_initObjScanSnapshot[j].pAreaEdict;
							mObjects.obj[i].areaflags     = s_initObjScanSnapshot[j].areaflags;
							mObjects.obj[i].default_owner = s_initObjScanSnapshot[j].default_owner;
							break;
						}
					}
				}

				g_cpOrderingFinalized = true;
				s_initObjReorderMode = false;
				MF_Log("[DODX] InitObj: reordered %d CPs to DLL order", mObjects.count);
#ifdef DODX_DEBUG_CP_INIT
				for (int i = 0; i < mObjects.count; i++)
				{
					edict_t *pe = mObjects.obj[i].pEdict;
					const char *tn = pe ? STRING(pe->v.targetname) : "?";
					MF_Log("[DODX]   CP[%d] index=%d owner=%d targetname='%s'",
						i, mObjects.obj[i].index, mObjects.obj[i].owner, tn);
				}
#endif
			}
			else
			{
				MF_Log("[DODX] InitObj: parsed %d control points from message", mObjects.count);
			}

			if (iFInitCP >= 0)
				MF_ExecuteForward(iFInitCP);
		}
		break;
	}
}

// KTP: Control Point ownership change - ported from dodfun module
// Parses SetObj message (cp_index, new_owner, unused_byte)
void Client_SetObj(void* mValue)
{
	static int id;

	switch (mState++)
	{
	case 0:
		id = *(int*)mValue;
		break;
	case 1:
	{
		int newOwner = *(int*)mValue;
		if (id >= 0 && id < mObjects.count)
		{
			int oldOwner = mObjects.obj[id].owner;
			mObjects.obj[id].owner = newOwner;
			// Track last CP capture for ObjScore correlation
			if (newOwner != oldOwner)
			{
				g_lastCapturedCP = id;
				g_lastCapturedTime = gpGlobals->time;
				if (iFCPCaptured >= 0)
					MF_ExecuteForward(iFCPCaptured, id, newOwner, oldOwner);
			}
		}
		break;
	}
	}
}

// This seems to be only called when the player spawns
void Client_PStatus(void* mValue)
{
	switch(mState++)
	{
		case 0:
		{
			// KTP: Safety check - gpGlobals must be valid
			if (!gpGlobals)
				break;

			int playerIdx = *(int*)mValue;

			// KTP: Safety check - validate maxClients before using
			int maxClients = gpGlobals->maxClients;
			if (maxClients < 1 || maxClients > 32)
				break;

			if (playerIdx >= 1 && playerIdx <= maxClients)
			{
				// New life begins — re-arm the observed-deaths per-life gate.
				// PStatus is not stats-pause gated, so this stays reliable
				// through round-freeze pauses (PreThink does not run then).
				if (playerIdx < 33)
					g_deathCountedThisLife[playerIdx] = false;
				MF_ExecuteForward(iFSpawnForward, playerIdx);
			}
			break;
		}
	}
}

// KTP: DeathMsg user message handler.
// Format: BYTE killer_index, BYTE victim_index, STRING weapon_logname.
// DeathMsg fires for every death — including suicides via "kill" console,
// world deaths (fall, drown, kill triggers) where no Damage message is sent.
// Damage hook already fires iFDeath for normal kills, so dedup against
// g_lastDeathReportTime to avoid double-firing for the same death.
void Client_DeathMsg(void* mValue)
{
	static int killerIdx;
	static int victimIdx;

	switch (mState++)
	{
	case 0:
		killerIdx = *(int*)mValue;
		break;
	case 1:
		victimIdx = *(int*)mValue;
		break;
	case 2:
	{
		const char *weaponName = (const char *)mValue;
		if (!gpGlobals) break;

		int maxClients = gpGlobals->maxClients;
		if (maxClients < 1 || maxClients > 32) break;

		if (victimIdx < 1 || victimIdx > maxClients) break;

		// Dedup: skip if Damage hook already fired iFDeath for this victim
		// in the current or last few frames. Damage and DeathMsg normally ship
		// in the same server frame (both generated during the same SV_RunCmd
		// pass), so well under 1ms apart. A 33ms window (~4 frames at
		// sv_maxupdaterate 120) is comfortably beyond expected engine timing
		// jitter while minimizing the chance of suppressing a legitimate
		// quick re-death.
		//
		// KNOWN LIMITATION: If a damage message races the DeathMsg by >33ms
		// (e.g. under a server FPS dip), DeathMsg fires first with TA=0 and
		// the Damage hook's subsequent fire gets suppressed by this guard.
		// A real teamkill via a world-damage source (grenade, trigger_hurt)
		// could then be logged as non-TK in HLStatsX. Narrow edge case —
		// the Damage hook owns real-time TK detection for the >99% normal
		// kill path. Revisit if stats regressions surface.
		float now = gpGlobals->time;
		// Negative delta = server time restarted (map change), not a dup.
		float delta = now - g_lastDeathReportTime[victimIdx];
		if (delta >= 0.0f && delta < 0.033f)
			break;

		// Resolve weapon name to wpnindex (matches xmod_get_wpnlogname behavior).
		int weapon = 0;
		if (weaponName && weaponName[0])
		{
			for (int i = 0; i < DODMAX_WEAPONS; i++)
			{
				if (strcmp(weaponName, weaponData[i].logname) == 0)
				{
					weapon = i;
					break;
				}
			}
		}

		// killer 0 (world) is left as-is; the SMA already remaps killer<1 to victim
		// to mark it as a suicide.
		if (killerIdx < 0) killerIdx = 0;
		if (killerIdx > maxClients) killerIdx = 0;

		int aim = 0;   // No hitplace info on DeathMsg
		int TA = 0;    // Teamkill flag — Damage hook owns the proper detection
		MF_ExecuteForward(iFDeath, killerIdx, victimIdx, weapon, aim, TA);
		g_lastDeathReportTime[victimIdx] = now;
		countObservedDeath(victimIdx);
		break;
	}
	}
}

