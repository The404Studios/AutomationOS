# AutomationOS Build System - Quick Reference

Fast reference for common build operations.

---

## 🚀 Quick Start (5 Minutes)

```bash
# 1. Check prerequisites
./scripts/setup-toolchain.sh --check

# 2. Configure
./configure

# 3. Build
make all

# 4. Test
make qemu
```

---

## 📋 Common Commands

### Build Commands

```bash
make all               # Build everything
make bootloader        # Build bootloader only
make kernel            # Build kernel only
make userspace         # Build userspace only
make iso               # Create bootable ISO
make clean             # Clean build artifacts
make help              # Show all targets
```

### Testing Commands

```bash
make qemu              # Run in QEMU
make qemu-debug        # Run with GDB debugging
make test              # Quick integration tests
make test-full         # Full test suite
make unit-tests        # Unit tests only
```

### Code Quality

```bash
make format            # Format all code
make check-format      # Verify formatting
make lint              # Run linter
make analyze-all       # All static analyzers
make coverage          # Generate coverage report
```

---

## 🔧 Configuration Options

### Basic Configuration

```bash
# Debug build
./configure --enable-debug

# Release build with optimizations
./configure --enable-release --enable-optimization=3

# Custom installation prefix
./configure --prefix=/opt/automationos
```

### Feature Selection

```bash
# Disable SMP
./configure --disable-smp

# Select specific drivers
./configure --with-drivers=nvme,ahci,e1000

# Enable LTO
./configure --enable-lto
```

### Development Options

```bash
# Enable AddressSanitizer
./configure --enable-sanitizer=address

# Maximum optimization
./configure --enable-optimization=fast --enable-lto
```

---

## 📦 Release & Packaging

### Create Release

```bash
# Build official release
./scripts/build-release.sh v1.0.0

# Output:
#   release/automationos-1.0.0.iso
#   release/automationos-1.0.0.tar.gz
#   release/automationos-1.0.0-src.tar.gz
#   release/SHA256SUMS
```

### Create Packages

```bash
# Debian package
./scripts/build-deb.sh 1.0.0
# Output: release/automationos_1.0.0_amd64.deb

# RPM package
./scripts/build-rpm.sh 1.0.0
# Output: release/automationos-1.0.0-1.x86_64.rpm
```

### Create Bootable USB

```bash
sudo ./scripts/create-bootable-usb.sh /dev/sdX
```

---

## 🐳 Docker Workflow

### Build Docker Image

```bash
docker build -f Dockerfile.build -t automationos-build .
```

### Build in Docker

```bash
# One-shot build
docker run --rm -v $PWD:/workspace automationos-build make all

# Interactive shell
docker run -it automationos-build /bin/bash
```

---

## 🧪 Testing Scenarios

### Quick Boot Test

```bash
make iso && make qemu
```

### Debug Kernel

```bash
# Terminal 1: Start QEMU with debugger
make qemu-debug

# Terminal 2: Attach GDB
gdb build/kernel.elf -ex 'target remote :1234'
```

### Full Test Suite

```bash
make clean
make test-full
```

### Code Coverage

```bash
make coverage
firefox docs/coverage/index.html
```

---

## 🔍 Troubleshooting

### Compiler Not Found

```bash
# Check toolchain
./scripts/setup-toolchain.sh --check

# Build from source
./scripts/setup-toolchain.sh --build
```

### Build Fails

```bash
# Clean and rebuild
make clean
./configure
make all

# Verbose output
make V=1
```

### ISO Creation Fails

```bash
# Install xorriso
sudo apt install xorriso      # Ubuntu
sudo pacman -S libisoburn     # Arch
brew install xorriso          # macOS
```

---

## 📊 Platform-Specific Notes

### Ubuntu/Debian

```bash
# Install dependencies
sudo apt install build-essential nasm python3 git \
    gcc-x86-64-elf binutils-x86-64-elf xorriso qemu-system-x86

# Build
./configure && make all
```

### Arch Linux

```bash
# Install dependencies
sudo pacman -S base-devel nasm python git \
    x86_64-elf-gcc x86_64-elf-binutils libisoburn qemu

# Build
./configure && make all
```

### macOS

```bash
# Install dependencies
brew install nasm python git xorriso qemu
brew tap messense/macos-cross-toolchains
brew install x86_64-elf-gcc x86_64-elf-binutils

# Build
./configure && make all
```

---

## 📈 CI/CD

### Local CI Testing

```bash
# Run CI pipeline locally
make ci-build
make ci-test
make security-check
```

### GitHub Actions

Automatically runs on:
- Push to main/develop
- Pull requests
- Manual trigger

Builds on:
- Ubuntu 22.04 & 24.04
- macOS latest
- WSL2 (Windows)
- Docker

---

## 🎯 Performance Tips

### Parallel Builds

```bash
# Use all CPU cores
make -j$(nproc)

# Specific core count
make -j8
```

### Incremental Builds

```bash
# Only rebuild changed files
make kernel   # Fast incremental
```

### Ccache (if available)

```bash
export CC="ccache x86_64-elf-gcc"
make all
```

---

## 📁 Output Files

### Build Artifacts

```
build/
├── kernel.elf           # Kernel binary
├── kernel.elf.debug     # Kernel with debug symbols
├── BOOTX64.EFI          # UEFI bootloader
├── AutomationOS.iso     # Bootable ISO
└── serial.log           # QEMU serial output
```

### Release Artifacts

```
release/
├── automationos-1.0.0.iso
├── automationos-1.0.0.tar.gz
├── automationos-1.0.0-src.tar.gz
├── automationos_1.0.0_amd64.deb
├── automationos-1.0.0-1.x86_64.rpm
└── SHA256SUMS
```

---

## 🔗 Quick Links

- **Full Documentation**: `docs/BUILD.md`
- **Deliverables**: `BUILD_SYSTEM_DELIVERABLES.md`
- **Main README**: `README.md`
- **Changelog**: `CHANGELOG.md`

---

## 💡 Common Workflows

### First-Time Build

```bash
git clone https://github.com/your-org/automationos
cd automationos
./scripts/setup-toolchain.sh
./configure
make all
make qemu
```

### Daily Development

```bash
# Edit code...
make kernel        # Quick rebuild
make qemu          # Test
```

### Pre-Commit

```bash
make format        # Format code
make lint          # Check style
make test          # Run tests
```

### Release Preparation

```bash
make clean
./configure --enable-release --enable-optimization=3
make all
make test-full
./scripts/build-release.sh v1.0.0
```

---

## 🆘 Get Help

```bash
# Build system help
make help
./configure --help
./scripts/setup-toolchain.sh --help

# Script help
./scripts/run-qemu.sh --help
./scripts/build-release.sh
```

---

*For detailed information, see `docs/BUILD.md`*
