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

// KTP: ReHLDS API includes for extension mode support
// These are relative to the public/ directory which contains the module SDK
#include "../../../public/resdk/common/hookchains.h"
#include "../../../public/resdk/engine/rehlds_api.h"
#include "../../../public/resdk/engine/IMessageManager.h"

// KTP: Extension mode state
static bool g_bExtensionMode = false;
static IRehldsHookchains* g_pRehldsHookchains = nullptr;
static IMessageManager* g_pMessageManager = nullptr;

#if defined(__linux__) || defined(__APPLE__)
// KTP: m_rgAmmo base adjust. +5 is measured off dod_i386.so md5 4f4727b2...
// (AmmoInventory indexes byte 0x474 = int 285 = 280 + 5), not auto-detected —
// the old detector scored candidate offsets at a hardcoded ammo index that was
// itself wrong, so it could confirm either answer. Override via
// addons/ktpamx/configs/dodx.ini: pdata_offset = 4 or 5.
int g_iLinuxPdataOffsetAdjust = 5;

// KTP: SCORE/DEATHS need their own adjust — same binary, two field families,
// two different shifts: m_rgAmmo sits at +5 and score/deaths at +4 in
// dod_i386.so md5 4f4727b2... See dodx.h SCORE/DEATHS section + research note
// KTPMatchHandler/research/OFFSETS_RESEARCH_2026-05-21.md.
int g_iScoreDeathsOffsetAdjust = 4;
#endif

// KTP: Forward declarations for ReHLDS hook handlers
static void DODX_OnTraceLine(IVoidHookChain<const float *, const float *, int, edict_t *, TraceResult *> *chain,
                              const float *v1, const float *v2, int fNoMonsters, edict_t *e, TraceResult *ptr);
static void DODX_OnSetClientKeyValue(IVoidHookChain<int, char *, const char *, const char *> *chain,
                                      int clientIndex, char *infobuffer, const char *key, const char *value);
static int DODX_OnRegUserMsg(IHookChain<int, const char *, int> *chain, const char *pszName, int iSize);
static void DODX_OnInitObjMessage(IVoidHookChain<IMessage *> *chain, IMessage *msg);
static void DODX_OnClientConnected(IVoidHookChain<IGameClient *> *chain, IGameClient *client);
static void DODX_OnSV_Spawn_f(IVoidHookChain<> *chain);
static void DODX_OnSV_DropClient(IVoidHookChain<IGameClient *, bool, const char *> *chain, IGameClient *client, bool crash, const char *reason);
static void DODX_OnChangelevel(IVoidHookChain<const char *, const char *> *chain, const char *s1, const char *s2);
static void DODX_OnSV_ActivateServer(IVoidHookChain<int> *chain, int runPhysics);

// KTP: Forward declarations for extension mode setup/cleanup functions
static bool DODX_SetupExtensionHooks();
static void DODX_CleanupExtensionHooks();
static char *DODX_LoadBSPEntityLump();
static void DODX_ReadBSPMapInfo();
void DODX_RegisterMessageHooks();
static void DODX_InitCPFromEntities();

funEventCall modMsgsEnd[MAX_REG_MSGS];
funEventCall modMsgs[MAX_REG_MSGS];
void (*function)(void*);
void (*endfunction)(void*);
CPlayer* mPlayer;
CPlayer players[33];
CMapInfo g_map;
CObjective mObjects;

// KTP: First edict pointer for ENTINDEX_SAFE
// Initialized in ServerActivate_Post to enable safe entity index calculation
edict_t* g_pFirstEdict = nullptr;

// KTP: Server active flag - prevents message processing during map changes
bool g_bServerActive = false;
bool g_cpOrderingFinalized = false;  // KTP: Set true once Client_InitObj has reordered mObjects to match DLL's SetObj id space.

// (g_bCPFromInitObj was removed — entity scan + BSP reorder is the sole CP ordering path)

bool rankBots;
int mState;
int mDest;
int mCurWpnEnd;
int mPlayerIndex;

int AlliesScore;
int AxisScore;

int iFDamage = -1;
int iFDeath = -1;
int iFScore = -1;
int iFSpawnForward = -1;
int iFTeamForward = -1;
int iFClassForward = -1;
int iFScopeForward = -1;
int iFProneForward = -1;
int iFWpnPickupForward = -1;
int iFCurWpnForward = -1;
int iFWeaponFire = -1;     // KTP: Per-shot primary-attack actuation (every fire, incl. pure misses)
int iFGrenadeExplode = -1;
int iFRocketExplode = -1;
int iFObjectTouched = -1;
int iFStaminaForward = -1;
int iFDamagePre = -1;  // KTP: Pre-damage forward for damage modification
int iFInitCP = -1;     // KTP: CP init forward
int iFCPCaptured = -1; // KTP: CP ownership change forward
int iFScoreEvent = -1; // KTP: Enriched score event with CP context

int g_lastCapturedCP = -1;      // KTP: Last CP from SetObj
float g_lastCapturedTime = 0.0f; // KTP: Time of last SetObj

int gmsgCurWeapon;
int gmsgCurWeaponEnd;
int gmsgHealth;
int gmsgResetHUD;
int gmsgObjScore;
int gmsgRoundState;
int gmsgTeamScore;
int gmsgScoreShort;
int gmsgPTeam;
int gmsgAmmoX;
int gmsgAmmoShort;
int gmsgSetFOV;
int gmsgSetFOV_End;
int gmsgObject;
int gmsgObject_End;
int gmsgPStatus;
int gmsgTeamInfo;  // KTP: For scoreboard team name refresh
int gmsgInitObj;   // KTP: CP tracking
int gmsgSetObj;    // KTP: CP tracking
int gmsgDeathMsg;  // KTP: Suicide / world-kill detection (no Damage path)
int gmsgWeaponList;  // KTP: per-map ammo-index registry

// KTP: Ammo-type index the DLL assigned each weapon on this map, or -1 until
// observed. Process-wide, so it MUST be cleared per map — a carried-over index
// would address another ammo type's counter with no error anywhere.
int g_ammoIndexByWeapon[DODMAX_WEAPONS];
int g_ammoRegistryEpoch = 0;

// Where DoD's own W_Precache leaves the grenades. It precaches all 31 weapons
// unconditionally in a fixed order — every nationality on every map — so these
// are invariant, which is the opposite of what issue #15 assumed. Measured live
// on six maps incl. dod_anzio and dod_harrington by reading AmmoInfoArray out of
// the running DLL: 9/11 on all six. Used only until this map's WeaponList lands.
static int DODX_DefaultGrenadeAmmoIndex(int weaponId)
{
	if (weaponId == 13) return 9;   // ammo_agrens
	if (weaponId == 14) return 11;  // ammo_ggrens
	return -1;
}

void DODX_ClearAmmoRegistry()
{
	for (int i = 0; i < DODMAX_WEAPONS; ++i)
		g_ammoIndexByWeapon[i] = -1;

	++g_ammoRegistryEpoch;
}

// DODW_MILLS_BOMB is DODX's own id for the British hand grenade — the DLL links
// no weapon_mills_bomb and registers no ammo type for it.
static int DODX_NormalizeGrenadeType(int grenadeType)
{
	if (grenadeType == 36)
		return 13;
	if (grenadeType == 13 || grenadeType == 14)
		return grenadeType;
	return -1;
}

int DODX_GrenadeAmmoIndex(int grenadeType)
{
	int weaponId = DODX_NormalizeGrenadeType(grenadeType);
	if (weaponId < 0)
		return -1;

	int observed = g_ammoIndexByWeapon[weaponId];
	return observed >= 0 ? observed : DODX_DefaultGrenadeAmmoIndex(weaponId);
}

// The tripwire the 9/11 constants never had: if the precache order ever stops
// being fixed, this is what says so instead of corrupting an ammo type in silence.
void DODX_CheckAmmoIndexDrift(int weaponId, int slot)
{
	int expected = DODX_DefaultGrenadeAmmoIndex(weaponId);
	if (expected < 0 || expected == slot)
		return;

	static int s_driftEpoch = -1;
	if (s_driftEpoch == g_ammoRegistryEpoch)
		return;

	s_driftEpoch = g_ammoRegistryEpoch;
	MF_Log("[DODX] weapon %d ammo slot is %d on this map, not the fixed-order %d — "
		"W_Precache is no longer invariant, re-check the grenade natives",
		weaponId, slot, expected);
}

void DODX_ObserveGrenadeAmmoIndex(int grenadeType, int slot)
{
	int weaponId = DODX_NormalizeGrenadeType(grenadeType);
	if (weaponId < 0 || slot < 0 || slot >= DODX_MAX_AMMO_SLOTS)
		return;

	int observed = g_ammoIndexByWeapon[weaponId];
	if (observed == slot)
		return;

	if (observed < 0)
	{
		g_ammoIndexByWeapon[weaponId] = slot;
		DODX_CheckAmmoIndexDrift(weaponId, slot);
		return;
	}

	// Two independent readings of the same registry disagreeing means one of the
	// two mechanisms has stopped telling the truth — worth a line, not a silent
	// overwrite. WeaponList is the earlier and broader source, so it wins.
	static int s_warnedEpoch = -1;
	if (s_warnedEpoch != g_ammoRegistryEpoch)
	{
		s_warnedEpoch = g_ammoRegistryEpoch;
		MF_Log("[DODX] ammo slot for weapon %d: pickup probe says %d, WeaponList said %d — keeping %d",
			weaponId, slot, observed, observed);
	}
}

RankSystem g_rank;
Grenades g_grenades;

// KTP: Gamerules access for scoreboard score modification
IGameConfig *g_pCommonConfig = nullptr;
IGameConfig *g_pGamerulesConfig = nullptr;
void **g_pGameRulesAddress = nullptr;
int g_iTeamScoreOffset = 56;  // Default offset from gamedata, may be overridden

// KTP: DoD round-timer field offsets (dodx_get_round_time + diagnostics).
// Resolved from shipped gamedata in OnPluginsLoaded; -1 = unresolved, the
// corresponding read is skipped (natives fail soft, never crash).
int g_iGrRoundTimeOffset = -1;       // CDoDTeamPlay::m_flRoundTime (float)
int g_iParaTimerPtrOffset = -1;      // CDoDTeamPlay::m_pParaTimer (CDodParaRoundTimer*)
int g_iParaRoundTimerOffset = -1;    // CDodParaRoundTimer::m_fRoundTimer (float)
int g_iParaBTimerOffset = -1;        // CDodParaRoundTimer::m_bTimer (bool)
int g_iRTimerRoundTimeOffset = -1;   // CDodRoundTimer::m_fRoundTime (float)
int g_iRTimerLengthOffset = -1;      // CDodRoundTimer::m_fTimerLength (float)
int g_iRTimerBTimerOffset = -1;      // CDodRoundTimer::m_bTimer (bool)

// KTP: DoD half-clock members (dodx_get_round_time). Empirically confirmed
// against a live mp_clan_restartround go-live via dodx_test_scan_gamerules
// (2026-07-11): m_flDoDMapTime is the half-clock base the client HUD counts
// from (0 from map load on pubs; rewritten to the restart-completion gametime
// by a clan restart), m_flRestartRoundTime is the scheduled restart target
// (request + mp_clan_timer, written when the restart is requested), and
// m_bRoundRestarting is the pending flag bridging the two.
int g_iDoDMapTimeOffset = -1;        // CDoDTeamPlay::m_flDoDMapTime (float)
int g_iRestartRoundTimeOffset = -1;  // CDoDTeamPlay::m_flRestartRoundTime (float)
int g_iRoundRestartingOffset = -1;   // CDoDTeamPlay::m_bRoundRestarting (BOOL)

// KTP: cached mp_timelimit cvar pointer for dodx_get_round_time — same
// extension-mode-safe pattern as the 2.7.22 hostname fix (string-lookup
// CVAR_GET_FLOAT is unsafe in extension-mode engine paths).
cvar_t *g_pcvarMpTimelimit = nullptr;

// KTP: CControlPointMaster members for dodx_get_score_tick_time().
//
// DoD awards periodic TERRITORIAL points — the score that ticks up while a team
// holds ground — from the map's single dod_control_point_master entity, on its
// own clock. The game client never surfaces that clock anywhere, which is why a
// broadcast overlay has to read it: m_fGivePointsTime is the absolute gametime
// of the next award and m_iGivePointsDelay is the period between awards, while
// m_bActive gates whether the master is scoring at all.
//
// UNLIKE the CDoDTeamPlay members above, these offsets DIFFER BY PLATFORM
// (windows 420/424/416 vs linux 436/440/432). Always resolve them through
// gamedata; never hardcode. Shipped in
// gamedata/common.games/entities.games/dod/offsets-ccontrolpointmaster.txt.
int g_iCPMGivePointsTimeOffset = -1;   // CControlPointMaster::m_fGivePointsTime (float)
int g_iCPMGivePointsDelayOffset = -1;  // CControlPointMaster::m_iGivePointsDelay (int)
int g_iCPMActiveOffset = -1;           // CControlPointMaster::m_bActive (BOOL)

// Resolved lazily per map by DODX_GetCPMaster(); cleared on map change so a
// freed edict from the previous map can never be read.
edict_t *g_pCPMasterEdict = nullptr;

cvar_t init_dodstats_maxsize ={"dodstats_maxsize","3500", 0 , 3500.0 };
cvar_t init_dodstats_reset ={"dodstats_reset","0"};
cvar_t init_dodstats_rank ={"dodstats_rank","0"};
cvar_t init_dodstats_rankbots ={"dodstats_rankbots","1"};
cvar_t init_dodstats_pause = {"dodstats_pause","0"};
cvar_t *dodstats_maxsize;
cvar_t *dodstats_reset;
cvar_t *dodstats_rank;
cvar_t *dodstats_rankbots;
cvar_t *dodstats_pause;

// User Messages
struct sUserMsg 
{
	const char *name;
	int* id;
	funEventCall func;
	bool endmsg;
}
g_user_msg[] = 
{
	{ "CurWeapon",	&gmsgCurWeapon,			Client_CurWeapon,		false },
	{ "CurWeapon",	&gmsgCurWeaponEnd,		Client_CurWeapon_End,	true  },
	{ "ObjScore",	&gmsgObjScore,			Client_ObjScore,		false },
	{ "RoundState",	&gmsgRoundState,		Client_RoundState,		false },
	{ "Health",		&gmsgHealth,			Client_Health_End,		true  },
	{ "ResetHUD",	&gmsgResetHUD,			Client_ResetHUD_End,	true  },
	{ "TeamScore",	&gmsgTeamScore,			Client_TeamScore,		false },
	{ "AmmoX",		&gmsgAmmoX,				Client_AmmoX,			false },
	{ "AmmoShort",	&gmsgAmmoShort,			Client_AmmoShort,		false },
	{ "SetFOV",		&gmsgSetFOV,			Client_SetFOV,			false },
	{ "SetFOV",		&gmsgSetFOV_End,		Client_SetFOV_End,		true  },
	{ "Object",		&gmsgObject,			Client_Object,			false },
	{ "Object",		&gmsgObject_End,		Client_Object_End,		true  },
	{ "PStatus",	&gmsgPStatus,			Client_PStatus,			false },
	{ "ScoreShort",	&gmsgScoreShort,		NULL,					false },
	{ "PTeam",		&gmsgPTeam,				NULL,					false },
	{ "TeamInfo",	&gmsgTeamInfo,			NULL,					false },  // KTP: For scoreboard refresh
	{ "InitObj",	&gmsgInitObj,			Client_InitObj,			false },  // KTP: CP tracking
	{ "SetObj",		&gmsgSetObj,			Client_SetObj,			false },  // KTP: CP tracking
	{ "DeathMsg",	&gmsgDeathMsg,			Client_DeathMsg,		false },  // KTP: Suicide path
	{ "WeaponList",	&gmsgWeaponList,		Client_WeaponList,		false },  // KTP: ammo-index registry
	{ 0,0,0,false }
};

const char* get_localinfo( const char* name , const char* def = 0 )
{
	const char* b = LOCALINFO( (char*)name );
	if (((b==0)||(*b==0)) && def )
		SET_LOCALINFO((char*)name,(char*)(b = def) );
	return b;
}

int RegUserMsg_Post(const char *pszName, int iSize)
{
	for (int i = 0; g_user_msg[i].name; ++i )
	{
		if(!*g_user_msg[i].id && strcmp(g_user_msg[i].name, pszName) == 0)
		{
			int id = META_RESULT_ORIG_RET(int);

			*g_user_msg[i].id = id;

			if(g_user_msg[i].endmsg)
				modMsgsEnd[id] = g_user_msg[i].func;
			else
				modMsgs[id] = g_user_msg[i].func;
			break;
		}
	}

	RETURN_META_VALUE(MRES_IGNORED, 0);
}

void ServerActivate_Post( edict_t *pEdictList, int edictCount, int clientMax ){

	// KTP: Cache the first edict for ENTINDEX_SAFE
	// pEdictList is worldspawn (index 0)
	g_pFirstEdict = pEdictList;
	g_bServerActive = true;  // KTP: Mark server as active for message processing

	rankBots = (int)dodstats_rankbots->value ? true:false;

	for( int i = 1; i <= gpGlobals->maxClients; ++i )
		GET_PLAYER_POINTER_I(i)->Init( i , pEdictList + i );

	RETURN_META(MRES_IGNORED);
}

void PlayerPreThink_Post(edict_t *pEntity) 
{
	if ( !isModuleActive() )
		RETURN_META(MRES_IGNORED);

	CPlayer *pPlayer = GET_PLAYER_POINTER(pEntity);
	if (!pPlayer->ingame)
		RETURN_META(MRES_IGNORED);

	// Zors
	pPlayer->PreThink();

	if(pPlayer->clearStats && pPlayer->clearStats < gpGlobals->time)
	{
		if(!ignoreBots(pEntity))
		{
			pPlayer->clearStats = 0.0f;
			if (pPlayer->rank)  // KTP: rank may be NULL in extension mode
				pPlayer->rank->updatePosition( &pPlayer->life );
			pPlayer->restartStats(false);
		}
	}

	if(pPlayer->clearRound && pPlayer->clearRound < gpGlobals->time)
	{
		pPlayer->clearRound = 0.0f;
		memset(static_cast<void *>(&pPlayer->round),0,sizeof(pPlayer->round));
		memset(&pPlayer->weaponsRnd,0,sizeof(pPlayer->weaponsRnd));
	}

	if (pPlayer->sendScore && pPlayer->sendScore < gpGlobals->time)
	{
		pPlayer->sendScore = 0;

		// KTP: Resolve pending CP index (ObjScore fires before SetObj)
		if (pPlayer->lastScoreCP == -2)
		{
			// Negative delta = server time restarted (map change), not a fresh capture.
			float capDelta = gpGlobals->time - g_lastCapturedTime;
			if (capDelta >= 0.0f && capDelta < 2.0f)
				pPlayer->lastScoreCP = g_lastCapturedCP;
			else
				pPlayer->lastScoreCP = -1;
		}

		MF_ExecuteForward(iFScore, pPlayer->index, pPlayer->lastScore, pPlayer->savedScore);
		if (iFScoreEvent >= 0)
			MF_ExecuteForward(iFScoreEvent, pPlayer->index, pPlayer->lastScore, (int)pPlayer->savedScore, pPlayer->lastScoreCP);
		pPlayer->lastScoreCP = -1;
	}

	RETURN_META(MRES_IGNORED);
}

void ServerDeactivate()
{
	// KTP: CRITICAL - Clear server active flag and g_pFirstEdict FIRST
	// This prevents message hooks from using stale pointers during map change
	g_bServerActive = false;
	g_pFirstEdict = nullptr;

	// KTP: Safety check - gpGlobals must be valid
	if (!gpGlobals)
	{
		RETURN_META(MRES_IGNORED);
	}

	int maxClients = gpGlobals->maxClients;
	if (maxClients < 1 || maxClients > 32)
		maxClients = 32;  // Fallback to safe default

	int i;
	for( i = 1;i<=maxClients; ++i)
	{
		CPlayer *pPlayer = GET_PLAYER_POINTER_I(i);
		if (pPlayer->ingame) pPlayer->Disconnect();
	}

	if ( (g_rank.getRankNum() >= (int)dodstats_maxsize->value) || ((int)dodstats_reset->value == 1) ) 
	{
		CVAR_SET_FLOAT("dodstats_reset",0.0);
		g_rank.clear();
	}

	// KTP: Skip rank save in extension mode (rank system is unused, avoids unnecessary file I/O)
	if (!g_bExtensionMode)
		g_rank.saveRank( MF_BuildPathname("%s",get_localinfo("dodstats") ) );

	// clear custom weapons info
	for ( i=DODMAX_WEAPONS-DODMAX_CUSTOMWPNS;i<DODMAX_WEAPONS;i++)
		weaponData[i].needcheck = false;

	g_map.Init();
	mObjects.Clear();
	g_lastCapturedCP = -1;
	g_lastCapturedTime = 0.0f;
	DODX_ClearAmmoRegistry();

	RETURN_META(MRES_IGNORED);
}

BOOL ClientConnect_Post( edict_t *pEntity, const char *pszName, const char *pszAddress, char szRejectReason[ 128 ]  )
{
	GET_PLAYER_POINTER(pEntity)->Connect(pszName,pszAddress);

	RETURN_META_VALUE(MRES_IGNORED, TRUE);
}

void ClientDisconnect( edict_t *pEntity ) 
{
	CPlayer *pPlayer = GET_PLAYER_POINTER(pEntity);

	if (pPlayer->ingame)
		pPlayer->Disconnect();

	RETURN_META(MRES_IGNORED);
}

void ClientPutInServer_Post( edict_t *pEntity ) 
{
	GET_PLAYER_POINTER(pEntity)->PutInServer();

	RETURN_META(MRES_IGNORED);
}

void ClientUserInfoChanged_Post( edict_t *pEntity, char *infobuffer ) 
{
	CPlayer *pPlayer = GET_PLAYER_POINTER(pEntity);

	const char* name = INFOKEY_VALUE(infobuffer,"name");
	const char* oldname = STRING(pEntity->v.netname);

	if ( pPlayer->ingame)
	{
		if ( strcmp(oldname,name) )
		{
			// KTP: rank may be NULL in extension mode
			if (pPlayer->rank)
			{
				if (!dodstats_rank->value)
					pPlayer->rank = g_rank.findEntryInRank( name, name );
				else
					pPlayer->rank->setName( name );
			}
		}
	}

	else if ( pPlayer->IsBot() ) 
	{
		pPlayer->Connect( name , "127.0.0.1" );
		pPlayer->PutInServer();
	}

	RETURN_META(MRES_IGNORED);
}

void MessageBegin_Post(int msg_dest, int msg_type, const float *pOrigin, edict_t *ed)
{
	// KTP: Use ENTINDEX_SAFE for consistency (also check ed->free)
	if(ed && !ed->free)
	{
		mPlayerIndex = ENTINDEX_SAFE(ed);
		mPlayer = GET_PLAYER_POINTER_I(mPlayerIndex);
	}

	else
	{
		mPlayerIndex = 0;
		mPlayer = NULL;
	}

	mDest = msg_dest;
	mState = 0;

	if ( msg_type < 0 || msg_type >= MAX_REG_MSGS )
		msg_type = 0;

	function=modMsgs[msg_type];
	endfunction=modMsgsEnd[msg_type];
	RETURN_META(MRES_IGNORED);
}

void MessageEnd_Post(void) {
	if (endfunction) (*endfunction)(NULL);
	RETURN_META(MRES_IGNORED);
}

void WriteByte_Post(int iValue) {
	if (function) (*function)((void *)&iValue);
	RETURN_META(MRES_IGNORED);
}

void WriteChar_Post(int iValue) {
	if (function) (*function)((void *)&iValue);
	RETURN_META(MRES_IGNORED);
}

void WriteShort_Post(int iValue) {
	if (function) (*function)((void *)&iValue);
	RETURN_META(MRES_IGNORED);
}

void WriteLong_Post(int iValue) {
	if (function) (*function)((void *)&iValue);
	RETURN_META(MRES_IGNORED);
}

void WriteAngle_Post(float flValue) {
	if (function) (*function)((void *)&flValue);
	RETURN_META(MRES_IGNORED);
}

void WriteCoord_Post(float flValue) {
	if (function) (*function)((void *)&flValue);
	RETURN_META(MRES_IGNORED);
}

void WriteString_Post(const char *sz) {
	if (function) (*function)((void *)sz);
	RETURN_META(MRES_IGNORED);
}

void WriteEntity_Post(int iValue) {
	if (function) (*function)((void *)&iValue);
	RETURN_META(MRES_IGNORED);
}

void TraceLine_Post(const float *v1, const float *v2, int fNoMonsters, edict_t *e, TraceResult *ptr) 
{
	if(ptr->pHit && (ptr->pHit->v.flags&(FL_CLIENT | FL_FAKECLIENT)) &&	e && (e->v.flags&(FL_CLIENT | FL_FAKECLIENT)))
	{
		GET_PLAYER_POINTER(e)->aiming = ptr->iHitgroup;
		RETURN_META(MRES_IGNORED);
	}

	if(e && e->v.owner && e->v.owner->v.flags&(FL_CLIENT | FL_FAKECLIENT))
	{
		CPlayer *pPlayer = GET_PLAYER_POINTER(e->v.owner);

		for(int i = 0;i < MAX_TRACE; i++)
		{
			// strcmp is deliberate: ALLOC_STRING does not intern in GoldSrc, so a
			// cached string_t compare never matches. That revert is what restored
			// dod_grenade_explosion and practice-mode grenade refill.
			if(strcmp(traceData[i].szName, STRING(e->v.classname)) == 0)
			{
				int grenId = (traceData[i].iId == 13 && g_map.detect_allies_country) ? 36 : traceData[i].iId;
				int rocketId = traceData[i].iId;

				if(traceData[i].iAction&ACT_NADE_SHOT)
				{
					if(traceData[i].iId == 13 && g_map.detect_allies_country)
						pPlayer->saveShot(grenId);
					else
						pPlayer->saveShot(traceData[i].iId);
				}
				
				else if(traceData[i].iAction&ACT_ROCKET_SHOT)
						pPlayer->saveShot(traceData[i].iId);

				cell position[3];
				position[0] = amx_ftoc(v2[0]);
				position[1] = amx_ftoc(v2[1]);
				position[2] = amx_ftoc(v2[2]);
				cell pos = MF_PrepareCellArray(position, 3);

				if(traceData[i].iAction&ACT_NADE_PUT)
				{
					g_grenades.put(e, traceData[i].fDel, grenId, GET_PLAYER_POINTER(e->v.owner));
					MF_ExecuteForward(iFGrenadeExplode, GET_PLAYER_POINTER(e->v.owner)->index, pos, grenId);
				}

				if(traceData[i].iAction&ACT_ROCKET_PUT)
					MF_ExecuteForward(iFRocketExplode, pPlayer->index, pos, rocketId);

				break;
			}
		}
	}
	RETURN_META(MRES_IGNORED);
}

void DispatchKeyValue_Post( edict_t *pentKeyvalue, KeyValueData *pkvd )
{
	if ( !pkvd->szClassName ){ 
		// info_doddetect
		if ( pkvd->szValue[0]=='i' && pkvd->szValue[5]=='d' ){
			g_map.pEdict = pentKeyvalue;
			g_map.initialized = true;
		}
		RETURN_META(MRES_IGNORED);
	}
	// info_doddetect
	if ( g_map.initialized && pentKeyvalue == g_map.pEdict ){
		if ( pkvd->szKeyName[0]=='d' && pkvd->szKeyName[7]=='a' ){
			if ( pkvd->szKeyName[8]=='l' ){
				switch ( pkvd->szKeyName[14] ){
				case 'c':
					g_map.detect_allies_country=atoi(pkvd->szValue);
					break;
				case 'p':
					g_map.detect_allies_paras=atoi(pkvd->szValue);
					break;
				}
			}
			else if ( pkvd->szKeyName[12]=='p' ) g_map.detect_axis_paras=atoi(pkvd->szValue);
		}
	}
	RETURN_META(MRES_IGNORED);
}

void SetClientKeyValue(int id, char *protocol, const char *type, const char *var)
{
	// ID: Number
	// protocol: \name\Sgt.MEOW\topcolor\1\bottomcolor\1\cl_lw\1\team\axis\model\axis-inf 
	// type: model
	// var: axis-inf

	// Check to see if its a player and we are setting a model
	if(strcmp(type, "model") == 0 && 
		(strcmp(var, "axis-inf") == 0 ||
		 strcmp(var, "axis-para") == 0 || 
		 strcmp(var, "us-inf") == 0 ||
		 strcmp(var, "us-para") == 0 || 
		 strcmp(var, "brit-inf") == 0))
	{
		CPlayer *pPlayer = GET_PLAYER_POINTER_I(id);
		if(!pPlayer->ingame)
			RETURN_META(MRES_IGNORED);

		if(pPlayer->setModel())
			RETURN_META(MRES_SUPERCEDE);
	}

	RETURN_META(MRES_IGNORED);
}

void OnMetaAttach()
{
	CVAR_REGISTER (&init_dodstats_maxsize);
	CVAR_REGISTER (&init_dodstats_reset);
	CVAR_REGISTER (&init_dodstats_rank);
	CVAR_REGISTER (&init_dodstats_rankbots);
	CVAR_REGISTER (&init_dodstats_pause);
	dodstats_maxsize=CVAR_GET_POINTER(init_dodstats_maxsize.name);
	dodstats_reset=CVAR_GET_POINTER(init_dodstats_reset.name);
	dodstats_rank=CVAR_GET_POINTER(init_dodstats_rank.name);
	dodstats_rankbots = CVAR_GET_POINTER(init_dodstats_rankbots.name);
	dodstats_pause = CVAR_GET_POINTER(init_dodstats_pause.name);
}

int AmxxCheckGame(const char *game)
{
	if (strcasecmp(game, "dod") == 0)
		return AMXX_GAME_OK;

	return AMXX_GAME_BAD;
}
void OnAmxxAttach()
{
	MF_AddNatives( stats_Natives );
	MF_AddNatives( base_Natives );
	MF_AddNatives( cp_Natives );

	// Zero-init would read as "ammo slot 0", a real slot; unknown must be -1.
	DODX_ClearAmmoRegistry();

	// KTP: Check if running in extension mode (without Metamod)
	if (MF_IsExtensionMode && MF_IsExtensionMode())
	{
		g_bExtensionMode = true;
		MF_PrintSrvConsole("[DODX] Running in ReHLDS extension mode.\n");

		// Setup ReHLDS hooks for extension mode
		DODX_SetupExtensionHooks();

		// Skip engine-dependent initialization - will be done in OnPluginsLoaded
		// NOTE: Config file loading moved to OnPluginsLoaded() because MF_BuildPathnameR
		// doesn't work correctly during OnAmxxAttach() in extension mode
		// NOTE: Cvar registration moved to OnPluginsLoaded() because engine
		// function pointers may not be ready yet in OnAmxxAttach for extension mode
		return;
	}

	// Non-extension mode: engine is ready, do normal init
	const char* path =  get_localinfo("dodstats_score","addons/amxmodx/data/dodstats.amxx");

	if ( path && *path )
	{
		char error[128];
		g_rank.loadCalc( MF_BuildPathname("%s",path) , error, sizeof(error));
	}

	if ( !g_rank.begin() )
	{
		g_rank.loadRank( MF_BuildPathname("%s",
		get_localinfo("dodstats","addons/amxmodx/data/dodstats.dat") ) );
	}

	g_map.Init();
}

void OnAmxxDetach()
{
	// KTP: Cleanup extension mode hooks before detaching
	DODX_CleanupExtensionHooks();

	// KTP: Cleanup gamerules config files
	IGameConfigManager *ConfigManager = MF_GetConfigManager();
	if (ConfigManager)
	{
		if (g_pCommonConfig)
			ConfigManager->CloseGameConfigFile(g_pCommonConfig);
		if (g_pGamerulesConfig)
			ConfigManager->CloseGameConfigFile(g_pGamerulesConfig);
	}
	g_pCommonConfig = nullptr;
	g_pGamerulesConfig = nullptr;
	g_pGameRulesAddress = nullptr;

	g_rank.clear();
	g_grenades.clear();
	g_rank.unloadCalc();
}

void OnPluginsLoaded()
{
	iFDeath = MF_RegisterForward("client_death",ET_IGNORE,FP_CELL,FP_CELL,FP_CELL,FP_CELL,FP_CELL,FP_DONE);
	iFDamage = MF_RegisterForward("client_damage",ET_IGNORE,FP_CELL,FP_CELL,FP_CELL,FP_CELL,FP_CELL,FP_CELL,FP_DONE);
	iFScore = MF_RegisterForward("client_score",ET_IGNORE,FP_CELL,FP_CELL,FP_CELL,FP_DONE);
	iFTeamForward = MF_RegisterForward("dod_client_changeteam",ET_IGNORE,FP_CELL/*id*/,FP_CELL/*team*/,FP_CELL/*oldteam*/,FP_DONE);
	iFSpawnForward = MF_RegisterForward("dod_client_spawn",ET_IGNORE,FP_CELL/*id*/,FP_DONE);
	iFClassForward = MF_RegisterForward("dod_client_changeclass",ET_IGNORE,FP_CELL/*id*/,FP_CELL/*class*/,FP_CELL/*oldclass*/,FP_DONE);
	iFScopeForward = MF_RegisterForward("dod_client_scope",ET_IGNORE,FP_CELL/*id*/,FP_CELL/*value*/,FP_DONE);
	iFWpnPickupForward = MF_RegisterForward("dod_client_weaponpickup",ET_IGNORE,FP_CELL/*id*/,FP_CELL/*weapon*/,FP_CELL/*value*/,FP_DONE);
	iFProneForward = MF_RegisterForward("dod_client_prone",ET_IGNORE,FP_CELL/*id*/,FP_CELL/*value*/,FP_DONE);
	iFCurWpnForward = MF_RegisterForward("dod_client_weaponswitch",ET_IGNORE,FP_CELL/*id*/,FP_CELL/*wpnew*/,FP_CELL/*wpnold*/,FP_DONE);
	iFWeaponFire = MF_RegisterForward("dod_client_weapon_fire",ET_IGNORE,FP_CELL/*id*/,FP_CELL/*weapon*/,FP_CELL/*gametime*/,FP_DONE);
	iFGrenadeExplode = MF_RegisterForward("dod_grenade_explosion",ET_IGNORE,FP_CELL/*id*/,FP_ARRAY/*pos[3]*/,FP_CELL/*wpnid*/,FP_DONE);
	iFRocketExplode = MF_RegisterForward("dod_rocket_explosion",ET_IGNORE,FP_CELL/*id*/,FP_ARRAY/*pos[3]*/,FP_CELL/*wpnid*/,FP_DONE);
	iFObjectTouched = MF_RegisterForward("dod_client_objectpickup",ET_IGNORE,FP_CELL/*id*/,FP_CELL/*object*/,FP_ARRAY/*pos[3]*/,FP_CELL/*value*/,FP_DONE);
	iFStaminaForward = MF_RegisterForward("dod_client_stamina",ET_IGNORE,FP_CELL/*id*/,FP_CELL/*stamina*/,FP_DONE);

	// KTP: HLStatsX integration forward - fired by dodx_flush_all_stats() native
	// stats_logging.sma should register for this to log weaponstats
	iFFlushStats = MF_RegisterForward("dod_stats_flush",ET_IGNORE,FP_CELL/*id*/,FP_DONE);

	// KTP: Pre-damage forward for damage modification (grenade reduction, etc.)
	// Fires before client_damage with ET_CONTINUE - return value is the modified damage
	// Return original damage to keep unchanged, return lower value to reduce, return 0 to block
	// Params: attacker, victim, damage, weapon, hitgroup, team_attack
	iFDamagePre = MF_RegisterForward("dod_damage_pre",ET_CONTINUE,FP_CELL,FP_CELL,FP_CELL,FP_CELL,FP_CELL,FP_CELL,FP_DONE);

	// KTP: Control point tracking forwards
	iFInitCP = MF_RegisterForward("controlpoints_init", ET_IGNORE, FP_DONE);
	iFCPCaptured = MF_RegisterForward("dod_control_point_captured", ET_IGNORE,
		FP_CELL/*cp_index*/, FP_CELL/*new_owner*/, FP_CELL/*old_owner*/, FP_DONE);
	iFScoreEvent = MF_RegisterForward("dod_score_event", ET_IGNORE,
		FP_CELL/*id*/, FP_CELL/*score_delta*/, FP_CELL/*total_score*/, FP_CELL/*cp_index*/, FP_DONE);

	// KTP: Initialize gamerules access for scoreboard score modification
	// This allows dodx_set_team_score/dodx_get_team_score natives to work
	IGameConfigManager *ConfigManager = MF_GetConfigManager();
	if (ConfigManager)
	{
		char error[256] = "";

		// Load common.games for g_pGameRules address signature
		if (ConfigManager->LoadGameConfigFile("common.games", &g_pCommonConfig, error, sizeof(error)))
		{
			// Try to get g_pGameRules address
			void *address = nullptr;
			if (g_pCommonConfig->GetAddress("g_pGameRules", &address) && address)
			{
				// Windows: address points to a pointer to g_pGameRules
				// Linux: address is g_pGameRules directly
#if defined(KE_WINDOWS)
				g_pGameRulesAddress = *reinterpret_cast<void***>(address);
#else
				g_pGameRulesAddress = reinterpret_cast<void**>(address);
#endif
			}
			else
			{
				MF_Log("[DODX] Warning: Could not find g_pGameRules address - scoreboard score natives disabled");
			}
		}
		else if (error[0])
		{
			MF_Log("[DODX] Warning: Could not load common.games: %s", error);
		}

		// Load gamerules.games for m_iTeamScores offset
		*error = '\0';
		if (ConfigManager->LoadGameConfigFile("common.games/gamerules.games", &g_pGamerulesConfig, error, sizeof(error)))
		{
			TypeDescription data;
			if (g_pGamerulesConfig->GetOffsetByClass("CDoDTeamPlay", "m_iTeamScores", &data))
			{
				g_iTeamScoreOffset = data.fieldOffset;
			}
		}
		else if (error[0])
		{
			MF_Log("[DODX] Warning: Could not load gamerules.games: %s", error);
		}

		// KTP: Resolve DoD round-timer offsets (dodx_get_round_time + the
		// dodx_test_dump_round_timers diagnostic). The gamedata already ships
		// all of these (offsets-cdodteamplay.txt, offsets-cdodroundtimer.txt,
		// offsets-cdodpararoundtimer.txt) — this only consumes what's there.
		// Gamerules-level fields resolve from either config (gamerules first,
		// common as fallback); the timer entity classes live in common.games.
		// Every lookup is optional: a miss leaves the -1 sentinel and the
		// dependent read is skipped.
		{
			TypeDescription data;
			if (g_pGamerulesConfig && g_pGamerulesConfig->GetOffsetByClass("CDoDTeamPlay", "m_flRoundTime", &data))
				g_iGrRoundTimeOffset = data.fieldOffset;
			else if (g_pCommonConfig && g_pCommonConfig->GetOffsetByClass("CDoDTeamPlay", "m_flRoundTime", &data))
				g_iGrRoundTimeOffset = data.fieldOffset;

			if (g_pGamerulesConfig && g_pGamerulesConfig->GetOffsetByClass("CDoDTeamPlay", "m_pParaTimer", &data))
				g_iParaTimerPtrOffset = data.fieldOffset;
			else if (g_pCommonConfig && g_pCommonConfig->GetOffsetByClass("CDoDTeamPlay", "m_pParaTimer", &data))
				g_iParaTimerPtrOffset = data.fieldOffset;

			if (g_pCommonConfig)
			{
				if (g_pCommonConfig->GetOffsetByClass("CDodParaRoundTimer", "m_fRoundTimer", &data))
					g_iParaRoundTimerOffset = data.fieldOffset;
				if (g_pCommonConfig->GetOffsetByClass("CDodParaRoundTimer", "m_bTimer", &data))
					g_iParaBTimerOffset = data.fieldOffset;
				if (g_pCommonConfig->GetOffsetByClass("CDodRoundTimer", "m_fRoundTime", &data))
					g_iRTimerRoundTimeOffset = data.fieldOffset;
				if (g_pCommonConfig->GetOffsetByClass("CDodRoundTimer", "m_fTimerLength", &data))
					g_iRTimerLengthOffset = data.fieldOffset;
				if (g_pCommonConfig->GetOffsetByClass("CDodRoundTimer", "m_bTimer", &data))
					g_iRTimerBTimerOffset = data.fieldOffset;
			}

			MF_Log("[DODX] round-timer offsets: gr.flRT=%d gr.pParaTimer=%d para.fRT=%d para.bT=%d rt.fRT=%d rt.fLen=%d rt.bT=%d",
				g_iGrRoundTimeOffset, g_iParaTimerPtrOffset, g_iParaRoundTimerOffset,
				g_iParaBTimerOffset, g_iRTimerRoundTimeOffset, g_iRTimerLengthOffset, g_iRTimerBTimerOffset);

			// Half-clock members for dodx_get_round_time (see globals block)
			IGameConfig *cfgs[2] = { g_pGamerulesConfig, g_pCommonConfig };
			for (int c = 0; c < 2; c++)
			{
				if (!cfgs[c]) continue;
				if (g_iDoDMapTimeOffset < 0 && cfgs[c]->GetOffsetByClass("CDoDTeamPlay", "m_flDoDMapTime", &data))
					g_iDoDMapTimeOffset = data.fieldOffset;
				if (g_iRestartRoundTimeOffset < 0 && cfgs[c]->GetOffsetByClass("CDoDTeamPlay", "m_flRestartRoundTime", &data))
					g_iRestartRoundTimeOffset = data.fieldOffset;
				if (g_iRoundRestartingOffset < 0 && cfgs[c]->GetOffsetByClass("CDoDTeamPlay", "m_bRoundRestarting", &data))
					g_iRoundRestartingOffset = data.fieldOffset;
			}
			MF_Log("[DODX] half-clock offsets: flDoDMapTime=%d flRestartRoundTime=%d bRoundRestarting=%d",
				g_iDoDMapTimeOffset, g_iRestartRoundTimeOffset, g_iRoundRestartingOffset);

			// Scoring-tick members for dodx_get_score_tick_time (see globals
			// block). Entity-class offsets, so common.games only — the
			// gamerules config carries no CControlPointMaster section.
			if (g_pCommonConfig)
			{
				if (g_pCommonConfig->GetOffsetByClass("CControlPointMaster", "m_fGivePointsTime", &data))
					g_iCPMGivePointsTimeOffset = data.fieldOffset;
				if (g_pCommonConfig->GetOffsetByClass("CControlPointMaster", "m_iGivePointsDelay", &data))
					g_iCPMGivePointsDelayOffset = data.fieldOffset;
				if (g_pCommonConfig->GetOffsetByClass("CControlPointMaster", "m_bActive", &data))
					g_iCPMActiveOffset = data.fieldOffset;
			}
			MF_Log("[DODX] scoring-tick offsets: fGivePointsTime=%d iGivePointsDelay=%d bActive=%d",
				g_iCPMGivePointsTimeOffset, g_iCPMGivePointsDelayOffset, g_iCPMActiveOffset);
		}
	}

	// KTP: cache mp_timelimit for dodx_get_round_time (engine is ready here in
	// both modes; CVAR_GET_POINTER once, then read ->value — never the string
	// lookup on a hot path)
	if (!g_pcvarMpTimelimit)
		g_pcvarMpTimelimit = CVAR_GET_POINTER("mp_timelimit");

	// KTP: In extension mode, do deferred engine-dependent initialization
	// Engine functions aren't ready during OnAmxxAttach in extension mode
	if (g_bExtensionMode)
	{
#if defined(__linux__) || defined(__APPLE__)
		// KTP: Load pdata offsets from addons/ktpamx/configs/dodx.ini if present.
		// Read in OnPluginsLoaded because MF_BuildPathnameR needs the engine ready.
		// The latch replaces the old g_bPdataOffsetForced guard, which also gated
		// the unrelated score_deaths_offset override.
		static bool s_offsetConfigRead = false;
		if (!s_offsetConfigRead)
		{
			s_offsetConfigRead = true;

			char configPath[256];
			MF_BuildPathnameR(configPath, sizeof(configPath), "addons/ktpamx/configs/dodx.ini");
			FILE* fp = fopen(configPath, "r");
			if (fp)
			{
				char line[128];
				while (fgets(line, sizeof(line), fp))
				{
					// Skip comments and empty lines
					if (line[0] == ';' || line[0] == '#' || line[0] == '\n' || line[0] == '\r')
						continue;

					int offset;
					if (sscanf(line, "pdata_offset = %d", &offset) == 1 ||
					    sscanf(line, "pdata_offset=%d", &offset) == 1)
					{
						if (offset == 4 || offset == 5)
						{
							g_iLinuxPdataOffsetAdjust = offset;
							MF_PrintSrvConsole("[DODX] Pdata offset forced to +%d via config file\n", offset);
						}
						else
						{
							MF_PrintSrvConsole("[DODX] Warning: Invalid pdata_offset %d in config (must be 4 or 5)\n", offset);
						}
						continue;
					}

					// KTP 2026-05-21: independent override for SCORE/DEATHS offset
					// (separate from grenade pdata_offset above).
					if (sscanf(line, "score_deaths_offset = %d", &offset) == 1 ||
					    sscanf(line, "score_deaths_offset=%d", &offset) == 1)
					{
						if (offset == 4 || offset == 5)
						{
							g_iScoreDeathsOffsetAdjust = offset;
							MF_PrintSrvConsole("[DODX] score/deaths offset forced to +%d via config file\n", offset);
						}
						else
						{
							MF_PrintSrvConsole("[DODX] Warning: Invalid score_deaths_offset %d (must be 4 or 5)\n", offset);
						}
						continue;
					}
				}
				fclose(fp);
			}

			MF_PrintSrvConsole("[DODX] Using m_rgAmmo pdata offset +%d (override via pdata_offset in dodx.ini)\n", g_iLinuxPdataOffsetAdjust);
			MF_PrintSrvConsole("[DODX] Using score/deaths offset +%d (override via score_deaths_offset in dodx.ini)\n", g_iScoreDeathsOffsetAdjust);
		}
#endif

		// KTP: Skip cvar registration in extension mode - CVAR_REGISTER crashes
		// because module SDK doesn't properly set up engine function pointers
		// for extension mode. The isModuleActive() function handles NULL pointers
		// gracefully (returns true = module always active).
		// Note: dodstats_pause, dodstats_rankbots etc. will remain NULL

		// KTP: Skip rank loading - not needed for HLStatsX logging
		g_map.Init();

		// KTP: Player init disabled - pfnPEntityOfEntIndex causes hang in OnPluginsLoaded
		// Players will be initialized on-demand when messages arrive.
#if defined(KTP_LANE_B_FAKECLIENTS)
		// Lane B's compile-only core already registers fake clients as AMXX
		// players. DODX must also retain their shot/hit counters or
		// get_user_wstats() has nothing to flush at match end. Extension mode
		// deliberately does not register dodstats_rankbots, so the normal NULL
		// fallback below is always false. This define is supplied only by the
		// isolated Lane B build; ordinary/preprod production artifacts do not
		// define it and preserve the existing bot exclusion.
		rankBots = true;
		MF_PrintSrvConsole("[DODX] Lane B test build: bot weapon counters enabled\n");
#else
		rankBots = dodstats_rankbots ? ((int)dodstats_rankbots->value ? true : false) : false;
#endif

		// KTP: Look up message IDs using MF_GetUserMsgId (provided by KTPAMXX)
		if (MF_GetUserMsgId)
		{
			for (int i = 0; g_user_msg[i].name; ++i)
			{
				int id = MF_GetUserMsgId(g_user_msg[i].name);
				if (id > 0)
				{
					*g_user_msg[i].id = id;
					if (g_user_msg[i].endmsg)
						modMsgsEnd[id] = g_user_msg[i].func;
					else
						modMsgs[id] = g_user_msg[i].func;
				}
				// MsgID logging removed — only useful during initial development
			}
		}

		// KTP: Register IMessageManager hooks for message interception
		DODX_RegisterMessageHooks();

		// KTP: Entity scan for CP data is deferred to SV_ActivateServer hook.
		// At OnPluginsLoaded time, entities haven't been spawned yet.
	}
}

// ============================================================================
// KTP: ReHLDS Extension Mode Hook Implementations
// ============================================================================

// KTP: slot whose usercmd most recently began (set in the PreThink hook body).
// The window it opens is never explicitly closed -- nothing hookable runs between
// the last cmd of a frame and the send phase -- so a shooter-owned trace fired
// after his cmd still passes while he remains the most recent cmd player. That
// residual is documented at the capture site rather than papered over.
static int g_ktpCmdOwner = 0;

// KTP: per-shot aim geometry capture (tier-2 sensor -- read once by
// dodx_get_shot_geom, guard rationale in KTPShotGeom.h). Reached only for
// player-hitting traces whose ignore-entity is a player, so the per-trace cost
// on every other trace is the branch that got us here; the trig runs at hit
// rate. Everything reported is a measurement -- no threshold, no conclusion.
//
// What can write this stash besides the bullet trace, and what stops each:
//   - other players' traces: excluded structurally (per-shooter stash, and the
//     trace's ignore-entity must be this shooter).
//   - traces outside the shooter's cmd window (e.g. HLSDK sends nothing here,
//     but a game-DLL trace in the send phase would qualify): excluded by the
//     cmd-owner gate, EXCEPT when this shooter was the frame's last cmd player,
//     because the window has no closing edge.
//   - a later same-cmd trace after the bullet's: excluded by first-wins.
//   - a shooter-owned player-hitting trace BETWEEN the PreThink hook body and
//     PostThink (player Think, or a touch handler under SV_Impact -- both run
//     in that gap in SV_RunCmd): NOT excluded, and the worst class here, because
//     first-wins lets it DISPLACE the bullet's own capture. No hookable edge
//     exists between those phases to fence it; an SV_PlayerRunPostThink
//     hookchain in KTP-ReHLDS would be the real fix.
//   - a shooter-owned player-hitting trace in his own cmd window when his bullet
//     hit nobody: NOT excluded. dod.so is closed source, so neither this class
//     nor the one above can be enumerated; vanilla HLSDK has none on these
//     paths. If one exists, the geometry is still a ray the shooter's own think
//     code cast this cmd, with his in-hand weapon id -- but it is not the
//     bullet, and the consumer should know this class exists.
static void KTPCaptureShotGeom(CPlayer *pPlayer, const float *v1, const float *v2, TraceResult *ptr)
{
	KTPShotGeom &sg = pPlayer->ktpShot;

	if (pPlayer->index != g_ktpCmdOwner || sg.cmdSeq == 0)
		return;

	// First capture wins the cmd: nothing after the bullet's PostThink trace can
	// replace it. The cost is the pre-PostThink displacement class enumerated
	// above; last-wins would only swap which unenumerable class is exposed.
	if (sg.geomSeq == sg.cmdSeq)
		return;

	// Target centre is the entity origin: GoldSrc keeps a player's origin at hull
	// centre, and lag compensation moves origin and relinks, so this is the centre
	// the shooter's trace actually ran against.
	float aim[3] = { v2[0] - v1[0], v2[1] - v1[1], v2[2] - v1[2] };
	float to[3]  = { ptr->pHit->v.origin[0] - v1[0],
	                 ptr->pHit->v.origin[1] - v1[1],
	                 ptr->pHit->v.origin[2] - v1[2] };

	float aimN[3], toN[3];
	float range = ktpshot::normalize3(to, toN);
	if (range <= 0.0f || ktpshot::normalize3(aim, aimN) <= 0.0f)
		return;

	// Bearing rate to THIS target since its previous captured sighting, averaged
	// over the gap between them; the gap ships alongside so the consumer judges
	// validity instead of this layer picking a horizon. -1 is "no usable prior"
	// -- a consumer must never read it as "stationary". The now > prevTime guard
	// matters: svtimebase is re-anchored per packet, so the delta can step
	// backward, and a negative dt would fabricate a rate.
	int tgtAngVelMdps = -1;
	int sightGapMs = -1;
	const int tgtIdx = ENTINDEX_SAFE(ptr->pHit);
	const double now = (double)gpGlobals->time;
	if (tgtIdx == sg.prevTarget && sg.prevTime > 0.0 && now > sg.prevTime)
	{
		const double dt = now - sg.prevTime;
		double mdps = ((double)ktpshot::angleUdeg(sg.prevDir, toN) / 1000.0) / dt;
		double gap  = dt * 1000.0 + 0.5;
		if (mdps > 2147483000.0) mdps = 2147483000.0;  // int32 headroom, not a threshold
		if (gap  > 2147483000.0) gap  = 2147483000.0;
		tgtAngVelMdps = (int)mdps;
		sightGapMs = (int)gap;
	}
	sg.prevTarget = tgtIdx;
	sg.prevTime = now;
	sg.prevDir[0] = toN[0]; sg.prevDir[1] = toN[1]; sg.prevDir[2] = toN[2];

	// How far the ray's start sits from the shooter's view origin. A penetration
	// continuation trace starts at the wall exit point, not the eye, which shrinks
	// range and inflates the angle for the same aim -- shipping the offset lets
	// the consumer separate those samples instead of this layer guessing.
	const edict_t *pe = pPlayer->pEdict;
	float off[3] = { pe->v.origin[0] + pe->v.view_ofs[0] - v1[0],
	                 pe->v.origin[1] + pe->v.view_ofs[1] - v1[1],
	                 pe->v.origin[2] + pe->v.view_ofs[2] - v1[2] };

	sg.geomSeq = sg.cmdSeq;
	sg.geomWeapon = pPlayer->current;
	sg.errUdeg = ktpshot::angleUdeg(aimN, toN);
	sg.rangeUnits = (int)(range + 0.5f);
	sg.tgtAngVelMdps = tgtAngVelMdps;
	sg.sightGapMs = sightGapMs;
	sg.hitgroup = ptr->iHitgroup;
	sg.startOffUnits = (int)(sqrtf(ktpshot::dot3(off, off)) + 0.5f);
}

// KTP: pack recorder for the tier-2.7 aim-vs-transmission sensor (KTPPackVis.h).
// SV_CreatePacketEntities hands over, per client per sent frame, the exact
// entity pack the engine is about to serialize -- one step downstream of the
// game DLL's AddToFullPack verdicts, so "recorded here" and "transmitted to
// that client" are the same statement. Player entities only: the sampler only
// ever asks about players, and a last-packed-time table makes the per-usercmd
// query O(1) -- no per-call scan of the frame ring, and no ring-length bound
// turning high ping into a stricter check.
//
// Why not read client_t::frames directly (the rh_is_entity_fullpacked shape):
// dodx has no ABI-safe path to client_t. engine_strucs.h pins NET_MAX_PAYLOAD
// at 3990 while this fork's engine builds netchan_t with 65536, so every
// client_t field after netchan -- frames included -- sits at the wrong offset
// through that mirror. The hook hands us engine-built pointers instead, which
// cannot go stale that way.
static double g_ktpPackLastSeen[33][33]; // [host][target] = sv time of the last pack containing target
static double g_ktpPackSince[33];        // when this slot's recording/occupancy continuity began
bool g_ktpPackRecorderLive = false;      // read by dodx_get_aim_vis_stats (NBase.cpp)

// Clock note, load-bearing for the sampler's compare: recording stamps
// gpGlobals->time in the send phase (sv.time), while the sampler reads it
// inside a usercmd (the shooter's svtimebase). They differ by at most about one
// client packet interval -- an order below the window -- and both freeze under
// a server pause together, which a wall clock would not.

static void KTPPackVisClearSlot(int slot, double now)
{
	for (int i = 0; i < 33; ++i)
	{
		g_ktpPackLastSeen[slot][i] = 0.0;
		g_ktpPackLastSeen[i][slot] = 0.0;
	}
	g_ktpPackSince[slot] = now;
}

static void KTPPackVisClearAll(double now)
{
	memset(g_ktpPackLastSeen, 0, sizeof(g_ktpPackLastSeen));
	for (int i = 0; i < 33; ++i)
		g_ktpPackSince[i] = now;
}

static int DODX_OnSVCreatePacketEntities(IRehldsHook_SV_CreatePacketEntities *chain,
	sv_delta_t type, IGameClient *cl, packet_entities_t *to, sizebuf_t *msg)
{
	// The pack is fully built before this call; the chain only serializes it.
	int ret = chain->callNext(type, cl, to, msg);

	if (g_bServerActive && gpGlobals && cl && to)
	{
		edict_t *pHostEdict = cl->GetEdict();
		const int host = pHostEdict ? ENTINDEX_SAFE(pHostEdict) : 0;
		if (host >= 1 && host <= gpGlobals->maxClients && host <= 32)
		{
			const double now = (double)gpGlobals->time;
			int maxc = gpGlobals->maxClients;
			if (maxc > 32)
				maxc = 32;
			// The engine caps a pack at 256 entries; clamping the walk to that
			// costs nothing and bounds this loop even against a corrupt count.
			int count = to->num_entities;
			if (count > 256)
				count = 256;
			for (int i = 0; i < count; ++i)
			{
				const int n = to->entities[i].number;
				if (n >= 1 && n <= maxc)
					g_ktpPackLastSeen[host][n] = now;
			}
		}
	}

	return ret;
}

// KTP: interpolation half of the sampling window, milliseconds. The GoldSrc
// client renders ex_interp (default 0.1 s) behind its newest update; this is
// that engine default. It is a forgiveness bound, not a judgement -- widening
// it only moves samples toward "packed", never toward suspicion.
#define KTP_VIS_INTERP_MS 100

// KTP: one aim-vs-transmission sample (tier 2.7). Reached from the same
// player-hitting-trace site as the shot capture, so the aim ray is one the game
// itself cast -- this layer casts none and defines no aim cone of its own.
// Everything recorded is a count; the meaning of the counts is the private
// consumer's problem.
static void KTPSampleAimVis(CPlayer *pPlayer, TraceResult *ptr)
{
	KTPPackVis &v = pPlayer->ktpVis;
	KTPShotGeom &sg = pPlayer->ktpShot;

	// Same cmd-window attribution gate as the shot capture, then one sample per
	// cmd: the first QUALIFYING trace speaks for it (the latch is set after the
	// team/alive filters, so a teammate-hitting trace does not spend the cmd).
	if (pPlayer->index != g_ktpCmdOwner || sg.cmdSeq == 0)
		return;
	if (v.lastSampleSeq == sg.cmdSeq)
		return;

	// Bots receive no entity packets, so every bot-shooter sample would read
	// "never packed" -- fabricated by construction. Bot TARGETS are packed to
	// real clients like any entity and stay sampled.
	if (pPlayer->bot)
		return;

	edict_t *pShooter = pPlayer->pEdict;
	edict_t *pTarget = ptr->pHit;
	if (!pShooter || pShooter->free || !pTarget || pTarget->free)
		return;

	// Live opponents only. Teammates and spectators are packed under the game's
	// own rules regardless of visibility and would only dilute the counters.
	const int shooterTeam = (int)pShooter->v.team;
	const int targetTeam = (int)pTarget->v.team;
	if (shooterTeam < 1 || shooterTeam > 2 || targetTeam < 1 || targetTeam > 2)
		return;
	if (shooterTeam == targetTeam)
		return;
	if (pTarget->v.deadflag != DEAD_NO || pTarget->v.health <= 0)
		return;

	const int host = pPlayer->index;
	const int tgt = ENTINDEX_SAFE(pTarget);
	if (host < 1 || host > 32 || tgt < 1 || tgt > gpGlobals->maxClients || tgt > 32)
		return;

	v.lastSampleSeq = sg.cmdSeq;

	// A dead recorder means every answer would be fabricated. That is the
	// unknown state, never a zero.
	if (!g_ktpPackRecorderLive)
	{
		v.samplesUnknown++;
		return;
	}

	int ping = 0, loss = 0;
	if (g_engfuncs.pfnGetPlayerStats)
		(*g_engfuncs.pfnGetPlayerStats)(pShooter, &ping, &loss);
	if (ping < 0)
		ping = 0;
	const int winMs = KTP_VIS_INTERP_MS + ping;

	const double now = (double)gpGlobals->time;
	const double last = g_ktpPackLastSeen[host][tgt];

	// The recorder stamps sv.time at send; this read runs on the shooter's
	// svtimebase, which dropped commands pull BEHIND the newest send stamp — so
	// a stamp slightly ahead of this clock is routine packet-loss skew, and it
	// concentrates on lossy clients. The window is a forgiveness bound, so
	// ahead-within-window reads as packed; only ahead-beyond-window — a real
	// map/clock boundary — is unanswerable.
	if (last > 0.0 && (last - now) * 1000.0 > (double)winMs)
	{
		v.samplesUnknown++;
		return;
	}

	if (last > 0.0 && (now - last) * 1000.0 <= (double)winMs)
	{
		ktppackvis::satAdd(v.samplesKnown, 1);
		ktppackvis::satAdd(v.windowMsSum, winMs);
		if (winMs > v.windowMsMax)
			v.windowMsMax = winMs;
		return;
	}

	// Not seen within the window. That only means something once recording has
	// covered a full window for BOTH slots; before that, absence of a stamp is
	// absence of coverage. since==0 is the never-stamped state and never counts
	// as covered — the storage default must not be the fabricating direction.
	const double since = (g_ktpPackSince[host] > g_ktpPackSince[tgt])
		? g_ktpPackSince[host] : g_ktpPackSince[tgt];
	if (since <= 0.0 || now < since || (now - since) * 1000.0 < (double)winMs)
	{
		v.samplesUnknown++;
		return;
	}

	ktppackvis::satAdd(v.samplesKnown, 1);
	ktppackvis::satAdd(v.samplesUnpacked, 1);
	ktppackvis::satAdd(v.windowMsSum, winMs);
	if (winMs > v.windowMsMax)
		v.windowMsMax = winMs;
}

// KTP: TraceLine hook handler - replaces FN_TraceLine_Post
static void DODX_OnTraceLine(IVoidHookChain<const float *, const float *, int, edict_t *, TraceResult *> *chain,
                              const float *v1, const float *v2, int fNoMonsters, edict_t *e, TraceResult *ptr)
{
	// Call the original first - this is a POST hook, we read results only
	chain->callNext(v1, v2, fNoMonsters, e, ptr);

	// KTP: Skip processing if server is not active (during map change)
	if (!g_bServerActive || !g_pFirstEdict || !gpGlobals)
		return;

	// KTP: Validate ptr before accessing
	if (!ptr)
		return;

	// Player aiming detection: when player traces and hits another player
	// Records iHitgroup for headshot tracking
	if (ptr->pHit && !ptr->pHit->free && (ptr->pHit->v.flags & (FL_CLIENT | FL_FAKECLIENT)) &&
	    e && !e->free && (e->v.flags & (FL_CLIENT | FL_FAKECLIENT)))
	{
		int idx = ENTINDEX_SAFE(e);
		if (idx >= 1 && idx <= gpGlobals->maxClients)
		{
			CPlayer* pPlayer = GET_PLAYER_POINTER_I(idx);
			if (pPlayer->ingame)
			{
				pPlayer->aiming = ptr->iHitgroup;
				KTPCaptureShotGeom(pPlayer, v1, v2, ptr);
				KTPSampleAimVis(pPlayer, ptr);
			}
		}
		return;
	}

	// Grenade/rocket tracking: when a projectile owned by a player traces
	if (e && !e->free && e->v.owner && !e->v.owner->free && (e->v.owner->v.flags & (FL_CLIENT | FL_FAKECLIENT)))
	{
		int ownerIdx = ENTINDEX_SAFE(e->v.owner);
		if (ownerIdx < 1 || ownerIdx > gpGlobals->maxClients)
			return;

		CPlayer *pPlayer = GET_PLAYER_POINTER_I(ownerIdx);
		if (!pPlayer->ingame)
			return;

		for (int i = 0; i < MAX_TRACE; i++)
		{
			// strcmp is deliberate — see the matching note on the Metamod TraceLine path.
			if (strcmp(traceData[i].szName, STRING(e->v.classname)) == 0)
			{
				int grenId = (traceData[i].iId == 13 && g_map.detect_allies_country) ? 36 : traceData[i].iId;
				int rocketId = traceData[i].iId;

				if (traceData[i].iAction & ACT_NADE_SHOT)
				{
					if (traceData[i].iId == 13 && g_map.detect_allies_country)
						pPlayer->saveShot(grenId);
					else
						pPlayer->saveShot(traceData[i].iId);
				}
				else if (traceData[i].iAction & ACT_ROCKET_SHOT)
					pPlayer->saveShot(traceData[i].iId);

				cell position[3];
				position[0] = amx_ftoc(v2[0]);
				position[1] = amx_ftoc(v2[1]);
				position[2] = amx_ftoc(v2[2]);
				cell pos = MF_PrepareCellArray(position, 3);

				if (traceData[i].iAction & ACT_NADE_PUT)
				{
					g_grenades.put(e, traceData[i].fDel, grenId, pPlayer);
					MF_ExecuteForward(iFGrenadeExplode, pPlayer->index, pos, grenId);
				}

				if (traceData[i].iAction & ACT_ROCKET_PUT)
					MF_ExecuteForward(iFRocketExplode, pPlayer->index, pos, rocketId);

				break;
			}
		}
	}
}

// KTP: SetClientKeyValue hook handler - replaces FN_SetClientKeyValue
static void DODX_OnSetClientKeyValue(IVoidHookChain<int, char *, const char *, const char *> *chain,
                                      int clientIndex, char *infobuffer, const char *key, const char *value)
{
	// Check to see if its a player and we are setting a model
	if(strcmp(key, "model") == 0 &&
		(strcmp(value, "axis-inf") == 0 ||
		 strcmp(value, "axis-para") == 0 ||
		 strcmp(value, "us-inf") == 0 ||
		 strcmp(value, "us-para") == 0 ||
		 strcmp(value, "brit-inf") == 0))
	{
		CPlayer *pPlayer = GET_PLAYER_POINTER_I(clientIndex);
		if(pPlayer->ingame)
		{
			if(pPlayer->setModel())
			{
				// Supercede - don't call original
				return;
			}
		}
	}

	// Call original
	chain->callNext(clientIndex, infobuffer, key, value);
}

// KTP: one usercmd's worth of aim + movement, folded into the player's running
// accumulators. Everything here is O(1) and allocation-free by construction; see
// KTPAimAccum.h for why a streaming fit rather than the buffered reference port.
static void KTPSampleAim(CPlayer *pPlayer, edict_t *pEntity)
{
	KTPAimStats &st = pPlayer->ktpAim;

	// Only a live player on a playing team. Dead players and spectators keep sending
	// usercmds, and in DoD +attack is the spectator "next player" bind -- so a spectator
	// panning smoothly produces the straightest aim trace on the server, from someone not
	// shooting at anything, and retention by duration does not help because a pan is LONG.
	// The team check is not redundant with IsAlive(): whether dod.so leaves deadflag and
	// health in a "dead" state for a spectator is a property of a closed-source DLL, so
	// relying on it alone would rest the gate on an assumption we cannot verify here.
	// Teams 1 and 2 play; 3 is spectator.
	const int team = pEntity->v.team;
	if (!pPlayer->IsAlive() || (team != 1 && team != 2))
	{
		// DISCARD rather than close. A window truncated by death is not a burst that
		// ended, and scoring it inflates windowsScored -- the denominator a consumer
		// needs -- by a quantity correlated with deaths rather than with aim.
		st.cur.Reset();
		st.ForgetGroundState();
		return;
	}

	const double t = gpGlobals->time;

	// v_angle is the resolved view angle for THIS usercmd, which is what the reference
	// detector scored. GoldSrc pitch arrives in (-180,180] here, so the fold is a guard
	// against an out-of-range value rather than an expected wrap.
	double pitch = pEntity->v.v_angle.x;
	if (pitch > 180.0) pitch -= 360.0;

	const bool attacking = (pEntity->v.button & IN_ATTACK) != 0;

	// Continuity is decided HERE, on both paths, because a burst can be broken by
	// samples that never arrive as easily as by ones that arrive without +attack.
	// A delivery gap is not trigger discipline: a .tech pause stops SV_RunCmd
	// outright (up to 300s of budget per team), and packet loss or a server-side
	// usercmd drop does the same for shorter spans. Bridging across one would emit a
	// single window whose duration is the outage -- and since retention keeps the
	// LONGEST windows, those would crowd out every genuine burst the player produced.
	//
	// The lower bound matters as much as the upper: svtimebase is re-anchored per
	// packet, so the delta can step backward, and an unguarded `delta <= BRIDGE_MS`
	// is satisfied by every negative value however large.
	// Sampling continuity, measured against the last ATTACKING sample. Deliberately a
	// different bound from the trigger release below: this one asks whether we were
	// still watching, and the answer must not depend on the client's frame rate.
	const double sinceLast = st.cur.open ? (t - st.cur.tLast) : 0.0;
	const bool   continuous = st.cur.open
	                          && sinceLast >= 0.0
	                          && sinceLast * 1000.0 <= KTPAim::CONTINUITY_MS;

	if (st.cur.open && (!continuous || st.cur.Overlong(t)))
		st.CloseWindow();      // scores what was genuinely observed, drops the pendings

	if (attacking)
	{
		// A bridged sample only belongs to the window once an attacking sample follows
		// it, so the held ones are folded in here rather than when they arrived.
		for (int i = 0; i < st.cur.pendCount; i++)
			st.cur.Add(st.cur.pendT[i], st.cur.pendP[i]);
		st.cur.pendCount = 0;

		st.cur.open = true;
		st.cur.Add(t, pitch);
		st.cur.tLast = t;
	}
	else if (st.cur.open)
	{
		// Hold the sample: only a later attacking sample proves it was mid-burst
		// rather than trailing the end of one. The trigger-release bound is applied
		// here, on the gap since firing stopped; the slot count is the backstop for a
		// pause, where the clock does not advance and no time bound can trip.
		if ((t - st.cur.tLast) * 1000.0 <= KTPAim::RELEASE_MS
		    && st.cur.pendCount < KTPAim::GAP_SLOTS)
		{
			st.cur.pendT[st.cur.pendCount] = t;
			st.cur.pendP[st.cur.pendCount] = pitch;
			st.cur.pendCount++;
		}
		else
		{
			st.CloseWindow();   // discards the pending bridge samples, as intended
		}
	}

	// Ground contact in TIME, not usercmd counts -- a count is a function of the
	// client's own cl_cmdrate, so a legal cvar change would move the signal.
	const bool onGround = (pEntity->v.flags & FL_ONGROUND) != 0;
	if (!st.groundKnown)
	{
		// First look since a reset or a respawn. Adopt the current state WITHOUT
		// counting a landing and WITHOUT timing this contact: we did not see it start,
		// so its duration is unknown and reporting one would be inventing it.
		st.onGroundPrev = onGround;
		st.groundKnown  = true;
		st.contactTimed = false;
	}
	else if (onGround && !st.onGroundPrev)
	{
		st.groundEnterTime = t;
		st.contactTimed = true;
	}
	else if (!onGround && st.onGroundPrev)
	{
		// Counted at contact END, not start, so the touch and its duration always land
		// in the SAME flush interval. Counting at start put them in different ones
		// whenever a contact straddled a flush, and any per-touch figure a consumer
		// derived was then quietly mixing two populations.
		if (st.contactTimed)
		{
			int ms = (int)((t - st.groundEnterTime) * 1000.0 + 0.5);
			if (ms < 0) ms = 0;
			st.groundTouches++;
			if (st.shortestGroundMs < 0 || ms < st.shortestGroundMs)
				st.shortestGroundMs = ms;
		}
		st.contactTimed = false;
	}
	st.onGroundPrev = onGround;
}

// KTP: PlayerPreThink hook handler - replaces FN_PlayerPreThink_Post
static void DODX_OnPlayerPreThink(IVoidHookChain<edict_t *, float> *chain, edict_t *pEntity, float time)
{
	// Call original first
	chain->callNext(pEntity, time);

	// Post-hook logic — basic safety checks that must pass regardless of stats state
	if (!pEntity || pEntity->free)
		return;

	if (!(pEntity->v.flags & FL_CLIENT))
		return;

	if (!gpGlobals)
		return;

	// KTP: Last-resort recovery. If DODX_OnSV_ActivateServer was missed for any
	// reason (hook not fired, INDEXENT(0) returned NULL, etc.), reconstruct
	// g_pFirstEdict from this player's edict so forwards can resume dispatching.
	// Before 2.7.4 this path existed unguarded; 2.7.4 replaced it with a hard
	// return, which turned any single missed re-init into a permanent silent
	// state on production (Denver 5, ATL1, NY1) — only fixable by plugin
	// re-attach. MF_Log so the underlying hook miss is visible in logs.
	if (g_bExtensionMode && !g_pFirstEdict)
	{
		int tmpIndex = ENTINDEX(pEntity);
		if (tmpIndex >= 1 && tmpIndex <= gpGlobals->maxClients)
		{
			g_pFirstEdict = pEntity - tmpIndex;
			g_bServerActive = true;
			// The per-map reset this path stands in for. A carried-over ammo slot
			// is worse than none: it reads as resolved, so the natives write
			// another ammo type's counter and report success.
			DODX_ClearAmmoRegistry();
			for (int i = 1; i <= gpGlobals->maxClients; ++i)
			{
				GET_PLAYER_POINTER_I(i)->Init(i, g_pFirstEdict + i);
				GET_PLAYER_POINTER_I(i)->ktpShot.reset();
				GET_PLAYER_POINTER_I(i)->ktpVis.reset();
			}
			// Recovery implies the activate hook was missed, so the pack table may
			// still describe the previous map. Restart coverage from here.
			KTPPackVisClearAll((double)gpGlobals->time);
			MF_Log("dodx: PreThink recovered g_pFirstEdict after SV_ActivateServer hook miss (player idx=%d)", tmpIndex);
		}
		else
		{
			return;
		}
	}

	if (!g_bServerActive)
		return;

	if (!g_pFirstEdict)
		return;

	int index = ENTINDEX_SAFE(pEntity);
	if (index < 1 || index > gpGlobals->maxClients)
		return;

	CPlayer *pPlayer = GET_PLAYER_POINTER_I(index);

	// KTP: In extension mode, initialize player on first PreThink call.
	// This replaces ClientPutInServer_Post which doesn't fire in extension mode.
	// MUST happen BEFORE the isModuleActive() check — player tracking (ingame flag,
	// pEdict pointer) must work even when stats collection is paused. Otherwise
	// natives like dodx_set_user_noclip, dodx_give_grenade etc. fail because
	// CHECK_PLAYER sees ingame=false.
	if (!pPlayer->ingame && g_bExtensionMode)
	{
		if (!pPlayer->pEdict)
		{
			pPlayer->Init(index, pEntity);
		}

		pPlayer->bot = (pEntity->v.flags & FL_FAKECLIENT) ? true : false;
		pPlayer->PutInServer();

		// Extension-mode connect point (Init() is skipped for a slot that already
		// has a pEdict) -- same reason ktpAim resets in Disconnect(): a mid-map
		// substitute must not inherit the leaver's stash or sighting baseline.
		pPlayer->ktpShot.reset();
		// Nor the leaver's visibility counters or pack history -- and coverage
		// for this slot restarts now, so early samples read unknown, not clean.
		pPlayer->ktpVis.reset();
		KTPPackVisClearSlot(index, (double)gpGlobals->time);
	}
	else if (!pPlayer->ingame)
	{
		return;
	}

	// KTP: open this player's shot-geometry cmd window. Deliberately in the hook
	// BODY, which runs after chain->callNext: the game's PreThink up there is where
	// the CurWeapon-driven fire forward reads the stash, so bumping here keeps the
	// read's cmdSeq equal to what the fire's own PostThink trace will stamp.
	// KTPShotGeom.h walks the ordering.
	pPlayer->ktpShot.cmdSeq++;
	g_ktpCmdOwner = index;

	// KTP: sample aim/movement BEFORE the isModuleActive() gate. Those pauses are
	// round-freeze and dodstats_pause -- scoring concerns. A fire window that spans a
	// pause boundary would otherwise be silently truncated mid-burst and score as a
	// short clean one, which is the direction that loses a detection.
	KTPSampleAim(pPlayer, pEntity);

	// Stats tracking — skip if module is paused (round-freeze, dodstats_pause cvar)
	if (!isModuleActive())
		return;

	pPlayer->PreThink();

	if(pPlayer->clearStats && pPlayer->clearStats < gpGlobals->time)
	{
		if(!ignoreBots(pEntity))
		{
			pPlayer->clearStats = 0.0f;
			if (pPlayer->rank)  // KTP: rank may be NULL in extension mode
				pPlayer->rank->updatePosition(&pPlayer->life);
			pPlayer->restartStats(false);
		}
	}

	if(pPlayer->clearRound && pPlayer->clearRound < gpGlobals->time)
	{
		pPlayer->clearRound = 0.0f;
		memset(static_cast<void *>(&pPlayer->round), 0, sizeof(pPlayer->round));
		memset(&pPlayer->weaponsRnd, 0, sizeof(pPlayer->weaponsRnd));
	}

	if (pPlayer->sendScore && pPlayer->sendScore < gpGlobals->time)
	{
		pPlayer->sendScore = 0;

		// KTP: Resolve pending CP index. ObjScore fires BEFORE SetObj in DoD,
		// so lastScoreCP=-2 means "ObjScore was received but SetObj hasn't set
		// g_lastCapturedCP yet". By now (~0.2s later), SetObj has fired.
		if (pPlayer->lastScoreCP == -2)
		{
			// Negative delta = server time restarted (map change), not a fresh capture.
			float capDelta = gpGlobals->time - g_lastCapturedTime;
			if (capDelta >= 0.0f && capDelta < 2.0f)
				pPlayer->lastScoreCP = g_lastCapturedCP;
			else
				pPlayer->lastScoreCP = -1;
		}

		MF_ExecuteForward(iFScore, pPlayer->index, pPlayer->lastScore, pPlayer->savedScore);
		if (iFScoreEvent >= 0)
			MF_ExecuteForward(iFScoreEvent, pPlayer->index, pPlayer->lastScore, (int)pPlayer->savedScore, pPlayer->lastScoreCP);
		pPlayer->lastScoreCP = -1;
	}
}

// KTP: RegUserMsg hook handler - replaces FN_RegUserMsg_Post
static int DODX_OnRegUserMsg(IHookChain<int, const char *, int> *chain, const char *pszName, int iSize)
{
	// Call original first to get the message ID
	int id = chain->callNext(pszName, iSize);

	// Post-hook logic (same as RegUserMsg_Post)
	for (int i = 0; g_user_msg[i].name; ++i)
	{
		if(!*g_user_msg[i].id && strcmp(g_user_msg[i].name, pszName) == 0)
		{
			*g_user_msg[i].id = id;

			if(g_user_msg[i].endmsg)
				modMsgsEnd[id] = g_user_msg[i].func;
			else
				modMsgs[id] = g_user_msg[i].func;
			break;
		}
	}

	return id;
}

// KTP: ClientConnected hook handler - replaces FN_ClientConnect_Post
static void DODX_OnClientConnected(IVoidHookChain<IGameClient *> *chain, IGameClient *client)
{
	// Call original first
	chain->callNext(client);

	if (!client)
		return;

	// KTP: Safety check - gpGlobals must be valid
	if (!gpGlobals)
		return;

	// GetId() is 0-based, player index is 1-based
	int clientIndex = client->GetId() + 1;
	if (clientIndex < 1 || clientIndex > gpGlobals->maxClients)
		return;

	CPlayer* pPlayer = GET_PLAYER_POINTER_I(clientIndex);

	// Get edict from IGameClient and ensure player is initialized with edict pointer
	edict_t* pEdict = client->GetEdict();
	if (pEdict && !pEdict->free)
	{
		// KTP: Calculate g_pFirstEdict using pointer arithmetic
		// This is the key initialization for extension mode!
		// Player edicts are at g_pFirstEdict + index, so worldspawn = pEdict - clientIndex
		if (!g_pFirstEdict)
		{
			g_pFirstEdict = pEdict - clientIndex;
		}

		// Initialize player with edict if not already done
		if (!pPlayer->pEdict)
			pPlayer->Init(clientIndex, pEdict);
	}

	// Determine if bot - check if no net channel (bots don't have network connections)
	INetChan* netChan = client->GetNetChan();
	pPlayer->bot = (netChan == nullptr);

	// NOTE: We don't call Connect() here because:
	// 1. Core AMXX already handles player connection via its own ClientConnected hook
	// 2. Connect() calls IsBot() which can crash if pEdict isn't fully ready
	// 3. The IP is already set by core AMXX's player initialization
	// We only set the bot flag and ensure pEdict is initialized for DODX tracking
}

// KTP: SV_Spawn_f hook handler - replaces FN_ClientPutInServer_Post
// This is called when the client sends the "spawn" command to enter the game
static void DODX_OnSV_Spawn_f(IVoidHookChain<> *chain)
{
	// Need to figure out which client is spawning
	// During SV_Spawn_f, the host_client global points to the spawning client
	// We can use the current command client from AMXX

	// Call original first
	chain->callNext();

	// In extension mode, we need another way to get the spawning client
	// SV_Spawn_f doesn't pass the client directly, it uses host_client internally
	// For now, we'll rely on messages being sent AFTER the player is marked ingame
	// The message handler already checks mPlayer->ingame

	// TODO: If we need to mark players ingame earlier, we can hook SV_SendServerinfo
	// which is called when the client first connects and receives server info
}

// KTP: PF_changelevel_I hook handler - called BEFORE changelevel happens
// This is our opportunity to disable message processing before pointers go stale
static void DODX_OnChangelevel(IVoidHookChain<const char *, const char *> *chain, const char *s1, const char *s2)
{
	// KTP: CRITICAL - Disable message processing BEFORE changelevel
	// This prevents crashes from stale pointers during map transition
	g_bServerActive = false;
	g_pFirstEdict = nullptr;

	// Clear CP data — pEdict pointers become stale after map change
	mObjects.Clear();

	// Clear grenade tracking — edict pointers become stale after map change
	g_grenades.clear();

	// NOTE: Do NOT reset AlliesScore/AxisScore here.
	// KTPMatchHandler reads scores during its changelevel hook (save_first_half_scores).
	// If we zero them here, the plugin reads 0-0 instead of the actual half score.
	// Scores are zeroed in DODX_OnSV_ActivateServer instead (after plugin hooks have run).

	// Call original to perform the changelevel
	chain->callNext(s1, s2);
}

// KTP: SV_ActivateServer hook handler - fires after map entities are fully spawned.
// In extension mode, this replaces ServerActivate_Post for g_pFirstEdict init,
// and scans for dod_control_point entities (since InitObj message was missed).
static void DODX_OnSV_ActivateServer(IVoidHookChain<int> *chain, int runPhysics)
{
	// KTP: Reset team scores for new map. Done here (not in OnChangelevel) because
	// KTPMatchHandler reads scores during its changelevel hook for half-time save.
	AlliesScore = 0;
	AxisScore = 0;

	// KTP: Per-map resets that ServerDeactivate does — it is Metamod-only, so in
	// extension mode these carry over. A stale g_lastCapturedTime from the previous
	// map is in the future relative to the new map's clock, so the 2s correlation
	// window in the ObjScore path would tag scores with the old map's CP index.
	g_lastCapturedCP = -1;
	g_lastCapturedTime = 0.0f;
	for (int i = DODMAX_WEAPONS - DODMAX_CUSTOMWPNS; i < DODMAX_WEAPONS; i++)
		weaponData[i].needcheck = false;

	// KTP: the previous map's control-point master edict is freed by now, so the
	// cached pointer must go with it — dodx_get_score_tick_time() re-finds it
	// lazily on the new map.
	g_pCPMasterEdict = nullptr;

	// Ammo indices are assigned by this map's precache order, so last map's are wrong.
	DODX_ClearAmmoRegistry();

	DODX_ReadBSPMapInfo();

	// KTP: Set up g_pFirstEdict and g_bServerActive BEFORE chain->callNext().
	// Entities are already spawned (SV_SpawnServer ran before SV_ActivateServer).
	if (gpGlobals && gpGlobals->maxEntities > 0)
	{
		edict_t *pWorld = INDEXENT(0);
		// NOTE: do NOT use FNullEnt — edict 0 IS the world entity (index 0 is valid).
		// Same fix as 2.7.5 (b95b82c1) in DODX_SetupExtensionHooks; the sibling
		// per-map path was missed there, leaving this block as the mechanism by
		// which prod servers accumulate silent-forward state across rotations.
		if (pWorld)
		{
			g_pFirstEdict = pWorld;
			g_bServerActive = true;

			// Initialize player slots. ktpShot alongside: a stash or sighting
			// baseline captured on the previous map describes positions that no
			// longer exist, and cmdSeq pairing alone cannot see the map boundary.
			for (int i = 1; i <= gpGlobals->maxClients; i++)
			{
				GET_PLAYER_POINTER_I(i)->Init(i, g_pFirstEdict + i);
				GET_PLAYER_POINTER_I(i)->ktpShot.reset();
				GET_PLAYER_POINTER_I(i)->ktpVis.reset();
			}
			g_ktpCmdOwner = 0;
			// Pack timestamps ride sv.time, which restarts with the map; stale
			// stamps would sit in the new map's future. Coverage restarts too.
			KTPPackVisClearAll((double)gpGlobals->time);
		}
		else
		{
			MF_Log("dodx: SV_ActivateServer INDEXENT(0) returned NULL — forwards will stall until PreThink fallback or restart");
		}
	}

	// KTP: Register IMessage hook for InitObj BEFORE chain->callNext().
	// The game DLL sends InitObj during ServerActivate (inside callNext).
	// By registering the hook here, we catch it with correct CP ordering.
	// Message IDs are available because GameDLLInit ran before SV_ActivateServer.
	{
		static bool s_initObjHooked = false;
		if (!s_initObjHooked && g_pMessageManager)
		{
			// Look up InitObj message ID via engine function
			int initObjId = MF_GetUserMsgId ? MF_GetUserMsgId("InitObj") : 0;
			if (initObjId > 0)
			{
				gmsgInitObj = initObjId;
				g_pMessageManager->registerHook(initObjId, DODX_OnInitObjMessage, HC_PRIORITY_DEFAULT);
				s_initObjHooked = true;
			}
		}
	}

	// Call original — game DLL's ServerActivate runs here, sending InitObj.
	// Our DODX_OnInitObjMessage hook catches it and populates mObjects with
	// the authoritative CP ordering that matches SetObj indices.
	chain->callNext(runPhysics);

	// Entity scan as fallback if InitObj wasn't intercepted
	DODX_InitCPFromEntities();
}

// KTP: SV_DropClient hook handler - replaces FN_ClientDisconnect
static void DODX_OnSV_DropClient(IVoidHookChain<IGameClient *, bool, const char *> *chain, IGameClient *client, bool crash, const char *reason)
{
	// KTP: Call chain first so AMXX core can fire client_disconnected while player is still "ingame"
	// This matches the PRE/POST behavior of Metamod's ClientDisconnect/ClientDisconnect_Post hooks
	chain->callNext(client, crash, reason);

	// KTP: Now do DODX cleanup AFTER chain (POST behavior)
	// Safety check - gpGlobals must be valid
	if (client && gpGlobals)
	{
		int clientIndex = client->GetId() + 1;
		if (clientIndex >= 1 && clientIndex <= gpGlobals->maxClients)
		{
			CPlayer* pPlayer = GET_PLAYER_POINTER_I(clientIndex);
			if (pPlayer->ingame)
			{
				pPlayer->Disconnect();
				// Disconnect-side twin of the connect reset in the PreThink hook,
				// kept beside the sensor's other lifecycle points rather than in
				// Disconnect(): capture and read exist only on this hook path.
				pPlayer->ktpShot.reset();
				pPlayer->ktpVis.reset();
			}

			// The leaver was also someone's angular-velocity baseline. Clear every
			// pointer into this slot, or the next occupant's first sighting computes
			// a bearing change between two different people. Clearing only ever
			// downgrades a rate to "no prior", never fabricates one.
			for (int i = 1; i <= gpGlobals->maxClients; ++i)
			{
				KTPShotGeom &o = GET_PLAYER_POINTER_I(i)->ktpShot;
				if (o.prevTarget == clientIndex)
				{
					o.prevTarget = 0;
					o.prevTime = 0.0;
				}
			}

			// Same slot-reuse hazard for the pack table: the leaver's packed-time
			// row and column must not describe the next occupant. Clearing only
			// ever turns an answer into unknown, never the reverse. gpGlobals is
			// non-null on this path (dereferenced by the loop above).
			if (clientIndex >= 1 && clientIndex <= 32)
				KTPPackVisClearSlot(clientIndex, (double)gpGlobals->time);
		}
	}
}

// KTP: InitObj IMessage hook — passthrough only.
// NOTE: IMessageManager does NOT dispatch during SV_ActivateServer, so this hook
// only catches client-connect InitObj messages (per-player CP data, not the initial
// full CP list). CP ordering comes from entity scan + BSP point_index reorder instead.
static void DODX_OnInitObjMessage(IVoidHookChain<IMessage *> *chain, IMessage *msg)
{
	chain->callNext(msg);
}

// info_doddetect drives the British/para weapon-id remaps in Utils.cpp and the
// grenade id in the TraceLine paths. Metamod reads it through DispatchKeyValue_Post,
// which never fires in extension mode — and keyvalues can't be read back off the
// spawned entity — so read them straight out of the BSP instead. Values are always
// assigned (g_map.Init() runs once per process here, not per map, so a previous
// map's British flag would otherwise carry over onto a US map).
static void DODX_ReadBSPMapInfo()
{
	g_map.detect_allies_country = 0;
	g_map.detect_allies_paras = 0;
	g_map.detect_axis_paras = 0;

	char *entData = DODX_LoadBSPEntityLump();
	if (!entData)
		return;

	char *pos = entData;
	bool found = false;

	while (*pos && !found)
	{
		while (*pos && *pos != '{') pos++;
		if (!*pos) break;
		pos++;

		char classname[64] = "";
		int allies_country = 0, allies_paras = 0, axis_paras = 0;

		while (*pos && *pos != '}')
		{
			while (*pos && (*pos == ' ' || *pos == '\t' || *pos == '\r' || *pos == '\n')) pos++;
			if (*pos == '}') break;

			// A truncated lump can leave us on the terminator here; pos++ would
			// step past the allocation and the outer loop would walk the heap.
			if (!*pos) break;
			if (*pos != '"') { pos++; continue; }
			pos++;
			char key[64] = "";
			int ki = 0;
			while (*pos && *pos != '"') {
				if (ki < 63) key[ki++] = *pos;
				pos++;
			}
			key[ki] = '\0';
			if (*pos == '"') pos++;

			while (*pos && (*pos == ' ' || *pos == '\t')) pos++;

			if (*pos != '"') continue;
			pos++;
			char value[256] = "";
			int vi = 0;
			while (*pos && *pos != '"') {
				if (vi < 255) value[vi++] = *pos;
				pos++;
			}
			value[vi] = '\0';
			if (*pos == '"') pos++;

			if (strcmp(key, "classname") == 0)
				strncpy(classname, value, 63);
			else if (strcmp(key, "detect_allies_country") == 0)
				allies_country = atoi(value);
			else if (strcmp(key, "detect_allies_paras") == 0)
				allies_paras = atoi(value);
			else if (strcmp(key, "detect_axis_paras") == 0)
				axis_paras = atoi(value);
		}

		if (*pos == '}') pos++;

		if (strcmp(classname, "info_doddetect") == 0)
		{
			g_map.detect_allies_country = allies_country;
			g_map.detect_allies_paras = allies_paras;
			g_map.detect_axis_paras = axis_paras;
			g_map.initialized = true;
			found = true;
		}
	}

	free(entData);

	// Unconditional: these values were silently wrong for years, so "present and
	// all zero" must be distinguishable from "never parsed".
	if (found)
		MF_Log("[DODX] BSP: %s info_doddetect — allies_country=%d allies_paras=%d axis_paras=%d",
			STRING(gpGlobals->mapname), g_map.detect_allies_country,
			g_map.detect_allies_paras, g_map.detect_axis_paras);
	else
		MF_Log("[DODX] BSP: %s has no info_doddetect — detect_* all 0", STRING(gpGlobals->mapname));
}

// KTP: Setup extension mode hooks
static bool DODX_SetupExtensionHooks()
{
	if (!MF_IsExtensionMode || !MF_IsExtensionMode())
		return false;

	// KTP: Get g_engfuncs from core AMXX - essential for extension mode
	// Without this, engine function calls like ENTINDEX will crash
	if (MF_GetEngineFuncs)
	{
		enginefuncs_t* pEngfuncs = (enginefuncs_t*)MF_GetEngineFuncs();
		if (pEngfuncs)
			memcpy(&g_engfuncs, pEngfuncs, sizeof(enginefuncs_t));
	}

	// KTP: Get gpGlobals from core AMXX - essential for extension mode
	// Without this, gpGlobals is NULL and many engine-dependent functions fail
	if (MF_GetGlobalVars)
		gpGlobals = (globalvars_t*)MF_GetGlobalVars();

	// Get ReHLDS hookchains
	if (MF_GetRehldsHookchains)
		g_pRehldsHookchains = (IRehldsHookchains*)MF_GetRehldsHookchains();

	// Get message manager
	if (MF_GetRehldsMessageManager)
		g_pMessageManager = (IMessageManager*)MF_GetRehldsMessageManager();

	if (!g_pRehldsHookchains)
		return false;

	// NOTE: ClientConnected hook not needed - bot detection uses FL_FAKECLIENT, IP never used

	// Register PlayerPreThink hook - main stats tracking loop
	// Also handles player initialization in extension mode (replaces ClientPutInServer_Post)
	if (g_pRehldsHookchains->SV_PlayerRunPreThink())
		g_pRehldsHookchains->SV_PlayerRunPreThink()->registerHook(DODX_OnPlayerPreThink, HC_PRIORITY_DEFAULT);

	// KTP: Register changelevel hook to disable message processing before map change
	// This prevents crashes from stale pointers during map transition
	if (g_pRehldsHookchains->PF_changelevel_I())
		g_pRehldsHookchains->PF_changelevel_I()->registerHook(DODX_OnChangelevel, HC_PRIORITY_DEFAULT);

	// KTP: Register TraceLine hook for:
	// 1. Player aiming detection (records iHitgroup for headshot tracking)
	// 2. Grenade/rocket tracking (fires dod_grenade_explosion, dod_rocket_explosion forwards)
	// NOTE: This is a POST hook - reads trace results only, does NOT modify.
	// Safe for wallpen because it never changes TraceResult or supercedes the call.
	if (g_pRehldsHookchains->PF_TraceLine())
		g_pRehldsHookchains->PF_TraceLine()->registerHook(DODX_OnTraceLine, HC_PRIORITY_DEFAULT);

	// KTP: Register SV_ActivateServer hook - fires after map entities are spawned.
	// In extension mode, this replaces ServerActivate_Post for:
	// 1. Setting g_pFirstEdict and g_bServerActive
	// 2. Scanning dod_control_point entities (InitObj was missed)
	if (g_pRehldsHookchains->SV_ActivateServer())
		g_pRehldsHookchains->SV_ActivateServer()->registerHook(DODX_OnSV_ActivateServer, HC_PRIORITY_DEFAULT);

	// KTP: Register SV_DropClient hook - the extension-mode replacement for FN_ClientDisconnect.
	// Without it CPlayer::Disconnect() never runs in production, so a slot's ingame flag,
	// weapons[] stats, savedScore and observed-death tally are inherited by the next player
	// who reuses the slot mid-map.
	// Chain order is load-bearing: the handler calls chain->callNext() first so the core's
	// SV_DropClient hook fires client_disconnected while the slot is still ingame (plugins
	// save stats there). dodx registers from OnAmxxAttach, i.e. before the core registers its
	// own SV_DropClient hook, and addHook appends equal priorities — so dodx sits outermost
	// and its cleanup runs after the forward. Re-check that if either registration moves.
	if (g_pRehldsHookchains->SV_DropClient())
		g_pRehldsHookchains->SV_DropClient()->registerHook(DODX_OnSV_DropClient, HC_PRIORITY_DEFAULT);

	// KTP: Register SV_CreatePacketEntities hook - the tier-2.7 pack recorder.
	// Post-style: records after callNext and modifies nothing the engine sends.
	// The live flag is what turns sampler answers from "unknown" into real ones,
	// so it flips only here and back off in cleanup.
	if (g_pRehldsHookchains->SV_CreatePacketEntities())
	{
		g_pRehldsHookchains->SV_CreatePacketEntities()->registerHook(DODX_OnSVCreatePacketEntities, HC_PRIORITY_DEFAULT);
		g_ktpPackRecorderLive = true;
		// Coverage restarts on the down->up transition itself, not only at the
		// enumerated lifecycle sites: "recording was interrupted" is the actual
		// precondition, and a cleanup->setup round trip reaches none of those
		// sites. Without gpGlobals the table stays never-stamped (since==0),
		// which the sampler reads as unknown — never as covered.
		if (gpGlobals)
			KTPPackVisClearAll((double)gpGlobals->time);
	}

	// KTP: Initialize g_pFirstEdict NOW as fallback.
	// The SV_ActivateServer hook fires on map changes, but on the FIRST map load
	// the server has already activated BEFORE this module registers its hooks.
	// Without this, g_pFirstEdict stays NULL until the first map change, breaking
	// all player tracking (PreThink bails out, CHECK_PLAYER fails, natives return 0).
	if (!g_pFirstEdict)
	{
		edict_t *pWorld = NULL;
		if (gpGlobals && gpGlobals->maxEntities > 0)
			pWorld = INDEXENT(0);
		else if (g_engfuncs.pfnPEntityOfEntIndex)
			pWorld = g_engfuncs.pfnPEntityOfEntIndex(0);

		// NOTE: Do NOT use FNullEnt — edict 0 IS the world entity (index 0 is valid)
		if (pWorld)
		{
			g_pFirstEdict = pWorld;
			g_bServerActive = true;

			int maxCl = gpGlobals ? gpGlobals->maxClients : 32;
			for (int i = 1; i <= maxCl; i++)
			{
				GET_PLAYER_POINTER_I(i)->Init(i, g_pFirstEdict + i);
				// ktpShot is zero-initialised at this point (module attach), so this
				// is shape-parity with the other two Init() loops, not a live fix.
				GET_PLAYER_POINTER_I(i)->ktpShot.reset();
				GET_PLAYER_POINTER_I(i)->ktpVis.reset();
			}
			// Not shape-parity: the since[] array is zero at attach, and on this
			// first-map path sv.time is already past zero -- without this, the
			// coverage test would pass a window nothing ever recorded.
			if (gpGlobals)
				KTPPackVisClearAll((double)gpGlobals->time);
		}
	}

	// Same first-map gap: DODX_OnSV_ActivateServer already ran, so without this
	// the boot map after every restart runs with detect_* = 0 until a changelevel.
	DODX_ReadBSPMapInfo();

	return true;
}

// KTP: Begin handler - called once at the start of each message to set up DODX's mPlayer/mState
static void DODX_OnMsgBegin(int msg_id, int dest, int player_index, edict_t* ed)
{
	// KTP: Bounds check msg_id before indexing into modMsgs/modMsgsEnd arrays
	if (msg_id < 0 || msg_id >= MAX_REG_MSGS)
		return;

	// KTP: Skip message processing if server is not active (during map change).
	// Exception: InitObj is global CP data sent during ServerActivate and on player
	// connect. We must process it regardless of g_bServerActive state because:
	// 1. During boot, g_bServerActive may be false due to changelevel/restart cycles
	// 2. Client_InitObj doesn't depend on player state or g_pFirstEdict
	// 3. Processing it early gives us correct CP ordering from the game DLL
	if (!g_bServerActive && msg_id != gmsgInitObj)
		return;

	// KTP: Use the player_index passed from the core, which has already been validated
	// This is more reliable than recalculating from edict
	if (gpGlobals && player_index >= 1 && player_index <= gpGlobals->maxClients)
	{
		mPlayerIndex = player_index;
		mPlayer = GET_PLAYER_POINTER_I(mPlayerIndex);
	}
	else
	{
		mPlayerIndex = 0;
		mPlayer = NULL;
	}

	mDest = dest;
	mState = 0;

	// Get the callbacks for this message type
	function = modMsgs[msg_id];
	endfunction = modMsgsEnd[msg_id];
}

// KTP: Register message hooks after message IDs are known
void DODX_RegisterMessageHooks()
{
	if (!g_bExtensionMode)
	{
		return;
	}

	// KTP: Use the new module message handler API instead of direct IMessageManager calls
	// This allows KTPAMXX core to forward messages to DODX handlers
	if (!MF_RegModuleMsgHandler || !MF_RegModuleMsgBeginHandler)
	{
		MF_Log("[DODX] Error: Module message API not available - cannot register message hooks");
		return;
	}

	int hookCount = 0;
	for (int i = 0; g_user_msg[i].name; ++i)
	{
		if (*g_user_msg[i].id > 0 && g_user_msg[i].func)
		{
			// Register begin handler for each message to set up mPlayer/mState
			MF_RegModuleMsgBeginHandler(*g_user_msg[i].id, DODX_OnMsgBegin);

			// Cast funEventCall to PFN_MODULE_MSG_HANDLER (both are void (*)(void*))
			if (MF_RegModuleMsgHandler(*g_user_msg[i].id, (PFN_MODULE_MSG_HANDLER)g_user_msg[i].func, g_user_msg[i].endmsg))
			{
				hookCount++;
			}
			else
			{
				MF_Log("[DODX] Warning: Failed to register handler for msg '%s' id=%d", g_user_msg[i].name, *g_user_msg[i].id);
			}
		}
	}
}

// KTP: BSP entity lump parser — reads point_index and point_default_owner keyvalues
// for dod_control_point entities.
//
// point_index: the game DLL orders CPs by point_index (1-based) from the BSP entity
// lump. SetObj cp_index = point_index - 1. FindEntityByClassname iteration order
// (edict number) does NOT match this ordering, so we must read point_index from the
// BSP and reorder.
//
// point_default_owner: the team that owns the CP at round start (0/absent neutral,
// 1 allies, 2 axis). In extension mode this is the ONLY source for it at map load —
// the pdata `owner`/`default_owner` fields read as 0 for every CP, and the engine
// sends SetObj only when a CP changes hands. The round-restart cascade does restore
// defaults, so the gap is map load → first restart: the whole first round.
struct bsp_cp_info {
	int point_index;      // -1 when absent OR explicitly negative; see hasPointIndex
	int default_owner;    // 0 neutral / 1 allies / 2 axis (0 when key absent)
	float origin_x;
	float origin_y;
	float origin_z;
};

// Sizes bspAll/bspCPs/bspUsed, which are indexed together — widening one and not
// the others is a stack overwrite, not a compile error. NOT the bound on sortedObj[]
// or used[] below: those are indexed by mObjects.count, capped at 12 by objinfo_t
// obj[12] in dodx.h, so SHRINKING this constant would overflow them.
#define BSP_MAX_CPS 12
static_assert(BSP_MAX_CPS <= 12, "mObjects.obj[] is 12 (dodx.h)");

// Caller owns the returned buffer (free()). NULL on any failure; the reason is logged.
static char *DODX_LoadBSPEntityLump()
{
	const char *mapName = STRING(gpGlobals->mapname);

	// Build path using game directory (MF_BuildPathnameR prepends mod dir)
	char bspPath[512];
	MF_BuildPathnameR(bspPath, sizeof(bspPath), "maps/%s.bsp", mapName);

	FILE *fp = fopen(bspPath, "rb");
	if (!fp)
	{
		// Fallback: try relative path (engine may set cwd to game dir)
		char bspPathRel[256];
		snprintf(bspPathRel, sizeof(bspPathRel), "maps/%s.bsp", mapName);
		fp = fopen(bspPathRel, "rb");
		if (!fp)
		{
			MF_Log("[DODX] BSP: Could not open '%s' or '%s'", bspPath, bspPathRel);
			return 0;
		}
		MF_Log("[DODX] BSP: Opened via relative path '%s'", bspPathRel);
	}

	// GoldSrc BSP v30: 4-byte version, then 15 lump entries (offset + length, 8 bytes each)
	// Entity lump is lump 0 (first entry, at bytes 4-11)
	// Short read and wrong version are separate failures: folded together, a
	// truncated file reports whatever `version` holds as its version number.
	int version = 0;
	if (fread(&version, 4, 1, fp) != 1)
	{
		MF_Log("[DODX] BSP: Short read on version field (file truncated?)");
		fclose(fp);
		return 0;
	}
	if (version != 30)
	{
		MF_Log("[DODX] BSP: Invalid version %d (expected 30)", version);
		fclose(fp);
		return 0;
	}

	int entOffset, entLength;
	if (fread(&entOffset, 4, 1, fp) != 1 || fread(&entLength, 4, 1, fp) != 1)
	{
		MF_Log("[DODX] BSP: Failed to read entity lump header");
		fclose(fp);
		return 0;
	}

	if (entLength <= 0 || entLength > 2 * 1024 * 1024)
	{
		MF_Log("[DODX] BSP: Entity lump length invalid (%d)", entLength);
		fclose(fp);
		return 0;
	}

	char *entData = (char *)malloc(entLength + 1);
	if (!entData)
	{
		// Was a silent return: CP init falls back to the pdata read with no trace
		// of why, the same failure mode the per-CP miss log exists to prevent.
		MF_Log("[DODX] BSP: Allocation failed for entity lump (%d bytes)", entLength);
		fclose(fp);
		return 0;
	}

	fseek(fp, entOffset, SEEK_SET);
	if ((int)fread(entData, 1, entLength, fp) != entLength)
	{
		MF_Log("[DODX] BSP: Failed to read entity lump data");
		free(entData);
		fclose(fp);
		return 0;
	}
	entData[entLength] = '\0';
	fclose(fp);
	return entData;
}

// Fills cpInfo with EVERY dod_control_point in the BSP (in entity-lump order) and
// returns that count. *outWithIndex receives how many carried a usable point_index —
// the reorder gate keys off that, NOT the return value, so adding default-owner
// parsing does not change ordering behaviour.
//
// One behavioural caveat: the maxCPs cutoff now counts every CP, where before it
// counted only indexed ones. On a map with more than maxCPs total CP entities that
// mixes indexed and non-indexed ones, parsing could stop before a later indexed
// entity the old code would still have reached. Moot in practice — mObjects itself
// caps at 12 and no DoD map ships more than 9 CPs.
static int DODX_ReadBSPControlPoints(bsp_cp_info *cpInfo, int maxCPs, int *outWithIndex)
{
	if (outWithIndex)
		*outWithIndex = 0;

	const char *mapName = STRING(gpGlobals->mapname);
	char *entData = DODX_LoadBSPEntityLump();
	if (!entData)
		return 0;

	// Parse entity lump for dod_control_point entities
	int cpCount = 0;
	int withIndex = 0;
	int negIndexCPs = 0;   // explicit point_index < 0 -- a map choice, not an absent key
	char *pos = entData;

	while (*pos && cpCount < maxCPs)
	{
		while (*pos && *pos != '{') pos++;
		if (!*pos) break;
		pos++;

		char classname[64] = "";
		// -1 doubles as "key absent" AND as a value maps actually write, so the
		// two were indistinguishable and both got dropped by the >= 0 test
		// below. Track presence separately.
		int point_index = -1;
		bool hasPointIndex = false;
		int default_owner = 0;
		float origin_x = 0, origin_y = 0, origin_z = 0;

		while (*pos && *pos != '}')
		{
			while (*pos && (*pos == ' ' || *pos == '\t' || *pos == '\r' || *pos == '\n')) pos++;
			if (*pos == '}') break;

			// Read key
			// A truncated lump can leave us on the terminator here; pos++ would
			// step past the allocation and the outer loop would walk the heap.
			if (!*pos) break;
			if (*pos != '"') { pos++; continue; }
			pos++;
			char key[64] = "";
			int ki = 0;
			while (*pos && *pos != '"') {
				if (ki < 63) key[ki++] = *pos;
				pos++;
			}
			key[ki] = '\0';
			if (*pos == '"') pos++;

			while (*pos && (*pos == ' ' || *pos == '\t')) pos++;

			// Read value (may be very long, e.g. wad paths > 500 chars)
			if (*pos != '"') continue;
			pos++;
			char value[256] = "";
			int vi = 0;
			while (*pos && *pos != '"') {
				if (vi < 255) value[vi++] = *pos;
				pos++;
			}
			value[vi] = '\0';
			if (*pos == '"') pos++;

			if (strcmp(key, "classname") == 0)
				strncpy(classname, value, 63);
			else if (strcmp(key, "point_index") == 0)
			{
				point_index = atoi(value);
				hasPointIndex = true;
			}
			else if (strcmp(key, "point_default_owner") == 0)
			{
				// Clamp at the parse site: this value is map-supplied and reaches
				// WRITE_BYTE (CMisc.cpp) and Pawn (CP_owner / CP_default_owner).
				// Out of range is not a crash — MSG_WriteByte casts without a range
				// error — but a nonsense value would truncate to something plausible,
				// and a public fork should not propagate it. 0=neutral, 1=allies,
				// 2=axis; anything else is treated as neutral.
				default_owner = atoi(value);
				if (default_owner < 0 || default_owner > 2)
				{
					// Log it: a silent cap turns a map bug into invisible behaviour.
					MF_Log("[DODX] BSP: point_default_owner=%d out of range on %s — treating as neutral",
						default_owner, STRING(gpGlobals->mapname));
					default_owner = 0;
				}
			}
			else if (strcmp(key, "origin") == 0)
				sscanf(value, "%f %f %f", &origin_x, &origin_y, &origin_z);
		}

		if (*pos == '}') pos++;

		if (strcmp(classname, "dod_control_point") == 0)
		{
			if (hasPointIndex && point_index < 0)
				negIndexCPs++;
			// Store EVERY CP: the default-owner seed matches by origin and must see
			// the unindexed ones too. The reorder still consumes only the indexed
			// subset, counted here.
			cpInfo[cpCount].point_index = point_index;
			cpInfo[cpCount].default_owner = default_owner;
			cpInfo[cpCount].origin_x = origin_x;
			cpInfo[cpCount].origin_y = origin_y;
			cpInfo[cpCount].origin_z = origin_z;
			if (point_index >= 0)
				withIndex++;
			cpCount++;
		}
	}

	free(entData);
	if (outWithIndex)
		*outWithIndex = withIndex;
	MF_Log("[DODX] BSP: Parsed %s — %d dod_control_point, %d with point_index, %d with a NEGATIVE point_index",
		mapName, cpCount, withIndex, negIndexCPs);
	return cpCount;
}

// KTP: Initialize CP tracking from entity data (extension mode only)
// In extension mode, the InitObj message is sent during ServerActivate before
// our message hooks are installed. This function scans for dod_control_point
// entities and populates mObjects directly from entity private data, then
// reorders by BSP point_index to match the game DLL's SetObj cp_index mapping.
static void DODX_InitCPFromEntities()
{
	if (!g_bExtensionMode)
		return;

	MF_Log("[DODX] CP entity scan starting");

	mObjects.Clear();
	g_cpOrderingFinalized = false;  // KTP: Allow first matching InitObj to reorder mObjects to DLL order

	// Use FindEntityByClassname instead of GETEDICT loop — pfnPEntityOfEntIndex
	// hangs during OnPluginsLoaded in extension mode, but pfnFindEntityByString is safe.
	edict_t *pEdict = NULL;
	while ((pEdict = FindEntityByClassname(pEdict, "dod_control_point")) != NULL && mObjects.count < 12)
	{
		if (!pEdict->pvPrivateData)
			continue;

		int idx = mObjects.count;
		pd_dcp &cpd = GET_CP_PD(pEdict);

		mObjects.obj[idx].pEdict = pEdict;
		mObjects.obj[idx].index = cpd.flag_id;
		mObjects.obj[idx].default_owner = cpd.owner;
		mObjects.obj[idx].owner = cpd.owner;
		mObjects.obj[idx].visible = 1;
		mObjects.obj[idx].icon_neutral = cpd.icon_neutral;
		mObjects.obj[idx].icon_allies = cpd.icon_allies;
		mObjects.obj[idx].icon_axis = cpd.icon_axis;
		// Read origin from edict vars, not pdata — the pdata origin offsets
		// are unreliable (observed as (0, world_x) on dod_anzio instead of (world_x, world_y)).
		mObjects.obj[idx].origin_x = pEdict->v.origin[0];
		mObjects.obj[idx].origin_y = pEdict->v.origin[1];
		mObjects.obj[idx].areaflags = 0;
		mObjects.obj[idx].pAreaEdict = NULL;
		mObjects.count++;
	}

	if (mObjects.count > 0)
	{
		// One BSP parse, two consumers: default ownership (below) and the
		// point_index reorder (further down). bspAll holds EVERY dod_control_point;
		// bspWithIndex counts the subset carrying a usable point_index, which is the
		// only thing the reorder gate may key off.
		bsp_cp_info bspAll[BSP_MAX_CPS];
		int bspWithIndex = 0;
		int bspTotal = DODX_ReadBSPControlPoints(bspAll, BSP_MAX_CPS, &bspWithIndex);

		// Seed default/current ownership from the BSP, matched by origin (the same
		// identifier the reorder uses — targetname is empty on many maps). Done
		// BEFORE any reordering, and the reorder moves whole objinfo_t structs, so
		// the values travel with their CP either way.
		//
		// Without this, `owner` starts at 0 (neutral) for every CP on a default-owned
		// map (dod_donner, dod_kalt, dod_saints2_*) until the flag is captured or the
		// round-restart cascade restores it — i.e. for the whole first round.
		//
		// Consume matches one-to-one, same as the reorder below. Without this the
		// match is many-to-one and silently first-wins: two CPs stacked in z, or
		// two entities that both fell back to origin (0,0,0) because the key was
		// absent, would take the same BSP entry with no diagnostic.
		bool bspUsed[BSP_MAX_CPS] = {};

		// Skip on an unreadable BSP: every CP would log its own miss, burying the
		// one line that says why (the loader already logged the open failure).
		for (int oi = 0; bspTotal > 0 && oi < mObjects.count; oi++)
		{
			bool matched = false;
			for (int bi = 0; bi < bspTotal; bi++)
			{
				if (bspUsed[bi]) continue;
				float dx = mObjects.obj[oi].origin_x - bspAll[bi].origin_x;
				float dy = mObjects.obj[oi].origin_y - bspAll[bi].origin_y;
				if (dx > -1.0f && dx < 1.0f && dy > -1.0f && dy < 1.0f)
				{
					mObjects.obj[oi].default_owner = bspAll[bi].default_owner;
					mObjects.obj[oi].owner = bspAll[bi].default_owner;
					bspUsed[bi] = true;
					matched = true;
					break;
				}
			}
			// Log the miss. Without it a failed match is indistinguishable from a
			// genuinely neutral map: default_owner just stays at the pdata read,
			// which is 0 for every CP — i.e. it looks fixed and silently isn't.
			if (!matched)
			{
				MF_Log("[DODX] BSP: no default_owner match for CP[%d] origin=(%.0f,%.0f) — leaving owner=%d",
					oi, mObjects.obj[oi].origin_x, mObjects.obj[oi].origin_y, mObjects.obj[oi].owner);
			}
		}

		// Reorder mObjects by BSP point_index to match game DLL's SetObj cp_index.
		// SetObj cp_index = point_index - 1 (point_index is 1-based in the BSP).
		if (mObjects.count > 1)
		{
			// Filtered view: only entries with a usable point_index, preserving the
			// exact input the reorder saw before default-owner parsing existed.
			bsp_cp_info bspCPs[BSP_MAX_CPS];
			int bspCount = 0;
			for (int bi = 0; bi < bspTotal && bspCount < bspWithIndex; bi++)
			{
				if (bspAll[bi].point_index >= 0)
					bspCPs[bspCount++] = bspAll[bi];
			}

#ifdef DODX_DEBUG_CP_INIT
			// Logged BEFORE the count gate so it still fires where the reorder short-circuits —
			// those are the maps worth investigating. flag_id is the DLL-assigned pdata index,
			// which separates a master/slave collapse from a coincidental point_index collision.
			// Build with -DDODX_DEBUG_CP_INIT=1; off in prod because it is ~25 lines per map load.
			MF_Log("[DODX] CP scan: entities=%d bsp_total=%d bsp_with_index=%d filtered=%d gate=%s",
				mObjects.count, bspTotal, bspWithIndex, bspCount,
				bspCount == mObjects.count ? "pass" : "SHORT-CIRCUIT");
			for (int oi = 0; oi < mObjects.count; oi++)
			{
				edict_t *pe = mObjects.obj[oi].pEdict;
				const char *tn = pe ? STRING(pe->v.targetname) : "?";
				MF_Log("[DODX] CP scan: entity[%d] flag_id=%d origin=(%.0f,%.0f) targetname='%s'",
					oi, mObjects.obj[oi].index, mObjects.obj[oi].origin_x,
					mObjects.obj[oi].origin_y, tn);
			}
			// bspAll, not the filtered view: an entry dropped for a missing point_index is
			// exactly what a collapse looks like, so it has to be visible.
			for (int bi = 0; bi < bspTotal; bi++)
			{
				MF_Log("[DODX] CP scan: bsp[%d] point_index=%d origin=(%.0f,%.0f) default_owner=%d",
					bi, bspAll[bi].point_index, bspAll[bi].origin_x,
					bspAll[bi].origin_y, bspAll[bi].default_owner);
			}
#endif

			if (bspCount == mObjects.count)
			{
				// Sort BSP entries by point_index (ascending) — insertion sort for small N
				for (int i = 1; i < bspCount; i++)
				{
					bsp_cp_info tmp = bspCPs[i];
					int j = i - 1;
					while (j >= 0 && bspCPs[j].point_index > tmp.point_index)
					{
						bspCPs[j + 1] = bspCPs[j];
						j--;
					}
					bspCPs[j + 1] = tmp;
				}

				// Match each sorted BSP entry to a scanned entity by origin coordinates.
				// Origin is unique per CP and available on both BSP and entity.
				// Targetname can be empty on many maps so it's unreliable for matching.
				objinfo_t sortedObj[12];
				bool used[12] = {};
				bool matched = true;

				for (int si = 0; si < bspCount; si++)
				{
					bool found = false;
					for (int oi = 0; oi < mObjects.count; oi++)
					{
						if (used[oi]) continue;
						// Match by origin (pdata origin matches BSP origin exactly)
						float dx = mObjects.obj[oi].origin_x - bspCPs[si].origin_x;
						float dy = mObjects.obj[oi].origin_y - bspCPs[si].origin_y;
						if (dx > -1.0f && dx < 1.0f && dy > -1.0f && dy < 1.0f)
						{
							sortedObj[si] = mObjects.obj[oi];
							sortedObj[si].index = bspCPs[si].point_index;
							used[oi] = true;
							found = true;
							break;
						}
					}
					if (!found)
					{
						matched = false;
						MF_Log("[DODX] BSP sort: no entity match for point_index=%d origin=(%.0f,%.0f)",
							bspCPs[si].point_index, bspCPs[si].origin_x, bspCPs[si].origin_y);
						break;
					}
				}

				if (matched)
				{
					memcpy(mObjects.obj, sortedObj, sizeof(objinfo_t) * mObjects.count);
				}
				else
				{
					MF_Log("[DODX] BSP sort failed — using entity scan order (cp names may be wrong)");
				}
			}
			else if (bspCount > 0)
			{
				MF_Log("[DODX] BSP CP count with point_index (%d) != entity scan count (%d), skipping reorder",
					bspCount, mObjects.count);
			}
			else if (mObjects.count > 0)
			{
				// Not "no CPs" -- every CP was rejected, so the reorder has
				// nothing and ordering falls back to edict-scan order, which
				// does NOT match the game DLL's (see the header comment on this
				// function). dod_saints2_b3e/_b2 hit this: 5 CPs, all with an
				// explicit point_index of -1. Say so instead of logging it as a
				// neutral statistic.
				// bspTotal disambiguates: 0 means the BSP was unreadable, not that
				// the map's point_index keys are bad. Same conflation 98aba6ec removed.
				MF_Log("[DODX] WARNING: %d control points but NONE carried a usable point_index "
					"(%d parsed from BSP) — CP ORDER IS UNRELIABLE on this map "
					"(falling back to entity scan order)",
					mObjects.count, bspTotal);
			}
			else
			{
				MF_Log("[DODX] BSP parse returned 0 CPs — using entity scan order");
			}
		}

		MF_Log("[DODX] CP init complete: %d control points", mObjects.count);
		for (int i = 0; i < mObjects.count; i++)
		{
			edict_t *pe = mObjects.obj[i].pEdict;
			const char *tn = pe ? STRING(pe->v.targetname) : "?";
			const char *nn = pe ? STRING(pe->v.netname) : "?";
			// default_owner is near-redundant with owner here, but it is the one field
			// that separates "seeded from the BSP" from "read 0 out of pdata" in a log.
			MF_Log("[DODX]   CP[%d] point_index=%d owner=%d default_owner=%d targetname='%s' netname='%s'",
				i, mObjects.obj[i].index, mObjects.obj[i].owner,
				mObjects.obj[i].default_owner, tn, nn);
		}

		if (iFInitCP >= 0)
			MF_ExecuteForward(iFInitCP);
	}
	else
	{
		MF_Log("[DODX] CP entity scan: no dod_control_point entities found");
	}
}

// KTP: Cleanup extension mode hooks
static void DODX_CleanupExtensionHooks()
{
	if (!g_bExtensionMode)
		return;

	if (g_pRehldsHookchains)
	{
		// Unregister PlayerPreThink hook
		if (g_pRehldsHookchains->SV_PlayerRunPreThink())
			g_pRehldsHookchains->SV_PlayerRunPreThink()->unregisterHook(DODX_OnPlayerPreThink);

		// Unregister changelevel hook
		if (g_pRehldsHookchains->PF_changelevel_I())
			g_pRehldsHookchains->PF_changelevel_I()->unregisterHook(DODX_OnChangelevel);

		// Unregister TraceLine hook
		if (g_pRehldsHookchains->PF_TraceLine())
			g_pRehldsHookchains->PF_TraceLine()->unregisterHook(DODX_OnTraceLine);

		// Unregister SV_ActivateServer hook
		if (g_pRehldsHookchains->SV_ActivateServer())
			g_pRehldsHookchains->SV_ActivateServer()->unregisterHook(DODX_OnSV_ActivateServer);

		// Unregister SV_DropClient hook
		if (g_pRehldsHookchains->SV_DropClient())
			g_pRehldsHookchains->SV_DropClient()->unregisterHook(DODX_OnSV_DropClient);

		// Unregister the pack recorder; with the flag down, samples read unknown
		// instead of trusting a table nothing feeds.
		if (g_pRehldsHookchains->SV_CreatePacketEntities())
			g_pRehldsHookchains->SV_CreatePacketEntities()->unregisterHook(DODX_OnSVCreatePacketEntities);
		g_ktpPackRecorderLive = false;
	}

	// KTP: Unregister InitObj IMessage hook
	if (g_pMessageManager && gmsgInitObj > 0)
		g_pMessageManager->unregisterHook(gmsgInitObj, DODX_OnInitObjMessage);

	// KTP: Unregister module message handlers via new API
	if (MF_UnregModuleMsgHandler)
	{
		for (int i = 0; g_user_msg[i].name; ++i)
		{
			if (*g_user_msg[i].id > 0 && g_user_msg[i].func)
			{
				MF_UnregModuleMsgHandler(*g_user_msg[i].id, (PFN_MODULE_MSG_HANDLER)g_user_msg[i].func, g_user_msg[i].endmsg);
			}
		}
	}

	g_pRehldsHookchains = nullptr;
	g_pMessageManager = nullptr;
}

