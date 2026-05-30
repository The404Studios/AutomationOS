# AutomationOS Build System Documentation

Complete guide to building, testing, and packaging AutomationOS.

## Table of Contents

1. [Quick Start](#quick-start)
2. [Prerequisites](#prerequisites)
3. [Configuration](#configuration)
4. [Building](#building)
5. [Testing](#testing)
6. [Packaging](#packaging)
7. [CI/CD Integration](#cicd-integration)
8. [Advanced Topics](#advanced-topics)
9. [Troubleshooting](#troubleshooting)

---

## Quick Start

### Build Everything (Default)

```bash
# Setup toolchain
./scripts/setup-toolchain.sh

# Configure and build
./configure
make all

# Test in QEMU
make qemu
```

### Build Release

```bash
./scripts/build-release.sh v1.0.0
```

---

## Prerequisites

### Required Tools

| Tool | Version | Purpose |
|------|---------|---------|
| `make` | 3.81+ | Build automation |
| `x86_64-elf-gcc` | 9.0+ | Cross-compiler |
| `x86_64-elf-ld` | 2.30+ | Linker |
| `nasm` | 2.14+ | Assembler |
| `python3` | 3.7+ | Build scripts |
| `xorriso` | 1.4+ | ISO creation |

### Optional Tools

| Tool | Purpose |
|------|---------|
| `qemu-system-x86_64` | Testing and emulation |
| `clang-format` | Code formatting |
| `clang-tidy` | Static analysis |
| `cppcheck` | Bug detection |
| `lcov` | Code coverage |

### Platform-Specific Installation

#### Ubuntu/Debian

```bash
# Essential tools
sudo apt install build-essential nasm python3 git xorriso mtools

# Cross-compiler (try package first)
sudo apt install gcc-x86-64-elf binutils-x86-64-elf || \
  ./scripts/setup-toolchain.sh

# Optional tools
sudo apt install qemu-system-x86 clang-format clang-tidy cppcheck lcov
```

#### Arch Linux

```bash
# Essential tools
sudo pacman -S base-devel nasm python git libisoburn mtools

# Cross-compiler
sudo pacman -S x86_64-elf-gcc x86_64-elf-binutils

# Optional tools
sudo pacman -S qemu clang cppcheck lcov
```

#### Fedora/RHEL

```bash
# Essential tools
sudo dnf install @development-tools nasm python3 git xorriso mtools

# Cross-compiler (build from source)
./scripts/setup-toolchain.sh

# Optional tools
sudo dnf install qemu-system-x86 clang-tools-extra cppcheck lcov
```

#### macOS

```bash
# Install Homebrew if not already installed
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"

# Essential tools
brew install nasm python git xorriso mtools

# Cross-compiler
brew tap messense/macos-cross-toolchains
brew install x86_64-elf-gcc x86_64-elf-binutils

# Optional tools
brew install qemu clang-format llvm cppcheck lcov
```

---

## Configuration

The `configure` script prepares the build environment with custom options.

### Basic Configuration

```bash
./configure
```

### Configuration Options

#### Installation Options

```bash
./configure --prefix=/opt/automationos    # Custom install path
./configure --target=x86_64-automationos  # Custom target triplet
```

#### Build Type

```bash
./configure --enable-debug       # Debug build (symbols, no optimization)
./configure --enable-release     # Release build (optimized, stripped)
```

#### Optimization

```bash
./configure --enable-optimization=0      # No optimization
./configure --enable-optimization=2      # Standard optimization (default)
./configure --enable-optimization=3      # Aggressive optimization
./configure --enable-optimization=fast   # Maximum speed optimization
./configure --enable-lto                 # Link-time optimization
```

#### Features

```bash
./configure --enable-smp         # Enable SMP support (default)
./configure --disable-smp        # Disable SMP

./configure --enable-security    # Enable security features (default)
./configure --disable-security   # Disable security features

./configure --enable-tests       # Enable test suite (default)
./configure --disable-tests      # Disable tests
```

#### Drivers

```bash
# Select specific drivers
./configure --with-drivers=nvme,ahci,e1000

# All drivers (default)
./configure --with-drivers=nvme,ahci,e1000,rtl8139,usb_hcd,usb_hid,keyboard,mouse
```

#### Sanitizers (for development)

```bash
./configure --enable-sanitizer=address     # AddressSanitizer
./configure --enable-sanitizer=thread      # ThreadSanitizer
./configure --enable-sanitizer=undefined   # UndefinedBehaviorSanitizer
```

#### Toolchain

```bash
./configure --with-toolchain=/opt/cross    # Custom toolchain path
./configure --cc=clang --ld=lld            # Use Clang/LLVM
```

### Configuration Files Generated

After running `./configure`, these files are created:

- `config.mk` - Build configuration variables
- `include/config.h` - C header with configuration defines

---

## Building

### Build Targets

#### Complete Build

```bash
make all          # Build bootloader + kernel + userspace + ISO
```

#### Individual Components

```bash
make bootloader   # Build AutoBoot UEFI bootloader only
make kernel       # Build kernel only
make userspace    # Build userspace programs
make iso          # Create bootable ISO image
```

#### Clean Targets

```bash
make clean        # Remove build artifacts
make distclean    # Deep clean (including config files)
```

### Build Output

Build artifacts are placed in the `build/` directory:

```
build/
├── kernel.elf          # Kernel ELF binary
├── kernel.elf.debug    # Kernel with debug symbols
├── BOOTX64.EFI         # UEFI bootloader
├── AutomationOS.iso    # Bootable ISO image
├── boot.bin            # Legacy boot sector
└── serial.log          # QEMU serial output
```

### Parallel Builds

```bash
make -j$(nproc)         # Use all CPU cores
make -j8                # Use 8 cores
```

### Verbose Build

```bash
make V=1                # Show full commands
make --trace            # Trace make decisions
```

---

## Testing

### QEMU Testing

#### Basic Testing

```bash
make qemu               # Boot in QEMU
make qemu-debug         # Boot with GDB server
```

#### QEMU Options

```bash
# Custom memory and CPU count
./scripts/run-qemu.sh -m 8G -smp 8

# VNC display
./scripts/run-qemu.sh --vnc

# Headless mode
./scripts/run-qemu.sh --no-display
```

#### Debugging with GDB

```bash
# Terminal 1: Start QEMU with debugger
make qemu-debug

# Terminal 2: Attach GDB
gdb build/kernel.elf -ex 'target remote :1234'

# Common GDB commands
(gdb) break kernel_main
(gdb) continue
(gdb) backtrace
(gdb) info registers
```

### Unit Tests

```bash
make unit-tests         # Build and run unit tests
```

### Integration Tests

```bash
make test               # Quick integration tests
make test-full          # Full test suite (rebuild + test)
make test-integration   # Integration tests only
```

### Hardware Testing

```bash
# Create bootable USB
./scripts/create-usb.sh /dev/sdX

# Test on real hardware
# (Requires physical machine or USB boot)
```

### Benchmark Tests

```bash
make test-bench         # Run performance benchmarks
```

### Code Coverage

```bash
# Generate coverage report
make coverage

# View report
firefox docs/coverage/index.html
```

---

## Packaging

### Release Builds

Create official releases with versioning:

```bash
./scripts/build-release.sh v1.0.0
```

This generates:
- `automationos-1.0.0.iso` - Bootable ISO
- `automationos-1.0.0.tar.gz` - Binary archive
- `automationos-1.0.0-src.tar.gz` - Source archive
- `SHA256SUMS` - Checksums
- `RELEASE_NOTES_v1.0.0.md` - Release notes template

### Debian Package

```bash
./scripts/build-deb.sh 1.0.0
```

Output: `release/automationos_1.0.0_amd64.deb`

Install:
```bash
sudo dpkg -i release/automationos_1.0.0_amd64.deb
sudo apt-get install -f  # Install dependencies
```

### RPM Package

```bash
./scripts/build-rpm.sh 1.0.0
```

Output: `release/automationos-1.0.0-1.x86_64.rpm`

Install:
```bash
sudo dnf install release/automationos-1.0.0-1.x86_64.rpm  # Fedora
sudo yum install release/automationos-1.0.0-1.x86_64.rpm  # RHEL
```

### Docker Image

```bash
# Build Docker image
make docker-build

# Test in Docker
make docker-test

# Run shell in container
docker run -it automationos-build:latest /bin/bash
```

---

## CI/CD Integration

### GitHub Actions

The project includes comprehensive CI/CD workflows:

#### Build Workflow (`.github/workflows/build.yml`)

Automatically builds on:
- Push to `main` or `develop`
- Pull requests
- Manual trigger

Platforms tested:
- Ubuntu 22.04 / 24.04
- macOS latest
- WSL2 (Windows)
- Docker

#### Test Workflow (`.github/workflows/test.yml`)

Runs on every commit:
- Unit tests
- Integration tests
- Code coverage
- Static analysis

#### Release Workflow (`.github/workflows/release.yml`)

Triggered by version tags:
- Build release artifacts
- Generate checksums
- Create GitHub release
- Upload ISO and archives

### Local CI Testing

```bash
# Run full CI suite locally
make ci-build
make ci-test
make security-check
```

### Custom CI Integration

#### GitLab CI (`.gitlab-ci.yml`)

```yaml
build:
  image: automationos-build:latest
  script:
    - ./configure
    - make all
  artifacts:
    paths:
      - build/AutomationOS.iso
```

#### Jenkins (Jenkinsfile)

```groovy
pipeline {
    agent { dockerfile { filename 'Dockerfile.build' } }
    stages {
        stage('Build') {
            steps {
                sh './configure'
                sh 'make all'
            }
        }
        stage('Test') {
            steps {
                sh 'make test'
            }
        }
    }
}
```

---

## Advanced Topics

### Cross-Compilation

#### Custom Target

```bash
./configure --target=aarch64-automationos
make kernel TARGET=aarch64
```

#### Multi-Architecture Build

```bash
# Build for multiple architectures
for arch in x86_64 aarch64; do
    ./configure --target=${arch}-automationos
    make all
    mv build/AutomationOS.iso release/AutomationOS-${arch}.iso
    make clean
done
```

### Custom Bootloader

```bash
# Build with GRUB instead of AutoBoot
make bootloader BOOTLOADER=grub
```

### Minimal Build

```bash
# Minimal kernel with no drivers
./configure --with-drivers= --disable-security --disable-smp
make kernel
```

### Static Analysis

```bash
# Run all analyzers
make analyze-all

# Individual analyzers
make cppcheck       # Bug detection
make clang-tidy     # Linting
make sparse         # Kernel semantic checker
```

### Code Formatting

```bash
# Format all code
make format

# Check formatting only
make check-format

# Install pre-commit hooks
make install-hooks
```

### Custom Build Scripts

Create custom build configurations:

```bash
# custom-build.sh
#!/bin/bash
./configure \
    --enable-optimization=3 \
    --enable-lto \
    --with-drivers=nvme,ahci \
    --prefix=/custom/path

make -j$(nproc) all
make iso
```

---

## Troubleshooting

### Common Issues

#### Cross-Compiler Not Found

**Problem:**
```
ERROR: x86_64-elf-gcc not found
```

**Solution:**
```bash
# Try package installation first
sudo apt install gcc-x86-64-elf binutils-x86-64-elf

# Or build from source
./scripts/setup-toolchain.sh --prefix=/opt/cross
export PATH="/opt/cross/bin:$PATH"
```

#### QEMU Fails to Start

**Problem:**
```
ERROR: ISO not found: build/AutomationOS.iso
```

**Solution:**
```bash
# Build ISO first
make iso

# Then run QEMU
make qemu
```

#### Build Fails with "Permission Denied"

**Problem:**
```
make: build/kernel.elf: Permission denied
```

**Solution:**
```bash
# Clean and rebuild
make clean
make all

# Check file permissions
ls -l build/
chmod +x scripts/*.sh
```

#### Linker Errors

**Problem:**
```
undefined reference to `__stack_chk_fail'
```

**Solution:**
```bash
# Disable stack protector in kernel build
# Edit kernel/Makefile and add: -fno-stack-protector
```

#### ISO Creation Fails

**Problem:**
```
ERROR: xorriso not found
```

**Solution:**
```bash
# Install xorriso
sudo apt install xorriso      # Ubuntu/Debian
sudo pacman -S libisoburn     # Arch
brew install xorriso          # macOS
```

### Build Performance

#### Slow Builds

```bash
# Use parallel builds
make -j$(nproc)

# Use ccache (if available)
export CC="ccache x86_64-elf-gcc"
make all

# Disable LTO for faster builds
./configure --disable-lto
```

#### Out of Memory

```bash
# Reduce parallel jobs
make -j2

# Use swap space
sudo dd if=/dev/zero of=/swapfile bs=1G count=4
sudo mkswap /swapfile
sudo swapon /swapfile
```

### Debugging Build Issues

#### Verbose Build Output

```bash
# See full commands
make V=1

# Trace make decisions
make --debug=v

# Log everything
make V=1 2>&1 | tee build.log
```

#### Check Dependencies

```bash
# Verify all tools
./scripts/setup-toolchain.sh --check

# Test cross-compiler
x86_64-elf-gcc -v
x86_64-elf-ld -v
```

#### Clean Everything

```bash
# Complete clean
make distclean

# Remove all generated files
git clean -fdx  # WARNING: Removes all untracked files
```

### Getting Help

- **Documentation**: `docs/` directory
- **GitHub Issues**: https://github.com/your-org/automationos/issues
- **Build logs**: `build/build.log`
- **CI logs**: Check GitHub Actions tab

---

## Appendix

### Makefile Targets Reference

| Target | Description |
|--------|-------------|
| `all` | Build everything (default) |
| `bootloader` | Build bootloader only |
| `kernel` | Build kernel only |
| `userspace` | Build userspace programs |
| `iso` | Create bootable ISO |
| `clean` | Remove build artifacts |
| `qemu` | Run in QEMU |
| `qemu-debug` | Run in QEMU with GDB |
| `test` | Run tests |
| `unit-tests` | Run unit tests |
| `coverage` | Generate coverage report |
| `format` | Format code |
| `check-format` | Check code formatting |
| `lint` | Run linter |
| `analyze-all` | Run all static analyzers |
| `ci-build` | CI build |
| `ci-test` | CI tests |
| `docker-build` | Build Docker image |
| `help` | Show help |

### Environment Variables

| Variable | Description | Default |
|----------|-------------|---------|
| `CC` | C compiler | `x86_64-elf-gcc` |
| `LD` | Linker | `x86_64-elf-ld` |
| `AS` | Assembler | `nasm` |
| `BUILD_DIR` | Build output directory | `build` |
| `V` | Verbose build | `0` |

### File Structure

```
AutomationOS/
├── boot/                # Bootloader source
├── kernel/              # Kernel source
├── userspace/           # Userspace programs
├── scripts/             # Build scripts
├── tests/               # Test suite
├── docs/                # Documentation
├── build/               # Build output (generated)
├── release/             # Release packages (generated)
├── Makefile             # Root makefile
├── configure            # Configuration script
├── config.mk            # Build configuration (generated)
└── README.md            # Project readme
```

---

*For more information, see the full documentation in the `docs/` directory.*
