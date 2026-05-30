#!/bin/bash
#
# Sign kernel ELF binary
# Appends signature header to kernel image
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
    echo "Usage: $0 <kernel-elf> [output-signed]"
    echo ""
    echo "Signs kernel ELF with RSA signature"
    echo ""
    echo "Arguments:"
    echo "  kernel-elf      - Path to kernel ELF binary"
    echo "  output-signed   - Output path (default: <kernel-elf>.signed)"
    exit 1
fi

KERNEL_ELF="$1"
OUTPUT="${2:-$KERNEL_ELF.signed}"

# Check files exist
if [ ! -f "$KERNEL_ELF" ]; then
    echo "ERROR: Kernel not found: $KERNEL_ELF"
    exit 1
fi

if [ ! -f "$PRIVATE_KEY" ]; then
    echo "ERROR: Private key not found: $PRIVATE_KEY"
    echo "Run scripts/generate-keys.sh first"
    exit 1
fi

echo "========================================"
echo "  Kernel Signing"
echo "========================================"
echo ""
echo "Kernel:      $KERNEL_ELF"
echo "Output:      $OUTPUT"
echo "Key:         $PRIVATE_KEY"
echo "Key ID:      $KEY_ID"
echo ""

# Step 1: Compute SHA-256 hash
echo "[1/4] Computing SHA-256 hash..."
KERNEL_HASH=$(openssl dgst -sha256 -binary "$KERNEL_ELF" | xxd -p -c 256)
echo "  Hash: $KERNEL_HASH"

# Step 2: Sign hash with RSA
echo "[2/4] Signing with RSA..."
TEMP_HASH=$(mktemp)
TEMP_SIG=$(mktemp)
echo "$KERNEL_HASH" | xxd -r -p > "$TEMP_HASH"
openssl rsautl -sign -inkey "$PRIVATE_KEY" -keyform PEM \
    -in "$TEMP_HASH" -out "$TEMP_SIG"
SIGNATURE=$(xxd -p -c 256 "$TEMP_SIG")
echo "  Signature length: $(wc -c < "$TEMP_SIG") bytes"

# Step 3: Create signature header
echo "[3/4] Creating signature header..."
python3 - << EOF
import struct
import binascii

# Read kernel
with open('$KERNEL_ELF', 'rb') as f:
    kernel_data = f.read()

# Read signature
with open('$TEMP_SIG', 'rb') as f:
    signature_data = f.read()

# Create signature header
SIGNATURE_MAGIC = 0x5349474E  # "SIGN"
header = struct.pack('<IIIII',
    SIGNATURE_MAGIC,  # magic
    1,                # version
    1,                # hash_algo (SHA-256)
    1,                # sig_algo (RSA-2048)
    $KEY_ID           # key_id
)

# Add hash (32 bytes)
hash_bytes = binascii.unhexlify('$KERNEL_HASH')
header += hash_bytes

# Add signature (pad to 512 bytes for RSA-2048/4096 max)
sig_len = len(signature_data)
header += struct.pack('<I', sig_len)
header += signature_data
header += b'\x00' * (512 - sig_len)  # Padding

# Write signed kernel
with open('$OUTPUT', 'wb') as f:
    f.write(kernel_data)
    f.write(header)

print(f"  Header size: {len(header)} bytes")
print(f"  Total size: {len(kernel_data) + len(header)} bytes")
EOF

# Cleanup
rm -f "$TEMP_HASH" "$TEMP_SIG"

# Step 4: Verify signature
echo "[4/4] Verifying signature..."
python3 "$SCRIPT_DIR/verify-signature.py" "$OUTPUT"

echo ""
echo "========================================"
echo "  Kernel Signed Successfully"
echo "========================================"
echo ""
echo "Signed kernel: $OUTPUT"
echo ""
