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

#ifndef DODX_H
#define DODX_H

#include "amxxmodule.h"
#include "CMisc.h"
#include "CRank.h"
#include <IGameConfigs.h>

// KTP: First edict pointer for safe entity index calculation
// This avoids calling engine functions in extension mode hooks
extern edict_t* g_pFirstEdict;

// KTP: whether the tier-2.7 pack recorder hook is registered (moduleconfig.cpp).
// Down means the sampler answers unknown; the read native reports it so a
// consumer can tell "nothing suspicious" from "nothing recorded".
extern bool g_ktpPackRecorderLive;

// KTP: False across a map change. DODX's message-begin handler bails on it
// WITHOUT resetting mState, and the core still runs the per-field handlers, so
// a handler that trusts mState must check this itself.
extern bool g_bServerActive;

// KTP: Safe ENTINDEX that uses pointer arithmetic instead of engine function
// This is safe to call during ReHLDS hooks when pfnIndexOfEdict may crash
inline int ENTINDEX_SAFE(const edict_t *pEdict)
{
	if (!pEdict || !g_pFirstEdict)
		return 0;
	return static_cast<int>(pEdict - g_pFirstEdict);
}

// KTP: Use ENTINDEX_SAFE for GET_PLAYER_POINTER to avoid crashes in extension mode
#define GET_PLAYER_POINTER(e)   (&players[ENTINDEX_SAFE(e)])
#define GET_PLAYER_POINTER_I(i) (&players[i])

#ifndef GETPLAYERAUTHID
	#define GETPLAYERAUTHID		(*g_engfuncs.pfnGetPlayerAuthId)
#endif

extern AMX_NATIVE_INFO stats_Natives[];
extern AMX_NATIVE_INFO base_Natives[];
extern AMX_NATIVE_INFO pd_Natives[];
extern AMX_NATIVE_INFO cp_Natives[];

// Weapons grabbing by type
enum
{
	DODWT_PRIMARY = 0,
	DODWT_SECONDARY,
	DODWT_MELEE,
	DODWT_GRENADE, 
	DODWT_OTHER
};

// Model Sequences
enum
{
	DOD_SEQ_PRONE_IDLE = 15,
	DOD_SEQ_PRONE_FORWARD,
	DOD_SEQ_PRONE_DOWN,
	DOD_SEQ_PRONE_UP
};

// KTP: Player private data offsets for scoreboard team name
// Same offsets as dodfun module, but implemented here for extension mode compatibility.
//
// HISTORY:
//   2026-05-11: SCORE/DEATHS originally hardcoded +5, crashed on Ubuntu 24.04
//               (struct shifted by one int between Ubuntu builds).
//   2026-05-11: First fix attempt — share `g_iLinuxPdataOffsetAdjust` with
//               grenade offsets. Loaded without crash but read wrong values.
//   2026-05-21: 2nd fail mode root-caused via disassembly. Grenade offsets and
//               SCORE/DEATHS offsets need DIFFERENT runtime adjustments on the
//               same OS — grenades = +5 on 24.04 (auto-detected, correct),
//               SCORE/DEATHS = +4 on 24.04 (this commit). The 5/21 test
//               failed because the shared global got promoted to +5 by the
//               grenade auto-detect, mis-aligning SCORE/DEATHS by one int.
//
// Confirmed 24.04 layout (from disassembling production dod_i386.so md5
// 4f4727b2..., research in KTPMatchHandler/research/OFFSETS_RESEARCH_2026-05-21.md):
//   m_iObjScore at byte 0x780 = int-offset 480 (= base 476 + 4)
//   m_iDeaths   at byte 0x784 = int-offset 481 (= base 477 + 4)
//
// TEAMNAME stays at (1400 + 5) char-array — different struct region, didn't
// shift in our testing.
#if defined(__linux__) || defined(__APPLE__)
	#define STEAM_PDOFFSET_TEAMNAME (1400 + 5)  // Linux offset adjustment (char array)
	// SCORE/DEATHS use g_iScoreDeathsOffsetAdjust (NOT the grenade adjust).
	// Default 4 (Ubuntu 24.04+, fleet-validated). No runtime auto-detect —
	// the previous auto-detect (which was designed for grenade offsets) gave
	// false-positive promotion to +5 on the score/deaths range. Declared
	// below alongside the grenade adjust.
	#define STEAM_PDOFFSET_SCORE    (476 + g_iScoreDeathsOffsetAdjust)
	#define STEAM_PDOFFSET_DEATHS   (477 + g_iScoreDeathsOffsetAdjust)
#else
	#define STEAM_PDOFFSET_TEAMNAME 1400        // Windows offset
	#define STEAM_PDOFFSET_SCORE    476         // Player score
	#define STEAM_PDOFFSET_DEATHS   477         // Player deaths
#endif

// KTP: `CBasePlayer::m_rgAmmo` as an int-offset into pvPrivateData. Measured in
// dod_i386.so md5 4f4727b2390d3a0ed6f5ad862dd6d4be: AmmoInventory indexes it at
// byte 0x474 (int 285, Linux = 280 + 5); SendAmmoUpdate pairs it with
// m_rgAmmoLast at 0x4F4. Re-measure against that md5 before changing either.
//
// Never write m_rgAmmoLast — SendAmmoUpdate diffs the pair every frame and emits
// the client's AmmoX from it, so writing both suppresses the HUD update.
#define PDOFFSET_AMMO_BASE 280
#define DODX_MAX_AMMO_SLOTS 32

#if defined(__linux__) || defined(__APPLE__)
	// Override via dodx.ini `pdata_offset`, for a future DoD build that shifts it.
	extern int g_iLinuxPdataOffsetAdjust;

	// Independent adjust for SCORE/DEATHS offsets (above). NOT shared with
	// the ammo adjust — they sit in different struct regions and can shift
	// by different amounts between Ubuntu builds. Fixed at +4 for the 24.04
	// baremetal fleet; override via dodx.ini `score_deaths_offset = N` if a
	// future Ubuntu bump shifts the layout.
	extern int g_iScoreDeathsOffsetAdjust;

	#define PDOFFSET_AMMO_ARRAY (PDOFFSET_AMMO_BASE + g_iLinuxPdataOffsetAdjust)
#else
	#define PDOFFSET_AMMO_ARRAY PDOFFSET_AMMO_BASE
#endif

// Ammo-type index the game DLL assigned to each DODW_* weapon on THIS map, or
// -1 while unobserved. Learned from the WeaponList message, whose first field is
// GetAmmoIndex(pszAmmo1) and whose seventh is the weapon id.
extern int g_ammoIndexByWeapon[DODMAX_WEAPONS];
// Bumped by every clear, so a per-map log latch has something to compare against.
extern int g_ammoRegistryEpoch;
void DODX_ClearAmmoRegistry();
// Observed slot, else DoD's fixed-precache-order default. -1 only for a type
// that is not a grenade.
int DODX_GrenadeAmmoIndex(int grenadeType);
// Second source: the slot the DLL itself credited when a grenade pickup landed.
void DODX_ObserveGrenadeAmmoIndex(int grenadeType, int slot);
// Logs once per map if an observed slot contradicts the fixed-order default.
void DODX_CheckAmmoIndexDrift(int weaponId, int slot);

// Weapons Structure
struct weapon_t 
{
	bool needcheck;
	bool melee;
	char logname[16];
	char name[32];
	int ammoSlot;
	int type;
};

struct weaponlist_s
{
	int grp;
	int bitfield;
	int clip;
	bool changeable;
};

// =============== KTP: Control Point Tracking (ported from dodfun) ===============

// DoD Control Point private data layout
struct pd_dcp {
	int iunk_0;
#if defined(_WIN32)
	int iunk_1; // windows only
#endif
	int iunk_2;	// pointer edict_t*
	int iunk_3;

	float origin_x;
	float origin_y;
	float origin_z; // 6

	float mins_x;
	float mins_y;
	float mins_z;

	float maxs_x;
	float maxs_y;
	float maxs_z;

	float angles_x;
	float angles_y;
	float angles_z; // 15

	int unknown_block1[19];
	int iunk_35; // pointer entvars_t*
	int iunk_36; // pointer entvars_t*
	int unknown_block2[52];
	int iunk_89; // pointer entvars_t*
// KTP: the Linux/Apple CControlPoint layout carries FIVE extra ints here, not
// four. With four, every field from `owner` down was read 4 bytes early, so each
// one returned its NEIGHBOUR:
//
//   field          struct (was)  gamedata  actually returned
//   owner                   372       376  the int below it — 0 for every CP
//   default_owner           384       388  —
//   flag_id                 388       392  m_iDefaultOwner  (0/1/2, not an index)
//   pointvalue              392       396  m_iIndex         (0..N-1, not a value)
//   points_for_player       396       400  —
//   points_for_team         400       404  —
//
// Offsets from gamedata/common.games/entities.games/dod/offsets-ccontrolpoint.txt
// (m_iTeam 376, m_iDefaultOwner 388, m_iIndex 392, m_iPointValue 396,
// m_iCapPoints 400, m_iTeamPoints 404). Windows was already byte-perfect, which
// is why this only ever showed up in production. One added int aligns all six.
//
// Derived twice, independently: from the published offsets above, and
// empirically from recorded prod matches, where `flag_id` was predicted exactly
// by each BSP's point_default_owner sequence on two maps
// (DoD-hud-observer docs/dodx-cp-index-space-findings.md).
#if defined (__linux__) || defined (__APPLE__)
	int iunk_extra1;
	int iunk_extra2;
	int iunk_extra3;
	int iunk_extra4;
	int iunk_extra5;
#endif
	int owner; // 90
	int iunk_91;
	int iunk_92;
	int default_owner; // 93
	int flag_id;
	int pointvalue;
	int points_for_player;
	int points_for_team;
	float funk_98; // always 1.0
	float cap_time;
	char cap_message[256]; // 100
	int iunk_164;
	int iunk_165;
	char target_allies[256]; //  166
	char target_axis[256]; // 230
	char target_reset[256];
	char model_allies[256]; // 358
	char model_axis[256]; // 422
	char model_neutral[256]; // 486
	int model_body_allies; // 550
	int model_body_axis;
	int model_body_neutral;
	int icon_allies;
	int icon_axis;
	int icon_neutral;
	int can_touch; // flags : 1-allies can't, 256-axis can't , default 0 (all can)
	int iunk_557;
	int iunk_558;
	char pointgroup[256];
	int iunk_623;
	int iunk_624;
	int iunk_625;
};

#define GET_CP_PD( x ) (*(pd_dcp*)x->pvPrivateData)

// DoD Capture Area private data layout
struct pd_dca {
	int iunk_0;
	int iunk_1;
	int iunk_2;
#if defined(_WIN32)
	int iunk_3; // if def windows
#endif

	float origin_x;
	float origin_y;
	float origin_z; // 6

	float mins_x;
	float mins_y;
	float mins_z;

	float maxs_x;
	float maxs_y;
	float maxs_z;

	float angles_x;
	float angles_y;
	float angles_z; // 15

	// KTP: block before cap_mode sized so cap_mode lines up with m_iCapMode per gamedata
	// (offsets-careacapture.txt). Windows m_iCapMode=492, Linux m_iCapMode=512.
	// Windows pre-block header = 64 bytes -> (492-64)/4 = 107 ints.
	// Linux pre-block header   = 60 bytes -> (512-60)/4 = 113 ints.
#if defined(_WIN32)
	int unknown_block_16[107];
#else
	int unknown_block_16[113];
#endif

	// Live CAreaCapture state (from gamedata offsets-careacapture.txt).
	// Reading these is the game's own source of truth — no AABB/radius math.
	int cap_mode;           // m_iCapMode
	int is_capturing;       // m_bCapturing (non-zero while a team is progressing a cap)
	int capturing_team;     // m_nCapturingTeam (1=allies, 2=axis, 0=none)
	int owning_team;        // m_nOwningTeam

	int cap_time;           // m_nCapTime (total seconds to cap; was named time_to_cap)
	float time_remaining;   // m_fTimeRemaining (seconds left on active cap; was iunk_128)
	int allies_numcap;      // m_nAlliesNumCap (required count to cap for allies)
	int axis_numcap;        // m_nAxisNumCap

	int num_allies;         // m_nNumAllies (live count of allies in zone; was iunk_131)
	int num_axis;           // m_nNumAxis (live count of axis in zone; was iunk_132)

	int can_cap;            // m_bAlliesCanCap combined w/ axis (flags: 1=allies, 256=axis)

	int iunk_134;
	int iunk_135;

	char allies_endcap[256]; // 136
	char axis_endcap[256]; // 200
	char allies_startcap[256]; // 264
	char axis_startcap[256]; // 328
	char allies_breakcap[256]; // 392
	char axis_breakcap[256]; // 456
	int iunk_520;
	char hud_sprite[256]; // 521

	int unknown_block_585[65];

	char object_group[256]; // 650
	int iunk_714;
	int iunk_715;
	int iunk_716;
};

#define GET_CA_PD( x ) (*(pd_dca*)x->pvPrivateData)

// Control point info struct
typedef struct objinfo_s {
	// initobj
	edict_t* pEdict;
	int index;
	int default_owner;
	int visible;
	int icon_neutral;
	int icon_allies;
	int icon_axis;
	float origin_x;
	float origin_y;
	// setobj
	int owner;
	// control area
	int areaflags; // 0-need check , 1-no area , 2-found area
	edict_t* pAreaEdict;
} objinfo_t;

// Control point manager
class CObjective {
public:
	int count;
	objinfo_t obj[12];
	inline void Clear() { count = 0; memset(obj, 0, sizeof(obj)); }
	void SetKeyValue(int index, char *keyname, char *value);
	void InitObj(int dest = MSG_ALL, edict_t* ed = NULL);
	void SetObj(int index);
	void UpdateOwner(int index, int team);
	void Sort();
};

// Control point data keys
enum CP_VALUE {
	CP_edict = 1,
	CP_area,
	CP_index,
	CP_owner,
	CP_default_owner,
	CP_visible,
	CP_icon_neutral,
	CP_icon_allies,
	CP_icon_axis,
	CP_origin_x,
	CP_origin_y,

	CP_can_touch,
	CP_pointvalue,

	CP_points_for_cap,
	CP_team_points,

	CP_model_body_neutral,
	CP_model_body_allies,
	CP_model_body_axis,

	// strings
	CP_name,
	CP_cap_message,
	CP_reset_capsound,
	CP_allies_capsound,
	CP_axis_capsound,
	CP_targetname,

	CP_model_neutral,
	CP_model_allies,
	CP_model_axis,
};

// Capture area data keys
enum CA_VALUE {
	CA_edict = 1,
	CA_allies_numcap,
	CA_axis_numcap,
	CA_timetocap,
	CA_can_cap,

	// Live cap state (read-only). See offsets-careacapture.txt.
	CA_num_allies,       // m_nNumAllies: players from allies team currently in zone
	CA_num_axis,         // m_nNumAxis: players from axis team currently in zone
	CA_is_capturing,     // m_bCapturing: non-zero while a cap is in progress
	CA_capturing_team,   // m_nCapturingTeam: 1=allies, 2=axis, 0=none
	CA_owning_team,      // m_nOwningTeam: 1=allies, 2=axis, 0=neutral
	CA_cap_mode,         // m_iCapMode
	CA_time_remaining,   // m_fTimeRemaining (float, read via Float:, cast with dodx_area_get_data)

	// strings
	CA_target,
	CA_sprite,
};

// Macro to lazily find capture area entity for a control point
#define GET_CAPTURE_AREA(x) \
	if ( mObjects.obj[x].areaflags == 0 ){\
		mObjects.obj[x].areaflags = 1;\
		while ( (mObjects.obj[x].pAreaEdict = FindEntityByString(mObjects.obj[x].pAreaEdict,"target",STRING(mObjects.obj[x].pEdict->v.targetname))) )\
			if ( strcmp( STRING(mObjects.obj[x].pAreaEdict->v.classname),"dod_capture_area" )==0){\
				mObjects.obj[x].areaflags = 2;\
				break;\
			}\
	}\
	if ( mObjects.obj[x].areaflags == 1 )\
		return 0;

extern bool rankBots;
extern int mState;
extern int mDest;
extern int mCurWpnEnd;
extern int mPlayerIndex;

void Client_CurWeapon(void*);
void Client_CurWeapon_End(void*);
void Client_Health_End(void*);
void Client_ResetHUD_End(void*);
void Client_ObjScore(void*);
void Client_TeamScore(void*);
void Client_RoundState(void*);
void Client_AmmoX(void*);
void Client_AmmoShort(void*);
void Client_SetFOV(void*);
void Client_SetFOV_End(void*);
void Client_Object(void*);
void Client_Object_End(void*);
void Client_PStatus(void*);
void Client_InitObj(void*);
void Client_SetObj(void*);
void Client_DeathMsg(void*);  // KTP: Suicide / world-kill detection
void Client_WeaponList(void*);  // KTP: per-map ammo-index registry

typedef void (*funEventCall)(void*);

extern int AlliesScore;
extern int AxisScore;

extern int gmsgCurWeapon;
extern int gmsgCurWeaponEnd;
extern int gmsgHealth;
extern int gmsgResetHUD;
extern int gmsgObjScore;
extern int gmsgRoundState;
extern int gmsgTeamScore;
extern int gmsgScoreShort;
extern int gmsgPTeam;
extern int gmsgAmmoX;
extern int gmsgAmmoShort;
extern int gmsgSetFOV;
extern int gmsgSetFOV_End;
extern int gmsgObject;
extern int gmsgObject_End;
extern int gmsgPStatus;
extern int gmsgTeamInfo;  // KTP: For scoreboard team name refresh
extern int gmsgInitObj;   // KTP: CP tracking
extern int gmsgSetObj;    // KTP: CP tracking
extern int gmsgDeathMsg;  // KTP: Suicide / world-kill detection (no Damage path)
extern int gmsgWeaponList;  // KTP: per-map ammo-index registry

extern int iFDamage;
extern int iFDeath;
extern int iFScore;
extern int iFSpawnForward;
extern int iFTeamForward;
extern int iFClassForward;
extern int iFScopeForward;
extern int iFProneForward;
extern int iFWpnPickupForward;
extern int iFCurWpnForward;
extern int iFWeaponFire;   // KTP: Per-shot primary-attack actuation forward
extern int iFGrenadeExplode;
extern int iFRocketExplode;
extern int iFObjectTouched;
extern int iFStaminaForward;
extern int iFFlushStats;  // KTP: Forward for stats flush notification
extern int iFDamagePre;   // KTP: Forward for damage modification (fires before client_damage)
extern int iFInitCP;      // KTP: Forward for CP init
extern int iFCPCaptured;  // KTP: Forward for CP ownership change
extern bool g_cpOrderingFinalized;  // KTP: Has InitObj reordered mObjects to match DLL?
extern int iFScoreEvent;  // KTP: Forward for enriched score event with CP context

// KTP: Last CP capture tracking (for ObjScore correlation)
extern int g_lastCapturedCP;     // CP index from most recent SetObj (-1 = none)
extern float g_lastCapturedTime; // gpGlobals->time when last SetObj fired

// KTP: Match ID for HLStatsX integration
extern char g_szMatchId[64];
const char* DODX_GetMatchId();

extern cvar_t* dodstats_maxsize;
extern cvar_t* dodstats_rank;
extern cvar_t* dodstats_reset;
extern cvar_t* dodstats_rankbots;
extern cvar_t* dodstats_pause;

// KTP: Plugin-controlled stats pause (for round-freeze filtering)
extern bool g_bStatsPaused;

extern weapon_t weaponData[DODMAX_WEAPONS];
extern traceVault traceData[MAX_TRACE];

extern Grenades g_grenades;
extern RankSystem g_rank;
extern CPlayer players[33];
extern CPlayer* mPlayer;
extern CMapInfo g_map;
extern CObjective mObjects;

int get_weaponid(CPlayer* player);
bool ignoreBots (edict_t *pEnt, edict_t *pOther = NULL );
bool isModuleActive();
edict_t *FindEntityByString(edict_t *pentStart, const char *szKeyword, const char *szValue);
edict_t *FindEntityByClassname(edict_t *pentStart, const char *szName);
edict_t *FindEntityInSphere(edict_t *pentStart, edict_t *origin, float radius);

#define CHECK_ENTITY(x) \
	if (x < 0 || x > gpGlobals->maxEntities) { \
		MF_LogError(amx, AMX_ERR_NATIVE, "Entity out of range (%d)", x); \
		return 0; \
	} else { \
		if (x <= gpGlobals->maxClients) { \
			if (!MF_IsPlayerIngame(x)) { \
				MF_LogError(amx, AMX_ERR_NATIVE, "Invalid player %d (not in-game)", x); \
				return 0; \
			} \
		} else { \
			if (x != 0 && FNullEnt(INDEXENT(x))) { \
				MF_LogError(amx, AMX_ERR_NATIVE, "Invalid entity %d", x); \
				return 0; \
			} \
		} \
	}

// KTP: Use players[] array directly to avoid AMXX function calls that may crash in extension mode
#define CHECK_PLAYER(x) \
	if (x < 1 || x > gpGlobals->maxClients) { \
		MF_LogError(amx, AMX_ERR_NATIVE, "Player out of range (%d)", x); \
		return 0; \
	} else { \
		CPlayer* _pCheck = &players[x]; \
		if (!_pCheck->ingame || !_pCheck->pEdict || _pCheck->pEdict->free || FNullEnt(_pCheck->pEdict)) { \
			return 0; \
		} \
	}

#define CHECK_PLAYERRANGE(x) \
	if (x > gpGlobals->maxClients || x < 1) \
	{ \
		MF_LogError(amx, AMX_ERR_NATIVE, "Player out of range (%d)", x); \
		return 0; \
	}

#define CHECK_NONPLAYER(x) \
	if (x < 1 || x <= gpGlobals->maxClients || x > gpGlobals->maxEntities) { \
		MF_LogError(amx, AMX_ERR_NATIVE, "Non-player entity %d out of range", x); \
		return 0; \
	} else { \
		if (FNullEnt(INDEXENT(x))) { \
			MF_LogError(amx, AMX_ERR_NATIVE, "Invalid non-player entity %d", x); \
			return 0; \
		} \
	}

#define GETEDICT(n) \
	((n >= 1 && n <= gpGlobals->maxClients) ? MF_GetPlayerEdict(n) : INDEXENT(n))

// KTP: Gamerules access for scoreboard score modification
// Loaded from common.games gamedata - signature scan finds g_pGameRules
extern IGameConfig *g_pCommonConfig;
extern IGameConfig *g_pGamerulesConfig;
extern void **g_pGameRulesAddress;  // Pointer to g_pGameRules pointer
extern int g_iTeamScoreOffset;       // Offset of m_iTeamScores in CDoDTeamPlay (56)

// KTP: DoD round-timer field offsets (see moduleconfig.cpp; -1 = unresolved)
extern int g_iGrRoundTimeOffset;     // CDoDTeamPlay::m_flRoundTime
extern int g_iParaTimerPtrOffset;    // CDoDTeamPlay::m_pParaTimer
extern int g_iParaRoundTimerOffset;  // CDodParaRoundTimer::m_fRoundTimer
extern int g_iParaBTimerOffset;      // CDodParaRoundTimer::m_bTimer
extern int g_iRTimerRoundTimeOffset; // CDodRoundTimer::m_fRoundTime
extern int g_iRTimerLengthOffset;    // CDodRoundTimer::m_fTimerLength
extern int g_iRTimerBTimerOffset;    // CDodRoundTimer::m_bTimer

// KTP: DoD half-clock members for dodx_get_round_time (see moduleconfig.cpp)
extern int g_iDoDMapTimeOffset;       // CDoDTeamPlay::m_flDoDMapTime
extern int g_iRestartRoundTimeOffset; // CDoDTeamPlay::m_flRestartRoundTime
extern int g_iRoundRestartingOffset;  // CDoDTeamPlay::m_bRoundRestarting
extern cvar_t *g_pcvarMpTimelimit;    // cached mp_timelimit

// KTP: CControlPointMaster members for dodx_get_score_tick_time. Unlike the
// CDoDTeamPlay members above, these offsets DIFFER BY PLATFORM — always resolve
// through gamedata (see moduleconfig.cpp).
extern int g_iCPMGivePointsTimeOffset;   // CControlPointMaster::m_fGivePointsTime
extern int g_iCPMGivePointsDelayOffset;  // CControlPointMaster::m_iGivePointsDelay
extern int g_iCPMActiveOffset;           // CControlPointMaster::m_bActive
extern edict_t *g_pCPMasterEdict;        // cached per map, cleared on map change

// Check if gamerules is available for score modification
inline bool DODX_HasGameRules()
{
	return (g_pGameRulesAddress && *g_pGameRulesAddress);
}

#endif // DODX_H
