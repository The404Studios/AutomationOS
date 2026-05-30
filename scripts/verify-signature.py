#!/usr/bin/env python3
"""
Verify signature on signed kernel/module
"""

import sys
import struct
import hashlib

SIGNATURE_MAGIC = 0x5349474E

def verify_signature(filepath):
    """Verify signature header on signed binary"""

    with open(filepath, 'rb') as f:
        data = f.read()

    # Find signature header at end
    # Header format: magic(4) + version(4) + hash_algo(4) + sig_algo(4) + key_id(4)
    #                + hash(32) + sig_size(4) + signature(512)
    header_size = 4 + 4 + 4 + 4 + 4 + 32 + 4 + 512
    if len(data) < header_size:
        print("ERROR: File too small to contain signature")
        return False

    # Extract header from end
    binary_data = data[:-header_size]
    header_data = data[-header_size:]

    # Parse header
    magic, version, hash_algo, sig_algo, key_id = struct.unpack('<IIIII', header_data[0:20])

    if magic != SIGNATURE_MAGIC:
        print(f"ERROR: Invalid signature magic: 0x{magic:08x} (expected 0x{SIGNATURE_MAGIC:08x})")
        return False

    embedded_hash = header_data[20:52]
    sig_size = struct.unpack('<I', header_data[52:56])[0]
    signature = header_data[56:56+sig_size]

    print(f"Signature header found:")
    print(f"  Version:       {version}")
    print(f"  Hash algo:     {hash_algo} (SHA-256)")
    print(f"  Sig algo:      {sig_algo} (RSA-{2048 if sig_algo == 1 else 4096})")
    print(f"  Key ID:        {key_id}")
    print(f"  Signature len: {sig_size} bytes")

    # Compute hash of binary data
    computed_hash = hashlib.sha256(binary_data).digest()

    print(f"\nHash verification:")
    print(f"  Embedded:  {embedded_hash.hex()}")
    print(f"  Computed:  {computed_hash.hex()}")

    if embedded_hash != computed_hash:
        print("  Status:    FAILED - Hash mismatch")
        return False

    print("  Status:    PASSED")

    # Note: We don't verify RSA signature here since we'd need the public key
    # The bootloader will do the full RSA verification
    print("\nNote: RSA signature verification will be performed by bootloader")

    return True

def main():
    if len(sys.argv) != 2:
        print("Usage: verify-signature.py <signed-binary>")
        sys.exit(1)

    filepath = sys.argv[1]

    print(f"Verifying signature on: {filepath}")
    print("=" * 60)

    if verify_signature(filepath):
        print("\n" + "=" * 60)
        print("Signature verification: SUCCESS")
        sys.exit(0)
    else:
        print("\n" + "=" * 60)
        print("Signature verification: FAILED")
        sys.exit(1)

if __name__ == '__main__':
    main()
