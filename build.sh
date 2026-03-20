#!/bin/bash
# VectorDoom WASM build script
# Usage: ./build.sh [clean|configure]
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# Prefer project-local emsdk (stable), fall back to /tmp/emsdk
if [ -d "$SCRIPT_DIR/emsdk/upstream/emscripten" ]; then
    EMSDK_DIR="$SCRIPT_DIR/emsdk"
elif [ -d "/tmp/emsdk/upstream/emscripten" ]; then
    EMSDK_DIR="/tmp/emsdk"
else
    echo "ERROR: No emsdk found at $SCRIPT_DIR/emsdk or /tmp/emsdk"
    echo "Install: cd $SCRIPT_DIR && git clone https://github.com/emscripten-core/emsdk.git && cd emsdk && ./emsdk install 5.0.2 && ./emsdk activate 5.0.2"
    exit 1
fi
echo "Using emsdk at: $EMSDK_DIR"

# Find node + python
NODE_DIR=$(ls -d "$EMSDK_DIR"/node/*/bin 2>/dev/null | head -1)
PYTHON_DIR=$(ls -d "$EMSDK_DIR"/python/*/bin 2>/dev/null | head -1)

export EMSDK="$EMSDK_DIR"
export EM_CONFIG="$EMSDK_DIR/.emscripten"
export PATH="$EMSDK_DIR/upstream/emscripten:$NODE_DIR:$PYTHON_DIR:/usr/bin:/bin:/usr/sbin:/sbin:/opt/homebrew/bin"

if [ "$1" = "configure" ]; then
    echo "Configuring..."
    emconfigure autoreconf -fiv
    ac_cv_exeext=".html" emconfigure ./configure --host=none-none-none
    echo "Configure complete! Now run: ./build.sh"
    exit 0
fi

if [ "$1" = "clean" ]; then
    echo "Cleaning..."
    emmake make clean
    exit 0
fi

echo "Building..."
emmake make -j4 2>&1
echo "Build complete!"
