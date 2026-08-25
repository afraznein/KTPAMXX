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
#include "CMisc.h"
#include "dodx.h"

// *****************************************************
// class CPlayer
// *****************************************************

void CPlayer::Disconnect()
{
	ingame = false;
	bot = false;
	savedScore = 0;

	// Cleared HERE and not only in Init() for the same reason as the death counter
	// below: Init() is skipped for a slot that already has a pEdict, so a mid-map
	// substitute would otherwise inherit the leaver's fire windows and be measured
	// on someone else's aim.
	ktpAim.Reset();

	// Zero the offset-validation death counter so a mid-map substitute
	// joining this recycled slot doesn't inherit the leaver's tally (Init()
	// is skipped for slots that already have a pEdict). Safe ordering: the
	// drop-client hook runs the chain first, so the plugin's disconnect-save
	// has already read the counter by the time this POST cleanup runs.
	extern int g_observedDeaths[33];
	extern bool g_deathCountedThisLife[33];
	if (index >= 1 && index < 33)
	{
		g_observedDeaths[index] = 0;
		g_deathCountedThisLife[index] = false;
	}

	oldteam = 0;
	oldclass = 0;
	oldprone = 0;
	oldstamina = 0.0f;

	// Model Stuff
	sModel.is_model_set = false;
	sModel.body_num = 0;

	// Object stuff
	object.pEdict = NULL;
	object.type = 0;
	object.carrying = false;
	object.do_forward = false;

	// KTP fix: check edict validity before accessing - can be freed during crash/map-change
	if (!pEdict || pEdict->free)
		return;

	if ( ignoreBots(pEdict) || !isModuleActive() ) // ignore if he is bot and bots rank is disabled or module is paused
		return;

	// KTP: rank may be NULL in extension mode
	if (rank)
		rank->updatePosition( &life );

}

void CPlayer::PutInServer(){

	ingame = true;

	if ( ignoreBots(pEdict) )
		return;

	restartStats();

	// KTP: In extension mode, skip rank system lookup since we don't load it
	// The rank system is optional; HLStatsX logging works without it
	if (MF_IsExtensionMode && MF_IsExtensionMode())
	{
		// Skip rank lookup - leave rank as NULL
		// ingame stays true, stats tracking will work via messages
		return;
	}

	const char* unique;
	const char* name = STRING(pEdict->v.netname);
	bool isip = false;
	switch((int)dodstats_rank->value) {
	case 1:
		if ( (unique = GETPLAYERAUTHID(pEdict)) == 0 )
			unique = name; // failed to get authid
		break;
	case 2:
		unique = ip;
		isip = true;
		break;
	default:
		unique = name;
	}
	if ( ( rank = g_rank.findEntryInRank( unique , name , isip) ) == 0 )
		ingame = false;
}

void CPlayer::Connect(const char* nn,const char* ippp ){
	bot = IsBot();
	strncpy(ip, ippp, sizeof(ip) - 1);
	ip[sizeof(ip) - 1] = '\0';
	// Strip the port from the ip
	for (size_t i = 0; i < sizeof(ip); i++)
	{
		if (ip[i] == ':')
		{
			ip[i] = '\0';
			break;
		}
	}

	// KTP 2026-05-21: reset the offset-validation gate's observed-death
	// counter for this slot so a reconnect doesn't inherit the prior
	// session's death tally.
	extern int g_observedDeaths[33];
	extern bool g_deathCountedThisLife[33];
	if (index >= 1 && index < 33)
	{
		g_observedDeaths[index] = 0;
		g_deathCountedThisLife[index] = false;
	}
}

void CPlayer::restartStats(bool all)
{
	if ( all )
	{
		memset(&weapons,0,sizeof(weapons));
		memset(static_cast<void *>(&round),0,sizeof(round));
		memset(&weaponsRnd,0,sizeof(weaponsRnd));
	}

	memset(&weaponsLife,0,sizeof(weaponsLife));   //DEC-Weapon (Round) stats
	memset(&attackers,0,sizeof(attackers));
	memset(&victims,0,sizeof(victims));
	life = {};
}

void CPlayer::Init( int pi, edict_t* pe )
{
	ktpAim.Reset();
	aiming = 0;
	wpnModel = 0;
	wpnscount = 0;
	lastScore = 0;
	sendScore = 0;
	lastScoreCP = -1;
	clearRound = 0.0f;
    pEdict = pe;
    index = pi;
	current = 0;
	clearStats = 0.0f;
	ingame =  false;
	bot = false;
	savedScore = 0;
	oldteam = 0;
	oldclass = 0;
	oldprone = 0;
	oldstamina = 0.0f;

	do_scoped = false;
	is_scoped = false;

	// Model Stuff
	sModel.is_model_set = false;
	sModel.body_num = 0;

	// Object stuff
	object.pEdict = NULL;
	object.type = 0;
	object.carrying = false;
	object.do_forward = false;

	// Zero the offset-validation death counter here too: Connect() is
	// unreachable in extension mode, and scoreboard pdata m_iDeaths zeroes
	// every map load — without this the counter is monotonic per process.
	extern int g_observedDeaths[33];
	extern bool g_deathCountedThisLife[33];
	if (index >= 1 && index < 33)
	{
		g_observedDeaths[index] = 0;
		g_deathCountedThisLife[index] = false;
	}
}

void CPlayer::saveKill(CPlayer* pVictim, int wweapon, int hhs, int ttk){

	if ( ignoreBots(pEdict,pVictim->pEdict) )
		return;

	if ( wweapon < 0 || wweapon >= DODMAX_WEAPONS )
		wweapon = 0;

	if ( pVictim->index == index )
	{ // killed self
		pVictim->weapons[0].deaths++;
		pVictim->life.deaths++;
		pVictim->round.deaths++;
		pVictim->weaponsLife[0].deaths++;       // DEC-Weapon (life) stats
		pVictim->weaponsRnd[0].deaths++;       // DEC-Weapon (round) stats
		return;
	}

	int vw = get_weaponid(pVictim);

	pVictim->attackers[index].name = (char*)weaponData[wweapon].name;
	pVictim->attackers[index].kills++;
	pVictim->attackers[index].hs += hhs;
	pVictim->attackers[index].tks += ttk;
	pVictim->attackers[0].kills++;
	pVictim->attackers[0].hs += hhs;
	pVictim->attackers[0].tks += ttk;
	pVictim->weapons[vw].deaths++;
	pVictim->weapons[0].deaths++;
	pVictim->life.deaths++;
	pVictim->round.deaths++;


	pVictim->weaponsLife[vw].deaths++; // DEC-Weapon (life) stats
	pVictim->weaponsLife[0].deaths++;                // DEC-Weapon (life) stats
	pVictim->weaponsRnd[vw].deaths++; // DEC-Weapon (round) stats
	pVictim->weaponsRnd[0].deaths++;                // DEC-Weapon (round) stats

	int vi = pVictim->index;
	victims[vi].name = (char*)weaponData[wweapon].name;
	victims[vi].deaths++;
	victims[vi].hs += hhs;
	victims[vi].tks += ttk;
	victims[0].deaths++;
	victims[0].hs += hhs;
	victims[0].tks += ttk;


	weaponsLife[wweapon].kills++;                // DEC-Weapon (life) stats
	weaponsLife[wweapon].hs += hhs;         // DEC-Weapon (life) stats
	weaponsLife[wweapon].tks += ttk;     // DEC-Weapon (life) stats
	weaponsLife[0].kills++;                     // DEC-Weapon (life) stats
	weaponsLife[0].hs += hhs;              // DEC-Weapon (life) stats
	weaponsLife[0].tks += ttk;          // DEC-Weapon (life) stats

	weaponsRnd[wweapon].kills++;                // DEC-Weapon (round) stats
	weaponsRnd[wweapon].hs += hhs;         // DEC-Weapon (round) stats
	weaponsRnd[wweapon].tks += ttk;     // DEC-Weapon (round) stats
	weaponsRnd[0].kills++;                     // DEC-Weapon (round) stats
	weaponsRnd[0].hs += hhs;              // DEC-Weapon (round) stats
	weaponsRnd[0].tks += ttk;          // DEC-Weapon (round) stats

	weapons[wweapon].kills++;
	weapons[wweapon].hs += hhs;
	weapons[wweapon].tks += ttk;
	weapons[0].kills++;
	weapons[0].hs += hhs;
	weapons[0].tks += ttk;
	life.kills++;
	life.hs += hhs;
	life.tks += ttk;
	round.kills++;
	round.hs += hhs;
	round.tks += ttk;
}

void CPlayer::saveHit(CPlayer* pVictim, int wweapon, int ddamage, int bbody){

	if ( ignoreBots(pEdict,pVictim->pEdict) )
		return;

	if ( wweapon < 0 || wweapon >= DODMAX_WEAPONS )
		wweapon = 0;

	if ( bbody < 0 || bbody >= 8 )
		bbody = 0;

	pVictim->attackers[index].hits++;
	pVictim->attackers[index].damage += ddamage;
	pVictim->attackers[index].bodyHits[bbody]++;
	pVictim->attackers[0].hits++;
	pVictim->attackers[0].damage += ddamage;
	pVictim->attackers[0].bodyHits[bbody]++;

	int vi = pVictim->index;
	victims[vi].hits++;
	victims[vi].damage += ddamage;
	victims[vi].bodyHits[bbody]++;
	victims[0].hits++;
	victims[0].damage += ddamage;
	victims[0].bodyHits[bbody]++;

	weaponsLife[wweapon].hits++;              // DEC-Weapon (life) stats
	weaponsLife[wweapon].damage += ddamage;    // DEC-Weapon (life) stats
	weaponsLife[wweapon].bodyHits[bbody]++;   // DEC-Weapon (life) stats
	weaponsLife[0].hits++;                   // DEC-Weapon (life) stats
	weaponsLife[0].damage += ddamage;         // DEC-Weapon (life) stats
	weaponsLife[0].bodyHits[bbody]++;        // DEC-Weapon (life) stats

	weaponsRnd[wweapon].hits++;              // DEC-Weapon (round) stats
	weaponsRnd[wweapon].damage += ddamage;    // DEC-Weapon (round) stats
	weaponsRnd[wweapon].bodyHits[bbody]++;   // DEC-Weapon (round) stats
	weaponsRnd[0].hits++;                   // DEC-Weapon (round) stats
	weaponsRnd[0].damage += ddamage;         // DEC-Weapon (round) stats
	weaponsRnd[0].bodyHits[bbody]++;        // DEC-Weapon (round) stats

	weapons[wweapon].hits++;
	weapons[wweapon].damage += ddamage;
	weapons[wweapon].bodyHits[bbody]++;
	weapons[0].hits++;
	weapons[0].damage += ddamage;
	weapons[0].bodyHits[bbody]++;

	life.hits++;
	life.damage += ddamage;
	life.bodyHits[bbody]++;

	round.hits++;
	round.damage += ddamage;
	round.bodyHits[bbody]++;
}

void CPlayer::saveShot(int weapon)
{

	if ( ignoreBots(pEdict) )
		return;

	// Per-fire actuation clock. saveShot is the chokepoint for clip-decrement AND the
	// hitscan-trace/grenade/rocket/melee-gated paths (8+ callers), so this also fires for
	// grenades/rockets/melee — correct for an actuation/input-multiplication clock; a
	// firearm-only consumer must filter by weapon id.
	if ( iFWeaponFire != -1 )
		MF_ExecuteForward(iFWeaponFire, index, weapon, amx_ftoc(gpGlobals->time));

	victims[0].shots++;
	weapons[weapon].shots++;
	weapons[0].shots++;
	life.shots++;
	round.shots++;
	weaponsLife[weapon].shots++;       // DEC-Weapon (life) stats
	weaponsLife[0].shots++;            // DEC-Weapon (life) stats

	weaponsRnd[weapon].shots++;       // DEC-Weapon (round) stats
	weaponsRnd[0].shots++;            // DEC-Weapon (round) stats
}

void CPlayer::updateScore(int weapon, int score)
{

	if ( ignoreBots(pEdict) )
		return;

	life.points += score;
	round.points += score;
	weaponsLife[weapon].points += score;
	weaponsLife[0].points += score;
	weaponsRnd[weapon].points += score;
	weaponsRnd[0].points += score;
	weapons[weapon].points += score;
	weapons[0].points += score;
}

void CPlayer::killPlayer()
{
	// KTP: Safety check - pEdict must be valid before any access
	if (!pEdict || pEdict->free)
		return;

	pEdict->v.dmg_inflictor = NULL;
	pEdict->v.health = 0;
	pEdict->v.deadflag = DEAD_RESPAWNABLE;
	pEdict->v.weaponmodel = 0;
	pEdict->v.weapons = 0;
}

void CPlayer::initModel(char* model)
{
	strncpy(sModel.modelclass, (const char*)model, sizeof(sModel.modelclass) - 1);
	sModel.modelclass[sizeof(sModel.modelclass) - 1] = '\0';
	sModel.is_model_set = true;
}

void CPlayer::clearModel()
{
	sModel.is_model_set = false;
}

bool CPlayer::setModel()
{
	if(!ingame || ignoreBots(pEdict))
		return false;

	if(sModel.is_model_set)
	{
		ENTITY_SET_KEYVALUE(pEdict, "model", sModel.modelclass);
		pEdict->v.body = sModel.body_num;
		return true;
	}

	return false;
}

void CPlayer::setBody(int bn)
{
	if(!ingame || ignoreBots(pEdict))
		return;

	sModel.body_num = bn;

	return;
}

/*
	iuser3 = 0 standing up
	iuser3 = 1 going prone or mg tearing down
	iuser3 = 2 setting up mg while laying down
*/
void CPlayer::PreThink()
{
	// KTP: Safety check - pEdict must be valid before any access
	if (!pEdict || pEdict->free)
		return;

	// Player observed alive = current life's death can't have been counted
	// yet. Backstop for the observed-deaths per-life gate in case a spawn
	// message was missed (PStatus/ResetHUD are the primary re-arm points).
	{
		extern bool g_deathCountedThisLife[33];
		if (index >= 1 && index < 33 && g_deathCountedThisLife[index] && IsAlive())
			g_deathCountedThisLife[index] = false;
	}

	if(!ingame || ignoreBots(pEdict))
		return;

	if(oldteam != pEdict->v.team && iFTeamForward != -1)
		MF_ExecuteForward(iFTeamForward, index, pEdict->v.team, oldteam);

	if(oldclass != pEdict->v.playerclass && iFClassForward != -1)
		MF_ExecuteForward(iFClassForward, index, pEdict->v.playerclass, oldclass);

	if(oldprone != pEdict->v.iuser3 && oldprone != 2 && pEdict->v.iuser3 != 2 && iFProneForward != -1)
		MF_ExecuteForward(iFProneForward, index, pEdict->v.iuser3);

	if(oldstamina > pEdict->v.fuser4 && iFStaminaForward != -1)
		MF_ExecuteForward(iFStaminaForward, index, ((int)pEdict->v.fuser4));

	if(wpns_bitfield != pEdict->v.weapons)
		WeaponsCheck(pEdict->v.weapons & ~(1<<31));

	// Set the old variables for 
	oldprone = pEdict->v.iuser3;
	oldteam = pEdict->v.team;
	oldclass = pEdict->v.playerclass;
	oldstamina = pEdict->v.fuser4;

	wpns_bitfield = pEdict->v.weapons & ~(1<<31);

	// Don't add button-based shot detection here. The CurWeapon handler
	// (usermsg.cpp) tracks shots via clip decrement and is authoritative in both
	// modes; a second path double-counts every shot and inflates accuracy.
}

void CPlayer::Scoping(int value)
{
	// Everyone gets a 0 then another call for 90, so I need to figure out
	// what weapon they have before I can then check if they are scoped or not

	do_scoped = false;

	switch(value)
	{
	// This is when the scope is dropped from the eye
	case 0:
		// Is this an initial call
		if(this->current == 0)
			return;

		//			SKar					Spring					SFG42						SEnfield
		if((this->current == 6 || this->current == 9 || this->current == 32 || this->current == 35) && is_scoped)
		{
			is_scoped = false;
			do_scoped = true;
		}

		break;

	// This is when the scope is put up to the eye
	case 20:
		//			SKar					Spring					SFG42						SEnfield
		if((this->current == 6 || this->current == 9 || this->current == 32 || this->current == 35) && !is_scoped)
		{
			is_scoped = true;
			do_scoped = true;
		}

		break;

	// This means the scope has been initialized
	case 90:
		is_scoped = false;
		return;
	};
}

void CPlayer::ScopingCheck()
{
	if(do_scoped)
		MF_ExecuteForward(iFScopeForward, index, (int)is_scoped);
}

void CPlayer::WeaponsCheck(int weapons)
{
	if(wpns_bitfield == 0)
		return;

	// KTP: Safety check - pEdict must be valid before any access
	if (!pEdict || pEdict->free)
		return;

	if(pEdict->v.weapons == 0)
		return;

	// KTP: XOR to find only changed weapon bits, then iterate only those.
	// Reduces from 42 iterations to ~2-3 on average (typical weapon pickup/drop).
	// Grenade slots (13, 14, 15, 16, 36) are masked out.
	static const int GRENADE_MASK = (1<<13) | (1<<14) | (1<<15) | (1<<16);
	// Note: bit 36 is beyond 32-bit int range, so it's never set in the bitfield

	int changed = (wpns_bitfield ^ weapons) & ~GRENADE_MASK;
	while (changed)
	{
		int i = __builtin_ctz(changed);  // Index of lowest set bit
		changed &= changed - 1;         // Clear lowest set bit
		if (i >= 1 && i < DODMAX_WEAPONS)
			MF_ExecuteForward(iFWpnPickupForward, index, i, ((weapons & (1 << i)) ? 1 : 0));
	}
}

// *****************************************************
// class Grenades
// *****************************************************
// KTP: Fixed-size pool — no allocation, no linked list traversal overhead
void Grenades::put(edict_t* grenade, float time, int type, CPlayer* player)
{
	// Find a free slot (expired or inactive)
	int slot = -1;
	for (int i = 0; i < MAX_GRENADES; i++)
	{
		if (!pool[i].active || pool[i].time <= gpGlobals->time)
		{
			slot = i;
			break;
		}
	}
	if (slot == -1)
		slot = 0;  // Overwrite oldest if full (shouldn't happen with 32 slots)

	pool[slot].player = player;
	pool[slot].grenade = grenade;
	pool[slot].time = gpGlobals->time + time;
	pool[slot].type = type;
	pool[slot].active = true;
	if (slot >= count)
		count = slot + 1;
}

bool Grenades::find(edict_t* enemy, CPlayer** p, int& type)
{
	bool found = false;
	float lastTime = 0.0;

	for (int i = 0; i < count; i++)
	{
		if (!pool[i].active)
			continue;

		if (pool[i].time <= gpGlobals->time)
		{
			pool[i].active = false;  // Expired, mark free
			continue;
		}

		if (pool[i].grenade == enemy)
		{
			found = true;
			// Need latest time because of caught grenades (two players can own same nade)
			if (pool[i].time > lastTime)
			{
				*p = pool[i].player;
				type = pool[i].type;
				lastTime = pool[i].time;
			}
		}
	}
	return found;
}

void Grenades::clear()
{
	for (int i = 0; i < MAX_GRENADES; i++)
		pool[i].active = false;
	count = 0;
}

// *****************************************************
// class CMapInfo
// *****************************************************

void CMapInfo::Init()
{
	pEdict = 0;
	initialized = false;

	/* default values from dod.fgd */
	detect_axis_paras = 0;
	detect_allies_paras = 0;
	detect_allies_country = 0;

}

// *****************************************************
// class CObjective (ported from dodfun for CP tracking)
// *****************************************************

void CObjective::SetKeyValue(int index, char *keyname, char *value)
{
	if (!obj[index].pEdict || FNullEnt(obj[index].pEdict))
		return;

	KeyValueData pkvd;

	pkvd.szClassName = (char *)STRING(obj[index].pEdict->v.classname);
	pkvd.szKeyName = keyname;
	pkvd.szValue = value;
	pkvd.fHandled = false;

	MDLL_KeyValue(obj[index].pEdict, &pkvd);
}

void CObjective::InitObj(int dest, edict_t* ed)
{
	if (count <= 0)
		return;

	// Type-0 MESSAGE_BEGIN is Sys_Error; gmsgInitObj is 0 until RegUserMsg
	// interception captures it. On the objective path, so far more reachable
	// than the native send sites that already carry this guard.
	if (gmsgInitObj <= 0)
		return;

	MESSAGE_BEGIN(dest, gmsgInitObj, 0, ed);
	WRITE_BYTE(count);
	for (int i = 0; i < count; i++)
	{
		// KTP: Use ENTINDEX_SAFE for extension mode safety
		WRITE_SHORT(ENTINDEX_SAFE(obj[i].pEdict));
		// The cp id on the wire is the array position, which is what Client_SetObj
		// subscripts with. obj[].index is that position + 1, so writing it would put
		// every control point one slot out on the client.
		WRITE_BYTE(i);
		WRITE_BYTE(obj[i].owner);
		WRITE_BYTE(obj[i].visible);
		WRITE_BYTE(obj[i].icon_neutral);
		WRITE_BYTE(obj[i].icon_allies);
		WRITE_BYTE(obj[i].icon_axis);
		WRITE_COORD(obj[i].origin_x);
		WRITE_COORD(obj[i].origin_y);
	}
	MESSAGE_END();
}

void CObjective::SetObj(int index)
{
	if (gmsgSetObj <= 0)
		return;   // same Sys_Error hazard as InitObj above

	MESSAGE_BEGIN(MSG_ALL, gmsgSetObj);
	// Position, not obj[].index — same reason as InitObj above.
	WRITE_BYTE(index);
	WRITE_BYTE(obj[index].owner);
	WRITE_BYTE(0);
	MESSAGE_END();
}

void CObjective::UpdateOwner(int index, int team)
{
	if (index < 0 || index >= count)
		return;
	if (!obj[index].pEdict || FNullEnt(obj[index].pEdict))
		return;

	obj[index].owner = team;
	GET_CP_PD(obj[index].pEdict).owner = team;

	switch (team)
	{
		case 0:
			obj[index].pEdict->v.model = MAKE_STRING(GET_CP_PD(obj[index].pEdict).model_neutral);
			obj[index].pEdict->v.body = GET_CP_PD(obj[index].pEdict).model_body_neutral;
			break;
		case 1:
			obj[index].pEdict->v.model = MAKE_STRING(GET_CP_PD(obj[index].pEdict).model_allies);
			obj[index].pEdict->v.body = GET_CP_PD(obj[index].pEdict).model_body_allies;
			break;
		case 2:
			obj[index].pEdict->v.model = MAKE_STRING(GET_CP_PD(obj[index].pEdict).model_axis);
			obj[index].pEdict->v.body = GET_CP_PD(obj[index].pEdict).model_body_axis;
			break;
	}
	mObjects.SetObj(index);
}

void CObjective::Sort()
{
	objinfo_t temp;
	for (int j = 0; j < count - 1; j++)
	{
		for (int i = 0; i < count - 1; i++)
		{
			if (obj[i].index > obj[i + 1].index)
			{
				temp = obj[i + 1];
				obj[i + 1] = obj[i];
				obj[i] = temp;
			}
		}
	}
}

