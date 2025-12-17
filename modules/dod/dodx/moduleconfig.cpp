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

// KTP: Forward declarations for ReHLDS hook handlers
static void DODX_OnTraceLine(IVoidHookChain<const float *, const float *, int, edict_t *, TraceResult *> *chain,
                              const float *v1, const float *v2, int fNoMonsters, edict_t *e, TraceResult *ptr);
static void DODX_OnSetClientKeyValue(IVoidHookChain<int, char *, const char *, const char *> *chain,
                                      int clientIndex, char *infobuffer, const char *key, const char *value);
static int DODX_OnRegUserMsg(IHookChain<int, const char *, int> *chain, const char *pszName, int iSize);
static void DODX_OnMessageHandler(IVoidHookChain<IMessage *> *chain, IMessage *msg);
static void DODX_OnClientConnected(IVoidHookChain<IGameClient *> *chain, IGameClient *client);
static void DODX_OnSV_Spawn_f(IVoidHookChain<> *chain);
static void DODX_OnSV_DropClient(IVoidHookChain<IGameClient *, bool, const char *> *chain, IGameClient *client, bool crash, const char *reason);
static void DODX_OnChangelevel(IVoidHookChain<const char *, const char *> *chain, const char *s1, const char *s2);

// KTP: Forward declarations for extension mode setup/cleanup functions
static bool DODX_SetupExtensionHooks();
static void DODX_CleanupExtensionHooks();
void DODX_DetectMapInfo();
void DODX_RegisterMessageHooks();

funEventCall modMsgsEnd[MAX_REG_MSGS];
funEventCall modMsgs[MAX_REG_MSGS];
void (*function)(void*);
void (*endfunction)(void*);
CPlayer* mPlayer;
CPlayer players[33];
CMapInfo g_map;

// KTP: First edict pointer for ENTINDEX_SAFE
// Initialized in ServerActivate_Post to enable safe entity index calculation
edict_t* g_pFirstEdict = nullptr;

// KTP: Server active flag - prevents message processing during map changes
bool g_bServerActive = false;

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
int iFGrenadeExplode = -1;
int iFRocketExplode = -1;
int iFObjectTouched = -1;
int iFStaminaForward = -1;

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

RankSystem g_rank;
Grenades g_grenades;

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
		MF_ExecuteForward(iFScore, pPlayer->index, pPlayer->lastScore, pPlayer->savedScore);
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

	g_rank.saveRank( MF_BuildPathname("%s",get_localinfo("dodstats") ) );

	// clear custom weapons info
	for ( i=DODMAX_WEAPONS-DODMAX_CUSTOMWPNS;i<DODMAX_WEAPONS;i++)
		weaponData[i].needcheck = false;

	g_map.Init();

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

	// KTP: Check if running in extension mode (without Metamod)
	if (MF_IsExtensionMode && MF_IsExtensionMode())
	{
		g_bExtensionMode = true;
		MF_PrintSrvConsole("[DODX] Running in ReHLDS extension mode.\n");

		// Setup ReHLDS hooks for extension mode
		DODX_SetupExtensionHooks();

		// Skip engine-dependent initialization - will be done in OnPluginsLoaded
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
	iFCurWpnForward = MF_RegisterForward("dod_client_weaponswitch",ET_IGNORE,FP_CELL/*id*/,FP_CELL/*wpnold*/,FP_CELL/*wpnew*/,FP_DONE);
	iFGrenadeExplode = MF_RegisterForward("dod_grenade_explosion",ET_IGNORE,FP_CELL/*id*/,FP_ARRAY/*pos[3]*/,FP_CELL/*wpnid*/,FP_DONE);
	iFRocketExplode = MF_RegisterForward("dod_rocket_explosion",ET_IGNORE,FP_CELL/*id*/,FP_ARRAY/*pos[3]*/,FP_CELL/*wpnid*/,FP_DONE);
	iFObjectTouched = MF_RegisterForward("dod_client_objectpickup",ET_IGNORE,FP_CELL/*id*/,FP_CELL/*object*/,FP_ARRAY/*pos[3]*/,FP_CELL/*value*/,FP_DONE);
	iFStaminaForward = MF_RegisterForward("dod_client_stamina",ET_IGNORE,FP_CELL/*id*/,FP_CELL/*stamina*/,FP_DONE);

	// KTP: In extension mode, do deferred engine-dependent initialization
	// Engine functions aren't ready during OnAmxxAttach in extension mode
	if (g_bExtensionMode)
	{
		// KTP: Skip cvar registration in extension mode - CVAR_REGISTER crashes
		// because module SDK doesn't properly set up engine function pointers
		// for extension mode. The isModuleActive() function handles NULL pointers
		// gracefully (returns true = module always active).
		// Note: dodstats_pause, dodstats_rankbots etc. will remain NULL

		// KTP: Skip rank loading - not needed for HLStatsX logging
		g_map.Init();

		// KTP: Player init disabled - pfnPEntityOfEntIndex causes hang in OnPluginsLoaded
		// Players will be initialized on-demand when messages arrive
		rankBots = dodstats_rankbots ? ((int)dodstats_rankbots->value ? true : false) : false;

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
			}
		}

		// KTP: Register IMessageManager hooks for message interception
		DODX_RegisterMessageHooks();
	}
}

// ============================================================================
// KTP: ReHLDS Extension Mode Hook Implementations
// ============================================================================

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
				pPlayer->aiming = ptr->iHitgroup;
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

// KTP: PlayerPreThink hook handler - replaces FN_PlayerPreThink_Post
static void DODX_OnPlayerPreThink(IVoidHookChain<edict_t *, float> *chain, edict_t *pEntity, float time)
{
	// Call original first
	chain->callNext(pEntity, time);

	// Post-hook logic
	if (!isModuleActive())
		return;

	if (!pEntity || pEntity->free)
		return;

	// Verify this is a valid player
	if (!(pEntity->v.flags & FL_CLIENT))
		return;

	// KTP: Safety check - gpGlobals must be valid
	if (!gpGlobals)
		return;

	// KTP: Extension mode initialization - calculate g_pFirstEdict and set g_bServerActive
	// In metamod mode, ServerActivate_Post handles this. In extension mode, we do it here on first valid player.
	if (g_bExtensionMode && !g_pFirstEdict)
	{
		// Calculate first edict from this player's edict
		// We need to figure out this player's index first using engine function
		int tmpIndex = ENTINDEX(pEntity);
		if (tmpIndex >= 1 && tmpIndex <= gpGlobals->maxClients)
		{
			g_pFirstEdict = pEntity - tmpIndex;
			g_bServerActive = true;

			// Initialize all player slots now that we have g_pFirstEdict
			for (int i = 1; i <= gpGlobals->maxClients; ++i)
				GET_PLAYER_POINTER_I(i)->Init(i, g_pFirstEdict + i);
		}
	}

	// KTP: Skip if server is not active (during map change)
	if (!g_bServerActive)
		return;

	// KTP: g_pFirstEdict must be set before we can do index calculations
	if (!g_pFirstEdict)
		return;

	// KTP: Use ENTINDEX_SAFE which uses pointer arithmetic instead of engine call
	int index = ENTINDEX_SAFE(pEntity);
	if (index < 1 || index > gpGlobals->maxClients)
		return;

	CPlayer *pPlayer = GET_PLAYER_POINTER_I(index);

	// KTP: In extension mode, initialize player on first PreThink call
	// This replaces ClientPutInServer_Post which doesn't fire in extension mode
	if (!pPlayer->ingame && g_bExtensionMode)
	{
		// Initialize player struct if not done yet
		if (!pPlayer->pEdict)
		{
			pPlayer->Init(index, pEntity);
		}

		// Mark as ingame and setup stats
		pPlayer->bot = (pEntity->v.flags & FL_FAKECLIENT) ? true : false;
		pPlayer->PutInServer();
	}
	else if (!pPlayer->ingame)
	{
		return;
	}

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
		MF_ExecuteForward(iFScore, pPlayer->index, pPlayer->lastScore, pPlayer->savedScore);
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

	// Call original to perform the changelevel
	chain->callNext(s1, s2);
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
			}
		}
	}
}

// KTP: Message handler for IMessageManager - replaces all the Write*_Post and MessageBegin/End_Post hooks
static void DODX_OnMessageHandler(IVoidHookChain<IMessage *> *chain, IMessage *msg)
{
	// Safety check
	if (!msg)
	{
		chain->callNext(msg);
		return;
	}

	// KTP: Skip all message processing if server is not active (during map change)
	if (!g_bServerActive)
	{
		chain->callNext(msg);
		return;
	}

	// Get message info
	int msg_type = msg->getId();
	edict_t *ed = msg->getEdict();

	// Validate message type is in range
	if (msg_type < 0 || msg_type >= MAX_REG_MSGS)
	{
		chain->callNext(msg);
		return;
	}

	// Set up player info (like MessageBegin_Post)
	// KTP: Extra safety - validate edict and gpGlobals before using ENTINDEX_SAFE
	if (ed && !ed->free && g_pFirstEdict && gpGlobals)
	{
		int idx = ENTINDEX_SAFE(ed);

		// Validate player index range
		if (idx < 1 || idx > gpGlobals->maxClients)
		{
			mPlayerIndex = 0;
			mPlayer = NULL;
		}
		else
		{
			mPlayerIndex = idx;
			mPlayer = GET_PLAYER_POINTER_I(mPlayerIndex);
		}
	}
	else
	{
		mPlayerIndex = 0;
		mPlayer = NULL;
	}

	mDest = static_cast<int>(msg->getDest());
	mState = 0;

	// Get the callbacks for this message type
	function = modMsgs[msg_type];
	endfunction = modMsgsEnd[msg_type];

	// Skip processing if no callbacks registered for this message
	if (!function && !endfunction)
	{
		chain->callNext(msg);
		return;
	}

	// KTP: Skip processing for players that aren't initialized yet.
	// In extension mode, players are initialized via SV_PlayerRunPreThink hook.
	// In Metamod mode, they're initialized via ClientPutInServer_Post.
	if (mPlayer && !mPlayer->ingame)
	{
		// Player not fully initialized yet, skip message processing
		chain->callNext(msg);
		return;
	}

	// Process message parameters (like the Write*_Post hooks)
	if (function)
	{
		int paramCount = msg->getParamCount();

		for (int i = 0; i < paramCount; i++)
		{
			IMessage::ParamType type = msg->getParamType(i);
			switch(type)
			{
				case IMessage::ParamType::Byte:
				case IMessage::ParamType::Char:
				case IMessage::ParamType::Short:
				case IMessage::ParamType::Long:
				case IMessage::ParamType::Entity:
				{
					int iValue = msg->getParamInt(i);
					(*function)((void *)&iValue);
					break;
				}
				case IMessage::ParamType::Angle:
				case IMessage::ParamType::Coord:
				{
					float flValue = msg->getParamFloat(i);
					(*function)((void *)&flValue);
					break;
				}
				case IMessage::ParamType::String:
				{
					const char *sz = msg->getParamString(i);
					(*function)((void *)sz);
					break;
				}
			}
		}
	}

	// Call end function (like MessageEnd_Post)
	if (endfunction)
	{
		(*endfunction)(NULL);
	}

	// Continue the chain
	chain->callNext(msg);
}

// KTP: Detect info_doddetect entity after map load (workaround for DispatchKeyValue_Post)
void DODX_DetectMapInfo()
{
	// Search for info_doddetect entity and read its keyvalues
	edict_t *pEnt = nullptr;
	while ((pEnt = FindEntityByClassname(pEnt, "info_doddetect")) != nullptr)
	{
		g_map.pEdict = pEnt;
		g_map.initialized = true;

		// Read the keyvalues from the entity - these should be set by the engine
		// Unfortunately, we can't easily read arbitrary keyvalues after spawn,
		// but for DOD the map info is typically static per map.
		// The entity keyvalues are stored in the BSP and loaded at spawn time.

		// For now, we'll use a fallback approach: check the map name
		// and use known defaults, or allow server admins to configure via cvars

		break;
	}
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

	// IMessageManager hooks enabled - players will be initialized on-demand
	// when messages arrive (in DODX_OnMessageHandler)

	return true;
}

// KTP: Begin handler - called once at the start of each message to set up DODX's mPlayer/mState
static void DODX_OnMsgBegin(int msg_id, int dest, int player_index, edict_t* ed)
{
	// KTP: Skip message processing if server is not active (during map change)
	if (!g_bServerActive)
		return;

	// KTP: Use the player_index passed from the core, which has already been validated
	// This is more reliable than recalculating from edict
	if (player_index >= 1 && player_index <= gpGlobals->maxClients)
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
	}

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
