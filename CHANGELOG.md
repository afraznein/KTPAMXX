# Changelog

All notable changes to KTP AMX will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [2.1.0] - 2025-12-06

### Added

#### New ReHLDS Hooks for Extension Mode
- **SV_ClientCommand hookchain** - Enables `register_clcmd`, menu systems, and `client_command` forward in extension mode
- **SV_InactivateClients hookchain** - Proper map change deactivation with `plugin_end` and `client_disconnect` forwards
- **SV_Spawn_f hookchain** - Client reinitialization after map change for `client_connect` and `client_putinserver` forwards

#### Map Change Support (Extension Mode)
- Clients now persist through map changes without disconnection
- All AMXX forwards (`plugin_init`, `plugin_cfg`, `client_connect`, `client_putinserver`) fire correctly on new maps
- Proper `plugin_end` and `client_disconnect` forwards during map transition

#### Client Command Processing (Extension Mode)
- Chat commands (`/start`, `.start`, etc.) now work in extension mode
- Menu selections (`menuselect 1-9`) properly handled
- `register_clcmd` and `register_menucmd` fully functional

### Fixed

- **SV_Spawn_f hook registration** - Function existed but was never registered, causing map change reconnect issues
- **Vtable alignment** - Fixed mismatch between KTPAMXX and KTPReHLDS headers (added 20+ missing virtual methods to IRehldsHookchains)
- **Debug logging cleanup** - Removed all debug `AMXXLOG_Log` statements from production code

### Changed

- **rehlds_api.h** - Updated to match KTPReHLDS vtable layout with all hookchain methods
- **mod_rehlds_api.cpp** - Updated for new API structure
- Removed deprecated debug logging from CForward.cpp, CMisc.cpp, CTask.cpp, amxmodx.cpp, cvars.cpp, meta_api.cpp

### Technical Details

#### New Hook Registrations (Extension Mode)
- `SV_ClientCommand` - Client command processing for chat commands and menus
- `SV_InactivateClients` - Map change deactivation sequence
- `SV_Spawn_f` - Client spawn command after map change reconnect

#### Map Change Sequence
The extension mode now properly handles the map change sequence:
1. `SV_InactivateClients()` → Fire disconnect forwards, clear player state
2. `SV_ActivateServer()` → Fire `plugin_init`, `plugin_cfg`
3. `SV_Spawn_f()` → Reinitialize reconnecting clients, fire `client_connect`, `client_putinserver`

---

## [2.0.0] - 2024-12-04

### Added

#### ReHLDS Extension Mode (Metamod-Free Operation)
- **Standalone ReHLDS extension support** - KTP AMX can now run as a direct ReHLDS extension without requiring Metamod
- **Dual-mode operation** - Automatically detects and adapts to both Metamod and extension modes
- **ReHLDS API integration** - Full integration with ReHLDS hooks and callbacks
- **Game DLL function wrappers** (`KTPAMX_*` macros) - Unified API that works in both operating modes
- **Module frame callbacks** - `MNF_RegModuleFrameFunc()` / `MNF_UnregModuleFrameFunc()` for modules requiring per-frame processing (replaces Metamod's pfnStartFrame for modules)

#### New Forward: `client_cvar_changed`
- **Real-time cvar monitoring** - Instant notification when clients respond to ANY cvar query
- **Event-driven architecture** - Zero polling overhead, 100% callback-based
- **KTP-ReHLDS integration** - Receives `pfnClientCvarChanged` callbacks from modified engine
- **Simple plugin API** - Single forward handler receives all cvar responses

#### Build System Improvements
- **Windows build support** - `build_windows.bat` for native Windows builds
- **Linux build support** - `build_linux.sh` with WSL compatibility
- **Cross-platform packaging** - `collect_builds.bat` to gather builds from both platforms
- **WSL build integration** - `build_linux_wsl.ps1` and `setup_wsl_build.ps1` for building Linux binaries from Windows
- **Plugin compilation toggle** - `--disable-plugins` configure flag to skip plugin compilation

#### Binary Renaming (KTP Branding)
- Main binary renamed from `amxmodx_mm` to `ktpamx` (Windows: `ktpamx.dll`, Linux: `ktpamx_i386.so`)
- Module binaries renamed from `*_amxx` suffix to `*_ktp` suffix
- Updated all default paths from `addons/amxmodx/` to `addons/ktpamx/`

### Changed

#### Core Architecture
- **Hybrid initialization** - Supports both `Meta_Attach()` (Metamod) and `AMXX_RehldsExtensionInit()` (ReHLDS extension)
- **Game DLL interface abstraction** - `g_pGameEntityInterface` pointer works in both modes
- **Forward execution** - All standard forwards now work in extension mode
- **Server command handling** - `amx` prefix commands work without Metamod

#### Path Defaults Updated
- `amxx_configsdir` default: `addons/ktpamx/configs`
- `amxx_pluginsdir` default: `addons/ktpamx/plugins`
- `amxx_datadir` default: `addons/ktpamx/data`

#### Build System
- `AMBuildScript` - Updated for KTP binary naming and optional plugin compilation
- `PackageScript` - Reorganized packaging for KTP AMX distribution
- `configure.py` - Added `--disable-plugins` option

#### Code Quality
- Added extensive debug logging throughout core systems (controlled by `AMXXLOG_Log`)
- Improved CRLF handling in config file parsing (CVault.cpp)
- Fixed cvar registration to properly handle pre-existing config values
- Added null pointer checks throughout player handling code

### Fixed

- **CVault CRLF handling** - Fixed "Can't use values with ASCII control characters" errors when config files have Windows line endings
- **Cvar registration race condition** - Properly handle cvars set by configs before plugin registration
- **Extension mode game DLL access** - Properly resolve server library base address in non-Metamod mode
- **Module loading in extension mode** - Graceful handling of Metamod-dependent modules

### Technical Details

#### New Global Variables
- `g_bRunningWithMetamod` - Boolean flag indicating Metamod presence
- `g_bRehldsExtensionInit` - Boolean flag indicating ReHLDS extension initialization
- `g_pGameEntityInterface` - Pointer to game DLL functions (works in both modes)

#### New Exported Functions (Extension Mode)
- `AMXX_RehldsExtensionInit()` - Entry point for ReHLDS extension loading
- `AMXX_RehldsExtensionShutdown()` - Cleanup for extension unloading

#### Hook Registrations (Extension Mode)
- `SV_DropClient` - Client disconnect handling
- `SV_ActivateServer` - Server activation (map load)
- `Cvar_DirectSet` - Cvar change monitoring
- `SV_WriteFullClientUpdate` - Client info updates
- `ED_Alloc` / `ED_Free` - Entity allocation tracking
- `SV_StartSound` - Sound emission hook
- `PF_Remove_I` - Entity removal hook
- `ClientConnected` / `SV_ConnectClient` - Client connection handling

### Compatibility Notes

- **Requires KTP-ReHLDS** for `client_cvar_changed` forward functionality
- **Backwards compatible** - All existing AMX Mod X plugins work unchanged
- **Standard ReHLDS compatible** - Extension mode works with standard ReHLDS (without `client_cvar_changed`)
- **Metamod compatible** - Can still run as traditional Metamod plugin

## [1.10.0] - Upstream

Base version forked from AMX Mod X 1.10.0.5468-dev.

See [AMX Mod X releases](https://github.com/alliedmodders/amxmodx/releases) for upstream changelog.

---

## Version History Summary

| Version | Date | Description |
|---------|------|-------------|
| 2.1.0 | 2025-12-06 | Map change support, client commands, menu systems in extension mode |
| 2.0.0 | 2024-12-04 | Major release: ReHLDS extension mode, KTP branding, client_cvar_changed |
| 1.10.0 | - | Base fork from AMX Mod X |

[2.1.0]: https://github.com/afraznein/KTPAMXX/releases/tag/v2.1.0
[2.0.0]: https://github.com/afraznein/KTPAMXX/releases/tag/v2.0.0
[1.10.0]: https://github.com/alliedmodders/amxmodx/releases/tag/1.10.0
