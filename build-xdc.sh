#!/bin/bash
# VectorDoom WebXDC Build Script
# Packages the compiled DOOM WASM + assets into a .xdc file

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Remove old build
rm -f vectordoom.xdc

# Create staging directory
rm -rf _xdc_staging
mkdir -p _xdc_staging

# Copy built files
cp src/vector-doom.js _xdc_staging/vector-doom.js
cp src/vector-doom.wasm _xdc_staging/vector-doom.wasm
cp src/webxdc-net.js _xdc_staging/webxdc-net.js
cp src/index.html _xdc_staging/index.html
cp src/doom1.wad _xdc_staging/doom1.wad
cp src/default.cfg _xdc_staging/default.cfg
cp manifest.toml _xdc_staging/manifest.toml

# Create icon if it doesn't exist
if [ ! -f icon.png ]; then
    cat > _xdc_staging/icon.svg << 'ICONEOF'
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 128 128">
  <rect width="128" height="128" fill="#1a0000"/>
  <text x="64" y="78" text-anchor="middle" font-family="monospace" font-weight="bold" font-size="36" fill="#cc0000">DOOM</text>
</svg>
ICONEOF
else
    cp icon.png _xdc_staging/icon.png
fi

# === OPTIMIZATION ===

# JS minification with terser (parallel)
if command -v npx &> /dev/null && npx terser --version &> /dev/null 2>&1; then
    echo "Minifying JS with terser..."

    ORIG_ENGINE=$(wc -c < _xdc_staging/vector-doom.js | tr -d ' ')

    # Emscripten runtime (biggest win)
    npx terser _xdc_staging/vector-doom.js --compress --mangle -o _xdc_staging/vector-doom.js &
    PID1=$!

    # Networking layer
    npx terser _xdc_staging/webxdc-net.js --compress --mangle -o _xdc_staging/webxdc-net.js &
    PID2=$!

    wait $PID1 $PID2

    NEW_ENGINE=$(wc -c < _xdc_staging/vector-doom.js | tr -d ' ')
    echo "  vector-doom.js: ${ORIG_ENGINE} -> ${NEW_ENGINE} bytes ($(( 100 - NEW_ENGINE * 100 / ORIG_ENGINE ))% reduction)"
else
    echo "WARNING: terser not found, skipping JS minification (npm i -g terser)"
fi

# HTML minification (strip comments, whitespace)
echo "Minifying HTML..."
perl -0777 -pe '
    s/<!--(?!\[).*?-->//gs;
    s/^\s+//gm;
    s/\s+$//gm;
    s/\n\n+/\n/g;
' _xdc_staging/index.html > _xdc_staging/index.html.min
mv _xdc_staging/index.html.min _xdc_staging/index.html

# Cleanup
rm -f _xdc_staging/.DS_Store

# Package as .xdc with max compression
cd _xdc_staging
zip -9 -r ../vectordoom.xdc . -x ".*"
cd ..

# Cleanup
rm -rf _xdc_staging

echo ""
echo "Build complete: vectordoom.xdc"
ls -lah vectordoom.xdc
