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

#include <stdarg.h>   // dump_append (round-timer diagnostic)
#include <string.h>   // memcpy (grenade pickup ammo-slot probe)
#include "amxxmodule.h"
#include "dodx.h"

/* DoD's WeaponList carries no weapon name — only the ammo index and weapon id */
weaponlist_s weaponlist[] =
{
	{ 0,     0,	  0,	false}, // 0,
	{ -1,    0,	 -1,	true }, // DODW_AMERKNIFE = 1,
	{ -1,    0,	 -1,	true }, // DODW_GERKNIFE,
	{  4,   64,	  7,	true }, // DODW_COLT,
	{  4,   64,	  8,	true }, // DODW_LUGER,
	{  3,  128,	  8,	true }, // DODW_GARAND,
	{  3,  128,	  5,	true }, // DODW_SCOPED_KAR,
	{  1,  128,	 30,	true }, // DODW_THOMPSON,
	{  6,  128,	 30,	true }, // DODW_STG44,
	{  5,  128,	  5,	true }, // DODW_SPRINGFIELD,
	{  3,  128,	  5,	true }, // DODW_KAR,
	{  6,  128,	 20,	true }, // DODW_BAR,
	{  1,  130,	 30,	true }, // DODW_MP40,
	{  9,   24,	 -1,	true }, // DODW_HANDGRENADE,
	{ 11,   24,	 -1,	true }, // DODW_STICKGRENADE,
	{ 12,   24,	 -1,	true }, // DODW_STICKGRENADE_EX,
	{ 10,   24,	 -1,	true }, // DODW_HANDGRENADE_EX,
	{  7, 2178,	250,	true }, // DODW_MG42,
	{  8,  130,	150,	true }, // DODW_30_CAL,
	{ -1,    0,	 -1,	true }, // DODW_SPADE,
	{  2,  128,	 15,	true }, // DODW_M1_CARBINE,
	{  2,  130,	 75,	true }, // DODW_MG34,
	{  1,  128,	 30,	true }, // DODW_GREASEGUN,
	{  6,  128,	 20,	true }, // DODW_FG42,
	{  2,  128,	 10,	true }, // DODW_K43,
	{  3,  128,	 10,	true }, // DODW_ENFIELD,
	{  1,  128,	 30,	true }, // DODW_STEN,
	{  6,  128,	 30,	true }, // DODW_BREN,
	{  4,   64,	  6,	true }, // DODW_WEBLEY,
	{ 13,  642,	  1,	true }, // DODW_BAZOOKA,
	{ 13,  642,	  1,	true }, // DODW_PANZERSCHRECK,
	{ 13,  642,	  1,	true }, // DODW_PIAT,
	{  3,  128,	 20,	true }, // DODW_SCOPED_FG42, UNSURE ABOUT THIS ONE
	{  2,  128,	 15,	true }, // DODW_FOLDING_CARBINE,
	{  0,    0,	  0,	false}, // DODW_KAR_BAYONET,
	{  3,  128,	 10,	true }, // DODW_SCOPED_ENFIELD, UNSURE ABOUT THIS ONE
	{  9,   24,	 -1,	true }, // DODW_MILLS_BOMB,
	{ -1,    0,	 -1,	true }, // DODW_BRITKNIFE,
	{ 38,    0,	  0,	false}, // DODW_GARAND_BUTT,
	{ 39,    0,	  0,	false}, // DODW_ENFIELD_BAYONET,
	{ 40,    0,	  0,	false}, // DODW_MORTAR,
	{ 41,    0,	  0,	false}, // DODW_K43_BUTT,
};

#define WEAPONLIST_SIZE (sizeof(weaponlist) / sizeof(weaponlist[0]))

// from id to name 3 params id, name, len
static cell AMX_NATIVE_CALL get_weapon_name(AMX *amx, cell *params)
{ 
	int id = params[1];

	if(id < 0 || id >= DODMAX_WEAPONS)
	{ 
		MF_LogError(amx, AMX_ERR_NATIVE, "Invalid weapon id %d", id);
		return 0;
	}

	return MF_SetAmxString(amx,params[2],weaponData[id].name,params[3]);
}

// from log to name
static cell AMX_NATIVE_CALL wpnlog_to_name(AMX *amx, cell *params)
{ 
	int iLen;
	char *log = MF_GetAmxString(amx,params[1],0,&iLen);

	for(int i = 0; i < DODMAX_WEAPONS; i++)
	{
		if(strcmp(log,weaponData[i].logname ) == 0)
			return MF_SetAmxString(amx,params[2],weaponData[i].name,params[3]);
	}
	return 0;
}

// from log to id
static cell AMX_NATIVE_CALL wpnlog_to_id(AMX *amx, cell *params)
{ 
	int iLen;
	char *log = MF_GetAmxString(amx, params[1], 0, &iLen);

	for(int i = 0; i < DODMAX_WEAPONS; i++)
	{
		if(strcmp(log,weaponData[i].logname) == 0)
			return i;
	}
	return 0;
}

// from id to log
static cell AMX_NATIVE_CALL get_weapon_logname(AMX *amx, cell *params)
{ 
	int id = params[1];

	if (id<0 || id>=DODMAX_WEAPONS)
	{ 
		MF_LogError(amx, AMX_ERR_NATIVE, "Invalid weapon id %d", id);
		return 0;
	}

	return MF_SetAmxString(amx,params[2],weaponData[id].logname,params[3]);
}

static cell AMX_NATIVE_CALL is_melee(AMX *amx, cell *params)
{
	int id = params[1];

	if(id < 0 || id >= DODMAX_WEAPONS)
	{ 
		MF_LogError(amx, AMX_ERR_NATIVE, "Invalid weapon id %d", id);
		return 0;
	}

	return weaponData[id].melee;
}

static cell AMX_NATIVE_CALL get_team_score(AMX *amx, cell *params)
{
	int index = params[1];

	switch ( index )
	{
	case 1:
		return AlliesScore;
		break;

	case 2:
		return AxisScore;
		break;
	}
	return 0;
}

static cell AMX_NATIVE_CALL get_user_score(AMX *amx, cell *params)
{
	int index = params[1];
	CHECK_PLAYER(index);
	CPlayer* pPlayer = GET_PLAYER_POINTER_I(index);

	if (pPlayer->ingame)
		return (cell)pPlayer->savedScore;

	return -1;
}

static cell AMX_NATIVE_CALL get_user_class(AMX *amx, cell *params)
{
	int index = params[1];
	CHECK_PLAYER(index);
	CPlayer* pPlayer = GET_PLAYER_POINTER_I(index);

	// KTP: Check pEdict is valid before accessing
	if (pPlayer->ingame && pPlayer->pEdict && !pPlayer->pEdict->free)
		return pPlayer->pEdict->v.playerclass;

	return 0;
}

// KTP: Set player class (ported from dodfun, extension mode compatible)
static cell AMX_NATIVE_CALL dodx_set_user_class(AMX *amx, cell *params)
{
	int index = params[1];
	CHECK_PLAYER(index);
	int iClass = params[2];

	CPlayer* pPlayer = GET_PLAYER_POINTER_I(index);
	if (!pPlayer->ingame || !pPlayer->pEdict || pPlayer->pEdict->free || !pPlayer->pEdict->pvPrivateData)
		return 0;

	if (iClass) {
		*((int*)pPlayer->pEdict->pvPrivateData + STEAM_PDOFFSET_CLASS) = iClass;
		*((int*)pPlayer->pEdict->pvPrivateData + STEAM_PDOFFSET_RCLASS) = 0; // disable random class
	} else {
		*((int*)pPlayer->pEdict->pvPrivateData + STEAM_PDOFFSET_RCLASS) = 1; // set random class
	}

	return 1;
}

// KTP: Set player team (ported from dodfun, extension mode compatible)
static cell AMX_NATIVE_CALL dodx_set_user_team(AMX *amx, cell *params)
{
	int index = params[1];
	CHECK_PLAYER(index);
	int iTeam = params[2];

	if (iTeam < 1 || iTeam > 3) {
		MF_Log("dodx_set_user_team: invalid team id %d", iTeam);
		return 0;
	}

	CPlayer* pPlayer = GET_PLAYER_POINTER_I(index);
	if (!pPlayer->ingame || !pPlayer->pEdict || pPlayer->pEdict->free || !pPlayer->pEdict->pvPrivateData)
		return 0;

	pPlayer->killPlayer();
	pPlayer->pEdict->v.team = iTeam;

	// Set team name in private data
	char* pTeamName = (char*)pPlayer->pEdict->pvPrivateData + STEAM_PDOFFSET_TEAMNAME;
	const char* teamName;
	switch (iTeam) {
		case 1: teamName = "Allies"; break;
		case 2: teamName = "Axis"; break;
		case 3: teamName = "Spectators"; break;
		default: teamName = ""; break;
	}
	strncpy(pTeamName, teamName, 15);
	pTeamName[15] = '\0';

	*((int*)pPlayer->pEdict->pvPrivateData + STEAM_PDOFFSET_RCLASS) = 1; // set random class

	// Broadcast team change if refresh requested
	if (params[3]) {
		// MESSAGE_BEGIN with type 0 is Sys_Error -- the engine kills the process.
		// gmsgPTeam is 0 until the RegUserMsg interception captures DoD's "PTeam"
		// registration, so a hook-ordering regression turns this native into a
		// server kill. Same recoverable guard the 2.7.22 sweep gave the sibling
		// send sites; this one was missed. The team change above has already been
		// applied, so returning 1 is correct -- only the client-side refresh is
		// skipped.
		if (gmsgPTeam <= 0)
		{
			MF_Log("dodx_set_user_team: PTeam message not registered, skipping refresh broadcast");
			return 1;
		}

		MESSAGE_BEGIN(MSG_ALL, gmsgPTeam);
		WRITE_BYTE(pPlayer->index);
		WRITE_BYTE(iTeam);
		MESSAGE_END();
	}

	return 1;
}

// KTP: Get player origin (extension mode compatible, no fakemeta needed)
static cell AMX_NATIVE_CALL dodx_get_user_origin(AMX *amx, cell *params)
{
	int index = params[1];
	CHECK_PLAYER(index);

	CPlayer* pPlayer = GET_PLAYER_POINTER_I(index);
	if (!pPlayer->ingame || !pPlayer->pEdict || pPlayer->pEdict->free)
		return 0;

	cell *origin = MF_GetAmxAddr(amx, params[2]);
	origin[0] = amx_ftoc(pPlayer->pEdict->v.origin[0]);
	origin[1] = amx_ftoc(pPlayer->pEdict->v.origin[1]);
	origin[2] = amx_ftoc(pPlayer->pEdict->v.origin[2]);

	return 1;
}

// KTP: Get player bounding box (extension mode compatible, no fakemeta needed)
//
// The companion to dodx_area_get_bounds. GoldSrc decides trigger membership by
// BBOX OVERLAP, not by whether the origin is inside the brush, so a caller
// asking "was this player in that zone?" needs the player's box too -- a point
// test rejects players the engine itself counts as inside.
//
// absmin/absmax are world-space and already account for stance: a prone DoD
// player has a flatter box than a standing one, which matters precisely at the
// zone edges where these questions get decided.
static cell AMX_NATIVE_CALL dodx_get_user_bounds(AMX *amx, cell *params)
{
	int index = params[1];
	CHECK_PLAYER(index);

	CPlayer* pPlayer = GET_PLAYER_POINTER_I(index);
	if (!pPlayer->ingame || !pPlayer->pEdict || pPlayer->pEdict->free)
		return 0;

	cell *mins = MF_GetAmxAddr(amx, params[2]);
	cell *maxs = MF_GetAmxAddr(amx, params[3]);
	for (int i = 0; i < 3; i++)
	{
		mins[i] = amx_ftoc(pPlayer->pEdict->v.absmin[i]);
		maxs[i] = amx_ftoc(pPlayer->pEdict->v.absmax[i]);
	}

	return 1;
}

// KTP: Set player origin (extension mode compatible, no fakemeta needed)
static cell AMX_NATIVE_CALL dodx_set_user_origin(AMX *amx, cell *params)
{
	int index = params[1];
	CHECK_PLAYER(index);

	CPlayer* pPlayer = GET_PLAYER_POINTER_I(index);
	if (!pPlayer->ingame || !pPlayer->pEdict || pPlayer->pEdict->free)
		return 0;

	cell *origin = MF_GetAmxAddr(amx, params[2]);
	pPlayer->pEdict->v.origin[0] = amx_ctof(origin[0]);
	pPlayer->pEdict->v.origin[1] = amx_ctof(origin[1]);
	pPlayer->pEdict->v.origin[2] = amx_ctof(origin[2]);

	return 1;
}

// KTP: Get player view angles (extension mode compatible, no fakemeta needed)
static cell AMX_NATIVE_CALL dodx_get_user_angles(AMX *amx, cell *params)
{
	int index = params[1];
	CHECK_PLAYER(index);

	CPlayer* pPlayer = GET_PLAYER_POINTER_I(index);
	if (!pPlayer->ingame || !pPlayer->pEdict || pPlayer->pEdict->free)
		return 0;

	cell *angles = MF_GetAmxAddr(amx, params[2]);
	angles[0] = amx_ftoc(pPlayer->pEdict->v.v_angle[0]);
	angles[1] = amx_ftoc(pPlayer->pEdict->v.v_angle[1]);
	angles[2] = amx_ftoc(pPlayer->pEdict->v.v_angle[2]);

	return 1;
}

// KTP: Set player view angles (extension mode compatible, no fakemeta needed)
static cell AMX_NATIVE_CALL dodx_set_user_angles(AMX *amx, cell *params)
{
	int index = params[1];
	CHECK_PLAYER(index);

	CPlayer* pPlayer = GET_PLAYER_POINTER_I(index);
	if (!pPlayer->ingame || !pPlayer->pEdict || pPlayer->pEdict->free)
		return 0;

	cell *angles = MF_GetAmxAddr(amx, params[2]);
	pPlayer->pEdict->v.v_angle[0] = amx_ctof(angles[0]);
	pPlayer->pEdict->v.v_angle[1] = amx_ctof(angles[1]);
	pPlayer->pEdict->v.v_angle[2] = amx_ctof(angles[2]);

	// Also set angles for proper view direction
	pPlayer->pEdict->v.angles[0] = amx_ctof(angles[0]) / -3.0f;
	pPlayer->pEdict->v.angles[1] = amx_ctof(angles[1]);
	pPlayer->pEdict->v.angles[2] = 0;

	// Fix view with fixangle
	pPlayer->pEdict->v.fixangle = 1;

	return 1;
}

static cell AMX_NATIVE_CALL user_kill(AMX *amx, cell *params)
{
	int index = params[1];
	CHECK_PLAYER(index);
	CPlayer* pPlayer = GET_PLAYER_POINTER_I(index);

	if(pPlayer->ingame && pPlayer->IsAlive())
	{
		pPlayer->killPlayer();
		return 1;
	}

	return 0;
}	

static cell AMX_NATIVE_CALL get_map_info(AMX *amx, cell *params)
{
	switch(params[1])
	{
	case 0:
		return g_map.detect_allies_country;
		break;

	case 1:
		return g_map.detect_allies_paras;
		break;

	case 2:
		return g_map.detect_axis_paras;
		break;

	default:
		MF_LogError(amx, AMX_ERR_NATIVE, "Invalid map info id %d", params[1]);
		break;
	}
	return -1;
}

static cell AMX_NATIVE_CALL get_user_pronestate(AMX *amx, cell *params)
{
	int index = params[1];
	CHECK_PLAYER(index);
	CPlayer* pPlayer = GET_PLAYER_POINTER_I(index);

	// KTP: Check pEdict is valid before accessing
	if (pPlayer->ingame && pPlayer->pEdict && !pPlayer->pEdict->free)
		return pPlayer->pEdict->v.iuser3;

	return 0;
}

static cell AMX_NATIVE_CALL get_user_weapon(AMX *amx, cell *params)
{
	int index = params[1];
	CHECK_PLAYER(index);
	CPlayer* pPlayer = GET_PLAYER_POINTER_I(index);

	if (pPlayer->ingame)
	{
		int wpn = pPlayer->current;
		cell *cpTemp = MF_GetAmxAddr(amx,params[2]);
		*cpTemp = pPlayer->weapons[wpn].clip;
		cpTemp = MF_GetAmxAddr(amx,params[3]);
		*cpTemp = pPlayer->weapons[wpn].ammo;
		return wpn;
	}

	return 0;
}

/* We want to get just the weapon of whichever type that the player is on him */
static cell AMX_NATIVE_CALL dod_weapon_type(AMX *amx, cell *params) /* 2 params */
{
	int index = params[1];
	int type = params[2];

	CHECK_PLAYER(index);

	if(type < DODWT_PRIMARY || type > DODWT_OTHER)
	{
		MF_LogError(amx, AMX_ERR_NATIVE, "Invalid weapon type id %d", type);
		return 0;
	}

	CPlayer* pPlayer = GET_PLAYER_POINTER_I(index);

	// KTP: Check pEdict is valid before accessing
	if(pPlayer->ingame && pPlayer->pEdict && !pPlayer->pEdict->free)
	{
		int weaponsbit = pPlayer->pEdict->v.weapons & ~(1<<31); // don't count last element

		for(int x = 1; x < DODMAX_WEAPONS; ++x)
		{
			if((weaponsbit&(1<<x)) > 0)
			{
				if(weaponData[x].type == type)
					return x;
			}
		}
	}

	return 0;
}

// forward
static cell AMX_NATIVE_CALL register_forward(AMX *amx, cell *params)
{ 

	#ifdef FORWARD_OLD_SYSTEM
		int iFunctionIndex;
		int err;
		switch( params[1] )
		{
		case 0:
			if((err = MF_AmxFindPublic(amx, "client_damage", &iFunctionIndex)) == AMX_ERR_NONE)
				g_damage_info.put( amx , iFunctionIndex );

			else
				MF_LogError(amx, err, "client_damage not found");
				return 0;
			break;

		case 1:
			if((err = MF_AmxFindPublic(amx, "client_death", &iFunctionIndex)) == AMX_ERR_NONE)
				g_death_info.put( amx , iFunctionIndex );

			else
				MF_LogError(amx, err, "client_Death not found");
				return 0;
			break;

		case 2:
			if((err = MF_AmxFindPublic(amx, "client_score", &iFunctionIndex)) == AMX_ERR_NONE)
				g_score_info.put( amx , iFunctionIndex );

			else
				MF_LogError(amx, err, "client_score not found");
				return 0;
			break;

		default:
			MF_LogError(amx, AMX_ERR_NATIVE, "Invalid forward id %d", params[2]);
			return 0;
		}
	#endif

	return 1;
}

// name,logname,melee=0 
static cell AMX_NATIVE_CALL register_cwpn(AMX *amx, cell *params)
{ 
	int i;
	bool bFree = false;

	for(i = DODMAX_WEAPONS - DODMAX_CUSTOMWPNS; i < DODMAX_WEAPONS; i++)
	{
		if(!weaponData[i].needcheck)
		{
			bFree = true;
			break;
		}
	}

	if(!bFree)
		return 0;

	int iLen;
	char *szName = MF_GetAmxString(amx, params[1], 0, &iLen);
	char *szLogName = MF_GetAmxString(amx, params[3], 0, &iLen);

	strncpy(weaponData[i].name, szName, sizeof(weaponData[i].name) - 1);
	weaponData[i].name[sizeof(weaponData[i].name) - 1] = '\0';
	strncpy(weaponData[i].logname, szLogName, sizeof(weaponData[i].logname) - 1);
	weaponData[i].logname[sizeof(weaponData[i].logname) - 1] = '\0';
	weaponData[i].needcheck = true;
	weaponData[i].melee = params[2] ? true:false;
	return i;
}

// wid,att,vic,dmg,hp=0
static cell AMX_NATIVE_CALL cwpn_dmg(AMX *amx, cell *params)
{ 
	int weapon = params[1];

	// only for custom weapons
	if(weapon < DODMAX_WEAPONS-DODMAX_CUSTOMWPNS)
	{ 
		MF_LogError(amx, AMX_ERR_NATIVE, "Invalid custom weapon id %d", weapon);
		return 0;
	}

	int att = params[2];
	CHECK_PLAYER(params[2]);

	int vic = params[3];
	CHECK_PLAYER(params[3]);
	
	int dmg = params[4];
	if(dmg<1)
	{
		MF_LogError(amx, AMX_ERR_NATIVE, "Invalid damage %d", dmg);
		return 0;
	}
	
	int aim = params[5];
	if(aim < 0 || aim > 7)
	{
		MF_LogError(amx, AMX_ERR_NATIVE, "Invalid aim %d", aim);
		return 0;
	}

	CPlayer* pAtt = GET_PLAYER_POINTER_I(att);
	CPlayer* pVic = GET_PLAYER_POINTER_I(vic);

	// KTP: Check pEdict is valid before accessing
	if (!pVic->pEdict || pVic->pEdict->free)
		return 0;
	if (!pAtt->pEdict || pAtt->pEdict->free)
		return 0;

	pVic->pEdict->v.dmg_inflictor = NULL;

	if(pAtt->index != pVic->index)
		pAtt->saveHit(pVic , weapon , dmg, aim);

	int TA = 0;

	if((pVic->pEdict->v.team == pAtt->pEdict->v.team) && (pVic != pAtt))
		TA = 1;

	MF_ExecuteForward(iFDamage,pAtt->index, pVic->index, dmg, weapon, aim, TA);

	if(pVic->IsAlive())
		return 1;

	pAtt->saveKill(pVic,weapon,( aim == 1 ) ? 1:0 ,TA);

	MF_ExecuteForward(iFDeath,pAtt->index, pVic->index, weapon, aim, TA);

	return 1;
}

// player,wid
static cell AMX_NATIVE_CALL cwpn_shot(AMX *amx, cell *params)
{ 
	int index = params[2];

	CHECK_PLAYER(index);

	int weapon = params[1];
	if(weapon < DODMAX_WEAPONS-DODMAX_CUSTOMWPNS)
	{
		MF_LogError(amx, AMX_ERR_NATIVE, "Invalid custom weapon id %d", weapon);
		return 0;
	}

	CPlayer* pPlayer = GET_PLAYER_POINTER_I(index);
	pPlayer->saveShot(weapon);

	return 1;
}

static cell AMX_NATIVE_CALL get_maxweapons(AMX *amx, cell *params)
{
	return DODMAX_WEAPONS;
}

static cell AMX_NATIVE_CALL get_stats_size(AMX *amx, cell *params)
{
	return 9;
}

static cell AMX_NATIVE_CALL is_custom(AMX *amx, cell *params)
{
	int weapon = params[1];

	if(weapon < DODMAX_WEAPONS-DODMAX_CUSTOMWPNS)
	{
		return 0;
	}
	return 1;
}

// player,wid
static cell AMX_NATIVE_CALL dod_get_user_team(AMX *amx, cell *params)
{
	int index = params[1];
	CHECK_PLAYER(index);

	CPlayer* pPlayer = GET_PLAYER_POINTER_I(index);
	// KTP: Check pEdict is valid before accessing
	if (!pPlayer->pEdict || pPlayer->pEdict->free)
		return 0;
	return pPlayer->pEdict->v.team;

}

// player,wid
// KTP: This function is disabled in extension mode - core AMXX's get_user_team is used instead
// The native registration for this function is commented out in the natives table below
static cell AMX_NATIVE_CALL get_user_team(AMX *amx, cell *params)
{
	int index = params[1];
	CHECK_PLAYER(index);

	CPlayer* pPlayer = GET_PLAYER_POINTER_I(index);
	// KTP: Check pEdict is valid before accessing
	if (!pPlayer->pEdict || pPlayer->pEdict->free)
		return 0;
	int iTeam = pPlayer->pEdict->v.team;

	if ( params[3] )
	{
		const char *szTeam = "";
		switch(iTeam)
		{
		case 1:
			szTeam = "Allies";
			break;

		case 2:
			szTeam = "Axis";
			break;
		}

		MF_SetAmxString(amx,params[2],szTeam,params[3]);
	}
	return iTeam;
}

static cell AMX_NATIVE_CALL dod_set_model(AMX *amx, cell *params) // player,model
{
	int index = params[1];
	CHECK_PLAYER(index);

	CPlayer* pPlayer = GET_PLAYER_POINTER_I(index);
	if(!pPlayer->ingame)
	{
		MF_LogError(amx, AMX_ERR_NATIVE, "Invalid Player, Not on Server");
		return 0;
	}

	int length;
	pPlayer->initModel((char*)STRING(ALLOC_STRING(MF_GetAmxString(amx, params[2], 1, &length))));

	return true;
}

static cell AMX_NATIVE_CALL dod_set_body(AMX *amx, cell *params) // player,bodynumber
{
	int index = params[1];
	CHECK_PLAYER(index);

	CPlayer* pPlayer = GET_PLAYER_POINTER_I(index);
	if(!pPlayer->ingame)
	{
		MF_LogError(amx, AMX_ERR_NATIVE, "Invalid Player, Not on Server");
		return 0;
	}

	pPlayer->setBody(params[2]);

	return true;
}

static cell AMX_NATIVE_CALL dod_clear_model(AMX *amx, cell *params) // player
{
	int index = params[1];
	CHECK_PLAYER(index);

	CPlayer* pPlayer = GET_PLAYER_POINTER_I(index);
	if(!pPlayer->ingame)
		return false;

	pPlayer->clearModel();

	return true;
}

/* 
0 [Byte]	1	// Weapons Groupings
1 [Byte]	210	// Total Rounds Allowed
2 [Byte]	-1	// Undefined Not Used
3 [Byte]	-1	// Undefined Not Used
4 [Byte]	2	// Weapon Slot
5 [Byte]	0	// Bucket ( Position Under Weapon Slot )
6 [Short]	7	// Weapon Number / Bit Field for the weapon
7 [Byte]	128	// Bit Field for the Ammo or Ammo Type
8 [Byte]	30	// Rounds Per Mag

id, wpnID, slot, position, totalrds
*/
static cell AMX_NATIVE_CALL dod_weaponlist(AMX *amx, cell *params) // player
{
	int id = params[1];
	int wpnID = params[2];
	int slot = params[3];
	int position = params[4];
	int totalrds = params[5];

	// Bounds check both indices before array access
	if (id < 0 || id >= (int)WEAPONLIST_SIZE)
	{
		MF_LogError(amx, AMX_ERR_NATIVE, "Invalid weapon id %d (max %d)", id, (int)WEAPONLIST_SIZE - 1);
		return 0;
	}
	if (wpnID < 0 || wpnID >= (int)WEAPONLIST_SIZE)
	{
		MF_LogError(amx, AMX_ERR_NATIVE, "Invalid wpnID %d (max %d)", wpnID, (int)WEAPONLIST_SIZE - 1);
		return 0;
	}

	if(!weaponlist[id].changeable)
	{
		MF_LogError(amx, AMX_ERR_NATIVE, "This Weapon Cannot be Changed");
		return 0;
	}

	UTIL_LogPrintf("ID (%d) WpnID (%d) Slot (%d) Pos (%d) Rounds (%d)", id, wpnID, slot, position, totalrds);

	CHECK_PLAYER(id);

	CPlayer* pPlayer = GET_PLAYER_POINTER_I(id);
	if(!pPlayer->ingame)
	{
		MF_LogError(amx, AMX_ERR_NATIVE, "Invalid Player, Not on Server");
		return 0;
	}

	// GET_USER_MSG_ID resolves through Metamod's gpMetaUtilFuncs, which is only
	// assigned in Meta_Query -- and Meta_Query never runs in extension mode, so
	// this is a NULL deref, not merely the type-0 Sys_Error the sibling sends
	// risk. Latent (no fleet plugin calls this native) but it is registered and
	// callable by any plugin.
	int msgWeaponList = GET_USER_MSG_ID(PLID, "WeaponList", NULL);
	if (msgWeaponList <= 0)
	{
		MF_Log("dod_set_weaponlist: WeaponList message unavailable (extension mode has no Metamod util funcs)");
		return 0;
	}

	MESSAGE_BEGIN(MSG_ONE, msgWeaponList, NULL, pPlayer->pEdict);
	WRITE_BYTE(weaponlist[wpnID].grp);
		WRITE_BYTE(totalrds);
		WRITE_BYTE(-1);
		WRITE_BYTE(-1);
		WRITE_BYTE(slot - 1);
		WRITE_BYTE(position);
		WRITE_SHORT(wpnID);
		WRITE_BYTE(weaponlist[wpnID].bitfield);

		// Is it grenades
		if(wpnID == 13 || wpnID == 14 || wpnID == 15 || wpnID == 16 || wpnID == 36)
			WRITE_BYTE(-1);
		else if(wpnID == 29 || wpnID == 30 || wpnID == 31)
			WRITE_BYTE(1);
		else
			WRITE_BYTE(weaponlist[wpnID].clip);
	MESSAGE_END();

	return 1;
}

// KTP: Set player's team name in private data (extension mode compatible)
// This affects server-side logs but NOT the scoreboard (DoD client hardcodes team names)
// native dodx_set_pl_teamname(id, const szName[]);
static cell AMX_NATIVE_CALL dodx_set_pl_teamname(AMX *amx, cell *params)
{
	int id = params[1];
	if (id < 1 || id > gpGlobals->maxClients)
		return 0;

	edict_t* pEdict = MF_GetPlayerEdict(id);
	if (!pEdict || !pEdict->pvPrivateData)
		return 0;

	int len;
	const char* szName = MF_GetAmxString(amx, params[2], 0, &len);

	// Copy exactly 16 bytes like dodfun does (null-padded)
	char nameBuf[16] = {0};
	int copyLen = (len < 15) ? len : 15;
	memcpy(nameBuf, szName, copyLen);

	// Copy all 16 bytes to private data
	char* pTeamName = (char*)pEdict->pvPrivateData + STEAM_PDOFFSET_TEAMNAME;
	for (int i = 0; i < 16; i++) {
		pTeamName[i] = nameBuf[i];
	}

	return 1;
}

// KTP: Per-player score / deaths read+write into DoD game-DLL private data.
// Offsets defined in dodx.h (STEAM_PDOFFSET_SCORE / STEAM_PDOFFSET_DEATHS;
// the same offsets dodx_set_pl_teamname / dodx_set_user_class use). Adds
// the missing piece KTPMatchHandler.sma needs to restore per-player
// scoreboard state across mid-match disconnect/reconnect (the AMXX/ReAPI
// path for these fields requires ReGameDll, which is Counter-Strike-only
// and unavailable on DoD — validated empirically on ATL:27019 2026-05-11
// via "Run time error 10 (native set_member_s)" when calling
// set_member(id, m_iDeaths, ...)).
//
// Safety: same guard chain as the other pdata writers in this file.

// native dodx_set_user_deaths(id, deaths);
static cell AMX_NATIVE_CALL dodx_set_user_deaths(AMX *amx, cell *params)
{
	int id = params[1];
	if (id < 1 || id > gpGlobals->maxClients)
		return 0;

	edict_t* pEdict = MF_GetPlayerEdict(id);
	if (!pEdict || !pEdict->pvPrivateData)
		return 0;

	*((int*)pEdict->pvPrivateData + STEAM_PDOFFSET_DEATHS) = params[2];

	// Re-baseline the offset-validation counter to the restored value —
	// otherwise a restored player's next save mismatches by exactly the
	// restored deaths and the gate refuses to persist (restore→re-disconnect).
	extern int g_observedDeaths[33];
	if (id < 33)
		g_observedDeaths[id] = params[2];

	return 1;
}

// native dodx_get_user_deaths(id);
static cell AMX_NATIVE_CALL dodx_get_user_deaths(AMX *amx, cell *params)
{
	int id = params[1];
	if (id < 1 || id > gpGlobals->maxClients)
		return 0;

	edict_t* pEdict = MF_GetPlayerEdict(id);
	if (!pEdict || !pEdict->pvPrivateData)
		return 0;

	return *((int*)pEdict->pvPrivateData + STEAM_PDOFFSET_DEATHS);
}

// native dodx_set_user_score(id, score);
static cell AMX_NATIVE_CALL dodx_set_user_score(AMX *amx, cell *params)
{
	int id = params[1];
	if (id < 1 || id > gpGlobals->maxClients)
		return 0;

	edict_t* pEdict = MF_GetPlayerEdict(id);
	if (!pEdict || !pEdict->pvPrivateData)
		return 0;

	*((int*)pEdict->pvPrivateData + STEAM_PDOFFSET_SCORE) = params[2];
	return 1;
}

// native dodx_get_user_score(id);
static cell AMX_NATIVE_CALL dodx_get_user_score(AMX *amx, cell *params)
{
	int id = params[1];
	if (id < 1 || id > gpGlobals->maxClients)
		return 0;

	edict_t* pEdict = MF_GetPlayerEdict(id);
	if (!pEdict || !pEdict->pvPrivateData)
		return 0;

	return *((int*)pEdict->pvPrivateData + STEAM_PDOFFSET_SCORE);
}

// KTP: Per-player observed-deaths counter for the score/deaths offset
// validation gate. Ticks ONCE per death via countObservedDeath() in
// usermsg.cpp's death paths (Damage hook AND Client_DeathMsg) — the
// per-life exactly-once gate (g_deathCountedThisLife) is the dedup; the
// 33ms window only guards saveKill/iFDeath forward semantics, not this
// counter. Covers normal frags, suicides (`kill` console command), and
// world deaths — same coverage as DoD's m_iDeaths increment in
// dod_i386.so.
//
// Why not life.deaths: that counter is updated via CPlayer::saveKill
// which is only invoked from the Damage hook path. Suicides via `kill`
// reach Client_DeathMsg but bypass damage flow, so life.deaths under-
// counts (verified empirically 5/21: 2 suicides + scoreboard deaths=2
// but life.deaths stayed 0).
//
// Why not AMXX get_user_deaths: AMXX core's CPlayer.deaths is only
// updated by its Client_ScoreInfo hook which doesn't catch DoD's death
// broadcasts (verified empirically 5/21: get_user_deaths returned 0).
//
// Use case: plugin calls this AND dodx_get_user_deaths at SAVE time;
// if they disagree, score_deaths_offset is mis-configured (likely a
// new OS bump shifted the struct again). Plugin logs + skips persist.
//
// native dodx_get_observed_deaths(id);
extern int g_observedDeaths[33];

static cell AMX_NATIVE_CALL dodx_get_observed_deaths(AMX *amx, cell *params)
{
	int id = params[1];
	if (id < 1 || id > gpGlobals->maxClients || id >= 33)
		return 0;

	return g_observedDeaths[id];
}

// KTP 2026-05-21: Refresh a player's scoreboard row by broadcasting a
// ScoreShort message in DoD's exact native format. Reads m_iObjScore +
// pev->frags + m_iDeaths from the player and sends them to all clients.
//
// Use case: after dodx_set_user_deaths / dodx_set_user_score writes (score
// persistence restore), the engine doesn't auto-broadcast until the next
// score-changing event. Calling this native immediately syncs every client's
// scoreboard with the just-written values.
//
// Format (derived from disassembling CDoDTeamPlay::PlayerKilled broadcast
// site at b2774-b27ee in dod_i386.so md5 4f4727b2...):
//   BYTE  player_index
//   SHORT m_iObjScore
//   SHORT (int)frags
//   SHORT m_iDeaths
//   BYTE  1               (constant; mirrors what engine sends on every death)
//
// Bypasses the AMX message_begin Pawn native path that crashed in the
// 2026-05-21 v1.3.1 RESTORE test (vtable lookup at ktpamx_i386.so:0x561c3
// segfaulted). Uses the same direct MESSAGE_BEGIN/WRITE_BYTE/MESSAGE_END
// engine funcs as dodx_broadcast_team_score (proven safe since v0.10.20
// per CLAUDE.md).
//
// native dodx_broadcast_scoreboard(id);
static cell AMX_NATIVE_CALL dodx_broadcast_scoreboard(AMX *amx, cell *params)
{
	int id = params[1];
	if (id < 1 || id > gpGlobals->maxClients)
		return 0;

	if (gmsgScoreShort <= 0)
	{
		MF_Log("dodx_broadcast_scoreboard: ScoreShort message not registered");
		return 0;
	}

	edict_t* pEdict = MF_GetPlayerEdict(id);
	if (!pEdict || !pEdict->pvPrivateData)
		return 0;

	int score  = *((int*)pEdict->pvPrivateData + STEAM_PDOFFSET_SCORE);
	int frags  = (int)pEdict->v.frags;
	int deaths = *((int*)pEdict->pvPrivateData + STEAM_PDOFFSET_DEATHS);

	MESSAGE_BEGIN(MSG_ALL, gmsgScoreShort, NULL);
	WRITE_BYTE(id);
	WRITE_SHORT(score);
	WRITE_SHORT(frags);
	WRITE_SHORT(deaths);
	WRITE_BYTE(1);
	MESSAGE_END();

	return 1;
}

// KTP: Set team score in gamerules (modifies the scoreboard directly)
// This allows restoring cumulative scores from 1st half when 2nd half starts
// native dodx_set_team_score(team, score);
static cell AMX_NATIVE_CALL dodx_set_team_score(AMX *amx, cell *params)
{
	// Recoverable failures log + return 0 instead of raising a native error —
	// callers (KTPScoreTracker) check the return and continue.
	if (!DODX_HasGameRules())
	{
		MF_Log("dodx_set_team_score: gamerules not available");
		return 0;
	}

	int team = params[1];   // 1=Allies, 2=Axis
	int score = params[2];

	if (team < 1 || team > 2)
	{
		MF_Log("dodx_set_team_score: invalid team %d (must be 1 or 2)", team);
		return 0;
	}

	// m_iTeamScores is int[32] at offset g_iTeamScoreOffset in gamerules
	// Team indices in DoD: 1=Allies, 2=Axis (same as array index)
	int *pScores = (int*)((char*)*g_pGameRulesAddress + g_iTeamScoreOffset);
	pScores[team] = score;

	return 1;
}

// KTP: Get team score from gamerules (reads the scoreboard value directly)
// native dodx_get_team_score(team);
static cell AMX_NATIVE_CALL dodx_get_team_score(AMX *amx, cell *params)
{
	// Check if gamerules is available
	if (!DODX_HasGameRules())
	{
		// Fallback to DODX tracked score (from TeamScore message)
		int team = params[1];
		switch (team)
		{
		case 1: return AlliesScore;
		case 2: return AxisScore;
		default: return 0;
		}
	}

	int team = params[1];   // 1=Allies, 2=Axis

	if (team < 1 || team > 2)
	{
		MF_Log("dodx_get_team_score: invalid team %d (must be 1 or 2)", team);
		return 0;
	}

	// m_iTeamScores is int[32] at offset g_iTeamScoreOffset in gamerules
	int *pScores = (int*)((char*)*g_pGameRulesAddress + g_iTeamScoreOffset);
	return pScores[team];
}

// KTP: Check if gamerules score modification is available
// native dodx_has_gamerules();
static cell AMX_NATIVE_CALL dodx_has_gamerules(AMX *amx, cell *params)
{
	return DODX_HasGameRules() ? 1 : 0;
}

// KTP: Authoritative seconds remaining in the current half, straight from the
// engine's own half-clock accounting — the closed-loop source for broadcast
// overlays (KTPHudObserver), replacing open-loop anchor estimates.
//
// DoD accounts the half clock as  timelimit·60 − (time − m_flDoDMapTime):
// m_flDoDMapTime is 0 from map load (pub behavior == get_timeleft) and is
// rewritten to the restart-completion gametime by a clan restart
// (mp_clan_restartround), which is when the client HUD clock rebases. During
// the restart countdown (m_bRoundRestarting set), m_flRestartRoundTime already
// holds the scheduled completion time, so the post-rebase clock is projected
// from it — callers get the correct post-go-live value for the entire
// countdown window instead of a stale pre-restart clock. Members confirmed
// against a live go-live via dodx_test_scan_gamerules (2026-07-11); offsets
// from shipped gamedata (identical across platforms for these members).
//
// Returns seconds remaining as a float; -1.0 when unavailable (no gamerules,
// offsets unresolved, mp_timelimit unset/0 = no time limit, or an implausible
// read). Never raises; callers must handle -1.0 by falling back.
// native Float:dodx_get_round_time();
static cell AMX_NATIVE_CALL dodx_get_round_time(AMX *amx, cell *params)
{
	static const float FAIL = -1.0f;

	if (!DODX_HasGameRules() || g_iDoDMapTimeOffset < 0 || !g_pcvarMpTimelimit)
		return amx_ftoc(FAIL);

	float limit_sec = g_pcvarMpTimelimit->value * 60.0f;
	if (limit_sec <= 0.0f)
		return amx_ftoc(FAIL);   // no time limit — "remaining" is undefined

	char *gr = (char*)*g_pGameRulesAddress;
	float base = *(float*)(gr + g_iDoDMapTimeOffset);

	// Countdown window: the engine has committed to a restart at
	// m_flRestartRoundTime but hasn't rebased m_flDoDMapTime yet — project.
	if (g_iRoundRestartingOffset >= 0 && g_iRestartRoundTimeOffset >= 0
		&& *(int*)(gr + g_iRoundRestartingOffset))
	{
		float target = *(float*)(gr + g_iRestartRoundTimeOffset);
		if (target > 0.0f)
			base = target;
	}

	// base is a past-or-imminent gametime; negative/garbage means a read gone
	// wrong (struct shift on a future dod.so update) — fail soft, never lie.
	if (base < 0.0f || base > gpGlobals->time + 3600.0f)
		return amx_ftoc(FAIL);

	float remaining = limit_sec - (gpGlobals->time - base);
	if (remaining < 0.0f)
		remaining = 0.0f;
	else if (remaining > 86400.0f)
		return amx_ftoc(FAIL);

	return amx_ftoc(remaining);
}

// KTP: Resolve (and cache) the map's dod_control_point_master.
//
// A map has at most one. FindEntityByClassname wraps pfnFindEntityByString,
// which is the extension-mode-safe way to walk entities (pfnPEntityOfEntIndex
// hangs during OnPluginsLoaded there — see DODX_InitCPFromEntities). The cache
// is cleared per map in DODX_OnSV_ActivateServer, so a freed edict is never
// read; the revalidation below covers the rest.
static edict_t *DODX_GetCPMaster()
{
	if (g_pCPMasterEdict)
	{
		// Revalidate: an edict can be freed and its slot reused without a map
		// change. free==0 && pvPrivateData means it is still a live entity, and
		// the classname check confirms the slot was not handed to something else.
		if (!g_pCPMasterEdict->free && g_pCPMasterEdict->pvPrivateData
			&& STRING(g_pCPMasterEdict->v.classname)
			&& !strcmp(STRING(g_pCPMasterEdict->v.classname), "dod_control_point_master"))
			return g_pCPMasterEdict;
		g_pCPMasterEdict = nullptr;
	}

	edict_t *pEdict = FindEntityByClassname(NULL, "dod_control_point_master");
	if (pEdict && !pEdict->free && pEdict->pvPrivateData)
		g_pCPMasterEdict = pEdict;

	return g_pCPMasterEdict;
}

// KTP: Seconds until the map's next TERRITORIAL scoring tick — the periodic
// team-point award for holding control points, driven by
// CControlPointMaster::m_fGivePointsTime.
//
// This is the closed-loop source for a broadcast overlay's scoring-tick
// countdown. The DoD client shows this NOWHERE — not the countdown, not the
// amount — so without it an observer can only infer the phase by watching the
// score move, which costs a lock-on period at every half start and again after
// every round restart (a restart rebases the master's clock).
//
// Returns seconds remaining as a float; -1.0 when unavailable (no master entity
// on the map, offsets unresolved, master inactive, or an implausible read).
// Never raises; callers must handle -1.0 by falling back.
// native Float:dodx_get_score_tick_time();
static cell AMX_NATIVE_CALL dodx_get_score_tick_time(AMX *amx, cell *params)
{
	static const float FAIL = -1.0f;

	if (g_iCPMGivePointsTimeOffset < 0 || !gpGlobals)
		return amx_ftoc(FAIL);

	edict_t *pMaster = DODX_GetCPMaster();
	if (!pMaster)
		return amx_ftoc(FAIL);

	char *pd = (char *)pMaster->pvPrivateData;

	// The master gates its own scoring; an inactive master never awards, so a
	// countdown against it would be a countdown to nothing.
	if (g_iCPMActiveOffset >= 0 && !*(int *)(pd + g_iCPMActiveOffset))
		return amx_ftoc(FAIL);

	float target = *(float *)(pd + g_iCPMGivePointsTimeOffset);

	// target is an absolute gametime in the near future. Anything negative, or
	// further out than a generous period bound, means the read went wrong (a
	// struct shift on a future dod.so). Fail soft — never report a made-up clock.
	if (target < 0.0f || target > gpGlobals->time + 3600.0f)
		return amx_ftoc(FAIL);

	float remaining = target - gpGlobals->time;
	if (remaining < 0.0f)
		remaining = 0.0f;

	return amx_ftoc(remaining);
}

// KTP: The map's territorial scoring PERIOD in seconds
// (CControlPointMaster::m_iGivePointsDelay), or -1 when unavailable.
//
// Companion to dodx_get_score_tick_time. Note the engine's observed award
// spacing runs slightly longer than this value (measured 30.50s against a
// delay of 30 on the KTP fleet) because the master only awards on its own
// think; treat this as the nominal period, not a phase.
// native dodx_get_score_tick_period();
static cell AMX_NATIVE_CALL dodx_get_score_tick_period(AMX *amx, cell *params)
{
	if (g_iCPMGivePointsDelayOffset < 0)
		return -1;

	edict_t *pMaster = DODX_GetCPMaster();
	if (!pMaster)
		return -1;

	int delay = *(int *)((char *)pMaster->pvPrivateData + g_iCPMGivePointsDelayOffset);
	if (delay <= 0 || delay > 3600)
		return -1;

	return delay;
}

// KTP: Test/diagnostic-only: explain what dodx_get_score_tick_time() is seeing.
//
// That native deliberately returns -1.0 on every failure path rather than a
// fabricated clock, which makes "the scoring panel isn't showing" indivisible
// from outside: no master entity, unresolved offsets, an inactive master and an
// implausible read all look identical. This prints which one it is.
// Read-only and safe on any map in any state. Production plugins MUST NOT call it.
// native dodx_test_dump_score_tick();
static cell AMX_NATIVE_CALL dodx_test_dump_score_tick(AMX *amx, cell *params)
{
	edict_t *pMaster = DODX_GetCPMaster();

	if (!pMaster)
	{
		MF_Log("[DODX] scoretick: NO dod_control_point_master entity found "
			"(offsets time=%d delay=%d active=%d)",
			g_iCPMGivePointsTimeOffset, g_iCPMGivePointsDelayOffset, g_iCPMActiveOffset);
		return 0;
	}

	if (g_iCPMGivePointsTimeOffset < 0)
	{
		MF_Log("[DODX] scoretick: master found but m_fGivePointsTime offset unresolved");
		return 0;
	}

	char *pd = (char *)pMaster->pvPrivateData;
	int active = (g_iCPMActiveOffset >= 0) ? *(int *)(pd + g_iCPMActiveOffset) : -1;
	float target = *(float *)(pd + g_iCPMGivePointsTimeOffset);
	int delay = (g_iCPMGivePointsDelayOffset >= 0)
		? *(int *)(pd + g_iCPMGivePointsDelayOffset) : -1;

	MF_Log("[DODX] scoretick: active=%d givePointsTime=%.2f delay=%d now=%.2f remaining=%.2f%s",
		active, target, delay, gpGlobals->time, target - gpGlobals->time,
		(active == 0) ? "  <- INACTIVE MASTER: native reports unavailable" : "");

	return 1;
}

// KTP: Broadcast TeamScore message to all clients
// This properly updates client scoreboards after modifying gamerules scores
// native dodx_broadcast_team_score(team, score);
static cell AMX_NATIVE_CALL dodx_broadcast_team_score(AMX *amx, cell *params)
{
	int team = params[1];   // 1=Allies, 2=Axis
	int score = params[2];

	if (team < 1 || team > 2)
	{
		MF_Log("dodx_broadcast_team_score: invalid team %d (must be 1 or 2)", team);
		return 0;
	}

	// First, set the gamerules score if available
	if (DODX_HasGameRules())
	{
		int *pScores = (int*)((char*)*g_pGameRulesAddress + g_iTeamScoreOffset);
		pScores[team] = score;
	}

	// Update DODX tracked score
	if (team == 1)
		AlliesScore = score;
	else
		AxisScore = score;

	// Check if we have the TeamScore message ID
	if (gmsgTeamScore <= 0)
	{
		MF_Log("dodx_broadcast_team_score: TeamScore message not registered");
		return 0;
	}

	// Send TeamScore message to all clients
	// DoD TeamScore format: BYTE(team) + SHORT(score)
	MESSAGE_BEGIN(MSG_ALL, gmsgTeamScore, NULL);
	WRITE_BYTE(team);
	WRITE_SHORT(score);
	MESSAGE_END();

	return 1;
}

// KTP: Set custom team name on scoreboard for all players on a team
// Sends TeamInfo message to all clients for each player on the team
// native dodx_set_scoreboard_team_name(team, const name[]);
static cell AMX_NATIVE_CALL dodx_set_scoreboard_team_name(AMX *amx, cell *params)
{
	int team = params[1];   // 1=Allies, 2=Axis

	if (team < 1 || team > 2)
	{
		MF_Log("dodx_set_scoreboard_team_name: invalid team %d (must be 1 or 2)", team);
		return 0;
	}

	// Check if we have the TeamInfo message ID
	if (gmsgTeamInfo <= 0)
	{
		MF_Log("dodx_set_scoreboard_team_name: TeamInfo message not registered");
		return 0;
	}

	// Get team name string from params
	int len;
	char *teamName = MF_GetAmxString(amx, params[2], 0, &len);
	if (!teamName || len == 0)
	{
		MF_Log("dodx_set_scoreboard_team_name: empty team name");
		return 0;
	}

	int count = 0;

	// Iterate through all players
	for (int i = 1; i <= gpGlobals->maxClients; i++)
	{
		CPlayer *pPlayer = GET_PLAYER_POINTER_I(i);
		if (!pPlayer || !pPlayer->pEdict || pPlayer->pEdict->free)
			continue;

		// Get player's team from edict
		int playerTeam = pPlayer->pEdict->v.team;
		if (playerTeam != team)
			continue;

		// Send TeamInfo message to ALL clients for this player
		// TeamInfo format: BYTE(player index) + STRING(team name)
		MESSAGE_BEGIN(MSG_ALL, gmsgTeamInfo, NULL);
		WRITE_BYTE(i);
		WRITE_STRING(teamName);
		MESSAGE_END();

		count++;
	}

	return count;
}

// KTP: Grenade ammo manipulation (ported from dodfun for extension mode).
// The slot is resolved per map, never assumed: the DLL numbers ammo types in
// map precache order, so a constant addresses a DIFFERENT ammo type's counter
// from one map to the next, silently and with no failure path.
static bool DODX_IsGrenadeType(int grenadeType)
{
	// DODW_HANDGRENADE, DODW_STICKGRENADE, DODW_MILLS_BOMB
	return grenadeType == 13 || grenadeType == 14 || grenadeType == 36;
}

static int *DODX_GrenadeAmmoCell(CPlayer *pPlayer, int grenadeType, const char *nativeName)
{
	int slot = DODX_GrenadeAmmoIndex(grenadeType);
	if (slot < 0)
	{
		MF_Log("%s: invalid grenade type %d", nativeName, grenadeType);
		return NULL;
	}

	return (int*)pPlayer->pEdict->pvPrivateData + PDOFFSET_AMMO_ARRAY + slot;
}

// dodx_set_grenade_ammo(id, grenade_type, count)
// grenade_type: DODW_HANDGRENADE (13), DODW_STICKGRENADE (14), DODW_MILLS_BOMB (36)
static cell AMX_NATIVE_CALL dodx_set_grenade_ammo(AMX *amx, cell *params)
{
	int index = params[1];
	CHECK_PLAYER(index);
	CPlayer* pPlayer = GET_PLAYER_POINTER_I(index);

	if (!pPlayer->ingame || !pPlayer->pEdict || !pPlayer->pEdict->pvPrivateData)
		return 0;

	int count = params[3];
	if (count < 0) count = 0;
	if (count > 10) count = 10;

	int *pAmmo = DODX_GrenadeAmmoCell(pPlayer, params[2], "dodx_set_grenade_ammo");
	if (!pAmmo)
		return 0;

	// m_rgAmmo only — see dodx.h: touching m_rgAmmoLast suppresses the DLL's AmmoX.
	*pAmmo = count;

	return 1;
}

// dodx_get_grenade_ammo(id, grenade_type)
// Count held, or -1 on a bad argument.
static cell AMX_NATIVE_CALL dodx_get_grenade_ammo(AMX *amx, cell *params)
{
	int index = params[1];
	CHECK_PLAYER(index);
	CPlayer* pPlayer = GET_PLAYER_POINTER_I(index);

	if (!pPlayer->ingame || !pPlayer->pEdict || !pPlayer->pEdict->pvPrivateData)
		return -1;

	int *pAmmo = DODX_GrenadeAmmoCell(pPlayer, params[2], "dodx_get_grenade_ammo");
	if (!pAmmo)
		return -1;

	return *pAmmo;
}

// dodx_get_grenade_ammo_index(grenade_type)
// The ammo slot this map uses for that grenade, or -1 if it is not a grenade.
// Callers that send their own AmmoX should take the slot from here rather than
// repeating the constant.
static cell AMX_NATIVE_CALL dodx_get_grenade_ammo_index(AMX *amx, cell *params)
{
	return DODX_GrenadeAmmoIndex(params[1]);
}

// KTP: Noclip control (ported from fun module for extension mode)
// dodx_set_user_noclip(id, noclip)
// noclip: 0 = disable, 1 = enable
static cell AMX_NATIVE_CALL dodx_set_user_noclip(AMX *amx, cell *params)
{
	int index = params[1];
	CHECK_PLAYER(index);

	CPlayer* pPlayer = GET_PLAYER_POINTER_I(index);
	if (!pPlayer->pEdict || !pPlayer->pEdict->pvPrivateData)
		return 0;

	// MOVETYPE_WALK = 3, MOVETYPE_NOCLIP = 8
	pPlayer->pEdict->v.movetype = params[2] ? 8 : 3;

	return 1;
}

// KTP: Get player movetype (for diagnostics — no engine module in extension mode)
// dodx_get_user_movetype(id)
// Returns: movetype value (3=WALK, 8=NOCLIP, etc.) or -1 on error
static cell AMX_NATIVE_CALL dodx_get_user_movetype(AMX *amx, cell *params)
{
	int index = params[1];
	if (index < 1 || index > gpGlobals->maxClients)
		return -1;

	CPlayer* pPlayer = GET_PLAYER_POINTER_I(index);
	if (!pPlayer->pEdict)
		return -1;

	return pPlayer->pEdict->v.movetype;
}

// KTP: Diagnostic — check why CHECK_PLAYER fails for a player index
// dodx_debug_player_state(id)
// Returns bitmask: bit0=ingame, bit1=pEdict!=null, bit2=!free, bit3=!FNullEnt
// A fully valid player returns 15 (0xF). 0 means completely uninitialized.
static cell AMX_NATIVE_CALL dodx_debug_player_state(AMX *amx, cell *params)
{
	int index = params[1];
	if (index < 1 || index > gpGlobals->maxClients)
		return -1;

	CPlayer* pPlayer = GET_PLAYER_POINTER_I(index);
	int result = 0;

	if (pPlayer->ingame)       result |= 1;
	if (pPlayer->pEdict)       result |= 2;
	if (pPlayer->pEdict && !pPlayer->pEdict->free)      result |= 4;
	if (pPlayer->pEdict && !FNullEnt(pPlayer->pEdict))   result |= 8;

	return result;
}

// KTP: Send AmmoX message to update client HUD
// dodx_send_ammox(id, ammo_slot, count)
// ammo_slot is a raw ammo-type index and is NOT constant across maps — get the
// grenade ones from dodx_get_grenade_ammo_index(). Setting ammo through
// dodx_set_grenade_ammo already makes the DLL emit its own AmmoX.
static cell AMX_NATIVE_CALL dodx_send_ammox(AMX *amx, cell *params)
{
	int index = params[1];
	CHECK_PLAYER(index);

	CPlayer* pPlayer = GET_PLAYER_POINTER_I(index);
	if (!pPlayer->pEdict)
		return 0;

	if (gmsgAmmoX <= 0)
	{
		MF_Log("dodx_send_ammox: AmmoX message not registered");
		return 0;
	}

	int ammoSlot = params[2];
	int count = params[3];

	// Clamp count to byte range
	if (count < 0) count = 0;
	if (count > 254) count = 254;

	MESSAGE_BEGIN(MSG_ONE, gmsgAmmoX, NULL, pPlayer->pEdict);
	WRITE_BYTE(ammoSlot);
	WRITE_BYTE(count);
	MESSAGE_END();

	return 1;
}

// KTP: Give a grenade weapon to a player (for infinite grenades in practice mode)
// dodx_give_grenade(id, grenade_type)
// grenade_type: DODW_HANDGRENADE (13), DODW_STICKGRENADE (14), DODW_MILLS_BOMB (36)
static cell AMX_NATIVE_CALL dodx_give_grenade(AMX *amx, cell *params)
{
	int index = params[1];
	CHECK_PLAYER(index);

	CPlayer* pPlayer = GET_PLAYER_POINTER_I(index);
	if (!pPlayer->pEdict || !pPlayer->IsAlive())
		return 0;

	int grenadeType = params[2];

	// Determine weapon classname based on grenade type
	const char* weaponClass;
	switch (grenadeType)
	{
		case 13: // DODW_HANDGRENADE
			weaponClass = "weapon_handgrenade";
			break;
		case 14: // DODW_STICKGRENADE
			weaponClass = "weapon_stickgrenade";
			break;
		case 36: // DODW_MILLS_BOMB
			// The DLL links no weapon_mills_bomb; the Mills bomb is
			// weapon_handgrenade with a British model.
			weaponClass = "weapon_handgrenade";
			break;
		default:
			MF_LogError(amx, AMX_ERR_NATIVE, "dodx_give_grenade: invalid grenade type %d", grenadeType);
			return 0;
	}

	// Get game DLL functions (works in both Metamod and extension mode)
	DLL_FUNCTIONS* pGameDll = (DLL_FUNCTIONS*)MF_GetGameDllFuncs();
	if (!pGameDll || !pGameDll->pfnSpawn || !pGameDll->pfnTouch)
		return 0;

	// Create the weapon entity
	edict_t* pWeapon = CREATE_NAMED_ENTITY(ALLOC_STRING(weaponClass));
	if (!pWeapon || FNullEnt(pWeapon))
		return 0;

	// Position at player's origin
	pWeapon->v.origin = pPlayer->pEdict->v.origin;
	pWeapon->v.spawnflags |= (1 << 30);  // SF_NORESPAWN - prevent respawn

	// Spawn the entity using game DLL function
	pGameDll->pfnSpawn(pWeapon);

	// Remember solid state AFTER spawn but BEFORE touch — pfnSpawn changes
	// solid (e.g. SOLID_NOT -> SOLID_TRIGGER), so capturing before spawn
	// would make the post-touch comparison always differ, leaking entities
	int oldSolid = pWeapon->v.solid;

	// A successful pickup makes the DLL credit ammo to the slot it assigned this
	// weapon on this map — a second reading of the same registry, independent of
	// WeaponList, so a grenade native still resolves if that message never lands.
	int *pAmmoArray = pPlayer->pEdict->pvPrivateData
		? (int*)pPlayer->pEdict->pvPrivateData + PDOFFSET_AMMO_ARRAY : NULL;
	int ammoBefore[DODX_MAX_AMMO_SLOTS];
	if (pAmmoArray)
		memcpy(ammoBefore, pAmmoArray, sizeof(ammoBefore));

	// Touch the player to pick it up using game DLL function
	pGameDll->pfnTouch(pWeapon, pPlayer->pEdict);

	if (pAmmoArray)
	{
		// Only an unambiguous single-slot rise identifies the slot; two movers
		// means something else changed ammo in the same call, so learn nothing.
		int moved = -1;
		for (int i = 0; i < DODX_MAX_AMMO_SLOTS; i++)
		{
			if (pAmmoArray[i] <= ammoBefore[i])
				continue;
			if (moved >= 0)
			{
				moved = -1;
				break;
			}
			moved = i;
		}
		if (moved >= 0)
			DODX_ObserveGrenadeAmmoIndex(grenadeType, moved);
	}

	// If solid state changed, pickup was successful
	// If not, entity wasn't picked up - remove it to avoid clutter
	if (pWeapon->v.solid == oldSolid && !FNullEnt(pWeapon) && pWeapon->free == 0)
	{
		REMOVE_ENTITY(pWeapon);
		return -1;  // Indicate pickup failed (player may already have max)
	}

	return 1;
}

// dodx_strip_grenade(id, grenade_type)
// Clears grenade ammo for a player (simplified - just zeros ammo slots)
// grenade_type: DODW_HANDGRENADE (13), DODW_STICKGRENADE (14), DODW_MILLS_BOMB (36)
static cell AMX_NATIVE_CALL dodx_strip_grenade(AMX *amx, cell *params)
{
	int index = params[1];
	CHECK_PLAYER(index);

	CPlayer* pPlayer = GET_PLAYER_POINTER_I(index);
	if (!pPlayer->pEdict || !pPlayer->pEdict->pvPrivateData)
		return 0;

	// Keeps the documented abort-on-bad-type contract, which the shared helper
	// downgrades to a log for the set/get pair.
	if (!DODX_IsGrenadeType(params[2]))
	{
		MF_LogError(amx, AMX_ERR_NATIVE, "dodx_strip_grenade: invalid grenade type %d", params[2]);
		return 0;
	}

	int *pAmmo = DODX_GrenadeAmmoCell(pPlayer, params[2], "dodx_strip_grenade");
	if (!pAmmo)
		return 0;

	*pAmmo = 0;

	return 1;
}

// dodx_debug_dump_ammo(id)
// This map's resolved grenade slots plus the player's non-zero m_rgAmmo entries.
static cell AMX_NATIVE_CALL dodx_debug_dump_ammo(AMX *amx, cell *params)
{
	int index = params[1];
	CHECK_PLAYER(index);

	CPlayer* pPlayer = GET_PLAYER_POINTER_I(index);
	if (!pPlayer->pEdict || !pPlayer->pEdict->pvPrivateData)
		return 0;

	const int *pAmmo = (const int*)pPlayer->pEdict->pvPrivateData + PDOFFSET_AMMO_ARRAY;

	MF_Log("[DODX DEBUG] Player %d - m_rgAmmo at int-offset %d; grenade slots this map: hand=%d stick=%d mills=%d",
		index, PDOFFSET_AMMO_ARRAY,
		DODX_GrenadeAmmoIndex(13), DODX_GrenadeAmmoIndex(14), DODX_GrenadeAmmoIndex(36));

	for (int i = 0; i < DODX_MAX_AMMO_SLOTS; i++)
	{
		if (pAmmo[i] != 0)
			MF_Log("[DODX DEBUG]   m_rgAmmo[%d] = %d", i, pAmmo[i]);
	}

	return 1;
}

// ============================================================================
// KTP TEST-ONLY: Forward dispatch primitives for the Tier 2 integration suite.
// ============================================================================
//
// These natives bypass the engine event chain and dispatch DODX forwards
// directly via MF_ExecuteForward. They are NOT for production use — they
// exist solely to give Phase 3 hot-path tests in
// `KTPInfrastructure/tests/integration/` a deterministic way to fire the
// `client_damage`, `dod_grenade_explosion`, `client_score`, and
// `dod_score_event` forwards without relying on bot-vs-bot combat
// (unreliable) or sniffing engine usermsg internals (high-coupling).
//
// Production safety:
//   - The natives are no-ops with respect to engine state — they only
//     dispatch the forward to subscribed plugins. They do NOT modify
//     pev->frags, weapon stats, or any other engine field.
//   - Any plugin that calls these COULD spoof a DODX forward (e.g. fake
//     a player attack), but DODX itself dispatches the same forwards from
//     real engine events using the same MF_ExecuteForward primitive — a
//     spoof is no more dangerous than a real event. The risk surface is
//     equivalent to "a plugin calls MF_ExecuteForward(iFDamage, ...)" which
//     any sufficiently-privileged plugin could already do via direct AMX
//     module access.
//   - Production plugins MUST NOT call these. Naming convention
//     (`dodx_test_dispatch_*` prefix) signals the intent.
//
// Each native short-circuits if the relevant forward isn't registered
// (defensive — should never happen in practice since DODX registers all
// forwards in OnAmxxAttach).

// dodx_test_dispatch_weapon_fire(id, weapon, Float:gametime)
// Fires the `dod_client_weapon_fire` forward. params[3] is already a Pawn
// Float cell — pass it through unchanged (production site amx_ftoc's a raw
// C++ float; here it's pre-encoded, so re-encoding would corrupt it).
// A synthetic dispatch whose handler calls dodx_get_shot_geom can consume a
// LIVE capture if the weapon id matches — one more reason production plugins
// must never call this.
static cell AMX_NATIVE_CALL dodx_test_dispatch_weapon_fire(AMX *amx, cell *params)
{
	if (iFWeaponFire == -1)
		return 0;

	int id        = params[1];
	int weapon    = params[2];
	cell gametime = params[3];   // Float cell, bit-reinterpreted IEEE 754

	// Bounds-check the player slot — see dodx_test_dispatch_damage rationale.
	int maxClients = gpGlobals->maxClients;
	if (id < 1 || id > maxClients) return 0;

	MF_ExecuteForward(iFWeaponFire, id, weapon, gametime);
	return 1;
}

// dodx_test_dispatch_damage(attacker, victim, damage, wpnindex, hitplace, TA)
// Fires the `client_damage` forward. All args are ints.
static cell AMX_NATIVE_CALL dodx_test_dispatch_damage(AMX *amx, cell *params)
{
	if (iFDamage < 0)
		return 0;

	int attacker = params[1];
	int victim   = params[2];
	int damage   = params[3];
	int wpnindex = params[4];
	int hitplace = params[5];
	int TA       = params[6];

	// Bounds-check the player slots so a subscribed handler that does
	// engine lookups (MF_IsPlayerIngame, dodx_get_user_*, etc.) on the
	// slot doesn't see UB from an out-of-range test value. Production
	// dispatch sites are gated on `GET_PLAYER_POINTER` which validates
	// implicitly; the test native has no such gate. attacker may be 0
	// (worldspawn-style "no attacker") so floor it at 0; victim must be
	// an actual player slot.
	int maxClients = gpGlobals->maxClients;
	if (attacker < 0 || attacker > maxClients) return 0;
	if (victim < 1 || victim > maxClients) return 0;

	MF_ExecuteForward(iFDamage, attacker, victim, damage, wpnindex, hitplace, TA);
	return 1;
}

// dodx_test_dispatch_grenade_explosion(id, Float:pos[3], wpnid)
// Fires the `dod_grenade_explosion` forward. pos is a Pawn-side Float[3]
// passed by reference; we re-pack as a cell array per the established
// pattern (see moduleconfig.cpp:487-491 for the production dispatch site).
static cell AMX_NATIVE_CALL dodx_test_dispatch_grenade_explosion(AMX *amx, cell *params)
{
	if (iFGrenadeExplode < 0)
		return 0;

	int id    = params[1];
	cell *posCells = MF_GetAmxAddr(amx, params[2]);
	int wpnid = params[3];

	// Bounds-check the player slot — see dodx_test_dispatch_damage rationale.
	int maxClients = gpGlobals->maxClients;
	if (id < 1 || id > maxClients) return 0;

	// Pawn cells already encode floats as bit-reinterpreted IEEE 754 — pass
	// through to MF_PrepareCellArray without re-conversion (the consumer
	// will read them back as Float: cells in the receiving plugin's public).
	cell position[3];
	position[0] = posCells[0];
	position[1] = posCells[1];
	position[2] = posCells[2];
	cell pos = MF_PrepareCellArray(position, 3);

	MF_ExecuteForward(iFGrenadeExplode, id, pos, wpnid);
	return 1;
}

// dodx_test_dispatch_score(id, score_delta, total_score, cp_index)
// Fires BOTH the `client_score` forward (3 args: id, score, total) AND
// the `dod_score_event` forward (4 args: id, delta, total, cp_index) —
// matching the production dispatch pattern (moduleconfig.cpp:276-278 and
// 1043-1045) where they fire in tandem.
//
// Both forwards independently guarded — early-out only if BOTH unregistered.
static cell AMX_NATIVE_CALL dodx_test_dispatch_score(AMX *amx, cell *params)
{
	if (iFScore < 0 && iFScoreEvent < 0)
		return 0;

	int id          = params[1];
	int score_delta = params[2];
	int total_score = params[3];
	int cp_index    = params[4];

	// Bounds-check the player slot — see dodx_test_dispatch_damage rationale.
	int maxClients = gpGlobals->maxClients;
	if (id < 1 || id > maxClients) return 0;

	if (iFScore >= 0)
		MF_ExecuteForward(iFScore, id, score_delta, total_score);
	if (iFScoreEvent >= 0)
		MF_ExecuteForward(iFScoreEvent, id, score_delta, total_score, cp_index);
	return 1;
}

// dodx_test_dispatch_cp_captured(cp_index, new_owner, old_owner)
// Fires the `dod_control_point_captured` forward. Phase 4 dispatch
// primitive — production dispatches this from usermsg.cpp:643-644 in
// response to engine flag-cap messages. Test-only.
static cell AMX_NATIVE_CALL dodx_test_dispatch_cp_captured(AMX *amx, cell *params)
{
	if (iFCPCaptured < 0)
		return 0;

	int cp_index  = params[1];
	int new_owner = params[2];
	int old_owner = params[3];

	MF_ExecuteForward(iFCPCaptured, cp_index, new_owner, old_owner);
	return 1;
}

// dodx_test_dispatch_client_spawn(id)
// Fires the `dod_client_spawn` forward (production site usermsg.cpp:686).
static cell AMX_NATIVE_CALL dodx_test_dispatch_client_spawn(AMX *amx, cell *params)
{
	if (iFSpawnForward < 0)
		return 0;

	int id = params[1];

	// Bounds-check the player slot — see dodx_test_dispatch_damage rationale.
	int maxClients = gpGlobals->maxClients;
	if (id < 1 || id > maxClients) return 0;

	MF_ExecuteForward(iFSpawnForward, id);
	return 1;
}

// dodx_test_dispatch_changeteam(id, team, oldteam)
// Fires the `dod_client_changeteam` forward (production site CMisc.cpp:409).
static cell AMX_NATIVE_CALL dodx_test_dispatch_changeteam(AMX *amx, cell *params)
{
	if (iFTeamForward < 0)
		return 0;

	int id      = params[1];
	int team    = params[2];
	int oldteam = params[3];

	// Bounds-check the player slot — see dodx_test_dispatch_damage rationale.
	int maxClients = gpGlobals->maxClients;
	if (id < 1 || id > maxClients) return 0;

	MF_ExecuteForward(iFTeamForward, id, team, oldteam);
	return 1;
}

// dodx_test_dispatch_changeclass(id, class, oldclass)
// Fires the `dod_client_changeclass` forward (production site CMisc.cpp:412).
static cell AMX_NATIVE_CALL dodx_test_dispatch_changeclass(AMX *amx, cell *params)
{
	if (iFClassForward < 0)
		return 0;

	int id       = params[1];
	int newclass = params[2];
	int oldclass = params[3];

	// Bounds-check the player slot — see dodx_test_dispatch_damage rationale.
	int maxClients = gpGlobals->maxClients;
	if (id < 1 || id > maxClients) return 0;

	MF_ExecuteForward(iFClassForward, id, newclass, oldclass);
	return 1;
}

// dodx_test_dispatch_client_death(killer, victim, wpnindex, hitplace, TK)
// Fires the `client_death` forward (production sites NBase.cpp:561 and
// usermsg.cpp:761 — killer first, victim second).
static cell AMX_NATIVE_CALL dodx_test_dispatch_client_death(AMX *amx, cell *params)
{
	if (iFDeath < 0)
		return 0;

	int killer   = params[1];
	int victim   = params[2];
	int wpnindex = params[3];
	int hitplace = params[4];
	int TK       = params[5];

	// Bounds-check the player slots — see dodx_test_dispatch_damage
	// rationale. killer may be 0 (world/suicide-style), victim must be
	// a real slot.
	int maxClients = gpGlobals->maxClients;
	if (killer < 0 || killer > maxClients) return 0;
	if (victim < 1 || victim > maxClients) return 0;

	MF_ExecuteForward(iFDeath, killer, victim, wpnindex, hitplace, TK);
	return 1;
}

// dodx_test_dispatch_stats_flush(id)
// Fires the `dod_stats_flush` forward for one player (production site
// NRank.cpp:410 loops it over all connected players from
// dodx_flush_all_stats; the per-player form lets a test assert a single
// synthetic player's flush without touching real engine state).
static cell AMX_NATIVE_CALL dodx_test_dispatch_stats_flush(AMX *amx, cell *params)
{
	if (iFFlushStats < 0)
		return 0;

	int id = params[1];

	// Bounds-check the player slot — see dodx_test_dispatch_damage rationale.
	int maxClients = gpGlobals->maxClients;
	if (id < 1 || id > maxClients) return 0;

	MF_ExecuteForward(iFFlushStats, id);
	return 1;
}

// dodx_test_dump_round_timers()
// Diagnostic: logs every known DoD round/half-timer candidate field in one
// line, plus one line per timer-suspect entity found by classname scan.
// Used by the KTPHudObserver timer-probe harness to empirically identify
// which field drives the client's visible half clock on standard (non-para)
// maps and its semantics (absolute end-gametime vs seconds-remaining) — the
// decision input for dodx_get_round_time(). For each float two derived
// interpretations are logged: A = raw − gpGlobals->time (remaining if the
// field is an absolute end time), B = gpGlobals->time − raw (elapsed if it
// is a start anchor). Read-only; safe on any map in any state. Offsets come
// from shipped gamedata (see moduleconfig.cpp OnPluginsLoaded); -1 =
// unresolved, that field is skipped. Returns the number of entities whose
// offsets were actually read — candidates that matched the classname substring
// but are not a known CDodRoundTimer are logged and skipped, not counted.
// Production plugins MUST NOT call this (diagnostic only).
// Saturating snprintf-append: snprintf returns the WOULD-HAVE-written length,
// so naive `len += snprintf(...)` can push len past the buffer and turn the
// next `sizeof - len` into unsigned wraparound. Clamps len to cap.
static int dump_append(char *buf, int len, int cap, const char *fmt, ...)
{
	if (len < 0 || len >= cap)
		return cap;
	va_list ap;
	va_start(ap, fmt);
	int wrote = vsnprintf(buf + len, cap - len, fmt, ap);
	va_end(ap);
	if (wrote < 0)
		return cap;
	len += wrote;
	return (len > cap) ? cap : len;
}

// Classnames whose private data is actually a CDodRoundTimer. Exact match
// only — see the entity scan below for why a substring is not enough.
static const char *const kRoundTimerClasses[] = {
	"dod_round_timer",
};

static cell AMX_NATIVE_CALL dodx_test_dump_round_timers(AMX *amx, cell *params)
{
	float now = gpGlobals->time;
	char line[512];
	int len = dump_append(line, 0, sizeof(line), "[DODX] rtdump t=%.3f gr=%d",
		now, DODX_HasGameRules() ? 1 : 0);

	if (DODX_HasGameRules())
	{
		char *gr = (char*)*g_pGameRulesAddress;
		if (g_iGrRoundTimeOffset >= 0)
		{
			float flRoundTime = *(float*)(gr + g_iGrRoundTimeOffset);
			len = dump_append(line, len, sizeof(line),
				" gr.flRT=%.2f grA=%.2f grB=%.2f",
				flRoundTime, flRoundTime - now, now - flRoundTime);
		}
		if (g_iParaTimerPtrOffset >= 0)
		{
			char *para = *(char**)(gr + g_iParaTimerPtrOffset);
			len = dump_append(line, len, sizeof(line), " para=%p", (void*)para);
			if (para)
			{
				if (g_iParaRoundTimerOffset >= 0)
				{
					float fRoundTimer = *(float*)(para + g_iParaRoundTimerOffset);
					len = dump_append(line, len, sizeof(line),
						" para.fRT=%.2f paraA=%.2f paraB=%.2f",
						fRoundTimer, fRoundTimer - now, now - fRoundTimer);
				}
				if (g_iParaBTimerOffset >= 0)
				{
					len = dump_append(line, len, sizeof(line), " para.bT=%d",
						(int)*(unsigned char*)(para + g_iParaBTimerOffset));
				}
			}
		}
	}
	MF_Log("%s", line);

	// Entity scan: any edict whose classname smells like a timer, dumped at
	// the CDodRoundTimer offsets — the fallback source if the gamerules-level
	// fields turn out to be dead on standard maps.
	int found = 0;
	for (int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities; i++)
	{
		edict_t *pEnt = INDEXENT(i);
		if (!pEnt || pEnt->free || !pEnt->pvPrivateData)
			continue;
		const char *cls = STRING(pEnt->v.classname);
		if (!cls || !cls[0])
			continue;
		if (!strstr(cls, "timer") && !strstr(cls, "round") && !strstr(cls, "clock"))
			continue;

		// The substring match is a DISCOVERY filter, not a type check: the
		// CDodRoundTimer offsets reach ~356 bytes into pvPrivateData, so
		// dereferencing them on whatever happened to contain "round" reads off
		// the end of any smaller class. Report every candidate; only read the
		// ones whose classname matches exactly.
		bool known = false;
		for (size_t k = 0; k < sizeof(kRoundTimerClasses) / sizeof(kRoundTimerClasses[0]); k++)
		{
			if (!strcmp(cls, kRoundTimerClasses[k])) { known = true; break; }
		}
		if (!known)
		{
			MF_Log("[DODX] rtdump-ent idx=%d cls=%s SKIPPED (not a known "
				"CDodRoundTimer classname; offsets not dereferenced)", i, cls);
			continue;
		}

		char *pd = (char*)pEnt->pvPrivateData;
		float fRT  = (g_iRTimerRoundTimeOffset >= 0) ? *(float*)(pd + g_iRTimerRoundTimeOffset) : -1.0f;
		float fLen = (g_iRTimerLengthOffset    >= 0) ? *(float*)(pd + g_iRTimerLengthOffset)    : -1.0f;
		int   bT   = (g_iRTimerBTimerOffset    >= 0) ? (int)*(unsigned char*)(pd + g_iRTimerBTimerOffset) : -1;
		MF_Log("[DODX] rtdump-ent idx=%d cls=%s rt.fRT=%.2f entA=%.2f entB=%.2f rt.fLen=%.2f rt.bT=%d",
			i, cls, fRT, fRT - now, now - fRT, fLen, bT);
		found++;
	}
	return found;
}

// dodx_test_scan_gamerules()
// Diagnostic: change-scanner over the first GR_SCAN_BYTES of the gamerules
// private data. First call snapshots and logs a baseline marker; every later
// call logs one line per dword that CHANGED since the previous call (offset,
// old/new as both float and int), then refreshes the snapshot. Purpose: the
// shipped-gamedata round-timer fields all proved dead on standard maps, but
// the DoD half clock demonstrably rebases inside gamerules at the
// mp_clan_restartround completion — diffing the struct across that edge
// exposes the real anchor member (a float jumping to ~restart gametime, to
// ~half-end gametime, or a fresh countdown). Capped at GR_SCAN_MAX_LINES
// lines per call to bound log volume. Read-only; test/diagnostic only.
// Returns the number of changed dwords (may exceed the log cap), -1 if no
// gamerules pointer.
//
// GR_SCAN_BYTES stays within the gamedata-documented CDoDTeamPlay extent
// (last documented member m_iEndIntermissionButtonHit at 572 + 4 = 576) —
// scanning past the allocation would be a genuine OOB read even if only a
// diagnostic. The 2026-07-11 half-clock discovery landed well inside this
// (m_flDoDMapTime@36, m_flRestartRoundTime@560).
#define GR_SCAN_BYTES     576
#define GR_SCAN_MAX_LINES 40
static cell AMX_NATIVE_CALL dodx_test_scan_gamerules(AMX *amx, cell *params)
{
	static unsigned char s_snap[GR_SCAN_BYTES];
	static void *s_snapFrom = nullptr;   // gamerules ptr the snapshot was taken from

	if (!DODX_HasGameRules())
		return -1;

	char *gr = (char*)*g_pGameRulesAddress;

	if (s_snapFrom != (void*)gr)
	{
		// First call, or gamerules was reallocated (new map) — new baseline.
		memcpy(s_snap, gr, GR_SCAN_BYTES);
		s_snapFrom = (void*)gr;
		MF_Log("[DODX] grscan baseline t=%.3f gr=%p bytes=%d", gpGlobals->time, (void*)gr, GR_SCAN_BYTES);
		return 0;
	}

	int changed = 0, logged = 0;
	for (int off = 0; off <= GR_SCAN_BYTES - 4; off += 4)
	{
		unsigned int oldv, newv;
		memcpy(&oldv, s_snap + off, 4);
		memcpy(&newv, gr + off, 4);
		if (oldv == newv)
			continue;

		changed++;
		if (logged < GR_SCAN_MAX_LINES)
		{
			float oldf, newf;
			memcpy(&oldf, &oldv, 4);
			memcpy(&newf, &newv, 4);
			MF_Log("[DODX] grscan t=%.3f off=%d old_f=%.3f new_f=%.3f old_i=%d new_i=%d",
				gpGlobals->time, off, oldf, newf, (int)oldv, (int)newv);
			logged++;
		}
	}
	if (changed > logged)
		MF_Log("[DODX] grscan t=%.3f (%d more changed dwords suppressed)", gpGlobals->time, changed - logged);

	memcpy(s_snap, gr, GR_SCAN_BYTES);
	return changed;
}

// KTP: read a player's aim/movement summary. Measurements only -- no threshold is
// applied here and none should be added; the consumer decides what the numbers mean.
//
// The window still in progress is deliberately NOT folded in: it has no final duration,
// and including it would report the same burst twice once it closes.
//
// out[] = { windowsScored, keptCount, groundTouches, shortestGroundMs }
static cell AMX_NATIVE_CALL dodx_get_aim_stats(AMX *amx, cell *params)
{
	int index = params[1];
	CHECK_PLAYER(index);

	CPlayer *pPlayer = GET_PLAYER_POINTER_I(index);
	if (!pPlayer->ingame || !pPlayer->pEdict || pPlayer->pEdict->free)
		return 0;

	const KTPAimStats &st = pPlayer->ktpAim;
	cell *out = MF_GetAmxAddr(amx, params[2]);

	out[0] = st.windowsScored;
	out[1] = st.keptCount;
	out[2] = st.groundTouches;
	out[3] = st.shortestGroundMs;

	return 1;
}

// KTP: read one retained fire window's geometry.
// out[] = { dur_ms, slope_milli_deg_per_s, rms_micro_deg, samples }
//
// Scaled integers rather than floats: these cross into Pawn and then into JSON, and a
// float round-trip through both is where a comparison silently shifts. The residual is
// in micro-degrees so that sub-milli-degree values keep several significant figures --
// a coarser unit would quantise small residuals into a handful of distinct buckets.
static cell AMX_NATIVE_CALL dodx_get_aim_window(AMX *amx, cell *params)
{
	int index = params[1];
	CHECK_PLAYER(index);

	CPlayer *pPlayer = GET_PLAYER_POINTER_I(index);
	if (!pPlayer->ingame || !pPlayer->pEdict || pPlayer->pEdict->free)
		return 0;

	const KTPAimStats &st = pPlayer->ktpAim;
	int slot = params[2];
	if (slot < 0 || slot >= st.keptCount)
		return 0;

	const KTPWindowStat &w = st.kept[slot];
	cell *out = MF_GetAmxAddr(amx, params[3]);

	out[0] = (cell)(w.dur * 1000.0 + 0.5);
	out[1] = (cell)(w.slope * 1000.0 + (w.slope < 0 ? -0.5 : 0.5));
	out[2] = (cell)(w.rms * 1000000.0 + 0.5);
	out[3] = w.n;

	return 1;
}

// KTP: read the geometry of the shot the caller's dod_client_weapon_fire forward is
// reporting. Measurements only -- no threshold is applied here and none should be
// added; the consumer decides what the numbers mean.
//
// Every guard failure returns 0 so the caller records NULL. Returning a stale or
// substitute value instead would attribute one shot's geometry to another -- in this
// system that is fabricated evidence against a real person, the single worst outcome.
// The guards, in order:
//   - stash empty or already consumed (a stash reports at most one shot);
//   - stash stamped by a different usercmd than the one this read sits in (see
//     KTPShotGeom.h for why the pairing key is a cmd ordinal, not gametime) --
//     consumed on failure, because cmdSeq only grows and it can never match later;
//   - the forward's weapon id differs from what the shooter held at trace time --
//     NOT consumed, because the stash may still belong to a dispatch that has not
//     read yet (a grenade forward probing ahead of the rifle's own read).
//
// out[] = { err_micro_deg, range_units, tgt_angvel_milli_deg_per_s, sight_gap_ms,
//           hitgroup, start_off_units }
static cell AMX_NATIVE_CALL dodx_get_shot_geom(AMX *amx, cell *params)
{
	int index = params[1];
	CHECK_PLAYER(index);

	CPlayer *pPlayer = GET_PLAYER_POINTER_I(index);
	if (!pPlayer->ingame || !pPlayer->pEdict || pPlayer->pEdict->free)
		return 0;

	KTPShotGeom &sg = pPlayer->ktpShot;

	if (sg.geomSeq == 0)
		return 0;
	if (sg.geomSeq != sg.cmdSeq)
	{
		sg.consume();
		return 0;
	}
	if ((int)params[2] != sg.geomWeapon)
		return 0;

	cell *out = MF_GetAmxAddr(amx, params[3]);
	out[0] = sg.errUdeg;
	out[1] = sg.rangeUnits;
	out[2] = sg.tgtAngVelMdps;
	out[3] = sg.sightGapMs;
	out[4] = sg.hitgroup;
	out[5] = sg.startOffUnits;

	sg.consume();
	return 1;
}

// KTP: clear a player's counters after a successful flush. Separate from the read so
// a failed POST does not silently discard the window that justified it.
static cell AMX_NATIVE_CALL dodx_reset_aim_stats(AMX *amx, cell *params)
{
	int index = params[1];
	CHECK_PLAYER(index);

	// ResetCounters, not Reset: the read natives exclude the in-progress window because
	// it will be reported once it closes, and wiping it here would make that false.
	GET_PLAYER_POINTER_I(index)->ktpAim.ResetCounters();
	return 1;
}

// KTP: read the tier-2.7 aim-vs-transmission counters. Measurements only -- no
// threshold is applied here and none should be added; KTPPackVis.h carries the
// direction and unknown-state rules a consumer must honour.
//
// out[] = { samples_known, samples_unpacked, samples_unknown,
//           window_ms_sum, window_ms_max, recorder_live }
static cell AMX_NATIVE_CALL dodx_get_aim_vis_stats(AMX *amx, cell *params)
{
	int index = params[1];
	CHECK_PLAYER(index);

	// Zeroed even on the failure return: a caller that mishandles the 0 must
	// read zeros, not whatever its own buffer held -- stale cells here are the
	// fabrication direction.
	cell *out = MF_GetAmxAddr(amx, params[2]);
	for (int i = 0; i < 6; ++i)
		out[i] = 0;

	CPlayer *pPlayer = GET_PLAYER_POINTER_I(index);
	if (!pPlayer->ingame || !pPlayer->pEdict || pPlayer->pEdict->free)
		return 0;

	const KTPPackVis &v = pPlayer->ktpVis;
	out[0] = v.samplesKnown;
	out[1] = v.samplesUnpacked;
	out[2] = v.samplesUnknown;
	out[3] = v.windowMsSum;
	out[4] = v.windowMsMax;
	out[5] = g_ktpPackRecorderLive ? 1 : 0;
	return 1;
}

// KTP: clear the tier-2.7 counters. Separate from the read for the same reason
// as the aim stats: a failed flush must not silently discard what justified it.
static cell AMX_NATIVE_CALL dodx_reset_aim_vis_stats(AMX *amx, cell *params)
{
	int index = params[1];
	CHECK_PLAYER(index);

	GET_PLAYER_POINTER_I(index)->ktpVis.reset();
	return 1;
}

AMX_NATIVE_INFO base_Natives[] =
{
	{ "dod_wpnlog_to_name", wpnlog_to_name },
	{ "dod_wpnlog_to_id", wpnlog_to_id },

	{ "dod_get_team_score", get_team_score },
	{ "dod_get_user_score", get_user_score },
	{ "dod_get_user_class", get_user_class },
	{ "dod_get_user_weapon", get_user_weapon },
	
	{ "dod_weapon_type", dod_weapon_type },

	{ "dod_get_map_info", get_map_info },
	{ "dod_user_kill", user_kill },
	{ "dod_get_pronestate", get_user_pronestate },

	{ "xmod_get_wpnname", get_weapon_name },
	{ "xmod_get_wpnlogname", get_weapon_logname },
	{ "xmod_is_melee_wpn", is_melee },
	{ "xmod_get_maxweapons", get_maxweapons },
	{ "xmod_get_stats_size", get_stats_size },
	{ "xmod_is_custom_wpn", is_custom },
  
	{ "register_statsfwd",register_forward },

	// Custom Weapon Support
	{ "custom_weapon_add", register_cwpn }, // name,melee,logname
	{ "custom_weapon_dmg", cwpn_dmg },
	{ "custom_weapon_shot", cwpn_shot },

	//****************************************

	// KTP: Disabled - use core AMXX get_user_team to avoid crash in extension mode
	// { "get_user_team", get_user_team },
	{ "get_weaponname", get_weapon_name },
	{ "get_user_weapon", get_user_weapon },
	{ "dod_get_user_team", dod_get_user_team },
	{ "dod_get_wpnname", get_weapon_name },
	{ "dod_get_wpnlogname", get_weapon_logname },
	{ "dod_is_melee", is_melee },

	{"dod_set_model",		dod_set_model},
	{"dod_set_body_number",	dod_set_body},
	{"dod_clear_model",		dod_clear_model},
	{"dod_set_weaponlist",	dod_weaponlist},

	// KTP: Scoreboard team name (extension mode compatible)
	{"dodx_set_pl_teamname", dodx_set_pl_teamname},

	// KTP: Per-player score / deaths pdata read+write (extension mode compatible)
	// Backs the mid-match disconnect/reconnect score-persistence path in
	// KTPMatchHandler; the AMXX/ReAPI set_member route requires CS-only
	// ReGameDll and crashes on DoD.
	{"dodx_set_user_deaths", dodx_set_user_deaths},
	{"dodx_get_user_deaths", dodx_get_user_deaths},
	{"dodx_set_user_score",  dodx_set_user_score},
	{"dodx_get_user_score",  dodx_get_user_score},
	{"dodx_get_observed_deaths", dodx_get_observed_deaths},  // engine-authoritative ground truth for the validation gate
	{"dodx_broadcast_scoreboard", dodx_broadcast_scoreboard},  // safe ScoreShort broadcast (no AMX message_begin)

	// KTP: Gamerules score modification (scoreboard scores)
	{"dodx_set_team_score", dodx_set_team_score},
	{"dodx_get_team_score", dodx_get_team_score},
	{"dodx_has_gamerules", dodx_has_gamerules},
	{"dodx_broadcast_team_score", dodx_broadcast_team_score},

	// KTP: Engine-authoritative half clock (closed-loop broadcast overlay time)
	{"dodx_get_round_time", dodx_get_round_time},

	// KTP: Engine-authoritative territorial scoring tick (closed-loop broadcast
	// overlay countdown; the DoD client never shows this clock)
	{"dodx_get_score_tick_time", dodx_get_score_tick_time},
	{"dodx_get_score_tick_period", dodx_get_score_tick_period},

	// KTP: Custom scoreboard team names
	{"dodx_set_scoreboard_team_name", dodx_set_scoreboard_team_name},

	// KTP: Grenade ammo manipulation (extension mode compatible)
	{"dodx_set_grenade_ammo", dodx_set_grenade_ammo},
	{"dodx_get_grenade_ammo", dodx_get_grenade_ammo},
	{"dodx_get_grenade_ammo_index", dodx_get_grenade_ammo_index},

	// KTP: Noclip control (extension mode compatible)
	{"dodx_set_user_noclip", dodx_set_user_noclip},
	{"dodx_get_user_movetype", dodx_get_user_movetype},
	{"dodx_debug_player_state", dodx_debug_player_state},
	{"dodx_send_ammox", dodx_send_ammox},

	// KTP: Give grenade weapon (for practice mode infinite grenades)
	{"dodx_give_grenade", dodx_give_grenade},
	{"dodx_strip_grenade", dodx_strip_grenade},
	{"dodx_debug_dump_ammo", dodx_debug_dump_ammo},

	// KTP: Player class/team/position manipulation (hostname broadcast state restoration)
	{"dodx_set_user_class", dodx_set_user_class},
	{"dodx_set_user_team", dodx_set_user_team},
	{"dodx_get_user_origin", dodx_get_user_origin},
	{"dodx_get_user_bounds", dodx_get_user_bounds},
	{"dodx_set_user_origin", dodx_set_user_origin},
	{"dodx_get_user_angles", dodx_get_user_angles},
	{"dodx_set_user_angles", dodx_set_user_angles},

	// KTP: TEST-ONLY forward dispatch primitives for Tier 2 integration tests.
	// Production plugins MUST NOT call these. See function bodies above for
	// rationale + safety analysis.
	{"dodx_test_dispatch_weapon_fire",       dodx_test_dispatch_weapon_fire},
	{"dodx_test_dispatch_damage",            dodx_test_dispatch_damage},
	{"dodx_test_dispatch_grenade_explosion", dodx_test_dispatch_grenade_explosion},
	{"dodx_test_dispatch_score",             dodx_test_dispatch_score},
	{"dodx_test_dispatch_cp_captured",       dodx_test_dispatch_cp_captured},
	{"dodx_test_dispatch_client_spawn",      dodx_test_dispatch_client_spawn},
	{"dodx_test_dispatch_changeteam",        dodx_test_dispatch_changeteam},
	{"dodx_test_dispatch_changeclass",       dodx_test_dispatch_changeclass},
	{"dodx_test_dispatch_client_death",      dodx_test_dispatch_client_death},
	{"dodx_test_dispatch_stats_flush",       dodx_test_dispatch_stats_flush},

	// KTP: Round-timer diagnostics (test/diagnostic only — see impl comments)
	{"dodx_test_dump_round_timers",          dodx_test_dump_round_timers},
	{"dodx_test_dump_score_tick",            dodx_test_dump_score_tick},
	{"dodx_test_scan_gamerules",             dodx_test_scan_gamerules},

	// KTP: shadow-mode aim/movement counters (blind audit tier 2.4 / 2.6)
	{"dodx_get_aim_stats",                   dodx_get_aim_stats},
	{"dodx_get_aim_window",                  dodx_get_aim_window},
	{"dodx_reset_aim_stats",                 dodx_reset_aim_stats},

	// KTP: per-shot aim geometry (blind audit tier 2.3)
	{"dodx_get_shot_geom",                   dodx_get_shot_geom},

	// KTP: aim-vs-transmission counters (blind audit tier 2.7)
	{"dodx_get_aim_vis_stats",               dodx_get_aim_vis_stats},
	{"dodx_reset_aim_vis_stats",             dodx_reset_aim_vis_stats},

	///*******************
	{ NULL, NULL }
};
