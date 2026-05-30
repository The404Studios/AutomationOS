# Tutorial 1: Getting Started with AutomationOS

**Difficulty:** Beginner  
**Time:** 15-20 minutes  
**Prerequisites:** Basic terminal knowledge  

---

## Introduction

Welcome to AutomationOS! In this tutorial, you'll learn how to:
- Set up your development environment
- Build AutomationOS from source
- Boot the OS in QEMU
- Understand what you're seeing on screen

By the end of this tutorial, you'll have AutomationOS running on your machine.

---

## What is AutomationOS?

AutomationOS is a modern, from-scratch operating system built for AI and automation workloads. It's a real operating system that:
- Boots on x86_64 hardware
- Manages memory with a buddy allocator and paging
- Runs processes with a scheduler
- Provides system calls for userspace programs

Phase 1 gives you a minimal but complete foundation - a bootable kernel with a simple shell.

---

## Prerequisites

### System Requirements

- **OS:** Linux (Ubuntu/Debian/Arch), macOS, or WSL2 on Windows
- **RAM:** 4GB minimum
- **Disk:** 500MB free space
- **Internet:** For downloading tools

### Required Tools

You'll need:
1. **GCC cross-compiler** for x86_64 bare-metal
2. **NASM** assembler
3. **QEMU** x86_64 emulator
4. **xorriso** for creating bootable ISOs
5. **Make** and basic build tools

Don't worry - we'll install everything step by step.

---

## Step 1: Install Dependencies

### Ubuntu/Debian

Open a terminal and run:

```bash
sudo apt update
sudo apt install build-essential nasm python3 qemu-system-x86 xorriso git
```

### Arch Linux

```bash
sudo pacman -S base-devel nasm python qemu xorriso git
```

### macOS

First install Homebrew if you haven't already:

```bash
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
```

Then install the tools:

```bash
brew install nasm python qemu xorriso git
```

**Verify installation:**

```bash
nasm --version    # Should show NASM version
qemu-system-x86_64 --version    # Should show QEMU version
xorriso --version # Should show xorriso version
```

---

## Step 2: Install Cross-Compiler Toolchain

AutomationOS requires a **cross-compiler** - a special compiler that builds code for x86_64 bare-metal (no OS).

### Quick Install (Ubuntu/Debian)

```bash
sudo apt install gcc-x86-64-elf binutils-x86-64-elf
```

### Quick Install (Arch Linux)

```bash
sudo pacman -S x86_64-elf-gcc x86_64-elf-binutils
```

### Build from Source (All Platforms)

If the quick install doesn't work, or you're on macOS, you'll need to build the toolchain from source. This takes 30-60 minutes.

AutomationOS includes an automated script:

```bash
# Download the repository first (next step)
cd AutomationOS
bash scripts/setup-toolchain.sh
```

The script will:
1. Download binutils and GCC source code
2. Build x86_64-elf cross-compiler
3. Install to `/usr/local/cross`
4. Add to your PATH

**Verify cross-compiler:**

```bash
x86_64-elf-gcc --version
x86_64-elf-ld --version
```

You should see version information for both tools.

---

## Step 3: Clone the Repository

Get the AutomationOS source code:

```bash
cd ~/Desktop  # Or wherever you keep projects
git clone <repository-url> AutomationOS
cd AutomationOS
```

Take a moment to explore:

```bash
ls -l
```

You'll see:
- `boot/` - UEFI bootloader
- `kernel/` - OS kernel
- `userspace/` - Init process and shell
- `scripts/` - Build and test scripts
- `docs/` - Documentation
- `Makefile` - Build system

---

## Step 4: Build AutomationOS

Now let's build everything!

### Full Build

```bash
make all
```

This will:
1. Build the UEFI bootloader (AutoBoot)
2. Compile the kernel
3. Build userspace programs (init, shell)
4. Create a bootable ISO image

**Expected output:**

```
[BOOTLOADER] Building AutoBoot UEFI bootloader...
[BOOTLOADER] Built: build/BOOTX64.EFI
[KERNEL] Building kernel...
[KERNEL] Built: build/kernel.elf
[USERSPACE] Building userspace programs...
[USERSPACE] Built: build/userspace/init/init
[USERSPACE] Built: build/userspace/shell/shell
[ISO] Creating bootable ISO...
[ISO] Created: build/AutomationOS.iso
✓ Build complete!
```

**Build time:** About 20-30 seconds on a modern machine.

### Understanding Build Artifacts

Check what was created:

```bash
ls -lh build/
```

You'll see:
- `BOOTX64.EFI` - UEFI bootloader (executable)
- `kernel.elf` - Kernel binary (ELF format)
- `AutomationOS.iso` - Bootable ISO image
- `userspace/` - Init and shell binaries

---

## Step 5: Boot in QEMU

Time to see it run!

```bash
make qemu
```

QEMU will open a window showing AutomationOS booting.

### What You'll See

After a moment, you should see:

```
=====================================
   AutomationOS v0.1.0
=====================================

[BOOTLOADER] AutoBoot UEFI Bootloader v0.1
[BOOTLOADER] Loading kernel from ISO...
[BOOTLOADER] Kernel loaded at 0xFFFFFFFF80000000
[BOOTLOADER] Jumping to kernel...

[KERNEL] AutomationOS kernel starting...
[GDT] Initializing Global Descriptor Table...
[GDT] GDT loaded
[PMM] Initializing physical memory manager...
[PMM] Total memory: 4096 MB
[PMM] Available memory: 4080 MB
[VMM] Initializing virtual memory manager...
[VMM] 4-level paging enabled
[HEAP] Initializing kernel heap...
[HEAP] Kernel heap initialized (32 MB)
[IDT] Initializing Interrupt Descriptor Table...
[IDT] IDT loaded (256 entries)
[PIT] Initializing timer at 100Hz...
[PIT] Timer initialized
[SERIAL] Serial console enabled (COM1)
[KEYBOARD] PS/2 keyboard initialized
[SCHEDULER] Initializing process scheduler...
[SCHEDULER] Round-robin scheduler ready
[SYSCALL] Initializing system call interface...
[SYSCALL] Registered 5 syscalls
[KERNEL] All subsystems initialized
[KERNEL] Free memory: 4048 MB

[INIT] Init process (PID 1) starting...
[INIT] Spawning shell...
[SHELL] Shell (PID 2) starting...

aos>
```

**Congratulations!** AutomationOS is running. The `aos>` prompt is the shell waiting for your input.

### Understanding Boot Output

Let's break down what happened:

1. **UEFI Bootloader** - AutoBoot loaded the kernel into memory
2. **Kernel Initialization:**
   - **GDT** - Set up memory segmentation
   - **PMM** - Buddy allocator manages physical RAM
   - **VMM** - 4-level paging for virtual memory
   - **Heap** - Slab allocator for kernel memory
   - **IDT** - Interrupt handling (keyboard, timer, etc.)
   - **PIT** - Programmable Interval Timer
   - **Drivers** - Serial console, PS/2 keyboard
   - **Scheduler** - Process scheduling
   - **Syscalls** - System call interface
3. **Init Process (PID 1)** - First userspace process
4. **Shell (PID 2)** - Interactive command-line interface

---

## Step 6: Interact with the Shell

Try some commands in the shell:

```bash
# Display help
aos> help

# Print a message
aos> echo Hello, AutomationOS!

# Show your process ID
aos> pid

# Exit the shell (will be respawned by init)
aos> exit
```

### Available Commands

- `echo [args...]` - Print arguments to stdout
- `help` - Show available commands
- `exit` - Exit the shell (init will respawn it)
- `clear` - Clear the screen
- `pid` - Show shell process ID

---

## Step 7: Exit QEMU

To exit QEMU:

- **Graphical window:** Close the window
- **Command line:** Press `Ctrl+A`, then `X`
- **If stuck:** `Ctrl+C` in the terminal

---

## Understanding the Architecture

### Memory Layout

AutomationOS uses a **higher-half kernel**:

```
0x0000000000000000 - 0x00007FFFFFFFFFFF   User space (128 TB)
0xFFFF800000000000 - 0xFFFFFFFFFFFFFFFF   Kernel space (128 TB)
  0xFFFFFFFF80000000 - Kernel code starts here
```

This means:
- Userspace programs run in low memory (0x0 - 0x7FFF...)
- Kernel runs in high memory (0xFFFF...)
- Each process has its own address space
- User programs can't access kernel memory

### Process Model

AutomationOS has a Unix-like process model:

```
Init (PID 1)
  └─> Shell (PID 2)
```

- **Init** is the first process, started by the kernel
- **Shell** is spawned by init using `fork()` and `execve()`
- If shell crashes, init respawns it

---

## What Just Happened?

You've successfully:

✅ Installed all required tools  
✅ Built a complete operating system from source  
✅ Booted it in QEMU  
✅ Interacted with a shell running in your OS  

Pretty awesome for 20 minutes of work!

---

## Common Issues

### "make: x86_64-elf-gcc: Command not found"

**Problem:** Cross-compiler not installed or not in PATH.

**Solution:**
```bash
# Run toolchain setup script
bash scripts/setup-toolchain.sh

# Add to PATH (add to ~/.bashrc for permanent)
export PATH="/usr/local/cross/bin:$PATH"
```

### "Build failed" or weird errors

**Problem:** Old build artifacts.

**Solution:**
```bash
make clean
make all
```

### QEMU shows black screen

**Problem:** ISO not created properly or QEMU not finding it.

**Solution:**
```bash
# Rebuild ISO
make iso

# Try explicit path
qemu-system-x86_64 -cdrom build/AutomationOS.iso -m 512M
```

### "xorriso: command not found"

**Problem:** xorriso not installed.

**Solution:**
```bash
# Ubuntu/Debian
sudo apt install xorriso

# Arch
sudo pacman -S libisoburn

# macOS
brew install xorriso
```

---

## Next Steps

Now that you have AutomationOS running, you're ready to:

1. **Tutorial 2: Hello World** - Write your first userspace program
2. **Tutorial 3: System Calls** - Learn how to use kernel services
3. **Tutorial 4: Debugging** - Use GDB to debug the kernel

See you in the next tutorial!

---

## Summary

In this tutorial, you learned:

- What AutomationOS is and what it does
- How to install the required development tools
- How to build the entire OS from source
- How to boot AutomationOS in QEMU
- Basic shell commands
- The architecture overview

**Key Concepts:**
- Cross-compilers compile code for different architectures
- UEFI bootloaders load the kernel into memory
- Kernels initialize hardware and provide services
- Init (PID 1) is the first userspace process
- System calls allow programs to request kernel services

---

## Resources

- [QUICKSTART.md](../../QUICKSTART.md) - Quick reference
- [Architecture Guide](../ARCHITECTURE.md) - Deep dive into design
- [Build Guide](../BUILD_GUIDE.md) - Advanced build options
- [Troubleshooting](../TROUBLESHOOTING.md) - Problem solving

---

**Next Tutorial:** [02_HELLO_WORLD.md](02_HELLO_WORLD.md) - Write your first userspace program

---

*Last Updated: 2026-05-26*
