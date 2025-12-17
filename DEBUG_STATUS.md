# DODX Shot Tracking Debug Status

## Current Issue
Weaponstats are not logging shots fired in DODX module running in ReHLDS extension mode.
Player fires weapons but no shot stats appear in logs - only "time" and "latency".

## Root Cause Found (2025-12-16)

### Problems Identified
1. **`g_bServerActive` never set to `true` in extension mode**
   - `ServerActivate_Post` is a Metamod callback that doesn't fire in extension mode
   - `g_bServerActive` was initialized to `false` and never changed
   - `DODX_OnPlayerPreThink` checked `if (!g_bServerActive) return;` causing early exit

2. **`g_pFirstEdict` never initialized in extension mode**
   - `DODX_OnClientConnected` was supposed to set this, but hook was NOT registered
   - Line 1165 had comment: `// NOTE: ClientConnected hook not needed`
   - This caused `ENTINDEX_SAFE` to fail and PreThink to return early

3. **`SERVER_PRINT` doesn't work in ReHLDS hookchain callbacks**
   - Debug messages in `DODX_OnPlayerPreThink` and `CMisc.cpp` used `SERVER_PRINT`
   - These never appeared because `SERVER_PRINT` doesn't flush during hookchains
   - Changed all to `MF_PrintSrvConsole` which works correctly

### Fixes Applied
1. **Extension mode initialization in PreThink** (`moduleconfig.cpp:777-793`)
   - Added code to calculate `g_pFirstEdict` from first valid player edict
   - Sets `g_bServerActive = true` when first player PreThink runs
   - Initializes all player slots once `g_pFirstEdict` is known

2. **Changed all `SERVER_PRINT` to `MF_PrintSrvConsole`**
   - `moduleconfig.cpp`: Lines 752, 788, 817, 838
   - `CMisc.cpp`: Lines 385, 442, 462, 558

## Expected Debug Output (After Fix)
After player connects and enters game:
1. `[DODX DEBUG] DODX_OnPlayerPreThink called for FIRST TIME!`
2. `[DODX DEBUG] Extension mode: g_pFirstEdict set from PreThink, g_bServerActive=true`
3. `[DODX DEBUG] OnPlayerPreThink reached for player X, ingame=0, g_bExtensionMode=1`
4. `[DODX DEBUG] Player X initialized via PreThink (extension mode)`
5. `[DODX DEBUG] PreThink: player=X ingame=1 ignoreBots=0`
6. `[DODX DEBUG] CheckShotFired CALLED: player=X buttons=... IN_ATTACK=...`
7. (When firing) `[DODX DEBUG] CheckShotFired ATTACK DETECTED: ...`
8. (When firing) `[DODX DEBUG] CheckShotFired: SAVING SHOT for player X weapon Y (name)`

## Key Files
- `modules/dod/dodx/moduleconfig.cpp` - Extension hook setup, PreThink handler
- `modules/dod/dodx/CMisc.cpp` - `PreThink()` and `CheckShotFired()` implementations
- `modules/dod/dodx/CMisc.h` - CPlayer class definition

## Shot Tracking Flow (Extension Mode)
1. `DODX_OnPlayerPreThink` hookchain callback is called by ReHLDS
2. On first call: initializes `g_pFirstEdict` and `g_bServerActive`
3. For each player: initializes player struct if `!ingame && g_bExtensionMode`
4. Calls `pPlayer->PreThink()` which calls `CheckShotFired()`
5. `CheckShotFired()` detects IN_ATTACK button press and calls `saveShot(weapon)`

## Build Commands
```bash
# Build
wsl bash -c "cd '/mnt/n/Nein_/KTP Git Projects/KTPAMXX' && sed 's/\r$//' build_linux.sh > /tmp/build_ktpamxx.sh && chmod +x /tmp/build_ktpamxx.sh && bash /tmp/build_ktpamxx.sh"

# The build script auto-deploys to staging folder
```

## Deploy Command
```cmd
copy "N:\Nein_\KTP Git Projects\KTPAMXX\build\amxmodx\packages\dod\addons\ktpamx\modules\dodx_ktp_i386.so" "N:\Nein_\KTP DoD Server\dod\addons\ktpamx\modules\dodx_ktp_i386.so" /Y
```

## Staging Folder
`N:\Nein_\KTP DoD Server\dod\addons\ktpamx\`
