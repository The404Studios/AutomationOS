# Security Policy

## Scope and posture

AutomationOS is a **hobby / research operating system**. It is **not** intended for production use
and makes **no security guarantees**. Do not run untrusted workloads on it or expose it to a
hostile network and rely on it to hold.

That said, security correctness is taken seriously as an engineering exercise:

- The kernel enforces ring 0/3 separation, per-process address-space isolation (fork + CoW), and a
  bounded syscall surface.
- Untrusted-input parsers (the JS/HTML/CSS engines, the network and font/filesystem parsers, the
  terminal) have been through systematic **adversarial finder → verifier** bug-hunt sweeps, with
  fixes committed across the parser and syscall boundaries.
- New hardware support is gated off by default, and all device waits are iteration-capped, limiting
  the blast radius of a misbehaving device.

The crypto library (SHA-2/3, HMAC, AES, ChaCha20-Poly1305, RSA, X25519, P-256, HKDF) and the TLS
1.2 client are **hand-rolled and unaudited** — treat them as educational, not trustworthy.

## Reporting a vulnerability

This is a personal project. If you find a security issue, please open a GitHub issue (or contact
the maintainer) describing the problem and a reproduction. There is no bounty and no SLA, but
reports are welcome and appreciated.

## Not covered

No formal threat model, no side-channel resistance, no secure boot, no signed updates, no
production hardening. Boots in legacy BIOS / CSM mode only.
