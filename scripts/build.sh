#!/bin/bash
set -e

MODULE_ID="dissolver"

# Resolve repo root (works from any directory)
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

echo "Building $MODULE_ID for ARM64 (aarch64)..."
echo "Repo root: $ROOT"

# Convert MINGW path to Windows path for Docker volume mount
if [[ "$ROOT" == /c/* ]]; then
    DOCKER_ROOT="C:${ROOT:2}"
elif [[ "$ROOT" == /C/* ]]; then
    DOCKER_ROOT="C:${ROOT:2}"
else
    DOCKER_ROOT="$ROOT"
fi

docker build -t ${MODULE_ID}-builder -f "$ROOT/Dockerfile" "$ROOT"

docker run --rm \
  -v "$DOCKER_ROOT:/build" \
  ${MODULE_ID}-builder \
  bash -c "
    dos2unix /build/src/dsp/*.c /build/src/dsp/*.h 2>/dev/null || true
    mkdir -p /build/dist/$MODULE_ID
    aarch64-linux-gnu-gcc \
      -O2 -shared -fPIC -ffast-math \
      -DPFFFT_SIMD_DISABLE \
      -o /build/dist/$MODULE_ID/$MODULE_ID.so \
      /build/src/dsp/dissolver.c \
      /build/src/dsp/dissolver_spectral.c \
      /build/src/dsp/pffft.c \
      -I/build/src/dsp \
      -lm
    cp /build/module.json /build/dist/$MODULE_ID/
    echo '=== Build complete ==='
    ls -la /build/dist/$MODULE_ID/
  "

# Package for release
cd "$ROOT"
tar -czf "dist/${MODULE_ID}-module.tar.gz" -C dist "$MODULE_ID"

echo "Built: dist/$MODULE_ID/"
echo "Package: dist/${MODULE_ID}-module.tar.gz"
