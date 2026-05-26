#!/bin/bash
# =============================================================================
# setup_assets.sh — Gera os assets necessários para o build do PS4 Torrent
#
# Pré-requisito: OpenOrbis Toolchain instalado e OO_PS4_TOOLCHAIN definido.
# Uso: ./scripts/setup_assets.sh
# =============================================================================

set -e

if [ -z "$OO_PS4_TOOLCHAIN" ]; then
    echo "ERRO: OO_PS4_TOOLCHAIN não definido."
    echo "Exporte a variável apontando para o toolchain:"
    echo "  export OO_PS4_TOOLCHAIN=/opt/openorbis"
    exit 1
fi

PKG_TOOL="$OO_PS4_TOOLCHAIN/bin/linux/PkgTool.Core"
PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
ASSETS_DIR="$PROJECT_DIR/assets"

mkdir -p "$ASSETS_DIR"

# ---------------------------------------------------------------------------
# 1. Gerar param.sfo (metadados do app PS4)
# ---------------------------------------------------------------------------
echo "==> Gerando param.sfo..."
"$PKG_TOOL" sfo_create \
    --content_id "UP0000-BTRC00001_00-0000000000000000" \
    --title      "PS4 Torrent" \
    --app_ver    "01.00" \
    --category   "gd" \
    "$ASSETS_DIR/param.sfo"

echo "   param.sfo criado."

# ---------------------------------------------------------------------------
# 2. Gerar icon0.png (placeholder 512x512 via ImageMagick, se disponível)
# ---------------------------------------------------------------------------
if command -v convert &>/dev/null; then
    echo "==> Gerando icon0.png (512x512)..."
    convert -size 512x512 \
        -background '#1a1a2e' \
        -fill '#e94560' \
        -font Helvetica-Bold -pointsize 80 \
        -gravity center \
        'label:PS4\nTorrent' \
        "$ASSETS_DIR/icon0.png"
    echo "   icon0.png criado."
else
    echo "==> ImageMagick não encontrado. Pulando icon0.png (opcional)."
fi

echo ""
echo "Assets prontos. Agora execute: make"
