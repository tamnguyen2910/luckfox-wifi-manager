#!/bin/bash
# Deploy script for WiFi Manager app with expect-based commands.
# Reads board config from config/board.env (git-ignored, contains secrets).
set -e

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_DIR/build"
ENV_FILE="$PROJECT_DIR/config/board.env"
EXPECT="$SCRIPT_DIR/deploy.exp"

if [ ! -f "$ENV_FILE" ]; then
    echo "Error: $ENV_FILE not found."
    echo "Copy config/board.env.example to config/board.env and edit with your board settings."
    exit 1
fi

source "$ENV_FILE"

BINARY="wifi-manager"
if [ ! -f "$BUILD_DIR/$BINARY" ]; then
    echo "Error: Binary not found at $BUILD_DIR/$BINARY"
    echo "Run build.sh first"
    exit 1
fi

echo "=== Deploying $BINARY to $BOARD_IP ==="

expect "$EXPECT" "$ENV_FILE" "$BUILD_DIR/$BINARY"
echo "=== Deploy completed ==="
