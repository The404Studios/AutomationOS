# Static Analysis Guide

## Overview

AutomationOS integrates multiple static analysis tools to ensure code quality, detect bugs early, and maintain security standards. This document describes how to use the static analysis tools and interpret their results.

## Available Tools

### 1. Clang Static Analyzer
- **Purpose:** Deep dataflow analysis, NULL pointer dereferences, memory leaks, use-after-free
- **Target:** `make analyze`
- **Runtime:** ~2-3 minutes (full scan)

### 2. Cppcheck
- **Purpose:** Common C/C++ bugs, bounds errors, uninitialized variables, resource leaks
- **Target:** `make cppcheck`
- **Runtime:** ~1 minute

### 3. Sparse (Linux Kernel Semantic Checker)
- **Purpose:** Kernel-specific issues, annotation checking, address space violations
- **Target:** `make sparse`
- **Runtime:** ~30 seconds

### 4. Custom Clang-Tidy Checks
- **Purpose:** AutomationOS-specific patterns and conventions
- **Target:** `make clang-tidy`
- **Runtime:** ~2 minutes

## Quick Start

### Run All Analyzers

```bash
# Run complete static analysis suite
make analyze-all

# Run specific analyzer
make analyze        # Clang Static Analyzer
make cppcheck       # Cppcheck
make sparse         # Sparse
make clang-tidy     # Clang-Tidy with custom checks
```

### View Results

```bash
# Generate HTML report (Clang Static Analyzer)
make analyze-report

# View latest scan results
cat build/static-analysis/latest-scan.txt

# View detailed results
ls build/static-analysis/
```

## Installation

### Ubuntu/Debian

```bash
sudo apt-get update
sudo apt-get install -y \
    clang \
    clang-tools \
    clang-tidy \
    cppcheck \
    sparse
```

### Arch Linux

```bash
sudo pacman -S \
    clang \
    clang-tools-extra \
    cppcheck \
    sparse
```

### macOS

```bash
brew install \
    llvm \
    cppcheck \
    sparse
```

## Tool Details

### Clang Static Analyzer

The Clang Static Analyzer performs deep path-sensitive analysis to find bugs.

**Checks Performed:**
- NULL pointer dereferences
- Use of uninitialized values
- Memory leaks
- Use-after-free
- Double-free
- Resource leaks (file descriptors, locks)
- Dead code
- Logic errors

**Usage:**

```bash
# Full analysis
make analyze

# Generate HTML report
make analyze-report

# Analyze specific file
scan-build --use-cc=x86_64-elf-gcc -o build/scan-results gcc -c kernel/core/mem/pmm.c
```

**Configuration:**

Analysis configuration is in `.clang-analyzer`:
- Enable all checkers
- Cross-compilation support for x86_64-elf-gcc
- Higher-half kernel address space handling

### Cppcheck

Cppcheck is a static analysis tool for C/C++ code focusing on zero false positives.

**Checks Performed:**
- Array bounds violations
- Null pointer dereferences
- Uninitialized variables
- Memory and resource leaks
- Invalid usage of STL
- Suspicious or redundant code
- Unused functions

**Usage:**

```bash
# Standard check
make cppcheck

# Verbose output
make cppcheck CPPCHECK_OPTS="--verbose"

# Check specific directory
cppcheck --enable=all --suppress=missingInclude kernel/core/
```

**Configuration:**

Configuration is in `.cppcheck`:
- Enable all checks except style (style checks produce too many false positives for kernel code)
- Suppress common false positives
- Kernel-specific macro definitions

### Sparse

Sparse is the semantic checker used by the Linux kernel to detect kernel-specific issues.

**Checks Performed:**
- Missing `__user` annotations on user pointers
- Address space violations
- Bitwise type errors
- Context imbalance (lock/unlock)
- Endianness issues
- NULL pointer arithmetic

**Usage:**

```bash
# Run sparse on all kernel code
make sparse

# Check specific file with context tracking
sparse -Wcontext kernel/core/syscall/handlers.c
```

**Configuration:**

Sparse configuration in `Makefile`:
- `-D__KERNEL__` for kernel mode
- `-Waddress-space` for user/kernel pointer checking
- `-Wcontext` for lock balance verification

**Kernel Annotations:**

```c
// User pointer annotation
int copy_from_user(void *to, const void __user *from, unsigned long n);

// Context checking (lock tracking)
void acquire_lock(lock_t *lock) __attribute__((context(lock, 0, 1)));
void release_lock(lock_t *lock) __attribute__((context(lock, 1, 0)));
```

### Clang-Tidy

Clang-Tidy provides lint-style checks and automatic fixes with custom rule support.

**AutomationOS Custom Checks:**

1. **Syscall Pointer Validation** (`automationos-syscall-validation`)
   - Ensures all syscalls validate user pointers
   - Requires `copy_from_user()`/`copy_to_user()` for user data

2. **NULL Check After Allocation** (`automationos-null-check`)
   - Verifies all `kmalloc()`/`pmm_alloc()` results are NULL-checked
   - Ensures error handling before use

3. **Lock Balance** (`automationos-lock-balance`)
   - Detects unbalanced lock acquire/release
   - Tracks spinlock and mutex operations
   - Reports missing unlocks

**Usage:**

```bash
# Run all checks
make clang-tidy

# Run specific check
clang-tidy -checks='automationos-*' kernel/core/syscall/handlers.c

# Apply automatic fixes
clang-tidy -checks='automationos-*' -fix kernel/core/mem/heap.c
```

**Configuration:**

Configuration is in `.clang-tidy`:
```yaml
Checks: >
  automationos-*,
  bugprone-*,
  cert-*,
  clang-analyzer-*,
  concurrency-*,
  performance-*,
  portability-*,
  readability-*
```

## CI Integration

Static analysis runs automatically on every commit via GitHub Actions.

### Workflow: `.github/workflows/static-analysis.yml`

```yaml
name: Static Analysis

on: [push, pull_request]

jobs:
  analyze:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v3
      - name: Install tools
        run: |
          sudo apt-get install -y clang clang-tools cppcheck sparse
      - name: Run static analysis
        run: make analyze-all
      - name: Upload results
        uses: actions/upload-artifact@v3
        with:
          name: static-analysis-results
          path: build/static-analysis/
```

### Failure Criteria

The build **fails** if:
- **Critical** issues found (NULL deref, use-after-free, memory leak)
- **High severity** issues > 0
- Lock imbalance detected
- User pointer validation missing in syscalls

The build **passes with warning** if:
- **Medium severity** issues (code quality, redundancy)
- **Low severity** issues (style, readability)

### Viewing CI Results

```bash
# Download artifact from GitHub Actions
gh run download <run-id> -n static-analysis-results

# View summary
cat static-analysis-summary.txt
```

## False Positives

Some warnings are false positives due to kernel-specific patterns. These are documented in `.static-analysis-suppressions`:

```
# Format: <tool>:<file>:<line>:<check>:<reason>
clang-analyzer:kernel/arch/x86_64/paging.c:120:core.NullDereference:Higher-half mapping guaranteed non-NULL
cppcheck:kernel/lib/string.c:45:uninitVar:Inline assembly initializes variable
sparse:kernel/core/mem/vmm.c:200:address-space:Physical address intentionally cast
```

### Adding Suppressions

```bash
# Add to suppressions file
echo "cppcheck:kernel/core/perf.c:150:uninitVar:Counter initialized by hardware" >> .static-analysis-suppressions

# Rebuild suppression list
make analyze-update-suppressions
```

## Best Practices

### Before Committing

1. **Run full analysis:**
   ```bash
   make analyze-all
   ```

2. **Fix critical issues immediately:**
   - NULL pointer dereferences
   - Memory leaks
   - Use-after-free

3. **Document false positives:**
   - Add to `.static-analysis-suppressions` with clear reason
   - Include context in commit message

### During Code Review

1. **Check static analysis results in PR:**
   - Review GitHub Actions output
   - Verify new warnings are addressed

2. **Require fixes for:**
   - All critical issues
   - High severity issues
   - Security-relevant warnings

3. **Allow documented false positives:**
   - Verified as safe
   - Added to suppressions with justification

### Weekly Maintenance

```bash
# Run weekly comprehensive scan
make analyze-weekly

# Review and triage warnings
cat build/static-analysis/weekly-report.txt

# Update suppressions if needed
make analyze-update-suppressions
```

## Troubleshooting

### "scan-build: command not found"

Install LLVM tools:
```bash
# Ubuntu/Debian
sudo apt-get install clang-tools

# macOS
brew install llvm
export PATH="/usr/local/opt/llvm/bin:$PATH"
```

### Cross-Compiler Issues

Clang Static Analyzer may have issues with `x86_64-elf-gcc`. Use wrapper:
```bash
# Set cross-compiler in scan-build
make analyze CC=x86_64-elf-gcc
```

### Too Many False Positives

Tune analysis sensitivity:
```bash
# Reduce false positives (fewer warnings)
make analyze ANALYZE_OPTS="--exclude kernel/lib/"

# Aggressive analysis (more warnings)
make analyze ANALYZE_OPTS="-enable-checker alpha"
```

### Sparse Annotation Errors

Add kernel attributes for Sparse:
```c
#ifdef __CHECKER__
# define __user __attribute__((noderef, address_space(1)))
# define __kernel __attribute__((address_space(0)))
#else
# define __user
# define __kernel
#endif
```

## Performance Optimization

### Incremental Analysis

```bash
# Analyze only changed files
make analyze-incremental

# Analyze specific subsystem
make analyze-kernel-core
make analyze-drivers
```

### Parallel Execution

```bash
# Run analyzers in parallel (faster)
make analyze-all -j4

# Background weekly scan
nohup make analyze-weekly > scan.log 2>&1 &
```

### Caching Results

Analysis results are cached in `build/static-analysis/cache/`:
- Only modified files are re-analyzed
- Cache cleared on `make clean`

## Metrics and Reporting

### Issue Severity Levels

- **Critical:** Crashes, memory corruption, security vulnerabilities
- **High:** Logic errors, resource leaks, undefined behavior
- **Medium:** Code quality, maintainability issues
- **Low:** Style, readability suggestions

### Weekly Report Format

```
=== AutomationOS Static Analysis Report ===
Date: 2026-05-26
Commit: a1b2c3d

Summary:
  Critical: 0
  High:     2
  Medium:   15
  Low:      42

Critical Issues: None

High Severity Issues:
  [clang-analyzer] kernel/core/sched/scheduler.c:345: Potential NULL deref in context_switch()
  [cppcheck] kernel/drivers/ps2.c:120: Uninitialized variable 'scancode'

Medium Severity Issues:
  [clang-tidy] kernel/core/mem/heap.c:200: Missing NULL check after kmalloc()
  ...

False Positives Documented: 8
New Suppressions: 2
```

## Advanced Usage

### Custom Checker Development

Create custom Clang-Tidy checks for AutomationOS patterns:

```bash
# Generate checker template
clang-tidy-generator automationos-custom-check

# Build custom checker
cd scripts/static-analysis/custom-checks/
mkdir build && cd build
cmake ..
make
```

See `scripts/static-analysis/custom-checks/README.md` for details.

### Integration with IDEs

#### VS Code

Install extensions:
- `clangd` for real-time analysis
- `C/C++` for Cppcheck integration

Configure `.vscode/settings.json`:
```json
{
  "clangd.arguments": [
    "--compile-commands-dir=build",
    "--enable-checker=automationos.*"
  ],
  "C_Cpp.cppcheck.enable": true
}
```

#### Vim/Neovim

Install `coc-clangd` or `ale`:
```vim
let g:ale_linters = {'c': ['clang', 'cppcheck', 'sparse']}
let g:ale_c_clang_options = '-std=gnu11 -Iinclude'
```

## References

- [Clang Static Analyzer Documentation](https://clang-analyzer.llvm.org/)
- [Cppcheck Manual](http://cppcheck.net/manual.pdf)
- [Sparse Documentation](https://sparse.docs.kernel.org/)
- [Linux Kernel Sparse Annotations](https://www.kernel.org/doc/html/latest/dev-tools/sparse.html)
- [AutomationOS Development Guide](DEVELOPMENT_GUIDE.md)

## Support

For questions or issues with static analysis:
1. Check [TROUBLESHOOTING.md](TROUBLESHOOTING.md)
2. Review existing suppressions in `.static-analysis-suppressions`
3. Open issue on GitHub with `static-analysis` label

---

**Last Updated:** 2026-05-26  
**Maintainer:** AutomationOS Static Analysis Team
