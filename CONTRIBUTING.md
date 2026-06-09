# Contributing

AutomationOS is a from-scratch hobby OS built with a deliberate, hardware-safe discipline. If you
want to hack on it, here's how the project works.

## The "brick" workflow

Work lands as **bricks**: small, isolated, hard-gated milestones.

- Each brick gets its **own branch** (`brick/<name>`) off a known-good baseline.
- Each brick has a **scoped design** (`docs/superpowers/specs/…`), a short doc under
  [`docs/bricks/`](docs/bricks/), explicit **risks**, **acceptance tests**, and **merge criteria**.
- Checkpoints are committed one at a time so history is bisectable.
- Brick specs are the source of truth; the doc under `docs/bricks/` is the summary.

## Hardware-safety laws (non-negotiable)

1. **New or risky hardware support is default-OFF behind a build flag.** A default build is
   byte-for-byte the validated configuration.
2. **Every device/controller wait is iteration-capped** with a tick-independent delay. The default
   scheduler is cooperative single-core — an unbounded wait in a syscall freezes the machine.
3. **QEMU first, real hardware second.** New hardware must pass the no-device and device-present
   boots in QEMU before it is flashed to the T410.
4. **Verify before claiming.** Findings are checked against the real code/registers before a patch;
   green means a build + boot-smoke gate actually passed.

## Build & test

```bash
bash scripts/quick_build.sh         # kernel only (fast)
bash scripts/build_all.sh           # userspace + ISO
bash scripts/smoke_boot.sh --build  # rebuild + boot smoke
```

Builds run under **WSL (Arch Linux)** with a stock host toolchain. See [docs/hardware.md](docs/hardware.md)
for the T410-safe profile and [docs/testing.md](docs/testing.md) for the smoke harness.

## Commits & releases

- Small, focused commits; conventional prefixes (`feat`, `fix`, `build`, `docs`, `test`).
- Tag known-good states (e.g. `stable-t410-recovery`) before risky changes.
- Cut a release for user-visible milestones (e.g. `v0.1-t410-desktop`, `v0.2-ide-projects`).

## What help is most useful

EHCI/USB on real hardware, the 82577LM NIC bring-up, writable filesystems, and browser
web-compatibility are the highest-leverage areas — all tracked on the [roadmap](docs/ROADMAP.md).
