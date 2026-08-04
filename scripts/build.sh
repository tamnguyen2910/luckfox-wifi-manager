#!/bin/bash
# ============================================================
# WiFi Manager - Build & Deploy Script
# Cross-compiles for Luckfox Pico (RV1106/RV1103)
# and deploys via SSH/SCP using expect script
# ============================================================
set -e

# ─── Configuration ─────────────────────────────
BOARD_IP="${BOARD_IP:-192.168.1.3}"
BOARD_USER="${BOARD_USER:-root}"
BOARD_PASS="${BOARD_PASS:-luckfox}"
BOARD_DEST="${BOARD_DEST:-/root}"
APP_NAME="wifi-manager"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$SCRIPT_DIR/.."

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
expect "${SCRIPT_DIR}/deploy.exp" "$BUILD_DIR/$APP_NAME"

echo ""
echo "BUILD + DEPLOY OK"
echo "Run on board:"
echo "  export QT_QPA_PLATFORM=linuxfb:fb=/dev/fb0:size=800x480:mmsize=800x480"
echo "  export QT_QPA_FONTDIR=/usr/share/fonts/dejavu/"
echo "  export LD_LIBRARY_PATH=/usr/lib:/usr/lib/qt5/lib"
echo "  $BOARD_DEST/$APP_NAME"
