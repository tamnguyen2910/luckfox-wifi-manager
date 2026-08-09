#!/bin/bash
# ============================================================
# WiFi Manager - Build & Deploy Script
# Cross-compiles for Luckfox Pico (RV1106/RV1103)
# and deploys via SSH/SCP using expect script
# ============================================================
set -e

# ─── Configuration ─────────────────────────────
APP_NAME="wifi-manager"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$SCRIPT_DIR/.."

# Load board config from git-ignored config/board.env
ENV_FILE="$PROJECT_DIR/config/board.env"
if [ -f "$ENV_FILE" ]; then
    source "$ENV_FILE"
elif [ -z "$BOARD_IP" ]; then
    echo "Error: $ENV_FILE not found."
    echo "Copy config/board.env.example to config/board.env and edit with your board settings."
    exit 1
fi

# Try to find qmake from environment, or use default path
QMAKE="${QMAKE:-/home/tamnguyen/Desktop/LINUX/Build_Luckfox/luckfox-pico/sysdrv/source/buildroot/buildroot-2023.02.6/output/host/bin/qmake}"

BUILD_DIR="${PROJECT_DIR}/build"

# ─── Parse arguments ───────────────────────────
DO_CLEAN=false
for arg in "$@"; do
    case "$arg" in
        --clean|-c) DO_CLEAN=true ;;
    esac
done

# ─── Step 1: qmake ─────────────────────────────
echo "=== [1/3] qmake ==="
cd "$PROJECT_DIR"
if [ "$DO_CLEAN" = true ]; then
    echo "Cleaning build directory..."
    rm -rf "$BUILD_DIR"
fi
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"
"$QMAKE" "${PROJECT_DIR}/wifi-manager.pro"

# ─── Step 2: make ──────────────────────────────
echo "=== [2/3] make ==="
make -j$(nproc)

if [ ! -f "$APP_NAME" ]; then
    echo "ERROR: Build failed! $APP_NAME not found."
    exit 1
fi
echo "Build OK: $BUILD_DIR/$APP_NAME"

# ─── Step 3: deploy ────────────────────────────
echo "=== [3/3] deploy to $BOARD_IP ==="
expect "${SCRIPT_DIR}/deploy.exp" "$ENV_FILE" "$BUILD_DIR/$APP_NAME"

echo ""
echo "BUILD + DEPLOY OK"
echo "Run on board:"
echo "  export QT_QPA_PLATFORM=$QT_QPA_PLATFORM"
echo "  export QT_QPA_FONTDIR=$QT_QPA_FONTDIR"
echo "  export LD_LIBRARY_PATH=$LD_LIBRARY_PATH"
echo "  $BOARD_DEST/$APP_NAME"
