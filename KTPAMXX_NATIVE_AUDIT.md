# KTPAMXX Native Audit - Extension Mode Compatibility

> **Version:** 2.4.0 | **Last Updated:** 2025-12-16 | **Status:** Active Development

[![Extension Mode](https://img.shields.io/badge/Extension%20Mode-Supported-brightgreen)](#)
[![Map Change](https://img.shields.io/badge/Map%20Change-Fixed-brightgreen)](#)
[![Client Commands](https://img.shields.io/badge/Client%20Commands-Working-brightgreen)](#)
[![Register Event](https://img.shields.io/badge/Register%20Event-Working-brightgreen)](#)

---

## Overview

This document tracks all native functions used by plugins to ensure KTPAMXX works correctly in **ReHLDS extension mode** (without Metamod). Each native is tracked with its implementation status and dependencies.

### Quick Reference for Context Recovery

When context resets, check:
1. **This file** for native/forward status
2. `amxmodx/meta_api.cpp` - Extension mode initialization
3. `amxmodx/amxmodx.cpp` - Native implementations
4. `amxmodx/CPlayer.cpp` - Player data structures

---

## Status Legend

| Status | Badge | Meaning |
|--------|-------|---------|
| OK | ![OK](https://img.shields.io/badge/-OK-brightgreen) | Works in extension mode |
| PARTIAL | ![PARTIAL](https://img.shields.io/badge/-PARTIAL-yellow) | Partially works |
| BROKEN | ![BROKEN](https://img.shields.io/badge/-BROKEN-red) | Does not work |
| UNTESTED | ![UNTESTED](https://img.shields.io/badge/-UNTESTED-lightgrey) | Not yet verified |
| N/A | ![N/A](https://img.shields.io/badge/-N%2FA-inactive) | Not applicable |

---

## Required ReHLDS Hooks

These hooks must be registered for extension mode to function:

### Connection & Lifecycle
| Hook | Purpose | Triggers |
|:-----|:--------|:---------|
| `ClientConnected` | Player connection | `client_connect` forward |
| `SV_DropClient` | Player disconnection | `client_disconnect` forward |
| `Steam_NotifyClientConnect` | Authorization | `client_authorized` forward |

### Server Events
| Hook | Purpose | Triggers |
|:-----|:--------|:---------|
| `SV_ActivateServer` | Server activation | `plugin_init`, `plugin_cfg` |
| `SV_InactivateClients` | Map change deactivation | `plugin_end`, `client_disconnect` |
| `SV_Frame` | Per-frame processing | `client_putinserver` detection |

### Client Processing
| Hook | Purpose | Triggers |
|:-----|:--------|:---------|
| `SV_ClientCommand` | Command processing | `register_clcmd`, menus, `client_command` |
| `SV_Spawn_f` | Client spawn command | `client_connect`, `client_putinserver` (map change) |
| `SV_ClientUserInfoChanged` | Client info changes | `client_infochanged` |

### Engine Functions
| Hook | Purpose | Triggers |
|:-----|:--------|:---------|
| `PF_RegUserMsg_I` | Message ID capture | HUD drawing, `client_print` |
| `pfnClientCvarChanged` | Cvar change callback | `client_cvar_changed` |
| `PF_changelevel_I` | Level change (3.16+) | `server_changelevel` (not yet used) |
| `PF_setmodel_I` | Entity setmodel (3.16+) | Model tracking (not yet used) |
| `IMessageManager` | Message interception | `register_event` (extension mode) |
| `AlertMessage` | Log message hook | `register_logevent` (extension mode) |

---

## Resolved Issues

### 1. User Message Registration ![FIXED](https://img.shields.io/badge/-FIXED-brightgreen)

**Problem:** `REG_USER_MSG` created duplicate messages with IDs 130+

**Solution:** KTPReHLDS `RegUserMsg_internal` now searches both:
- `sv_gpUserMsgs` (messages already sent to clients)
- `sv_gpNewUserMsgs` (newly registered messages)

### 2. Client Commands Not Working ![FIXED](https://img.shields.io/badge/-FIXED-brightgreen)

**Problem:** Chat commands (`/start`, `.start`) and menus didn't work

**Solution:** Added `SV_ClientCommand` hookchain to KTPReHLDS

### 3. Map Change Breaks Forwards ![FIXED](https://img.shields.io/badge/-FIXED-brightgreen)

**Problem:** `client_putinserver` didn't fire after map change

**Solution:** Added `SV_InactivateClients` + `SV_Spawn_f` hooks

### 4. register_event Not Working in Extension Mode ![FIXED](https://img.shields.io/badge/-FIXED-brightgreen)

**Problem:** `register_event` didn't work in extension mode because Metamod's engine message hooks weren't available

**Solution:** Integrated `IMessageManager` from KTPReHLDS API:
- Added `GetMessageManager()` to ReHLDS API interface
- Register per-message-ID hooks via `IMessageManager::registerHook()`
- `MessageHook_Handler` parses message parameters and fires AMXX event callbacks
- Hooks installed on-demand when `register_event` is called

### 5. register_logevent Not Working in Extension Mode ![FIXED](https://img.shields.io/badge/-FIXED-brightgreen)

**Problem:** `register_logevent` didn't work in extension mode because Metamod's `pfnAlertMessage` hook wasn't available

**Solution:** Added `AlertMessage` hookchain to KTPReHLDS:
- New hookchain intercepts all `AlertMessage` calls with pre-formatted message
- `AlertMessage_RH` in KTPAMXX filters for `at_logged` messages
- Calls `g_logevents.setLogString()`, `parseLogString()`, and `executeLogEvents()`
- Also fires `plugin_log` forward

---

## Map Change Implementation

### Sequence Diagram

```
┌─────────────────────────────────────────────────────────────────┐
│                     MAP CHANGE SEQUENCE                         │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  1. SV_InactivateClients()                                      │
│     ├── Fire client_disconnect forwards                         │
│     ├── Fire plugin_end forward                                 │
│     └── Clear AMXX player state                                 │
│                           ↓                                     │
│  2. SV_ServerShutdown()                                         │
│     └── Shut down current map                                   │
│                           ↓                                     │
│  3. SV_SpawnServer()                                            │
│     └── Load new map                                            │
│                           ↓                                     │
│  4. SV_ActivateServer()                                         │
│     ├── Fire plugin_init forward                                │
│     └── Fire plugin_cfg forward                                 │
│                           ↓                                     │
│  5. SV_Spawn_f() (per client)                                   │
│     ├── Check if !pPlayer->initialized                          │
│     ├── Call pPlayer->Connect()                                 │
│     ├── Fire client_connect forward                             │
│     ├── Call pPlayer->PutInServer()                             │
│     └── Fire client_putinserver forward                         │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

### Key Insight

The `SV_Spawn_f` hook is critical because it fires for **BOTH**:
- Initial client connections (after `ClientConnected`)
- Map change reconnections (where `ClientConnected` does NOT fire)

By checking `pPlayer->initialized`, we detect if reinitialization is needed.

---

## Forwards Status

### Core Forwards

| Forward | Status | Hook Required | Used By |
|:--------|:------:|:--------------|:--------|
| `plugin_init` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | SV_ActivateServer | All plugins |
| `plugin_cfg` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | After plugin_init | Config loading |
| `plugin_precache` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | SV_Spawn | Precaching |
| `plugin_end` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | SV_InactivateClients | Cleanup |
| `plugin_natives` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | Before plugin_init | Native registration |

### Client Forwards

| Forward | Status | Hook Required | Used By |
|:--------|:------:|:--------------|:--------|
| `client_connect` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | ClientConnected / SV_Spawn_f | Connection handling |
| `client_authorized` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | Steam_NotifyClientConnect | Auth checks |
| `client_putinserver` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | SV_Frame / SV_Spawn_f | Player init |
| `client_disconnect` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | SV_DropClient | Disconnect handling |
| `client_disconnected` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | SV_DropClient | Post-disconnect |
| `client_command` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | SV_ClientCommand | Command processing |
| `client_infochanged` | ![N/A](https://img.shields.io/badge/-N%2FA-inactive) | SV_ClientUserInfoChanged | Not needed |

### Custom KTP Forwards

| Forward | Status | Hook Required | Used By |
|:--------|:------:|:--------------|:--------|
| `client_cvar_changed` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | pfnClientCvarChanged | KTPCvarChecker |
| `inconsistent_file` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | SV_CheckConsistencyResponse | KTPFileChecker |

---

## Native Status by Category

### Registration Natives

| Native | Status | Notes |
|:-------|:------:|:------|
| `register_plugin` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | Core functionality |
| `register_cvar` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | CVAR system |
| `register_clcmd` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | Client commands |
| `register_concmd` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | Console commands |
| `register_srvcmd` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | Server commands |
| `register_dictionary` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | Language files |
| `register_menucmd` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | Menu handlers |
| `register_menuid` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | Menu IDs |
| `register_event` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | Event handlers (via IMessageManager) |
| `register_logevent` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | Log events (via AlertMessage hook) |

### Player Info Natives

| Native | Status | Notes |
|:-------|:------:|:------|
| `get_user_name` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | Player name |
| `get_user_authid` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | Steam ID |
| `get_user_userid` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | User ID |
| `get_user_ip` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | IP address |
| `get_user_flags` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | Admin flags |
| `get_user_team` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | Team number |
| `is_user_connected` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | Connection status |
| `is_user_alive` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | Alive status |
| `is_user_bot` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | Bot detection |
| `is_user_hltv` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | HLTV detection |
| `get_players` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | Player list |
| `get_playersnum` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | Player count |
| `MaxClients` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | Max clients |

### CVAR Natives

| Native | Status | Notes |
|:-------|:------:|:------|
| `get_cvar_string` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | String value |
| `get_cvar_num` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | Integer value |
| `get_cvar_float` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | Float value |
| `get_cvar_pointer` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | Pointer access |
| `get_pcvar_string` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | Direct string |
| `get_pcvar_num` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | Direct integer |
| `get_pcvar_float` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | Direct float |
| `set_cvar_*` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | Set operations |
| `set_pcvar_*` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | Direct set |

### Communication Natives

| Native | Status | Notes |
|:-------|:------:|:------|
| `client_print` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | Print to client |
| `client_cmd` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | Execute on client |
| `server_cmd` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | Server command |
| `server_exec` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | Execute server |
| `server_print` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | Console output |
| `console_print` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | Console print |
| `engclient_print` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | Engine print |
| `show_menu` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | Display menu |

### HUD Natives

| Native | Status | Notes |
|:-------|:------:|:------|
| `CreateHudSyncObj` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | Sync object |
| `set_hudmessage` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | Configure HUD |
| `show_hudmessage` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | Display HUD |
| `ShowSyncHudMsg` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | Synced HUD |
| `ClearSyncHud` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | Clear HUD |

### Task Natives

| Native | Status | Notes |
|:-------|:------:|:------|
| `set_task` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | Schedule task |
| `remove_task` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | Cancel task |
| `task_exists` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | Check task |

### File I/O Natives

| Native | Status | Notes |
|:-------|:------:|:------|
| `fopen` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | Open file |
| `fclose` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | Close file |
| `fgets` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | Read line |
| `fputs` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | Write line |
| `feof` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | End of file |
| `file_exists` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | Check exists |
| `read_file` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | Read file |
| `write_file` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | Write file |

### Logging Natives

| Native | Status | Notes |
|:-------|:------:|:------|
| `log_amx` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | AMX log |
| `log_to_file` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | File log |
| `log_message` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | Message log |

### String/Utility Natives

| Native | Status | Notes |
|:-------|:------:|:------|
| `formatex` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | Format string |
| `format` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | Format string |
| `copy` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | Copy string |
| `strlen` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | String length |
| `contain` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | Find substring |
| `containi` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | Case-insensitive |
| `equal` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | Compare |
| `equali` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | Case-insensitive |
| `trim` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | Trim whitespace |
| `str_to_num` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | String to int |
| `floatstr` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | String to float |
| `read_argv` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | Read argument |
| `read_argc` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | Argument count |
| `read_args` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | All arguments |

### Special Natives

| Native | Status | Notes |
|:-------|:------:|:------|
| `query_client_cvar` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | Query cvar |
| `get_configsdir` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | Config path |
| `get_localinfo` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | Local info |
| `get_mapname` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | Current map |
| `get_systime` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | System time |
| `get_time` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | Formatted time |
| `get_timeleft` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | Time remaining |
| `get_gametime` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | Game time |

---

## Module Dependencies

| Module | Status | Required For | Notes |
|:-------|:------:|:-------------|:------|
| amxxcurl | ![OK](https://img.shields.io/badge/-OK-brightgreen) | HTTP requests | Uses frame callbacks in extension mode |
| ReAPI | ![OK](https://img.shields.io/badge/-OK-brightgreen) | ReHLDS/ReGameDLL hooks | Extension mode supported |
| DODX | ![OK](https://img.shields.io/badge/-OK-brightgreen) | DoD stats, weapons | Extension mode working - PreThink, changelevel, TraceLine, 16 message hooks |
| SQLite | ![BROKEN](https://img.shields.io/badge/-BROKEN-red) | Database | Crashes on load - Metamod hooks incompatible |
| MySQL | ![UNTESTED](https://img.shields.io/badge/-UNTESTED-lightgrey) | Database | Needs extension mode testing |
| Fun | ![UNTESTED](https://img.shields.io/badge/-UNTESTED-lightgrey) | user_slap, user_kill | |
| Engine | ![UNTESTED](https://img.shields.io/badge/-UNTESTED-lightgrey) | engclient_cmd, changelevel | |
| Fakemeta | ![UNTESTED](https://img.shields.io/badge/-UNTESTED-lightgrey) | pev, set_pev | |
| CStrike | ![UNTESTED](https://img.shields.io/badge/-UNTESTED-lightgrey) | CS-specific natives | |
| Hamsandwich | ![UNTESTED](https://img.shields.io/badge/-UNTESTED-lightgrey) | Function hooking | |

> **Note:** The `client_cvar_changed` forward is a **KTP-native feature** built directly into KTPAMXX, not from the ReAPI module. It hooks the KTPReHLDS `pfnClientCvarChanged` callback.

### Module Extension Mode Status Details

**Working Modules:**
- **amxxcurl**: Uses `MF_RegModuleFrameFunc()` for async polling instead of Metamod's `StartFrame`
- **ReAPI**: Has dedicated `extension_mode.cpp` with ReHLDS hookchain support

**Fully Working Modules:**
- **DODX**: Extension mode fully functional. See [DODX Extension Mode Hooks](#dodx-extension-mode-hooks) for details.
  - All player stats natives work (`get_user_wstats`, `get_user_rstats`, etc.)
  - Shot tracking via button state detection (IN_ATTACK monitoring in PreThink)
  - TraceLine hook enables hit detection and aiming statistics
  - All pEdict accesses hardened with null/free checks
  - All ENTINDEX calls use ENTINDEX_SAFE
  - Disabled DODX's `get_user_team` native (was overriding core AMXX and crashing)
  - ClientConnected hook not needed (bot detection uses FL_FAKECLIENT, IP never used)

**Broken Modules (Crash on Load):**
- **SQLite**: Has Metamod hooks that are incompatible with extension mode. Crashes during module initialization.

**Needs Investigation:**
- **MySQL**: Similar to SQLite, likely has Metamod dependencies. Needs testing before production use.

---

## DODX Extension Mode Hooks

The DODX module requires ReHLDS hooks to function in extension mode. Hooks have been carefully analyzed and enabled as needed.

### Active Hooks

| Hook | Status | Purpose | Notes |
|:-----|:------:|:--------|:------|
| SV_PlayerRunPreThink | ![OK](https://img.shields.io/badge/-OK-brightgreen) | Stats tracking loop | Also handles lazy player initialization |
| PF_changelevel_I | ![OK](https://img.shields.io/badge/-OK-brightgreen) | Pre-changelevel cleanup | Disables message processing before map change |
| PF_TraceLine | ![OK](https://img.shields.io/badge/-OK-brightgreen) | Hit detection/aiming | POST hook only - reads results, safe for wallpen |
| IMessageManager | ![OK](https://img.shields.io/badge/-OK-brightgreen) | Message interception | 16 message hooks for game stats |

### Disabled/Not Needed Hooks

| Hook | Status | Purpose | Reason |
|:-----|:------:|:--------|:-------|
| RegUserMsg | ![N/A](https://img.shields.io/badge/-N%2FA-inactive) | Message ID capture | Using `MF_GetUserMsgId()` lookup instead |
| SetClientKeyValue | ![DISABLED](https://img.shields.io/badge/-DISABLED-red) | Player model override | Not wanted - custom player models disabled |
| ClientConnected | ![N/A](https://img.shields.io/badge/-N%2FA-inactive) | Player initialization | IP stored but never used by DODX |
| SV_Spawn_f | ![N/A](https://img.shields.io/badge/-N%2FA-inactive) | PutInServer equivalent | Player init done in PreThink instead |
| SV_DropClient | ![N/A](https://img.shields.io/badge/-N%2FA-inactive) | Player disconnect | Core AMXX handles disconnect |

### IMessageManager Hooks (16 total)

Stats tracking intercepts these game messages:

| Message | Purpose |
|:--------|:--------|
| CurWeapon | Current weapon tracking |
| ObjScore | Objective score updates |
| RoundState | Round state changes |
| Health | Damage/death detection |
| ResetHUD | Stats reset on spawn |
| TeamScore | Team score tracking |
| AmmoX / AmmoShort | Ammo tracking |
| SetFOV | Scope detection |
| Object | Objective pickup/drop |
| PStatus | Player spawn detection |
| ScoreShort / PTeam | Score/team tracking |

### Known Issues Fixed

1. **DODX `get_user_team` Override Crash** - DODX registered its own `get_user_team` native which overrode core AMXX's implementation. The DODX version crashed in extension mode. Fixed by disabling registration in `NBase.cpp`.

2. **ClientConnected Hook Not Needed** - Analysis revealed the CPlayer::bot flag is determined via FL_FAKECLIENT check (not from the hook) and CPlayer::ip is stored but never read anywhere in DODX. Hook disabled - no functional impact on stats.

3. **Map Change Crash** - Fixed by adding `PF_changelevel_I` hook that sets `g_bServerActive = false` and clears `g_pFirstEdict` before changelevel occurs, preventing message handlers from using stale pointers.

4. **stats_logging.sma Disconnect Crash** - `get_user_wstats` called during `client_disconnected` crashed because edict was already freed. Fixed by hardening `CHECK_PLAYER` macro in `dodx.h` to check `edict->free` before calling `FNullEnt()`.

5. **ENTINDEX_SAFE Conversion** - All raw `ENTINDEX()` calls converted to `ENTINDEX_SAFE()` which uses pointer arithmetic instead of engine function calls. Prevents crashes from invalid edict pointers.

6. **pEdict Access Hardening** - All pEdict accesses throughout DODX now have proper `if (!pEdict || pEdict->free)` guards to prevent crashes from stale or invalid pointers.

---

## Priority Plugins

### KTP Custom Plugins (High Priority)

| Plugin | Description | Status |
|:-------|:------------|:------:|
| KTPCvarChecker | Real-time cvar validation | ![OK](https://img.shields.io/badge/-OK-brightgreen) |
| KTPMatchHandler | Match management, HUD | ![OK](https://img.shields.io/badge/-OK-brightgreen) |
| KTPAdminAudit | Admin kick auditing | ![OK](https://img.shields.io/badge/-OK-brightgreen) |
| KTPFileChecker | File consistency | ![UNTESTED](https://img.shields.io/badge/-UNTESTED-lightgrey) |

### Core AMXX Plugins (High Priority)

| Plugin | Description | Status |
|:-------|:------------|:------:|
| admin.sma | Admin system | ![UNTESTED](https://img.shields.io/badge/-UNTESTED-lightgrey) |
| admincmd.sma | Admin commands | ![UNTESTED](https://img.shields.io/badge/-UNTESTED-lightgrey) |
| plmenu.sma | Player menu | ![UNTESTED](https://img.shields.io/badge/-UNTESTED-lightgrey) |
| timeleft.sma | Time display | ![UNTESTED](https://img.shields.io/badge/-UNTESTED-lightgrey) |

### DoD Plugins (Medium Priority)

| Plugin | Description | Status |
|:-------|:------------|:------:|
| dod/stats.sma | Statistics | ![UNTESTED](https://img.shields.io/badge/-UNTESTED-lightgrey) |
| dod/plmenu.sma | DoD player menu | ![UNTESTED](https://img.shields.io/badge/-UNTESTED-lightgrey) |
| dod/stats_logging.sma | Stats logging | ![OK](https://img.shields.io/badge/-OK-brightgreen) |

---

## Changelog

<details>
<summary><strong>2025-12-16</strong> - DODX Extension Mode Complete Rewrite</summary>

- **DODX EXTENSION MODE HOOKS** - Complete set of ReHLDS hook handlers
  - `DODX_OnPlayerPreThink` - Main stats tracking loop (replaces FN_PlayerPreThink_Post)
  - `DODX_OnClientConnected` - Player connection (replaces FN_ClientConnect_Post)
  - `DODX_OnSV_Spawn_f` - Player spawn (replaces FN_ClientPutInServer_Post)
  - `DODX_OnSV_DropClient` - Player disconnect (replaces FN_ClientDisconnect)
  - `DODX_OnChangelevel` - Pre-changelevel cleanup
  - `DODX_OnTraceLine` - Hit detection and aiming
  - IMessageManager hooks for 16 game message types

- **ENTINDEX_SAFE IMPLEMENTATION** - Safe entity index calculation
  - New inline function uses pointer arithmetic instead of engine calls
  - New `g_pFirstEdict` global cached in ServerActivate_Post
  - `GET_PLAYER_POINTER` macro updated to use ENTINDEX_SAFE
  - Prevents crashes from calling engine functions during ReHLDS hooks

- **g_bServerActive FLAG** - Map change protection
  - Tracks whether server is in valid state for processing
  - Set true in ServerActivate_Post, false in ServerDeactivate and OnChangelevel
  - Prevents message hooks from using stale pointers during map changes

- **STATS NATIVES SAFETY HARDENING** - All stats natives now have:
  - `gpGlobals` NULL check (can be NULL during map change)
  - Player index range validation
  - `pEdict` and `pEdict->free` checks before access
  - `pPlayer->rank` NULL checks (rank not used in extension mode)
  - Hardened: get_user_astats, get_user_vstats, get_user_wstats, get_user_wlstats,
    get_user_wrstats, get_user_stats, get_user_lstats, get_user_rstats, reset_user_wstats

- **CHECK_PLAYER MACRO REWRITE** - Safer player validation
  - Uses `players[]` array directly instead of MF_IsPlayerIngame/MF_GetPlayerEdict
  - Checks `pEdict->free` before calling `FNullEnt()`
  - Prevents crashes when player edict freed during disconnect

- **DODX SHOT TRACKING** - Button state shot detection
  - `CheckShotFired()` monitors IN_ATTACK button in PreThink
  - Detects rising edge (new shots) and held attack (automatic weapons)
  - Per-weapon fire rate delays for accurate shot counting
  - New CPlayer fields: oldbuttons, lastShotTime, nextShotTime

- **MODULE SDK EXTENSIONS** - New functions for extension mode modules
  - `MF_GetEngineFuncs()` - Returns pointer to engine function table
  - `MF_GetGlobalVars()` - Returns pointer to gpGlobals
  - `MF_GetUserMsgId()` - Message ID lookup by name
  - `MF_RegModuleMsgHandler()` / `MF_UnregModuleMsgHandler()` - Message handler registration
  - `MF_RegModuleMsgBeginHandler()` - Message begin handler

- **DODX DEFERRED INITIALIZATION** - Extension mode init timing
  - Cvar registration moved from OnAmxxAttach to OnPluginsLoaded
  - Message ID lookup via MF_GetUserMsgId instead of engine calls
  - Player init via PreThink hook (lazy initialization)

- **LOG FILE FIX** - Fixed log rotation in stats_logging.sma

- **DEBUG CLEANUP** - Removed all verbose debug statements

</details>

<details>
<summary><strong>2025-12-14</strong> - DODX Extension Mode Complete</summary>

- **DODX NOW FULLY FUNCTIONAL** - All hooks enabled and working in extension mode
  - SV_PlayerRunPreThink - Stats tracking loop with lazy player init
  - PF_changelevel_I - Pre-changelevel cleanup prevents stale pointer crashes
  - PF_TraceLine - Hit detection/aiming (POST hook only, safe for wallpen)
  - IMessageManager - 16 message hooks for comprehensive stats tracking

- **STATS_LOGGING CRASH FIX** - `get_user_wstats` called during `client_disconnected` crashed
  - Root cause: Edict already freed when disconnect forward fires
  - Solution: Hardened `CHECK_PLAYER` macro in `dodx.h` to check `edict->free` before `FNullEnt()`

- **STATS_LOGGING VERIFIED WORKING** - Plugin tested and confirmed functional
  - Logs `weaponstats`, `weaponstats2`, `time`, and `latency` on player disconnect
  - Output correctly written to HLDS log files for stats parsers
  - Added `set_task(1.0, "enable_logging")` to force logging on after server startup

- **ENTINDEX_SAFE AUDIT** - Verified all ENTINDEX calls use safe version
  - All raw `ENTINDEX()` calls converted to `ENTINDEX_SAFE()`
  - Uses pointer arithmetic (`pEdict - g_pFirstEdict`) instead of engine function
  - Prevents crashes from invalid edict pointers

- **pEdict ACCESS HARDENING** - All pEdict accesses now have proper guards
  - Pattern: `if (!pEdict || pEdict->free)` before any edict access
  - Prevents crashes from stale or invalid pointers during map changes

</details>

<details>
<summary><strong>2025-12-13</strong> - DODX Crash Fix & Hook Analysis</summary>

- **ROOT CAUSE IDENTIFIED** - `get_user_team` crash was caused by DODX module override
  - DODX registered its own `get_user_team` native before core AMXX
  - Module natives are registered first (line 503-513 in modules.cpp), then core AMXX
  - The DODX version crashed in extension mode due to missing message hooks
  - **Fix:** Disabled DODX's `get_user_team` registration in `NBase.cpp:656-657`

- **ClientConnected HOOK NOT NEEDED** - Analysis revealed hook provides no value
  - `CPlayer::bot` flag is never used - `ignoreBots()` checks `FL_FAKECLIENT` flag directly
  - `CPlayer::ip` field is never read anywhere in DODX
  - Hook disabled permanently - no functional impact on stats

- **CLEANED UP DEBUG LOGGING** - Removed ~80 debug log messages from DODX moduleconfig.cpp
  - Only 2 essential messages remain (startup and init complete)
  - Removed verbose hook registration/message ID logging
  - Simplified extension hook setup functions

- **UPDATED DOCUMENTATION** - Added DODX Extension Mode Hooks section to track incremental progress

</details>

<details>
<summary><strong>2025-12-08</strong> - Module Extension Mode Testing</summary>

- **TESTED MODULE COMPATIBILITY** - Verified which modules work in extension mode
  - **amxxcurl**: ✅ Working - uses `MF_RegModuleFrameFunc()` for async polling
  - **ReAPI**: ✅ Working - has dedicated `extension_mode.cpp` with ReHLDS hookchain support
  - **SQLite**: ❌ Crashes - Metamod hooks incompatible with extension mode
  - **DODX**: ❌ Crashes - Extension hooks disabled, needs further debugging
  - **MySQL**: ⚠️ Untested - likely same issues as SQLite

- **DISABLED DODX EXTENSION HOOKS** - Temporarily disabled via `#if 0` in `moduleconfig.cpp`
  - Hooks were crashing during initialization
  - Basic DODX natives may not work without message interception

- **FIXED BUILD NAMES** - Updated build configs to output `_ktp_i386.so`:
  - `KTPAmxxCurl/premake5.lua` and `AmxxCurl.make`: `amxxcurl_ktp_i386.so`
  - `KTPReAPI/reapi/CMakeLists.txt`: `reapi_ktp_i386.so`

</details>

<details>
<summary><strong>2025-12-07</strong> - register_event and register_logevent Extension Mode Support</summary>

- **ADDED IMessageManager INTEGRATION** - `register_event` now works in extension mode
  - Added `GetMessageManager()` to ReHLDS API interface (`rehlds_api.h`)
  - Added `RehldsMessageManager` global pointer (`mod_rehlds_api.cpp`)
  - Implemented `MessageHook_Handler` for message interception (`meta_api.cpp`)
  - Hooks installed on-demand via `InstallMessageHook()` when plugins call `register_event`
  - Cleaned up `IMessageManager.h` header (removed duplicate includes)

- **FIXED getEventId() FOR EXTENSION MODE** - Message ID lookup now works without Metamod
  - Uses `REG_USER_MSG(name, -1)` which returns existing IDs due to KTPReHLDS dual-list search

- **ADDED AlertMessage HOOKCHAIN** - `register_logevent` now works in extension mode
  - Added `AlertMessage` hookchain to KTPReHLDS (`sys_dll.cpp`, `rehlds_api.h`, `rehlds_api_impl.h/cpp`)
  - Hookchain passes pre-formatted message string and ALERT_TYPE
  - Implemented `AlertMessage_RH` in KTPAMXX to process `at_logged` messages
  - Fires log events and `plugin_log` forward

</details>

<details>
<summary><strong>2025-12-06</strong> - Map Change & Client Commands Fix</summary>

- **FIXED MAP CHANGE** - Clients persist through map changes with proper forward firing
  - Added `SV_InactivateClients` hookchain to KTPReHLDS
  - Fixed `SV_Spawn_f_RH` hook registration (was never registered!)
  - Marked `plugin_end` forward as OK

- **FIXED CLIENT COMMANDS & HUD**
  - Added `SV_ClientCommand` hookchain to KTPReHLDS
  - Fixed vtable alignment (added 20+ missing virtual methods)
  - Chat commands and menus now work in extension mode

</details>

<details>
<summary><strong>2025-12-05</strong> - Message System Fix</summary>

- **FIXED MESSAGE SYSTEM** - Modified KTPReHLDS `RegUserMsg_internal` to search both message lists
- Bulk marked working natives as OK
- Removed debug logging from all files

</details>

<details>
<summary><strong>2025-12-05</strong> - Initial Creation</summary>

- Initial document creation
- Analyzed all custom and core AMXX plugins
- Documented hooks, forwards, and native dependencies

</details>

---

## Related Projects

- **[KTPAMXX](https://github.com/afraznein/KTPAMXX)** - This project (AMX Mod X fork)
- **[KTPReHLDS](https://github.com/afraznein/KTPReHLDS)** - Modified ReHLDS with extension hooks
- **[KTPCvarChecker](https://github.com/afraznein/KTPCvarChecker)** - Real-time cvar enforcement

---

> **Note:** This is a living document. Update status badges as features are tested and verified.
