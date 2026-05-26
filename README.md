# AutomationOS

AI-native operating system built from scratch.

## Phase 1: Core Foundation (MVP)

Bootable system with minimal shell.

## Build Requirements

- GCC cross-compiler for x86_64-elf
- NASM (assembler)
- Python 3.11+
- QEMU (for testing)
- xorriso (for ISO generation)

## Quick Start

```bash
# Setup toolchain
bash scripts/setup-toolchain.sh

# Build everything
make all

# Run in QEMU
make qemu
```

## Architecture

- **Bootloader**: AutoBoot (custom UEFI bootloader)
- **Kernel**: Higher-half x86_64 monolithic kernel
- **Userspace**: Init + simple shell

## Development

- `make bootloader` - Build AutoBoot
- `make kernel` - Build kernel
- `make userspace` - Build userspace programs
- `make iso` - Generate bootable ISO
- `make qemu` - Test in QEMU
- `make qemu-debug` - Debug with GDB
- `make clean` - Clean build artifacts
