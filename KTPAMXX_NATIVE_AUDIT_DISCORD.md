# KTPAMXX Extension Mode - Native & Forward Audit
> Last Updated: 2025-12-06

## Quick Reference
When context resets, check:
- `amxmodx/meta_api.cpp` - Extension mode initialization
- `amxmodx/amxmodx.cpp` - Native implementations
- `amxmodx/CPlayer.cpp` - Player data structures

---

## Status Legend
```
OK       = Works in extension mode
BROKEN   = Does not work
PARTIAL  = Partially works
UNTESTED = Not yet verified
N/A      = Not applicable
```

---

## Required Hooks (Extension Mode)

### Connection & Lifecycle
- **ClientConnected** → `client_connect` forward
- **SV_DropClient** → `client_disconnect` forward
- **Steam_NotifyClientConnect** → `client_authorized` forward

### Server Events
- **SV_ActivateServer** → `plugin_init`, `plugin_cfg`
- **SV_InactivateClients** → `plugin_end`, `client_disconnect` (map change)
- **SV_Frame** → `client_putinserver` detection

### Client Processing
- **SV_ClientCommand** → `register_clcmd`, menus, `client_command`
- **SV_Spawn_f** → client reconnect after map change
- **SV_ClientUserInfoChanged** → `client_infochanged`

### Engine
- **PF_RegUserMsg_I** → HUD messages, `client_print`
- **pfnClientCvarChanged** → `client_cvar_changed`
- **PF_changelevel_I** → `server_changelevel` (KTPReHLDS 3.16+, not yet used)
- **PF_setmodel_I** → entity model tracking (KTPReHLDS 3.16+, not yet used)

---

## Resolved Issues

### 1. User Message Registration
**Problem:** `REG_USER_MSG` created duplicates with IDs 130+
**Fix:** KTPReHLDS searches both `sv_gpUserMsgs` AND `sv_gpNewUserMsgs`

### 2. Client Commands Not Working
**Problem:** Chat commands and menus didn't work
**Fix:** Added `SV_ClientCommand` hookchain

### 3. Map Change Breaks Forwards
**Problem:** `client_putinserver` didn't fire after map change
**Fix:** Added `SV_InactivateClients` + `SV_Spawn_f` hooks

---

## Map Change Sequence
```
1. SV_InactivateClients() → Fire disconnect forwards, clear state
2. SV_ServerShutdown()    → Shut down current map
3. SV_SpawnServer()       → Load new map
4. SV_ActivateServer()    → Fire plugin_init, plugin_cfg
5. SV_Spawn_f()           → Clients reconnect, fire client_connect/putinserver
```

---

## Core Forwards Status

### Plugin Lifecycle
```diff
+ plugin_init       OK - SV_ActivateServer
+ plugin_cfg        OK - After plugin_init
+ plugin_precache   OK - SV_Spawn (pre-activation)
+ plugin_end        OK - SV_InactivateClients
- plugin_natives    UNTESTED
```

### Client Forwards
```diff
+ client_connect       OK - ClientConnected / SV_Spawn_f
+ client_authorized    OK - Steam_NotifyClientConnect
+ client_putinserver   OK - SV_Frame / SV_Spawn_f
+ client_disconnect    OK - SV_DropClient
+ client_disconnected  OK - SV_DropClient
+ client_command       OK - SV_ClientCommand
+ client_cvar_changed  OK - pfnClientCvarChanged (KTP custom)
- client_infochanged   UNTESTED
```

---

## Native Status by Category

### Registration (All OK)
`register_plugin`, `register_cvar`, `register_clcmd`, `register_concmd`, `register_srvcmd`, `register_dictionary`, `register_menucmd`, `register_menuid`

### Player Info (All OK)
`get_user_name`, `get_user_authid`, `get_user_userid`, `get_user_ip`, `get_user_flags`, `get_user_team`, `is_user_connected`, `is_user_alive`, `is_user_bot`, `is_user_hltv`, `get_players`, `get_playersnum`, `MaxClients`

### CVARs (All OK)
`register_cvar`, `get_cvar_string`, `get_cvar_num`, `get_cvar_float`, `get_cvar_pointer`, `get_pcvar_string`, `get_pcvar_num`, `get_pcvar_float`

### Communication (All OK)
`client_print`, `client_cmd`, `server_cmd`, `server_exec`, `server_print`, `console_print`, `engclient_print`, `show_menu`

### HUD (All OK)
`CreateHudSyncObj`, `set_hudmessage`, `show_hudmessage`, `ShowSyncHudMsg`, `ClearSyncHud`

### Tasks (All OK)
`set_task`, `remove_task`, `task_exists`

### Files (All OK)
`fopen`, `fclose`, `fgets`, `fputs`, `feof`, `file_exists`, `read_file`, `write_file`

### Logging (All OK)
`log_amx`, `log_to_file`, `log_message`

### Special (All OK)
`query_client_cvar`, `get_configsdir`, `get_localinfo`, `get_mapname`, `get_systime`, `get_time`, `get_timeleft`, `get_gametime`

### Strings (All OK)
`formatex`, `format`, `copy`, `strlen`, `contain`, `containi`, `equal`, `equali`, `trim`, `add`, `parse`, `str_to_num`, `floatstr`, `floatabs`, `floatround`, `read_argv`, `read_argc`, `read_args`

---

## Module Dependencies

| Module | Status | Notes |
|--------|--------|-------|
| DODX | UNTESTED | DoD stats |
| Fun | UNTESTED | user_slap, user_kill |
| Engine | UNTESTED | engclient_cmd, changelevel |
| Fakemeta | UNTESTED | pev, set_pev |
| CStrike | UNTESTED | CS-specific |
| SQL/SQLite | UNTESTED | Database |
| cURL | UNTESTED | HTTP |
| ReAPI | UNTESTED | ReHLDS/ReGameDLL |

---

## Priority Plugins

### Custom KTP (HIGH)
1. **KTPCvarChecker** - Real-time cvar validation
2. **KTPMatchHandler** - Match management, HUD
3. **KTPAdminAudit** - Admin kick auditing
4. **KTPFileChecker** - File consistency

### Core AMXX (HIGH)
1. **admin.sma** - Admin system
2. **admincmd.sma** - Admin commands
3. **plmenu.sma** - Player menu
4. **timeleft.sma** - Time display

### DoD-Specific (HIGH)
1. **dod/stats.sma** - Statistics
2. **dod/plmenu.sma** - Player menu
3. **dod/stats_logging.sma** - Stats logging

---

## Changelog

**2025-12-06:** Map change fix - Added `SV_InactivateClients` + `SV_Spawn_f` hooks
**2025-12-06:** Client commands - Added `SV_ClientCommand` hook, fixed vtable alignment
**2025-12-05:** Message system - Fixed `REG_USER_MSG` in KTPReHLDS
**2025-12-05:** Initial audit - Core natives and forwards verified
