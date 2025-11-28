# KTPAMXX

**Modified AMX Mod X with real-time client cvar detection for competitive Half-Life engine games**

Custom fork of AMX Mod X featuring the `client_cvar_changed` forward, enabling instant detection of client-side console variable changes. Designed for competitive Day of Defeat and Counter-Strike servers requiring strict anti-cheat enforcement.

Part of the [KTP Competitive Infrastructure](https://github.com/afraznein).

---

## 🎯 What is KTPAMXX?

KTPAMXX is a modified version of AMX Mod X that adds native support for **real-time client console variable (cvar) monitoring**. By integrating with [KTP-ReHLDS](https://github.com/afraznein/KTPReHLDS)'s `pfnClientCvarChanged` callback, KTPAMXX provides plugin developers with instant notifications when clients respond to cvar queries, eliminating the need for periodic polling and enabling true real-time anti-cheat enforcement.

### Why KTPAMXX?

Standard AMX Mod X requires plugins to:
1. Call `query_client_cvar()` to request a cvar value
2. Wait for callback with response
3. Manually track all queries and responses

This approach has limitations:
- ❌ Polling overhead (periodic checks waste CPU)
- ❌ Delayed detection (15-60 second intervals)
- ❌ Complex plugin code (tracking queries and callbacks)

**KTPAMXX solves this:**
- ✅ **Instant detection** - Notified immediately when client responds to ANY cvar query
- ✅ **Zero polling** - 100% event-driven (no wasted CPU cycles)
- ✅ **Simple plugin code** - One forward receives all cvar responses

---

## ✨ Key Features

### New Forward: `client_cvar_changed`

```pawn
/**
 * Called when a client responds to ANY cvar query
 * Triggered by KTP-ReHLDS pfnClientCvarChanged callback
 *
 * @param id        Client index (1-32)
 * @param cvar      Name of the cvar
 * @param value     Value returned by client (as string)
 *
 * @note This forward fires for ALL cvar queries, not just your plugin's
 * @note Use this for real-time monitoring without query_client_cvar callbacks
 */
forward client_cvar_changed(id, const cvar[], const value[]);
```

**Example Plugin:**
```pawn
#include <amxmodx>

public plugin_init() {
    register_plugin("Cvar Monitor", "1.0", "You")
}

// Automatically called when client responds to any cvar query
public client_cvar_changed(id, const cvar[], const value[]) {
    // Check if this is a cvar we care about
    if (equal(cvar, "r_fullbright")) {
        new Float:val = floatstr(value)
        if (val != 0.0) {
            // Player is using r_fullbright - enforce correct value
            client_cmd(id, "r_fullbright 0")
            log_amx("Player %d violated r_fullbright: %f", id, val)
        }
    }

    return PLUGIN_CONTINUE
}
```

### Other Features

- ✅ **Backwards compatible** - All existing AMX Mod X plugins work unchanged
- ✅ **Cross-platform** - Windows and Linux build support added
- ✅ **KTP-ReHLDS integration** - Receives callbacks from custom engine hooks
- ✅ **Zero overhead** - Forward only fires when cvars actually change
- ✅ **Performance optimized** - Event-driven architecture (no polling)

---

## 🔧 Technical Details

### How It Works

```
┌─────────────────────────────────────┐
│  Game Client                        │
│  - Server queries cvar              │
│  - Client responds with value       │
└────────────────┬────────────────────┘
                 │ Network packet
                 ↓
┌─────────────────────────────────────┐
│  KTP-ReHLDS (Modified Engine)       │
│  - pfnClientCvarChanged callback    │
│  - Receives cvar name + value       │
└────────────────┬────────────────────┘
                 │ C++ callback
                 ↓
┌─────────────────────────────────────┐
│  KTPAMXX (Modified AMX Mod X)       │
│  - meta_api.cpp receives callback   │
│  - Fires client_cvar_changed()      │
│  - Forward sent to all plugins      │
└────────────────┬────────────────────┘
                 │ AMX Forward
                 ↓
┌─────────────────────────────────────┐
│  Your Plugin                        │
│  - client_cvar_changed() called     │
│  - Validate cvar value              │
│  - Take action if needed            │
└─────────────────────────────────────┘
```

### Code Changes

**Modified File:** `amxmodx/meta_api.cpp`

Added callback hook that receives `pfnClientCvarChanged` from KTP-ReHLDS and forwards it to AMX plugins:

```cpp
// Receives callback from KTP-ReHLDS when client responds to cvar query
void ClientCvarChanged(edict_t *pEdict, const char *cvar, const char *value) {
    int id = ENTINDEX(pEdict);

    // Fire client_cvar_changed forward to all plugins
    cell ret = g_forwards.executeForwards(FM_ClientCvarChanged, id, cvar, value);
}
```

**Modified File:** `amxmodx/amxmodx.inc`

Added forward declaration:

```pawn
/**
 * Called when a client responds to a cvar query
 */
forward client_cvar_changed(id, const cvar[], const value[]);
```

---

## 🚀 Building from Source

### Prerequisites

- **Python 3.8+**
- **AMBuild** - AlliedModders build system
- **Compiler**:
  - Windows: Visual Studio 2019+ or MSVC Build Tools
  - Linux: GCC 7.3+ or Clang 6.0+

### Build Steps

#### Windows

```bash
# Install AMBuild
pip install git+https://github.com/alliedmodders/ambuild

# Configure build
python configure.py --enable-optimize

# Build
ambuild
```

#### Linux

```bash
# Install dependencies
sudo apt-get install gcc-multilib g++-multilib

# Install AMBuild
pip3 install git+https://github.com/alliedmodders/ambuild

# Configure build
python3 configure.py --enable-optimize

# Build
ambuild
```

### Build Output

Compiled binaries are located in:
- **Windows**: `build/package/addons/amxmodx/`
- **Linux**: `build/package/addons/amxmodx/`

Key files:
- `amxmodx_mm.dll` / `amxmodx_mm_i386.so` - Main module
- `modules/*.dll` / `modules/*.so` - Extension modules

---

## 📦 Installation

### Step 1: Install KTP-ReHLDS

KTPAMXX requires [KTP-ReHLDS](https://github.com/afraznein/KTPReHLDS) (modified engine with `pfnClientCvarChanged` callback).

### Step 2: Install KTPAMXX

1. **Backup existing AMX Mod X** installation
2. **Replace files** with KTPAMXX binaries:
   ```
   addons/amxmodx/amxmodx_mm.dll       (Windows)
   addons/amxmodx/amxmodx_mm_i386.so   (Linux)
   ```
3. **Restart server**

### Step 3: Verify Installation

Check server console on startup:
```
AMX Mod X version 1.10.0.xxxx-dev+5468
Compiled: <date>
Build ID: <id>
Core mode: JIT+ASM32
```

Test in a plugin:
```pawn
public plugin_init() {
    // If this compiles, client_cvar_changed is available
    register_forward(FM_ClientCvarChanged, "on_cvar_changed")
}
```

---

## 🔗 Related Projects

### KTP Competitive Infrastructure

KTPAMXX is part of the KTP stack:

**🔧 Engine Layer:**
- **[KTP-ReHLDS](https://github.com/afraznein/KTPReHLDS)** - Modified Half-Life engine with `pfnClientCvarChanged` callback
- **[hlsdk](https://github.com/afraznein/hlsdk)** - Modified Half-Life SDK headers with callback definitions

**🎮 Scripting Layer:**
- **[KTPAMXX](https://github.com/afraznein/KTPAMXX)** - This project (Modified AMX Mod X)

**🛡️ Plugin Layer:**
- **[KTPCvarChecker](https://github.com/afraznein/KTPCvarChecker)** - Real-time anti-cheat cvar enforcement plugin

**🎯 Game Management:**
- **[KTP Match Handler](https://github.com/afraznein/KTPMatchHandler)** - Competitive match system with pause

---

## 🆚 Differences from Standard AMX Mod X

| Feature | Standard AMX Mod X | KTPAMXX |
|---------|-------------------|---------|
| Cvar Detection | `query_client_cvar()` + callback | `client_cvar_changed` forward |
| Detection Method | Manual queries only | Automatic on ALL responses |
| Performance | Polling required | Event-driven (zero overhead) |
| Plugin Complexity | Complex query tracking | Single forward handler |
| Latency | Delayed (polling interval) | Instant (< 1 second) |
| Compatibility | Standard ReHLDS | Requires KTP-ReHLDS |

---

## 📚 Documentation

### For Plugin Developers

**Using `client_cvar_changed`:**

```pawn
public client_cvar_changed(id, const cvar[], const value[]) {
    // IMPORTANT: This fires for ALL cvar responses, not just yours

    // 1. Validate player ID
    if (!is_user_connected(id))
        return PLUGIN_CONTINUE

    // 2. Check if it's a cvar you care about
    if (equal(cvar, "my_cvar")) {
        // 3. Process the value
        new Float:val = floatstr(value)

        // 4. Take action
        if (val != 1.0) {
            client_cmd(id, "my_cvar 1")
        }
    }

    return PLUGIN_CONTINUE
}
```

**Best Practices:**

1. **Filter by cvar name** - Forward fires for ALL cvars, filter to yours
2. **Rate limit** - Prevent spam by limiting checks per player
3. **Prevent recursion** - Use flags when enforcing values via `client_cmd()`
4. **Validate player** - Always check `is_user_connected()` first

**Example Anti-Cheat Plugin:**

See [KTPCvarChecker](https://github.com/afraznein/KTPCvarChecker) for a complete real-world example.

---

## 📝 Version History

### KTPAMXX Modifications

- **2025-11-28** - Added Windows and Linux build support
- **2025-11-27** - Added `client_cvar_changed` forward for real-time cvar validation
- **Base Version** - Forked from AMX Mod X 1.10.0.5468-dev

### Upstream AMX Mod X

For upstream AMX Mod X changelog, see: https://github.com/alliedmodders/amxmodx/releases

---

## 📄 License

**GNU General Public License v3** (same as upstream AMX Mod X)

Based on [AMX Mod X](https://github.com/alliedmodders/amxmodx) by AlliedModders LLC.

See `public/licenses/LICENSE.txt` for full license text.

---

## 🙏 Credits

**KTPAMXX Modifications:**
- **Nein_** ([@afraznein](https://github.com/afraznein)) - `client_cvar_changed` forward, KTP-ReHLDS integration

**Upstream AMX Mod X:**
- **AlliedModders LLC** - Original AMX Mod X development
- **AMX Mod X Team** - Continued maintenance and development

---

## 🔗 Links

- **GitHub**: https://github.com/afraznein/KTPAMXX
- **KTP Infrastructure**: https://github.com/afraznein
- **Upstream AMX Mod X**: https://github.com/alliedmodders/amxmodx
- **AMX Mod X Website**: https://amxmodx.org/
- **AMX Mod X Forums**: https://forums.alliedmods.net/forumdisplay.php?f=3

---

## ⚠️ Important Notes

### Requirements

- ✅ **KTP-ReHLDS required** - Will NOT work with standard ReHLDS or HLDS
- ✅ **No backwards compatibility** - Designed specifically for KTP infrastructure
- ⚠️ **Not compatible** with standard Half-Life engines

### Compatibility

- ✅ **All existing AMX Mod X plugins** work without modification
- ✅ **Standard forwards** (`plugin_init`, `client_putinserver`, etc.) unchanged
- ✅ **Standard natives** (`get_user_name`, `client_cmd`, etc.) unchanged
- ✅ **New forward is optional** - Only use `client_cvar_changed` if you need it

### Support

For issues with:
- **`client_cvar_changed` forward** - Open issue on this repo
- **Standard AMX Mod X features** - See [upstream documentation](https://wiki.alliedmods.net/)
- **KTP-ReHLDS integration** - See [KTP-ReHLDS repo](https://github.com/afraznein/KTPReHLDS)

---

**KTPAMXX** - Real-time client monitoring for competitive Half-Life servers. 🛡️
