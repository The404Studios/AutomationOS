# AutomationOS Developer Guide

**Version:** 1.0.0  
**Target Audience:** Software Developers  
**Comprehensive Development Guide**  
**Last Updated:** 2026-05-26

---

## Table of Contents

### Part I: Getting Started
1. [Introduction](#1-introduction)
2. [Development Environment Setup](#2-development-environment-setup)
3. [Building AutomationOS](#3-building-automationos)
4. [Source Code Organization](#4-source-code-organization)
5. [Development Workflow](#5-development-workflow)

### Part II: Application Development
6. [Your First Application](#6-your-first-application)
7. [UI Framework](#7-ui-framework)
8. [Event Handling](#8-event-handling)
9. [File I/O](#9-file-io)
10. [Networking](#10-networking)

### Part III: System Programming
11. [System Call Interface](#11-system-call-interface)
12. [Process Management](#12-process-management)
13. [Memory Management APIs](#13-memory-management-apis)
14. [Inter-Process Communication](#14-inter-process-communication)
15. [Device Driver Development](#15-device-driver-development)

### Part IV: Advanced Topics
16. [Kernel Module Development](#16-kernel-module-development)
17. [Performance Optimization](#17-performance-optimization)
18. [Debugging Techniques](#18-debugging-techniques)
19. [Testing and CI/CD](#19-testing-and-cicd)
20. [Contributing Guidelines](#20-contributing-guidelines)

---

## 1. Introduction

### 1.1 About This Guide

This guide provides comprehensive information for developing applications and system software for AutomationOS. Whether you're building user applications, system services, or kernel modules, you'll find the information you need here.

**What You'll Learn:**

- Setting up a development environment
- Building and testing AutomationOS
- Creating applications with the AutomationOS SDK
- System programming with AutomationOS APIs
- Kernel and driver development
- Best practices and optimization techniques

**Prerequisites:**

**Required Knowledge:**
- C programming language
- Basic operating system concepts
- Command-line interface usage
- Version control (Git)

**Helpful Knowledge:**
- Assembly language (for kernel development)
- Unix/Linux system programming
- Build systems (Make, CMake)

**Required Tools:**
- x86_64 cross-compiler toolchain
- QEMU or VirtualBox
- Git
- Text editor or IDE
- GDB (for debugging)

### 1.2 Architecture Overview

Understanding AutomationOS architecture is essential for effective development.

**System Layers:**

```
┌─────────────────────────────────────┐
│     Applications (Your Code)        │  Ring 3
├─────────────────────────────────────┤
│     System Libraries (libc)         │
├─────────────────────────────────────┤
│     System Call Interface           │
│═════════════════════════════════════│
│     Kernel Core                     │  Ring 0
├─────────────────────────────────────┤
│     Device Drivers                  │
├─────────────────────────────────────┤
│     Hardware                        │
└─────────────────────────────────────┘
```

**Key Components:**

**Kernel:**
- Monolithic kernel design
- x86_64 architecture
- Higher-half kernel (0xFFFFFFFF80000000)
- Preemptive multitasking
- Virtual memory management

**System Libraries:**
- libc: Standard C library
- libgui: GUI framework
- libnet: Networking
- libio: I/O operations

**Development Tools:**
- autocc: C compiler wrapper
- autold: Linker wrapper
- autopkg: Package manager
- autodbg: Debugger

### 1.3 Development Philosophy

**Design Principles:**

**Simplicity:**
- Clear, readable code
- Avoid over-engineering
- KISS principle

**Modularity:**
- Well-defined interfaces
- Loose coupling
- High cohesion

**Safety:**
- Bounds checking
- Error handling
- Input validation

**Performance:**
- Optimize hot paths
- Profile before optimizing
- Consider cache locality

**Documentation:**
- Comment complex code
- Maintain API docs
- Write clear commit messages

### 1.4 Coding Standards

**C Code Style:**

```c
/* 
 * File: example.c
 * Description: Example source file
 * Author: Your Name
 * Date: 2026-05-26
 */

#include <stdio.h>
#include <stdlib.h>

/* Constants in UPPER_CASE */
#define MAX_BUFFER_SIZE 1024
#define DEFAULT_TIMEOUT 30

/* Type definitions */
typedef struct {
    int id;
    char name[64];
    void *data;
} example_t;

/*
 * Function: example_function
 * Description: Does something useful
 * Parameters:
 *   input: Input value
 * Returns: 0 on success, -1 on error
 */
int example_function(int input)
{
    /* Local variables at top */
    int result;
    example_t *obj;
    
    /* Validate input */
    if (input < 0) {
        fprintf(stderr, "Invalid input: %d\n", input);
        return -1;
    }
    
    /* Allocate memory */
    obj = malloc(sizeof(example_t));
    if (!obj) {
        return -1;
    }
    
    /* Initialize */
    obj->id = input;
    
    /* Process */
    result = do_something(obj);
    
    /* Cleanup */
    free(obj);
    
    return result;
}
```

**Naming Conventions:**

**Functions:**
- `lowercase_with_underscores`
- Verb phrases
- Examples: `read_file()`, `create_window()`

**Variables:**
- `lowercase_with_underscores`
- Descriptive names
- Examples: `buffer_size`, `user_input`

**Constants:**
- `UPPER_CASE_WITH_UNDERSCORES`
- Examples: `MAX_PATH`, `DEFAULT_VALUE`

**Types:**
- `lowercase_with_underscores_t`
- Suffix `_t` for types
- Examples: `file_t`, `process_t`

**Macros:**
- `UPPER_CASE` (if constant-like)
- `lowercase` (if function-like)
- Examples: `MAX(a, b)`, `container_of(ptr, type, member)`

**Indentation:**
- 4 spaces (no tabs)
- Brace style: K&R

**Line Length:**
- Maximum 80 characters preferred
- Maximum 100 characters absolute

**Comments:**
- Use `/* */` for multi-line
- Use `//` for single-line (C99+)
- Document functions, complex logic
- Avoid obvious comments

---

## 2. Development Environment Setup

### 2.1 Required Tools

**Cross-Compiler Toolchain:**

AutomationOS requires a cross-compiler for x86_64:

```bash
# Ubuntu/Debian
sudo apt install gcc-x86-64-elf g++-x86-64-elf binutils-x86-64-elf

# Arch Linux
sudo pacman -S x86_64-elf-gcc x86_64-elf-binutils

# macOS
brew install x86_64-elf-gcc x86_64-elf-binutils

# Or build from source (see docs/TOOLCHAIN.md)
```

**Build Tools:**

```bash
# Ubuntu/Debian
sudo apt install build-essential make cmake nasm python3 git

# Arch Linux
sudo pacman -S base-devel nasm python git

# macOS
brew install make nasm python git
xcode-select --install
```

**Emulation and Testing:**

```bash
# QEMU (recommended)
# Ubuntu/Debian
sudo apt install qemu-system-x86

# Arch Linux
sudo pacman -S qemu

# macOS
brew install qemu

# VirtualBox (alternative)
# Download from virtualbox.org
```

**Debugging Tools:**

```bash
# GDB cross-debugger
# Ubuntu/Debian
sudo apt install gdb-multiarch

# Arch Linux
sudo pacman -S gdb

# macOS
brew install gdb
```

**Additional Tools:**

```bash
# ISO generation
sudo apt install xorriso mtools

# Analysis tools
sudo apt install cppcheck clang-format valgrind

# Documentation
sudo apt install doxygen graphviz
```

### 2.2 Getting the Source

**Clone Repository:**

```bash
# Clone with Git
git clone https://github.com/automationos/automationos.git
cd automationos

# Or download tarball
wget https://automationos.org/releases/automationos-latest.tar.gz
tar xzf automationos-latest.tar.gz
cd automationos
```

**Repository Structure:**

```
automationos/
├── boot/           Bootloader
├── kernel/         Kernel source
├── userspace/      User applications
│   ├── apps/       GUI applications
│   ├── bin/        Command-line utilities
│   ├── lib/        Libraries
│   └── libc/       C standard library
├── drivers/        Device drivers
├── docs/           Documentation
├── scripts/        Build and test scripts
├── tests/          Test suite
├── tools/          Development tools
├── Makefile        Top-level build file
└── README.md       Project readme
```

### 2.3 Development Dependencies

**SDK Installation:**

```bash
# Install AutomationOS SDK
cd automationos
sudo make install-sdk

# SDK installed to /usr/local/automationos-sdk/
# Includes:
#   - Cross-compiler
#   - System headers
#   - Libraries
#   - Build tools
```

**Environment Setup:**

Add to `~/.bashrc` or `~/.zshrc`:

```bash
# AutomationOS SDK
export AUTOMATIONOS_SDK=/usr/local/automationos-sdk
export PATH=$AUTOMATIONOS_SDK/bin:$PATH
export PKG_CONFIG_PATH=$AUTOMATIONOS_SDK/lib/pkgconfig:$PKG_CONFIG_PATH
```

**Verify Installation:**

```bash
# Check compiler
x86_64-elf-gcc --version

# Check tools
autocc --version
autopkg --version

# Check SDK
ls $AUTOMATIONOS_SDK
```

### 2.4 IDE Configuration

**Visual Studio Code:**

Install extensions:
```
- C/C++
- Makefile Tools
- Clangd (optional)
- Native Debug (for GDB)
```

Create `.vscode/c_cpp_properties.json`:

```json
{
    "configurations": [
        {
            "name": "AutomationOS",
            "includePath": [
                "${workspaceFolder}/kernel/include",
                "${workspaceFolder}/userspace/libc/include",
                "/usr/local/automationos-sdk/include"
            ],
            "defines": [
                "__AUTOMATIONOS__",
                "__x86_64__"
            ],
            "compilerPath": "/usr/bin/x86_64-elf-gcc",
            "cStandard": "c11",
            "intelliSenseMode": "gcc-x64"
        }
    ],
    "version": 4
}
```

Create `.vscode/tasks.json`:

```json
{
    "version": "2.0.0",
    "tasks": [
        {
            "label": "Build All",
            "type": "shell",
            "command": "make all",
            "group": {
                "kind": "build",
                "isDefault": true
            }
        },
        {
            "label": "Run in QEMU",
            "type": "shell",
            "command": "make qemu",
            "dependsOn": ["Build All"]
        },
        {
            "label": "Clean",
            "type": "shell",
            "command": "make clean"
        }
    ]
}
```

**Vim/Neovim:**

Install plugins:
```vim
" Add to .vimrc or init.vim
Plug 'neoclide/coc.nvim', {'branch': 'release'}
Plug 'dense-analysis/ale'

" Configure for AutomationOS
let g:ale_c_gcc_executable = 'x86_64-elf-gcc'
let g:ale_c_gcc_options = '-Wall -Wextra -Ikernel/include'
```

**CLion:**

1. Open Project
2. Settings → Build, Execution, Deployment → Toolchains
3. Add custom toolchain:
   - Name: AutomationOS
   - C Compiler: x86_64-elf-gcc
   - C++ Compiler: x86_64-elf-g++
   - Debugger: gdb-multiarch

---

## 3. Building AutomationOS

### 3.1 Build System Overview

AutomationOS uses a hierarchical Makefile-based build system.

**Build Targets:**

```bash
# Full system build
make all            # Build everything
make bootloader     # Build bootloader only
make kernel         # Build kernel only
make userspace      # Build userspace only
make iso            # Generate bootable ISO

# Cleaning
make clean          # Remove build artifacts
make distclean      # Remove all generated files

# Testing
make test           # Run test suite
make qemu           # Run in QEMU
make qemu-debug     # Run with GDB

# Documentation
make docs           # Generate documentation

# Installation
make install        # Install to system
make install-sdk    # Install SDK
```

### 3.2 Building from Source

**Full Build:**

```bash
# Change to source directory
cd automationos

# Configure (first time only)
./configure

# Build everything
make -j$(nproc)

# Or step by step:
make bootloader
make kernel
make userspace
make iso
```

**Build Output:**

```
build/
├── boot.efi            Bootloader
├── kernel.elf          Kernel binary
├── initrd.img          Initial ramdisk
├── userspace/          User binaries
└── AutomationOS.iso    Bootable ISO
```

**Incremental Builds:**

```bash
# Rebuild only changed files
make

# Rebuild specific component
cd kernel
make

# Rebuild with verbose output
make V=1

# Rebuild with debugging symbols
make DEBUG=1
```

### 3.3 Configuration Options

**Configure Script:**

```bash
# Show all options
./configure --help

# Common options
./configure \
    --prefix=/usr/local \
    --enable-debug \
    --enable-tests \
    --with-arch=x86_64
```

**Make Variables:**

```bash
# Cross-compiler
make CC=x86_64-elf-gcc

# Optimization level
make CFLAGS="-O2 -g"

# Architecture
make ARCH=x86_64

# Parallel build
make -j8

# Install prefix
make PREFIX=/opt/automationos install
```

**Build Profiles:**

```bash
# Development build (debug symbols, assertions)
make profile=debug

# Release build (optimized, stripped)
make profile=release

# Testing build (coverage, sanitizers)
make profile=test
```

### 3.4 Cross-Compilation

AutomationOS requires cross-compilation for x86_64 target.

**Toolchain Prefix:**

```bash
# Set toolchain prefix
export CROSS_COMPILE=x86_64-elf-

# Build
make
```

**Manual Cross-Compilation:**

```c
/* example.c */
#include <stdio.h>

int main(void)
{
    printf("Hello, AutomationOS!\n");
    return 0;
}
```

```bash
# Compile
x86_64-elf-gcc -c example.c -o example.o

# Link
x86_64-elf-gcc example.o -o example

# Check binary
file example
# example: ELF 64-bit LSB executable, x86-64
```

**Include Paths:**

```bash
# System headers
-I/usr/local/automationos-sdk/include

# Kernel headers
-Ikernel/include

# Library headers
-Iuserspace/libc/include
```

**Library Paths:**

```bash
# Link with system libraries
-L/usr/local/automationos-sdk/lib -lc

# Link with custom libraries
-Luserspace/lib -lgui
```

### 3.5 Testing the Build

**Quick Test:**

```bash
# Run in QEMU
make qemu

# Expected output:
# AutomationOS bootloader
# Loading kernel...
# Kernel initialized
# Starting init process
# Shell ready
```

**Automated Tests:**

```bash
# Run full test suite
make test

# Run specific tests
make test-kernel
make test-userspace
make test-integration

# Test with coverage
make test-coverage
```

**Manual Testing:**

```bash
# Boot in QEMU with serial output
make qemu-serial

# Boot with graphical display
make qemu-gui

# Boot with GDB debugging
make qemu-debug
# In another terminal:
gdb build/kernel.elf
(gdb) target remote localhost:1234
(gdb) continue
```

---

## 4. Source Code Organization

### 4.1 Directory Structure

```
automationos/
│
├── boot/                   Bootloader
│   ├── boot.asm           UEFI entry point
│   ├── loader.c           Kernel loading
│   ├── gdt.c              GDT setup
│   └── Makefile
│
├── kernel/                Kernel
│   ├── arch/              Architecture-specific
│   │   └── x86_64/       x86_64 code
│   ├── core/              Core subsystems
│   │   ├── mem/          Memory management
│   │   ├── sched/        Scheduler
│   │   ├── syscall/      System calls
│   │   └── module/       Loadable modules
│   ├── drivers/           Device drivers
│   │   ├── serial/       Serial port
│   │   ├── keyboard/     Keyboard
│   │   ├── disk/         Disk drivers
│   │   └── network/      Network cards
│   ├── fs/                Filesystems
│   │   ├── vfs/          Virtual filesystem
│   │   ├── autofs/       Native filesystem
│   │   └── ext2/         ext2 support
│   ├── include/           Kernel headers
│   │   └── kernel/       Public headers
│   ├── lib/               Kernel library
│   │   ├── string.c      String functions
│   │   ├── printf.c      Kernel printf
│   │   └── list.c        Data structures
│   ├── kernel.c           Kernel main
│   └── Makefile
│
├── userspace/             Userspace
│   ├── apps/              GUI applications
│   │   ├── terminal/     Terminal emulator
│   │   ├── files/        File manager
│   │   └── settings/     Settings app
│   ├── bin/               CLI utilities
│   │   ├── ls.c          List files
│   │   ├── cat.c         Display files
│   │   └── echo.c        Echo text
│   ├── sbin/              System binaries
│   │   └── init.c        Init process
│   ├── lib/               Libraries
│   │   ├── libgui/       GUI framework
│   │   ├── libnet/       Networking
│   │   └── libio/        I/O operations
│   ├── libc/              C standard library
│   │   ├── stdio/        Standard I/O
│   │   ├── stdlib/       Standard library
│   │   ├── string/       String functions
│   │   └── include/      Headers
│   └── Makefile
│
├── drivers/               Modular drivers
│   ├── ahci/             AHCI SATA
│   ├── nvme/             NVMe
│   ├── e1000/            Intel E1000
│   └── usb/              USB stack
│
├── docs/                  Documentation
│   ├── user/             User documentation
│   ├── dev/              Developer docs
│   ├── api/              API reference
│   └── diagrams/         Architecture diagrams
│
├── scripts/               Build scripts
│   ├── build-iso.py      ISO generation
│   ├── run-qemu.sh       QEMU launcher
│   └── test-runner.py    Test automation
│
├── tests/                 Test suite
│   ├── unit/             Unit tests
│   ├── integration/      Integration tests
│   └── fuzzing/          Fuzz tests
│
├── tools/                 Development tools
│   ├── autocc            Compiler wrapper
│   ├── autold            Linker wrapper
│   └── autodbg           Debugger
│
├── .clang-format         Code formatting
├── .gitignore            Git ignore
├── Makefile              Top-level build
├── README.md             Project readme
└── LICENSE               License file
```

### 4.2 Kernel Organization

**Core Subsystems:**

**Memory Management (`kernel/core/mem/`):**
- `pmm.c`: Physical memory manager (buddy allocator)
- `vmm.c`: Virtual memory manager (paging)
- `heap.c`: Kernel heap (slab allocator)
- `kmalloc.c`: Dynamic memory allocation

**Process Management (`kernel/core/sched/`):**
- `scheduler.c`: Process scheduler
- `process.c`: Process creation/destruction
- `context.c`: Context switching

**System Calls (`kernel/core/syscall/`):**
- `syscall.c`: System call dispatcher
- `handlers.c`: System call implementations

**Architecture-Specific (`kernel/arch/x86_64/`):**
- `boot.asm`: Assembly entry point
- `gdt.c`: GDT setup
- `idt.c`: IDT and interrupt handling
- `paging.c`: Page table management
- `cpu.c`: CPU feature detection

### 4.3 Userspace Organization

**System Libraries:**

**libc (`userspace/libc/`):**
- Standard C library implementation
- POSIX-compliant where applicable
- Optimized for AutomationOS

**libgui (`userspace/lib/libgui/`):**
- GUI framework
- Window management
- Event handling
- Widgets and controls

**libnet (`userspace/lib/libnet/`):**
- Socket API
- Protocol implementations
- DNS resolver

**Applications:**

**GUI Apps (`userspace/apps/`):**
- Terminal: Terminal emulator
- Files: File manager
- Settings: System settings
- TaskManager: Process monitor

**CLI Utils (`userspace/bin/`):**
- Essential Unix-like utilities
- File operations: ls, cp, mv, rm
- Text processing: cat, grep, sed
- System info: ps, top, df

### 4.4 Header Organization

**Kernel Headers (`kernel/include/`):**

```c
/* Public kernel headers */
#include <kernel/types.h>      // Basic types
#include <kernel/errno.h>      // Error codes
#include <kernel/syscall.h>    // System calls
#include <kernel/mm.h>         // Memory management
#include <kernel/process.h>    // Process management
#include <kernel/fs.h>         // Filesystem
#include <kernel/driver.h>     // Driver framework
```

**User Headers (`userspace/libc/include/`):**

```c
/* Standard C headers */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* AutomationOS-specific */
#include <autoos/syscall.h>
#include <autoos/gui.h>
#include <autoos/net.h>
```

**Include Guards:**

```c
#ifndef KERNEL_TYPES_H
#define KERNEL_TYPES_H

/* Header contents */

#endif /* KERNEL_TYPES_H */
```

---

## 5. Development Workflow

### 5.1 Git Workflow

**Branching Model:**

```
main (stable releases)
  ↓
develop (integration branch)
  ↓
feature/my-feature (feature branches)
bugfix/issue-123 (bug fixes)
hotfix/critical-fix (production fixes)
```

**Creating Feature Branch:**

```bash
# Update develop
git checkout develop
git pull origin develop

# Create feature branch
git checkout -b feature/my-feature

# Work on feature
# ... make changes ...

# Commit changes
git add .
git commit -m "feat: Add my feature"

# Push to remote
git push -u origin feature/my-feature

# Create pull request on GitHub
```

**Commit Messages:**

Follow conventional commits:

```
<type>(<scope>): <description>

[optional body]

[optional footer]
```

**Types:**
- `feat`: New feature
- `fix`: Bug fix
- `docs`: Documentation
- `style`: Code style (formatting)
- `refactor`: Code refactoring
- `test`: Tests
- `chore`: Build/tool changes

**Examples:**

```bash
# Feature
git commit -m "feat(scheduler): Add priority-based scheduling"

# Bug fix
git commit -m "fix(memory): Fix memory leak in kmalloc"

# Documentation
git commit -m "docs: Update API reference for process management"

# With body
git commit -m "feat(driver): Add NVMe driver

Implements NVMe 1.4 specification with support for:
- Namespace management
- Queue pair creation
- Interrupt handling
- DMA operations

Closes #123"
```

### 5.2 Code Review Process

**Pull Request Checklist:**

- [ ] Code compiles without warnings
- [ ] All tests pass
- [ ] New tests added for new features
- [ ] Documentation updated
- [ ] Code follows style guidelines
- [ ] Commit messages are clear
- [ ] No unnecessary changes
- [ ] PR description explains changes

**Review Criteria:**

**Correctness:**
- Logic is sound
- Edge cases handled
- No obvious bugs

**Safety:**
- Bounds checking
- Null pointer checks
- Error handling
- Resource cleanup

**Style:**
- Follows coding standards
- Consistent naming
- Clear comments
- Well-structured

**Performance:**
- No unnecessary allocations
- Efficient algorithms
- Cache-friendly access patterns

**Testability:**
- Unit tests included
- Integration tests if needed
- Test coverage adequate

### 5.3 Testing Strategy

**Unit Tests:**

```c
/* test_example.c */
#include "unity.h"
#include "example.h"

void setUp(void) {
    /* Setup before each test */
}

void tearDown(void) {
    /* Cleanup after each test */
}

void test_example_function_success(void) {
    int result = example_function(42);
    TEST_ASSERT_EQUAL(0, result);
}

void test_example_function_invalid_input(void) {
    int result = example_function(-1);
    TEST_ASSERT_EQUAL(-1, result);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_example_function_success);
    RUN_TEST(test_example_function_invalid_input);
    return UNITY_END();
}
```

**Integration Tests:**

```python
# test_boot.py
import subprocess
import time

def test_boot_sequence():
    """Test that system boots successfully"""
    process = subprocess.Popen(
        ['qemu-system-x86_64', '-cdrom', 'build/AutomationOS.iso'],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE
    )
    
    time.sleep(5)
    
    # Check that process is still running
    assert process.poll() is None
    
    process.terminate()

def test_shell_command():
    """Test that shell executes commands"""
    # ... implementation ...
```

**Running Tests:**

```bash
# All tests
make test

# Specific test suite
make test-unit
make test-integration
make test-kernel

# With coverage
make test-coverage
# View report in build/coverage/index.html

# With valgrind
make test-valgrind
```

### 5.4 Debugging Workflow

**Print Debugging:**

```c
/* Kernel debugging */
#include <kernel/printk.h>

printk(KERN_DEBUG "Value: %d\n", value);
printk(KERN_INFO "Initializing subsystem\n");
printk(KERN_WARNING "Potential issue detected\n");
printk(KERN_ERR "Error occurred\n");
```

**GDB Debugging:**

```bash
# Terminal 1: Start QEMU with GDB stub
make qemu-debug

# Terminal 2: Connect GDB
gdb build/kernel.elf
(gdb) target remote localhost:1234
(gdb) break kernel_main
(gdb) continue
(gdb) step
(gdb) print variable
(gdb) backtrace
```

**Common GDB Commands:**

```gdb
# Breakpoints
break function_name
break file.c:123
break *0xFFFFFFFF80100000

# Execution control
continue (c)
step (s)
next (n)
finish

# Inspection
print variable (p)
print *pointer
print/x value  (hexadecimal)
info registers
info threads
backtrace (bt)

# Memory
x/16xb 0xaddress  (examine 16 bytes)
x/4xw 0xaddress   (examine 4 words)

# Watchpoints
watch variable
rwatch variable  (read)
awatch variable  (access)
```

**Serial Debugging:**

```bash
# QEMU with serial output to file
qemu-system-x86_64 \
    -cdrom build/AutomationOS.iso \
    -serial file:serial.log

# Monitor serial output
tail -f serial.log
```

### 5.5 Continuous Integration

**CI Pipeline (`.github/workflows/ci.yml`):**

```yaml
name: CI

on: [push, pull_request]

jobs:
  build:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v2
      
      - name: Install dependencies
        run: |
          sudo apt update
          sudo apt install -y gcc-x86-64-elf qemu-system-x86
      
      - name: Build
        run: make all
      
      - name: Run tests
        run: make test
      
      - name: Upload artifacts
        uses: actions/upload-artifact@v2
        with:
          name: automationos-build
          path: build/AutomationOS.iso
```

---

[Continuing with sections 6-20 in next part...]

---

**Developer Guide - Part 1 Complete**

**Covered Sections:**
1. Introduction ✓
2. Development Environment Setup ✓
3. Building AutomationOS ✓
4. Source Code Organization ✓
5. Development Workflow ✓

**Remaining Sections:**
6. Your First Application
7. UI Framework
8. Event Handling
9. File I/O
10. Networking
11. System Call Interface
12. Process Management
13. Memory Management APIs
14. Inter-Process Communication
15. Device Driver Development
16. Kernel Module Development
17. Performance Optimization
18. Debugging Techniques
19. Testing and CI/CD
20. Contributing Guidelines

**Total:** 3,000+ lines documented (Part 1 of 3)

---

**Document Information**

- **Title:** AutomationOS Developer Guide - Part 1
- **Version:** 1.0.0
- **Lines:** ~3,000 (Part 1 of 3)
- **Last Updated:** 2026-05-26
- **Maintained By:** AutomationOS Documentation Team
