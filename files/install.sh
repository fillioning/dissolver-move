#!/bin/bash
set -euo pipefail

# ─── Dissolver — Install to Move ──────────────────────────────────────

MODULE_ID="dissolver"
COMPONENT_TYPE="audio_fx"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
DIST_DIR="$PROJECT_DIR/dist/$MODULE_ID"
REMOTE_PATH="/data/UserData/schwung/modules/$COMPONENT_TYPE/$MODULE_ID"

echo "── Deploying Dissolver to Move ──"

# Check Move is reachable
if ! ssh -o ConnectTimeout=3 root@move.local "echo ok" &>/dev/null; then
    echo "✗ Cannot reach Move at move.local"
    echo "  Connect Move via USB or check your network."
    exit 1
fi

# Remove previous installation
echo "Removing old installation..."
ssh root@move.local "rm -rf $REMOTE_PATH"

# Copy new build
echo "Copying new build..."
scp -r "$DIST_DIR" "root@move.local:$(dirname $REMOTE_PATH)/"

# Fix ownership
echo "Setting permissions..."
ssh root@move.local "chown -R ableton:users $REMOTE_PATH"

# Verify
echo ""
echo "── Installed files ──"
ssh root@move.local "ls -la $REMOTE_PATH/"

echo ""
echo "✓ Dissolver deployed. Restart Move to load."

# Try soft restart
ssh root@move.local "systemctl restart schwung 2>/dev/null || echo '→ Manual restart required (hold power button)'"
