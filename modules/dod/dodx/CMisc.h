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

#ifndef CMISC_H
#define CMISC_H

#include "CRank.h"
#include "KTPAimAccum.h"
#include "KTPShotGeom.h"
#include "KTPPackVis.h"

#define DODMAX_CUSTOMWPNS	5	// custom weapons
#define DODMAX_WEAPONS		(42 + DODMAX_CUSTOMWPNS)

#if defined(_WIN32)
#define LINUXOFFSET	0
#else
#define LINUXOFFSET	5
#endif

// KTP: Private data offsets for player class/team manipulation
#define STEAM_PDOFFSET_CLASS	367 + LINUXOFFSET  // player class
#define STEAM_PDOFFSET_RCLASS	368 + LINUXOFFSET  // random class flag

#define DOD_VERSION "0.1"

#define MAX_TRACE	6

struct traceVault
{
	char szName[16];
	int iId;
	int iAction;
	float fDel;
};

#define ACT_NADE_NONE		(0)
#define ACT_NADE_SHOT		(1<<0)
#define ACT_NADE_PUT		(1<<1)
#define ACT_NADE_THROW		(1<<2)

#define ACT_ROCKET_NONE		(0)
#define ACT_ROCKET_SHOT		(1<<0)
#define ACT_ROCKET_PUT		(1<<3)


// *****************************************************
// class CPlayer
// *****************************************************

class CPlayer
{
	private:
		char ip[32];

	public:
		// KTP: shadow-mode aim/movement counters, fed once per usercmd from
		// SV_PlayerRunPreThink. Measure-only -- nothing here reaches a verdict.
		KTPAimStats ktpAim;

		// KTP: per-shot aim geometry stash, written by the TraceLine capture and
		// read once by dodx_get_shot_geom. Reset lives in moduleconfig.cpp (the
		// extension-mode connect/disconnect/map paths), not in Init()/Disconnect(),
		// because capture and read exist only in extension mode.
		KTPShotGeom ktpShot;

		// KTP: aim-vs-transmission counters, sampled beside the shot capture.
		// Same lifecycle rule as ktpShot: resets live in moduleconfig.cpp.
		KTPPackVis ktpVis;

		edict_t* pEdict;
		int index;
		int aiming;
		int current;
		int old;
		int wpnModel;
		int wpnscount;
		int wpns_bitfield;
		int old_weapons[DODMAX_WEAPONS];

		float savedScore;
		int lastScore;
		float sendScore;
		int lastScoreCP;  // KTP: CP index that triggered this score (-1 = no CP correlation)

		bool ingame;
		bool bot;
		float clearStats;
		float clearRound;

		int oldteam;
		int oldclass;
		float oldstamina;

		struct ModelStruct
		{
			int body_num;
			bool is_model_set;
			char modelclass[64];
		}
		sModel;

		int oldprone;
		bool do_scoped;
		bool is_scoped;

		struct ObjectStruct
		{
			edict_t* pEdict;
			bool carrying;
			bool do_forward;
			int type;
		}
		object;

		struct PlayerWeapon : public Stats 
		{
			char*		name;
			int			ammo;
			int			clip;
		};

		PlayerWeapon	weapons[DODMAX_WEAPONS];
		PlayerWeapon	attackers[33];
		PlayerWeapon	victims[33];
		Stats			weaponsLife[DODMAX_WEAPONS]; // DEC-Weapon (Life) stats
		Stats			weaponsRnd[DODMAX_WEAPONS]; // DEC-Weapon (Round) stats
		Stats			life;
		Stats			round;

		RankSystem::RankStats*	rank;

		void Init(  int pi, edict_t* pe );
		void Connect(const char* name,const char* ip );
		void PutInServer();
		void Disconnect();
		void saveKill(CPlayer* pVictim, int weapon, int hs, int tk);
		void saveHit(CPlayer* pVictim, int weapon, int damage, int aiming);
		void saveShot(int weapon);
		void updateScore(int weapon, int score);
		void restartStats(bool all = true);
		void killPlayer();
		void initModel(char*);
		void clearModel();
		bool setModel();
		void setBody(int);
		void PreThink();
		void Scoping(int);
		void ScopingCheck();
		void WeaponsCheck(int);

		inline bool IsBot()
		{
			// KTP: Check pEdict is valid before accessing
			if (!pEdict || pEdict->free)
				return false;
			const char* auth= (*g_engfuncs.pfnGetPlayerAuthId)(pEdict);
			return ( auth && !strcmp( auth , "BOT" ) );
		}

		inline bool IsAlive()
		{
			// KTP: Check pEdict is valid before accessing
			if (!pEdict || pEdict->free)
				return false;
			return ((pEdict->v.deadflag==DEAD_NO)&&(pEdict->v.health>0));
		}
};

// *****************************************************
// class Grenades
// *****************************************************
// KTP: Fixed-size pool replaces linked list — no per-grenade allocation, O(n) scan with small n
class Grenades
{
  static const int MAX_GRENADES = 32;
  struct Obj
  {
    CPlayer* player;
    edict_t* grenade;
    float time;
    int type;
    bool active;
  };
  Obj pool[MAX_GRENADES];
  int count;

public:
	Grenades() : count(0) { clear(); }
	~Grenades() {}
	void put(edict_t* grenade, float time, int type, CPlayer* player);
	bool find(edict_t* enemy, CPlayer** p, int& type);
	void clear();
};

// *****************************************************
// class CMapInfo
// *****************************************************

class CMapInfo
{
public:
	edict_t* pEdict;
	bool initialized;

	int detect_axis_paras;
	int detect_allies_paras;
	int detect_allies_country;

	void Init();
};



#endif // CMISC_H

