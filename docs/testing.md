# Testing

## Boot smoke harness

`scripts/smoke_boot.sh` boots the ISO under QEMU, captures the serial log, and asserts a set of
boot invariants — kernel start, no panics/faults, fork+CoW isolation, the on-device compiler,
crypto/TLS known-answer tests, the networking + socket path, and the desktop/compositor coming up.

```bash
bash scripts/smoke_boot.sh            # boot the existing ISO and check
bash scripts/smoke_boot.sh --build    # rebuild the kernel + ISO first, then check
bash scripts/smoke_boot.sh --iso build/automationos-t410.iso   # test a specific image
```

## Honest reporting

The **kernel → desktop path passes every run**. The harness also exercises experimental surfaces,
and a handful of its checks are *known* non-passes rather than silent failures:

- a harness **tag artifact** (a `[KERNEL]`-tag check that the log format no longer emits),
- **build-gated selftests** (heap/slab selftests compiled out under `BOOT_QUIET`; `RQLOCK`/`AFFINITY`
  checks that only exist under `SCHED_DEBUG`),
- **experimental browser-wave apps** (userspace exceptions in the from-scratch browser pipeline).

The number passing therefore depends on the build profile (e.g. `SCHED_DEBUG` on/off changes which
gated checks exist). Rather than publish a single brittle "X/Y" badge, we track the catalogue above
and treat any *new* failure on the core path as a regression.

## Per-brick verification

Each [brick](bricks/) ships its own verification script under `build_test/` (for example the USB
bricks build both the default and gated kernels, boot the no-device and device-present cases in
QEMU, and assert on the serial markers + desktop-reached + no-panic). New hardware is **QEMU-first**:
it must pass the no-device and device-present boots in QEMU before it is ever flashed to the T410.

## Capturing screenshots

`build_test/shot.sh <iso> <name>` boots an image headless and uses QMP `screendump` to save
`screenshots/<name>.png` — the desktop image in the README is produced this way.
