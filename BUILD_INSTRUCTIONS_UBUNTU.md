# Building KTPAMXX on Ubuntu Server

This guide will help you build the custom KTPAMXX binaries on your Ubuntu server.

## Prerequisites

### 1. Install Required Packages

```bash
sudo apt-get update
sudo apt-get install -y \
    build-essential \
    g++-multilib \
    gcc-multilib \
    git \
    python3 \
    python3-pip \
    lib32stdc++6 \
    lib32z1-dev \
    libc6-dev-i386
```

### 2. Enable 32-bit Architecture Support

AMX Mod X requires 32-bit builds:

```bash
sudo dpkg --add-architecture i386
sudo apt-get update
sudo apt-get install -y libc6:i386 libstdc++6:i386
```

## Upload Files to Ubuntu Server

You need to transfer the following directories from your Windows machine to the Ubuntu server:

1. **KTPAMXX** - The custom AMX Mod X code
2. **hlsdk** - The modified Half-Life SDK (with pfnClientCvarChanged)
3. **metamod-am** - Metamod source

### Recommended Structure on Ubuntu:
```
/home/youruser/ktp/
├── KTPAMXX/
├── hlsdk/
└── metamod-am/
```

### Transfer Methods:

**Option 1: Using Git (Recommended)**
```bash
# On Ubuntu server
cd /home/youruser
mkdir -p ktp
cd ktp

# Clone KTPAMXX
git clone [your-ktpamxx-repo-url] KTPAMXX

# Clone dependencies (these should already exist from checkout-deps.sh)
git clone https://github.com/alliedmodders/metamod-hl1 metamod-am
git clone https://github.com/alliedmodders/hlsdk hlsdk
```

**Option 2: Using SCP/SFTP**
```bash
# From Windows (Git Bash or PowerShell with scp)
scp -r "N:\Nein_\KTP Git Projects\KTPAMXX" user@server:/home/user/ktp/
scp -r "N:\Nein_\KTP Git Projects\hlsdk" user@server:/home/user/ktp/
scp -r "N:\Nein_\KTP Git Projects\metamod-am" user@server:/home/user/ktp/
```

## Important: Apply the HLSDK Modification

The custom `pfnClientCvarChanged` callback must be in the HLSDK headers on Ubuntu as well.

**On Ubuntu server**, edit `/home/youruser/ktp/hlsdk/engine/eiface.h`:

Find the `NEW_DLL_FUNCTIONS` structure (around line 515) and ensure it includes:

```c
typedef struct
{
	// Called right before the object's memory is freed.
	// Calls its destructor.
	void			(*pfnOnFreeEntPrivateData)(edict_t *pEnt);
	void			(*pfnGameShutdown)(void);
	int				(*pfnShouldCollide)( edict_t *pentTouched, edict_t *pentOther );
	void			(*pfnCvarValue)( const edict_t *pEnt, const char *value );
    void            (*pfnCvarValue2)( const edict_t *pEnt, int requestID, const char *cvarName, const char *value );

	// KTP Custom: Real-time client cvar change notification
	// Called whenever a client cvar query response is received from the client
	// This provides real-time notification for ALL client cvars (not just FCVAR_USERINFO)
	// Use this to implement real-time cvar validation/enforcement without periodic polling
	// Parameters: pEnt - player entity, cvarName - name of cvar, value - current value
	void			(*pfnClientCvarChanged)( const edict_t *pEnt, const char *cvarName, const char *value );
} NEW_DLL_FUNCTIONS;
```

## Build Process

### 1. Run the dependency checkout script

```bash
cd /home/youruser/ktp/KTPAMXX/support
bash checkout-deps.sh --no-mysql
```

This will:
- Download and set up AMBuild
- Clone metamod-am and hlsdk if not present
- Set up the build environment

### 2. Make the build script executable

```bash
cd /home/youruser/ktp/KTPAMXX
chmod +x build_linux.sh
```

### 3. Run the build

```bash
./build_linux.sh
```

The build will:
- Check for required tools
- Install AMBuild if needed
- Configure the build for Linux (32-bit)
- Compile all sources
- Output: `obj-linux/amxmodx/amxmodx_mm_i386.so`

## Manual Build (Alternative)

If you prefer to build manually:

```bash
cd /home/youruser/ktp/KTPAMXX

# Set environment variables
export METAMOD="/home/youruser/ktp/metamod-am"
export HLSDK="/home/youruser/ktp/hlsdk"

# Configure
python3 configure.py --enable-optimize --no-mysql

# Build
cd obj-linux
python3 $(which ambuild)
cd ..
```

## Troubleshooting

### "g++: error: unrecognized command line option '-m32'"

Install 32-bit build tools:
```bash
sudo apt-get install g++-multilib gcc-multilib
```

### "cannot find -lstdc++"

Install 32-bit standard library:
```bash
sudo apt-get install lib32stdc++6
```

### "AMBuild not found"

Install manually:
```bash
cd /home/youruser/ktp/KTPAMXX/support
git clone https://github.com/alliedmodders/ambuild
cd ambuild
python3 -m pip install --user .
```

### Build fails with missing headers

Ensure you've installed all development packages:
```bash
sudo apt-get install build-essential g++-multilib gcc-multilib libc6-dev-i386
```

## Deployment

Once built successfully:

```bash
# Copy to your Half-Life server
cp obj-linux/amxmodx/amxmodx_mm_i386.so /path/to/hlds/cstrike/addons/amxmodx/dlls/

# Ensure it's executable
chmod +x /path/to/hlds/cstrike/addons/amxmodx/dlls/amxmodx_mm_i386.so

# Restart your server
```

## Verification

To verify the build includes your changes:

```bash
# Check exported symbols (should include GiveFnptrsToDll)
nm -D obj-linux/amxmodx/amxmodx_mm_i386.so | grep GiveFnptrsToDll

# Check file type (should be 32-bit)
file obj-linux/amxmodx/amxmodx_mm_i386.so
# Expected: ELF 32-bit LSB shared object, Intel 80386

# Check dependencies
ldd obj-linux/amxmodx/amxmodx_mm_i386.so
```

## Next Steps

After deploying both:
1. **KTPReHLDS** Linux build (swds_linux with pfnClientCvarChanged)
2. **KTPAMXX** Linux build (amxmodx_mm_i386.so with client_cvar_changed forward)

You can update your KTPCvarChecker plugin to use the real-time `client_cvar_changed` forward instead of periodic polling.
