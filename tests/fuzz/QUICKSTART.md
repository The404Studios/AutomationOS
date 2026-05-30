# Fuzzing Quick Start Guide

Get started with AutomationOS fuzzing in 5 minutes.

## Prerequisites

- Ubuntu/Debian or Arch Linux
- 8GB+ RAM
- 50GB+ disk space
- sudo privileges

## Installation

### One-Line Install (Ubuntu/Debian)

```bash
sudo apt-get update && sudo apt-get install -y afl++ gcc-multilib python3 lcov && \
cd tests/fuzz && make && echo "✅ Installation complete!"
```

### One-Line Install (Arch Linux)

```bash
sudo pacman -S --noconfirm afl++ gcc-multilib python lcov && \
cd tests/fuzz && make && echo "✅ Installation complete!"
```

## Quick Test (2 minutes)

```bash
# 1. Build fuzzers
make -C tests/fuzz

# 2. Generate corpus
cd tests/fuzz && python3 generate_corpus.py && cd ../..

# 3. Run smoke test
make -C tests/fuzz test

# Expected output: "All smoke tests passed!"
```

## Your First Fuzzing Campaign (5 minutes)

```bash
# Run all fuzzers for 5 minutes
./scripts/run-fuzzer.sh --all --time 5m

# Check results
./scripts/fuzzer-status.sh

# View any crashes
ls -lh tests/fuzz/crashes/
```

## Standalone Fuzzing (No AFL++)

If AFL++ is not available, run fuzzers in standalone mode:

```bash
# Syscall fuzzer (1 million iterations, ~15 minutes)
./tests/fuzz/syscall_fuzzer --iterations 1000000

# Heap stress test (10 concurrent threads)
./tests/fuzz/heap_fuzzer --stress --concurrent 10 --iterations 100000

# Driver fuzzer (PS/2)
./tests/fuzz/driver_fuzzer --driver ps2 --iterations 100000
```

## AFL++ Fuzzing (Coverage-Guided)

For best results, use AFL++:

```bash
# Syscall fuzzer with AFL++
afl-fuzz -i tests/fuzz/corpus/syscall_seeds \
         -o tests/fuzz/output/syscall \
         -m none \
         -- ./tests/fuzz/syscall_fuzzer @@

# Press Ctrl+C to stop, then check crashes:
ls tests/fuzz/output/syscall/default/crashes/
```

## Common Issues

### "AFL++ not found"
```bash
# Install AFL++
sudo apt-get install afl++  # Ubuntu/Debian
sudo pacman -S afl++        # Arch Linux
```

### "Permission denied on /proc/sys/kernel/core_pattern"
```bash
# Configure system for fuzzing (requires sudo)
echo core | sudo tee /proc/sys/kernel/core_pattern
```

### Low execution speed
```bash
# Disable ASAN for faster fuzzing (rebuild without sanitizers)
make -C tests/fuzz clean
CFLAGS="-O3 -g" make -C tests/fuzz
```

## Next Steps

- **Read the full guide**: `docs/FUZZING_GUIDE.md`
- **Learn crash triage**: `docs/CRASH_TRIAGE.md`
- **Run 24h campaign**: `./scripts/run-fuzzer.sh --continuous`
- **Check CI integration**: `.github/workflows/fuzzing.yml`

## Quick Reference

| Command | Description |
|---------|-------------|
| `make -C tests/fuzz` | Build all fuzzers |
| `make -C tests/fuzz test` | Run smoke tests |
| `make -C tests/fuzz clean` | Clean build artifacts |
| `./scripts/run-fuzzer.sh --all --time 1h` | Fuzz for 1 hour |
| `./scripts/fuzzer-status.sh` | Check fuzzing status |
| `ls tests/fuzz/crashes/` | View crash files |

## Help

For detailed documentation, see:
- Full guide: `docs/FUZZING_GUIDE.md`
- Crash triage: `docs/CRASH_TRIAGE.md`
- Fuzzer README: `tests/fuzz/README.md`

Report bugs or ask questions in GitHub Issues.

Happy fuzzing! 🐛🔨
