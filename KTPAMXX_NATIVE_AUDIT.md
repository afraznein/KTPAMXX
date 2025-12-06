# KTPAMXX Native Audit - Extension Mode Compatibility

## Purpose
This document tracks all native functions used by plugins to ensure KTPAMXX works correctly
in ReHLDS extension mode (without Metamod). Each native is tracked with its implementation
status and any dependencies it has on hooks, variables, or engine functions.

## Quick Reference for Context Recovery
When context resets, check:
1. This file for native/forward status
2. `amxmodx/meta_api.cpp` - Extension mode initialization (KTPAMX_InitAsRehldsExtension)
3. `amxmodx/amxmodx.cpp` - Native implementations
4. `amxmodx/CPlayer.cpp` - Player data structures

## Status Legend
- **OK**: Works in extension mode
- **BROKEN**: Does not work in extension mode
- **PARTIAL**: Partially works, some functionality missing
- **UNTESTED**: Not yet verified
- **N/A**: Not applicable for extension mode

---

## Critical Implementation Notes

### Variables That Need Initialization
| Variable | Purpose | Where Set |
|----------|---------|-----------|
| `g_players[]` | Player data array | Connection hooks |
| `g_players_num` | Active player count | Connection/disconnection |
| `gpGlobals` | Engine globals pointer | GiveFnptrsToDll |
| `g_engfuncs` | Engine functions table | GiveFnptrsToDll |
| `modMsgs[]` / `modMsgsEnd[]` | Message hook handlers | SV_ActivateServer |
| `gmsgTextMsg`, `gmsgSayText`, etc. | Message IDs | REG_USER_MSG lookup |

### Hooks Required for Extension Mode
| Hook | Purpose | Triggers |
|------|---------|----------|
| **SV_ActivateServer** | Server activation, plugin loading, user message lookup | plugin_init, plugin_cfg |
| **SV_Spawn (pre-activation)** | Precache phase | plugin_precache |
| **ClientConnected** | Player connection | client_connect forward |
| **SV_DropClient** | Player disconnection | client_disconnect forward |
| **Steam_NotifyClientConnect** | Authorization | client_authorized forward |
| **SV_Frame** | Per-frame processing | client_putinserver detection |
| **ClientCommand** | Client command processing | client_command forward |
| **SV_ClientUserInfoChanged** | Client info changes | client_infochanged forward |
| **IMessageManager hooks** | Message interception | register_event functionality |
| **pfnClientCvarChanged** | Cvar change callback | client_cvar_changed forward |
| **File consistency check** | File validation | inconsistent_file forward |
| **PF_RegUserMsg_I** | Message ID capture | HUD drawing, client_print |
| **SV_ClientCommand** | Client command processing | register_clcmd, menus, client_command forward |
| **SV_InactivateClients** | Map change deactivation | plugin_end, client_disconnect forwards |
| **SV_Spawn_f** | Client spawn command after map change | client_connect, client_putinserver forwards |

### Critical Timing Issues
1. **SV_ActivateServer hook order** - KTPAMXX must initialize BEFORE chain->callNext()
   so user messages are registered before clients receive the message table
2. **plugin_precache** must run before server is fully active (during SV_Spawn)
3. **client_putinserver** detection in SV_Frame must check spawned state
4. **User message IDs** must be looked up after engine registers them but before clients connect
5. **Map change sequence** - `SV_InactivateClients()` → `SV_ServerShutdown()` → `SV_SpawnServer()` → `SV_ActivateServer()`
6. **Map change reconnect** - Clients don't go through `ClientConnected` after map change; `SV_Spawn_f` must handle initialization

---

## Known Issues (RESOLVED)
1. ~~User message registration timing~~ - Fixed by calling chain->callNext() first in SV_ActivateServer_RH
2. ~~REG_USER_MSG creates new messages~~ - **FIXED** in KTPReHLDS: `RegUserMsg_internal` now searches both
   `sv_gpUserMsgs` AND `sv_gpNewUserMsgs` lists, so looking up existing messages returns the correct ID
   instead of creating duplicates with high IDs (130+) that clients don't recognize.
3. ~~Map change breaks client forwards~~ - **FIXED**: Added `SV_InactivateClients` hook for proper deactivation
   and `SV_Spawn_f` hook for client reinitialization after map change. Clients persist through map change
   and AMXX forwards (`client_connect`, `client_putinserver`) fire correctly on the new map.

---

## Map Change Implementation (RESOLVED)

### How Map Change Works in Extension Mode
When a map change occurs via `changelevel`, the engine follows this sequence:
1. **SV_InactivateClients()** - Marks all clients as inactive (but doesn't disconnect them)
2. **SV_ServerShutdown()** - Shuts down the current map
3. **SV_SpawnServer()** - Loads the new map
4. **SV_ActivateServer()** - Activates the new map
5. Clients receive `reconnect` stufftext command and reconnect to the server

### The Problem
During map change reconnect, clients do NOT go through the normal `ClientConnected` hook because they're
already connected (just inactive). This meant AMXX player state wasn't being reinitialized, causing:
- `client_connect` and `client_putinserver` forwards not firing after map change
- Plugins like cvar checker not working on the new map
- Player data (`CPlayer`) not being properly reset

### The Solution
Two hooks work together to handle map changes:

1. **SV_InactivateClients_RH** (fires at START of map change):
   - Fires `client_disconnect` forwards for all connected players
   - Fires `plugin_end` forward
   - Clears AMXX player state (calls `pPlayer->Disconnect()` which is internal cleanup only)
   - Disables DropClient hook during transition

2. **SV_Spawn_f_RH** (fires when client sends "spawn" command after reconnect):
   - Checks if player was never initialized (`!pPlayer->initialized`)
   - If not initialized, calls `pPlayer->Connect()` and fires `client_connect` forward
   - Calls `pPlayer->PutInServer()` and fires `client_putinserver` forward
   - Works for both initial connection AND map change reconnection

### Key Insight
The `SV_Spawn_f` hook is critical because it's the only hook that fires for BOTH:
- Initial client connections (after `ClientConnected`)
- Map change reconnections (where `ClientConnected` does NOT fire)

By checking `pPlayer->initialized`, we can detect if this is a reconnection that needs initialization.

### Files Modified
- **KTPAMXX `meta_api.cpp`**: Added `SV_InactivateClients_RH` and `SV_Spawn_f_RH` hooks
- **KTPReHLDS**: Added `SV_InactivateClients` hookchain (already had `SV_Spawn_f`)

---

## Message System Implementation (RESOLVED)

### How It Works Now
The message system in extension mode works as follows:

1. **Game DLL registers messages** during `GameDLLInit()` - these go into `sv_gpNewUserMsgs`
2. **KTPAMXX calls `REG_USER_MSG`** during `SV_ActivateServer_RH` to look up message IDs
3. **KTPReHLDS fix** ensures `RegUserMsg_internal` searches BOTH `sv_gpUserMsgs` AND `sv_gpNewUserMsgs`
4. **Existing IDs are returned** instead of creating new messages

### Files Modified
- **KTPReHLDS `sv_main.cpp`**: Modified `RegUserMsg_internal` to search both message lists
- **KTPAMXX `meta_api.cpp`**: Uses `REG_USER_MSG` to look up message IDs during server activation

### Key Insight
The original engine only searched `sv_gpUserMsgs` (messages already sent to clients), but newly registered
messages start in `sv_gpNewUserMsgs`. By searching both lists, we can look up message IDs without creating
duplicates.

---

## Master Native List by Category

### Registration Natives
| Native | Status | Dependencies | Used By |
|--------|--------|--------------|---------|
| `register_plugin` | OK | None | All plugins |
| `register_cvar` | OK | CVAR_REGISTER | All plugins |
| `register_clcmd` | **OK** | Command system, SV_ClientCommand hook | ktp_cvar, KTPMatchHandler, timeleft, admin |
| `register_concmd` | OK | Command system | KTPAdminAudit, admin, admincmd, adminchat |
| `register_srvcmd` | OK | Command system | admin, timeleft, plmenu |
| `register_dictionary` | OK | Lang system | ktp_cvar, admin, admincmd, plmenu |
| `register_event` | UNTESTED | Message hooks | nextmap, mapchooser, plmenu, stats.sma |
| `register_logevent` | UNTESTED | Log parsing | Stats plugins |
| `register_menucmd` | **OK** | Menu system, SV_ClientCommand hook | stats.sma, plmenu, mapchooser |
| `register_menuid` | **OK** | Menu system | stats.sma, plmenu, mapchooser |
| `register_statsfwd` | UNTESTED | DODX module | stats.sma |

### Player Info Natives
| Native | Status | Dependencies | Used By |
|--------|--------|--------------|---------|
| `get_user_name` | OK | g_players | All plugins |
| `get_user_authid` | OK | g_players | All plugins |
| `get_user_userid` | OK | g_players | admin, admincmd, adminchat |
| `get_user_ip` | OK | g_players | All plugins |
| `get_user_flags` | OK | g_players, admin | KTPAdminAudit, admin, admincmd, plmenu |
| `get_user_team` | OK | g_players | KTPMatchHandler, plmenu, stats.sma |
| `get_user_info` | UNTESTED | INFOKEY_VALUE | admin, stats_logging.sma |
| `get_user_health` | UNTESTED | g_players, pev | stats.sma, plmenu |
| `get_user_origin` | UNTESTED | pev | stats.sma |
| `get_user_time` | UNTESTED | g_players | stats_logging.sma |
| `get_user_ping` | UNTESTED | g_players | stats_logging.sma |
| `is_user_connected` | OK | g_players | All plugins |
| `is_user_alive` | OK | g_players, pev | Stats plugins, plmenu |
| `is_user_bot` | OK | g_players | ktp_cvar, ktp_file, admincmd, plmenu |
| `is_user_hltv` | OK | g_players | ktp_cvar, ktp_file |
| `is_user_admin` | UNTESTED | Admin system | adminchat, plmenu |
| `is_user_connecting` | UNTESTED | g_players | admincmd |
| `get_players` | OK | g_players | KTPAdminAudit, admin, admincmd, plmenu |
| `get_playersnum` | OK | g_players | admincmd |
| `find_player` | UNTESTED | g_players | KTPAdminAudit, admincmd |
| `MaxClients` | OK | gpGlobals | admincmd, plmenu |

### CVAR Natives
| Native | Status | Dependencies | Used By |
|--------|--------|--------------|---------|
| `get_cvar_string` | OK | CVAR_GET_STRING | KTPAdminAudit, ktp_cvar |
| `get_cvar_num` | OK | CVAR_GET_FLOAT | Many plugins |
| `get_cvar_float` | OK | CVAR_GET_FLOAT | stats.sma, mapchooser |
| `set_cvar_float` | UNTESTED | CVAR_SET_FLOAT | admin, mapchooser |
| `set_cvar_string` | UNTESTED | CVAR_SET_STRING | mapchooser |
| `get_cvar_pointer` | OK | CVAR_GET_POINTER | admincmd, plmenu, timeleft |
| `get_pcvar_string` | OK | cvar_t pointer | All plugins |
| `get_pcvar_num` | OK | cvar_t pointer | All plugins |
| `get_pcvar_float` | OK | cvar_t pointer | admin, admincmd, plmenu |
| `get_pcvar_flags` | UNTESTED | cvar_t pointer | admincmd |
| `set_pcvar_string` | UNTESTED | cvar_t pointer | admincmd, timeleft |
| `set_pcvar_num` | UNTESTED | cvar_t pointer | admincmd, plmenu |
| `set_pcvar_float` | UNTESTED | cvar_t pointer | admincmd, plmenu, nextmap |
| `set_pcvar_flags` | UNTESTED | cvar_t pointer | admincmd |

### Communication Natives
| Native | Status | Dependencies | Used By |
|--------|--------|--------------|---------|
| `client_print` | OK | gmsgTextMsg, MESSAGE_BEGIN/END | All plugins |
| `client_cmd` | OK | CLIENT_COMMAND | ktp_cvar, timeleft, stats.sma, mapchooser |
| `server_cmd` | OK | SERVER_COMMAND | All plugins |
| `server_exec` | OK | SERVER_EXECUTE | KTPMatchHandler, admincmd, plmenu |
| `server_print` | OK | Con_Printf | admin, admincmd, plmenu |
| `console_print` | OK | g_engfuncs | admin, admincmd, adminchat, plmenu |
| `engclient_print` | OK | pfnClientPrintf | admin |
| `show_activity_key` | UNTESTED | Activity display | admincmd, plmenu |
| `show_activity_id` | UNTESTED | Activity display | admincmd, plmenu |
| `show_motd` | UNTESTED | MOTD display | stats.sma |
| `show_menu` | **OK** | Menu display, SV_ClientCommand hook | stats.sma, plmenu, mapchooser |
| `colored_menus` | UNTESTED | Menu capabilities | plmenu, mapchooser |

### Task Natives
| Native | Status | Dependencies | Used By |
|--------|--------|--------------|---------|
| `set_task` | OK | g_tasksMngr | ktp_cvar, ktp_file, KTPMatchHandler, timeleft, admincmd |
| `remove_task` | OK | g_tasksMngr | ktp_cvar, KTPMatchHandler, timeleft, stats_logging.sma |
| `task_exists` | OK | g_tasksMngr | KTPMatchHandler |

### File Natives
| Native | Status | Dependencies | Used By |
|--------|--------|--------------|---------|
| `fopen` | OK | File I/O | All plugins |
| `fclose` | OK | File I/O | All plugins |
| `fgets` | OK | File I/O | All plugins |
| `fputs` | OK | File I/O | ktp_cvar |
| `feof` | OK | File I/O | All plugins |
| `file_exists` | OK | File I/O | ktp_cvar, admin, admincmd, plmenu, nextmap |
| `read_file` | OK | File I/O | admin, plmenu, nextmap |
| `write_file` | OK | File I/O | admin |

### Logging Natives
| Native | Status | Dependencies | Used By |
|--------|--------|--------------|---------|
| `log_amx` | OK | Logging system | All plugins |
| `log_to_file` | OK | File I/O | ktp_file, KTPMatchHandler |
| `log_message` | OK | ALERT | ktp_file, admincmd, adminchat, stats_logging.sma |

### HUD Natives
| Native | Status | Dependencies | Used By |
|--------|--------|--------------|---------|
| `CreateHudSyncObj` | **OK** | HUD system | KTPMatchHandler, stats.sma |
| `set_hudmessage` | **OK** | HUD system, PF_RegUserMsg_I hook | KTPMatchHandler, timeleft, adminchat, stats.sma |
| `show_hudmessage` | **OK** | HUD system, PF_RegUserMsg_I hook | timeleft, adminchat, stats.sma |
| `ShowSyncHudMsg` | **OK** | HUD system, PF_RegUserMsg_I hook | KTPMatchHandler, stats.sma |
| `ClearSyncHud` | **OK** | HUD system | KTPMatchHandler |

### Special Natives (Critical for Extension Mode)
| Native | Status | Dependencies | Used By |
|--------|--------|--------------|---------|
| `query_client_cvar` | OK | Engine callback, g_NewDLL_Available | ktp_cvar |
| `force_unmodified` | UNTESTED | Engine precache | ktp_file |
| `precache_generic` | UNTESTED | Precache system | statssounds.sma |
| `get_configsdir` | OK | Path resolution | All plugins |
| `get_localinfo` | OK | Local info | ktp_file, nextmap, mapchooser |
| `set_localinfo` | UNTESTED | Local info | nextmap, mapchooser |
| `get_mapname` | OK | gpGlobals | KTPMatchHandler, admincmd, nextmap, mapchooser |
| `get_modname` | UNTESTED | Game info | plmenu, admincmd |
| `is_map_valid` | UNTESTED | Map validation | admincmd, nextmap, mapchooser |
| `get_systime` | OK | System time | ktp_cvar, KTPMatchHandler |
| `get_time` | OK | Time formatting | ktp_cvar, KTPMatchHandler, timeleft |
| `get_timeleft` | OK | gpGlobals | timeleft, mapchooser |
| `get_gametime` | OK | gpGlobals | stats.sma, antiflood, timeleft |
| `is_dedicated_server` | UNTESTED | Engine check | admin |

### Admin System Natives
| Native | Status | Dependencies | Used By |
|--------|--------|--------------|---------|
| `admins_push` | UNTESTED | Admin system | admin |
| `admins_flush` | UNTESTED | Admin system | admin |
| `admins_num` | UNTESTED | Admin system | admin |
| `admins_lookup` | UNTESTED | Admin system | admin |
| `set_user_flags` | UNTESTED | Admin system | admin |
| `remove_user_flags` | UNTESTED | Admin system | admin |
| `read_flags` | UNTESTED | Admin system | admin, plmenu |
| `get_flags` | UNTESTED | Admin system | admin, admincmd |
| `cmd_access` | UNTESTED | Admin system | admin, admincmd, adminchat, plmenu |
| `cmd_target` | UNTESTED | Admin system | admin, admincmd, adminchat |
| `access` | UNTESTED | Admin flags check | plmenu, adminchat |
| `get_concmd` | UNTESTED | Command info | adminchat |

### Message/Event Natives
| Native | Status | Dependencies | Used By |
|--------|--------|--------------|---------|
| `message_begin` | UNTESTED | MESSAGE_BEGIN | admincmd |
| `message_end` | UNTESTED | MESSAGE_END | admincmd |
| `read_data` | UNTESTED | Event data | stats.sma, mapchooser, plmenu |
| `get_msg_block` | UNTESTED | Message blocking | Various |
| `set_msg_block` | UNTESTED | Message blocking | Various |

### String/Utility Natives
| Native | Status | Dependencies | Used By |
|--------|--------|--------------|---------|
| `formatex` | OK | String formatting | All plugins |
| `format` | OK | String formatting | All plugins |
| `copy` | OK | String copy | All plugins |
| `strlen` | OK | String length | All plugins |
| `contain` | OK | String search | All plugins |
| `containi` | OK | Case-insensitive search | KTPAdminAudit |
| `equal` | OK | String compare | All plugins |
| `equali` | OK | Case-insensitive compare | All plugins |
| `trim` | OK | String trim | ktp_cvar, ktp_file |
| `add` | OK | String append | ktp_cvar |
| `parse` | OK | String parsing | admin, plmenu, timeleft |
| `replace` | UNTESTED | String replace | stats.sma |
| `replace_all` | UNTESTED | Replace all occurrences | stats.sma |
| `str_to_num` | OK | String to int | All plugins |
| `num_to_str` | UNTESTED | Int to string | stats.sma |
| `num_to_word` | UNTESTED | Number to word | timeleft |
| `floatstr` | OK | String to float | ktp_cvar |
| `floatabs` | OK | Float absolute | ktp_cvar |
| `floatround` | OK | Float round | ktp_cvar |
| `read_argv` | OK | Read argument | All plugins |
| `read_argc` | OK | Argument count | All plugins |
| `read_args` | OK | Read all args | admincmd, adminchat |
| `remove_quotes` | UNTESTED | Remove quotes | admincmd |
| `get_distance` | UNTESTED | Distance calc | stats.sma |

### Cross-Plugin Variable Natives
| Native | Status | Dependencies | Used By |
|--------|--------|--------------|---------|
| `get_xvar_id` | UNTESTED | xvar lookup | plmenu, admincmd |
| `get_xvar_num` | UNTESTED | xvar read | plmenu, admincmd |
| `set_xvar_num` | UNTESTED | xvar write | admincmd |
| `xvar_exists` | UNTESTED | xvar check | admincmd |

### Plugin/Module Natives
| Native | Status | Dependencies | Used By |
|--------|--------|--------------|---------|
| `get_plugin` | UNTESTED | Plugin info | admincmd |
| `get_pluginsnum` | UNTESTED | Plugin count | admincmd |
| `get_module` | UNTESTED | Module info | admincmd |
| `get_modulesnum` | UNTESTED | Module count | admincmd |
| `LibraryExists` | UNTESTED | Library check | plmenu |
| `set_module_filter` | UNTESTED | Module filtering | plmenu |
| `set_native_filter` | UNTESTED | Native filtering | plmenu |

### Language Natives
| Native | Status | Dependencies | Used By |
|--------|--------|--------------|---------|
| `get_lang` | UNTESTED | Language system | adminchat |
| `get_langsnum` | UNTESTED | Language count | adminchat |

### Array/Trie Data Structures
| Native | Status | Dependencies | Used By |
|--------|--------|--------------|---------|
| `ArrayCreate` | UNTESTED | Dynamic arrays | mapchooser, plmenu |
| `ArrayDestroy` | UNTESTED | Dynamic arrays | mapchooser, plmenu |
| `ArrayPushCell` | UNTESTED | Array operations | plmenu |
| `ArrayGetCell` | UNTESTED | Array operations | plmenu |
| `ArrayPushString` | UNTESTED | Array operations | mapchooser |
| `ArrayGetString` | UNTESTED | Array operations | mapchooser |
| `ArrayGetStringHandle` | UNTESTED | Array operations | mapchooser |
| `ArrayClear` | UNTESTED | Array operations | plmenu |
| `ArraySize` | UNTESTED | Array operations | plmenu |
| `TrieCreate` | UNTESTED | Trie structure | admincmd |
| `TrieDestroy` | UNTESTED | Trie structure | admincmd |
| `TrieSetString` | UNTESTED | Trie operations | admincmd, plmenu |
| `TrieGetString` | UNTESTED | Trie operations | admincmd |
| `TrieKeyExists` | UNTESTED | Trie operations | admincmd |

---

## Forwards Status

### Core Forwards
| Forward | Status | Hook Required | Used By |
|---------|--------|---------------|---------|
| `plugin_init` | OK | SV_ActivateServer | All plugins |
| `plugin_cfg` | OK | After plugin_init | KTPAdminAudit, KTPMatchHandler, adminchat, plmenu |
| `plugin_precache` | OK | SV_Spawn (pre-activation) | ktp_file, statssounds.sma |
| `plugin_end` | OK | SV_InactivateClients (map change) | KTPMatchHandler, admincmd, nextmap, mapchooser, plmenu |
| `plugin_natives` | UNTESTED | Before plugin_init | plmenu |

### Client Forwards
| Forward | Status | Hook Required | Used By |
|---------|--------|---------------|---------|
| `client_connect` | OK | ClientConnected | admin, KTPMatchHandler |
| `client_authorized` | OK | Steam_NotifyClientConnect | admin |
| `client_putinserver` | OK | SV_Frame detection | ktp_cvar, ktp_file, admin, plmenu, stats.sma, stats_logging.sma |
| `client_disconnect` | OK | SV_DropClient | ktp_cvar, KTPMatchHandler |
| `client_disconnected` | OK | SV_DropClient | admincmd, stats.sma, stats_logging.sma |
| `client_infochanged` | UNTESTED | SV_ClientUserInfoChanged | admin |
| `client_command` | **OK** | SV_ClientCommand hook | Many plugins |

### Custom KTP Forwards
| Forward | Status | Hook Required | Used By |
|---------|--------|---------------|---------|
| `client_cvar_changed` | OK | pfnClientCvarChanged callback | ktp_cvar |
| `inconsistent_file` | UNTESTED | File consistency check | ktp_file |

### DODX Forwards (Module-provided)
| Forward | Status | Hook Required | Used By |
|---------|--------|---------------|---------|
| `client_damage` | UNTESTED | DODX module | stats.sma |
| `client_death` | UNTESTED | DODX module | stats.sma |
| `client_score` | UNTESTED | DODX module | - |
| `dod_client_changeteam` | UNTESTED | DODX module | - |
| `dod_client_changeclass` | UNTESTED | DODX module | - |
| `dod_client_spawn` | UNTESTED | DODX module | - |
| `dod_client_scope` | UNTESTED | DODX module | - |
| `dod_client_weaponpickup` | UNTESTED | DODX module | - |
| `dod_client_prone` | UNTESTED | DODX module | - |
| `dod_client_weaponswitch` | UNTESTED | DODX module | - |
| `dod_grenade_explosion` | UNTESTED | DODX module | - |
| `dod_rocket_explosion` | UNTESTED | DODX module | - |
| `dod_client_objectpickup` | UNTESTED | DODX module | - |
| `dod_client_stamina` | UNTESTED | DODX module | - |
| `get_score` | UNTESTED | Stats ranking | dodstats.sma |

---

## Module Dependencies Summary

| Module | Required For | Status |
|--------|--------------|--------|
| **DODX (dodx_amxx)** | DoD stats, weapons, game-specific natives | UNTESTED |
| **Fun (fun_amxx)** | user_slap, user_kill | UNTESTED |
| **Engine (engine_amxx)** | engclient_cmd, engine_changelevel, pev | UNTESTED |
| **Fakemeta (fakemeta_amxx)** | get_ent_data, set_ent_data | UNTESTED |
| **CStrike (cstrike_amxx)** | cs_get_user_team, cs_set_user_team, etc. | UNTESTED |
| **SQL/SQLite** | Database functionality | UNTESTED |
| **cURL** | HTTP requests (Discord integration) | UNTESTED |
| **ReAPI** | ReHLDS/ReGameDLL hooks | UNTESTED |

### Engine Module Natives
| Native | Description | Used By |
|--------|-------------|---------|
| `engine_changelevel` | Change map | admincmd, nextmap |
| `engclient_cmd` | Execute client command | plmenu, dod/plmenu |
| `pev` / `set_pev` | Entity property access | Various |
| `get_ent_data` / `set_ent_data` | Entity data access | plmenu |

### Fun Module Natives
| Native | Description | Used By |
|--------|-------------|---------|
| `user_kill` | Kill player | admincmd, plmenu |
| `user_slap` | Slap player | admincmd, plmenu |

### CStrike Module Natives
| Native | Description | Used By |
|--------|-------------|---------|
| `cs_get_user_team` | Get CS team | plmenu |
| `cs_set_user_team` | Set CS team | plmenu |
| `cs_get_user_deaths` | Get death count | plmenu |
| `cs_set_user_deaths` | Set death count | plmenu |
| `cs_reset_user_model` | Reset player model | plmenu |
| `cstrike_running` | Check if CS mod | mapchooser |

### DODX Module Natives
| Native | Description | Used By |
|--------|-------------|---------|
| `register_statsfwd` | Register stats forwards | stats.sma |
| `get_user_wstats` | Weapon stats | stats.sma, stats_logging.sma |
| `get_user_wrstats` | Round weapon stats | stats.sma |
| `get_user_stats` | Overall stats | stats.sma |
| `get_user_rstats` | Round stats | stats.sma |
| `get_user_vstats` | Victim stats | stats.sma |
| `get_user_astats` | Attacker stats | stats.sma |
| `get_stats` | Stats from file | stats.sma |
| `get_statsnum` | Total stats entries | stats.sma |
| `xmod_is_melee_wpn` | Check if melee weapon | stats.sma |
| `xmod_get_wpnname` | Get weapon name | stats.sma |
| `xmod_get_wpnlogname` | Get weapon log name | stats_logging.sma |
| `dod_get_team_score` | Team score | stats.sma |
| `dod_user_kill` | Kill player (no death) | plmenu.sma |

---

## Event Registration Requirements

| Event | Description | Used By |
|-------|-------------|---------|
| Event 30 | Intermission | nextmap, stats.sma |
| TeamScore | Team score update | mapchooser |
| TeamInfo | Team info change | plmenu |
| TextMsg | Text message | plmenu |
| ResetHUD | HUD reset | stats.sma |
| RoundState | Round state change | stats.sma |
| CurWeapon | Current weapon | stats.sma |
| ObjScore | Objective score | stats.sma |

---

## Priority Plugins

### Custom Plugins (HIGH PRIORITY)
1. **KTPAdminAudit** - `KTPAdminAudit.sma` - Admin kick auditing with Discord
2. **KTPCvarChecker** - `ktp_cvar.sma` - Real-time cvar validation
3. **KTPMatchHandler** - `KTPMatchHandler.sma` - Match management with pause, Discord, HUD
4. **KTPFileChecker** - `ktp_file.sma` - File consistency checking

### Module Plugins (MEDIUM PRIORITY)
1. **KTPReAPI** - ReAPI module integration
2. **KTPAmxxCurl** - HTTP/FTP functionality (cURL module)

### Core AMXX Plugins (HIGH PRIORITY)
1. **admin.sma** - Admin system (client_authorized, client_infochanged)
2. **admincmd.sma** - Admin commands (Fun, Engine modules)
3. **plmenu.sma** - Player menu (Fun, Engine, CStrike, Fakemeta modules)
4. **timeleft.sma** - Time remaining (HUD, tasks)

### DoD-Specific (HIGH PRIORITY)
1. **dod/stats.sma** - DoD statistics (DODX module)
2. **dod/dodstats.sma** - DoD stats module
3. **dod/plmenu.sma** - DoD player menu (DODX, Fun, Engine)
4. **dod/stats_logging.sma** - Stats logging (DODX)

### Modules Required
- SQL/SQLite - Database functionality
- DOD module (dodx, dodfun) - DoD-specific natives
- Stats modules - Player statistics
- cURL module - HTTP requests

---

## Native Extraction Progress

### Phase 1: Custom Plugins - COMPLETE
- [x] KTPAdminAudit.sma
- [x] ktp_cvar.sma
- [x] KTPMatchHandler.sma
- [x] ktp_file.sma

### Phase 2: Core AMXX Plugins - COMPLETE
- [x] admin.sma
- [x] admincmd.sma
- [x] plmenu.sma
- [x] timeleft.sma
- [x] antiflood.sma
- [x] nextmap.sma
- [x] mapchooser.sma
- [x] adminchat.sma

### Phase 3: DoD Plugins - COMPLETE
- [x] dod/stats.sma
- [x] dod/dodstats.sma
- [x] dod/plmenu.sma
- [x] dod/stats_logging.sma
- [x] dod/statssounds.sma

### Phase 4: Module Includes - PENDING
- [ ] sqlx.inc
- [ ] dodx.inc
- [ ] dodfun.inc
- [ ] dodstats.inc

---

## Detailed Plugin Analysis

### KTPAdminAudit.sma
**Natives Used:**
- `register_plugin`, `register_cvar`, `register_concmd`
- `get_configsdir`, `get_cvar_string`, `get_pcvar_string`, `get_pcvar_num`
- `log_amx`
- `is_user_connected`, `get_user_name`, `get_user_authid`, `get_user_ip`, `get_user_flags`
- `get_players`, `find_player`
- `read_argv`
- `fopen`, `fclose`, `fgets`, `feof`
- String operations

**Forwards Used:** `plugin_init`, `plugin_cfg`

**Module Dependencies:** cURL (optional, for Discord)

---

### ktp_cvar.sma
**Natives Used:**
- `register_plugin`, `register_cvar`, `register_clcmd`, `register_dictionary`
- `get_configsdir`, `file_exists`, `server_cmd`
- `get_systime`, `floatstr`, `floatabs`, `floatround`
- `log_amx`
- `is_user_connected`, `is_user_bot`, `is_user_hltv`
- `get_user_name`, `get_user_authid`, `get_user_ip`
- `client_print`, `client_cmd`
- `set_task`, `remove_task`
- `query_client_cvar`
- `get_pcvar_num`, `get_pcvar_string`, `get_cvar_string`
- File operations, `get_time`, String operations

**Forwards Used:** `plugin_init`, `client_putinserver`, `client_disconnected`, **`client_cvar_changed`** (CUSTOM)

**Critical:** `client_cvar_changed` is a custom KTP forward triggered by pfnClientCvarChanged

---

### KTPMatchHandler.sma
**Natives Used:**
- `register_plugin`, `register_cvar`, `register_clcmd`, `register_concmd`
- `get_configsdir`, `get_pcvar_string`, `get_pcvar_num`
- `log_to_file`, `log_amx`
- `get_time`, `get_systime`, `get_mapname`
- `get_user_name`, `get_user_authid`, `get_user_ip`, `get_user_team`
- `is_user_connected`, `get_players`
- `client_print`, `server_cmd`, `server_exec`
- `set_task`, `remove_task`
- File operations
- `CreateHudSyncObj`, `set_hudmessage`, `ShowSyncHudMsg`, `ClearSyncHud`

**Forwards Used:** `plugin_init`, `plugin_cfg`, `plugin_end`, `client_connect`, `client_disconnect`

**Module Dependencies:** ReAPI (optional), cURL (optional)

---

### ktp_file.sma
**Natives Used:**
- `register_plugin`, `register_cvar`
- `get_localinfo`, `get_pcvar_num`
- `log_amx`, `log_to_file`, `log_message`
- `is_user_bot`, `is_user_hltv`
- `get_user_name`, `get_user_authid`
- `client_print`, `server_cmd`
- `set_task`
- File operations
- **`force_unmodified`** - File consistency checking

**Forwards Used:** `plugin_init`, **`plugin_precache`** (CRITICAL), `client_putinserver`, **`inconsistent_file`** (CUSTOM)

**Critical:** `plugin_precache` and `inconsistent_file` require special hook timing

---

### admin.sma
**Natives Used:**
- `register_plugin`, `register_cvar`, `register_concmd`, `register_srvcmd`, `register_dictionary`
- `get_configsdir`, `server_cmd`, `server_print`
- `set_cvar_float`, `get_cvar_string`, `get_pcvar_num`, `get_pcvar_float`, `get_pcvar_string`
- `remove_user_flags`, `set_user_flags`, `read_flags`, `get_flags`
- `admins_push`, `admins_flush`, `admins_num`, `admins_lookup`
- `get_players`, `get_user_name`, `get_user_authid`, `get_user_ip`, `get_user_userid`, `get_user_info`
- `is_user_connected`, `is_dedicated_server`
- `log_amx`, `console_print`, `engclient_print`
- `cmd_access`, `cmd_target`
- `read_argc`, `read_argv`, `parse`
- File operations, String operations

**Forwards Used:** `plugin_init`, `client_connect`, **`client_authorized`**, `client_putinserver`, **`client_infochanged`**

**Critical:** `client_authorized` and `client_infochanged` require ReHLDS hooks

**SQL Module (optional):** SQL_MakeStdTuple, SQL_Connect, SQL_PrepareQuery, SQL_Execute, etc.

---

### admincmd.sma
**Natives Used:**
- `register_plugin`, `register_cvar`, `register_concmd`, `register_clcmd`, `register_dictionary`
- All CVAR operations
- Player info natives
- `cmd_access`, `cmd_target`
- `read_argv`, `read_argc`, `read_args`
- `server_cmd`, `server_exec`, `server_print`, `console_print`, `client_print`, `client_cmd`
- `log_amx`, `log_message`
- `show_activity_key`, `show_activity_id`
- `get_players`, `get_playersnum`, `MaxClients`
- `get_mapname`, `get_modname`, `is_map_valid`
- `get_plugin`, `get_pluginsnum`, `get_module`, `get_modulesnum`
- Cross-plugin variables
- **`user_kill`**, **`user_slap`** (Fun module)
- **`engine_changelevel`** (Engine module)
- `message_begin`, `message_end`
- Trie operations

**Forwards Used:** `plugin_init`, `plugin_end`, `client_disconnected`

**Module Dependencies:** Fun, Engine

---

### plmenu.sma
**Natives Used:**
- All registration natives including `register_event`
- `set_module_filter`, `set_native_filter`, `LibraryExists`
- All CVAR and player info natives
- `access`, `cmd_access`
- All communication natives
- **`user_kill`**, **`user_slap`** (Fun)
- **`engclient_cmd`** (Engine)
- **`cs_get_user_team`**, **`cs_set_user_team`**, etc. (CStrike)
- **`get_ent_data`**, **`set_ent_data`** (Fakemeta)
- Array and Trie operations

**Forwards Used:** `plugin_init`, `plugin_cfg`, `plugin_end`, `plugin_natives`, `client_putinserver`

**Module Dependencies:** Fun, Engine, CStrike (optional), Fakemeta (optional)

---

### dod/stats.sma
**Natives Used:**
- Registration natives including `register_event`, `register_statsfwd`
- HUD natives
- Player info natives
- **DODX stats natives:** `get_user_wstats`, `get_user_astats`, `get_user_vstats`, etc.
- **DODX game natives:** `xmod_is_melee_wpn`, `xmod_get_wpnname`, `dod_get_team_score`
- `read_data` for event data

**Forwards Used:** `plugin_init`, `plugin_cfg`, `client_putinserver`, `client_disconnected`, **`client_damage`**, **`client_death`**

**Module Dependencies:** DODX (required)

---

### Other Analyzed Plugins
- **timeleft.sma** - Time display, HUD messages, repeating tasks
- **nextmap.sma** - Map management, `register_event` for Event 30, `engine_changelevel`
- **antiflood.sma** - Minimal dependencies
- **mapchooser.sma** - `register_event` for TeamScore, Array operations
- **adminchat.sma** - HUD messages, language natives
- **dod/stats_logging.sma** - DODX stats, logging
- **dod/plmenu.sma** - DODX, Fun, Engine modules
- **dod/statssounds.sma** - `plugin_precache`, `precache_generic`
- **dod/dodstats.sma** - Simple rank calculation using DODX constants

---

## Changelog
- 2025-12-05: Initial creation, analyzed 4 custom plugins + admin.sma
- 2025-12-05: Added comprehensive native lists by category
- 2025-12-05: Documented critical forwards and dependencies
- 2025-12-05: Added DoD plugin analysis
- 2025-12-05: Added DODX module natives and forwards summary
- 2025-12-05: Added core AMXX plugin analysis
- 2025-12-05: Added Engine, Fun, CStrike, Array/Trie natives
- 2025-12-05: Added event registration summary
- 2025-12-05: Updated Master Native List with all categories
- 2025-12-05: Reorganized document structure - moved implementation notes, hooks, timing issues to top
- 2025-12-05: Consolidated all native lists, forward lists, and module dependencies
- 2025-12-05: Updated progress tracking to reflect completed analysis phases
- 2025-12-05: Fixed UserMsg errors - updated Known Issues and added TODO for proper message hook system
- 2025-12-05: Marked confirmed natives as OK: is_user_connected, is_user_alive, is_user_bot, is_user_hltv, set_task, remove_task, task_exists, query_client_cvar
- 2025-12-05: Marked confirmed forwards as OK: plugin_init, plugin_cfg, plugin_precache, client_connect, client_authorized, client_putinserver, client_disconnect, client_disconnected, client_cvar_changed
- 2025-12-05: Removed debug logging from amxmodx.cpp, CForward.cpp, CMisc.cpp, CTask.cpp, meta_api.cpp
- 2025-12-05: **FIXED MESSAGE SYSTEM** - Modified KTPReHLDS `RegUserMsg_internal` to search both `sv_gpUserMsgs` AND `sv_gpNewUserMsgs` lists
- 2025-12-05: Message IDs now correctly looked up via `REG_USER_MSG` in extension mode (returns existing IDs instead of creating duplicates)
- 2025-12-05: Marked communication natives as OK: client_print, server_cmd, server_exec, server_print, console_print, engclient_print
- 2025-12-05: Updated Known Issues section - all major issues now RESOLVED
- 2025-12-05: Bulk marked working natives as OK based on successful plugin operation:
  - Registration: register_plugin, register_cvar, register_clcmd, register_concmd, register_srvcmd, register_dictionary
  - Player info: get_user_name, get_user_authid, get_user_ip, get_user_userid, get_user_flags, get_user_team, get_players, get_playersnum, MaxClients
  - CVARs: get_cvar_string, get_cvar_num, get_cvar_float, get_cvar_pointer, get_pcvar_string, get_pcvar_num, get_pcvar_float
  - Communication: client_cmd
  - File I/O: fopen, fclose, fgets, fputs, feof, file_exists, read_file, write_file
  - Logging: log_amx, log_to_file, log_message
  - Special: get_configsdir, get_localinfo, get_mapname, get_systime, get_time, get_timeleft, get_gametime
  - String: formatex, format, copy, strlen, contain, containi, equal, equali, trim, add, parse, str_to_num, floatstr, floatabs, floatround, read_argv, read_argc, read_args
- 2025-12-06: **FIXED CLIENT COMMANDS & HUD** - Added SV_ClientCommand hookchain to KTPReHLDS and handler to KTPAMXX
  - Fixed vtable alignment issue between KTPAMXX and KTPReHLDS headers (added 20+ missing virtual methods)
  - Marked as OK: register_clcmd, register_menucmd, register_menuid, show_menu, client_command forward
  - Marked HUD natives as OK: CreateHudSyncObj, set_hudmessage, show_hudmessage, ShowSyncHudMsg, ClearSyncHud
  - Chat commands (/start, .start, etc.) and menu selections now work in extension mode
- 2025-12-06: **FIXED MAP CHANGE** - Clients now persist through map changes with proper AMXX forward firing
  - Added SV_InactivateClients hookchain to KTPReHLDS for map change deactivation
  - Added SV_InactivateClients_RH handler to fire plugin_end and client_disconnect forwards
  - Fixed SV_Spawn_f_RH hook registration (function existed but was never registered!)
  - SV_Spawn_f_RH now handles client reinitialization after map change reconnect
  - Marked plugin_end forward as OK
  - Cvar checker and other plugins now work correctly after map change
