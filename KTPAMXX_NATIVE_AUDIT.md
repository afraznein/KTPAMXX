# KTPAMXX Native Audit - Extension Mode Compatibility

> **Version:** 2.1.0 | **Last Updated:** 2025-12-06 | **Status:** Active Development

[![Extension Mode](https://img.shields.io/badge/Extension%20Mode-Supported-brightgreen)](#)
[![Map Change](https://img.shields.io/badge/Map%20Change-Fixed-brightgreen)](#)
[![Client Commands](https://img.shields.io/badge/Client%20Commands-Working-brightgreen)](#)

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
| `plugin_natives` | ![UNTESTED](https://img.shields.io/badge/-UNTESTED-lightgrey) | Before plugin_init | Native registration |

### Client Forwards

| Forward | Status | Hook Required | Used By |
|:--------|:------:|:--------------|:--------|
| `client_connect` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | ClientConnected / SV_Spawn_f | Connection handling |
| `client_authorized` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | Steam_NotifyClientConnect | Auth checks |
| `client_putinserver` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | SV_Frame / SV_Spawn_f | Player init |
| `client_disconnect` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | SV_DropClient | Disconnect handling |
| `client_disconnected` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | SV_DropClient | Post-disconnect |
| `client_command` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | SV_ClientCommand | Command processing |
| `client_infochanged` | ![UNTESTED](https://img.shields.io/badge/-UNTESTED-lightgrey) | SV_ClientUserInfoChanged | Info updates |

### Custom KTP Forwards

| Forward | Status | Hook Required | Used By |
|:--------|:------:|:--------------|:--------|
| `client_cvar_changed` | ![OK](https://img.shields.io/badge/-OK-brightgreen) | pfnClientCvarChanged | KTPCvarChecker |
| `inconsistent_file` | ![UNTESTED](https://img.shields.io/badge/-UNTESTED-lightgrey) | File consistency | KTPFileChecker |

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
| `register_event` | ![UNTESTED](https://img.shields.io/badge/-UNTESTED-lightgrey) | Event handlers |
| `register_logevent` | ![UNTESTED](https://img.shields.io/badge/-UNTESTED-lightgrey) | Log events |

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
| `set_cvar_*` | ![UNTESTED](https://img.shields.io/badge/-UNTESTED-lightgrey) | Set operations |
| `set_pcvar_*` | ![UNTESTED](https://img.shields.io/badge/-UNTESTED-lightgrey) | Direct set |

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

| Module | Status | Required For |
|:-------|:------:|:-------------|
| DODX | ![UNTESTED](https://img.shields.io/badge/-UNTESTED-lightgrey) | DoD stats, weapons |
| Fun | ![UNTESTED](https://img.shields.io/badge/-UNTESTED-lightgrey) | user_slap, user_kill |
| Engine | ![UNTESTED](https://img.shields.io/badge/-UNTESTED-lightgrey) | engclient_cmd, changelevel |
| Fakemeta | ![UNTESTED](https://img.shields.io/badge/-UNTESTED-lightgrey) | pev, set_pev |
| CStrike | ![UNTESTED](https://img.shields.io/badge/-UNTESTED-lightgrey) | CS-specific natives |
| SQL/SQLite | ![UNTESTED](https://img.shields.io/badge/-UNTESTED-lightgrey) | Database |
| cURL | ![UNTESTED](https://img.shields.io/badge/-UNTESTED-lightgrey) | HTTP requests |
| ReAPI | ![UNTESTED](https://img.shields.io/badge/-UNTESTED-lightgrey) | ReHLDS hooks |

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
| dod/stats_logging.sma | Stats logging | ![UNTESTED](https://img.shields.io/badge/-UNTESTED-lightgrey) |

---

## Changelog

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
