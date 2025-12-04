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
export METAMOD="$(pwd)/../metamod-am"

# Check for HLSDK in common locations
if [ -d "$(pwd)/../hlsdk" ]; then
    export HLSDK="$(pwd)/../hlsdk"
elif [ -d "$(pwd)/../KTPhlsdk" ]; then
    export HLSDK="$(pwd)/../KTPhlsdk"
else
    export HLSDK="$(pwd)/../hlsdk"  # Will fail with helpful error message below
fi

echo "Build environment:"
echo "  METAMOD: $METAMOD"
echo "  HLSDK: $HLSDK"
echo ""

# Check dependencies exist
if [ ! -d "$METAMOD" ]; then
    echo "ERROR: Metamod not found at $METAMOD"
    echo "Please ensure metamod-am is in the parent directory"
    exit 1
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
    echo "You can now deploy this to your server's addons/ktpamx/dlls/ directory"
else
    echo ""
    echo "========================================"
    echo "BUILD FAILED!"
    echo "========================================"
    echo "Check the output above for errors"
    exit 1
fi
