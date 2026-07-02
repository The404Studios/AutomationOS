# AutomationOS Performance Benchmark Suite

Comprehensive performance validation suite for AutomationOS kernel optimizations.

## Overview

This benchmark suite validates the following optimization claims:

- **PCID Support**: 40-60% context switch speedup
- **Per-CPU Page Caches**: 10x allocation speedup
- **Optimized Syscall Path**: 40-56% syscall speedup
- **Overall System**: 30-50% improvement

## Quick Start

```bash
# Build all benchmarks
make all

# Run all benchmarks
./run_all.sh

# Run specific category
make run-micro    # Micro-benchmarks only
make run-macro    # Macro-benchmarks only

# Run individual benchmark
./micro/context_switch_bench
```

## Benchmark Categories

### Micro-Benchmarks (`micro/`)

**Context Switch Benchmark** (`context_switch_bench`)
- Measures thread and process context switch latency
- Tests with varying TLB pressure
- Validates PCID optimization (40-60% improvement target)

**Allocation Benchmark** (`allocation_bench`)
- Measures page allocation latency
- Compares global allocator vs per-CPU cache
- Validates per-CPU cache optimization (10x improvement target)

**Syscall Benchmark** (`syscall_bench`)
- Measures syscall overhead for various syscalls
- Tests fast path optimizations
- Validates syscall path optimization (40-56% improvement target)

### Macro-Benchmarks (`macro/`)

**Boot Time Benchmark** (`boot_time_bench`)
- Simulates kernel boot sequence
- Measures each boot stage
- Target: < 5 seconds total boot time

**Process Benchmark** (`process_bench`)
- Measures fork, exec, context switching
- Tests zombie reaping
- Process creation throughput

### Workload Benchmarks (`workloads/`)

**Compile Benchmark** (`compile_bench`)
- Simulates kernel compilation workload
- Heavy syscall usage (open/read/write/stat)
- Mix of I/O and CPU work

### Stress Tests (`stress/`)

**Memory Stress** (`memory_stress`)
- Allocates until OOM
- Rapid allocation/deallocation cycles
- Memory fragmentation test
- Multi-threaded allocation stress

### Regression Tests (`regression/`)

**Baseline** (`baseline`)
- Quick performance baseline
- Tracks metrics over time
- Detects regressions > 5%
- Generates JSON reports for CI

## Build System

### Makefile Targets

```bash
make all          # Build all benchmarks
make clean        # Clean build artifacts
make micro        # Build micro-benchmarks
make macro        # Build macro-benchmarks
make workloads    # Build workload benchmarks
make stress       # Build stress tests
make regression   # Build regression tests

make run          # Build and run all
make run-micro    # Run micro-benchmarks
make run-macro    # Run macro-benchmarks
```

### Build Requirements

- GCC 7.0+ or Clang 10.0+
- Linux kernel 5.4+
- libc with pthread support
- libm (math library)

**Optional:**
- `perf` for profiling
- `valgrind` for cache analysis
- `jq` for JSON parsing
- `gnuplot` for visualization

## Running Benchmarks

### Basic Usage

```bash
# Run all benchmarks and generate report
./run_all.sh

# Run specific benchmark
./micro/context_switch_bench

# Update regression baseline
./regression/baseline --update
```

### Best Practices

1. **Set CPU governor to performance:**
   ```bash
   sudo cpupower frequency-set -g performance
   ```

2. **Disable turbo boost (for consistency):**
   ```bash
   echo 1 | sudo tee /sys/devices/system/cpu/intel_pstate/no_turbo
   ```

3. **Pin to specific CPU core:**
   ```bash
   taskset -c 0 ./micro/context_switch_bench
   ```

4. **Run multiple times and take median:**
   ```bash
   for i in {1..5}; do
       ./micro/syscall_bench >> results.txt
   done
   ```

5. **Close background applications:**
   - Close web browsers, IDEs, etc.
   - Stop unnecessary services
   - Check with `htop` or `ps aux`

## Understanding Results

### Micro-Benchmark Output

```
=== Context Switch Benchmark (Threads) ===
Measuring 10000 context switches...

--- Results ---
Samples:    10000
Min:        324.50 ns
Max:        2103.25 ns
Mean:       456.78 ns
Std Dev:    87.32 ns
Median:     445.12 ns
P95:        598.45 ns
P99:        723.89 ns
```

**Key metrics:**
- **Median**: Most representative (ignores outliers)
- **P95/P99**: Tail latency (worst-case performance)
- **Std Dev**: Consistency (lower is better)

### Regression Detection

The regression baseline tracks key metrics and alerts if any regress by > 5%:

```bash
# Establish baseline (first run)
./regression/baseline --update

# Check for regressions (subsequent runs)
./regression/baseline

# Output:
context_switch_cycles
  Baseline:   1234 cycles (456.78 ns)
  Current:    1189 cycles (440.12 ns)
  Change:     -3.65%
  Status:     OK
```

**Status levels:**
- `OK`: Within ±5% of baseline
- `IMPROVEMENT`: > 5% faster
- `REGRESSION`: > 5% slower

## Profiling and Analysis

### CPU Profiling with perf

```bash
# Record profile
sudo perf record -g -- ./micro/context_switch_bench

# View report
sudo perf report

# Generate flamegraph
sudo perf script | stackcollapse-perf.pl | flamegraph.pl > flame.svg
```

### Cache Analysis with cachegrind

```bash
# Run with cache profiling
valgrind --tool=cachegrind ./micro/allocation_bench

# View results
cg_annotate cachegrind.out.<pid>

# Key metrics:
# - I1mr: Instruction cache misses
# - D1mr: Data cache misses
# - LLmr: Last-level cache misses
```

### Memory Profiling with massif

```bash
# Profile memory usage
valgrind --tool=massif ./stress/memory_stress

# View results
ms_print massif.out.<pid>
```

### Kernel Tracing with perf

```bash
# Trace context switches
sudo perf record -e sched:sched_switch -a -g

# Trace page allocations
sudo perf record -e kmem:mm_page_alloc -a -g

# Trace syscalls
sudo perf trace -a
```

## CI Integration

### GitHub Actions Example

```yaml
name: Performance Benchmarks

on: [push, pull_request]

jobs:
  benchmark:
    runs-on: ubuntu-latest
    
    steps:
    - uses: actions/checkout@v2
    
    - name: Install dependencies
      run: |
        sudo apt-get update
        sudo apt-get install -y build-essential linux-tools-generic
    
    - name: Build benchmarks
      run: |
        cd benchmarks
        make all
    
    - name: Run regression baseline
      run: |
        cd benchmarks
        ./regression/baseline --baseline baseline.txt
    
    - name: Check for regressions
      run: |
        if grep -q "REGRESSION DETECTED" regression_report.json; then
          echo "Performance regression detected!"
          exit 1
        fi
    
    - name: Upload results
      uses: actions/upload-artifact@v2
      with:
        name: benchmark-results
        path: benchmarks/results/
```

## Results Directory Structure

```
results/
├── benchmark_20260526_143022.txt    # Full benchmark output
├── benchmarks_20260526_143022.tar.gz # Archived results
├── regression_report.json            # JSON report for CI
└── baseline.txt                      # Regression baseline
```

## Interpreting Performance Claims

### PCID Optimization (40-60% improvement)

**What to look for:**
- Context switch median: Should be 40-60% lower than without PCID
- Greater improvement with high TLB pressure
- CPU must support PCID (check with `cpuid`)

**Expected values:**
- Without PCID: 1000-2000 cycles
- With PCID: 400-800 cycles

### Per-CPU Cache (10x improvement)

**What to look for:**
- Allocation median: Should be 90%+ faster
- Cache hit rate: Should be > 90%
- Benefits scale with CPU count

**Expected values:**
- Without cache: 500-1000 cycles
- With cache: 5-50 cycles (90%+ hit rate)

### Syscall Optimization (40-56% improvement)

**What to look for:**
- Syscall latency: Should be 40-56% lower
- Biggest wins on simple syscalls (getpid)
- Error paths should be very fast (< 100 cycles)

**Expected values:**
- Unoptimized: 300-500 cycles
- Optimized: 150-250 cycles

## Hardware Requirements

### Minimum

- x86_64 CPU (Intel or AMD)
- 4 GB RAM
- 10 GB disk space
- Linux kernel 5.4+

### Recommended

- Modern x86_64 CPU with PCID support
- 8+ GB RAM
- SSD storage
- Linux kernel 6.0+
- Isolated CPU core for benchmarking

### For Full Validation

- NVMe SSD (for I/O benchmarks)
- Gigabit Ethernet (for network benchmarks)
- 8+ CPU cores (for scaling tests)
- Hardware performance counters enabled

## Troubleshooting

### "CPU governor not set to performance"

```bash
# Set governor
sudo cpupower frequency-set -g performance

# Verify
cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
```

### High variance in results

- Close background applications
- Disable turbo boost
- Run benchmarks multiple times
- Pin to specific CPU core
- Check for thermal throttling

### Benchmarks fail to build

```bash
# Check GCC version (need 7.0+)
gcc --version

# Install build dependencies
sudo apt-get install build-essential

# Clean and rebuild
make clean
make all
```

### Permission denied errors

```bash
# Some benchmarks need higher limits
ulimit -n 65536  # File descriptors
ulimit -u 65536  # User processes
```

## Contributing

### Adding New Benchmarks

1. Create source file in appropriate directory
2. Add to `Makefile` targets
3. Update `run_all.sh` to include new benchmark
4. Document in this README
5. Add expected results to `PERFORMANCE_VALIDATION.md`

### Benchmark Guidelines

- Use `bench_common.h` utilities
- Include warmup iterations
- Run sufficient iterations (1000+ for micro-benchmarks)
- Calculate statistics (min/max/mean/median/p95/p99)
- Print results in consistent format
- Document methodology and expected results

## References

- [Performance Validation Report](../docs/PERFORMANCE_VALIDATION.md)
- [Intel Software Developer Manual](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)
- [Linux Kernel Documentation](https://www.kernel.org/doc/html/latest/)
- [perf Examples](http://www.brendangregg.com/perf.html)

## License

This benchmark suite is part of AutomationOS and is licensed under the same terms.

## Support

For questions or issues:
- Open an issue on GitHub
- Check existing benchmarks for examples
- Review `PERFORMANCE_VALIDATION.md` for detailed analysis

---

**Last Updated:** 2026-05-26  
**Version:** 1.0  
**Maintained By:** AutomationOS Performance Team
