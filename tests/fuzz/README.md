# Fuzzing Infrastructure for AutomationOS

This directory contains comprehensive fuzzing infrastructure for discovering vulnerabilities in AutomationOS.

## Directory Structure

```
tests/fuzz/
├── syscall_fuzzer.c       # Coverage-guided syscall fuzzer
├── heap_fuzzer.c          # Memory allocator fuzzer
├── driver_fuzzer.c        # PS/2, serial, framebuffer driver fuzzer
├── harness.c              # User-space harness for fuzzing
├── fuzzer_common.h        # Common fuzzing utilities
├── Makefile               # Build system for fuzzers
├── corpus/                # Seed inputs for fuzzing
│   ├── syscall_seeds/     # Syscall test cases
│   ├── heap_seeds/        # Heap operations
│   └── driver_seeds/      # Driver inputs
├── crashes/               # Crash artifacts
└── output/                # Fuzzer output and coverage data
```

## Fuzzers Overview

### 1. Syscall Fuzzer (`syscall_fuzzer.c`)
- **Target**: All syscalls (SYS_EXIT, SYS_FORK, SYS_READ, SYS_WRITE, SYS_GETPID, etc.)
- **Method**: AFL++ coverage-guided fuzzing with random arguments
- **Detects**: 
  - Invalid argument handling
  - Integer overflows
  - Privilege escalation
  - NULL pointer dereferences
  - Buffer overflows

### 2. Heap Fuzzer (`heap_fuzzer.c`)
- **Target**: kmalloc/kfree allocator
- **Method**: Random allocation patterns with stress testing
- **Detects**:
  - Heap corruption
  - Double-free vulnerabilities
  - Use-after-free
  - Memory leaks
  - Fragmentation issues

### 3. Driver Fuzzer (`driver_fuzzer.c`)
- **Targets**: PS/2, Serial, Framebuffer drivers
- **Method**: Malformed input data and random ioctls
- **Detects**:
  - Input validation bugs
  - Race conditions
  - Buffer overflows in ring buffers
  - Interrupt handler crashes

## Prerequisites

### Install AFL++
```bash
# Ubuntu/Debian
sudo apt-get install afl++ gcc-multilib

# Arch Linux  
sudo pacman -S afl++ gcc-multilib

# From source
git clone https://github.com/AFLplusplus/AFLplusplus
cd AFLplusplus
make
sudo make install
```

### System Configuration
```bash
# Increase core pattern for crash detection
echo core | sudo tee /proc/sys/kernel/core_pattern

# Disable ASLR for deterministic fuzzing
echo 0 | sudo tee /proc/sys/kernel/randomize_va_space

# Allow ptrace for crash monitoring
echo 0 | sudo tee /proc/sys/kernel/yama/ptrace_scope
```

## Building Fuzzers

```bash
# Build all fuzzers
make -C tests/fuzz

# Build specific fuzzer
make -C tests/fuzz syscall_fuzzer
make -C tests/fuzz heap_fuzzer
make -C tests/fuzz driver_fuzzer
```

## Running Fuzzers

### Quick Start
```bash
# Run all fuzzers in parallel (recommended)
./scripts/run-fuzzer.sh --all --time 1h

# Run specific fuzzer
./scripts/run-fuzzer.sh --syscall --time 30m
./scripts/run-fuzzer.sh --heap --time 30m
./scripts/run-fuzzer.sh --driver --time 30m
```

### Manual Fuzzing

#### Syscall Fuzzer
```bash
# AFL++ mode (recommended)
afl-fuzz -i tests/fuzz/corpus/syscall_seeds \
         -o tests/fuzz/output/syscall \
         -m none \
         -- ./tests/fuzz/syscall_fuzzer @@

# Standalone mode (no AFL++)
./tests/fuzz/syscall_fuzzer --iterations 1000000
```

#### Heap Fuzzer
```bash
# AFL++ mode
afl-fuzz -i tests/fuzz/corpus/heap_seeds \
         -o tests/fuzz/output/heap \
         -m none \
         -- ./tests/fuzz/heap_fuzzer @@

# Standalone mode with stress test
./tests/fuzz/heap_fuzzer --stress --concurrent 100
```

#### Driver Fuzzer
```bash
# AFL++ mode
afl-fuzz -i tests/fuzz/corpus/driver_seeds \
         -o tests/fuzz/output/driver \
         -m none \
         -- ./tests/fuzz/driver_fuzzer @@

# Standalone mode
./tests/fuzz/driver_fuzzer --driver ps2 --iterations 100000
```

## Continuous Fuzzing

### 24/7 Fuzzing Setup
```bash
# Start continuous fuzzing (requires screen/tmux)
./scripts/run-fuzzer.sh --continuous

# Or use systemd service
sudo cp scripts/automationos-fuzzer.service /etc/systemd/system/
sudo systemctl enable automationos-fuzzer
sudo systemctl start automationos-fuzzer
```

### Monitoring
```bash
# Check fuzzer status
./scripts/fuzzer-status.sh

# View crashes
ls -lh tests/fuzz/crashes/

# Analyze coverage
./scripts/coverage-report.sh
```

## Crash Triage

See [docs/CRASH_TRIAGE.md](../../docs/CRASH_TRIAGE.md) for detailed crash analysis procedures.

Quick triage:
```bash
# Reproduce crash
./tests/fuzz/syscall_fuzzer --input tests/fuzz/crashes/id:000042,sig:11

# Analyze with GDB
gdb ./tests/fuzz/syscall_fuzzer
(gdb) run --input tests/fuzz/crashes/id:000042,sig:11
(gdb) bt
(gdb) info registers

# Generate crash report
./scripts/triage-crash.sh tests/fuzz/crashes/id:000042,sig:11
```

## Coverage Analysis

```bash
# Generate coverage report
make -C tests/fuzz coverage

# View HTML report
firefox tests/fuzz/output/coverage/index.html

# Check coverage percentage
./scripts/coverage-report.sh --summary
```

## CI Integration

Fuzzers run automatically on every PR via GitHub Actions:
- 1 hour fuzzing session per PR
- Automatic crash detection and bug filing
- Coverage regression detection

See `.github/workflows/fuzzing.yml` for configuration.

## Performance Tuning

### AFL++ Settings
```bash
# Use QEMU mode for binary-only fuzzing
AFL_QEMU_PERSISTENT=1 afl-fuzz ...

# Enable cmplog for better mutation
afl-fuzz -c 0 ...

# Use multiple cores
parallel afl-fuzz -S fuzzer{} ::: {1..8}
```

### Hardware Requirements
- **Minimum**: 4 cores, 8GB RAM, 50GB disk
- **Recommended**: 16+ cores, 32GB+ RAM, 500GB SSD
- **Optimal**: Dedicated fuzzing server with 64+ cores

## Bugs Found

Track discovered vulnerabilities in [BUGS_FOUND.md](./BUGS_FOUND.md).

## Contributing

When adding new fuzzers:
1. Follow the template in `fuzzer_common.h`
2. Add corpus seeds to `corpus/`
3. Update this README
4. Add CI integration in `.github/workflows/fuzzing.yml`

## References

- [AFL++ Documentation](https://github.com/AFLplusplus/AFLplusplus/blob/stable/docs/README.md)
- [Fuzzing Project](https://fuzzing-project.org/)
- [OSS-Fuzz](https://google.github.io/oss-fuzz/)
- [Kernel Fuzzing with Syzkaller](https://github.com/google/syzkaller)
