# Secure Boot Chain Setup Guide

## Overview

AutomationOS implements a complete secure boot chain with RSA signature verification:

```
UEFI Firmware → Bootloader → Kernel → Modules
     (UEFI SB)    (verify)   (verify)
```

All components are cryptographically verified before execution to ensure integrity.

## Quick Start

### 1. Generate Signing Keys

```bash
cd AutomationOS
bash scripts/generate-keys.sh
```

This creates:
- `keys/kernel-signing-key.pem` - Private key (RSA-2048)
- `keys/kernel-signing-key-pub.pem` - Public key
- `boot/boot_pubkey.h` - Embedded public key for bootloader

**IMPORTANT**: Store the private key securely! Never commit it to version control.

### 2. Build and Sign Kernel

```bash
make kernel
bash scripts/sign-kernel.sh build/kernel.elf build/kernel.elf.signed
```

### 3. Build Bootable Image

```bash
make iso
```

The build system automatically uses the signed kernel when creating the ISO.

### 4. Test in QEMU

```bash
make qemu
```

The bootloader will verify the kernel signature before loading.

## Architecture

### Signature Format

Each signed binary (kernel or module) has an appended signature header:

```c
struct signature_header {
    uint32_t magic;      // 0x5349474E ("SIGN")
    uint32_t version;    // Format version (1)
    uint32_t hash_algo;  // 1 = SHA-256
    uint32_t sig_algo;   // 1 = RSA-2048, 2 = RSA-4096
    uint32_t key_id;     // For key rotation
    uint8_t hash[32];    // SHA-256 of data
    uint8_t signature[256/512]; // RSA signature
};
```

### Verification Process

1. **Hash Computation**: SHA-256 hash of binary data
2. **Hash Comparison**: Compare with embedded hash
3. **RSA Verification**: Decrypt signature with public key, verify PKCS#1 v1.5 padding and hash

### Trusted Keyring

The kernel maintains a keyring of trusted public keys:
- Key ID 1: Root key (embedded in bootloader)
- Keys 2-15: Additional keys (can be added/revoked at runtime)

## Key Management

### Key Rotation

To rotate signing keys:

1. Generate new key pair:
```bash
bash scripts/generate-keys.sh
```

2. Add new key to trusted keyring (in kernel)
3. Sign new binaries with new key
4. Revoke old key after transition period

### Key Revocation

To revoke a compromised key:

```c
crypto_revoke_key(key_id);
```

Revoked keys are rejected during signature verification.

## Module Signing

### Sign a Module

```bash
bash scripts/sign-module.sh build/my_module.ko
```

### Enforce Signed Modules

By default, the kernel requires all modules to be signed. To disable (for development):

```c
module_set_signature_enforcement(false);
```

### Check Module Signature

```bash
python3 scripts/verify-signature.py build/my_module.ko
```

## Security Properties

### What is Protected

✅ Kernel binary integrity (bootloader verifies before loading)
✅ Kernel module integrity (kernel verifies before loading)
✅ Tampering detection (any modification invalidates signature)
✅ Rollback protection (with version checks)

### Threat Model

**Protected Against:**
- Malicious kernel modifications
- Unsigned kernel execution
- Rootkit module loading
- Supply chain attacks (if keys are protected)

**NOT Protected Against:**
- Physical attacks (DMA, JTAG)
- Bootloader compromise (requires UEFI Secure Boot)
- Key theft (store keys in HSM/TPM for production)

## UEFI Secure Boot Integration

For full chain-of-trust, integrate with UEFI Secure Boot:

### 1. Generate Platform Key (PK)

```bash
openssl req -new -x509 -newkey rsa:2048 -subj "/CN=Platform Key/" \
    -keyout PK.key -out PK.crt -days 3650 -nodes
```

### 2. Sign Bootloader

```bash
sbsign --key PK.key --cert PK.crt \
    build/BOOTX64.EFI --output build/BOOTX64.EFI.signed
```

### 3. Enroll Keys in UEFI

Use `efi-updatevar` or UEFI Setup to enroll PK, KEK, and db keys.

## Performance

Signature verification overhead:
- **SHA-256 hashing**: ~500 MB/s (software)
- **RSA-2048 verify**: ~5ms per signature
- **Total boot time impact**: <100ms

## Troubleshooting

### Boot Fails with "Signature Verification Failed"

**Cause**: Kernel signature is invalid or tampered

**Solution**:
1. Verify kernel is signed: `python3 scripts/verify-signature.py build/kernel.elf`
2. Regenerate signature: `bash scripts/sign-kernel.sh build/kernel.elf`
3. Rebuild ISO: `make iso`

### Module Load Fails

**Cause**: Module is unsigned or signature is invalid

**Solution**:
1. Sign module: `bash scripts/sign-module.sh build/module.ko`
2. Or disable enforcement (dev only): `module_set_signature_enforcement(false)`

### Keys Not Found

**Cause**: Keys not generated

**Solution**:
```bash
bash scripts/generate-keys.sh
```

## Best Practices

### Development

- Use signature enforcement disabled for faster iteration
- Keep development and production keys separate
- Test signed kernels before deployment

### Production

- Generate keys on air-gapped machine
- Store private keys in HSM or TPM
- Enable UEFI Secure Boot
- Use key rotation (annually)
- Monitor audit logs for signature failures

### CI/CD

- Generate ephemeral keys for CI builds
- Sign release builds with production keys
- Verify signatures in automated tests
- Archive signed binaries with signatures

## Security Audit

Cryptographic implementation follows:
- **FIPS 180-4**: SHA-256
- **PKCS#1 v1.5**: RSA signatures
- **RFC 8017**: RSA cryptography

Timing-safe comparison prevents timing attacks on signature verification.

## Advanced: Custom Crypto Backend

To use hardware crypto acceleration:

1. Implement `crypto_backend_t` interface
2. Register backend: `crypto_register_backend()`
3. Rebuild kernel

Example backends:
- **AES-NI**: Intel AES instructions
- **TPM 2.0**: Trusted Platform Module
- **SE**: Secure Enclave (Apple Silicon)

## References

- [UEFI Secure Boot Specification](https://uefi.org/sites/default/files/resources/UEFI_Spec_2_10.pdf)
- [Linux Kernel Module Signing](https://www.kernel.org/doc/html/latest/admin-guide/module-signing.html)
- [PKCS#1 v1.5](https://tools.ietf.org/html/rfc8017)
- [FIPS 180-4 SHA](https://nvlpubs.nist.gov/nistpubs/FIPS/NIST.FIPS.180-4.pdf)

## Support

For issues or questions:
- File issue on GitHub
- Email: security@automationos.dev
- Read: `docs/security/capability-guide.md`
