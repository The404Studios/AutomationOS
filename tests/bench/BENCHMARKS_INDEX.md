# AutomationOS Benchmark Suite - Complete Index

Quick reference guide to all benchmarks, their purpose, and how to use them.

## Benchmark Overview

| # | Benchmark | File | LOC | Purpose | Key Metric | Target |
|---|-----------|------|-----|---------|------------|--------|
| 1 | Context Switch | bench_context_switch.c | 175 | PCID optimization | cycles | <1500 |
| 2 | Syscall Latency | bench_syscall.c | 236 | Fast syscall path | cycles | <100 |
| 3 | Futex | bench_futex.c | 270 | Userspace mutex | cycles | <20 |
| 4 | File I/O | bench_file_io.c | 380 | Read-ahead | MB/s | >150 |
| 5 | Network | bench_network.c | 420 | Network stack | MB/s | >80 |
| 6 | Fork | bench_fork.c | 510 | Process creation | ms | <10 |
| 7 | Malloc | bench_malloc.c | 450 | Memory allocator | cycles | <100 |
| 8 | Epoll | bench_epoll.c | 480 | Event notification | cycles | <500 |
| 9 | Memory | bench_memory.c | - | Memory subsystem | - | - |
| 10 | Huge Pages | bench_huge_pages.c | - | Large pages | - | - |
| 11 | Read-ahead/Sendfile | bench_readahead_sendfile.c | - | Zero-copy I/O | - | - |

**Total**: 11 benchmarks, ~3,000 lines of code

## Quick Commands

```bash
# Build all
make

# Build specific
make bench_futex

# Run all
make test

# Run specific
./bench_syscall

# Clean
make clean

# Install to ISO
make install
```

## Benchmark Details

### 1. bench_context_switch

**What it tests**: Process context switching performance

**Subtests**:
- `bench_context_switch()` - Raw context switch overhead
- `bench_scheduler_overhead()` - Scheduler with varying queue sizes
- `bench_context_switch_workloads()` - Different workload conditions

**Expected output**:
```
[BENCH] Context Switch Benchmark
=================================
[BENCH] Running 20000 context switches...
[PERF] Context Switch (n=20000):
  Min: 850 cycles
  Avg: 1200 cycles
  Max: 2400 cycles

Performance:
  Switches/second: 2500000
  Time per switch: 0.40 us
```

**Usage**:
```bash
./bench_context_switch
```

**Validates**: PCID TLB optimization

---

### 2. bench_syscall

**What it tests**: Syscall entry/exit overhead

**Subtests**:
- `bench_syscall_null()` - getpid (minimal work)
- `bench_syscall_entry_exit()` - Isolate syscall instruction
- `bench_syscall_throughput()` - Maximum calls/second
- `bench_syscall_methods()` - SYSCALL vs INT comparison

**Expected output**:
```
[BENCH] Null Syscall Benchmark (getpid)
========================================
[PERF] Syscall (getpid) (n=100000):
  Min: 72 cycles
  Avg: 85 cycles
  Max: 124 cycles
```

**Usage**:
```bash
./bench_syscall
```

**Validates**: Fast syscall path optimization

---

### 3. bench_futex

**What it tests**: Fast userspace mutex performance

**Subtests**:
- `bench_futex_atomic_only()` - Pure atomic CAS
- `bench_futex_uncontended()` - Lock/unlock without contention
- `bench_futex_throughput()` - Operations per second

**Expected output**:
```
[BENCH] Futex Uncontended Lock/Unlock
=======================================
[PERF] Futex Uncontended (n=10000):
  Min: 10 cycles
  Avg: 15 cycles
  Max: 28 cycles

[PASS] Uncontended locks are fast (<20 cycles)
```

**Usage**:
```bash
./bench_futex
```

**Validates**: Futex fast path stays in userspace

---

### 4. bench_file_io

**What it tests**: File system I/O performance

**Subtests**:
- `bench_sequential_read()` - Sequential 4KB reads
- `bench_block_sizes()` - Compare 512B - 64KB blocks
- `bench_sendfile()` - Zero-copy transfer

**Expected output**:
```
[BENCH] Sequential File Read (4KB chunks)
==========================================
[BENCH] Reading 1048576 bytes in 256 chunks of 4096 bytes...
[BENCH] Total cycles: 9000000
[BENCH] Time: 5 ms
[BENCH] Throughput: 180 MB/s
[PASS] Read-ahead is working well (>150 MB/s)
```

**Usage**:
```bash
./bench_file_io
```

**Validates**: Read-ahead optimization

---

### 5. bench_network

**What it tests**: Network stack performance

**Subtests**:
- `bench_socket_creation()` - TCP/UDP socket overhead
- `bench_buffer_operations()` - Send/recv performance
- `bench_throughput_estimate()` - Network throughput
- `bench_tcp_loopback()` - Full stack test

**Expected output**:
```
[BENCH] TCP socket creation: 3500 cycles (avg)
[BENCH] UDP socket creation: 2800 cycles (avg)
[BENCH] Send operation (1024 bytes): 2400 cycles (avg)
[BENCH] Estimated throughput: 120 MB/s
```

**Usage**:
```bash
./bench_network
```

**Validates**: Network stack efficiency

---

### 6. bench_fork

**What it tests**: Process creation performance

**Subtests**:
- `bench_fork_only()` - Fork overhead
- `bench_fork_exit_wait()` - Complete lifecycle
- `bench_fork_memory_sizes()` - CoW effectiveness
- `bench_process_creation_rate()` - Processes/second
- `bench_spawn()` - Spawn syscall (if available)

**Expected output**:
```
[BENCH] Fork Overhead
=====================
[BENCH] Creating 100 child processes...
[BENCH] Average fork time:
  Cycles: 24000000
  Time:   8000 us (~8 ms)

[PASS] Fork performance good (<10ms)

[BENCH] Fork with Different Memory Footprints
==============================================
  Memory size     4096 bytes:   2500 us (7500000 cycles)
  Memory size    65536 bytes:   2650 us (7950000 cycles)
  Memory size  1048576 bytes:   3000 us (9000000 cycles)
  Memory size 16777216 bytes:   3200 us (9600000 cycles)
[INFO] CoW (Copy-on-Write) should make large forks fast
```

**Usage**:
```bash
./bench_fork
```

**Validates**: Copy-on-Write page table optimization

---

### 7. bench_malloc

**What it tests**: Memory allocator performance

**Subtests**:
- `bench_small_allocations()` - 64-byte allocs (tcache)
- `bench_allocation_sizes()` - 16B - 16KB comparison
- `bench_allocation_patterns()` - LIFO/interleaved/random
- `bench_realloc()` - Growing/shrinking realloc
- `bench_calloc()` - Calloc vs malloc+memset

**Expected output**:
```
[BENCH] Small Allocations (64 bytes)
=====================================
[PERF] malloc(64) (n=10000):
  Min: 32 cycles
  Avg: 42 cycles
  Max: 78 cycles

[PERF] free(64) (n=1000):
  Min: 28 cycles
  Avg: 38 cycles
  Max: 65 cycles

[PASS] malloc tcache fast (<50 cycles)
```

**Usage**:
```bash
./bench_malloc
```

**Validates**: Tcache memory allocator

---

### 8. bench_epoll

**What it tests**: Event notification scalability

**Subtests**:
- `bench_epoll_create()` - Instance creation
- `bench_epoll_ctl()` - Add/modify/delete operations
- `bench_epoll_wait_scalability()` - O(1) verification
- `bench_epoll_wait_with_events()` - With ready events
- `bench_epoll_throughput()` - Events/second

**Expected output**:
```
[BENCH] Epoll Wait Scalability (O(1) test)
===========================================
[PERF] epoll_wait (10 fds) (n=1000):
  Min: 320 cycles
  Avg: 380 cycles
  Max: 520 cycles

[PERF] epoll_wait (100 fds) (n=1000):
  Min: 340 cycles
  Avg: 395 cycles
  Max: 540 cycles

[PERF] epoll_wait (1000 fds) (n=1000):
  Min: 350 cycles
  Avg: 410 cycles
  Max: 560 cycles

[INFO] epoll_wait should show O(1) behavior:
[INFO] Performance should be similar regardless of fd count
```

**Usage**:
```bash
./bench_epoll
```

**Validates**: Epoll O(1) scalability

---

## Performance Targets Summary

| Benchmark | Target | Expected Range | Units |
|-----------|--------|----------------|-------|
| Context Switch | <1500 | 800-1500 | cycles |
| Syscall | <100 | 60-100 | cycles |
| Futex | <20 | 10-20 | cycles |
| File Read | >150 | 150-250 | MB/s |
| Network | >80 | 80-200 | MB/s |
| Fork | <10 | 5-10 | ms |
| Malloc (64B) | <100 | 30-100 | cycles |
| Epoll Wait | <500 | 200-500 | cycles |

## Common Options

Most benchmarks support these patterns:

**Running**: Just execute the binary
```bash
./bench_name
```

**Capturing output**:
```bash
./bench_name > results.txt 2>&1
```

**Extracting metrics**:
```bash
./bench_name | grep "Avg:"
./bench_name | grep "\[PASS\]"
./bench_name | grep "cycles"
```

## Interpreting Results

### Status Indicators

- **[PASS]** - Performance meets or exceeds target
- **[INFO]** - Informational message
- **[WARN]** - Performance below target but functional
- **[ERROR]** - Benchmark failed to execute

### Cycle Measurements

All cycle measurements use RDTSC (Read Time-Stamp Counter):
- **Min**: Best-case (cache hot, no interrupts)
- **Avg**: Typical performance
- **Max**: Worst-case (cache cold, interrupts, etc.)

**Note**: Assumes 3 GHz CPU. Actual frequency may vary.

### Throughput Measurements

- **MB/s**: Megabytes per second
- **Ops/s**: Operations per second
- **M/s**: Millions per second

## Troubleshooting

### Benchmark won't run

**Check**:
1. Is the binary executable? `chmod +x bench_name`
2. Are dependencies available? (kernel features, syscalls)
3. Run with verbose output: `./bench_name 2>&1`

### Results seem wrong

**Check**:
1. CPU frequency matches assumption (3 GHz)
2. No background processes consuming CPU
3. Not running in slow emulator/VM
4. Sufficient iterations for averaging

### Feature not available

Some benchmarks require specific kernel features:
- `bench_futex` → SYS_FUTEX syscall
- `bench_epoll` → SYS_EPOLL_* syscalls
- `bench_file_io` → File system initialized
- `bench_network` → Network stack initialized

## Adding New Benchmarks

Template structure:

```c
#include <stdint.h>
#include <stdio.h>

// RDTSC helpers
static inline uint64_t rdtsc_fence(void) { /* ... */ }
static inline uint64_t rdtscp(void) { /* ... */ }

// Statistics helpers
typedef struct { /* ... */ } perf_stat_t;
void perf_stat_init(perf_stat_t* stat, const char* name);
void perf_stat_record(perf_stat_t* stat, uint64_t cycles);
void perf_stat_report(perf_stat_t* stat);

// Benchmark function
void bench_my_feature(void) {
    printf("\n[BENCH] My Feature\n");
    // ... implementation ...
}

int main(void) {
    bench_my_feature();
    return 0;
}
```

Add to Makefile:
```makefile
bench_my_feature: bench_my_feature.c
    $(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
```

## Documentation Index

- **README.md** - User guide and quick start
- **BENCHMARKS_INDEX.md** (this file) - Complete reference
- **../BENCHMARK_SUITE_DELIVERABLE.md** - Implementation report
- **../BENCHMARK_INTEGRATION_CHECKLIST.md** - Integration guide
- **../BENCHMARK_SUITE_SUMMARY.md** - Executive summary

## Support

For questions or issues:
1. Check individual benchmark source code
2. Review documentation files
3. Examine similar benchmarks for patterns
4. Check kernel feature implementation status

---

**Last Updated**: 2026-05-29  
**Version**: 1.0  
**Status**: Production Ready
