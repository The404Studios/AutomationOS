# AutomationOS Performance Benchmark Suite

Comprehensive performance testing suite for verifying all claimed kernel optimizations.

## Overview

This benchmark suite measures real-world performance across all critical kernel subsystems:

- **Context switching** - Preemptive scheduling, PCID optimization
- **Syscall latency** - Fast path, entry/exit overhead
- **Futex performance** - Userspace atomic operations
- **File I/O** - Read-ahead, sendfile zero-copy
- **Network stack** - TCP/UDP loopback throughput
- **Process management** - Fork/exec, CoW page tables
- **Memory allocation** - Tcache, slab allocator
- **Epoll scalability** - O(1) event notification

## Quick Start

### Building Benchmarks

```bash
cd tests/bench
make
```

### Running Individual Benchmarks

```bash
# Context switch performance
./bench_context_switch

# Syscall latency
./bench_syscall

# Futex (fast userspace mutex)
./bench_futex

# File I/O throughput
./bench_file_io

# Network stack
./bench_network

# Process creation
./bench_fork

# Memory allocation
./bench_malloc

# Epoll scalability
./bench_epoll
```

### Running All Benchmarks

```bash
# From tests/bench directory
make test

# Or use the runner script
../../scripts/run_benchmarks.sh
```

## Performance Targets

### Context Switch
- **Without PCID**: 2000-3000 cycles
- **With PCID**: 800-1200 cycles (40-60% improvement)
- **Target**: <1500 cycles

### Syscall Latency
- **getpid (null syscall)**: <100 cycles
- **read/write**: <500 cycles
- **Throughput**: >10M syscalls/second

### Futex Performance
- **Uncontended lock**: <20 cycles (pure userspace atomic)
- **Lock/unlock pair**: ~10-20 cycles
- **Expected**: No kernel entry on fast path

### File I/O
- **Sequential read (4KB blocks)**:
  - Without read-ahead: ~50 MB/s
  - With read-ahead: >150 MB/s (3-4x improvement)
- **Sendfile zero-copy**: >200 MB/s

### Network Throughput
- **Loopback**: >80 MB/s
- **Socket creation**: <5000 cycles
- **Send/recv**: <3000 cycles per operation

### Process Creation
- **Fork time**: <10 ms
- **Fork+exec+wait**: <15 ms
- **Creation rate**: >50 processes/second
- **CoW optimization**: Large forks similar speed to small

### Memory Allocation
- **Small malloc (64B)**: <50 cycles (tcache hit)
- **Medium malloc**: <200 cycles
- **Free**: <50 cycles (tcache)
- **Throughput**: >10M allocations/second

### Epoll Scalability
- **epoll_create**: <1000 cycles
- **epoll_ctl**: <500 cycles
- **epoll_wait**: <500 cycles (O(1) regardless of fd count)
- **Throughput**: >1M epoll_wait/second

## Benchmark Details

### bench_context_switch

Measures the overhead of switching between two processes:

1. **Raw context switch** - Pure switching cost
2. **Scheduler overhead** - pick_next with varying queue sizes
3. **PCID comparison** - With/without PCID optimization

**Key metrics:**
- Cycles per context switch
- Switches per second
- Scheduler O(1) behavior verification

### bench_syscall

Measures syscall entry/exit overhead:

1. **Null syscall** - getpid (minimal work)
2. **Entry/exit only** - Isolate syscall instruction cost
3. **Throughput** - Max syscalls/second
4. **Method comparison** - SYSCALL vs INT 0x80

**Key metrics:**
- Cycles per syscall
- Syscalls per second
- Handler dispatch overhead

### bench_futex

Measures fast userspace mutex performance:

1. **Atomic operations** - Pure CAS overhead
2. **Uncontended lock** - Fast path (no syscall)
3. **Throughput** - Lock/unlock pairs per second

**Key metrics:**
- Cycles per lock/unlock
- Verification that uncontended locks avoid kernel

### bench_file_io

Measures file system performance:

1. **Sequential read** - 4KB chunks, 1MB total
2. **Block size comparison** - 512B to 64KB
3. **Sendfile** - Zero-copy file transfer

**Key metrics:**
- MB/s throughput
- Cycles per byte
- Read-ahead effectiveness

### bench_network

Measures network stack performance:

1. **Socket creation** - TCP and UDP
2. **Send/recv operations** - Buffer copy overhead
3. **Loopback throughput** - Full-stack performance

**Key metrics:**
- Cycles per socket operation
- MB/s throughput
- Packet processing rate

### bench_fork

Measures process lifecycle:

1. **Fork only** - Process creation overhead
2. **Fork+exit+wait** - Complete lifecycle
3. **Memory footprint** - CoW effectiveness
4. **Creation rate** - Processes/second

**Key metrics:**
- Time per fork
- CoW page table efficiency
- Process management overhead

### bench_malloc

Measures memory allocator performance:

1. **Small allocations** - Tcache performance
2. **Size comparison** - 16B to 16KB
3. **Allocation patterns** - LIFO, FIFO, random
4. **Realloc/calloc** - Special cases

**Key metrics:**
- Cycles per malloc/free
- Tcache hit rate
- Allocator scalability

### bench_epoll

Measures event notification scalability:

1. **epoll_create** - Instance creation
2. **epoll_ctl** - Add/modify/delete operations
3. **epoll_wait scalability** - O(1) verification with 10-1000 fds
4. **Throughput** - Events processed per second

**Key metrics:**
- O(1) behavior verification
- Cycles per operation
- Events per second

## Integration with Build System

### Adding to ISO

```bash
make install
# Copies all benchmarks to iso_root/tests/bench/
```

### Adding to Boot Tests

Add to `scripts/smoke_boot.sh`:

```bash
# Run performance benchmarks
echo "=== Performance Benchmarks ==="
/tests/bench/bench_syscall
/tests/bench/bench_context_switch
/tests/bench/bench_futex
```

## Interpreting Results

### PASS Indicators

Benchmarks print `[PASS]` when performance meets or exceeds targets:

```
[PASS] Context switch: 1200 cycles (<1500 target)
[PASS] Syscall latency: 85 cycles (<100 target)
[PASS] Futex uncontended: 12 cycles (<20 target)
```

### WARNING Indicators

`[WARN]` indicates performance below target but still functional:

```
[WARN] File read throughput: 120 MB/s (expected >150 MB/s)
```

### Performance Regression Detection

Compare results across kernel versions:

```bash
# Baseline
./bench_context_switch > baseline.txt

# After changes
./bench_context_switch > current.txt

# Compare
diff baseline.txt current.txt
```

## Common Issues

### Low Throughput

**Problem**: File I/O or network benchmarks show low throughput

**Solutions**:
- Check read-ahead is enabled
- Verify sendfile implementation
- Ensure no debug logging in hot paths

### High Syscall Latency

**Problem**: Syscalls > 200 cycles

**Solutions**:
- Verify SYSCALL/SYSRET MSRs configured correctly
- Check no unnecessary work in syscall entry/exit
- Ensure fast path optimizations enabled

### Context Switch Overhead

**Problem**: Context switches > 2000 cycles

**Solutions**:
- Verify PCID support enabled
- Check TLB invalidation strategy
- Ensure FPU state not saved unnecessarily

## Performance Profiling

### CPU Cycle Measurement

All benchmarks use RDTSC (Read Time-Stamp Counter) for cycle-accurate timing:

```c
uint64_t start = rdtsc_fence();  // Serializing fence
// ... operation ...
uint64_t end = rdtscp();         // Serializing read
uint64_t cycles = end - start;
```

### Statistical Analysis

Benchmarks collect min/avg/max across many iterations:

```
[PERF] Context switch (n=10000):
  Min: 850 cycles
  Avg: 1200 cycles
  Max: 2400 cycles
```

## Adding New Benchmarks

Template for new benchmark:

```c
#include <stdint.h>
#include <stdio.h>

static inline uint64_t rdtsc_fence(void) {
    uint32_t lo, hi;
    __asm__ volatile("lfence; rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static inline uint64_t rdtscp(void) {
    uint32_t lo, hi, aux;
    __asm__ volatile("rdtscp" : "=a"(lo), "=d"(hi), "=c"(aux));
    return ((uint64_t)hi << 32) | lo;
}

void bench_my_feature(void) {
    printf("\n[BENCH] My Feature\n");
    printf("==================\n");

    const int iterations = 1000;
    uint64_t total = 0;

    for (int i = 0; i < iterations; i++) {
        uint64_t start = rdtsc_fence();
        // ... operation to benchmark ...
        uint64_t end = rdtscp();
        total += (end - start);
    }

    uint64_t avg = total / iterations;
    printf("[BENCH] Average: %llu cycles\n", avg);

    if (avg < TARGET_CYCLES) {
        printf("[PASS] Performance meets target\n");
    }
}

int main(void) {
    bench_my_feature();
    return 0;
}
```

## References

- RDTSC timing: Intel® 64 and IA-32 Architectures Software Developer's Manual
- Syscall overhead: Linux kernel syscall entry assembly
- Futex design: Fuss, Futexes and Furwocks (Hubertus Franke et al.)
- Epoll scalability: The Implementation of epoll (Davide Libenzi)
