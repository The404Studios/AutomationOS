# Security policy

AutomationOS is a from-scratch hobby operating system. It is **not** hardened
for hostile multi-user or production use. This document is an honest statement
of its current security posture — what is enforced, what is deliberately not,
and what is deferred — so nobody relies on a guarantee that is not there.

## Reporting

Please open a private security advisory on the GitHub repository
(`Security` → `Report a vulnerability`), or a regular issue for lower-severity
findings. Include a reproduction and the affected commit.

## Supported configuration

- **Default build:** single-CPU (cooperative + PIT-preemptive) scheduler, boots
  to RAM (ramfs + initrd). This is the configuration the boot smoke suite gates.
- **SMP scheduler** (`SMP_SCHED`) is experimental and **off by default**.

## What is enforced

- Ring-3 ↔ ring-0 syscall boundary: user-pointer validation (`copy_user_*`),
  cross-process ownership gates on sockets, dir handles, and shared memory,
  ELF-image bounds checking, and W^X on anonymous/heap pages (NX).
- TLS: correct X.509 chain verification math (RSA/ECDSA, `basicConstraints`
  cA), hostname matching with embedded-NUL rejection, and X25519 low-order
  rejection. See the ROBUST hardening history.
- TLS trust is now **enforced by default at the client layer.** The HTTP layer
  surfaces the per-fetch verdict (`http_last_trusted()`); `wget` refuses to emit
  the body of an HTTPS response whose certificate chain did not authenticate
  against a built-in CA root (override with `-k`/`--insecure`), and `browser2`
  shows a red address-bar indicator for encrypted-but-unauthenticated pages
  instead of a false green padlock.

## Known limitations (deliberate or deferred — do NOT assume otherwise)

- **HTTPS trust anchors to a fixed 7-root CA bundle.** The client fetchers
  (`wget`, `browser2`) now fail closed on an unauthenticated chain, but the
  built-in root set (`ca_roots_data.h`) is small and never refreshed at runtime:
  a server whose chain anchors to a root outside that bundle is refused even
  though it is legitimate (`-k` overrides in `wget`). There is no OCSP/CRL
  revocation check. Any *other* in-OS TLS consumer that does not itself gate on
  `tls_cert_trusted()` / `http_last_trusted()` remains encrypted-but-unauthenticated.
- **Poweroff/reboot are intentionally unrestricted.** This is a single-user
  desktop where Shut Down / Restart are ordinary GUI apps; a `pid==1` gate would
  break those buttons, and there is no capability model to distinguish them.
- **Capability, seccomp, and rlimit subsystems are non-enforcing scaffolding** —
  present in the tree but not wired into syscall dispatch. Do not rely on them.
- **SMP-only races** (documented, reachable only with the gated SMP scheduler,
  not the default single-core build): `sys_rt_sigreturn` global sig-frame,
  `copy_user` TOCTOU-vs-`munmap` (no ring-0 exception-fixup table yet),
  PID-reuse ordering, channel-grant/refcount locking, cross-CPU TLB shootdown.
- **`SYS_RANDOM` is not a CSPRNG without RDRAND.** On QEMU/modern CPUs it uses
  RDRAND; the physical ThinkPad T410 target (pre-Ivy-Bridge, no RDRAND) falls
  back to a weak PRNG — low-entropy TLS keys there. A real DRBG is deferred
  hardware-tail work.
- **No file-permission enforcement** (mode/uid/gid are stored but not checked);
  acceptable for a single-user OS.
- **No certificate revocation** (OCSP/CRL), no TLS session resumption; TLS 1.3
  is opt-in (`TLS13=1`), default is TLS 1.2.

This list is maintained from the whole-system completeness sweep; items move out
as they are closed.
