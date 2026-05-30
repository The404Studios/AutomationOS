# AutomationOS Fuzzing Guide

This guide explains how to run and manage the fuzzing infrastructure for AutomationOS.

## Table of Contents

1. [Quick Start](#quick-start)
2. [Fuzzer Overview](#fuzzer-overview)
3. [Installation](#installation)
4. [Running Fuzzers](#running-fuzzers)
5. [Interpreting Results](#interpreting-results)
6. [Coverage Analysis](#coverage-analysis)
7. [Best Practices](#best-practices)
8. [Troubleshooting](#troubleshooting)

## Quick Start

```bash
# 1. Install dependencies
make -C tests/fuzz install-deps

# 2. Build fuzzers
make -C tests/fuzz

# 3. Run quick smoke test
make -C tests/fuzz test

# 4. Start fuzzing (1 hour campaign)
./scripts/run-fuzzer.sh --all --time 1h

# 5. Check for crashes
ls -lh tests/fuzz/crashes/
```

## Fuzzer Overview

### 1. Syscall Fuzzer (`syscall_fuzzer`)

**Purpose**: Discover vulnerabilities in system call handling

**Targets**:
- `SYS_READ` - File descriptor reads
- `SYS_WRITE` - File descriptor writes
- `SYS_GETPID` - Process ID retrieval
- Additional syscalls as implemented

**Vulnerabilities Detected**:
- Invalid argument handling
- Integer overflows in size parameters
- NULL pointer dereferences
- Buffer overflows
- Privilege escalation bugs
- TOCTOU (Time-of-Check-Time-of-Use) races

**Test Strategy**:
- Random argument generation with edge cases
- Boundary value testing (0, -1, MAX_INT, etc.)
- Invalid pointer testing (NULL, low addresses, high addresses)
- Mixed valid/invalid argument combinations

### 2. Heap Fuzzer (`heap_fuzzer`)

**Purpose**: Stress-test the kernel heap allocator

**Targets**:
- `kmalloc()` - Memory allocation
- `kfree()` - Memory deallocation
- `krealloc()` - Memory reallocation (if implemented)

**Vulnerabilities Detected**:
- Heap corruption
- Double-free vulnerabilities
- Use-after-free (UAF) bugs
- Memory leaks
- Fragmentation issues
- Race conditions in concurrent allocations

**Test Strategy**:
- Random allocation/deallocation patterns
- Stress testing with thousands of concurrent allocations
- Fragmentation patterns (alternating alloc/free)
- Edge case sizes (0, 1, MAX_SIZE)
- Reallocation chains

### 3. Driver Fuzzer (`driver_fuzzer`)

**Purpose**: Test device driver robustness

**Targets**:
- PS/2 keyboard driver (`ps2.c`)
- Serial port driver (`serial.c`)
- Framebuffer driver (`framebuffer.c`)

**Vulnerabilities Detected**:
- Input validation bugs
- Buffer overflows in circular buffers
- Race conditions in interrupt handlers
- TOCTOU bugs in driver logic
- Invalid ioctl handling

**Test Strategy**:

**PS/2 Driver**:
- Random scancode injection
- Buffer overflow tests (rapid key presses)
- Modifier key combinations
- Invalid scancodes

**Serial Driver**:
- Control character injection
- FIFO overflow tests
- Baud rate edge cases
- Binary data patterns

**Framebuffer Driver**:
- Out-of-bounds pixel writes
- Mode switching stress tests
- Concurrent frame buffer access
- Invalid resolution/BPP combinations

## Installation

### Ubuntu/Debian

```bash
sudo apt-get update
sudo apt-get install -y afl++ gcc-multilib python3 lcov
```

### Arch Linux

```bash
sudo pacman -S afl++ gcc-multilib python lcov
```

### From Source (AFL++)

```bash
git clone https://github.com/AFLplusplus/AFLplusplus
cd AFLplusplus
make
sudo make install
```

### System Configuration for AFL++

```bash
# Optimize for fuzzing
echo core | sudo tee /proc/sys/kernel/core_pattern
echo 0 | sudo tee /proc/sys/kernel/randomize_va_space
echo 0 | sudo tee /proc/sys/kernel/yama/ptrace_scope

# Increase file descriptor limits
ulimit -n 8192
```

## Running Fuzzers

### Standalone Mode (No AFL++)

Standalone mode runs fuzzers without AFL++. Useful for quick testing or when AFL++ is not available.

```bash
# Syscall fuzzer - 1 million iterations
./tests/fuzz/syscall_fuzzer --iterations 1000000

# Heap fuzzer - stress test with 10 concurrent threads
./tests/fuzz/heap_fuzzer --stress --concurrent 10 --iterations 100000

# Driver fuzzer - PS/2 keyboard
./tests/fuzz/driver_fuzzer --driver ps2 --iterations 100000

# Driver fuzzer - Serial port
./tests/fuzz/driver_fuzzer --driver serial --iterations 100000

# Driver fuzzer - Framebuffer
./tests/fuzz/driver_fuzzer --driver fb --iterations 100000
```

### AFL++ Mode (Recommended)

AFL++ provides coverage-guided fuzzing with intelligent mutation strategies.

```bash
# Single fuzzer
afl-fuzz -i tests/fuzz/corpus/syscall_seeds \
         -o tests/fuzz/output/syscall \
         -m none \
         -- ./tests/fuzz/syscall_fuzzer @@

# Heap fuzzer
afl-fuzz -i tests/fuzz/corpus/heap_seeds \
         -o tests/fuzz/output/heap \
         -m none \
         -- ./tests/fuzz/heap_fuzzer @@

# Driver fuzzer
afl-fuzz -i tests/fuzz/corpus/driver_seeds \
         -o tests/fuzz/output/driver \
         -m none \
         -- ./tests/fuzz/driver_fuzzer @@
```

### Orchestrated Campaigns

Use the orchestration script for coordinated fuzzing:

```bash
# Fuzz everything for 1 hour
./scripts/run-fuzzer.sh --all --time 1h

# Fuzz syscalls only for 30 minutes
./scripts/run-fuzzer.sh --syscall --time 30m

# Continuous fuzzing (24/7)
./scripts/run-fuzzer.sh --continuous

# Multiple fuzzers in parallel
./scripts/run-fuzzer.sh --syscall --heap --driver --time 2h
```

### Parallel Fuzzing (Multi-Core)

AFL++ supports parallel fuzzing with multiple instances:

```bash
# Master instance
afl-fuzz -i corpus/syscall_seeds -o output/syscall \
         -M fuzzer01 -m none -- ./syscall_fuzzer @@

# Secondary instances (run in separate terminals)
afl-fuzz -i corpus/syscall_seeds -o output/syscall \
         -S fuzzer02 -m none -- ./syscall_fuzzer @@

afl-fuzz -i corpus/syscall_seeds -o output/syscall \
         -S fuzzer03 -m none -- ./syscall_fuzzer @@

# Or use GNU parallel
seq 1 8 | parallel -j 8 \
    afl-fuzz -i corpus/syscall_seeds -o output/syscall \
    -S fuzzer{} -m none -- ./syscall_fuzzer @@
```

## Interpreting Results

### AFL++ Status Screen

```
american fuzzy lop ++4.00a (syscall_fuzzer)

┌─ process timing ────────────────────────────────────┐
│        run time : 0 days, 1 hrs, 23 min, 45 sec     │
│   last new path : 0 days, 0 hrs, 15 min, 32 sec     │
│ last uniq crash : none seen yet                     │
│  last uniq hang : none seen yet                     │
└─────────────────────────────────────────────────────┘

┌─ overall results ───────────────────────────────────┐
│   cycles done : 342                                  │
│  total paths : 1,245                                 │
│ uniq crashes : 0                                     │
│   uniq hangs : 0                                     │
└─────────────────────────────────────────────────────┘

┌─ cycle progress ────────────────────────────────────┐
│  now processing : 832 (66.8%)                        │
│ paths timed out : 0 (0.00%)                          │
└─────────────────────────────────────────────────────┘

┌─ map coverage ──────────────────────────────────────┐
│    map density : 2.34% / 5.12%                       │
│ count coverage : 1.87 bits/tuple                     │
└─────────────────────────────────────────────────────┘

┌─ stage progress ────────────────────────────────────┐
│  now trying : havoc                                  │
│ stage execs : 45678/100000 (45.68%)                 │
│ total execs : 4.56M                                  │
│  exec speed : 1023/sec                               │
└─────────────────────────────────────────────────────┘
```

**Key Metrics**:
- **cycles done**: Number of complete queue iterations (more is better)
- **total paths**: Unique code paths discovered (coverage indicator)
- **uniq crashes**: Distinct crashes found (investigate immediately!)
- **uniq hangs**: Distinct hangs found (timeout bugs)
- **map density**: Code coverage percentage
- **exec speed**: Executions per second (higher is better)

### Standalone Mode Output

```
[FUZZER] Starting standalone syscall fuzzing (1000000 iterations)...
Progress: 342156/1000000 (34.22%)

==================== FUZZING STATISTICS ====================
Total Iterations:   1000000
Total Executions:   1000000
Crashes Found:      0
Hangs Found:        0
Unique Crashes:     0
Elapsed Time:       987 seconds
Exec/sec:           1013.17
Coverage:           0.00%
===========================================================
```

## Coverage Analysis

### Generate Coverage Report

```bash
# Build with coverage instrumentation
make -C tests/fuzz coverage

# Open HTML report
firefox tests/fuzz/output/coverage/index.html
```

### Interpreting Coverage

**Target Coverage Goals**:
- **Syscall handlers**: > 90% line coverage
- **Heap allocator**: > 85% line coverage
- **Drivers**: > 70% line coverage

**Low coverage indicators**:
- Unreachable error handling paths
- Dead code
- Missing test cases for edge conditions

## Best Practices

### 1. Start with Short Runs

```bash
# 5-minute test run before long campaigns
./scripts/run-fuzzer.sh --all --time 5m
```

### 2. Monitor System Resources

```bash
# Watch CPU and memory usage
htop

# Monitor fuzzer output directory size
du -sh tests/fuzz/output/

# Check disk space
df -h
```

### 3. Prioritize High-Risk Components

Focus fuzzing time on:
1. Syscall handlers (most common attack surface)
2. Memory allocator (critical for stability)
3. Input drivers (PS/2, serial)

### 4. Use Diverse Seed Corpus

```bash
# Add your own seeds
echo "custom_input" > tests/fuzz/corpus/syscall_seeds/seed_custom_001

# Minimize corpus (remove redundant seeds)
afl-cmin -i corpus/syscall_seeds -o corpus/syscall_seeds_min \
         -- ./syscall_fuzzer @@
```

### 5. Run Continuous Fuzzing

```bash
# Use screen or tmux for background fuzzing
screen -S fuzzing
./scripts/run-fuzzer.sh --continuous
# Press Ctrl+A, D to detach

# Reattach later
screen -r fuzzing
```

### 6. Regular Crash Triage

```bash
# Check for crashes daily
./scripts/fuzzer-status.sh

# Triage new crashes
./scripts/triage-crash.sh tests/fuzz/crashes/id:000042,sig:11
```

## Troubleshooting

### AFL++ Not Starting

**Problem**: `[-] Hmm, your system is configured to send core dump notifications to an external utility...`

**Solution**:
```bash
echo core | sudo tee /proc/sys/kernel/core_pattern
```

### Low Execution Speed

**Problem**: `exec speed: 10/sec` (very slow)

**Solutions**:
- Disable ASAN for faster execution (remove `-fsanitize=address`)
- Use persistent mode (requires code modification)
- Reduce timeout: `afl-fuzz -t 1000+ ...`

### Out of Memory

**Problem**: Fuzzer crashes with OOM

**Solutions**:
```bash
# Limit memory per fuzzer
afl-fuzz -m 512 ...  # 512MB limit

# Reduce concurrent allocations in heap fuzzer
./heap_fuzzer --concurrent 2  # Instead of 10
```

### No New Paths Found

**Problem**: AFL++ stops discovering new paths early

**Solutions**:
- Improve seed corpus quality
- Enable cmplog: `afl-fuzz -c 0 ...`
- Use deterministic mode: `afl-fuzz -D ...`
- Increase mutation strategies

### Crashes Not Reproducible

**Problem**: Crash only occurs during fuzzing, not during replay

**Possible causes**:
- Race condition (timing-dependent)
- Uninitialized memory (varies between runs)
- Resource exhaustion (needs long-running fuzzer)

**Solutions**:
```bash
# Run under valgrind
valgrind ./syscall_fuzzer --input crash_file

# Enable ASAN
ASAN_OPTIONS=detect_leaks=1 ./syscall_fuzzer --input crash_file
```

## Advanced Topics

### Custom Mutators

Create `custom_mutator.c`:
```c
#include "afl-fuzz.h"

size_t afl_custom_fuzz(uint8_t *buf, size_t buf_size,
                       uint8_t **out_buf, uint8_t *add_buf,
                       size_t add_buf_size, size_t max_size) {
    // Custom mutation logic
    return mutated_size;
}
```

Build:
```bash
gcc -shared -fPIC custom_mutator.c -o custom_mutator.so
AFL_CUSTOM_MUTATOR_LIBRARY=./custom_mutator.so afl-fuzz ...
```

### Distributed Fuzzing

Use `afl-sync` to synchronize multiple fuzzing machines:

```bash
# Machine 1
afl-fuzz -i corpus -o sync_dir -M machine1 -- ./fuzzer @@

# Machine 2
rsync -avz machine1:sync_dir/ sync_dir/
afl-fuzz -i corpus -o sync_dir -S machine2 -- ./fuzzer @@
```

### Integration with CI/CD

See `.github/workflows/fuzzing.yml` for GitHub Actions integration.

## References

- [AFL++ Documentation](https://github.com/AFLplusplus/AFLplusplus/blob/stable/docs/)
- [The Fuzzing Book](https://www.fuzzingbook.org/)
- [OSS-Fuzz](https://google.github.io/oss-fuzz/)
- [Fuzzing Project](https://fuzzing-project.org/)
- [AutomationOS Crash Triage Guide](CRASH_TRIAGE.md)
