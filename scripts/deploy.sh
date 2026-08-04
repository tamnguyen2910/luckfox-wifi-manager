#!/bin/bash

# Deploy script for WiFi Manager app with expect-based commands (no env variables)
set -e

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
BUILD_DIR="$SCRIPT_DIR/../build"
EXPECT="$SCRIPT_DIR/deploy.exp"

BINARY="wifi-manager"
BOARD_IP="192.168.1.3"
BOARD_USER="root"
BOARD_PASS="luckfox"
BOARD_DEST="/root"

if [ ! -f "$BUILD_DIR/$BINARY" ]; then
    echo "Error: Binary not found at $BUILD_DIR/$BINARY"
    echo "Run build.sh first"
    exit 1
fi

echo "=== Deploying $BINARY to $BOARD_IP ==="
expect "$EXPECT" "$BUILD_DIR/$BINARY"
echo "=== Deploy completed ==="
