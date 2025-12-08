# KTP AMX

**Modified AMX Mod X with ReHLDS extension mode and real-time client cvar detection**

A major fork of AMX Mod X featuring standalone ReHLDS extension support (no Metamod required) and the `client_cvar_changed` forward for instant detection of client-side console variable changes. Designed for competitive Day of Defeat and Counter-Strike servers requiring strict anti-cheat enforcement.

Part of the [KTP Competitive Infrastructure](https://github.com/afraznein).

---

## What's New in v2.2.0

### Extension Mode Event Support

KTP AMX v2.2.0 adds `register_event` and `register_logevent` support in extension mode:

- **`register_event` works** - Via KTPReHLDS IMessageManager integration
- **`register_logevent` works** - Via AlertMessage hookchain
- **Module API** - Modules can now access ReHLDS API directly (`MF_GetRehldsApi`, etc.)
- **Module suffix change** - Default suffix changed from `_amxx` to `_ktp`

### Module Compatibility

| Module | Extension Mode Status |
|--------|----------------------|
| amxxcurl | Working |
| ReAPI | Working |
| DODX | Broken (hooks disabled) |
| SQLite | Broken (Metamod incompatible) |

---

## What's New in v2.1.0

### Full Map Change Support (Extension Mode)

KTP AMX v2.1.0 added complete map change support in extension mode:

- **Seamless map transitions** - Clients persist through map changes without disconnection
- **All forwards fire correctly** - `plugin_init`, `plugin_cfg`, `client_connect`, `client_putinserver` work on new maps
- **Chat commands work** - `/start`, `.start`, and other chat-triggered commands fully functional
- **Menu systems work** - `register_menucmd`, `show_menu`, and menu selections (`menuselect 1-9`)

### New ReHLDS Hooks

| Hook | Purpose |
|------|---------|
| `SV_ClientCommand` | Chat commands, menus, `client_command` forward |
| `SV_InactivateClients` | Map change deactivation, `plugin_end` forward |
| `SV_Spawn_f` | Client reconnect after map change |

---

## What's New in v2.0.0

### ReHLDS Extension Mode (No Metamod Required)

KTP AMX can now run as a **direct ReHLDS extension**, eliminating the Metamod dependency:

| Feature | Traditional (Metamod) | Extension Mode |
|---------|----------------------|----------------|
| Dependencies | ReHLDS + Metamod | ReHLDS only |
| Load method | Metamod plugin | ReHLDS extension |
| Binary | `ktpamx.dll` via Metamod | `ktpamx.dll` direct |
| Performance | Standard | Slightly reduced overhead |
| Compatibility | Full | Most features (no Metamod hooks) |

### Real-Time Cvar Monitoring

The `client_cvar_changed` forward provides instant notification when clients respond to cvar queries:

```pawn
public client_cvar_changed(id, const cvar[], const value[]) {
    if (equal(cvar, "r_fullbright") && floatstr(value) != 0.0) {
        client_cmd(id, "r_fullbright 0")
        log_amx("Player %d violated r_fullbright", id)
    }
    return PLUGIN_CONTINUE
}
```

### KTP Branding

- Main binary: `ktpamx.dll` / `ktpamx_i386.so`
- Module suffix: `*_ktp.dll` / `*_ktp_i386.so`
- Default paths: `addons/ktpamx/`

---

## Features

### Core Capabilities

- **Dual-mode operation** - Runs with Metamod (traditional) or as standalone ReHLDS extension
- **Real-time cvar monitoring** - `client_cvar_changed` forward for instant anti-cheat
- **Full backwards compatibility** - All existing AMX Mod X plugins work unchanged
- **Cross-platform** - Windows and Linux support with unified build system

### For Plugin Developers

- **New forward**: `client_cvar_changed(id, cvar[], value[])` - Event-driven cvar monitoring
- **Standard API unchanged** - All existing natives and forwards work identically
- **Zero code changes required** - Drop-in replacement for AMX Mod X

### For Server Operators

- **Simpler deployment** - Extension mode requires only ReHLDS, no Metamod setup
- **Better anti-cheat** - Real-time cvar validation instead of polling
- **Standard configs** - Same configuration files as AMX Mod X (in `addons/ktpamx/`)

---

## Installation

### Option 1: ReHLDS Extension Mode (Recommended)

1. Install [ReHLDS](https://github.com/dreamstalker/rehlds) (or [KTP-ReHLDS](https://github.com/afraznein/KTPReHLDS) for `client_cvar_changed`)
2. Copy KTP AMX files to your server:
   ```
   addons/ktpamx/
   ├── dlls/
   │   └── ktpamx.dll (or ktpamx_i386.so)
   ├── configs/
   ├── data/
   ├── logs/
   ├── modules/
   ├── plugins/
   └── scripting/
   ```
3. Add to ReHLDS extension config or load via your preferred method
4. Restart server

### Option 2: Metamod Mode (Traditional)

1. Install ReHLDS and Metamod
2. Copy KTP AMX files (same structure as above)
3. Add to `metamod/plugins.ini`:
   ```
   win32 addons/ktpamx/dlls/ktpamx.dll
   linux addons/ktpamx/dlls/ktpamx_i386.so
   ```
4. Restart server

### Verify Installation

Check server console on startup:
```
KTP AMX v2.2.0 loaded
Core mode: JIT+ASM32
Running as: ReHLDS Extension (or: Metamod Plugin)
```

---

## Building from Source

### Prerequisites

- **Python 3.8+**
- **AMBuild**: `pip install git+https://github.com/alliedmodders/ambuild`
- **Windows**: Visual Studio 2019+ or MSVC Build Tools
- **Linux**: GCC 7.3+ with multilib support (`gcc-multilib g++-multilib`)

### Build Commands

#### Windows
```bash
python configure.py --enable-optimize
ambuild
```

#### Linux
```bash
python3 configure.py --enable-optimize
ambuild
```

#### Skip Plugin Compilation
```bash
python configure.py --enable-optimize --disable-plugins
ambuild
```

### Build Output

Binaries are packaged in `build/package/addons/ktpamx/`:
- `dlls/ktpamx.dll` / `dlls/ktpamx_i386.so` - Main binary
- `modules/*.dll` / `modules/*.so` - Extension modules

### Cross-Platform Building (Windows + WSL)

```powershell
# Setup WSL build environment
.\setup_wsl_build.ps1

# Build for both platforms
.\build_windows.bat
.\build_linux_wsl.ps1

# Collect builds
.\collect_builds.bat
```

---

## Configuration

KTP AMX uses the same configuration structure as AMX Mod X, with paths updated to `addons/ktpamx/`:

```
addons/ktpamx/
├── configs/
│   ├── amxx.cfg          # Main configuration
│   ├── plugins.ini       # Plugin load list
│   ├── modules.ini       # Module load list
│   ├── users.ini         # Admin users
│   └── cmdaccess.ini     # Command access levels
├── data/
│   ├── lang/             # Language files
│   └── gamedata/         # Game signature data
├── logs/                 # Log output
├── modules/              # Binary modules
├── plugins/              # Compiled plugins (.amxx)
└── scripting/            # Plugin source (.sma)
```

---

## API Reference

### New Forward: client_cvar_changed

```pawn
/**
 * Called when a client responds to ANY cvar query.
 * Requires KTP-ReHLDS for full functionality.
 *
 * @param id        Client index (1-32)
 * @param cvar      Name of the queried cvar
 * @param value     Value returned by client (string)
 *
 * @note Fires for ALL cvar queries from any source
 * @note Use for real-time monitoring without polling
 */
forward client_cvar_changed(id, const cvar[], const value[]);
```

### Example: Anti-Cheat Plugin

```pawn
#include <amxmodx>

new g_enforcing[MAX_PLAYERS + 1]

public plugin_init() {
    register_plugin("Cvar Enforcer", "2.0", "KTP")
}

public client_cvar_changed(id, const cvar[], const value[]) {
    if (!is_user_connected(id) || g_enforcing[id])
        return PLUGIN_CONTINUE

    // Enforce r_fullbright = 0
    if (equal(cvar, "r_fullbright")) {
        if (floatstr(value) != 0.0) {
            g_enforcing[id] = true
            client_cmd(id, "r_fullbright 0")
            set_task(0.5, "clear_enforce", id)
            log_amx("Enforced r_fullbright on player %d", id)
        }
    }

    return PLUGIN_CONTINUE
}

public clear_enforce(id) {
    g_enforcing[id] = false
}

public client_disconnected(id) {
    g_enforcing[id] = false
}
```

---

## Architecture

### How Extension Mode Works

```
┌─────────────────────────────────────────┐
│  ReHLDS Engine                          │
│  - Loads ktpamx as extension            │
│  - Provides hook callbacks              │
└────────────────┬────────────────────────┘
                 │ ReHLDS API
                 ↓
┌─────────────────────────────────────────┐
│  KTP AMX                                │
│  - AMXX_RehldsExtensionInit()           │
│  - Registers ReHLDS hooks               │
│  - Loads plugins and modules            │
└────────────────┬────────────────────────┘
                 │ AMX Forwards
                 ↓
┌─────────────────────────────────────────┐
│  Plugins                                │
│  - Standard forwards work normally      │
│  - client_cvar_changed available        │
└─────────────────────────────────────────┘
```

### ReHLDS Hooks (Extension Mode)

KTP AMX registers these ReHLDS hooks when running in extension mode:

#### Connection & Lifecycle
- `ClientConnected` / `SV_ConnectClient` - Client connection handling
- `SV_DropClient` - Client disconnect handling
- `Steam_NotifyClientConnect` - Client authorization

#### Server Events
- `SV_ActivateServer` - Map load / server activation
- `SV_InactivateClients` - Map change deactivation (fires `plugin_end`, `client_disconnect`)
- `SV_Frame` - Per-frame processing

#### Client Processing
- `SV_ClientCommand` - Client command processing (`register_clcmd`, menus, `client_command`)
- `SV_Spawn_f` - Client spawn command (handles reconnect after map change)
- `SV_WriteFullClientUpdate` - Client info updates

#### Engine Functions
- `Cvar_DirectSet` - Cvar change monitoring
- `ED_Alloc` / `ED_Free` - Entity allocation
- `SV_StartSound` - Sound emission
- `PF_RegUserMsg_I` - Message ID capture for HUD drawing

---

## Compatibility

### Requirements

| Component | Extension Mode | Metamod Mode |
|-----------|---------------|--------------|
| ReHLDS | Required | Required |
| KTP-ReHLDS | For client_cvar_changed | For client_cvar_changed |
| Metamod | Not needed | Required |

### Plugin Compatibility

- **100% backwards compatible** with AMX Mod X plugins
- **Standard forwards** work identically in both modes
- **Modules** work in both modes (some Metamod-specific features unavailable in extension mode)

### Known Limitations (Extension Mode)

- Metamod plugin features unavailable (no LOAD_PLUGIN, UNLOAD_PLUGIN)
- Some modules that rely on Metamod hooks may have reduced functionality
- Fakemeta module's Metamod-specific functions are no-ops

---

## Related Projects

### KTP Competitive Infrastructure

- **[KTP-ReHLDS](https://github.com/afraznein/KTPReHLDS)** - Modified ReHLDS with `pfnClientCvarChanged`
- **[KTP AMX](https://github.com/afraznein/KTPAMXX)** - This project
- **[KTPCvarChecker](https://github.com/afraznein/KTPCvarChecker)** - Real-time cvar enforcement plugin

### Upstream

- **[AMX Mod X](https://github.com/alliedmodders/amxmodx)** - Original project
- **[ReHLDS](https://github.com/dreamstalker/rehlds)** - Reverse-engineered HLDS

---

## License

**GNU General Public License v3.0**

Based on [AMX Mod X](https://github.com/alliedmodders/amxmodx) by AlliedModders LLC.

See [LICENSE](LICENSE) for full text.

---

## Credits

**KTP AMX Development:**
- **Nein_** ([@afraznein](https://github.com/afraznein)) - ReHLDS extension mode, `client_cvar_changed`, KTP integration

**Upstream AMX Mod X:**
- **AlliedModders LLC** and **AMX Mod X Team**

---

## Links

- **Repository**: https://github.com/afraznein/KTPAMXX
- **Changelog**: [CHANGELOG.md](CHANGELOG.md)
- **Issues**: https://github.com/afraznein/KTPAMXX/issues
- **Upstream AMX Mod X**: https://github.com/alliedmodders/amxmodx
