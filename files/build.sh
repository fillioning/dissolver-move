#!/bin/bash
set -euo pipefail

# ─── Dissolver — Build Script ─────────────────────────────────────────
# Cross-compiles for ARM64 (Ableton Move) using Docker

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
MODULE_ID="dissolver"

echo "╔══════════════════════════════════════╗"
echo "║  Building Dissolver for Ableton Move ║"
echo "╚══════════════════════════════════════╝"

# Ensure dist directory exists
mkdir -p "$PROJECT_DIR/dist/$MODULE_ID"

# Build Docker image for cross-compilation
docker build -t dissolver-builder -f "$SCRIPT_DIR/Dockerfile" "$PROJECT_DIR"

# Create container, compile, extract artifact
CONTAINER_ID=$(docker create dissolver-builder)
docker cp "$CONTAINER_ID:/build/dissolver.so" "$PROJECT_DIR/dist/$MODULE_ID/dissolver.so"
docker rm "$CONTAINER_ID" > /dev/null

# Copy module.json alongside the .so
cp "$PROJECT_DIR/src/module.json" "$PROJECT_DIR/dist/$MODULE_ID/"

# Verify
echo ""
echo "── Build output ──"
ls -la "$PROJECT_DIR/dist/$MODULE_ID/"
echo ""
echo "✓ Build complete: dist/$MODULE_ID/"
