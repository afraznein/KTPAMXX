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

// KTP: Match ID for HLStatsX integration
// Format: "KTP-{timestamp}-{mapname}" set by KTPMatchHandler
char g_szMatchId[64] = "";

// KTP: Forward for stats flush notification
int iFFlushStats = -1;

// KTP: Get current match ID (for stats_logging to include in logs)
const char* DODX_GetMatchId()
{
	return g_szMatchId;
}

static cell AMX_NATIVE_CALL get_user_astats(AMX *amx, cell *params) /* 6 param */
{
	// KTP: gpGlobals can be NULL during map change in extension mode
	if (!gpGlobals)
		return 0;

	int index = params[1];
	if (index < 0 || index > gpGlobals->maxClients)
		return 0;
	int attacker = params[2];
	if (attacker < 0 || attacker > gpGlobals->maxClients)
		return 0;
	CPlayer* pPlayer = &players[index];
	// KTP: Safety check - verify player is valid before accessing stats
	if (!pPlayer->ingame || !pPlayer->pEdict || pPlayer->pEdict->free)
		return 0;
	if (pPlayer->attackers[attacker].hits){
		cell *cpStats = MF_GetAmxAddr(amx,params[3]);
		cell *cpBodyHits = MF_GetAmxAddr(amx,params[4]);
		CPlayer::PlayerWeapon* stats = &pPlayer->attackers[attacker];
		cpStats[0] = stats->kills;
		cpStats[1] = stats->deaths;
		cpStats[2] = stats->hs;
		cpStats[3] = stats->tks;
		cpStats[4] = stats->shots;
		cpStats[5] = stats->hits;
		cpStats[6] = stats->damage;
		for(int i = 1; i < 8; ++i)
			cpBodyHits[i] = stats->bodyHits[i];
		if (params[6] && attacker && stats->name)
			MF_SetAmxString(amx,params[5],stats->name,params[6]);
		return 1;
	}
	return 0;
}

static cell AMX_NATIVE_CALL get_user_vstats(AMX *amx, cell *params) /* 6 param */
{
	// KTP: gpGlobals can be NULL during map change in extension mode
	if (!gpGlobals)
		return 0;

	int index = params[1];
	if (index < 0 || index > gpGlobals->maxClients)
		return 0;
	int victim = params[2];
	if (victim < 0 || victim > gpGlobals->maxClients)
		return 0;
	CPlayer* pPlayer = &players[index];
	// KTP: Safety check - verify player is valid before accessing stats
	if (!pPlayer->ingame || !pPlayer->pEdict || pPlayer->pEdict->free)
		return 0;
	if (pPlayer->victims[victim].hits){
		cell *cpStats = MF_GetAmxAddr(amx,params[3]);
		cell *cpBodyHits = MF_GetAmxAddr(amx,params[4]);
		CPlayer::PlayerWeapon* stats = &pPlayer->victims[victim];
		cpStats[0] = stats->kills;
		cpStats[1] = stats->deaths;
		cpStats[2] = stats->hs;
		cpStats[3] = stats->tks;
		cpStats[4] = stats->shots;
		cpStats[5] = stats->hits;
		cpStats[6] = stats->damage;
		for(int i = 1; i < 8; ++i)
			cpBodyHits[i] = stats->bodyHits[i];
		if (params[6] && victim && stats->name)
			MF_SetAmxString(amx,params[5],stats->name,params[6]);
		return 1;
	}
	return 0;
}

static cell AMX_NATIVE_CALL get_user_wlstats(AMX *amx, cell *params) /* 4 param */ // DEC-Weapon (round) stats (end)
{
	// KTP: gpGlobals can be NULL during map change in extension mode
	if (!gpGlobals)
		return 0;

	int index = params[1];
	if (index < 1 || index > gpGlobals->maxClients)
		return 0;
	int weapon = params[2];
	if (weapon<0||weapon>=DODMAX_WEAPONS){
		MF_LogError(amx, AMX_ERR_NATIVE, "Invalid weapon id %d", weapon);
		return 0;
	}
	CPlayer* pPlayer = &players[index];
	// KTP: Safety check - verify player is valid before accessing stats
	if (!pPlayer->ingame || !pPlayer->pEdict || pPlayer->pEdict->free)
		return 0;
	if (pPlayer->weaponsLife[weapon].shots){
		cell *cpStats = MF_GetAmxAddr(amx,params[3]);
		cell *cpBodyHits = MF_GetAmxAddr(amx,params[4]);
		Stats* stats = &pPlayer->weaponsLife[weapon];
		cpStats[0] = stats->kills;
		cpStats[1] = stats->deaths;
		cpStats[2] = stats->hs;
		cpStats[3] = stats->tks;
		cpStats[4] = stats->shots;
		cpStats[5] = stats->hits;
		cpStats[6] = stats->damage;
		cpStats[7] = stats->points;
		for(int i = 1; i < 8; ++i)
			cpBodyHits[i] = stats->bodyHits[i];
		return 1;
	}
	return 0;
}

static cell AMX_NATIVE_CALL get_user_wrstats(AMX *amx, cell *params) /* 4 param */ // DEC-Weapon (round) stats (end)
{
	// KTP: gpGlobals can be NULL during map change in extension mode
	if (!gpGlobals)
		return 0;

	int index = params[1];
	if (index < 1 || index > gpGlobals->maxClients)
		return 0;
	int weapon = params[2];
	if (weapon<0||weapon>=DODMAX_WEAPONS){
		MF_LogError(amx, AMX_ERR_NATIVE, "Invalid weapon id %d", weapon);
		return 0;
	}
	CPlayer* pPlayer = &players[index];
	// KTP: Safety check - verify player is valid before accessing stats
	if (!pPlayer->ingame || !pPlayer->pEdict || pPlayer->pEdict->free)
		return 0;
	if (pPlayer->weaponsLife[weapon].shots){
		cell *cpStats = MF_GetAmxAddr(amx,params[3]);
		cell *cpBodyHits = MF_GetAmxAddr(amx,params[4]);
		Stats* stats = &pPlayer->weaponsRnd[weapon];
		cpStats[0] = stats->kills;
		cpStats[1] = stats->deaths;
		cpStats[2] = stats->hs;
		cpStats[3] = stats->tks;
		cpStats[4] = stats->shots;
		cpStats[5] = stats->hits;
		cpStats[6] = stats->damage;
		cpStats[7] = stats->points;
		for(int i = 1; i < 8; ++i)
			cpBodyHits[i] = stats->bodyHits[i];
		return 1;
	}
	return 0;
}

static cell AMX_NATIVE_CALL get_user_wstats(AMX *amx, cell *params) /* 4 param */
{
	int index = params[1];

	// KTP: gpGlobals can be NULL during map change in extension mode - return safely
	if (!gpGlobals) {
		return 0;
	}

	// KTP: Range check
	if (index < 1 || index > gpGlobals->maxClients) {
		return 0;
	}

	CPlayer* pPlayer = &players[index];

	// KTP: Safety check - player may not be valid during disconnect in extension mode
	if (!pPlayer->ingame || !pPlayer->pEdict || pPlayer->pEdict->free) {
		return 0;
	}

	int weapon = params[2];
	if (weapon<0||weapon>=DODMAX_WEAPONS){
		MF_LogError(amx, AMX_ERR_NATIVE, "Invalid weapon id %d", weapon);
		return 0;
	}

	if (pPlayer->weapons[weapon].shots){
		cell *cpStats = MF_GetAmxAddr(amx,params[3]);
		cell *cpBodyHits = MF_GetAmxAddr(amx,params[4]);
		CPlayer::PlayerWeapon* stats = &pPlayer->weapons[weapon];
		cpStats[0] = stats->kills;
		cpStats[1] = stats->deaths;
		cpStats[2] = stats->hs;
		cpStats[3] = stats->tks;
		cpStats[4] = stats->shots;
		cpStats[5] = stats->hits;
		cpStats[6] = stats->damage;
		cpStats[7] = stats->points;
		for(int i = 1; i < 8; ++i)
			cpBodyHits[i] = stats->bodyHits[i];
		return 1;
	}
	return 0;
}

static cell AMX_NATIVE_CALL reset_user_wstats(AMX *amx, cell *params) /* 6 param */
{
	// KTP: gpGlobals can be NULL during map change in extension mode
	if (!gpGlobals)
		return 0;

	int index = params[1];
	if (index < 1 || index > gpGlobals->maxClients)
		return 0;
	CPlayer* pPlayer = &players[index];
	// KTP: Safety check - verify player is valid before accessing stats
	if (!pPlayer->ingame || !pPlayer->pEdict || pPlayer->pEdict->free)
		return 0;
	pPlayer->restartStats();
	return 1;
}

static cell AMX_NATIVE_CALL get_user_stats(AMX *amx, cell *params) /* 3 param */
{
	// KTP: gpGlobals can be NULL during map change in extension mode
	if (!gpGlobals)
		return 0;

	int index = params[1];
	if (index < 1 || index > gpGlobals->maxClients)
		return 0;
	CPlayer* pPlayer = &players[index];
	// KTP: Safety check - verify player is valid before accessing stats
	if (!pPlayer->ingame || !pPlayer->pEdict || pPlayer->pEdict->free)
		return 0;
	if ( pPlayer->ingame ){
		cell *cpStats = MF_GetAmxAddr(amx,params[2]);
		cell *cpBodyHits = MF_GetAmxAddr(amx,params[3]);
		// KTP: rank may be NULL in extension mode - return zeros
		if (!pPlayer->rank) {
			for(int i = 0; i < 9; ++i)
				cpStats[i] = 0;
			for(int i = 0; i < 8; ++i)
				cpBodyHits[i] = 0;
			return 0;
		}
		cpStats[0] = pPlayer->rank->kills;
		cpStats[1] = pPlayer->rank->deaths;
		cpStats[2] = pPlayer->rank->hs;
		cpStats[3] = pPlayer->rank->tks;
		cpStats[4] = pPlayer->rank->shots;
		cpStats[5] = pPlayer->rank->hits;
		cpStats[6] = pPlayer->rank->damage;
		cpStats[7] = pPlayer->rank->points;
		cpStats[8] = pPlayer->rank->getPosition();
		for(int i = 1; i < 8; ++i)
			cpBodyHits[i] = pPlayer->rank->bodyHits[i];
		return pPlayer->rank->getPosition();
	}
	return 0;

}

static cell AMX_NATIVE_CALL get_user_lstats(AMX *amx, cell *params) /* 3 param */
{
	// KTP: gpGlobals can be NULL during map change in extension mode
	if (!gpGlobals)
		return 0;

	int index = params[1];
	if (index < 1 || index > gpGlobals->maxClients)
		return 0;
	CPlayer* pPlayer = &players[index];
	// KTP: Safety check - verify player is valid before accessing stats
	if (!pPlayer->ingame || !pPlayer->pEdict || pPlayer->pEdict->free)
		return 0;
	if (pPlayer->ingame){
		cell *cpStats = MF_GetAmxAddr(amx,params[2]);
		cell *cpBodyHits = MF_GetAmxAddr(amx,params[3]);
		cpStats[0] = pPlayer->life.kills;
		cpStats[1] = pPlayer->life.deaths;
		cpStats[2] = pPlayer->life.hs;
		cpStats[3] = pPlayer->life.tks;
		cpStats[4] = pPlayer->life.shots;
		cpStats[5] = pPlayer->life.hits;
		cpStats[6] = pPlayer->life.damage;
		cpStats[7] = pPlayer->life.points;
		for(int i = 1; i < 8; ++i)
			cpBodyHits[i] = pPlayer->life.bodyHits[i];
		return 1;
	}
	return 0;
}

static cell AMX_NATIVE_CALL get_user_rstats(AMX *amx, cell *params) /* 3 param */
{
	// KTP: gpGlobals can be NULL during map change in extension mode
	if (!gpGlobals)
		return 0;

	int index = params[1];
	if (index < 1 || index > gpGlobals->maxClients)
		return 0;
	CPlayer* pPlayer = &players[index];
	// KTP: Safety check - verify player is valid before accessing stats
	if (!pPlayer->ingame || !pPlayer->pEdict || pPlayer->pEdict->free)
		return 0;
	if (pPlayer->ingame){
		cell *cpStats = MF_GetAmxAddr(amx,params[2]);
		cell *cpBodyHits = MF_GetAmxAddr(amx,params[3]);
		cpStats[0] = pPlayer->round.kills;
		cpStats[1] = pPlayer->round.deaths;
		cpStats[2] = pPlayer->round.hs;
		cpStats[3] = pPlayer->round.tks;
		cpStats[4] = pPlayer->round.shots;
		cpStats[5] = pPlayer->round.hits;
		cpStats[6] = pPlayer->round.damage;
		cpStats[7] = pPlayer->round.points;
		for(int i = 1; i < 8; ++i)
			cpBodyHits[i] = pPlayer->round.bodyHits[i];
		return 1;
	}
	return 0;
}

static cell AMX_NATIVE_CALL get_stats(AMX *amx, cell *params) /* 3 param */
{
	
	int index = params[1] + 1;
	for(RankSystem::iterator a = g_rank.front(); a ; --a){
		if ((*a).getPosition() == index)  {
			cell *cpStats = MF_GetAmxAddr(amx,params[2]);
			cell *cpBodyHits = MF_GetAmxAddr(amx,params[3]);
			cpStats[0] = (*a).kills;
			cpStats[1] = (*a).deaths;
			cpStats[2] = (*a).hs;
			cpStats[3] = (*a).tks;
			cpStats[4] = (*a).shots;
			cpStats[5] = (*a).hits;
			cpStats[6] = (*a).damage;
			cpStats[7] = (*a).points;
			cpStats[8] = (*a).getPosition();
			MF_SetAmxString(amx,params[4],(*a).getName(),params[5]);
			for(int i = 1; i < 8; ++i)
				cpBodyHits[i] = (*a).bodyHits[i];
			return --a ? (*a).getPosition() : 0;
		}
	}
	
	return 0;
}

static cell AMX_NATIVE_CALL get_statsnum(AMX *amx, cell *params)
{
	return g_rank.getRankNum();
}

// ============================================================================
// KTP: HLStatsX Integration Natives
// ============================================================================

// Native: dodx_flush_all_stats()
// Purpose: Fire forward to notify stats_logging.sma to log all pending player stats
// This allows stats_logging to write weaponstats for all players (warmup stats flush)
// Returns: Number of players for which the forward was fired
static cell AMX_NATIVE_CALL dodx_flush_all_stats(AMX *amx, cell *params)
{
	// KTP: Safety check - gpGlobals must be valid
	if (!gpGlobals)
		return 0;

	// KTP: Check if forward is registered
	if (iFFlushStats < 0)
	{
		MF_LogError(amx, AMX_ERR_NATIVE, "dodx_flush_all_stats: dod_stats_flush forward not registered");
		return 0;
	}

	int count = 0;
	int maxClients = gpGlobals->maxClients;
	if (maxClients < 1 || maxClients > 32)
		maxClients = 32;

	// Fire forward for each connected player
	// stats_logging.sma will handle the actual logging
	for (int i = 1; i <= maxClients; i++)
	{
		CPlayer* pPlayer = GET_PLAYER_POINTER_I(i);
		if (pPlayer && pPlayer->ingame && pPlayer->pEdict && !pPlayer->pEdict->free)
		{
			// Fire forward: dod_stats_flush(id)
			// stats_logging.sma should register for this and log weaponstats
			MF_ExecuteForward(iFFlushStats, i);
			count++;
		}
	}

	return count;
}

// Native: dodx_reset_all_stats()
// Purpose: Reset all accumulated stats for all connected players
// This clears weapons[], attackers[], victims[], weaponsLife[], weaponsRnd[], life, round
// Returns: Number of players reset
static cell AMX_NATIVE_CALL dodx_reset_all_stats(AMX *amx, cell *params)
{
	// KTP: Safety check - gpGlobals must be valid
	if (!gpGlobals)
		return 0;

	int count = 0;
	int maxClients = gpGlobals->maxClients;
	if (maxClients < 1 || maxClients > 32)
		maxClients = 32;

	for (int i = 1; i <= maxClients; i++)
	{
		CPlayer* pPlayer = GET_PLAYER_POINTER_I(i);
		if (pPlayer && pPlayer->ingame && pPlayer->pEdict && !pPlayer->pEdict->free)
		{
			// restartStats(true) clears all stats including weapons[], victims[], attackers[]
			pPlayer->restartStats(true);
			count++;
		}
	}

	return count;
}

// Native: dodx_set_match_id(const matchId[])
// Purpose: Set the current match context for stats logging
// When set, stats_logging.sma should include this in weaponstats log lines
// Pass empty string to clear the match context
// Returns: 1 on success
static cell AMX_NATIVE_CALL dodx_set_match_id(AMX *amx, cell *params)
{
	int len;
	const char* matchId = MF_GetAmxString(amx, params[1], 0, &len);

	if (matchId)
	{
		// Copy match ID (or clear if empty)
		strncpy(g_szMatchId, matchId, sizeof(g_szMatchId) - 1);
		g_szMatchId[sizeof(g_szMatchId) - 1] = '\0';
	}
	else
	{
		g_szMatchId[0] = '\0';
	}

	return 1;
}

// Native: dodx_get_match_id(output[], maxlen)
// Purpose: Get the current match ID (for stats_logging to include in logs)
// Returns: Length of match ID string, 0 if not set
static cell AMX_NATIVE_CALL dodx_get_match_id(AMX *amx, cell *params)
{
	if (g_szMatchId[0] == '\0')
		return 0;

	return MF_SetAmxString(amx, params[1], g_szMatchId, params[2]);
}

AMX_NATIVE_INFO stats_Natives[] = {
	{ "get_stats",      get_stats},
	{ "get_statsnum",   get_statsnum},
	{ "get_user_astats",  get_user_astats },
	{ "get_user_rstats",  get_user_rstats }, // round stats , cleared after RoundState = 1
	{ "get_user_lstats",  get_user_lstats }, // from spawn to spawn stats
	{ "get_user_stats",   get_user_stats },
	{ "get_user_vstats",  get_user_vstats },
	{ "get_user_wlstats",  get_user_wlstats}, // DEC-Weapon(Life) Stats
	{ "get_user_wrstats",  get_user_wrstats}, // DEC-Weapon(Round) Stats
	{ "get_user_wstats",  get_user_wstats},
	{ "reset_user_wstats",  reset_user_wstats },

	// KTP: HLStatsX integration natives
	{ "dodx_flush_all_stats",  dodx_flush_all_stats },
	{ "dodx_reset_all_stats",  dodx_reset_all_stats },
	{ "dodx_set_match_id",     dodx_set_match_id },
	{ "dodx_get_match_id",     dodx_get_match_id },

	{ NULL, NULL }
};

