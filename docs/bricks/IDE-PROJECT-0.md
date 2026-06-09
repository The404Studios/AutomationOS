# Brick: IDE-PROJECT-0

> **Bricks** are small, isolated, hard-gated milestones with a scoped design, explicit risks,
> acceptance tests, and merge criteria.

**Goal:** a real project lifecycle in the IDE — **create → build → run → desktop icon** — where
every desktop folder/icon maps to a real VFS object, and (later) projects survive a reboot.

## Why

The IDE could compile, but output dumped into the `/Desktop` root, opening a desktop folder was
broken (icons stored only a truncated name), and nothing survived reboot. The build→run chain
itself already worked (the compiler emits a correct-ABI ELF that `SYS_SPAWN` runs); the gaps were
the **project model + desktop wiring + durability**.

## Scope

- **Phase 1 (ramfs):** New Project creates `/Desktop/Projects/<Name>/{src,build,res}` + a flat
  `project.json`; Build emits `build/<Name>.elf`; Run spawns it; the desktop surfaces projects as
  PROJECT icons and opens the real folder. **Core law:** the display name is not identity — the full
  VFS path is identity (fixes the "open the wrong folder" truncation bug).
- **Phase 2 (durable):** a gated `persistfs` mount at `/Desktop/Projects` over the proven flat
  `diskfs`, so projects survive a reboot — T410-safe (compile-gated, bounded AHCI probe, ramfs
  fallback).

## Deferred (Layer 2)

C++/Python compilers (the toolchain stays single-file C + ASM); writable ext2; making all of
`/Desktop` durable.

## Status

| Phase | Scope | Status |
|---|---|---|
| 1 | project folders, build output, desktop icon path/type, open real folder | ✅ done (pushed) |
| 2 | `persistfs` durable mount | Planned |

> Note: the Phase-1 desktop commits introduced a separate cosmetic regression (raw-path window
> titles + a stray window), tracked as its own brick `DESKTOP-PROJECT-REGRESSION-0`.

## Acceptance (Phase 1)

New Project → a PROJECT icon appears → double-click opens the **real** directory in the file
manager → edit `src/main.c` → Build emits `build/<Name>.elf` → Run spawns that exact ELF → a loose
`/Desktop/foo.c` still builds to `/Desktop/foo.elf` (no regression).
