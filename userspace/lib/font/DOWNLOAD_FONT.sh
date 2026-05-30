#!/bin/bash
#
# Download DejaVu Sans font for bundling with AutomationOS
#

set -e

FONT_DIR="$(dirname "$0")"
cd "$FONT_DIR"

echo "Downloading DejaVu Sans font..."

# DejaVu Fonts 2.37 (latest stable)
DEJAVU_VERSION="2.37"
DEJAVU_URL="https://github.com/dejavu-fonts/dejavu-fonts/releases/download/version_${DEJAVU_VERSION//./_}/dejavu-fonts-ttf-${DEJAVU_VERSION}.tar.bz2"

# Download
echo "Fetching $DEJAVU_URL..."
curl -L -o dejavu-fonts.tar.bz2 "$DEJAVU_URL"

# Extract
echo "Extracting..."
tar xjf dejavu-fonts.tar.bz2

# Copy the fonts we need
mkdir -p fonts
cp "dejavu-fonts-ttf-${DEJAVU_VERSION}/ttf/DejaVuSans.ttf" fonts/
cp "dejavu-fonts-ttf-${DEJAVU_VERSION}/ttf/DejaVuSans-Bold.ttf" fonts/
cp "dejavu-fonts-ttf-${DEJAVU_VERSION}/ttf/DejaVuSansMono.ttf" fonts/

# Copy license
cp "dejavu-fonts-ttf-${DEJAVU_VERSION}/LICENSE" fonts/DEJAVU_LICENSE.txt

# Cleanup
rm -rf dejavu-fonts.tar.bz2 "dejavu-fonts-ttf-${DEJAVU_VERSION}"

echo ""
echo "✓ DejaVu Sans fonts downloaded to fonts/"
ls -lh fonts/*.ttf

echo ""
echo "License: Bitstream Vera + Arev Fonts (Free)"
echo "See fonts/DEJAVU_LICENSE.txt for details"
