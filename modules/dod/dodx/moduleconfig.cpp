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
	int i;
	for( i = 1;i<=gpGlobals->maxClients; ++i)
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
			if (!dodstats_rank->value)
				pPlayer->rank = g_rank.findEntryInRank( name, name );
			else
				pPlayer->rank->setName( name );
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
	if(ed)
	{
		mPlayerIndex = ENTINDEX(ed);
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

	// KTP: Check if running in extension mode (without Metamod)
	if (MF_IsExtensionMode && MF_IsExtensionMode())
	{
		g_bExtensionMode = true;
		MF_PrintSrvConsole("[DODX] Running in ReHLDS extension mode.\n");
		// KTP: Extension mode hooks temporarily disabled pending further debugging
		// Basic DODX natives will still work, but advanced message interception may be limited
		MF_PrintSrvConsole("[DODX] Note: Extension mode hooks are currently disabled.\n");
#if 0
		// Setup ReHLDS hooks for extension mode
		if (DODX_SetupExtensionHooks())
		{
			MF_PrintSrvConsole("[DODX] ReHLDS hooks registered successfully.\n");
		}
		else
		{
			MF_PrintSrvConsole("[DODX] Warning: Some ReHLDS hooks could not be registered.\n");
		}
#endif
	}
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

	// KTP: In extension mode, try to detect info_doddetect entities after map load
	// and register message hooks now that message IDs are known
	if (g_bExtensionMode)
	{
		DODX_DetectMapInfo();
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
	// Call the original first
	chain->callNext(v1, v2, fNoMonsters, e, ptr);

	// Now do the post-hook logic (same as TraceLine_Post)
	if(ptr->pHit && (ptr->pHit->v.flags & (FL_CLIENT | FL_FAKECLIENT)) && e && (e->v.flags & (FL_CLIENT | FL_FAKECLIENT)))
	{
		GET_PLAYER_POINTER(e)->aiming = ptr->iHitgroup;
		return;
	}

	if(e && e->v.owner && e->v.owner->v.flags & (FL_CLIENT | FL_FAKECLIENT))
	{
		CPlayer *pPlayer = GET_PLAYER_POINTER(e->v.owner);

		for(int i = 0; i < MAX_TRACE; i++)
		{
			if(strcmp(traceData[i].szName, STRING(e->v.classname)) == 0)
			{
				int grenId = (traceData[i].iId == 13 && g_map.detect_allies_country) ? 36 : traceData[i].iId;
				int rocketId = traceData[i].iId;

				if(traceData[i].iAction & ACT_NADE_SHOT)
				{
					if(traceData[i].iId == 13 && g_map.detect_allies_country)
						pPlayer->saveShot(grenId);
					else
						pPlayer->saveShot(traceData[i].iId);
				}
				else if(traceData[i].iAction & ACT_ROCKET_SHOT)
					pPlayer->saveShot(traceData[i].iId);

				cell position[3];
				position[0] = amx_ftoc(v2[0]);
				position[1] = amx_ftoc(v2[1]);
				position[2] = amx_ftoc(v2[2]);
				cell pos = MF_PrepareCellArray(position, 3);

				if(traceData[i].iAction & ACT_NADE_PUT)
				{
					g_grenades.put(e, traceData[i].fDel, grenId, GET_PLAYER_POINTER(e->v.owner));
					MF_ExecuteForward(iFGrenadeExplode, GET_PLAYER_POINTER(e->v.owner)->index, pos, grenId);
				}

				if(traceData[i].iAction & ACT_ROCKET_PUT)
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

// KTP: Message handler for IMessageManager - replaces all the Write*_Post and MessageBegin/End_Post hooks
static void DODX_OnMessageHandler(IVoidHookChain<IMessage *> *chain, IMessage *msg)
{
	// Get message info
	int msg_type = msg->getId();
	edict_t *ed = msg->getEdict();

	// Set up player info (like MessageBegin_Post)
	if(ed)
	{
		mPlayerIndex = ENTINDEX(ed);
		mPlayer = GET_PLAYER_POINTER_I(mPlayerIndex);
	}
	else
	{
		mPlayerIndex = 0;
		mPlayer = NULL;
	}

	mDest = static_cast<int>(msg->getDest());
	mState = 0;

	// Get the callbacks for this message type
	if (msg_type >= 0 && msg_type < MAX_REG_MSGS)
	{
		function = modMsgs[msg_type];
		endfunction = modMsgsEnd[msg_type];
	}
	else
	{
		function = nullptr;
		endfunction = nullptr;
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

		MF_PrintSrvConsole("[DODX] Found info_doddetect entity.\n");
		break;
	}

	if (!g_map.initialized)
	{
		MF_PrintSrvConsole("[DODX] No info_doddetect entity found. Using default map settings.\n");
	}
}

// KTP: Setup extension mode hooks
static bool DODX_SetupExtensionHooks()
{
	if (!MF_IsExtensionMode || !MF_IsExtensionMode())
		return false;

	// Get ReHLDS hookchains
	if (MF_GetRehldsHookchains)
	{
		g_pRehldsHookchains = (IRehldsHookchains*)MF_GetRehldsHookchains();
	}

	// Get message manager
	if (MF_GetRehldsMessageManager)
	{
		g_pMessageManager = (IMessageManager*)MF_GetRehldsMessageManager();
	}

	if (!g_pRehldsHookchains)
	{
		MF_PrintSrvConsole("[DODX] Warning: Could not get ReHLDS hookchains.\n");
		return false;
	}

	// Register TraceLine hook
	if (g_pRehldsHookchains->PF_TraceLine())
	{
		g_pRehldsHookchains->PF_TraceLine()->registerHook(DODX_OnTraceLine, HC_PRIORITY_DEFAULT);
		MF_PrintSrvConsole("[DODX] Registered PF_TraceLine hook.\n");
	}

	// Register SetClientKeyValue hook
	if (g_pRehldsHookchains->PF_SetClientKeyValue())
	{
		g_pRehldsHookchains->PF_SetClientKeyValue()->registerHook(DODX_OnSetClientKeyValue, HC_PRIORITY_DEFAULT);
		MF_PrintSrvConsole("[DODX] Registered PF_SetClientKeyValue hook.\n");
	}

	// Register RegUserMsg hook
	if (g_pRehldsHookchains->PF_RegUserMsg_I())
	{
		g_pRehldsHookchains->PF_RegUserMsg_I()->registerHook(DODX_OnRegUserMsg, HC_PRIORITY_DEFAULT);
		MF_PrintSrvConsole("[DODX] Registered PF_RegUserMsg_I hook.\n");
	}

	// Register message hooks via IMessageManager
	if (g_pMessageManager)
	{
		// We need to register hooks for each message type we care about
		// This will be done in OnPluginsLoaded after message IDs are known
		MF_PrintSrvConsole("[DODX] IMessageManager available for message interception.\n");
	}

	return true;
}

// KTP: Register message hooks after message IDs are known
void DODX_RegisterMessageHooks()
{
	if (!g_bExtensionMode || !g_pMessageManager)
		return;

	// Register hooks for all message types we're tracking
	for (int i = 0; g_user_msg[i].name; ++i)
	{
		if (*g_user_msg[i].id > 0)
		{
			g_pMessageManager->registerHook(*g_user_msg[i].id, DODX_OnMessageHandler, HC_PRIORITY_DEFAULT);
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
		// Unregister TraceLine hook
		if (g_pRehldsHookchains->PF_TraceLine())
		{
			g_pRehldsHookchains->PF_TraceLine()->unregisterHook(DODX_OnTraceLine);
		}

		// Unregister SetClientKeyValue hook
		if (g_pRehldsHookchains->PF_SetClientKeyValue())
		{
			g_pRehldsHookchains->PF_SetClientKeyValue()->unregisterHook(DODX_OnSetClientKeyValue);
		}

		// Unregister RegUserMsg hook
		if (g_pRehldsHookchains->PF_RegUserMsg_I())
		{
			g_pRehldsHookchains->PF_RegUserMsg_I()->unregisterHook(DODX_OnRegUserMsg);
		}
	}

	if (g_pMessageManager)
	{
		// Unregister message hooks
		for (int i = 0; g_user_msg[i].name; ++i)
		{
			if (*g_user_msg[i].id > 0)
			{
				g_pMessageManager->unregisterHook(*g_user_msg[i].id, DODX_OnMessageHandler);
			}
		}
	}

	g_pRehldsHookchains = nullptr;
	g_pMessageManager = nullptr;
}
