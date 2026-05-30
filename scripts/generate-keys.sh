#!/bin/bash
#
# Generate RSA key pair for kernel/module signing
# Creates keys in keys/ directory
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
KEYS_DIR="$PROJECT_ROOT/keys"

# Configuration
KEY_SIZE=2048  # RSA key size (2048 or 4096)
KEY_NAME="kernel-signing-key"

echo "========================================"
echo "  AutomationOS Key Generation"
echo "========================================"
echo ""

# Create keys directory
mkdir -p "$KEYS_DIR"

# Check if keys already exist
if [ -f "$KEYS_DIR/$KEY_NAME.pem" ]; then
    echo "WARNING: Keys already exist in $KEYS_DIR"
    read -p "Regenerate keys? This will invalidate all existing signatures! (y/N): " -n 1 -r
    echo
    if [[ ! $REPLY =~ ^[Yy]$ ]]; then
        echo "Aborted."
        exit 1
    fi
fi

echo "[1/4] Generating RSA-$KEY_SIZE private key..."
openssl genrsa -out "$KEYS_DIR/$KEY_NAME.pem" $KEY_SIZE
chmod 600 "$KEYS_DIR/$KEY_NAME.pem"
echo "  Private key: $KEYS_DIR/$KEY_NAME.pem"

echo "[2/4] Extracting public key..."
openssl rsa -in "$KEYS_DIR/$KEY_NAME.pem" \
    -pubout -out "$KEYS_DIR/$KEY_NAME-pub.pem"
echo "  Public key: $KEYS_DIR/$KEY_NAME-pub.pem"

echo "[3/4] Extracting modulus and exponent..."
openssl rsa -pubin -in "$KEYS_DIR/$KEY_NAME-pub.pem" \
    -text -noout > "$KEYS_DIR/$KEY_NAME-pub.txt"
echo "  Key info: $KEYS_DIR/$KEY_NAME-pub.txt"

echo "[4/4] Generating C header with embedded public key..."
python3 "$SCRIPT_DIR/embed-pubkey.py" \
    "$KEYS_DIR/$KEY_NAME-pub.pem" \
    "$PROJECT_ROOT/boot/boot_pubkey.h"
echo "  Header: boot/boot_pubkey.h"

echo ""
echo "========================================"
echo "  Key Generation Complete"
echo "========================================"
echo ""
echo "IMPORTANT SECURITY NOTES:"
echo "  1. Store $KEY_NAME.pem in a secure location"
echo "  2. Never commit private keys to version control"
echo "  3. keys/ directory is in .gitignore by default"
echo "  4. Regenerating keys requires re-signing all binaries"
echo ""
echo "Public key fingerprint:"
openssl rsa -pubin -in "$KEYS_DIR/$KEY_NAME-pub.pem" -outform DER | \
    openssl dgst -sha256 -hex
echo ""
