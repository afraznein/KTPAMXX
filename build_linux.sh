#!/bin/bash
# KTP AMX Linux Build Script
# Run this on your Ubuntu server

set -e  # Exit on error

echo "========================================"
echo "KTP AMX Linux Build Script"
echo "========================================"
echo "Working directory: $(pwd)"

# Check for required tools
echo "Checking for required tools..."

if ! command -v git &> /dev/null; then
    echo "ERROR: git is not installed. Install with: sudo apt-get install git"
    exit 1
fi

if ! command -v python3 &> /dev/null; then
    echo "ERROR: python3 is not installed. Install with: sudo apt-get install python3 python3-pip"
    exit 1
fi

if ! command -v gcc &> /dev/null; then
    echo "ERROR: gcc is not installed. Install with: sudo apt-get install build-essential g++-multilib"
    exit 1
fi

# Check for 32-bit build support
if ! dpkg --print-foreign-architectures | grep -q i386; then
    echo "WARNING: 32-bit architecture support may not be enabled"
    echo "You may need to run: sudo dpkg --add-architecture i386 && sudo apt-get update"
fi

echo "All required tools found!"
echo ""

# Set up Python virtual environment for AMBuild
VENV_DIR="$HOME/.ambuild-venv"
echo "Checking for AMBuild virtual environment..."
if [ ! -f "$VENV_DIR/bin/activate" ]; then
    echo "Creating virtual environment at $VENV_DIR..."
    rm -rf "$VENV_DIR"
    python3 -m venv "$VENV_DIR"
fi

# Activate virtual environment
source "$VENV_DIR/bin/activate"

# Install AMBuild if not present
echo "Checking for AMBuild..."
if ! python3 -c "import ambuild2" 2>/dev/null; then
    echo "AMBuild not found. Installing..."

    if [ ! -d "support/ambuild" ]; then
        echo "ERROR: AMBuild directory not found. Run checkout-deps.sh first"
        exit 1
    fi

    cd support/ambuild
    pip install .
    cd ../..
    echo "AMBuild installed!"
else
    echo "AMBuild already installed!"
fi
echo ""

# Set environment variables
# Check for metamod (optional for extension mode builds)
if [ -d "$(pwd)/../metamod-am" ]; then
    export METAMOD="$(pwd)/../metamod-am"
elif [ -d "$(pwd)/../metamod" ]; then
    export METAMOD="$(pwd)/../metamod"
else
    # No metamod found - will build extension mode only
    export METAMOD=""
fi

# Check for HLSDK in common locations
if [ -d "$(pwd)/../hlsdk" ]; then
    export HLSDK="$(pwd)/../hlsdk"
elif [ -d "$(pwd)/../KTPhlsdk" ]; then
    export HLSDK="$(pwd)/../KTPhlsdk"
else
    export HLSDK="$(pwd)/../hlsdk"  # Will fail with helpful error message below
fi

echo "Build environment:"
if [ -n "$METAMOD" ]; then
    echo "  METAMOD: $METAMOD"
else
    echo "  METAMOD: (not found - extension mode only)"
fi
echo "  HLSDK: $HLSDK"
echo ""

# Check dependencies exist
# Metamod is optional for extension mode builds
if [ -z "$METAMOD" ]; then
    echo "NOTE: Metamod not found. Building in extension mode only."
fi

if [ ! -d "$HLSDK" ]; then
    echo "ERROR: HLSDK not found at $HLSDK"
    echo "Please ensure hlsdk is in the parent directory"
    exit 1
fi

# Clean previous build folder
echo "Cleaning previous build..."
rm -rf obj-linux 2>/dev/null || true

# Configure build
# --no-plugins: Skip plugin compilation (plugins are platform-independent, compile on Windows instead)
echo "Configuring build..."
python3 configure.py --enable-optimize --no-mysql --no-plugins

# Build
echo "Building KTP AMX..."
cd obj-linux
python3 $(which ambuild)
cd ..

# Check if build succeeded
BINARY_PATH="obj-linux/packages/base/addons/ktpamx/dlls/ktpamx_i386.so"
if [ -f "$BINARY_PATH" ]; then
    echo ""
    echo "========================================"
    echo "BUILD SUCCESS!"
    echo "========================================"
    echo "Binary: $BINARY_PATH"
    ls -lh "$BINARY_PATH"
    echo ""

    # Deploy to staging folder for easy VPS upload
    # This is a Windows path accessed via WSL
    DEPLOY_DIR="/mnt/n/Nein_/KTP DoD Server"
    if [ -d "$DEPLOY_DIR" ]; then
        echo "Deploying to staging folder..."

        # Deploy main AMXX binary
        mkdir -p "$DEPLOY_DIR/dod/addons/ktpamx/dlls"
        cp "$BINARY_PATH" "$DEPLOY_DIR/dod/addons/ktpamx/dlls/"
        echo "  -> Copied ktpamx_i386.so"

        # Deploy DODX module if built
        DODX_PATH="obj-linux/packages/dod/addons/ktpamx/modules/dodx_ktp_i386.so"
        if [ -f "$DODX_PATH" ]; then
            mkdir -p "$DEPLOY_DIR/dod/addons/ktpamx/modules"
            cp "$DODX_PATH" "$DEPLOY_DIR/dod/addons/ktpamx/modules/"
            echo "  -> Copied dodx_ktp_i386.so"
        fi

        # Deploy Fun module if built
        FUN_PATH="obj-linux/packages/base/addons/ktpamx/modules/fun_ktp_i386.so"
        if [ -f "$FUN_PATH" ]; then
            mkdir -p "$DEPLOY_DIR/dod/addons/ktpamx/modules"
            cp "$FUN_PATH" "$DEPLOY_DIR/dod/addons/ktpamx/modules/"
            echo "  -> Copied fun_ktp_i386.so"
        fi

        # Deploy Engine module if built
        ENGINE_PATH="obj-linux/packages/base/addons/ktpamx/modules/engine_ktp_i386.so"
        if [ -f "$ENGINE_PATH" ]; then
            mkdir -p "$DEPLOY_DIR/dod/addons/ktpamx/modules"
            cp "$ENGINE_PATH" "$DEPLOY_DIR/dod/addons/ktpamx/modules/"
            echo "  -> Copied engine_ktp_i386.so"
        fi

        # Deploy FakeMeta module if built
        FAKEMETA_PATH="obj-linux/packages/base/addons/ktpamx/modules/fakemeta_ktp_i386.so"
        if [ -f "$FAKEMETA_PATH" ]; then
            mkdir -p "$DEPLOY_DIR/dod/addons/ktpamx/modules"
            cp "$FAKEMETA_PATH" "$DEPLOY_DIR/dod/addons/ktpamx/modules/"
            echo "  -> Copied fakemeta_ktp_i386.so"
        fi

        # Deploy stats_logging plugin if it exists
        STATS_LOGGING_PATH="plugins/dod/stats_logging.amxx"
        if [ -f "$STATS_LOGGING_PATH" ]; then
            mkdir -p "$DEPLOY_DIR/dod/addons/ktpamx/plugins"
            cp "$STATS_LOGGING_PATH" "$DEPLOY_DIR/dod/addons/ktpamx/plugins/"
            echo "  -> Copied stats_logging.amxx"
        fi

        echo ""
        echo "Files staged at: $DEPLOY_DIR/dod/addons/ktpamx/"
        echo "Ready for upload to VPS!"
    else
        echo "Staging folder not found: $DEPLOY_DIR"
        echo "You can manually deploy from: $BINARY_PATH"
    fi
else
    echo ""
    echo "========================================"
    echo "BUILD FAILED!"
    echo "========================================"
    echo "Check the output above for errors"
    exit 1
fi
