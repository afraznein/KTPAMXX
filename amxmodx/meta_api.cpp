// vim: set ts=4 sw=4 tw=99 noet:
//
// AMX Mod X, based on AMX Mod by Aleksander Naszko ("OLO").
// Copyright (C) The AMX Mod X Development Team.
//
// This software is licensed under the GNU General Public License, version 3 or higher.
// Additional exceptions apply. For full license details, see LICENSE.txt or visit:
//     https://alliedmods.net/amxmodx-license

#include <time.h>
#include "amxmodx.h"
#include "fakemeta.h"
#include "CMenu.h"
#include "newmenus.h"
#include "natives.h"
#include "binlog.h"
#include "optimizer.h"
#include "libraries.h"
#include "messages.h"

#include "datastructs.h"
#include "CFlagManager.h"
#include <amxmodx_version.h>
#include "trie_natives.h"
#include "CDataPack.h"
#include "textparse.h"
#include "CvarManager.h"
#include "CLibrarySys.h"
#include "CFileSystem.h"
#include "gameconfigs.h"
#include "CGameConfigs.h"
#include <engine_strucs.h>
#include <CDetour/detours.h>
#include "CoreConfig.h"
#include <resdk/mod_rehlds_api.h>
#include <amtl/am-utility.h>
#ifdef __linux__
#include <arpa/inet.h>
#endif

plugin_info_t Plugin_info =
{
	META_INTERFACE_VERSION,		// ifvers
	"KTP AMX",					// name
	AMXX_VERSION,			// version
	__DATE__,					// date
	"KTP / AMX Mod X Dev Team",	// author
	"https://github.com/afraznein",	// url
	"KTPAMX",					// logtag
	PT_STARTUP,					// (when) loadable
	PT_ANYTIME,					// (when) unloadable
};

// KTP: Dummy meta_globals for extension mode - RETURN_META macros need this
static meta_globals_t g_DummyMetaGlobals;
meta_globals_t *gpMetaGlobals = &g_DummyMetaGlobals;  // Default to dummy, Metamod will override
gamedll_funcs_t *gpGamedllFuncs;
mutil_funcs_t *gpMetaUtilFuncs;
enginefuncs_t g_engfuncs;
globalvars_t *gpGlobals;

funEventCall modMsgsEnd[MAX_REG_MSGS];
funEventCall modMsgs[MAX_REG_MSGS];

void (*function)(void*);
void (*endfunction)(void*);

extern List<AUTHORIZEFUNC> g_auth_funcs;
extern ke::Vector<CAdminData *> DynamicAdmins;

CLog g_log;
CForwardMngr g_forwards;
ke::Vector<ke::AutoPtr<CPlayer *>> g_auth;
// KTP: Pending client_putinserver forwards — bitmask of player indices waiting to spawn
// Bit N corresponds to player index N+1. Zero cost when no players joining.
// Extension-mode only: set by SV_Spawn_f_RH, drained by SV_Frame_RH. Metamod
// fires putinserver directly from C_ClientPutInServer_Post and never sets this.
static uint32_t g_putinserver_mask = 0;
ke::Vector<ke::AutoPtr<ForceObject>> g_forcemodels;
ke::Vector<ke::AutoPtr<ForceObject>> g_forcesounds;
ke::Vector<ke::AutoPtr<ForceObject>> g_forcegeneric;
CPlayer g_players[33];
CPlayer* mPlayer;
CPluginMngr g_plugins;
CTaskMngr g_tasksMngr;
CFrameActionMngr g_frameActionMngr;
CmdMngr g_commands;
CFlagManager FlagMan;
EventsMngr g_events;
Grenades g_grenades;
LogEventsMngr g_logevents;
MenuMngr g_menucmds;
CLangMngr g_langMngr;
ke::AString g_log_dir;
ke::AString g_mod_name;
XVars g_xvars;

bool g_bmod_tfc;
bool g_bmod_cstrike;
bool g_bmod_dod;
bool g_bmod_dmc;
bool g_bmod_ricochet;
bool g_bmod_valve;
bool g_bmod_gearbox;
bool g_official_mod;
bool g_dontprecache;
bool g_forcedmodules;
bool g_forcedsounds;

fakecmd_t g_fakecmd;

float g_game_restarting;
float g_game_timeleft;
float g_task_time;
float g_auth_time;

bool g_initialized = false;
bool g_coloredmenus;
bool g_activated = false;
bool g_NewDLL_Available = false;
bool g_bRunningWithMetamod = false;  // KTP: Track if Metamod is present
bool g_bRehldsExtensionInit = false; // KTP: Track if initialized as ReHLDS extension
DLL_FUNCTIONS *g_pGameEntityInterface = nullptr;  // KTP: Game DLL functions - works in both Metamod and extension mode

#ifdef MEMORY_TEST
	float g_next_memreport_time;
	unsigned int g_memreport_count;
	ke::AString g_memreport_dir;
	bool g_memreport_enabled;
	#define MEMREPORT_INTERVAL 300.0f	/* 5 mins */
#endif // MEMORY_TEST

hudtextparms_t g_hudset;
//int g_edict_point;
int g_players_num;
int mPlayerIndex;
int mState;
int g_srvindex;

CDetour *DropClientDetour;
bool g_isDropClientHookEnabled = false;
bool g_isDropClientHookAvailable = false;
void SV_DropClient_RH(IRehldsHook_SV_DropClient *chain, IGameClient *cl, bool crash, const char *format);

// KTP: SV_Spawn_f hook for client_putinserver forward in extension mode
void SV_Spawn_f_RH(IRehldsHook_SV_Spawn_f *chain);

// KTP: SV_Frame hook for per-frame processing in extension mode
void SV_Frame_RH(IRehldsHook_SV_Frame *chain);

// KTP: Track map changes in extension mode - prevents processing during transitions
static bool g_bMapChangeInProgress = false;

// Set once teardown starts. KTP_ExtensionShutdown latches g_bMapChangeInProgress
// on purpose to suppress plugin code while the engine tears itself down, so the
// stuck-flag watchdog below must be able to tell that deliberate latch apart
// from a changelevel that never completed. KTP_ExtensionShutdown's own
// s_extShutdownDone is function-local and cannot be seen from SV_Frame_RH.
static bool g_bExtShuttingDown = false;

// AX-02: during a NAT slot seizure the engine has ALREADY swapped the incoming
// player's userinfo and Steam id into the slot (sv_main.cpp:2669-2676, upstream
// of the ClientConnected hook), so get_user_authid() reports the NEWCOMER to a
// handler running the OLD session's disconnect forwards -- filing a leaver's
// stats under someone else's SteamID.
//
// Scoped to one index for the duration of those forwards, deliberately NOT a
// general preference for the cache: preferring it everywhere would return a
// stale id whenever the engine's is fresher (late Steam auth, re-auth), trading
// a rare edge case for a permanent regression on every consumer.
int g_authReplayIndex = -1;
const char *g_authReplayAuthid = NULL;

// RAII: a Pawn handler that errors out of executeForwards must not be able to
// leave the marker latched on a slot.
struct KTPAuthReplayGuard
{
	KTPAuthReplayGuard(int index, const char *authid)
	{ g_authReplayIndex = index; g_authReplayAuthid = authid; }
	~KTPAuthReplayGuard()
	{ g_authReplayIndex = -1; g_authReplayAuthid = NULL; }
};

// KTP: Track if precache hooks have processed force_unmodified lists this map
static bool g_bExtPrecacheProcessed = false;

// KTP: Track if AMXX init was called during precache phase
// When true, plugin_init/plugin_cfg are deferred to SV_ActivateServer
static bool g_bInitDuringPrecache = false;

// KTP: PF_precache_model_I hook - processes force_unmodified during precache phase
static int PF_precache_model_I_RH(IRehldsHook_PF_precache_model_I *chain, const char *s);

// KTP: PF_changelevel_I hook - sets map change flag BEFORE any processing happens
static void PF_changelevel_I_RH(IRehldsHook_PF_changelevel_I *chain, const char *s1, const char *s2);

// KTP: Extension mode hooks for all required forwards
void ClientConnected_RH(IRehldsHook_ClientConnected *chain, IGameClient *cl);
qboolean Steam_NotifyClientConnect_RH(IRehldsHook_Steam_NotifyClientConnect *chain, IGameClient *cl, const void *pvSteam2Key, unsigned int ucbSteam2Key);
// KTP: SV_ClientUserInfoChanged hook for client_infochanged + CPlayer::name refresh in extension mode.
// The Metamod-mode path (C_ClientUserInfoChanged_Post via gFunctionTable_Post) never fires here,
// so without this hook get_user_name() stays at the connect-time name through every respawn
// after a setinfo "name" "..." mid-life.
void SV_ClientUserInfoChanged_RH(IRehldsHook_SV_ClientUserInfoChanged *chain, IGameClient *cl);
bool SV_CheckConsistencyResponse_RH(IRehldsHook_SV_CheckConsistencyResponse *chain, IGameClient *cl, resource_t *resource, uint32 hash);

// KTP: IMessageManager hook for register_event in extension mode
void MessageHook_Handler(IVoidHookChain<IMessage *> *chain, IMessage *msg);
bool g_MessageHooksInstalled[MAX_REG_MSGS];  // Track which message IDs have hooks
void InstallMessageHook(int msg_id);

// KTP: Forward declarations for module message handler functions (from modules.cpp)
void CallModuleMsgBeginHandlers(int msg_id, int dest, int player_index, edict_t* ed);
void CallModuleMsgHandlers(int msg_id, void* mValue);
void CallModuleMsgEndHandlers(int msg_id);

cvar_t init_amxmodx_version = {"amxmodx_version", "", FCVAR_SERVER | FCVAR_SPONLY};
cvar_t init_amxmodx_modules = {"amxmodx_modules", "", FCVAR_SPONLY};
cvar_t init_amxmodx_debug = {"amx_debug", "1", FCVAR_SPONLY};
cvar_t init_amxmodx_mldebug = {"amx_mldebug", "", FCVAR_SPONLY};
cvar_t init_amxmodx_language = {"amx_language", "en", FCVAR_SERVER};
cvar_t init_amxmodx_cl_langs = {"amx_client_languages", "1", FCVAR_SERVER};
cvar_t init_amxmodx_perflog = { "amx_perflog_ms", "1.0", FCVAR_SPONLY };

cvar_t* amxmodx_version = NULL;
cvar_t* amxmodx_modules = NULL;
cvar_t* amxmodx_debug = NULL;
cvar_t* amxmodx_language = NULL;
cvar_t* amxmodx_perflog = NULL;

cvar_t* hostname = NULL;
cvar_t* mp_timelimit = NULL;

// main forwards
int FF_ClientCommand = -1;
int FF_ClientConnect = -1;
int FF_ClientDisconnect = -1;
int FF_ClientDisconnected = -1;
int FF_ClientRemove = -1;
int FF_ClientInfoChanged = -1;
int FF_ClientCvarChanged = -1;  // KTP Custom: Real-time cvar change notification
int FF_ClientPutInServer = -1;
int FF_PluginInit = -1;
int FF_PluginCfg = -1;
int FF_PluginPrecache = -1;
int FF_PluginLog = -1;
int FF_PluginEnd = -1;
int FF_InconsistentFile = -1;
int FF_ClientAuthorized = -1;
int FF_ChangeLevel = -1;
int FF_ClientConnectEx = -1;

IFileSystem* g_FileSystem;
HLTypeConversion TypeConversion;

bool ColoredMenus(const char *ModName)
{
	const char * pModNames[] = { "cstrike", "czero", "dmc", "dod", "tfc", "valve" };
	const size_t ModsCount = sizeof(pModNames) / sizeof(const char *);

	for (size_t i = 0; i < ModsCount; ++i)
	{
		if (strcmp(ModName, pModNames[i]) == 0)
			return true; // this game modification currently supports colored menus
	}

	return false; // no colored menus are supported for this game modification
}

void ParseAndOrAdd(CStack<ke::AString *> & files, const char *name)
{
	if (strncmp(name, "plugins-", 8) == 0)
	{
#if !defined WIN32
		size_t len = strlen(name);
		if (strcmp(&name[len-4], ".ini") == 0)
		{
#endif
			ke::AString *pString = new ke::AString(name);
			files.push(pString);
#if !defined WIN32
		}
#endif
	}
}

void BuildPluginFileList(const char *initialdir, CStack<ke::AString *> & files)
{
	char path[PLATFORM_MAX_PATH];
#if defined WIN32
	build_pathname_r(path, sizeof(path), "%s/*.ini", initialdir);
	_finddata_t fd;
	intptr_t handle = _findfirst(path, &fd);

	if (handle < 0)
	{
		return;
	}

	while (!_findnext(handle, &fd))
	{
		ParseAndOrAdd(files, fd.name);
	}

	_findclose(handle);
#elif defined(__linux__) || defined(__APPLE__)
	build_pathname_r(path, sizeof(path), "%s/", initialdir);
	struct dirent *ep;
	DIR *dp;

	if ((dp = opendir(path)) == NULL)
	{
		return;
	}

	while ( (ep=readdir(dp)) != NULL )
	{
		ParseAndOrAdd(files, ep->d_name);
	}

	closedir (dp);
#endif
}

//Loads a plugin list into the Plugin Cache and Load Modules cache
void LoadExtraPluginsToPCALM(const char *initialdir)
{
	CStack<ke::AString *> files;
	BuildPluginFileList(initialdir, files);
	char path[255];
	while (!files.empty())
	{
		ke::AString *pString = files.front();
		ke::SafeSprintf(path, sizeof(path), "%s/%s",
			initialdir,
			pString->chars());
		g_plugins.CALMFromFile(path);
		delete pString;
		files.pop();
	}
}

void LoadExtraPluginsFromDir(const char *initialdir)
{
	CStack<ke::AString *> files;
	char path[255];
	BuildPluginFileList(initialdir, files);
	while (!files.empty())
	{
		ke::AString *pString = files.front();
		ke::SafeSprintf(path, sizeof(path), "%s/%s",
			initialdir,
			pString->chars());
		g_plugins.loadPluginsFromFile(path);
		delete pString;
		files.pop();
	}
}

// Precache	stuff from force consistency calls
// or check	for	pointed	files won't	be done
int	C_PrecacheModel(const char *s)
{
	if (!g_forcedmodules)
	{
		g_forcedmodules	= true;
		for (auto &model : g_forcemodels)
		{
			PRECACHE_MODEL(model->getFilename());
			ENGINE_FORCE_UNMODIFIED(model->getForceType(), model->getMin(), model->getMax(), model->getFilename());
		}
	}

	RETURN_META_VALUE(MRES_IGNORED, 0);
}

int	C_PrecacheSound(const char *s)
{
	if (!g_forcedsounds)
	{
		g_forcedsounds = true;
		for (auto &sound : g_forcesounds)
		{
			PRECACHE_SOUND(sound->getFilename());
			ENGINE_FORCE_UNMODIFIED(sound->getForceType(), sound->getMin(), sound->getMax(), sound->getFilename());
		}

		if (!g_bmod_cstrike)
		{
			PRECACHE_SOUND("weapons/cbar_hitbod1.wav");
			PRECACHE_SOUND("weapons/cbar_hitbod2.wav");
			PRECACHE_SOUND("weapons/cbar_hitbod3.wav");
		}
	}

	RETURN_META_VALUE(MRES_IGNORED, 0);
}

// On InconsistentFile call	forward	function from plugins
int	C_InconsistentFile(const edict_t *player, const char *filename, char *disconnect_message)
{
	if (FF_InconsistentFile < 0)
		RETURN_META_VALUE(MRES_IGNORED,	FALSE);

	if (MDLL_InconsistentFile(player, filename, disconnect_message))
	{
		CPlayer	*pPlayer = GET_PLAYER_POINTER((edict_t *)player);

		if (executeForwards(FF_InconsistentFile, static_cast<cell>(pPlayer->index),
			filename, disconnect_message) == 1)
			RETURN_META_VALUE(MRES_SUPERCEDE, FALSE);

		RETURN_META_VALUE(MRES_SUPERCEDE, TRUE);
	}

	RETURN_META_VALUE(MRES_IGNORED, FALSE);
}

const char*	get_localinfo(const char* name, const char* def)
{
	const char* b = LOCALINFO((char*)name);

	if (b == 0 || *b == 0)
	{
		SET_LOCALINFO((char*)name, (char*)(b = def));
	}

	return b;
}

const char*	get_localinfo_r(const char *name, const char *def, char buffer[], size_t maxlength)
{
	const char* b = LOCALINFO((char*)name);

	if (b == 0 || *b == 0)
	{
		SET_LOCALINFO((char*)name, (char*)(b = def));
	}

	ke::SafeSprintf(buffer, maxlength, "%s", b);

	return buffer;
}

// Very	first point	at map load
// Load	AMX	modules	for	new	native functions
// Initialize AMX stuff	and	load it's plugins from plugins.ini list
// Call	precache forward function from plugins
int	C_Spawn(edict_t *pent)
{
	if (g_initialized)
	{
		RETURN_META_VALUE(MRES_IGNORED, 0);
	}

	g_activated = false;
	g_initialized = true;
	g_forcedmodules = false;
	g_forcedsounds = false;

	g_srvindex = IS_DEDICATED_SERVER() ? 0 : 1;

	hostname = CVAR_GET_POINTER("hostname");
	mp_timelimit = CVAR_GET_POINTER("mp_timelimit");

	// Fix for crashing on mods that do not have mp_timelimit
	if (mp_timelimit == NULL)
	{
		static cvar_t timelimit_holder;

		timelimit_holder.name = "mp_timelimit";
		timelimit_holder.string = "0";
		timelimit_holder.flags = 0;
		timelimit_holder.value = 0.0;

		CVAR_REGISTER(&timelimit_holder);

		mp_timelimit = &timelimit_holder;

	}

	g_frameActionMngr.clear();
	g_forwards.clear();

	g_log.MapChange();

	// ###### Initialize task manager
	g_tasksMngr.registerTimers(&gpGlobals->time, &mp_timelimit->value, &g_game_timeleft);

	// ###### Initialize commands prefixes
	// KTP: Register longer prefixes before shorter ones — findPrefix uses strncmp
	// with prefix length, so "say" (len 3) would match "say_team" if checked first.
	// Longest-prefix-first ensures "say_team" gets its own list (~80 fewer entries to
	// scan when dispatching "say" commands).
	g_commands.registerPrefix("say_team");
	g_commands.registerPrefix("say");
	g_commands.registerPrefix("amxx");
	g_commands.registerPrefix("amx");
	g_commands.registerPrefix("admin_");
	g_commands.registerPrefix("sm_");
	g_commands.registerPrefix("cm_");

	// make sure localinfos are set
	get_localinfo("amxx_basedir", "addons/ktpamx");
	get_localinfo("amxx_pluginsdir", "addons/ktpamx/plugins");
	get_localinfo("amxx_modulesdir", "addons/ktpamx/modules");
	get_localinfo("amxx_configsdir", "addons/ktpamx/configs");
	get_localinfo("amxx_customdir", "addons/ktpamx/custom");

	// make sure bcompat localinfos are set
	get_localinfo("amx_basedir", "addons/ktpamx");
	get_localinfo("amx_configdir", "addons/ktpamx/configs");
	get_localinfo("amx_langdir", "addons/ktpamx/data/amxmod-lang");
	get_localinfo("amx_modulesdir", "addons/ktpamx/modules");
	get_localinfo("amx_pluginsdir", "addons/ktpamx/plugins");
	get_localinfo("amx_logdir", "addons/ktpamx/logs");

	FlagMan.LoadFile();

	ArrayHandles.clear();
	TrieHandles.clear();
	TrieIterHandles.clear();
	TrieSnapshotHandles.clear();
	DataPackHandles.clear();
	TextParsersHandles.clear();
	GameConfigHandle.clear();

	char map_pluginsfile_path[256];
	char prefixed_map_pluginsfile[256];
	char configs_dir[256];

	// ###### Load modules
	loadModules(get_localinfo("amxx_modules", "addons/ktpamx/configs/modules.ini"), PT_ANYTIME);

	get_localinfo_r("amxx_configsdir", "addons/ktpamx/configs", configs_dir, sizeof(configs_dir)-1);
	g_plugins.CALMFromFile(get_localinfo("amxx_plugins", "addons/ktpamx/configs/plugins.ini"));
	LoadExtraPluginsToPCALM(configs_dir);
	char temporaryMap[64], *tmap_ptr;

	ke::SafeSprintf(temporaryMap, sizeof(temporaryMap), "%s", STRING(gpGlobals->mapname));

	prefixed_map_pluginsfile[0] = '\0';
	if ((tmap_ptr = strchr(temporaryMap, '_')) != NULL)
	{
		// this map has a prefix

		*tmap_ptr = '\0';
		ke::SafeSprintf(prefixed_map_pluginsfile,
			sizeof(prefixed_map_pluginsfile),
			"%s/maps/plugins-%s.ini",
			configs_dir,
			temporaryMap);
		g_plugins.CALMFromFile(prefixed_map_pluginsfile);
	}

	ke::SafeSprintf(map_pluginsfile_path,
		sizeof(map_pluginsfile_path),
		"%s/maps/plugins-%s.ini",
		configs_dir,
		STRING(gpGlobals->mapname));
	g_plugins.CALMFromFile(map_pluginsfile_path);

	int loaded = countModules(CountModules_Running); // Call after attachModules so all modules don't have pending stat

	// Set some info about amx version and modules
	CVAR_SET_STRING(init_amxmodx_version.name, AMXX_VERSION);
	char buffer[32];
	sprintf(buffer, "%d", loaded);
	CVAR_SET_STRING(init_amxmodx_modules.name, buffer);

	// ###### Load Vault
	char file[PLATFORM_MAX_PATH];
	g_vault.setSource(build_pathname_r(file, sizeof(file), "%s", get_localinfo("amxx_vault", "addons/ktpamx/configs/vault.ini")));
	g_vault.loadVault();

	// ###### Init time and freeze tasks
	g_game_timeleft = g_bmod_dod ? 1.0f : 0.0f;
	g_task_time = gpGlobals->time + 99999.0f;
	g_auth_time = gpGlobals->time + 99999.0f;
#ifdef MEMORY_TEST
	g_next_memreport_time = gpGlobals->time + 99999.0f;
#endif
	g_players_num = 0;

	// Set server flags
	memset(g_players[0].flags, -1, sizeof(g_players[0].flags));

	g_opt_level = atoi(get_localinfo("optimizer", "7"));
	if (!g_opt_level)
		g_opt_level = 7;

	// ###### Load AMX Mod X plugins
	g_plugins.loadPluginsFromFile(get_localinfo("amxx_plugins", "addons/ktpamx/configs/plugins.ini"));
	LoadExtraPluginsFromDir(configs_dir);
	g_plugins.loadPluginsFromFile(map_pluginsfile_path, false);
	if (prefixed_map_pluginsfile[0] != '\0')
	{
		g_plugins.loadPluginsFromFile(prefixed_map_pluginsfile, false);
	}

	g_plugins.Finalize();
	g_plugins.InvalidateCache();

	// Register forwards
	FF_PluginInit = registerForward("plugin_init", ET_IGNORE, FP_DONE);
	FF_ClientCommand = registerForward("client_command", ET_STOP, FP_CELL, FP_DONE);
	FF_ClientConnect = registerForward("client_connect", ET_IGNORE, FP_CELL, FP_DONE);
	FF_ClientDisconnect = registerForward("client_disconnect", ET_IGNORE, FP_CELL, FP_DONE);
	FF_ClientDisconnected = registerForward("client_disconnected", ET_IGNORE, FP_CELL, FP_CELL, FP_ARRAY, FP_CELL, FP_DONE);
	FF_ClientRemove = registerForward("client_remove", ET_IGNORE, FP_CELL, FP_CELL, FP_STRING, FP_DONE);
	FF_ClientInfoChanged = registerForward("client_infochanged", ET_IGNORE, FP_CELL, FP_DONE);
	FF_ClientCvarChanged = registerForward("client_cvar_changed", ET_IGNORE, FP_CELL, FP_STRING, FP_STRING, FP_DONE);  // KTP Custom
	FF_ClientPutInServer = registerForward("client_putinserver", ET_IGNORE, FP_CELL, FP_DONE);
	FF_PluginCfg = registerForward("plugin_cfg", ET_IGNORE, FP_DONE);
	FF_PluginPrecache = registerForward("plugin_precache", ET_IGNORE, FP_DONE);
	FF_PluginLog = registerForward("plugin_log", ET_STOP, FP_DONE);
	FF_PluginEnd = registerForward("plugin_end", ET_IGNORE, FP_DONE);
	FF_InconsistentFile = registerForward("inconsistent_file", ET_STOP, FP_CELL, FP_STRING, FP_STRINGEX, FP_DONE);
	FF_ClientAuthorized = registerForward("client_authorized", ET_IGNORE, FP_CELL, FP_STRING, FP_DONE);
	FF_ChangeLevel = registerForward("server_changelevel", ET_STOP, FP_STRING, FP_DONE);
	FF_ClientConnectEx = registerForward("client_connectex", ET_STOP, FP_CELL, FP_STRING, FP_STRING, FP_ARRAY, FP_DONE);

	CoreCfg.OnAmxxInitialized();

#if defined BINLOG_ENABLED
	if (!g_BinLog.Open())
	{
		LOG_ERROR(PLID, "Binary log failed to open.");
	}
	g_binlog_level = atoi(get_localinfo("bin_logging", "17"));
	g_binlog_maxsize = atoi(get_localinfo("max_binlog_size", "20"));
#endif

	modules_callPluginsLoaded();

	TypeConversion.init();

	// ###### Call precache forward function
	g_dontprecache = false;
	executeForwards(FF_PluginPrecache);
	g_dontprecache = true;

	for (auto &generic : g_forcegeneric)
	{
		PRECACHE_GENERIC(generic->getFilename());
		ENGINE_FORCE_UNMODIFIED(generic->getForceType(), generic->getMin(), generic->getMax(), generic->getFilename());
	}

	RETURN_META_VALUE(MRES_IGNORED, 0);
}

struct sUserMsg
{
	const char* name;
	int* id;
	funEventCall func;
	bool endmsg;
	bool cstrike;
} g_user_msg[] =
{
	{"CurWeapon", &gmsgCurWeapon, Client_CurWeapon, false, false},
	{"Damage", &gmsgDamage, Client_DamageEnd, true, true},
	{"DeathMsg", &gmsgDeathMsg,	Client_DeathMsg, false, true},
	{"TextMsg", &gmsgTextMsg, Client_TextMsg, false, false},
	{"TeamInfo", &gmsgTeamInfo, Client_TeamInfo, false, false},
	{"WeaponList", &gmsgWeaponList, Client_WeaponList, false, false},
	{"MOTD", &gmsgMOTD,	0, false, false},
	{"ServerName", &gmsgServerName,	0, false, false},
	{"Health", &gmsgHealth,	0, false, false},
	{"Battery", &gmsgBattery, 0, false, false},
	{"ShowMenu", &gmsgShowMenu, Client_ShowMenu, false, false},
	{"SendAudio", &gmsgSendAudio, 0, false, false},
	{"AmmoX", &gmsgAmmoX, Client_AmmoX, false, false},
	{"ScoreInfo", &gmsgScoreInfo, Client_ScoreInfo,	false, false},
	{"VGUIMenu", &gmsgVGUIMenu, Client_VGUIMenu, false, false},
	{"AmmoPickup", &gmsgAmmoPickup,	Client_AmmoPickup, false, false},
	{"WeapPickup", &gmsgWeapPickup, 0, false, false},
	{"ResetHUD", &gmsgResetHUD, 0, false, false},
	{"RoundTime", &gmsgRoundTime, 0, false, false},
	{"SayText", &gmsgSayText, 0, false, false},
	{"InitHUD", &gmsgInitHUD, Client_InitHUDEnd, true, false},
	{0, 0, 0, false, false}
};

int	C_RegUserMsg_Post(const char *pszName, int iSize)
{
	for (int i = 0; g_user_msg[i].name;	++i)
	{
		if (strcmp(g_user_msg[i].name, pszName) == 0)
		{
			int id = META_RESULT_ORIG_RET(int);
			*g_user_msg[i].id =	id;

			if (!g_user_msg[i].cstrike || g_bmod_cstrike)
			{
				if (g_user_msg[i].endmsg)
					modMsgsEnd[id] = g_user_msg[i].func;
				else
					modMsgs[id] = g_user_msg[i].func;
			}
			break;
		}
	 }

	RETURN_META_VALUE(MRES_IGNORED, 0);
}

/*
Much more later	after precache.	All	is precached, server
will be	flaged as ready	to use so call
plugin_init	forward	function from plugins
*/
void C_ServerActivate(edict_t *pEdictList, int edictCount, int clientMax)
{
	// KTP: Only use GET_USER_MSG_ID in Metamod mode - it requires gpMetaUtilFuncs
	// In extension mode, message IDs are looked up via REG_USER_MSG in SV_ActivateServer_RH
	if (g_bRunningWithMetamod)
	{
		int id;

		for (int i = 0; g_user_msg[i].name;	++i)
		{
			if ((*g_user_msg[i].id == 0) && (id = GET_USER_MSG_ID(PLID, g_user_msg[i].name, NULL)) != 0)
			{
				*g_user_msg[i].id =	id;

				if (!g_user_msg[i].cstrike || g_bmod_cstrike)
				{
					if (g_user_msg[i].endmsg)
						modMsgsEnd[id] = g_user_msg[i].func;
					else
						modMsgs[id] = g_user_msg[i].func;
				}
			}
		}
	}

	if (g_isDropClientHookAvailable)
	{
		if (!g_isDropClientHookEnabled)
		{
			if (RehldsApi)
			{
				RehldsHookchains->SV_DropClient()->registerHook(SV_DropClient_RH);
			}
			else
			{
				DropClientDetour->EnableDetour();
			}
			g_isDropClientHookEnabled = true;
		}
	}

	RETURN_META(MRES_IGNORED);
}

void C_ServerActivate_Post(edict_t *pEdictList, int edictCount, int clientMax)
{
	if (g_activated)
		RETURN_META(MRES_IGNORED);

	for (int i = 1; i <= gpGlobals->maxClients; ++i)
	{
		CPlayer	*pPlayer = GET_PLAYER_POINTER_I(i);
		pPlayer->Init(pEdictList + i, i);
	}

	CoreCfg.ExecuteMainConfig();    // Execute amxx.cfg

	executeForwards(FF_PluginInit);
	executeForwards(FF_PluginCfg);

	CoreCfg.ExecuteAutoConfigs();   // Execute configs created with AutoExecConfig native.
	CoreCfg.SetMapConfigTimer(6.1); // Prepare per-map configs to be executed 6.1 seconds later.
	                                // Original value which was used in admin.sma.

	// Correct time in Counter-Strike and other mods (except DOD)
	if (!g_bmod_dod)
		g_game_timeleft = 0;

	g_task_time = gpGlobals->time;
	g_auth_time = gpGlobals->time;

#ifdef MEMORY_TEST
	g_next_memreport_time = gpGlobals->time + MEMREPORT_INTERVAL;
	g_memreport_count = 0;
	g_memreport_enabled = true;
#endif

	g_activated = true;

	RETURN_META(MRES_IGNORED);
}

// Call	plugin_end forward function	from plugins.
void C_ServerDeactivate()
{
	if (!g_activated)
		RETURN_META(MRES_IGNORED);

	for (int i = 1; i <= gpGlobals->maxClients; ++i)
	{
		CPlayer	*pPlayer = GET_PLAYER_POINTER_I(i);

		if (pPlayer->initialized)
		{
			// deprecated
			executeForwards(FF_ClientDisconnect, static_cast<cell>(pPlayer->index));

			if (g_isDropClientHookAvailable && !pPlayer->disconnecting)
			{
				executeForwards(FF_ClientDisconnected, static_cast<cell>(pPlayer->index), FALSE, prepareCharArray(const_cast<char*>(""), 0), 0);
			}
		}

		if (pPlayer->ingame)
		{
			auto wasDisconnecting = pPlayer->disconnecting;

			pPlayer->Disconnect();
			--g_players_num;

			if (!wasDisconnecting && g_isDropClientHookAvailable)
			{
				executeForwards(FF_ClientRemove, static_cast<cell>(pPlayer->index), FALSE, const_cast<char*>(""));
			}
		}
	}

	if (g_isDropClientHookAvailable)
	{
		if (g_isDropClientHookEnabled)
		{
			if (RehldsApi)
			{
				RehldsHookchains->SV_DropClient()->unregisterHook(SV_DropClient_RH);
			}
			else
			{
				DropClientDetour->DisableDetour();
			}
			g_isDropClientHookEnabled = false;
		}
	}

	g_players_num	= 0;
	executeForwards(FF_PluginEnd);

	RETURN_META(MRES_IGNORED);
}

extern ke::Vector<cell *> g_hudsync;

// After all clear whole AMX configuration
// However leave AMX modules which are loaded only once
void C_ServerDeactivate_Post()
{
	if (!g_initialized)
		RETURN_META(MRES_IGNORED);

	modules_callPluginsUnloading();

	CoreCfg.Clear();

	g_auth.clear();
	g_putinserver_mask = 0;  // KTP: Clear pending putinserver bitmask
	g_commands.clear();
	g_forcemodels.clear();
	g_forcesounds.clear();
	g_forcegeneric.clear();
	g_bExtPrecacheProcessed = false;  // KTP: Reset for next map
	g_bInitDuringPrecache = false;  // KTP: Reset for next map
	g_grenades.clear();
	g_tasksMngr.clear();
	g_frameActionMngr.clear();
	g_forwards.clear();
	g_logevents.clearLogEvents();
	g_events.clearEvents();
	g_menucmds.clear();
	ClearMenus();
	g_vault.clear();
	g_xvars.clear();
	g_plugins.clear();
	g_langMngr.Clear();

	ArrayHandles.clear();
	TrieHandles.clear();
	TrieIterHandles.clear();
	TrieSnapshotHandles.clear();
	DataPackHandles.clear();
	TextParsersHandles.clear();
	GameConfigHandle.clear();

	g_CvarManager.OnPluginUnloaded();

	ClearPluginLibraries();
	modules_callPluginsUnloaded();

	detachReloadModules();

	ClearMessages();

	// Flush the dynamic admins list
	for (size_t iter=DynamicAdmins.length();iter--; )
	{
		delete DynamicAdmins[iter];
	}

	DynamicAdmins.clear();
	for (unsigned int i=0; i<g_hudsync.length(); i++)
		delete [] g_hudsync[i];
	g_hudsync.clear();

	FlagMan.WriteCommands();

	// last memreport
#ifdef MEMORY_TEST
	if (g_memreport_enabled)
	{
		if (g_memreport_count == 0)
		{
			// make new directory
			time_t td;
			time(&td);
			tm *curTime = localtime(&td);
			int i = 0;
#if defined(__linux__) || defined(__APPLE__)
			mkdir(build_pathname("%s/memreports", get_localinfo("amxx_basedir", "addons/ktpamx")), 0700);
#else
			mkdir(build_pathname("%s/memreports", get_localinfo("amxx_basedir", "addons/ktpamx")));
#endif
			while (true)
			{
				char buffer[256];
				sprintf(buffer, "%s/memreports/D%02d%02d%03d", get_localinfo("amxx_basedir", "addons/ktpamx"), curTime->tm_mon + 1, curTime->tm_mday, i);
#if defined(__linux__) || defined(__APPLE__)
				mkdir(build_pathname("%s", g_log_dir.chars()), 0700);
				if (mkdir(build_pathname(buffer), 0700) < 0)
#else
				mkdir(build_pathname("%s", g_log_dir.chars()));
				if (mkdir(build_pathname(buffer)) < 0)
#endif
				{
					if (errno == EEXIST)
					{
						// good
						++i;
						continue;
					} else {
						// bad
						g_memreport_enabled = false;
						AMXXLOG_Log("[AMXX] Fatal error: Can't create directory for memreport files (%s)", buffer);
						break;
					}
				}
				g_memreport_dir = buffer;

				// g_memreport_dir should be valid now
				break;
			}
		}

		m_dumpMemoryReport(build_pathname("%s/r%03d.txt", g_memreport_dir.chars(), g_memreport_count));
		AMXXLOG_Log("Memreport #%d created (file \"%s/r%03d.txt\") (interval %f)", g_memreport_count + 1, g_memreport_dir.chars(), g_memreport_count, MEMREPORT_INTERVAL);

		g_memreport_count++;
	}
#endif // MEMORY_TEST

#if defined BINLOG_ENABLED
	g_BinLog.Close();
#endif

	g_initialized = false;

	RETURN_META(MRES_IGNORED);
}

BOOL C_ClientConnect_Post(edict_t *pEntity, const char *pszName, const char *pszAddress, char szRejectReason[128])
{
	int index = ENTINDEX(pEntity);
	if (index < 1 || index > gpGlobals->maxClients)
		RETURN_META_VALUE(MRES_IGNORED, TRUE);

	CPlayer* pPlayer = GET_PLAYER_POINTER(pEntity);
	if (!pPlayer->IsBot())
	{
		bool a = pPlayer->Connect(pszName, pszAddress);
		executeForwards(FF_ClientConnect, static_cast<cell>(pPlayer->index));

		if (a)
		{
			auto playerToAuth = ke::AutoPtr<CPlayer *>(new CPlayer*(pPlayer));
			if (playerToAuth)
				g_auth.append(ke::Move(playerToAuth));
		} else {
			const char* authid = GETPLAYERAUTHID(pEntity);
			pPlayer->Authorize(authid);
			if (g_auth_funcs.size())
			{
				List<AUTHORIZEFUNC>::iterator iter, end=g_auth_funcs.end();
				AUTHORIZEFUNC fn;
				for (iter=g_auth_funcs.begin(); iter!=end; iter++)
				{
					fn = (*iter);
					fn(pPlayer->index, authid);
				}
			}
			executeForwards(FF_ClientAuthorized, static_cast<cell>(pPlayer->index), authid);
		}
	}

	RETURN_META_VALUE(MRES_IGNORED, TRUE);
}

BOOL C_ClientConnect(edict_t *pEntity, const char *pszName, const char *pszAddress, char szRejectReason[128])
{
	CPlayer* pPlayer = GET_PLAYER_POINTER(pEntity);

	if(executeForwards(FF_ClientConnectEx, static_cast<cell>(pPlayer->index), pszName, pszAddress, prepareCharArray(szRejectReason, 128, true)))
		RETURN_META_VALUE(MRES_SUPERCEDE, FALSE);

	RETURN_META_VALUE(MRES_IGNORED, TRUE);
}

void C_ClientDisconnect(edict_t *pEntity)
{
	CPlayer *pPlayer = GET_PLAYER_POINTER(pEntity);

	if (pPlayer->initialized)
	{
		// deprecated
		executeForwards(FF_ClientDisconnect, static_cast<cell>(pPlayer->index));

		if (g_isDropClientHookAvailable && !pPlayer->disconnecting)
		{
			executeForwards(FF_ClientDisconnected, static_cast<cell>(pPlayer->index), FALSE, prepareCharArray(const_cast<char*>(""), 0), 0);
		}
	}

	if (pPlayer->ingame)
	{
		--g_players_num;
	}

	auto wasDisconnecting = pPlayer->disconnecting;

	pPlayer->Disconnect();

	if (!wasDisconnecting && g_isDropClientHookAvailable)
	{
		executeForwards(FF_ClientRemove, static_cast<cell>(pPlayer->index), FALSE, const_cast<char*>(""));
	}

	RETURN_META(MRES_IGNORED);
}

CPlayer* SV_DropClient_PreHook(edict_s *client, qboolean crash, const char *buffer, size_t buffer_size)
{
	auto pPlayer = client ? GET_PLAYER_POINTER(client) : nullptr;

	if (pPlayer)
	{
		if (pPlayer->initialized)
		{
			pPlayer->disconnecting = true;
			executeForwards(FF_ClientDisconnected, pPlayer->index, TRUE, prepareCharArray(const_cast<char*>(buffer), buffer_size, true), buffer_size - 1);
		}
	}

	return pPlayer;
}

void SV_DropClient_PostHook(CPlayer *pPlayer, qboolean crash, const char *buffer)
{
	if (pPlayer)
	{
		// Metamod decrements here via C_ClientDisconnect, which never runs in
		// extension mode — so get_playersnum() (the no-argument form, which
		// returns the cached counter) over-counted after every mid-map
		// disconnect until the next map change zeroed it. Same `ingame` guard
		// as C_ClientDisconnect, and it must precede Disconnect(): that call
		// clears the flag this test reads.
		if (pPlayer->ingame)
		{
			--g_players_num;
		}

		pPlayer->Disconnect();
		executeForwards(FF_ClientRemove, pPlayer->index, TRUE, buffer);
	}
}

// void SV_DropClient(client_t *cl, qboolean crash, const char *fmt, ...);
DETOUR_DECL_STATIC3_VAR(SV_DropClient, void, client_t*, cl, qboolean, crash, const char*, format)
{
	char buffer[1024];

	va_list ap;
	va_start(ap, format);
	ke::SafeVsprintf(buffer, sizeof(buffer) - 1, format, ap);
	va_end(ap);

	auto pPlayer = SV_DropClient_PreHook(cl->edict, crash, buffer, ARRAY_LENGTH(buffer));

	DETOUR_STATIC_CALL(SV_DropClient)(cl, crash, "%s", buffer);

	SV_DropClient_PostHook(pPlayer, crash, buffer);
}

void SV_DropClient_RH(IRehldsHook_SV_DropClient *chain, IGameClient *cl, bool crash, const char *format)
{
	char buffer[1024];
	ke::SafeStrcpy(buffer, sizeof(buffer), format);

	auto pPlayer = SV_DropClient_PreHook(cl->GetEdict(), crash, buffer, ARRAY_LENGTH(buffer));

	chain->callNext(cl, crash, buffer);

	SV_DropClient_PostHook(pPlayer, crash, buffer);
}

// KTP: SV_Spawn_f hook for client_putinserver forward in extension mode
// This is called when a client sends the "spawn" command after fully connecting
void SV_Spawn_f_RH(IRehldsHook_SV_Spawn_f *chain)
{
	chain->callNext();

	// KTP: Skip spawn processing during map change to prevent crashes
	if (g_bMapChangeInProgress)
		return;

	// Get the host client (the client that sent the spawn command) AFTER chain call
	// This ensures the client state is properly set up by the engine
	if (!RehldsFuncs)
		return;

	IGameClient *cl = RehldsFuncs->GetHostClient();
	if (!cl)
		return;

	// Must be fully connected before we process
	if (!cl->IsConnected())
		return;

	edict_t *pEntity = cl->GetEdict();
	if (!pEntity || pEntity->free)
		return;

	// Validate player index
	int index = ENTINDEX(pEntity);
	if (index < 1 || index > gpGlobals->maxClients)
		return;

	CPlayer *pPlayer = GET_PLAYER_POINTER_I(index);
	if (!pPlayer)
		return;

	// Skip bots
	if (pEntity->v.flags & FL_FAKECLIENT)
		return;

	// If player was never connected (e.g., map change reconnect where ClientConnected doesn't fire),
	// initialize now. Connect() sets initialized=true, so check that flag.
	if (!pPlayer->initialized)
	{
		const char *pszName = cl->GetName();
		char pszAddress[64] = "0.0.0.0";

		INetChan *netChan = cl->GetNetChan();
		if (netChan)
		{
			const netadr_t *addr = netChan->GetRemoteAdr();
			if (addr)
			{
				ke::SafeSprintf(pszAddress, sizeof(pszAddress), "%d.%d.%d.%d:%d",
					addr->ip[0], addr->ip[1], addr->ip[2], addr->ip[3],
					ntohs(addr->port));
			}
		}

		pPlayer->Connect(pszName ? pszName : "", pszAddress);
		executeForwards(FF_ClientConnect, static_cast<cell>(pPlayer->index));
	}

	// Only call PutInServer if player is initialized and not already ingame
	// PutInServer() sets ingame=true, so check that flag
	if (pPlayer->initialized && !pPlayer->ingame)
	{
		// KTP: Ensure pEdict is set before PutInServer - natives like get_user_team need it
		if (!pPlayer->pEdict || pPlayer->pEdict != pEntity)
			pPlayer->pEdict = pEntity;

		// KTP: Authorize the player if not already authorized
		// In extension mode, Steam_NotifyClientConnect may not have fired yet
		if (!pPlayer->authorized)
		{
			const char *authid = GETPLAYERAUTHID(pEntity);
			pPlayer->Authorize(authid);
			if (g_auth_funcs.size())
			{
				List<AUTHORIZEFUNC>::iterator iter, end = g_auth_funcs.end();
				AUTHORIZEFUNC fn;
				for (iter = g_auth_funcs.begin(); iter != end; iter++)
				{
					fn = (*iter);
					fn(index, authid);
				}
			}
			executeForwards(FF_ClientAuthorized, static_cast<cell>(index), authid);
		}

		pPlayer->PutInServer();
		++g_players_num;

		// KTP: Queue the client_putinserver forward for next frame
		// We cannot call it directly here because the client is not fully spawned yet
		// and plugins may try to send network messages (like client_print) which will crash
		if (index >= 1 && index <= 32)
			g_putinserver_mask |= (1u << (index - 1));
	}
}

// KTP: PF_changelevel_I hook - sets map change flag BEFORE any map change processing
// This is called when the changelevel command is executed, BEFORE SV_InactivateClients
static void PF_changelevel_I_RH(IRehldsHook_PF_changelevel_I *chain, const char *s1, const char *s2)
{
	// Only latch for a map that actually exists. This call merely queues "changelevel s1"
	// into Cbuf; Host_Changelevel_f runs it next frame and rejects an unknown map with the
	// same IS_MAP_VALID test, returning before SV_ActivateServer — the only place the flag
	// clears. Latching unconditionally therefore wedges every AMXX path that early-outs on
	// it (tasks, module frame callbacks, events, logevents, client commands) for the rest
	// of the map. A real map change still latches here, and SV_InactivateClients_RH latches
	// again once the change is genuinely under way.
	// Only readers are ReHLDS hooks, and under Metamod SV_ActivateServer_RH
	// passes through before reaching the clear — so latching there would wedge
	// the flag on for good. See SV_ClientCommand_RH.
	if (!g_bRunningWithMetamod && s1 && IS_MAP_VALID(s1))
		g_bMapChangeInProgress = true;

	// Continue with the map change
	chain->callNext(s1, s2);
}

// KTP: SV_Frame hook for per-frame processing in extension mode
// This replaces C_StartFrame_Post functionality when running without Metamod
// AX-01 residual: PF_changelevel_I_internal burns its once-per-spawncount queue
// guard whether or not the map was valid, so an invalid pfnChangeLevel followed
// by a valid one in the same spawncount latches g_bMapChangeInProgress while the
// queue is dropped. SV_ActivateServer never runs, nothing clears the flag, and
// every AMXX path that early-outs on it stays dead for the rest of the map —
// tasks, module frame callbacks (Discord), events, logevents, "." commands.
// Recovery was previously an rcon changelevel or the nightly restart.
//
// So: if the flag is still set after a spell of frames long enough that no real
// changelevel could still be in flight, assume nothing is coming and release it.
// Time rather than a frame count because frame rate varies by an order of
// magnitude across the fleet, and a real transition barely ticks SV_Frame at all.
#define KTP_MAPCHANGE_STUCK_SECONDS 30.0f

// When the latch began, map-relative. MUST be cleared on every healthy frame
// (see SV_Frame_RH): if it only reset when the watchdog fired, a stale value
// would survive a legitimate map change, and a short map followed by a long one
// would make the next real changelevel look 30s overdue on its very first
// frame — releasing the suppression mid-transition, which is the crash this
// flag exists to prevent.
static float g_mapChangeLatchedAt = 0.0f;

static void KTPAMX_MapChangeWatchdog()
{
	float &s_latchedAt = g_mapChangeLatchedAt;

	// Teardown latches the same flag deliberately and owns it until the process
	// exits. Clearing it here would re-enable the plugin code shutdown just
	// finished suppressing.
	if (g_bExtShuttingDown)
	{
		s_latchedAt = 0.0f;
		return;
	}

	const float now = gpGlobals->time;

	// gpGlobals->time is map-relative and restarts with the map. A real change
	// would have cleared the flag via SV_ActivateServer, so going backwards here
	// means the reference is stale rather than that time passed — re-latch.
	if (s_latchedAt == 0.0f || now < s_latchedAt)
	{
		s_latchedAt = now;
		return;
	}

	if (now - s_latchedAt < KTP_MAPCHANGE_STUCK_SECONDS)
		return;

	AMXXLOG_Log("[AMXX] WATCHDOG: map-change flag stuck for %.0fs with the server "
		"still running — a changelevel was latched but never activated (AX-01). "
		"Releasing it; AMXX processing resumes.", now - s_latchedAt);

	g_bMapChangeInProgress = false;
	s_latchedAt = 0.0f;
}

void SV_Frame_RH(IRehldsHook_SV_Frame *chain)
{
	chain->callNext();

	// KTP: Skip all AMXX processing during map change to prevent crashes
	// Game state is invalid during transition (entities freed, globals changing, etc.)
	if (g_bMapChangeInProgress)
	{
		KTPAMX_MapChangeWatchdog();
		return;
	}

	// Reached only when the flag is clear, so the latch reference is dead.
	g_mapChangeLatchedAt = 0.0f;

	// Execute frame callbacks (equivalent to C_StartFrame_Post)
	g_frameActionMngr.ExecuteFrameCallbacks();

	// KTP: Execute module frame callbacks (for cURL async, etc)
	Module_ExecuteFrameCallbacks();

	// Process tasks (throttled to every 0.1 seconds like C_StartFrame_Post)
	if (g_task_time <= gpGlobals->time)
	{
		g_task_time = gpGlobals->time + 0.1f;
		g_tasksMngr.startFrame();
		CoreCfg.OnMapConfigTimer();
	}

	// Process pending client_putinserver forwards (bitmask — zero cost when empty)
	if (g_putinserver_mask)
	{
		for (int i = 0; i < gpGlobals->maxClients; i++)
		{
			if (!(g_putinserver_mask & (1u << i)))
				continue;

			int playerIndex = i + 1;
			CPlayer* pPlayer = GET_PLAYER_POINTER_I(playerIndex);
			if (!pPlayer || !pPlayer->pEdict || pPlayer->pEdict->free)
			{
				g_putinserver_mask &= ~(1u << i);  // Invalid, clear bit
				continue;
			}

			IGameClient* cl = RehldsSvs ? RehldsSvs->GetClient(i) : nullptr;
			if (!cl)
			{
				g_putinserver_mask &= ~(1u << i);
				continue;
			}

			if (!cl->IsSpawned())
				continue;  // Not ready yet, keep bit set

			// Client is spawned - fire the forward and clear bit
			g_putinserver_mask &= ~(1u << i);
			if (FF_ClientPutInServer >= 0)
				executeForwards(FF_ClientPutInServer, static_cast<cell>(playerIndex));
		}
	}
}


// KTP: IMessageManager hook for register_event in extension mode
// This is called for each message type that has registered events
void MessageHook_Handler(IVoidHookChain<IMessage *> *chain, IMessage *msg)
{
	// KTP: Defensive guard — null msg should never happen from engine, but if it does,
	// skip entirely. Do NOT call chain->callNext(null) as downstream hooks dereference msg.
	if (!msg)
		return;

	chain->callNext(msg);

	// KTP: Skip message processing during map change to prevent crashes
	if (g_bMapChangeInProgress)
		return;

	int msg_type = msg->getId();
	if (msg_type < 0 || msg_type >= MAX_REG_MSGS)
		return;

	// Get edict and player info
	edict_t *ed = msg->getEdict();
	if (ed)
	{
		mPlayerIndex = ENTINDEX(ed);
		mPlayer = GET_PLAYER_POINTER_I(mPlayerIndex);
	}
	else
	{
		mPlayerIndex = 0;
		mPlayer = 0;
	}

	// Initialize event parser
	mState = 0;
	function = modMsgs[msg_type];
	endfunction = modMsgsEnd[msg_type];

	g_events.parserInit(msg_type, &gpGlobals->time, mPlayer, mPlayerIndex);

	// KTP: Call module begin handlers (allows modules like DODX to set up their mPlayer/mState)
	int mDest = static_cast<int>(msg->getDest());
	CallModuleMsgBeginHandlers(msg_type, mDest, mPlayerIndex, ed);

	// Parse all parameters from the message
	int paramCount = msg->getParamCount();
	for (int i = 0; i < paramCount; i++)
	{
		IMessage::ParamType ptype = msg->getParamType(i);
		switch (ptype)
		{
			case IMessage::ParamType::Byte:
			case IMessage::ParamType::Char:
			case IMessage::ParamType::Short:
			case IMessage::ParamType::Long:
			case IMessage::ParamType::Entity:
			{
				int iValue = msg->getParamInt(i);
				g_events.parseValue(iValue);
				if (function) (*function)((void *)&iValue);
				// KTP: Call module handlers (for DODX stats etc)
				CallModuleMsgHandlers(msg_type, (void *)&iValue);
				break;
			}
			case IMessage::ParamType::Angle:
			case IMessage::ParamType::Coord:
			{
				float flValue = msg->getParamFloat(i);
				g_events.parseValue(flValue);
				if (function) (*function)((void *)&flValue);
				// KTP: Call module handlers (for DODX stats etc)
				CallModuleMsgHandlers(msg_type, (void *)&flValue);
				break;
			}
			case IMessage::ParamType::String:
			{
				const char *sz = msg->getParamString(i);
				g_events.parseValue(sz);
				if (function) (*function)((void *)sz);
				// KTP: Call module handlers (for DODX stats etc)
				CallModuleMsgHandlers(msg_type, (void *)sz);
				break;
			}
		}
	}

	// Execute events
	g_events.executeEvents();
	if (endfunction) (*endfunction)(NULL);
	// KTP: Call module end handlers (for DODX stats etc)
	CallModuleMsgEndHandlers(msg_type);
}

// KTP: Install IMessageManager hook for a specific message ID (extension mode only)
void InstallMessageHook(int msg_id)
{
	if (g_bRunningWithMetamod)
		return;  // Metamod handles this via engine hooks

	if (!RehldsMessageManager)
		return;  // MessageManager not available

	if (msg_id < 0 || msg_id >= MAX_REG_MSGS)
		return;

	if (g_MessageHooksInstalled[msg_id])
		return;  // Already installed

	RehldsMessageManager->registerHook(msg_id, MessageHook_Handler, HC_PRIORITY_DEFAULT);
	g_MessageHooksInstalled[msg_id] = true;
}


// KTP: SV_CheckConsistencyResponse hook for inconsistent_file forward in extension mode
// This is called when the server receives a consistency response from a client
// Hook return value: TRUE = file is inconsistent (kick player), FALSE = file is OK (allow)
bool SV_CheckConsistencyResponse_RH(IRehldsHook_SV_CheckConsistencyResponse *chain, IGameClient *cl, resource_t *resource, uint32 hash)
{
	// Call the original first to see if it's inconsistent
	// result = TRUE means hashes don't match (inconsistent), FALSE means they match (consistent)
	bool result = chain->callNext(cl, resource, hash);

	// Only trigger the forward if the file IS inconsistent (result = true)
	if (result && resource && cl)
	{
		edict_t *pEntity = cl->GetEdict();
		if (pEntity)
		{
			CPlayer *pPlayer = GET_PLAYER_POINTER(pEntity);
			if (pPlayer && pPlayer->initialized && FF_InconsistentFile >= 0)
			{
				char disconnect_message[256] = "";

				// Execute the forward: inconsistent_file(id, const filename[], disconnect_message[64])
				// Forward return: 1 (PLUGIN_HANDLED) = allow player to stay, 0 = kick
				// Hook return: FALSE = file OK (allow), TRUE = file bad (kick)
				if (executeForwards(FF_InconsistentFile, static_cast<cell>(pPlayer->index),
					resource->szFileName, disconnect_message) == 1)
				{
					// Plugin wants to allow the player to stay - return FALSE (consistent/OK)
					return false;
				}

				// Plugin returns 0 or forward doesn't exist - kick the player
				// Return TRUE (inconsistent) so engine kicks them
			}
		}
	}

	return result;
}

// Forward declaration for init function
static void KTPAMX_InitAsRehldsExtension();

// KTP: PF_precache_model_I hook - processes force_unmodified during precache phase in extension mode
// This hook fires when the engine precaches any model. On first call, we:
// 1. Initialize AMXX fully (modules, plugins, hooks) with deferred plugin_init/plugin_cfg
// 2. Execute plugin_precache forward (so plugins can call force_unmodified)
// 3. Process all force lists with ENGINE_FORCE_UNMODIFIED
// This ensures force_unmodified works because we're still in the spawn/precache phase.
static int PF_precache_model_I_RH(IRehldsHook_PF_precache_model_I *chain, const char *s)
{
	// Worst case of the whole set: under Metamod this fires long after Meta_Attach
	// with g_bRehldsExtensionInit still false, so it would run a SECOND full AMXX
	// init — reloading plugins and re-registering every forward and hook.
	if (g_bRunningWithMetamod)
		return chain->callNext(s);

	// Only process once per map
	if (!g_bExtPrecacheProcessed)
	{
		g_bExtPrecacheProcessed = true;

		// KTP: Initialize AMXX during precache phase if not already done
		// This loads modules, plugins, and sets up all infrastructure.
		// plugin_init/plugin_cfg are deferred to SV_ActivateServer when g_bInitDuringPrecache is set.
		if (!g_bRehldsExtensionInit)
		{
			g_bInitDuringPrecache = true;
			KTPAMX_InitAsRehldsExtension();
		}

		// Execute plugin_precache forward - plugins will call force_unmodified() here
		g_dontprecache = false;
		executeForwards(FF_PluginPrecache);
		g_dontprecache = true;

		// Now process all force lists with ENGINE_FORCE_UNMODIFIED
		// We're still in the precache phase so this is allowed
		for (auto &model : g_forcemodels)
		{
			ENGINE_FORCE_UNMODIFIED(model->getForceType(), model->getMin(), model->getMax(), model->getFilename());
		}
		for (auto &sound : g_forcesounds)
		{
			ENGINE_FORCE_UNMODIFIED(sound->getForceType(), sound->getMin(), sound->getMax(), sound->getFilename());
		}
		for (auto &generic : g_forcegeneric)
		{
			ENGINE_FORCE_UNMODIFIED(generic->getForceType(), generic->getMin(), generic->getMax(), generic->getFilename());
		}
	}

	return chain->callNext(s);
}

// KTP: ClientConnected hook for client_connect and client_connectex forwards in extension mode
void ClientConnected_RH(IRehldsHook_ClientConnected *chain, IGameClient *cl)
{
	chain->callNext(cl);

	if (!cl)
		return;

	edict_t *pEntity = cl->GetEdict();
	if (!pEntity)
		return;

	// Validate player index
	int index = ENTINDEX(pEntity);
	if (index < 1 || index > gpGlobals->maxClients)
		return;

	CPlayer *pPlayer = GET_PLAYER_POINTER_I(index);
	if (!pPlayer)
		return;

	// Skip bots using edict flags (safer than pPlayer->IsBot() on uninitialized data)
	if (pEntity->v.flags & FL_FAKECLIENT)
		return;

	const char *pszName = cl->GetName();

	// Format the IP address manually from netadr_t
	char pszAddress[64] = "0.0.0.0";
	INetChan *netChan = cl->GetNetChan();
	if (netChan)
	{
		const netadr_t *addr = netChan->GetRemoteAdr();
		if (addr)
		{
			ke::SafeSprintf(pszAddress, sizeof(pszAddress), "%d.%d.%d.%d:%d",
				addr->ip[0], addr->ip[1], addr->ip[2], addr->ip[3],
				ntohs(addr->port));
		}
	}

	// KTP: Set pEdict before calling Connect() - extension mode doesn't have C_ServerActivate_Post
	// to initialize players with Init(), so we need to set the edict here
	pPlayer->pEdict = pEntity;
	pPlayer->index = index;

	// KTP: Retire a stale session before reusing the slot. On a mid-map crash-reconnect
	// ReHLDS's SV_ConnectClient reconnect path calls pfnClientDisconnect directly, and in
	// extension mode nothing wraps the DLL table — so CPlayer::Disconnect() never ran and
	// `ingame` is still true from the dead session. Connect() below wipes flags[] and
	// clears `authorized`, while SV_Spawn_f_RH's `initialized && !ingame` gate stays false:
	// the player never gets re-authorized or put in server, and their admin access is gone
	// until a full disconnect. Replaying the C_ClientDisconnect flow here clears `ingame`,
	// so the normal Steam/Spawn_f path re-authorizes the new session.
	// Gate on `ingame` ALONE — not `initialized`. On a map-change reconnect
	// SV_InactivateClients_RH already disconnected the slot (ingame=false) and
	// Steam_NotifyClientConnect_RH re-Connect()ed it since (initialized=true), so gating on
	// `initialized` would fire disconnect forwards for every player on every map change.
	if (pPlayer->ingame)
	{
		// W2: client_remove below is part of the SAME replay, but it fires after
		// Disconnect() has cleared pPlayer->authid -- so the guard needs a value
		// copy that outlives the CPlayer field, not a chars() pointer into it.
		ke::AString departingAuthid(pPlayer->authid);

		if (pPlayer->initialized)
		{
			// The engine already replaced this slot's identity with the incoming
			// player's, so point get_user_authid at the departing session's id.
			KTPAuthReplayGuard authReplay(index, departingAuthid.chars());

			// deprecated
			executeForwards(FF_ClientDisconnect, static_cast<cell>(index));

			if (g_isDropClientHookAvailable && !pPlayer->disconnecting)
			{
				executeForwards(FF_ClientDisconnected, static_cast<cell>(index), FALSE, prepareCharArray(const_cast<char*>(""), 0), 0);
			}
		}

		--g_players_num;

		auto wasDisconnecting = pPlayer->disconnecting;

		pPlayer->Disconnect();

		if (!wasDisconnecting && g_isDropClientHookAvailable)
		{
			// Same replay, same substitution -- a client_remove handler reading
			// get_user_authid() would otherwise see the incoming player.
			KTPAuthReplayGuard authReplay(index, departingAuthid.chars());
			executeForwards(FF_ClientRemove, static_cast<cell>(index), FALSE, const_cast<char*>(""));
		}
	}

	// AX-08: Steam_NotifyClientConnect_RH runs FIRST in SV_ConnectClient and has
	// already done Connect() + client_connect + Authorize() + client_authorized
	// for this session. Re-running Connect() here does not just duplicate the
	// forwards -- Connect() sets authorized=false and memsets flags[], so it
	// discards the admin flags that authorize just resolved, and SV_Spawn_f_RH
	// then re-authorizes and fires client_authorized a second time. Metamod
	// fires each exactly once (C_ClientConnect owns it); this is the parity gap.
	//
	// `initialized` is the right test: Disconnect() clears it at map change, so
	// a genuine new session still connects here, and a client whose Steam
	// notify never ran (nothing set initialized) is unaffected.
	if (!pPlayer->initialized)
	{
		// Initialize player first so forwards have valid player data
		pPlayer->Connect(pszName ? pszName : "", pszAddress);

		// Call client_connect forward
		executeForwards(FF_ClientConnect, static_cast<cell>(index));
	}
	else
	{
		// W1: Steam_NotifyClientConnect_RH called Connect() with cl->GetName()
		// BEFORE the engine ran SV_ExtractFromUserinfo, so on a reused slot the
		// cached name can still be the previous occupant's. This hook fires after
		// the engine has settled; the old unconditional Connect() used to correct
		// it, so refresh explicitly now that we skip that. name/ip feed admin.sma's
		// FLAG_IP matching -- a stale value lands in the admin-auth path.
		pPlayer->name = pszName ? pszName : "";
		pPlayer->ip   = pszAddress;
	}

	// C3: client_connectex has NO counterpart in Steam_NotifyClientConnect_RH or
	// SV_Spawn_f_RH -- both fire only client_connect. Under Metamod,
	// C_ClientConnect_Post fires BOTH every map change, so gating this alongside
	// client_connect silently stopped delivering it on every reconnect. Stays
	// outside the gate; rejection cannot be honoured this late either way.
	char szRejectReason[128] = "";
	executeForwards(FF_ClientConnectEx, static_cast<cell>(index),
		pszName ? pszName : "", pszAddress, prepareCharArray(szRejectReason, 128, true));

	// Note: Don't queue client_putinserver here - player isn't spawned yet.
	// SV_Spawn_f_RH will queue it after calling PutInServer().
}

// KTP: Steam_NotifyClientConnect hook for client_authorized forward in extension mode
// This is called when Steam validates a client's connection
qboolean Steam_NotifyClientConnect_RH(IRehldsHook_Steam_NotifyClientConnect *chain, IGameClient *cl, const void *pvSteam2Key, unsigned int ucbSteam2Key)
{
	if (!cl)
		return chain->callNext(cl, pvSteam2Key, ucbSteam2Key);

	qboolean result = chain->callNext(cl, pvSteam2Key, ucbSteam2Key);

	if (!result || !cl)
		return result;

	// Check if client is actually connected (not server Steam auth during boot)
	if (!cl->IsConnected())
		return result;

	edict_t *pEntity = cl->GetEdict();
	if (!pEntity)
		return result;

	// Validate player index
	int index = ENTINDEX(pEntity);
	if (index < 1 || index > gpGlobals->maxClients)
		return result;

	CPlayer *pPlayer = GET_PLAYER_POINTER_I(index);
	if (!pPlayer)
		return result;

	// Skip bots
	if (pEntity->v.flags & FL_FAKECLIENT)
		return result;

	// If player isn't connected yet (Steam validated before ClientConnected hook),
	// initialize them first so forwards work properly
	if (!pPlayer->ingame)
	{
		const char *pszName = cl->GetName();
		char pszAddress[64] = "0.0.0.0";

		INetChan *netChan = cl->GetNetChan();
		if (netChan)
		{
			const netadr_t *addr = netChan->GetRemoteAdr();
			if (addr)
			{
				ke::SafeSprintf(pszAddress, sizeof(pszAddress), "%d.%d.%d.%d:%d",
					addr->ip[0], addr->ip[1], addr->ip[2], addr->ip[3],
					ntohs(addr->port));
			}
		}

		pPlayer->Connect(pszName ? pszName : "", pszAddress);
		executeForwards(FF_ClientConnect, static_cast<cell>(index));
	}

	// Only authorize once
	if (!pPlayer->authorized)
	{
		const char *authid = GETPLAYERAUTHID(pEntity);
		pPlayer->Authorize(authid);
		if (g_auth_funcs.size())
		{
			List<AUTHORIZEFUNC>::iterator iter, end = g_auth_funcs.end();
			AUTHORIZEFUNC fn;
			for (iter = g_auth_funcs.begin(); iter != end; iter++)
			{
				fn = (*iter);
				fn(index, authid);
			}
		}
		executeForwards(FF_ClientAuthorized, static_cast<cell>(index), authid);
	}

	return result;
}

// KTP: SV_ClientUserInfoChanged hook — fires client_infochanged forward and
// refreshes CPlayer::name from the engine's infobuffer. The Metamod equivalent
// (C_ClientUserInfoChanged_Post via gFunctionTable_Post.pfnClientUserInfoChanged)
// never fires in extension mode because the engine calls the game DLL's
// pfnClientUserInfoChanged directly, so without this hook get_user_name()
// returns the connect-time name forever — every respawn after a setinfo "name"
// "..." reads the stale CPlayer::name cache and old names persist on AMXX HUDs.
void SV_ClientUserInfoChanged_RH(IRehldsHook_SV_ClientUserInfoChanged *chain, IGameClient *cl)
{
	// Pass through first so the engine has applied the userinfo change before we
	// re-read the infobuffer.
	chain->callNext(cl);

	// In Metamod mode, C_ClientUserInfoChanged_Post fires via gFunctionTable_Post —
	// don't double-fire client_infochanged here.
	if (g_bRunningWithMetamod)
		return;

	if (!cl || !cl->IsConnected())
		return;

	edict_t *pEntity = cl->GetEdict();
	if (!pEntity)
		return;

	int index = ENTINDEX(pEntity);
	if (index < 1 || index > gpGlobals->maxClients)
		return;

	CPlayer *pPlayer = GET_PLAYER_POINTER_I(index);

#if defined(KTP_LANE_B_FAKECLIENTS)
	// ========================= TEST BUILD ONLY =========================
	// Register fake clients (bots) as AMXX players in extension mode.
	//
	// NOT FOR PRODUCTION. Guarded by a compile-time define that is set only by
	// the Lane B test build; a normal build of this file is byte-for-byte
	// unchanged, so the fleet cannot inherit this by deploy accident. Same
	// containment shape as KTPMatchHandler's -DKTP_TEST_MODE.
	//
	// Why it is needed: in extension mode AMXX never registers a bot as a
	// player, so `is_user_connected()` is false for every bot and
	// `get_players()` returns 0 while the engine has a full server. Measured
	// with 6 bots demonstrably fighting and capturing. Every emit path in
	// KTPAMXX/plugins/dod/ktp_stats_capture.inc is gated on that native
	// (ksc_on_death, the assist loop, ksc_emit_break, ksc_origin_str), so the
	// stats-capture work cannot be tested against bots without this.
	//
	// Note DODX is unaffected either way: it keeps its own player array keyed
	// off FL_FAKECLIENT, which is why its forwards fire for bots while AMXX
	// sees nobody. The comment this replaces claimed "DODX extension hooks
	// handle bot connect/putinserver elsewhere" — true of DODX's tracking, but
	// not of AMXX's player list, and that distinction is the whole gap.
	//
	// This mirrors the Metamod path's C_ClientUserInfoChanged_Post
	// `else if (pPlayer->IsBot())` branch. It must run BEFORE the
	// initialized/ingame guard below: a bot is neither, so that guard returns
	// first and the FL_FAKECLIENT check further down is never even reached.
	if (pPlayer && !pPlayer->ingame && (pEntity->v.flags & FL_FAKECLIENT))
	{
		char *botinfo = GET_INFOKEYBUFFER(pEntity);
		const char *botname = botinfo ? INFOKEY_VALUE(botinfo, "name") : "";

		pPlayer->Connect(botname ? botname : "", "127.0.0.1");
		executeForwards(FF_ClientConnect, static_cast<cell>(pPlayer->index));

		const char *authid = GETPLAYERAUTHID(pEntity);
		pPlayer->Authorize(authid);
		if (g_auth_funcs.size())
		{
			List<AUTHORIZEFUNC>::iterator iter, end = g_auth_funcs.end();
			AUTHORIZEFUNC fn;
			for (iter = g_auth_funcs.begin(); iter != end; iter++)
			{
				fn = (*iter);
				fn(pPlayer->index, authid);
			}
		}
		executeForwards(FF_ClientAuthorized, static_cast<cell>(pPlayer->index), authid);

		// Natives like get_user_team() read pEdict; the extension-mode
		// SV_Spawn_f path sets it explicitly for the same reason.
		pPlayer->pEdict = pEntity;

		pPlayer->PutInServer();
		++g_players_num;
		executeForwards(FF_ClientPutInServer, static_cast<cell>(pPlayer->index));
	}
	// =================== END TEST BUILD ONLY ===========================
#endif

	// Match the C_ClientCvarChanged guard below — plugin handlers assume a fully
	// connected player; firing FF_ClientInfoChanged before initialized + ingame are
	// both true leads to undefined behaviour in plugin code.
	if (!pPlayer || !pPlayer->initialized || !pPlayer->ingame)
		return;

	// Skip bots — fakeclient userinfo is set once at connect and not authoritative.
	// Note: unlike the Metamod path's C_ClientUserInfoChanged_Post, we do not need
	// the `else if (pPlayer->IsBot())` Connect-emulation branch here — DODX extension
	// hooks handle bot connect/putinserver elsewhere (see extension_mode_no_fakemeta.md).
	// (A Lane B test build adds exactly that branch above, behind
	// KTP_LANE_B_FAKECLIENTS.)
	if (pEntity->v.flags & FL_FAKECLIENT)
		return;

	// Only fire the forward when the cache was actually refreshed. If GET_INFOKEYBUFFER
	// returns NULL or the "name" key is absent (engine infobuffer corruption / connect-
	// time race), firing with stale `pPlayer->name` would reproduce the same bug class
	// this hook exists to fix.
	char *infobuffer = GET_INFOKEYBUFFER(pEntity);
	if (!infobuffer)
		return;

	const char *name = INFOKEY_VALUE(infobuffer, "name");
	if (!name || !*name)
		return;

	// Fire BEFORE refreshing the cache, matching C_ClientUserInfoChanged_Post. Plugins
	// detect a rename by comparing cached get_user_name() against get_user_info("name");
	// updating pPlayer->name first makes those equal and the rename undetectable.
	executeForwards(FF_ClientInfoChanged, static_cast<cell>(index));
	pPlayer->name = name;
}

void C_ClientPutInServer_Post(edict_t *pEntity)
{
	CPlayer *pPlayer = GET_PLAYER_POINTER(pEntity);

	if (!pPlayer->IsBot())
	{
		pPlayer->PutInServer();
		++g_players_num;
		// KTP: In extension mode, client_putinserver forward is handled by SV_Spawn_f hook
		// to avoid duplicate calls. Only fire the forward in Metamod mode.
		if (g_bRunningWithMetamod)
		{
			executeForwards(FF_ClientPutInServer, static_cast<cell>(pPlayer->index));
		}
	}

	RETURN_META(MRES_IGNORED);
}

void C_ClientUserInfoChanged_Post(edict_t *pEntity, char *infobuffer)
{
	CPlayer *pPlayer = GET_PLAYER_POINTER(pEntity);

	executeForwards(FF_ClientInfoChanged, static_cast<cell>(pPlayer->index));
	const char* name = INFOKEY_VALUE(infobuffer, "name");

	// Emulate bot connection and putinserver
	if (pPlayer->ingame)
	{
		pPlayer->name = name;  // Make sure player have name up to date
	}
	else if (pPlayer->IsBot())
	{
		pPlayer->Connect(name, "127.0.0.1"/*CVAR_GET_STRING("net_address")*/);

		executeForwards(FF_ClientConnect, static_cast<cell>(pPlayer->index));

		const char* authid = GETPLAYERAUTHID(pEntity);

		pPlayer->Authorize(authid);
		if (g_auth_funcs.size())
		{
			List<AUTHORIZEFUNC>::iterator iter, end=g_auth_funcs.end();
			AUTHORIZEFUNC fn;
			for (iter=g_auth_funcs.begin(); iter!=end; iter++)
			{
				fn = (*iter);
				fn(pPlayer->index, authid);
			}
		}
		executeForwards(FF_ClientAuthorized, static_cast<cell>(pPlayer->index), authid);

		pPlayer->PutInServer();
		++g_players_num;

		executeForwards(FF_ClientPutInServer, static_cast<cell>(pPlayer->index));
	}

	RETURN_META(MRES_IGNORED);
}

// KTP Custom: Real-time client cvar change notification
// Called by ReHLDS when a client cvar query response is received
// This enables real-time cvar validation without periodic polling
void C_ClientCvarChanged(const edict_t *pEntity, const char *cvarName, const char *value)
{
	if (!pEntity || FNullEnt(pEntity))
		RETURN_META(MRES_IGNORED);

	int index = ENTINDEX(const_cast<edict_t*>(pEntity));
	if (index < 1 || index > gpGlobals->maxClients)
		RETURN_META(MRES_IGNORED);

	CPlayer *pPlayer = GET_PLAYER_POINTER_I(index);

	// KTP: Guard against cvar responses arriving during reconnect/map-change.
	// Plugin handlers assume a fully connected player; firing before initialized
	// and ingame are both true leads to undefined behaviour in plugin code.
	if (!pPlayer->initialized || !pPlayer->ingame)
		RETURN_META(MRES_IGNORED);

	executeForwards(FF_ClientCvarChanged, static_cast<cell>(pPlayer->index), cvarName, value);
	RETURN_META(MRES_IGNORED);
}

void C_ClientCommand(edict_t *pEntity)
{
	CPlayer *pPlayer = GET_PLAYER_POINTER(pEntity);

	META_RES result = MRES_IGNORED;
	cell ret = 0;

	const char* cmd = CMD_ARGV(0);
	const char* arg = CMD_ARGV(1);

	// Handle "amx" if not on listenserver
	if (IS_DEDICATED_SERVER())
	{
		if (cmd && stricmp(cmd, "amx") == 0)
		{
			// Print version
			static char buf[1024];
			size_t len = 0;

			sprintf(buf, "%s %s\n", Plugin_info.name, Plugin_info.version);
			CLIENT_PRINT(pEntity, print_console, buf);
			len = sprintf(buf, "Author: Tony 'Nein_' (https://github.com/afraznein)\n");
			CLIENT_PRINT(pEntity, print_console, buf);
			len = sprintf(buf, "Based on AMX Mod X by:\n         David \"BAILOPAN\" Anderson, Pavol \"PM OnoTo\" Marko, Felix \"SniperBeamer\" Geyer\n");
			len += sprintf(&buf[len], "         Jonny \"Got His Gun\" Bergstrom, Lukasz \"SidLuke\" Wlasinski\n");
			CLIENT_PRINT(pEntity, print_console, buf);
			len = sprintf(buf, "         Christian \"Basic-Master\" Hammacher, Borja \"faluco\" Ferrer\n");
			len += sprintf(&buf[len], "         Scott \"DS\" Ehlert\n");
			len += sprintf(&buf[len], "Compiled: %s\nURL: https://github.com/afraznein/KTPAMXX\n", __DATE__ ", " __TIME__);
			CLIENT_PRINT(pEntity, print_console, buf);
#ifdef JIT
			sprintf(buf, "Core mode: JIT\n");
#else
#ifdef ASM32
			sprintf(buf, "Core mode: ASM\n");
#else
			sprintf(buf, "Core mode: Normal\n");
#endif
#endif
			CLIENT_PRINT(pEntity, print_console, buf);
			RETURN_META(MRES_SUPERCEDE);
		}
	}

	if (executeForwards(FF_ClientCommand, static_cast<cell>(pPlayer->index)) > 0)
		RETURN_META(MRES_SUPERCEDE);

	/* check for command and if needed also for first argument and call proper function */

	CmdMngr::iterator aa = g_commands.clcmdprefixbegin(cmd);

	if (!aa)
		aa = g_commands.clcmdbegin();

	while (aa)
	{
		if ((*aa).matchCommandLine(cmd, arg) && (*aa).getPlugin()->isExecutable((*aa).getFunction()))
		{
			ret = executeForwards((*aa).getFunction(), static_cast<cell>(pPlayer->index),
				static_cast<cell>((*aa).getFlags()), static_cast<cell>((*aa).getId()));
			if (ret & 2) result = MRES_SUPERCEDE;
			if (ret & 1) RETURN_META(MRES_SUPERCEDE);
		}
		++aa;
	}

	/* check menu commands */

	if (!strcmp(cmd, "menuselect"))
	{
		int	pressed_key	= atoi(arg) - 1;
		int	bit_key	= (1<<pressed_key);

		if (pPlayer->keys &	bit_key)
		{
			if (gpGlobals->time > pPlayer->menuexpire)
			{
				if (Menu *pMenu = get_menu_by_id(pPlayer->newmenu))
				{
					pMenu->Close(pPlayer->index);

					RETURN_META(MRES_SUPERCEDE);
				}
				else if (pPlayer->menu > 0 && !pPlayer->vgui)
				{
					pPlayer->menu = 0;
					pPlayer->keys = 0;

					RETURN_META(MRES_SUPERCEDE);
				}
			}

			int menuid = pPlayer->menu;
			pPlayer->menu = 0;

			/* First, do new menus */
			int func_was_executed = -1;
			if (pPlayer->newmenu != -1)
			{
				int menu = pPlayer->newmenu;
				pPlayer->newmenu = -1;
				if (Menu *pMenu = get_menu_by_id(menu))
				{
					int item = pMenu->PagekeyToItem(pPlayer->page, pressed_key+1);
					if (item == MENU_BACK)
					{
						if (pMenu->pageCallback >= 0)
							executeForwards(pMenu->pageCallback, static_cast<cell>(pPlayer->index), static_cast<cell>(MENU_BACK), static_cast<cell>(menu));

						// Re-validate: plugin callback may have destroyed this menu
						pMenu = get_menu_by_id(menu);
						if (pMenu)
							pMenu->Display(pPlayer->index, pPlayer->page - 1);
					} else if (item == MENU_MORE) {
						if (pMenu->pageCallback >= 0)
							executeForwards(pMenu->pageCallback, static_cast<cell>(pPlayer->index), static_cast<cell>(MENU_MORE), static_cast<cell>(menu));

						// Re-validate: plugin callback may have destroyed this menu
						pMenu = get_menu_by_id(menu);
						if (pMenu)
							pMenu->Display(pPlayer->index, pPlayer->page + 1);
					} else {
						// Capture func before execution — menu may be destroyed in callback
						int menuFunc = pMenu->func;
						ret = executeForwards(menuFunc, static_cast<cell>(pPlayer->index), static_cast<cell>(menu), static_cast<cell>(item));
						/**
						 * No matter what we marked it as executed, since the callback styles are
						 * entirely different.  After all, this is a backwards compat shim.
						 */
						func_was_executed = menuFunc;
						if (ret & 2)
						{
							result = MRES_SUPERCEDE;
						} else if (ret & 1) {
							RETURN_META(MRES_SUPERCEDE);
						}
					}
				}
			}

			/* Now, do old menus */
			MenuMngr::iterator a = g_menucmds.begin();

			while (a)
			{
				g_menucmds.SetWatchIter(a);
				if ((*a).matchCommand(menuid, bit_key)
					&& (*a).getPlugin()->isExecutable((*a).getFunction())
					&& (func_was_executed == -1
						|| !g_forwards.isSameSPForward(func_was_executed, (*a).getFunction()))
					)
				{
					ret = executeForwards((*a).getFunction(), static_cast<cell>(pPlayer->index),
						static_cast<cell>(pressed_key), 0);

					if (ret & 2) result = MRES_SUPERCEDE;
					if (ret & 1) RETURN_META(MRES_SUPERCEDE);
				}
				if (g_menucmds.GetWatchIter() != a)
				{
					a = g_menucmds.GetWatchIter();
				} else {
					++a;
				}
			}
		}
	}

	/* check for PLUGIN_HANDLED_MAIN and block hl call if needed */
	RETURN_META(result);
}

void C_StartFrame_Post(void)
{
	if (g_auth_time < gpGlobals->time)
	{
		g_auth_time = gpGlobals->time + 0.7f;

		size_t i = 0;
		while (i < g_auth.length())
		{
			auto player = g_auth[i].get();
			const char*	auth = GETPLAYERAUTHID((*player)->pEdict);

			if ((auth == 0) || (*auth == 0))
			{
				g_auth.remove(i);
				continue;
			}

			if (strcmp(auth, "STEAM_ID_PENDING"))
			{
				(*player)->Authorize(auth);
				if (g_auth_funcs.size())
				{
					List<AUTHORIZEFUNC>::iterator iter, end=g_auth_funcs.end();
					AUTHORIZEFUNC fn;
					for (iter=g_auth_funcs.begin(); iter!=end; iter++)
					{
						fn = (*iter);
						fn((*player)->index, auth);
					}
				}
				executeForwards(FF_ClientAuthorized, static_cast<cell>((*player)->index), auth);
				g_auth.remove(i);

				continue;
			}
			i++;
		}
	}

#ifdef MEMORY_TEST
	if (g_memreport_enabled && g_next_memreport_time <= gpGlobals->time)
	{
		g_next_memreport_time = gpGlobals->time + MEMREPORT_INTERVAL;

		if (g_memreport_count == 0)
		{
			// make new directory
			time_t td;
			time(&td);
			tm *curTime = localtime(&td);

			int i = 0;
#if defined(__linux__) || defined(__APPLE__)
			mkdir(build_pathname("%s/memreports", get_localinfo("amxx_basedir", "addons/ktpamx")), 0700);
#else
			mkdir(build_pathname("%s/memreports", get_localinfo("amxx_basedir", "addons/ktpamx")));
#endif
			while (true)
			{
				char buffer[256];
				sprintf(buffer, "%s/memreports/D%02d%02d%03d", get_localinfo("amxx_basedir", "addons/ktpamx"), curTime->tm_mon + 1, curTime->tm_mday, i);
#if defined(__linux__) || defined(__APPLE__)
				mkdir(build_pathname("%s", g_log_dir.chars()), 0700);
				if (mkdir(build_pathname(buffer), 0700) < 0)
#else
				mkdir(build_pathname("%s", g_log_dir.chars()));
				if (mkdir(build_pathname(buffer)) < 0)
#endif
				{
					if (errno == EEXIST)
					{
						// good
						++i;
						continue;
					} else {
						// bad
						g_memreport_enabled = false;
						AMXXLOG_Log("[AMXX] Fatal error: Can't create directory for memreport files (%s)", buffer);
						break;
					}
				}
				g_memreport_dir = buffer;
				// g_memreport_dir should be valid now
				break;
			}
		}

		m_dumpMemoryReport(build_pathname("%s/r%03d.txt", g_memreport_dir.chars(), g_memreport_count));
		AMXXLOG_Log("Memreport #%d created (file \"%s/r%03d.txt\") (interval %f)", g_memreport_count + 1, g_memreport_dir.chars(), g_memreport_count, MEMREPORT_INTERVAL);

		g_memreport_count++;
	}
#endif // MEMORY_TEST

	g_frameActionMngr.ExecuteFrameCallbacks();

	if (g_task_time > gpGlobals->time)
		RETURN_META(MRES_IGNORED);

	g_task_time = gpGlobals->time + 0.1f;
	g_tasksMngr.startFrame();

	CoreCfg.OnMapConfigTimer();

	RETURN_META(MRES_IGNORED);
}

void C_MessageBegin_Post(int msg_dest, int msg_type, const float *pOrigin, edict_t *ed)
{
	if (ed)
	{
		mPlayerIndex = ENTINDEX(ed);
		mPlayer	= GET_PLAYER_POINTER_I(mPlayerIndex);
	} else {
		mPlayerIndex = 0;
		mPlayer	= 0;
	}

	if (msg_type < 0 || msg_type >= MAX_REG_MSGS)
		msg_type = 0;

	mState = 0;
	function = modMsgs[msg_type];
	endfunction = modMsgsEnd[msg_type];

	g_events.parserInit(msg_type, &gpGlobals->time, mPlayer, mPlayerIndex);

	RETURN_META(MRES_IGNORED);
}

void C_WriteByte_Post(int iValue)
{
	g_events.parseValue(iValue);
	if (function) (*function)((void *)&iValue);

	RETURN_META(MRES_IGNORED);
}

void C_WriteChar_Post(int iValue)
{
	g_events.parseValue(iValue);
	if (function) (*function)((void *)&iValue);

	RETURN_META(MRES_IGNORED);
}

void C_WriteShort_Post(int iValue)
{
	g_events.parseValue(iValue);
	if (function) (*function)((void *)&iValue);

	RETURN_META(MRES_IGNORED);
}

void C_WriteLong_Post(int iValue)
{
	g_events.parseValue(iValue);
	if (function) (*function)((void *)&iValue);

	RETURN_META(MRES_IGNORED);
}

void C_WriteAngle_Post(float flValue)
{
	g_events.parseValue(flValue);
	if (function) (*function)((void *)&flValue);

	RETURN_META(MRES_IGNORED);
}

void C_WriteCoord_Post(float flValue)
{
	g_events.parseValue(flValue);
	if (function) (*function)((void *)&flValue);

	RETURN_META(MRES_IGNORED);
}

void C_WriteString_Post(const char *sz)
{
	g_events.parseValue(sz);
	if (function) (*function)((void *)sz);

	RETURN_META(MRES_IGNORED);
}

void C_WriteEntity_Post(int iValue)
{
	g_events.parseValue(iValue);
	if (function) (*function)((void *)&iValue);

	RETURN_META(MRES_IGNORED);
}

void C_MessageEnd_Post(void)
{
	g_events.executeEvents();
	if (endfunction) (*endfunction)(NULL);

	RETURN_META(MRES_IGNORED);
}

const char *C_Cmd_Args(void)
{
	// if the global "fake" flag is set, which means that engclient_cmd was used, supercede the function
	if (g_fakecmd.fake)
		RETURN_META_VALUE(MRES_SUPERCEDE, (g_fakecmd.argc > 1) ? g_fakecmd.args : g_fakecmd.argv[0]);

	// otherwise ignore it
	RETURN_META_VALUE(MRES_IGNORED, NULL);
}

const char *C_Cmd_Argv(int argc)
{
	// if the global "fake" flag is set, which means that engclient_cmd was used, supercede the function
	if (g_fakecmd.fake)
		RETURN_META_VALUE(MRES_SUPERCEDE, (argc < 3) ? g_fakecmd.argv[argc] : "");

	// otherwise ignore it
	RETURN_META_VALUE(MRES_IGNORED, NULL);
}

int	C_Cmd_Argc(void)
{
	// if the global "fake" flag is set, which means that engclient_cmd was used, supercede the function
	if (g_fakecmd.fake)
		RETURN_META_VALUE(MRES_SUPERCEDE, g_fakecmd.argc);

	// otherwise ignore it
	RETURN_META_VALUE(MRES_IGNORED, 0);
}

// Grenade has been	thrown.
// Only	here we	may	find out who is	an owner.
void C_SetModel(edict_t *e, const char *m)
{
	if (!m || strcmp(m, "models/w_hegrenade.mdl") != 0)
	{
		RETURN_META(MRES_IGNORED);
	}

	if (e->v.owner)
	{
		g_grenades.put(e, 1.75f, 4, GET_PLAYER_POINTER(e->v.owner));
	}

	RETURN_META(MRES_IGNORED);
}

// Save	at what	part of	body a player is aiming
void C_TraceLine_Post(const float *v1, const float *v2, int fNoMonsters, edict_t *e, TraceResult *ptr)
{
	if (e && (e->v.flags & (FL_CLIENT | FL_FAKECLIENT)))
	{
		CPlayer* pPlayer = GET_PLAYER_POINTER(e);

		if (ptr->pHit && (ptr->pHit->v.flags & (FL_CLIENT | FL_FAKECLIENT)))
			pPlayer->aiming = ptr->iHitgroup;

		pPlayer->lastTrace = ptr->vecEndPos;
	}

	RETURN_META(MRES_IGNORED);
}

void C_AlertMessage(ALERT_TYPE atype, const char *szFmt, ...)
{
	if (atype != at_logged)
	{
		RETURN_META(MRES_IGNORED);
	}

	/* There are also more messages but we want only logs
	at_notice,
	at_console,		// same	as at_notice, but forces a ConPrintf, not a	message	box
	at_aiconsole,	// same	as at_console, but only	shown if developer level is	2!
	at_warning,
	at_error,
	at_logged		// Server print to console ( only in multiplayer games ).
	*/

	cell retVal = 0;

	// execute logevents and plugin_log forward
	if (g_logevents.logEventsExist()
		|| g_forwards.getFuncsNum(FF_PluginLog))
	{
		va_list	logArgPtr;
		va_start(logArgPtr, szFmt);
		g_logevents.setLogString(szFmt, logArgPtr);
		va_end(logArgPtr);
		g_logevents.parseLogString();

		if (g_logevents.logEventsExist())
		{
			g_logevents.executeLogEvents();
		}

		retVal = executeForwards(FF_PluginLog);
	}

	if (retVal)
	{
		RETURN_META(MRES_SUPERCEDE);
	}

	RETURN_META(MRES_IGNORED);
}

void C_ChangeLevel(const char *map, const char *what)
{
	int ret = executeForwards(FF_ChangeLevel,  map);
	if (ret)
		RETURN_META(MRES_SUPERCEDE);
	RETURN_META(MRES_IGNORED);
}

void C_CvarValue2(const edict_t *pEdict, int requestId, const char *cvar, const char *value)
{
	CPlayer *pPlayer = GET_PLAYER_POINTER(pEdict);
	if (pPlayer->queries.empty())
		RETURN_META(MRES_IGNORED);

	List<ClientCvarQuery_Info *>::iterator iter, end=pPlayer->queries.end();
	ClientCvarQuery_Info *info;
	for (iter=pPlayer->queries.begin(); iter!=end; iter++)
	{
		info = (*iter);
		if ( info->requestId == requestId )
		{
			if (info->paramLen)
			{
				cell arr = prepareCellArray(info->params, info->paramLen);
				executeForwards(info->resultFwd, static_cast<cell>(ENTINDEX(pEdict)),
					cvar, value, arr);
			} else {
				executeForwards(info->resultFwd, static_cast<cell>(ENTINDEX(pEdict)),
					cvar, value);
			}
			unregisterSPForward(info->resultFwd);
			pPlayer->queries.erase(iter);
			delete [] info->params;
			delete info;

			break;
		}
	}

	RETURN_META(MRES_HANDLED);
}

C_DLLEXPORT	int	Meta_Query(const char	*ifvers, plugin_info_t **pPlugInfo,	mutil_funcs_t *pMetaUtilFuncs)
{
	gpMetaUtilFuncs = pMetaUtilFuncs;
	*pPlugInfo = &Plugin_info;

	int	mmajor = 0, mminor = 0,	pmajor = 0, pminor = 0;

	sscanf(ifvers, "%d:%d",	&mmajor, &mminor);
	sscanf(Plugin_info.ifvers, "%d:%d",	&pmajor, &pminor);

	if (strcmp(ifvers, Plugin_info.ifvers))
	{
		LOG_MESSAGE(PLID, "warning: ifvers mismatch (pl \"%s\") (mm \"%s\")", Plugin_info.ifvers, ifvers);
		if (pmajor > mmajor)
		{
			LOG_ERROR(PLID, "metamod version is too old for this plugin; update metamod");
			return (FALSE);
		} else if (pmajor < mmajor) {
			LOG_ERROR(PLID, "metamod version is incompatible with this plugin; please find a newer version of this plugin");
			return (FALSE);
		} else if (pmajor == mmajor) {
			if (pminor > mminor)
			{
				LOG_ERROR(PLID, "metamod version is incompatible with this plugin; please find a newer version of this plugin");
				return FALSE;
			} else if (pminor < mminor) {
				LOG_MESSAGE(PLID, "warning: there may be a newer version of metamod available");
			}
		}
	}

	// :NOTE: Don't call modules query here (g_FakeMeta.Meta_Query), because we don't know modules yet. Do it in Meta_Attach
	return (TRUE);
}

static META_FUNCTIONS gMetaFunctionTable;
C_DLLEXPORT	int	Meta_Attach(PLUG_LOADTIME now, META_FUNCTIONS *pFunctionTable, meta_globals_t *pMGlobals, gamedll_funcs_t *pGamedllFuncs)
{
	if (now > Plugin_info.loadable)
	{
		LOG_ERROR(PLID,	"Can't load	plugin right now");
		return (FALSE);
	}

	// KTP: Mark that we're running with Metamod
	g_bRunningWithMetamod = true;

	gpMetaGlobals = pMGlobals;
	gMetaFunctionTable.pfnGetEntityAPI2 = GetEntityAPI2;
	gMetaFunctionTable.pfnGetEntityAPI2_Post = GetEntityAPI2_Post;
	gMetaFunctionTable.pfnGetEngineFunctions = GetEngineFunctions;
	gMetaFunctionTable.pfnGetEngineFunctions_Post = GetEngineFunctions_Post;
#if !defined AMD64
	gMetaFunctionTable.pfnGetNewDLLFunctions = GetNewDLLFunctions;
#endif

	memcpy(pFunctionTable, &gMetaFunctionTable, sizeof(META_FUNCTIONS));
	gpGamedllFuncs=pGamedllFuncs;

	// KTP: Initialize game entity interface from Metamod
	if (gpGamedllFuncs && gpGamedllFuncs->dllapi_table)
	{
		g_pGameEntityInterface = gpGamedllFuncs->dllapi_table;
	}

	Module_CacheFunctions();

	CVAR_REGISTER(&init_amxmodx_version);
	CVAR_REGISTER(&init_amxmodx_modules);
	CVAR_REGISTER(&init_amxmodx_debug);
	CVAR_REGISTER(&init_amxmodx_mldebug);
	CVAR_REGISTER(&init_amxmodx_language);
	CVAR_REGISTER(&init_amxmodx_cl_langs);
	CVAR_REGISTER(&init_amxmodx_perflog);

	amxmodx_version = CVAR_GET_POINTER(init_amxmodx_version.name);
	amxmodx_debug = CVAR_GET_POINTER(init_amxmodx_debug.name);
	amxmodx_language = CVAR_GET_POINTER(init_amxmodx_language.name);
	amxmodx_perflog = CVAR_GET_POINTER(init_amxmodx_perflog.name);

	REG_SVR_COMMAND("amx", amx_command);

	char gameDir[512];
	GET_GAME_DIR(gameDir);
	char *a = gameDir;
	int i = 0;

	while (gameDir[i])
		if (gameDir[i++] ==	'/')
			a = &gameDir[i];

	g_mod_name = a;

	g_coloredmenus = ColoredMenus(g_mod_name.chars()); // whether or not to use colored menus

	// ###### Print short GPL
	print_srvconsole("\n   KTP AMX version %s (based on AMX Mod X)\n"
					 "   Copyright (c) 2004-2015 AMX Mod X Development Team, 2025-2026 KTP\n", AMXX_VERSION);
	print_srvconsole("   This is free software licensed under GPL v3.\n"
					 "   Type 'amx gpl' for details.\n  \n");

	// ###### Load custom path configuration
	Vault amx_config;
	amx_config.setSource(build_pathname("%s", get_localinfo("amxx_cfg", "addons/ktpamx/configs/core.ini")));

	if (amx_config.loadVault())
	{
		Vault::iterator	a =	amx_config.begin();

		while (a != amx_config.end())
		{
			SET_LOCALINFO((char*)a.key().chars(), (char*)a.value().chars());
			++a;
		}
		amx_config.clear();
	}

	// ###### Initialize logging here
	g_log_dir = get_localinfo("amxx_logs", "addons/ktpamx/logs");
	g_log.SetLogType("amxx_logging");

	// ###### Now attach metamod modules
	// This will also call modules Meta_Query and Meta_Attach functions
	loadModules(get_localinfo("amxx_modules", "addons/ktpamx/configs/modules.ini"), now);

	GET_HOOK_TABLES(PLID, &g_pEngTable, NULL, NULL);

	FlagMan.SetFile("cmdaccess.ini");

	ConfigManager.OnAmxxStartup();

	if (RehldsApi_Init())
	{
		RehldsHookchains->SV_DropClient()->registerHook(SV_DropClient_RH);
		g_isDropClientHookAvailable = true;
		g_isDropClientHookEnabled = true;
	}
	else
	{
		void *address = nullptr;

		if (CommonConfig && CommonConfig->GetMemSig("SV_DropClient", &address) && address)
		{
			DropClientDetour = DETOUR_CREATE_STATIC_FIXED(SV_DropClient, address);
			g_isDropClientHookAvailable = true;
			g_isDropClientHookEnabled = true;
		}
		else
		{
			auto reason = RehldsApi ? "update ReHLDS" : "check your gamedata files";
			AMXXLOG_Log("client_disconnected and client_remove forwards have been disabled - %s.", reason);
		}
	}

	g_CvarManager.CreateCvarHook();

	GET_IFACE<IFileSystem>("filesystem_stdio", g_FileSystem, FILESYSTEM_INTERFACE_VERSION);

	return (TRUE);
}

C_DLLEXPORT	int	Meta_Detach(PLUG_LOADTIME now, PL_UNLOAD_REASON	reason)
{
	if (now > Plugin_info.unloadable && reason != PNL_CMD_FORCED)
	{
		LOG_ERROR(PLID,	"Can't unload plugin right now");
		return (FALSE);
	}

	modules_callPluginsUnloading();

	g_auth.clear();
	g_frameActionMngr.clear();
	g_forwards.clear();
	g_commands.clear();
	g_forcemodels.clear();
	g_forcesounds.clear();
	g_forcegeneric.clear();
	g_bExtPrecacheProcessed = false;  // KTP: Reset for next map
	g_bInitDuringPrecache = false;  // KTP: Reset for next map
	g_grenades.clear();
	g_tasksMngr.clear();
	g_logevents.clearLogEvents();
	g_events.clearEvents();
	g_menucmds.clear();
	ClearMenus();
	g_vault.clear();
	g_xvars.clear();
	g_plugins.clear();
	g_langMngr.Clear();

	ArrayHandles.clear();
	TrieHandles.clear();
	TrieIterHandles.clear();
	TrieSnapshotHandles.clear();
	DataPackHandles.clear();
	TextParsersHandles.clear();
	GameConfigHandle.clear();

	ClearMessages();

	modules_callPluginsUnloaded();

	detachModules();

	g_log.CloseFile();
	g_log.AsyncShutdown();

	Module_UncacheFunctions();

	ClearLibraries(LibSource_Plugin);
	ClearLibraries(LibSource_Module);

	if (g_isDropClientHookAvailable)
	{
		if (RehldsApi)
		{
			if (g_isDropClientHookEnabled)
			{
				RehldsHookchains->SV_DropClient()->unregisterHook(SV_DropClient_RH);
			}
		}
		else
		{
			DropClientDetour->Destroy();
		}
		g_isDropClientHookAvailable = false;
		g_isDropClientHookEnabled = false;
	}

	return (TRUE);
}

// KTP: called by KTP-ReHLDS (3.22.0.928+) from ReleaseEntityDlls, before the
// extension dlclose loop. This is the extension-mode stand-in for Meta_Detach,
// which never runs without Metamod: without it dodx/reapi/amxxcurl get no
// AMXX_Detach, and their exit-time destructors later run against an unmapped
// core (the CHI1 shutdown-segfault class).
//
// Engine contract (sys_dll.cpp): Cmd_Shutdown / Cvar_Shutdown / NET_Shutdown
// have already run — nothing here may touch cvars, engine commands, or engine
// networking. That is also why no plugin forward fires from here: plugin_end
// runs arbitrary Pawn that is free to call cvar natives. On a changelevel-quit
// the final map's plugin_end already fired in SV_InactivateClients_RH; on a
// direct quit it stays unfired, as before — firing it safely needs an earlier
// engine hook, not this one.
C_DLLEXPORT void KTP_ExtensionShutdown(void)
{
	static bool s_extShutdownDone = false;
	if (s_extShutdownDone)
		return;
	s_extShutdownDone = true;

	// Attach never completed (e.g. fatal exit before SV_ActivateServer), or
	// we're under Metamod where Meta_Detach owns teardown: nothing to do.
	if (!g_bRehldsExtensionInit)
		return;

	// Make AlertMessage_RH and SV_Frame_RH early-out for the rest of teardown —
	// an at_logged alert here (amxx_logging 3, module MF_Log) would otherwise
	// run plugin_log Pawn code after the engine destroyed cvars and commands.
	// Order matters: the shutdown marker must be visible BEFORE the latch, or a
	// frame landing between the two sees a latch with no owner and the watchdog
	// would clear the suppression teardown depends on.
	g_bExtShuttingDown = true;
	g_bMapChangeInProgress = true;

	AMXXLOG_Log("[AMXX] Extension shutdown: detaching modules");

	// Same order as Meta_Detach: plugin-owned state, module notifications,
	// module detach, then logging last so modules can log from AMXX_Detach.
	modules_callPluginsUnloading();

	g_auth.clear();
	g_frameActionMngr.clear();
	g_forwards.clear();
	g_commands.clear();
	g_forcemodels.clear();
	g_forcesounds.clear();
	g_forcegeneric.clear();
	g_grenades.clear();
	g_tasksMngr.clear();
	g_logevents.clearLogEvents();
	g_events.clearEvents();
	g_menucmds.clear();
	ClearMenus();
	g_vault.clear();
	g_xvars.clear();
	g_plugins.clear();
	g_langMngr.Clear();

	ArrayHandles.clear();
	TrieHandles.clear();
	TrieIterHandles.clear();
	TrieSnapshotHandles.clear();
	DataPackHandles.clear();
	TextParsersHandles.clear();
	GameConfigHandle.clear();

	ClearMessages();

	modules_callPluginsUnloaded();

	// AMXX_Detach + dlclose per module while this core (and the engine) are
	// still mapped — module static destructors run here, not at process exit.
	detachModules();

	g_log.CloseFile();
	g_log.AsyncShutdown();

	Module_UncacheFunctions();

	ClearLibraries(LibSource_Plugin);
	ClearLibraries(LibSource_Module);
}

// KTP: Forward declarations for ReHLDS extension initialization
static void KTPAMX_InitAsRehldsExtension();
static void KTPAMX_ReloadPlugins();
static void SV_ActivateServer_RH(IRehldsHook_SV_ActivateServer *chain, int runPhysics);
static void SV_ClientCommand_RH(IRehldsHook_SV_ClientCommand *chain, edict_t *pEdict);
static void SV_InactivateClients_RH(IRehldsHook_SV_InactivateClients *chain);
static void AlertMessage_RH(IRehldsHook_AlertMessage *chain, ALERT_TYPE atype, const char *szMsg);

// KTP: Message ID capture system for extension mode
// We hook pfnRegUserMsg to capture message IDs as they're registered by the game DLL
// This allows us to look up IDs without calling REG_USER_MSG (which creates new messages)


static int PF_RegUserMsg_RH(IRehldsHook_PF_RegUserMsg_I *chain, const char *pszName, int iSize)
{
	// Call original function first to get the ID
	int id = chain->callNext(pszName, iSize);

	// Metamod resolves these through C_Spawn's REG_USER_MSG instead.
	if (g_bRunningWithMetamod)
		return id;

	// Capture the ID for messages we care about
	for (int i = 0; g_user_msg[i].name; ++i)
	{
		if (strcmp(g_user_msg[i].name, pszName) == 0)
		{
			*g_user_msg[i].id = id;

			// Set up message handlers if applicable
			if (!g_user_msg[i].cstrike || g_bmod_cstrike)
			{
				if (g_user_msg[i].endmsg)
					modMsgsEnd[id] = g_user_msg[i].func;
				else
					modMsgs[id] = g_user_msg[i].func;
			}
			break;
		}
	}

	return id;
}

C_DLLEXPORT void WINAPI GiveFnptrsToDll(enginefuncs_t* pengfuncsFromEngine, globalvars_t *pGlobals)
{
	memcpy(&g_engfuncs, pengfuncsFromEngine, sizeof(enginefuncs_t));
	gpGlobals = pGlobals;

	// KTP: Try to detect if we're loading as a ReHLDS extension (no Metamod)
	if (!g_bRunningWithMetamod && !g_bRehldsExtensionInit)
	{
		if (RehldsApi_Init())
		{
			RehldsHookchains->SV_ActivateServer()->registerHook(SV_ActivateServer_RH);

			// KTP: Hook pfnRegUserMsg to capture message IDs as they're registered
			// This is critical - we need to know message IDs without calling REG_USER_MSG
			// (which creates new messages if they don't exist)
			// KTP: Register hook for PF_RegUserMsg_I to capture message IDs
			RehldsHookchains->PF_RegUserMsg_I()->registerHook(PF_RegUserMsg_RH);
			RehldsHookchains->SV_ClientCommand()->registerHook(SV_ClientCommand_RH);

			// KTP: Hook SV_InactivateClients to run deactivation BEFORE clients are disconnected
			// This fires at the START of any map change sequence (rcon, game DLL, etc.)
			RehldsHookchains->SV_InactivateClients()->registerHook(SV_InactivateClients_RH);

			// KTP: Hook AlertMessage for register_logevent in extension mode
			RehldsHookchains->AlertMessage()->registerHook(AlertMessage_RH);

			// KTP: Hook PF_changelevel_I to set map change flag BEFORE any map change processing
			// This prevents crashes from hooks running with stale data during the transition
			if (RehldsHookchains->PF_changelevel_I())
				RehldsHookchains->PF_changelevel_I()->registerHook(PF_changelevel_I_RH);

			// KTP: Hook PF_precache_model_I to process force_unmodified during precache phase
			// This is CRITICAL - force_unmodified only works during spawn/precache
			if (RehldsHookchains->PF_precache_model_I())
				RehldsHookchains->PF_precache_model_I()->registerHook(PF_precache_model_I_RH);

			print_srvconsole("[KTP AMX] ReHLDS extension mode detected, will initialize on server activate.\n");
		}
		else
		{
			// Without this the version gate fails silently and the server runs on with
			// zero extension hooks registered - no forwards, no match handling.
			print_srvconsole("[KTP AMX] FATAL: ReHLDS API rejected (need >= %d.%d). Engine is older than this build; stage engine+core+reapi+dodx together.\n",
				REHLDS_API_VERSION_MAJOR, REHLDS_API_VERSION_MINOR);
		}
	}
}

DLL_FUNCTIONS gFunctionTable;
C_DLLEXPORT	int	GetEntityAPI2(DLL_FUNCTIONS *pFunctionTable, int *interfaceVersion)
{
	// Note: This function is only called when loaded via Metamod (as a plugin)
	// For ReHLDS extension mode, initialization happens via GiveFnptrsToDll -> SV_ActivateServer hook

	memset(&gFunctionTable, 0, sizeof(DLL_FUNCTIONS));

	gFunctionTable.pfnSpawn = C_Spawn;
	gFunctionTable.pfnClientCommand = C_ClientCommand;
	gFunctionTable.pfnServerDeactivate = C_ServerDeactivate;
	gFunctionTable.pfnClientDisconnect = C_ClientDisconnect;
	gFunctionTable.pfnInconsistentFile = C_InconsistentFile;
	gFunctionTable.pfnServerActivate = C_ServerActivate;
	gFunctionTable.pfnClientConnect = C_ClientConnect;

	memcpy(pFunctionTable, &gFunctionTable, sizeof(DLL_FUNCTIONS));

	return 1;
}

DLL_FUNCTIONS gFunctionTable_Post;
C_DLLEXPORT	int	GetEntityAPI2_Post(DLL_FUNCTIONS *pFunctionTable, int *interfaceVersion)
{
	memset(&gFunctionTable_Post, 0, sizeof(DLL_FUNCTIONS));

	gFunctionTable_Post.pfnClientPutInServer = C_ClientPutInServer_Post;
	gFunctionTable_Post.pfnClientUserInfoChanged = C_ClientUserInfoChanged_Post;
	gFunctionTable_Post.pfnServerActivate = C_ServerActivate_Post;
	gFunctionTable_Post.pfnClientConnect = C_ClientConnect_Post;
	gFunctionTable_Post.pfnStartFrame = C_StartFrame_Post;
	gFunctionTable_Post.pfnServerDeactivate = C_ServerDeactivate_Post;

	memcpy(pFunctionTable, &gFunctionTable_Post, sizeof(DLL_FUNCTIONS));

	return 1;
}

enginefuncs_t meta_engfuncs;

C_DLLEXPORT	int	GetEngineFunctions(enginefuncs_t *pengfuncsFromEngine, int *interfaceVersion)
{
	memset(&meta_engfuncs, 0, sizeof(enginefuncs_t));

	if (stricmp(g_mod_name.chars(), "cstrike") == 0 || stricmp(g_mod_name.chars(), "czero") == 0)
	{
		meta_engfuncs.pfnSetModel =	C_SetModel;
		g_bmod_cstrike = true;
	} else {
		g_bmod_cstrike  = false;
		g_bmod_dod      = !stricmp(g_mod_name.chars(), "dod");
		g_bmod_dmc      = !stricmp(g_mod_name.chars(), "dmc");
		g_bmod_tfc      = !stricmp(g_mod_name.chars(), "tfc");
		g_bmod_ricochet = !stricmp(g_mod_name.chars(), "ricochet");
		g_bmod_valve    = !stricmp(g_mod_name.chars(), "valve");
		g_bmod_gearbox  = !stricmp(g_mod_name.chars(), "gearbox");
	}

	g_official_mod = g_bmod_cstrike || g_bmod_dod || g_bmod_dmc || g_bmod_ricochet || g_bmod_tfc || g_bmod_valve || g_bmod_gearbox;

	meta_engfuncs.pfnCmd_Argc = C_Cmd_Argc;
	meta_engfuncs.pfnCmd_Argv = C_Cmd_Argv;
	meta_engfuncs.pfnCmd_Args = C_Cmd_Args;
	meta_engfuncs.pfnPrecacheModel = C_PrecacheModel;
	meta_engfuncs.pfnPrecacheSound = C_PrecacheSound;
	meta_engfuncs.pfnChangeLevel = C_ChangeLevel;

	/* message stuff from messages.h/cpp */
	meta_engfuncs.pfnMessageBegin = C_MessageBegin;
	meta_engfuncs.pfnMessageEnd = C_MessageEnd;
	meta_engfuncs.pfnWriteAngle = C_WriteAngle;
	meta_engfuncs.pfnWriteByte = C_WriteByte;
	meta_engfuncs.pfnWriteChar = C_WriteChar;
	meta_engfuncs.pfnWriteCoord = C_WriteCoord;
	meta_engfuncs.pfnWriteEntity = C_WriteEntity;
	meta_engfuncs.pfnWriteLong = C_WriteLong;
	meta_engfuncs.pfnWriteShort = C_WriteShort;
	meta_engfuncs.pfnWriteString = C_WriteString;

	meta_engfuncs.pfnAlertMessage = C_AlertMessage;

	memcpy(pengfuncsFromEngine, &meta_engfuncs, sizeof(enginefuncs_t));

	return 1;
}

enginefuncs_t meta_engfuncs_post;
C_DLLEXPORT	int	GetEngineFunctions_Post(enginefuncs_t *pengfuncsFromEngine,	int	*interfaceVersion)
{
	memset(&meta_engfuncs_post, 0, sizeof(enginefuncs_t));

	meta_engfuncs_post.pfnTraceLine = C_TraceLine_Post;
	meta_engfuncs_post.pfnMessageBegin = C_MessageBegin_Post;
	meta_engfuncs_post.pfnMessageEnd = C_MessageEnd_Post;
	meta_engfuncs_post.pfnWriteByte = C_WriteByte_Post;
	meta_engfuncs_post.pfnWriteChar = C_WriteChar_Post;
	meta_engfuncs_post.pfnWriteShort = C_WriteShort_Post;
	meta_engfuncs_post.pfnWriteLong = C_WriteLong_Post;
	meta_engfuncs_post.pfnWriteAngle = C_WriteAngle_Post;
	meta_engfuncs_post.pfnWriteCoord = C_WriteCoord_Post;
	meta_engfuncs_post.pfnWriteString = C_WriteString_Post;
	meta_engfuncs_post.pfnWriteEntity = C_WriteEntity_Post;
	meta_engfuncs_post.pfnRegUserMsg = C_RegUserMsg_Post;

	memcpy(pengfuncsFromEngine, &meta_engfuncs_post, sizeof(enginefuncs_t));

	return 1;
}

//quick hack - disable all newdll stuff for AMD64
// until VALVe gets their act together!
#if !defined AMD64
NEW_DLL_FUNCTIONS gNewDLLFunctionTable;
C_DLLEXPORT int GetNewDLLFunctions(NEW_DLL_FUNCTIONS *pNewFunctionTable, int *interfaceVersion)
{
	memset(&gNewDLLFunctionTable, 0, sizeof(NEW_DLL_FUNCTIONS));

	// default metamod does not call this if the gamedll doesn't provide it
	if (g_engfuncs.pfnQueryClientCvarValue2)
	{
		gNewDLLFunctionTable.pfnCvarValue2 = C_CvarValue2;
		g_NewDLL_Available = true;
	}

	// KTP Custom: Hook real-time client cvar change notification from ReHLDS
	// This provides instant cvar validation without periodic polling
	gNewDLLFunctionTable.pfnClientCvarChanged = C_ClientCvarChanged;

	memcpy(pNewFunctionTable, &gNewDLLFunctionTable, sizeof(NEW_DLL_FUNCTIONS));

	return 1;
}
#endif

// ============================================================================
// KTP: ReHLDS Extension Loading Support
// ============================================================================
// When running without Metamod (loaded via extensions.ini), we need to handle
// initialization ourselves via ReHLDS hookchains.

// g_bMapChangeInProgress is declared earlier (near line 148) for use by SV_Frame_RH

// ReHLDS hook for SV_ActivateServer - called when server activates
static void SV_ActivateServer_RH(IRehldsHook_SV_ActivateServer *chain, int runPhysics)
{
	// Only handle extension mode
	if (g_bRunningWithMetamod)
	{
		chain->callNext(runPhysics);
		return;
	}

	// KTP: Call chain->callNext() FIRST to let ReHLDS merge sv_gpNewUserMsgs into sv_gpUserMsgs.
	//
	// The game DLL registers user messages during GameDLLInit/ServerActivate, which go into
	// sv_gpNewUserMsgs. REG_USER_MSG lookups only check sv_gpUserMsgs, so if we init BEFORE
	// the merge, our lookups create DUPLICATE message IDs instead of finding existing ones.
	// This causes "UserMsg: Not Present on Client XX" errors.
	//
	// On initial server start: No clients are connected, so SV_ActivateServer_internal's
	// client loop (which sends user messages) does nothing. Clients connecting AFTER
	// activation will receive the complete message list via SV_New_f.
	//
	// On map change: Existing clients get SV_BuildReconnect which triggers a full reconnect,
	// so they also receive the complete message list.

	// Call chain first - this activates the server and merges message lists
	chain->callNext(runPhysics);

	// If we haven't initialized yet, do extension init now
	if (!g_bRehldsExtensionInit)
	{
		KTPAMX_InitAsRehldsExtension();
		// KTP: Clear the map change flag - on first init this is NOT a map change
		// SV_InactivateClients_RH may have set it, but we're just starting up
		g_bMapChangeInProgress = false;
		g_bInitDuringPrecache = false;  // Clear flag for next map
		return;
	}

	// KTP: If init was done during precache, finish initialization now
	// plugin_init/plugin_cfg were deferred because game state wasn't ready during precache
	if (g_bInitDuringPrecache)
	{
		g_bInitDuringPrecache = false;  // Clear flag

		// Execute amxx.cfg before plugin_init/plugin_cfg (matching Metamod mode)
		CoreCfg.ExecuteMainConfig();

		// Execute plugin_init forwards
		executeForwards(FF_PluginInit);

		// Execute plugin_cfg
		executeForwards(FF_PluginCfg);
		CoreCfg.ExecuteAutoConfigs();
		CoreCfg.SetMapConfigTimer(6.1);

		// Reset task time to enable task execution
		g_task_time = gpGlobals->time;
		g_auth_time = gpGlobals->time;

		// Correct time in Counter-Strike and other mods (except DOD)
		if (!g_bmod_dod)
			g_game_timeleft = 0;

		// g_activated was set by KTPAMX_InitAsRehldsExtension; this path only
		// finishes the deferred plugin_init/plugin_cfg.

		print_srvconsole("[KTP AMX] Completed initialization (plugin_init/plugin_cfg executed).\n");
		AMXXLOG_Log("KTP AMX initialization completed - SV_ActivateServer phase");

		g_bMapChangeInProgress = false;
		return;
	}

	// Check if this is a map change (deactivation was already done in SV_InactivateClients_RH)
	if (g_bMapChangeInProgress)
	{
		// Re-initialize for the new map
		KTPAMX_ReloadPlugins();
		g_bMapChangeInProgress = false;
	}
}

// KTP: Hook for SV_InactivateClients - runs deactivation at start of map change
// This mimics what Metamod does in C_ServerDeactivate, which is called by SV_ServerShutdown
// We hook here because in extension mode, the engine calls game DLL's ServerDeactivate, not ours
static void SV_InactivateClients_RH(IRehldsHook_SV_InactivateClients *chain)
{
	// Only handle extension mode
	if (g_bRunningWithMetamod)
	{
		chain->callNext();
		return;
	}

	// KTP: Set map change flag FIRST before any processing
	g_bMapChangeInProgress = true;

	// Only run deactivation if we're initialized and activated
	if (g_bRehldsExtensionInit && g_activated)
	{
		// KTP: Fire client disconnect forwards so plugins can clean up per-player state.
		// Game state (edicts, player data) is still valid here — chain->callNext() hasn't
		// run yet. This mirrors C_ServerDeactivate in Metamod mode (meta_api.cpp:754).
		for (int i = 1; i <= gpGlobals->maxClients; ++i)
		{
			CPlayer *pPlayer = GET_PLAYER_POINTER_I(i);

			if (pPlayer->initialized)
			{
				executeForwards(FF_ClientDisconnect, static_cast<cell>(pPlayer->index));

				if (g_isDropClientHookAvailable && !pPlayer->disconnecting)
				{
					executeForwards(FF_ClientDisconnected, static_cast<cell>(pPlayer->index), FALSE, prepareCharArray(const_cast<char*>(""), 0), 0);
				}
			}

			if (pPlayer->ingame)
			{
				auto wasDisconnecting = pPlayer->disconnecting;

				// Clear AMXX internal state (doesn't kick the player)
				pPlayer->Disconnect();

				if (!wasDisconnecting && g_isDropClientHookAvailable)
				{
					executeForwards(FF_ClientRemove, static_cast<cell>(pPlayer->index), FALSE, const_cast<char*>(""));
				}
			}
		}

		// Disable DropClient hook during map transition
		if (g_isDropClientHookAvailable && g_isDropClientHookEnabled)
		{
			if (RehldsApi)
			{
				RehldsHookchains->SV_DropClient()->unregisterHook(SV_DropClient_RH);
			}
			else if (DropClientDetour)
			{
				DropClientDetour->DisableDetour();
			}
			g_isDropClientHookEnabled = false;
		}

		g_players_num = 0;

		// KTP: Fire plugin_end so plugins can persist state, close handles, etc.
		executeForwards(FF_PluginEnd);

		// KTP: Reset precache flag and clear force lists for next map
		// In Metamod mode this happens in C_ServerDeactivate_Post, but that doesn't
		// get called in extension mode. Without this, plugin_precache won't fire on
		// map changes and force_unmodified() won't work after the first map.
		g_bExtPrecacheProcessed = false;
		g_forcemodels.clear();
		g_forcesounds.clear();
		g_forcegeneric.clear();
	}

	// Continue with client inactivation
	chain->callNext();
}

// KTP: Hook for AlertMessage - handles register_logevent in extension mode
// In Metamod mode, C_AlertMessage hooks pfnAlertMessage via meta_engfuncs
// In extension mode, we hook the AlertMessage function directly via ReHLDS hookchain
//
// KNOWN BEHAVIORAL DIFFERENCE (Extension vs Metamod mode):
// In Metamod mode, the plugin_log forward's return value controls log suppression —
// returning PLUGIN_HANDLED from plugin_log causes RETURN_META(MRES_SUPERCEDE), which
// prevents the engine from writing the log line (see C_AlertMessage above).
//
// In extension mode, we call chain->callNext() BEFORE executing plugin_log, so the
// log line has already been written by the time the forward fires. The return value
// is captured but cannot suppress the log. This means plugins that rely on plugin_log
// to filter log output (e.g., KTPMatchHandler filtering DoD log events from HLTV relay)
// will NOT suppress logs in extension mode.
//
// This is an inherent limitation of the ReHLDS hookchain API — AlertMessage is a void
// function with no pre-hook mechanism to prevent the log write. A workaround would
// require a KTP-ReHLDS engine change to support pre-hook suppression.
static void AlertMessage_RH(IRehldsHook_AlertMessage *chain, ALERT_TYPE atype, const char *szMsg)
{
	// Call the original first (log is written here — cannot be suppressed after this point)
	chain->callNext(atype, szMsg);

	// Metamod mode runs logevents through C_AlertMessage; see SV_ClientCommand_RH.
	if (g_bRunningWithMetamod)
		return;

	// KTP: Skip log event processing during map change to prevent crashes
	if (g_bMapChangeInProgress)
		return;

	// Only process logged messages (same check as C_AlertMessage)
	if (atype != at_logged)
		return;

	// Execute logevents and plugin_log forward
	// Note: retVal is intentionally unused — see behavioral difference note above
	if (g_logevents.logEventsExist() || g_forwards.getFuncsNum(FF_PluginLog))
	{
		g_logevents.setLogString("%s", szMsg);
		g_logevents.parseLogString();

		if (g_logevents.logEventsExist())
		{
			g_logevents.executeLogEvents();
		}

		executeForwards(FF_PluginLog);
	}
}

// KTP: Handle map change activation - extension mode only
// This mimics what C_ServerActivate_Post does in Metamod mode
static void KTPAMX_ReloadPlugins()
{
	// Metamod mode runs g_log.MapChange() from C_Spawn every map; the extension
	// port never did, so per-map log rotation (amxx_logging 2), the async-mode
	// re-latch, and the drop-counter report only work if it runs here too.
	g_log.MapChange();

	// Same story, same hook: C_Spawn reloads cmdaccess.ini every map so an edit
	// takes effect on the next map change, which is what the file's own header
	// tells admins to do. Unforced, so an unchanged file is a cheap no-op.
	FlagMan.LoadFile();

	// Clear tasks so they don't fire with stale data
	g_tasksMngr.clear();

	// Clear frame actions from previous map
	g_frameActionMngr.clear();

	// Clear menus from previous map (prevents stale menu handler references)
	ClearMenus();

	// Re-initialize task manager timers for new map
	g_game_timeleft = g_bmod_dod ? 1.0f : 0.0f;
	g_tasksMngr.registerTimers(&gpGlobals->time, &mp_timelimit->value, &g_game_timeleft);

	// Re-initialize all player slots (like C_ServerActivate_Post does)
	for (int i = 1; i <= gpGlobals->maxClients; ++i)
	{
		CPlayer *pPlayer = GET_PLAYER_POINTER_I(i);
		edict_t *pEdict = INDEXENT(i);
		pPlayer->Init(pEdict, i);
	}

	// KTP: Notify modules to clean up plugin-specific state before re-init.
	// This allows ReAPI to clear hookchain vectors (100% plugin-owned) so hooks
	// don't accumulate on each map change. Module-owned state (registered during
	// AMXX_Attach) is preserved because modules handle their own cleanup.
	modules_callPluginsUnloading();

	// KTP: Clear data handles to prevent unbounded memory growth.
	// Plugins recreate these in plugin_init each map change. Without clearing,
	// old handles leak indefinitely (ArrayCreate, TrieCreate, etc. accumulate).
	ArrayHandles.clear();
	TrieHandles.clear();
	TrieIterHandles.clear();
	TrieSnapshotHandles.clear();
	DataPackHandles.clear();
	TextParsersHandles.clear();
	GameConfigHandle.clear();

	// KTP: Clear HUD sync objects (leak cell[] arrays each map change)
	for (unsigned int i = 0; i < g_hudsync.length(); i++)
		delete [] g_hudsync[i];
	g_hudsync.clear();

	// KTP: Flush dynamic admins (leak CAdminData objects each map change)
	for (size_t iter = DynamicAdmins.length(); iter--; )
		delete DynamicAdmins[iter];
	DynamicAdmins.clear();

	// KTP: Clear cvar hooks/binds from previous map's plugins
	g_CvarManager.OnPluginUnloaded();

	// KTP: Clear plugin-owned state that doesn't have dedup protection.
	// These mirror what C_ServerDeactivate_Post does in Metamod mode.
	// Items with dedup (events, log events, commands, menus, forwards) are safe
	// to skip — they return existing handles on re-registration.
	g_xvars.clear();
	// Reload, don't just clear: Metamod pairs the C_ServerDeactivate_Post clear
	// with a C_Spawn loadVault every map. Clearing alone made get_vaultdata read
	// empty after the first map change while vault.ini still held the value.
	g_vault.clear();
	g_vault.loadVault();
	// KTP: DO NOT call ClearPluginLibraries() here!
	// It munmap's dynamic native thunks (register_native), but plugin_natives()
	// is NOT re-called during KTPAMX_ReloadPlugins (only plugin_init/plugin_cfg).
	// The calling plugin's AMX native table retains the old thunk pointer →
	// use-after-free → segfault at page-aligned address on next native call.
	// Dynamic natives persist safely since the AMX instances are not reloaded.
	g_grenades.clear();
	g_auth.clear();
	g_putinserver_mask = 0;

	// Execute plugin_init and plugin_cfg for the new map
	// Plugins are still loaded, we just fire the forwards so they can reinitialize
	executeForwards(FF_PluginInit);
	executeForwards(FF_PluginCfg);

	// Correct time in Counter-Strike and other mods (except DOD)
	if (!g_bmod_dod)
		g_game_timeleft = 0;

	// Reset task and auth time to enable execution
	g_task_time = gpGlobals->time;
	g_auth_time = gpGlobals->time;

	// Re-enable DropClient hook for the new map
	if (g_isDropClientHookAvailable && !g_isDropClientHookEnabled)
	{
		if (RehldsApi)
		{
			RehldsHookchains->SV_DropClient()->registerHook(SV_DropClient_RH);
		}
		else if (DropClientDetour)
		{
			DropClientDetour->EnableDetour();
		}
		g_isDropClientHookEnabled = true;
	}
}

// Initialize KTP AMX as a ReHLDS extension (no Metamod)
static void KTPAMX_InitAsRehldsExtension()
{
	if (g_bRehldsExtensionInit)
		return;

	g_bRehldsExtensionInit = true;

	// KTP: Initialize game entity interface from ReHLDS
	// This allows calling game DLL functions (ClientKill, etc.) without Metamod
	if (RehldsFuncs && RehldsFuncs->GetEntityInterface)
	{
		g_pGameEntityInterface = RehldsFuncs->GetEntityInterface();
	}

	// Initialize module cache
	Module_CacheFunctions();

	// Register cvars
	CVAR_REGISTER(&init_amxmodx_version);
	CVAR_REGISTER(&init_amxmodx_modules);
	CVAR_REGISTER(&init_amxmodx_debug);
	CVAR_REGISTER(&init_amxmodx_mldebug);
	CVAR_REGISTER(&init_amxmodx_language);
	CVAR_REGISTER(&init_amxmodx_cl_langs);
	CVAR_REGISTER(&init_amxmodx_perflog);

	amxmodx_version = CVAR_GET_POINTER(init_amxmodx_version.name);
	amxmodx_debug = CVAR_GET_POINTER(init_amxmodx_debug.name);
	amxmodx_language = CVAR_GET_POINTER(init_amxmodx_language.name);
	amxmodx_perflog = CVAR_GET_POINTER(init_amxmodx_perflog.name);

	// Register amx command
	REG_SVR_COMMAND("amx", amx_command);

	// Get game directory name
	char gameDir[512];
	GET_GAME_DIR(gameDir);
	char *a = gameDir;
	int i = 0;

	while (gameDir[i])
		if (gameDir[i++] == '/')
			a = &gameDir[i];

	g_mod_name = a;

	// KTP: Initialize mod detection (extension mode) - equivalent to C_ServerActivate for Metamod
	if (stricmp(g_mod_name.chars(), "cstrike") == 0 || stricmp(g_mod_name.chars(), "czero") == 0)
	{
		g_bmod_cstrike = true;
	} else {
		g_bmod_cstrike = false;
		g_bmod_dod      = !stricmp(g_mod_name.chars(), "dod");
		g_bmod_dmc      = !stricmp(g_mod_name.chars(), "dmc");
		g_bmod_tfc      = !stricmp(g_mod_name.chars(), "tfc");
		g_bmod_ricochet = !stricmp(g_mod_name.chars(), "ricochet");
		g_bmod_valve    = !stricmp(g_mod_name.chars(), "valve");
		g_bmod_gearbox  = !stricmp(g_mod_name.chars(), "gearbox");
	}
	g_official_mod = g_bmod_cstrike || g_bmod_dod || g_bmod_dmc || g_bmod_ricochet || g_bmod_tfc || g_bmod_valve || g_bmod_gearbox;

	g_coloredmenus = ColoredMenus(g_mod_name.chars());

	// Print startup message
	print_srvconsole("\n   KTP AMX version %s (ReHLDS Extension Mode)\n"
					 "   Copyright (c) 2004-2015 AMX Mod X Development Team, 2025-2026 KTP\n", AMXX_VERSION);
	print_srvconsole("   Running without Metamod - using ReHLDS hookchains.\n\n");

	// Load custom path configuration
	Vault amx_config;
	amx_config.setSource(build_pathname("%s", get_localinfo("amxx_cfg", "addons/ktpamx/configs/core.ini")));

	if (amx_config.loadVault())
	{
		Vault::iterator iter = amx_config.begin();
		while (iter != amx_config.end())
		{
			SET_LOCALINFO((char*)iter.key().chars(), (char*)iter.value().chars());
			++iter;
		}
		amx_config.clear();
	}

	// Initialize logging. MapChange() rather than bare SetLogType(): Metamod
	// mode runs it via C_Spawn on the first map too — it creates the log dir,
	// latches amxx_log_async, and starts the per-map file for amxx_logging 2.
	g_log_dir = get_localinfo("amxx_logs", "addons/ktpamx/logs");
	g_log.MapChange();

	// Load modules - in extension mode, modules that require Metamod hooks won't fully work,
	// but pure AMXX modules and modules using Re* API hookchains (like ReAPI) will work
	loadModules(get_localinfo("amxx_modules", "addons/ktpamx/configs/modules.ini"), PT_ANYTIME);

	FlagMan.SetFile("cmdaccess.ini");
	// LoadFile too, not just SetFile: its only other caller is C_Spawn, which
	// never runs in extension mode, so cmdaccess.ini was named but never parsed
	// and every rule in it was silently discarded. Same first-map/every-map
	// split as g_log.MapChange() above -- KTPAMX_ReloadPlugins() does the rest.
	FlagMan.LoadFile();

	ConfigManager.OnAmxxStartup();

	// Setup ReHLDS hooks (already have ReHLDS API since we're an extension)
	RehldsHookchains->SV_DropClient()->registerHook(SV_DropClient_RH);
	g_isDropClientHookAvailable = true;
	g_isDropClientHookEnabled = true;

	// KTP: Register SV_CheckConsistencyResponse hook for inconsistent_file forward
	RehldsHookchains->SV_CheckConsistencyResponse()->registerHook(SV_CheckConsistencyResponse_RH);

	// KTP: Register ClientConnected hook for client_connect and client_connectex forwards
	RehldsHookchains->ClientConnected()->registerHook(ClientConnected_RH);

	// KTP: Register Steam_NotifyClientConnect hook for client_authorized forward
	RehldsHookchains->Steam_NotifyClientConnect()->registerHook(Steam_NotifyClientConnect_RH);

	// KTP: Register SV_ClientUserInfoChanged hook for client_infochanged forward
	// and to keep CPlayer::name in sync with the engine on userinfo changes.
	RehldsHookchains->SV_ClientUserInfoChanged()->registerHook(SV_ClientUserInfoChanged_RH);

	// Steam_GSBUpdateUserData and ExecuteServerStringCmd are deliberately NOT hooked:
	// both were pure pass-throughs, and SV_ClientUserInfoChanged (2.7.16) now covers
	// the niche the GSB one was reserved for. See CHANGELOG 2.6.x.

	// KTP: Register SV_Frame hook for per-frame processing (client_putinserver forwards, etc)
	RehldsHookchains->SV_Frame()->registerHook(SV_Frame_RH);

	// KTP: Register SV_Spawn_f hook for client_putinserver forward on map change reconnect
	// During map change, clients don't go through ClientConnected, so SV_Spawn_f handles initialization
	RehldsHookchains->SV_Spawn_f()->registerHook(SV_Spawn_f_RH);

	g_CvarManager.CreateCvarHook();

	GET_IFACE<IFileSystem>("filesystem_stdio", g_FileSystem, FILESYSTEM_INTERFACE_VERSION);

	// KTP: Enable query_client_cvar in extension mode if engine supports it
	// This is normally set in GetNewDLLFunctions which Metamod calls, but in extension mode
	// we need to check and set it ourselves
	if (g_engfuncs.pfnQueryClientCvarValue2)
	{
		g_NewDLL_Available = true;
	}

	// KTP: Load plugins (equivalent to ServerActivate in Metamod mode)
	char map_pluginsfile_path[256];
	char prefixed_map_pluginsfile[256];
	char configs_dir[256];

	get_localinfo_r("amxx_configsdir", "addons/ktpamx/configs", configs_dir, sizeof(configs_dir)-1);

	const char *plugins_file = get_localinfo("amxx_plugins", "addons/ktpamx/configs/plugins.ini");

	g_plugins.CALMFromFile(plugins_file);

	LoadExtraPluginsToPCALM(configs_dir);

	char temporaryMap[64], *tmap_ptr;
	ke::SafeSprintf(temporaryMap, sizeof(temporaryMap), "%s", STRING(gpGlobals->mapname));

	prefixed_map_pluginsfile[0] = '\0';
	if ((tmap_ptr = strchr(temporaryMap, '_')) != NULL)
	{
		*tmap_ptr = '\0';
		ke::SafeSprintf(prefixed_map_pluginsfile,
			sizeof(prefixed_map_pluginsfile),
			"%s/maps/plugins-%s.ini",
			configs_dir,
			temporaryMap);
		g_plugins.CALMFromFile(prefixed_map_pluginsfile);
	}

	ke::SafeSprintf(map_pluginsfile_path,
		sizeof(map_pluginsfile_path),
		"%s/maps/plugins-%s.ini",
		configs_dir,
		STRING(gpGlobals->mapname));
	g_plugins.CALMFromFile(map_pluginsfile_path);

	int loaded = countModules(CountModules_Running);
	CVAR_SET_STRING(init_amxmodx_version.name, AMXX_VERSION);
	char buffer[32];
	sprintf(buffer, "%d", loaded);
	CVAR_SET_STRING(init_amxmodx_modules.name, buffer);

	// Same C_Spawn-only story as the vault and cmdaccess below: without these the
	// prefix lists stay empty, registerCmdPrefix always fails, and every client
	// command dispatch degrades to a linear scan of the flat clcmdlist. Must run
	// before plugins register anything, and longest-prefix-first — findPrefix
	// matches on the stored prefix's length, so a bare "say" swallows "say_team".
	g_commands.registerPrefix("say_team");
	g_commands.registerPrefix("say");
	g_commands.registerPrefix("amxx");
	g_commands.registerPrefix("amx");
	g_commands.registerPrefix("admin_");
	g_commands.registerPrefix("sm_");
	g_commands.registerPrefix("cm_");

	// Load Vault
	char file[PLATFORM_MAX_PATH];
	g_vault.setSource(build_pathname_r(file, sizeof(file), "%s", get_localinfo("amxx_vault", "addons/ktpamx/configs/vault.ini")));
	g_vault.loadVault();

	// Init time and freeze tasks
	g_game_timeleft = g_bmod_dod ? 1.0f : 0.0f;
	g_task_time = gpGlobals->time + 99999.0f;
	g_auth_time = gpGlobals->time + 99999.0f;
	g_players_num = 0;

	// KTP: Initialize mp_timelimit pointer for task manager (same as C_Spawn does for Metamod mode)
	mp_timelimit = CVAR_GET_POINTER("mp_timelimit");
	if (mp_timelimit == NULL)
	{
		// Some mods don't have mp_timelimit, create a holder
		static cvar_t timelimit_holder_ext;
		timelimit_holder_ext.name = "mp_timelimit";
		timelimit_holder_ext.string = "0";
		timelimit_holder_ext.flags = 0;
		timelimit_holder_ext.value = 0.0;
		CVAR_REGISTER(&timelimit_holder_ext);
		mp_timelimit = &timelimit_holder_ext;
	}

	// KTP: Metamod caches this in C_Spawn, which never runs in extension mode — leaving
	// hostname NULL, so get_user_name(0)/show_motd() would deref a NULL cvar and crash.
	hostname = CVAR_GET_POINTER("hostname");

	// KTP: Initialize task manager timers - CRITICAL for set_task to work!
	g_tasksMngr.registerTimers(&gpGlobals->time, &mp_timelimit->value, &g_game_timeleft);

	// Set server flags
	memset(g_players[0].flags, -1, sizeof(g_players[0].flags));

	g_opt_level = atoi(get_localinfo("optimizer", "7"));
	if (!g_opt_level)
		g_opt_level = 7;

	// Load AMX Mod X plugins
	g_plugins.loadPluginsFromFile(get_localinfo("amxx_plugins", "addons/ktpamx/configs/plugins.ini"));

	LoadExtraPluginsFromDir(configs_dir);
	g_plugins.loadPluginsFromFile(map_pluginsfile_path, false);
	if (prefixed_map_pluginsfile[0] != '\0')
	{
		g_plugins.loadPluginsFromFile(prefixed_map_pluginsfile, false);
	}

	g_plugins.Finalize();
	g_plugins.InvalidateCache();

	// Register forwards (FF_PluginPrecache may already be registered from early load)
	FF_PluginInit = registerForward("plugin_init", ET_IGNORE, FP_DONE);
	FF_ClientCommand = registerForward("client_command", ET_STOP, FP_CELL, FP_DONE);
	FF_ClientConnect = registerForward("client_connect", ET_IGNORE, FP_CELL, FP_DONE);
	FF_ClientDisconnect = registerForward("client_disconnect", ET_IGNORE, FP_CELL, FP_DONE);
	FF_ClientDisconnected = registerForward("client_disconnected", ET_IGNORE, FP_CELL, FP_CELL, FP_ARRAY, FP_CELL, FP_DONE);
	FF_ClientRemove = registerForward("client_remove", ET_IGNORE, FP_CELL, FP_CELL, FP_STRING, FP_DONE);
	FF_ClientInfoChanged = registerForward("client_infochanged", ET_IGNORE, FP_CELL, FP_DONE);
	FF_ClientCvarChanged = registerForward("client_cvar_changed", ET_IGNORE, FP_CELL, FP_STRING, FP_STRING, FP_DONE);
	FF_ClientPutInServer = registerForward("client_putinserver", ET_IGNORE, FP_CELL, FP_DONE);
	FF_PluginCfg = registerForward("plugin_cfg", ET_IGNORE, FP_DONE);
	if (FF_PluginPrecache < 0)  // Only register if not already done during early load
		FF_PluginPrecache = registerForward("plugin_precache", ET_IGNORE, FP_DONE);
	FF_PluginLog = registerForward("plugin_log", ET_STOP, FP_DONE);
	FF_PluginEnd = registerForward("plugin_end", ET_IGNORE, FP_DONE);
	FF_InconsistentFile = registerForward("inconsistent_file", ET_STOP, FP_CELL, FP_STRING, FP_STRINGEX, FP_DONE);
	FF_ClientAuthorized = registerForward("client_authorized", ET_IGNORE, FP_CELL, FP_STRING, FP_DONE);
	FF_ChangeLevel = registerForward("server_changelevel", ET_STOP, FP_STRING, FP_DONE);
	FF_ClientConnectEx = registerForward("client_connectex", ET_STOP, FP_CELL, FP_STRING, FP_STRING, FP_ARRAY, FP_DONE);

	CoreCfg.OnAmxxInitialized();

	// Notify modules that plugins are loaded
	modules_callPluginsLoaded();

	// KTP: Initialize player slots (equivalent to C_ServerActivate_Post for Metamod mode)
	// This is critical - without this, player data will have garbage values
	for (int i = 1; i <= gpGlobals->maxClients; ++i)
	{
		CPlayer *pPlayer = GET_PLAYER_POINTER_I(i);
		edict_t *pEdict = INDEXENT(i);
		pPlayer->Init(pEdict, i);
	}

	// KTP: Look up message IDs using REG_USER_MSG - since the game DLL has already registered
	// these messages, REG_USER_MSG will return the existing IDs (not create new ones)
	for (int i = 0; g_user_msg[i].name; ++i)
	{
		// Skip CS-only messages if not running CS
		if (g_user_msg[i].cstrike && !g_bmod_cstrike)
			continue;

		// REG_USER_MSG returns existing ID if message already registered, or creates new one
		// Since game DLL registered these during GameDLLInit, we'll get existing IDs
		int id = REG_USER_MSG(g_user_msg[i].name, -1);
		if (id > 0)
		{
			*g_user_msg[i].id = id;

			// Set up message handlers
			if (g_user_msg[i].func)
			{
				if (g_user_msg[i].endmsg)
					modMsgsEnd[id] = g_user_msg[i].func;
				else
					modMsgs[id] = g_user_msg[i].func;
			}
		}
		else
		{
			AMXXLOG_Log("[KTP AMX] Warning: Message '%s' not found (id=%d)", g_user_msg[i].name, id);
		}
	}

	// Initialize type conversion
	TypeConversion.init();

	// The one place this is set in extension mode. It gates the teardown/hook paths
	// (e.g. SV_InactivateClients_RH), not plugin readiness — so it is deliberately
	// true before plugin_init, including on the deferred-precache path below.
	g_activated = true;
	g_initialized = true;

	// KTP: plugin_precache and force_unmodified processing now happens in PF_precache_model_I_RH
	// which fires during the actual precache phase. Only run it here as fallback if the hook didn't fire.
	if (!g_bExtPrecacheProcessed)
	{
		g_dontprecache = false;
		executeForwards(FF_PluginPrecache);
		g_dontprecache = true;
		// Note: force_unmodified won't work here - too late for ENGINE_FORCE_UNMODIFIED
	}

	// KTP: If init was called during precache phase, defer plugin_init/plugin_cfg to SV_ActivateServer
	// These forwards need game state that isn't fully ready during precache.
	if (g_bInitDuringPrecache)
	{
		print_srvconsole("[KTP AMX] Loaded %d plugin(s) during precache (plugin_init deferred).\n", g_plugins.getPluginsNum());
		AMXXLOG_Log("KTP AMX initialized as ReHLDS extension (no Metamod) - precache phase");
		return;
	}

	// KTP: Execute amxx.cfg before plugin_init/plugin_cfg (matching Metamod mode)
	CoreCfg.ExecuteMainConfig();

	// Execute plugin_init forwards
	executeForwards(FF_PluginInit);

	// KTP: Execute plugin_cfg directly (same as KTPAMX_ReloadPlugins does on map change)
	// This matches how Metamod mode does it in C_ServerActivate_Post
	executeForwards(FF_PluginCfg);
	CoreCfg.ExecuteAutoConfigs();
	CoreCfg.SetMapConfigTimer(6.1);
	print_srvconsole("[KTP AMX] Loaded %d plugin(s).\n", g_plugins.getPluginsNum());

	// KTP: Reset task time to enable task execution
	// In Metamod mode this happens in C_ServerActivate_Post, but in extension mode
	// we need to do it here. Without this, g_task_time stays at gpGlobals->time + 99999
	// and tasks never fire.
	g_task_time = gpGlobals->time;
	g_auth_time = gpGlobals->time;

	// Correct time in Counter-Strike and other mods (except DOD)
	if (!g_bmod_dod)
		g_game_timeleft = 0;

	AMXXLOG_Log("KTP AMX initialized as ReHLDS extension (no Metamod)");
}

// KTP: ReHLDS hookchain handler for SV_ClientCommand
// This is the extension mode equivalent of C_ClientCommand
// Instead of RETURN_META(MRES_SUPERCEDE), we simply return without calling chain->callNext
static void SV_ClientCommand_RH(IRehldsHook_SV_ClientCommand *chain, edict_t *pEdict)
{
	// These hooks register in GiveFnptrsToDll, which Metamod calls BEFORE
	// Meta_Attach — so under a Metamod+ReHLDS load they'd be live alongside
	// C_ClientCommand and every command would process twice. Same passthrough
	// SV_ActivateServer_RH/SV_InactivateClients_RH already carry.
	if (g_bRunningWithMetamod)
	{
		chain->callNext(pEdict);
		return;
	}

	// KTP: Skip client command processing during map change to prevent crashes
	if (g_bMapChangeInProgress)
	{
		chain->callNext(pEdict);
		return;
	}

	CPlayer *pPlayer = GET_PLAYER_POINTER(pEdict);

	bool supercede = false;
	cell ret = 0;

	const char* cmd = CMD_ARGV(0);
	const char* arg = CMD_ARGV(1);

	// Handle "amx" command if on dedicated server
	if (IS_DEDICATED_SERVER())
	{
		if (cmd && stricmp(cmd, "amx") == 0)
		{
			// Print version
			static char buf[1024];
			size_t len = 0;

			sprintf(buf, "%s %s\n", Plugin_info.name, Plugin_info.version);
			CLIENT_PRINT(pEdict, print_console, buf);
			len = sprintf(buf, "Author: Tony 'Nein_' (https://github.com/afraznein)\n");
			CLIENT_PRINT(pEdict, print_console, buf);
			len = sprintf(buf, "Based on AMX Mod X by:\n         David \"BAILOPAN\" Anderson, Pavol \"PM OnoTo\" Marko, Felix \"SniperBeamer\" Geyer\n");
			len += sprintf(&buf[len], "         Jonny \"Got His Gun\" Bergstrom, Lukasz \"SidLuke\" Wlasinski\n");
			CLIENT_PRINT(pEdict, print_console, buf);
			len = sprintf(buf, "         Christian \"Basic-Master\" Hammacher, Borja \"faluco\" Ferrer\n");
			len += sprintf(&buf[len], "         Scott \"DS\" Ehlert\n");
			len += sprintf(&buf[len], "Compiled: %s\nURL: https://github.com/afraznein/KTPAMXX\n", __DATE__ ", " __TIME__);
			CLIENT_PRINT(pEdict, print_console, buf);
#ifdef JIT
			sprintf(buf, "Core mode: JIT\n");
#else
#ifdef ASM32
			sprintf(buf, "Core mode: ASM\n");
#else
			sprintf(buf, "Core mode: Normal\n");
#endif
#endif
			CLIENT_PRINT(pEdict, print_console, buf);
			return; // Supercede - don't call chain->callNext
		}
	}

	// Execute client_command forward
	if (executeForwards(FF_ClientCommand, static_cast<cell>(pPlayer->index)) > 0)
		return; // Supercede

	// Check for registered client commands
	CmdMngr::iterator aa = g_commands.clcmdprefixbegin(cmd);

	if (!aa)
		aa = g_commands.clcmdbegin();

	while (aa)
	{
		if ((*aa).matchCommandLine(cmd, arg) && (*aa).getPlugin()->isExecutable((*aa).getFunction()))
		{
			ret = executeForwards((*aa).getFunction(), static_cast<cell>(pPlayer->index),
				static_cast<cell>((*aa).getFlags()), static_cast<cell>((*aa).getId()));
			if (ret & 2) supercede = true;
			if (ret & 1) return; // Supercede immediately
		}
		++aa;
	}

	// Check menu commands
	if (!strcmp(cmd, "menuselect"))
	{
		int pressed_key = atoi(arg) - 1;
		int bit_key = (1 << pressed_key);

		if (pPlayer->keys & bit_key)
		{
			if (gpGlobals->time > pPlayer->menuexpire)
			{
				if (Menu *pMenu = get_menu_by_id(pPlayer->newmenu))
				{
					pMenu->Close(pPlayer->index);
					return; // Supercede
				}
				else if (pPlayer->menu > 0 && !pPlayer->vgui)
				{
					pPlayer->menu = 0;
					pPlayer->keys = 0;
					return; // Supercede
				}
			}

			int menuid = pPlayer->menu;
			pPlayer->menu = 0;

			// First, do new menus
			int func_was_executed = -1;
			if (pPlayer->newmenu != -1)
			{
				int menu = pPlayer->newmenu;
				pPlayer->newmenu = -1;
				if (Menu *pMenu = get_menu_by_id(menu))
				{
					int item = pMenu->PagekeyToItem(pPlayer->page, pressed_key + 1);
					if (item == MENU_BACK)
					{
						if (pMenu->pageCallback >= 0)
							executeForwards(pMenu->pageCallback, static_cast<cell>(pPlayer->index), static_cast<cell>(MENU_BACK), static_cast<cell>(menu));

						// Re-validate: plugin callback may have destroyed this menu
						pMenu = get_menu_by_id(menu);
						if (pMenu)
							pMenu->Display(pPlayer->index, pPlayer->page - 1);
					}
					else if (item == MENU_MORE)
					{
						if (pMenu->pageCallback >= 0)
							executeForwards(pMenu->pageCallback, static_cast<cell>(pPlayer->index), static_cast<cell>(MENU_MORE), static_cast<cell>(menu));

						// Re-validate: plugin callback may have destroyed this menu
						pMenu = get_menu_by_id(menu);
						if (pMenu)
							pMenu->Display(pPlayer->index, pPlayer->page + 1);
					}
					else
					{
						// Capture func before execution — menu may be destroyed in callback
						int menuFunc = pMenu->func;
						ret = executeForwards(menuFunc, static_cast<cell>(pPlayer->index), static_cast<cell>(menu), static_cast<cell>(item));
						func_was_executed = menuFunc;
						if (ret & 2)
						{
							supercede = true;
						}
						else if (ret & 1)
						{
							return; // Supercede
						}
					}
				}
			}

			// Now, do old menus
			MenuMngr::iterator a = g_menucmds.begin();

			while (a)
			{
				g_menucmds.SetWatchIter(a);
				if ((*a).matchCommand(menuid, bit_key)
					&& (*a).getPlugin()->isExecutable((*a).getFunction())
					&& (func_was_executed == -1
						|| !g_forwards.isSameSPForward(func_was_executed, (*a).getFunction()))
					)
				{
					ret = executeForwards((*a).getFunction(), static_cast<cell>(pPlayer->index),
						static_cast<cell>(pressed_key), 0);

					if (ret & 2) supercede = true;
					if (ret & 1) return; // Supercede
				}
				if (g_menucmds.GetWatchIter() != a)
				{
					a = g_menucmds.GetWatchIter();
				}
				else
				{
					++a;
				}
			}
		}
	}

	// If not superceding, continue the hookchain (call original pfnClientCommand)
	if (!supercede)
	{
		chain->callNext(pEdict);
	}
}
