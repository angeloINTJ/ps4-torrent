#!/bin/bash
# =============================================================================
# setup_assets.sh — Generate the assets required to build PS4 Torrent
#
# Prerequisite: OpenOrbis Toolchain installed and OO_PS4_TOOLCHAIN set.
# Usage: ./scripts/setup_assets.sh
# =============================================================================

set -e

if [ -z "$OO_PS4_TOOLCHAIN" ]; then
    echo "ERROR: OO_PS4_TOOLCHAIN not set."
    echo "Export the variable pointing to the toolchain:"
    echo "  export OO_PS4_TOOLCHAIN=/opt/openorbis"
    exit 1
fi

PKG_TOOL="$OO_PS4_TOOLCHAIN/bin/linux/PkgTool.Core"
PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
ASSETS_DIR="$PROJECT_DIR/assets"

mkdir -p "$ASSETS_DIR"

# ---------------------------------------------------------------------------
# 1. Generate param.sfo (PS4 app metadata)
# ---------------------------------------------------------------------------
echo "==> Generating param.sfo..."

"$PKG_TOOL" sfo_new "$ASSETS_DIR/param.sfo"

"$PKG_TOOL" sfo_setentry --value "0"       --type integer --maxsize 4   "$ASSETS_DIR/param.sfo" "APP_TYPE"
"$PKG_TOOL" sfo_setentry --value "01.00"   --type string  --maxsize 8   "$ASSETS_DIR/param.sfo" "APP_VER"
"$PKG_TOOL" sfo_setentry --value "0"       --type integer --maxsize 4   "$ASSETS_DIR/param.sfo" "ATTRIBUTE"
"$PKG_TOOL" sfo_setentry --value "gd"      --type string  --maxsize 4   "$ASSETS_DIR/param.sfo" "CATEGORY"
"$PKG_TOOL" sfo_setentry --value "UP0000-BTRC00001_00-0000000000000000" --type string --maxsize 48 "$ASSETS_DIR/param.sfo" "CONTENT_ID"
"$PKG_TOOL" sfo_setentry --value "PS4 Torrent" --type string --maxsize 128 "$ASSETS_DIR/param.sfo" "TITLE"
"$PKG_TOOL" sfo_setentry --value "BTRC00001" --type string --maxsize 12 "$ASSETS_DIR/param.sfo" "TITLE_ID"
"$PKG_TOOL" sfo_setentry --value "01.00"   --type string  --maxsize 8   "$ASSETS_DIR/param.sfo" "VERSION"

echo "   param.sfo created."

# ---------------------------------------------------------------------------
# 2. Generate icon0.png (512x512 placeholder via ImageMagick, if available)
# ---------------------------------------------------------------------------
if command -v convert &>/dev/null; then
    echo "==> Generating icon0.png (512x512)..."
    convert -size 512x512 \
        -background '#1a1a2e' \
        -fill '#e94560' \
        -font Helvetica-Bold -pointsize 80 \
        -gravity center \
        'label:PS4\nTorrent' \
        "$ASSETS_DIR/icon0.png"
    echo "   icon0.png created."
else
    echo "==> ImageMagick not found. Skipping icon0.png (optional)."
fi

echo ""
echo "Assets ready. Now run: make"
