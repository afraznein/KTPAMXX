# KTP AMX

**Version 2.7.31** | Modified AMX Mod X with ReHLDS extension mode, real-time client cvar detection, and async game-thread-safe logging

A major fork of [AMX Mod X](https://github.com/alliedmodders/amxmodx) featuring standalone ReHLDS extension support (no Metamod required) and the `client_cvar_changed` forward for instant detection of client-side console variable changes. Designed for competitive Day of Defeat servers requiring strict anti-cheat enforcement.

Part of the [KTP Competitive Infrastructure](https://github.com/afraznein).

---

## Architecture Position

KTPAMXX is the **scripting platform** (Layer 2) of the KTP competitive stack:

```
Layer 6: Application Plugins (KTPMatchHandler, KTPAdminAudit, etc.)
Layer 5: Game Stats Module (DODX with HLStatsX integration)
Layer 4: HTTP Module (KTP AMXX Curl)
Layer 3: Engine Bridge (KTP-ReAPI)
Layer 2: Scripting Platform (KTPAMXX) <-- YOU ARE HERE
Layer 1: Game Engine (KTP-ReHLDS)
```

---

## Key Features

### Extension Mode (No Metamod Required)

Runs as a direct ReHLDS extension, eliminating the Metamod dependency. Wall penetration breaks when using ReHLDS + Metamod together — extension mode bypasses this entirely.

| Feature | Traditional (Metamod) | Extension Mode |
|---------|----------------------|----------------|
| Dependencies | ReHLDS + Metamod | ReHLDS only |
| Load method | Metamod plugin | ReHLDS extension |
| Compatibility | Full | Most features (no Metamod hooks) |

### Real-Time Cvar Monitoring

The `client_cvar_changed` forward provides instant notification when clients respond to cvar queries — event-driven instead of polling.

```pawn
forward client_cvar_changed(id, const cvar[], const value[]);
```

### Module Frame Callback API

`MF_RegModuleFrameFunc()` allows modules to register per-frame callbacks without Metamod's `pfnStartFrame`. Used by KTPAmxxCurl for async HTTP processing.

### Module SDK Extensions

Extension mode exposes additional APIs for third-party modules:

| API | Purpose |
|-----|---------|
| `MF_GetEngineFuncs()` | Engine function table access |
| `MF_GetGlobalVars()` | Global variables access |
| `MF_IsExtensionMode()` | Check if running without Metamod |
| `MF_GetRehldsApi()` | ReHLDS API interface pointer — *reserved; no consumer yet* |

Also exported and reserved: `MF_GetRehldsFuncs()`, `MF_GetRehldsServerData()`,
`MF_Reg`/`UnregModuleFrameFunc()`. In-tree modules use the narrower
`MF_GetRehldsHookchains()` / `MF_GetRehldsMessageManager()` instead.

### DODX Module (KTP Additions)

The DODX stats module includes extensive KTP-specific natives:

**Score Management:** `dodx_set_team_score`, `dodx_get_team_score`, `dodx_broadcast_team_score`, `dodx_set_scoreboard_team_name`, `dodx_has_gamerules`, `dodx_get_round_time`

**Match Clocks:** `dodx_get_score_tick_time`, `dodx_get_score_tick_period` — the territorial scoring clock read off `dod_control_point_master`. `dodx_get_score_tick_period` is the **nominal** delay, not a phase.

**Aim Geometry (measure-only):** `dodx_get_shot_geom`, `dodx_get_aim_vis_stats`, `dodx_reset_aim_vis_stats`

**Score Persistence:** `dodx_set_user_deaths`, `dodx_get_user_deaths`, `dodx_set_user_score`, `dodx_get_user_score`, `dodx_get_observed_deaths`, `dodx_broadcast_scoreboard`

**HLStatsX Integration:** `dodx_flush_all_stats`, `dodx_reset_all_stats`, `dodx_set_match_id`, `dodx_get_match_id`, `dodx_set_stats_paused`, `dodx_set_pl_teamname`

**Grenade Manipulation:** `dodx_set_grenade_ammo`, `dodx_get_grenade_ammo`, `dodx_get_grenade_ammo_index`, `dodx_send_ammox`, `dodx_give_grenade`, `dodx_strip_grenade`

**Player Manipulation:** `dodx_set_user_noclip`, `dodx_set_user_class`, `dodx_set_user_team`, `dodx_get/set_user_origin`, `dodx_get/set_user_angles`

**Pre-Damage Forward:** `dod_damage_pre(attacker, victim, damage, wpnindex, hitplace, TA)` — return modified damage or 0 to block.

**Per-Shot Forward:** `dod_client_weapon_fire(id, weapon, Float:gametime)` — fires per weapon actuation, including pure misses. Read the coverage envelope in `dodx.inc` before counting on it: never fires for bots, suppressed while stats are paused, no dry-fire events, and grenades/rockets arrive by a separate path.

See `plugins/include/dodx.inc` for full API documentation — the KTP natives carry their fail-soft return contracts and caller obligations there, not just signatures.

### Shared Includes

| Include | Purpose |
|---------|---------|
| `ktp_discord.inc` | Discord webhook integration (config loading, channel routing, embeds) |
| `dodx.inc` | DODX natives including all KTP additions |
| `reapi_engine_const.inc` | KTP-ReHLDS hook constants |

---

## Installation

### Extension Mode (Recommended)

1. Install [KTP-ReHLDS](https://github.com/afraznein/KTPReHLDS)
2. Copy KTP AMX files to your server:
   ```
   addons/ktpamx/
   ├── dlls/ktpamx_i386.so
   ├── configs/
   ├── data/
   ├── logs/
   ├── modules/
   ├── plugins/
   └── scripting/
   ```
3. Add to `<gamedir>/addons/extensions.ini` (for DoD: `dod/addons/extensions.ini`):
   ```
   addons/ktpamx/dlls/ktpamx_i386.so
   ```
   Both the file location and the path inside it are relative to the game
   directory. Get either wrong and the engine loads nothing, prints no error,
   and runs as vanilla HLDS — check the `ktp_extension_loaded` cvar to confirm.
4. Restart server

### Metamod Mode (Traditional)

1. Install ReHLDS and Metamod
2. Copy KTP AMX files (same structure as above)
3. Add to `metamod/plugins.ini`:
   ```
   linux addons/ktpamx/dlls/ktpamx_i386.so
   ```
4. Restart server

### Verify Installation

Check server console on startup:
```
KTP AMX version <version> (ReHLDS Extension Mode)
```

---

## Building from Source

### Prerequisites

- Python 3.8+ with [AMBuild](https://github.com/alliedmodders/ambuild)
- GCC 7.3+ with multilib support (`gcc-multilib g++-multilib`)
- KTPhlsdk (HLSDK headers) in sibling directory

### Linux (WSL)

```bash
wsl bash -c "cd '/mnt/n/Nein_/KTP Git Projects/KTPAMXX' && bash build_linux.sh"
```

The build script handles AMBuild setup, configuration, and auto-stages output to `KTP DoD Server/serverfiles/`.

### Manual Build

```bash
python3 configure.py --enable-optimize --no-mysql --no-plugins
cd obj-linux && python3 $(which ambuild)
```

### Build Output

| File | Description |
|------|-------------|
| `ktpamx_i386.so` | Main AMXX binary |
| `dodx_ktp_i386.so` | DoD stats module |
| `fun_ktp_i386.so` | Fun module |
| `engine_ktp_i386.so` | Engine module |
| `fakemeta_ktp_i386.so` | FakeMeta module |

`build_linux.sh` stages only `ktpamx_i386.so` and `dodx_ktp_i386.so` into the server tree — the rest build but KTP does not deploy them.

---

## ReHLDS Hooks (Extension Mode)

KTP AMX registers these hooks when running as a ReHLDS extension:

**Connection & Lifecycle:** `ClientConnected`, `SV_ConnectClient`, `SV_DropClient`, `Steam_NotifyClientConnect`

**Server Events:** `SV_ActivateServer`, `SV_InactivateClients`, `SV_Frame`

**Client Processing:** `SV_ClientCommand`, `SV_Spawn_f`, `SV_WriteFullClientUpdate`, `SV_ClientUserInfoChanged`

**Engine Functions:** `Cvar_DirectSet`, `ED_Alloc`, `ED_Free`, `SV_StartSound`, `PF_RegUserMsg_I`, `PF_changelevel_I`, `PF_precache_model_I`, `PF_setmodel_I`, `PF_TraceLine`, `PF_SetClientKeyValue`, `AlertMessage`, `SV_PlayerRunPreThink`

---

## Compatibility

| Component | Extension Mode | Metamod Mode |
|-----------|---------------|--------------|
| ReHLDS | Required | Required |
| KTP-ReHLDS | For `client_cvar_changed` | For `client_cvar_changed` |
| Metamod | Not needed | Required |

100% backwards compatible with existing AMX Mod X plugins. Standard forwards work identically in both modes.

**Extension Mode Limitations:**
- Metamod plugin management unavailable (`LOAD_PLUGIN`, `UNLOAD_PLUGIN`)
- Fakemeta module's Metamod-specific functions are no-ops

---

## Logging (async writer, 2.7.19+)

AMXX log lines (`log_amx`, error logs) are written by a dedicated writer thread — the game thread only formats and enqueues, so a slow or stalling disk can no longer freeze server frames. Behavior and file layout are unchanged (`addons/ktpamx/logs/`, controlled by the standard `amxx_logging` localinfo: `1` daily file, `2` per-map file, `3` HL logs).

- `localinfo amxx_log_async 0` — restore the legacy synchronous write path (latched per map change).
- If the writer ever drops lines (queue full, disk failure), the count is reported to the server console at the next map change: `[AMXX] async log writer dropped N line(s)…`.
- Crash durability matches the old behavior: the log file is line-buffered, so at most the in-flight line is lost.

---

## Version Information

- **Current Version**: 2.7.31 (2026-08) — DODX-only cut; the fleet core binary is still 2.7.27
- **Based on**: AMX Mod X 1.10.0 (upstream)
- **Platform**: GCC 7.3+ / Visual Studio 2019+
- **Compatible with**: KTP-ReHLDS 3.22.0.904+, KTP-ReAPI 5.29.0.362-ktp+. Extension-mode teardown (`KTP_ExtensionShutdown`) needs ReHLDS .928+, and the 2.7.24 `client_infochanged` ordering fix only becomes reachable on .929+ — below those those fixes are inert. 2.7.25 adds no new engine-version floor, and neither do 2.7.26 through 2.7.31.

**2.7.25 behavior notes for plugin authors** — three extension-mode parity gaps closed, all of which
change what working code sees:

- `get_vaultdata` now survives a map change. Previously the vault was cleared and never reloaded, so
  a value written on map 1 read back as `""` on map 2 while `vault.ini` still held it.
- `dod_get_map_info` (DODX) now returns real values instead of always 0, and `get_weaponid` performs
  the British/para weapon remaps. Affects any plugin branching on allies-country or paras, and the
  weapon id reported by `dod_grenade_explosion` on those maps.
- `cmdaccess.ini` is actually parsed (2.7.25, AX-09/22) — rules in it were silently discarded before.

See [CHANGELOG.md](CHANGELOG.md) for full version history.

---

## Related Projects

**KTP Stack:**
- [KTP-ReHLDS](https://github.com/afraznein/KTPReHLDS) - Game engine with `pfnClientCvarChanged`
- [KTP-ReAPI](https://github.com/afraznein/KTPReAPI) - Engine bridge module
- [KTPAmxxCurl](https://github.com/afraznein/KTPAmxxCurl) - Async HTTP module
- [KTPMatchHandler](https://github.com/afraznein/KTPMatchHandler) - Match management

**Upstream:**
- [AMX Mod X](https://github.com/alliedmodders/amxmodx) - Original project
- [ReHLDS](https://github.com/dreamstalker/rehlds) - Reverse-engineered HLDS

---

## License

GNU General Public License v3.0 - Same as upstream AMX Mod X. See [LICENSE](LICENSE) for full text.
