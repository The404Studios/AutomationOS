#!/bin/bash
#
# Download stb_truetype.h from official repository
#

set -e

FONT_DIR="$(dirname "$0")"
cd "$FONT_DIR"

echo "Downloading stb_truetype.h from GitHub..."

curl -L -o stb_truetype.h \
    "https://raw.githubusercontent.com/nothings/stb/master/stb_truetype.h"

if [ -f "stb_truetype.h" ]; then
    SIZE=$(stat -f%z "stb_truetype.h" 2>/dev/null || stat -c%s "stb_truetype.h" 2>/dev/null)
    echo "✓ Downloaded stb_truetype.h ($SIZE bytes)"

    # Verify it's actually the header file
    if head -1 stb_truetype.h | grep -q "stb_truetype"; then
        echo "✓ File verified"
    else
        echo "✗ File verification failed - may be corrupted"
        exit 1
    fi
else
    echo "✗ Download failed"
    exit 1
fi

echo ""
echo "stb_truetype.h is ready for use!"
echo "License: Public Domain / MIT (Sean Barrett)"
