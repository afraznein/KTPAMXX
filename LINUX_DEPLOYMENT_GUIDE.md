# Complete KTP Linux Deployment Guide

This guide covers deploying both KTPReHLDS and KTPAMXX to your Ubuntu server for real-time client cvar detection.

## Overview

You have made custom modifications to two components:

1. **KTPReHLDS** - Modified Half-Life Dedicated Server
   - Location: `N:\Nein_\KTP Git Projects\KTPReHLDS`
   - Modification: Added `pfnClientCvarChanged` callback in `rehlds/rehlds/public/rehlds/eiface.h`
   - Output: `swds_linux` (the engine binary)

2. **KTPAMXX** - Custom AMX Mod X
   - Location: `N:\Nein_\KTP Git Projects\KTPAMXX`
   - Modification: Added `client_cvar_changed` forward in `amxmodx/meta_api.cpp`
   - Output: `amxmodx_mm_i386.so` (the Metamod plugin)

## Prerequisites on Ubuntu Server

```bash
# Update system
sudo apt-get update && sudo apt-get upgrade -y

# Install build tools
sudo apt-get install -y \
    build-essential \
    g++-multilib \
    gcc-multilib \
    git \
    python3 \
    python3-pip \
    lib32stdc++6 \
    lib32z1-dev \
    libc6-dev-i386 \
    cmake

# Enable 32-bit architecture
sudo dpkg --add-architecture i386
sudo apt-get update
sudo apt-get install -y libc6:i386 libstdc++6:i386
```

## Step 1: Build KTPReHLDS for Linux

### Transfer Files to Ubuntu

```bash
# On Ubuntu server, create directory structure
mkdir -p ~/ktp
cd ~/ktp

# Option 1: If you have git repos set up
git clone [your-ktprehlds-repo] KTPReHLDS

# Option 2: Transfer from Windows via SCP
# Run from Windows Git Bash:
# scp -r "N:\Nein_\KTP Git Projects\KTPReHLDS" user@server:~/ktp/
```

### Build ReHLDS

```bash
cd ~/ktp/KTPReHLDS/rehlds

# Initialize submodules
git submodule update --init --recursive

# Create build directory
mkdir -p build
cd build

# Configure with CMake
cmake .. -DCMAKE_BUILD_TYPE=Release

# Build (this will take several minutes)
make -j$(nproc)

# Output will be in: rehlds/build/rehlds/engine_i486.so
# This needs to be renamed to swds_linux
cp rehlds/engine_i486.so ../../swds_linux
```

### Expected Output
- `~/ktp/KTPReHLDS/rehlds/build/rehlds/swds_linux` (or `engine_i486.so`)

## Step 2: Build KTPAMXX for Linux

### Transfer Files to Ubuntu

```bash
cd ~/ktp

# Clone KTPAMXX
git clone [your-ktpamxx-repo] KTPAMXX

# Or transfer via SCP:
# scp -r "N:\Nein_\KTP Git Projects\KTPAMXX" user@server:~/ktp/
# scp -r "N:\Nein_\KTP Git Projects\hlsdk" user@server:~/ktp/
# scp -r "N:\Nein_\KTP Git Projects\metamod-am" user@server:~/ktp/
```

### CRITICAL: Apply HLSDK Modification

Edit `~/ktp/hlsdk/engine/eiface.h` and add the `pfnClientCvarChanged` callback to the `NEW_DLL_FUNCTIONS` structure (see BUILD_INSTRUCTIONS_UBUNTU.md for exact code).

### Run Build Script

```bash
cd ~/ktp/KTPAMXX

# Make script executable
chmod +x build_linux.sh

# Run build
./build_linux.sh
```

### Expected Output
- `~/ktp/KTPAMXX/obj-linux/amxmodx/amxmodx_mm_i386.so`

## Step 3: Deploy to Half-Life Server

Assuming your HLDS installation is at `/home/hlds/hlds_linux`:

```bash
# Set your HLDS path
HLDS_PATH="/home/hlds/hlds_linux"

# Deploy KTPReHLDS engine
cp ~/ktp/KTPReHLDS/rehlds/build/rehlds/swds_linux $HLDS_PATH/swds_linux
chmod +x $HLDS_PATH/swds_linux

# Deploy KTPAMXX
cp ~/ktp/KTPAMXX/obj-linux/amxmodx/amxmodx_mm_i386.so \
   $HLDS_PATH/cstrike/addons/amxmodx/dlls/amxmodx_mm_i386.so
chmod +x $HLDS_PATH/cstrike/addons/amxmodx/dlls/amxmodx_mm_i386.so

# Also update the include file for plugin compilation
cp ~/ktp/KTPAMXX/plugins/include/amxmodx.inc \
   $HLDS_PATH/cstrike/addons/amxmodx/scripting/include/amxmodx.inc
```

## Step 4: Verify Installation

```bash
# Check file types (should be 32-bit ELF)
file $HLDS_PATH/swds_linux
# Expected: ELF 32-bit LSB executable, Intel 80386

file $HLDS_PATH/cstrike/addons/amxmodx/dlls/amxmodx_mm_i386.so
# Expected: ELF 32-bit LSB shared object, Intel 80386

# Check library dependencies
ldd $HLDS_PATH/swds_linux
ldd $HLDS_PATH/cstrike/addons/amxmodx/dlls/amxmodx_mm_i386.so
```

## Step 5: Test the Server

```bash
# Start server in screen/tmux for testing
cd $HLDS_PATH
./hlds_run -game cstrike +map de_dust2 +maxplayers 32

# Connect to server and check AMX version
# In server console or via RCON:
amxx version

# Should show your custom build
```

## Step 6: Update KTPCvarChecker Plugin

Now that both components are deployed, you can modify your KTPCvarChecker plugin to use the real-time forward:

```pawn
// In KTPCvarChecker.sma, add:
public client_cvar_changed(id, const cvar[], const value[])
{
    // Real-time cvar change detection!
    // This is called immediately when client responds to cvar query

    // Check if this cvar is in our validation list
    // Apply validation logic
    // Take action if needed (kick, ban, notify admins, etc.)

    server_print("[KTP Cvar] Player %d changed %s to %s", id, cvar, value)
}
```

## File Checklist

Files you need to transfer to Ubuntu:

- [ ] `N:\Nein_\KTP Git Projects\KTPReHLDS` → `~/ktp/KTPReHLDS`
- [ ] `N:\Nein_\KTP Git Projects\KTPAMXX` → `~/ktp/KTPAMXX`
- [ ] `N:\Nein_\KTP Git Projects\hlsdk` → `~/ktp/hlsdk` (with pfnClientCvarChanged modification)
- [ ] `N:\Nein_\KTP Git Projects\metamod-am` → `~/ktp/metamod-am`

## Build Output Checklist

After building, verify these files exist:

- [ ] `~/ktp/KTPReHLDS/rehlds/build/rehlds/swds_linux` (or engine_i486.so)
- [ ] `~/ktp/KTPAMXX/obj-linux/amxmodx/amxmodx_mm_i386.so`

## Deployment Checklist

After deploying, verify these files in HLDS:

- [ ] `$HLDS_PATH/swds_linux` - KTPReHLDS engine
- [ ] `$HLDS_PATH/cstrike/addons/amxmodx/dlls/amxmodx_mm_i386.so` - KTPAMXX
- [ ] `$HLDS_PATH/cstrike/addons/amxmodx/scripting/include/amxmodx.inc` - Updated with client_cvar_changed forward

## Troubleshooting

### Build Errors

**"cannot find -lstdc++"**
```bash
sudo apt-get install lib32stdc++6 g++-multilib
```

**"No such file or directory" for includes**
```bash
# Install missing development packages
sudo apt-get install libc6-dev-i386
```

### Runtime Errors

**"error while loading shared libraries"**
```bash
# Install 32-bit runtime libraries
sudo apt-get install lib32z1 lib32stdc++6
```

**Server crashes on startup**
```bash
# Check compatibility - ensure both ReHLDS and AMX are 32-bit
file $HLDS_PATH/swds_linux
file $HLDS_PATH/cstrike/addons/amxmodx/dlls/amxmodx_mm_i386.so

# Check server logs
tail -f $HLDS_PATH/cstrike/addons/metamod/log/*.log
tail -f $HLDS_PATH/cstrike/addons/amxmodx/logs/*.log
```

## Performance Notes

The real-time `client_cvar_changed` forward is **much more efficient** than periodic polling because:

1. **No polling overhead** - Server doesn't constantly query cvars
2. **Immediate detection** - Changes detected as soon as client responds
3. **Lower network usage** - Only queries cvars when needed
4. **Better player experience** - No periodic lag spikes from mass cvar queries

## Security Notes

- Keep your KTPReHLDS and KTPAMXX repositories **private** if they contain security-sensitive logic
- The `pfnClientCvarChanged` callback is a powerful feature - ensure your validation logic is secure
- Consider rate limiting cvar queries to prevent client-side exploits

## Support

For build issues:
- Check AMBuild documentation: https://wiki.alliedmods.net/AMBuild
- ReHLDS build guide: https://github.com/dreamstalker/rehlds

For runtime issues:
- Check Metamod logs: `addons/metamod/logs/`
- Check AMX Mod X logs: `addons/amxmodx/logs/`
- Enable debug mode in `amxx.cfg`: `amx_debug 1`
