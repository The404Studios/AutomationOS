#!/bin/bash
#
# Sign kernel module (.ko file)
# Appends signature section to ELF module
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
KEYS_DIR="$PROJECT_ROOT/keys"

# Configuration
PRIVATE_KEY="$KEYS_DIR/kernel-signing-key.pem"
KEY_ID=1

# Usage
if [ $# -lt 1 ]; then
    echo "Usage: $0 <module.ko> [output-signed.ko]"
    echo ""
    echo "Signs kernel module with RSA signature"
    echo ""
    echo "Arguments:"
    echo "  module.ko       - Path to kernel module"
    echo "  output-signed   - Output path (default: overwrites input)"
    exit 1
fi

MODULE="$1"
OUTPUT="${2:-$MODULE}"

# Check files exist
if [ ! -f "$MODULE" ]; then
    echo "ERROR: Module not found: $MODULE"
    exit 1
fi

if [ ! -f "$PRIVATE_KEY" ]; then
    echo "ERROR: Private key not found: $PRIVATE_KEY"
    echo "Run scripts/generate-keys.sh first"
    exit 1
fi

echo "========================================"
echo "  Module Signing"
echo "========================================"
echo ""
echo "Module:      $MODULE"
echo "Output:      $OUTPUT"
echo "Key:         $PRIVATE_KEY"
echo "Key ID:      $KEY_ID"
echo ""

# Step 1: Compute SHA-256 hash
echo "[1/3] Computing SHA-256 hash..."
MODULE_HASH=$(openssl dgst -sha256 -binary "$MODULE" | xxd -p -c 256)
echo "  Hash: $MODULE_HASH"

# Step 2: Sign hash with RSA
echo "[2/3] Signing with RSA..."
TEMP_HASH=$(mktemp)
TEMP_SIG=$(mktemp)
echo "$MODULE_HASH" | xxd -r -p > "$TEMP_HASH"
openssl rsautl -sign -inkey "$PRIVATE_KEY" -keyform PEM \
    -in "$TEMP_HASH" -out "$TEMP_SIG"
echo "  Signature length: $(wc -c < "$TEMP_SIG") bytes"

# Step 3: Append signature to module
echo "[3/3] Appending signature..."
if [ "$OUTPUT" != "$MODULE" ]; then
    cp "$MODULE" "$OUTPUT"
fi

# Append signature header (same format as kernel)
python3 - << EOF
import struct
import binascii

# Read signature
with open('$TEMP_SIG', 'rb') as f:
    signature_data = f.read()

# Create signature header
SIGNATURE_MAGIC = 0x5349474E
header = struct.pack('<IIIII',
    SIGNATURE_MAGIC,
    1,    # version
    1,    # hash_algo
    1,    # sig_algo
    $KEY_ID
)

hash_bytes = binascii.unhexlify('$MODULE_HASH')
header += hash_bytes

sig_len = len(signature_data)
header += struct.pack('<I', sig_len)
header += signature_data
header += b'\x00' * (512 - sig_len)

# Append to module
with open('$OUTPUT', 'ab') as f:
    f.write(header)

print(f"  Signature appended: {len(header)} bytes")
EOF

# Cleanup
rm -f "$TEMP_HASH" "$TEMP_SIG"

echo ""
echo "========================================"
echo "  Module Signed Successfully"
echo "========================================"
echo ""
echo "Signed module: $OUTPUT"
echo ""
