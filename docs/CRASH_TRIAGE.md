# AutomationOS Crash Triage Guide

This guide provides systematic procedures for analyzing and fixing crashes discovered by fuzzers.

## Table of Contents

1. [Overview](#overview)
2. [Initial Triage](#initial-triage)
3. [Crash Classification](#crash-classification)
4. [Root Cause Analysis](#root-cause-analysis)
5. [Reproducing Crashes](#reproducing-crashes)
6. [Debugging Techniques](#debugging-techniques)
7. [Writing Regression Tests](#writing-regression-tests)
8. [Bug Reporting](#bug-reporting)

## Overview

### Crash Triage Workflow

```
┌─────────────────┐
│ Crash Detected  │
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│ 1. Reproduce    │  ← Can you trigger it reliably?
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│ 2. Classify     │  ← What type of bug is it?
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│ 3. Analyze      │  ← What's the root cause?
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│ 4. Fix          │  ← Implement the patch
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│ 5. Test         │  ← Verify fix + regression test
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│ 6. Document     │  ← File bug report + CVE if needed
└─────────────────┘
```

## Initial Triage

### Step 1: Collect Crash Information

When a fuzzer crashes, collect:

```bash
# 1. Crash input file
CRASH_FILE="tests/fuzz/crashes/id:000042,sig:11,src:000123,op:havoc,rep:4"

# 2. Fuzzer logs
cat tests/fuzz/output/syscall/default/fuzzer_stats

# 3. System logs (if applicable)
dmesg | tail -50
journalctl -xe | tail -50
```

### Step 2: Quick Reproduction

```bash
# Try to reproduce immediately
./tests/fuzz/syscall_fuzzer --input "$CRASH_FILE"
```

**Outcome**:
- ✅ **Reproducible**: Crash happens every time → Proceed to classification
- ❌ **Non-reproducible**: Intermittent crash → Likely race condition or resource leak

### Step 3: Initial Classification

```bash
# Run with AddressSanitizer (ASAN)
ASAN_OPTIONS=detect_leaks=1:symbolize=1:abort_on_error=1 \
    ./tests/fuzz/syscall_fuzzer --input "$CRASH_FILE"

# Run with Valgrind
valgrind --leak-check=full --track-origins=yes \
    ./tests/fuzz/syscall_fuzzer --input "$CRASH_FILE"
```

## Crash Classification

### Common Crash Types

| Signal | Type | Description | Severity |
|--------|------|-------------|----------|
| SIGSEGV (11) | Segmentation Fault | Invalid memory access (NULL deref, buffer overflow) | **CRITICAL** |
| SIGILL (4) | Illegal Instruction | Corrupted function pointer, ROP gadget | **CRITICAL** |
| SIGABRT (6) | Assertion Failure | Failed assertion, double-free detected | **HIGH** |
| SIGFPE (8) | Floating Point Exception | Division by zero, integer overflow | **MEDIUM** |
| SIGBUS (7) | Bus Error | Misaligned memory access | **HIGH** |
| SIGALRM (14) | Timeout | Infinite loop, deadlock | **HIGH** |

### Vulnerability Classification

#### 1. Buffer Overflow

**Symptoms**:
- SIGSEGV with write to unmapped memory
- ASAN: heap-buffer-overflow or stack-buffer-overflow

**Example**:
```
==12345==ERROR: AddressSanitizer: heap-buffer-overflow on address 0x61400000fe44
WRITE of size 4 at 0x61400000fe44 thread T0
    #0 0x4a2b5c in sys_read kernel/core/syscall/handlers.c:42
```

**Root Cause**: Missing bounds check

**Severity**: **CRITICAL** (arbitrary code execution possible)

#### 2. NULL Pointer Dereference

**Symptoms**:
- SIGSEGV with address near 0x0
- Crash in simple pointer dereference

**Example**:
```
Program received signal SIGSEGV, Segmentation fault.
0x0000000000401234 in sys_read (fd=0, buf=0x0, count=100) at handlers.c:25
25          *buf = data;
(gdb) print buf
$1 = (void *) 0x0
```

**Root Cause**: Missing NULL check

**Severity**: **HIGH** (denial of service)

#### 3. Use-After-Free (UAF)

**Symptoms**:
- SIGSEGV with seemingly valid pointer
- ASAN: heap-use-after-free
- Pointer points to freed memory

**Example**:
```
==12345==ERROR: AddressSanitizer: heap-use-after-free on address 0x60300000eff0
READ of size 8 at 0x60300000eff0 thread T0
    #0 0x4a3c5d in process_exit kernel/core/sched/process.c:87
freed by thread T0 here:
    #1 0x4a3b12 in kfree kernel/core/mem/heap.c:82
```

**Root Cause**: Dangling pointer after free

**Severity**: **CRITICAL** (arbitrary code execution possible)

#### 4. Double-Free

**Symptoms**:
- SIGABRT from malloc/free
- ASAN: attempting double-free
- Heap corruption detected

**Example**:
```
==12345==ERROR: AddressSanitizer: attempting double-free on 0x60400000eff0
    #0 0x4a2f45 in kfree kernel/core/mem/heap.c:82
    #1 0x4a3214 in cleanup kernel/core/syscall/handlers.c:156
```

**Root Cause**: Free called twice on same pointer

**Severity**: **CRITICAL** (heap corruption → code execution)

#### 5. Integer Overflow

**Symptoms**:
- Unexpected large allocation
- SIGFPE or silent wraparound
- Incorrect size calculation

**Example**:
```c
size_t total = size1 + size2;  // Overflow if size1 + size2 > UINT64_MAX
void* buf = kmalloc(total);    // Allocates tiny buffer
memcpy(buf, data, size1);      // Buffer overflow!
```

**Root Cause**: Missing overflow check

**Severity**: **HIGH** (can lead to buffer overflow)

#### 6. Race Condition

**Symptoms**:
- Non-reproducible crash
- Occurs only under high load
- TOCTOU (Time-of-Check-Time-of-Use)

**Example**:
```c
// Thread 1
if (ptr != NULL) {         // Check
    // [RACE WINDOW]
    use_ptr(ptr);          // Use
}

// Thread 2
ptr = NULL;                // Concurrent modification
```

**Root Cause**: Missing synchronization

**Severity**: **HIGH** (unpredictable behavior)

## Root Cause Analysis

### GDB Workflow

```bash
# 1. Start GDB
gdb ./tests/fuzz/syscall_fuzzer

# 2. Run with crash input
(gdb) run --input tests/fuzz/crashes/id:000042,sig:11

# 3. Examine crash
(gdb) backtrace          # Call stack
(gdb) frame 0            # Focus on crash frame
(gdb) info registers     # Register values
(gdb) x/20x $rsp         # Stack dump
(gdb) disassemble        # Assembly code

# 4. Inspect variables
(gdb) print ptr
(gdb) print *ptr         # Dereference (if valid)
(gdb) print sizeof(buf)

# 5. Examine memory
(gdb) x/40wx 0x7fffffffd000   # Hex dump
(gdb) x/s 0x7fffffffd000      # String dump

# 6. Set breakpoints for next run
(gdb) break handlers.c:42
(gdb) run --input tests/fuzz/crashes/id:000042,sig:11
(gdb) continue
```

### ASAN Analysis

```bash
# Run with verbose ASAN
ASAN_OPTIONS=verbosity=2:detect_leaks=1:symbolize=1 \
    ./tests/fuzz/syscall_fuzzer --input "$CRASH_FILE" 2>&1 | tee asan.log

# Examine ASAN report
cat asan.log
```

**ASAN Report Anatomy**:

```
==12345==ERROR: AddressSanitizer: heap-buffer-overflow
  on address 0x61400000fe44 at pc 0x004a2b5c bp 0x7ffd12345678 sp 0x7ffd12345670
WRITE of size 4 at 0x61400000fe44 thread T0
    #0 0x4a2b5c in sys_read handlers.c:42:5          ← Crash location
    #1 0x4a1d34 in syscall_dispatch syscall.c:50:12   ← Caller
    #2 0x401234 in main fuzzer.c:123:8                ← Entry point

0x61400000fe44 is located 4 bytes to the right of 64-byte region [0x61400000fe00,0x61400000fe40)
allocated by thread T0 here:
    #0 0x4a2a12 in malloc                             ← Allocation
    #1 0x4a3c45 in kmalloc heap.c:47:12               ← Wrapper

SUMMARY: AddressSanitizer: heap-buffer-overflow handlers.c:42:5 in sys_read
```

**Key Information**:
- **Error type**: heap-buffer-overflow
- **Crash location**: handlers.c:42 (sys_read)
- **Access type**: WRITE of 4 bytes
- **Address**: 4 bytes beyond allocated region
- **Allocation site**: heap.c:47 (kmalloc)

### Valgrind Analysis

```bash
valgrind --leak-check=full \
         --track-origins=yes \
         --show-reachable=yes \
         ./tests/fuzz/syscall_fuzzer --input "$CRASH_FILE" 2>&1 | tee valgrind.log
```

## Reproducing Crashes

### Deterministic Reproduction

```bash
# 1. Disable ASLR
echo 0 | sudo tee /proc/sys/kernel/randomize_va_space

# 2. Set fixed random seed
export FUZZ_SEED=12345

# 3. Run fuzzer
./tests/fuzz/syscall_fuzzer --input "$CRASH_FILE" --seed 12345
```

### Race Condition Reproduction

```bash
# Use stress-ng or custom scripts to increase race likelihood
stress-ng --cpu 8 --io 4 --vm 2 &
STRESS_PID=$!

# Run fuzzer multiple times
for i in {1..1000}; do
    ./tests/fuzz/syscall_fuzzer --input "$CRASH_FILE" || break
done

kill $STRESS_PID
```

### Minimizing Crash Input

```bash
# Use afl-tmin to minimize crash input
afl-tmin -i "$CRASH_FILE" \
         -o tests/fuzz/crashes/minimized_crash \
         -- ./tests/fuzz/syscall_fuzzer @@

# Manually minimize
# 1. Remove bytes from end
# 2. Try to trigger crash
# 3. Repeat until minimal input found
```

## Debugging Techniques

### Static Analysis

```bash
# Cppcheck
cppcheck --enable=all --inconclusive kernel/core/syscall/handlers.c

# Clang Static Analyzer
scan-build make -C kernel/

# Coverity (if available)
cov-build --dir cov-int make
cov-analyze --dir cov-int
cov-format-errors --dir cov-int
```

### Dynamic Analysis

```bash
# ThreadSanitizer (race detection)
gcc -fsanitize=thread -g fuzzer.c -o fuzzer
./fuzzer --input "$CRASH_FILE"

# UndefinedBehaviorSanitizer
gcc -fsanitize=undefined -g fuzzer.c -o fuzzer
./fuzzer --input "$CRASH_FILE"

# Memory Sanitizer (uninitialized memory)
clang -fsanitize=memory -g fuzzer.c -o fuzzer
./fuzzer --input "$CRASH_FILE"
```

### Reverse Engineering

```bash
# Disassemble binary
objdump -d tests/fuzz/syscall_fuzzer > disasm.txt

# Decompile with Ghidra
ghidra &
# File → Import File → syscall_fuzzer
# Analyze → Auto Analyze

# Use IDA Pro (commercial)
ida64 tests/fuzz/syscall_fuzzer
```

## Writing Regression Tests

### Unit Test for Bug

```c
// tests/unit/test_syscall_overflow.c

#include "../../kernel/include/syscall.h"
#include <assert.h>

void test_read_overflow_bug() {
    // Reproduce the bug
    uint64_t fd = 0;
    uint64_t buf = 0x1000;
    uint64_t count = UINT64_MAX;  // Overflow trigger

    int64_t result = sys_read(fd, buf, count, 0, 0, 0);

    // Should return error, not crash
    assert(result == EINVAL);
}

int main() {
    test_read_overflow_bug();
    printf("Test passed!\n");
    return 0;
}
```

### Fuzzer-Specific Test

```bash
# Add crash input to regression corpus
cp "$CRASH_FILE" tests/fuzz/corpus/syscall_seeds/regression_000042

# Verify it doesn't crash after fix
./tests/fuzz/syscall_fuzzer --input tests/fuzz/corpus/syscall_seeds/regression_000042
echo $?  # Should be 0 (success)
```

## Bug Reporting

### Internal Bug Report Template

```markdown
# Bug Report: [Heap Buffer Overflow in sys_read()]

## Summary
Buffer overflow in sys_read() when count parameter exceeds buffer size.

## Severity
**CRITICAL** - Arbitrary code execution possible

## Affected Components
- kernel/core/syscall/handlers.c:42

## Reproduction
1. Build fuzzer: `make -C tests/fuzz`
2. Run: `./tests/fuzz/syscall_fuzzer --input tests/fuzz/crashes/id:000042`
3. Observe SIGSEGV

## Root Cause
Missing bounds check on `count` parameter before memcpy().

```c
// handlers.c:42
int64_t sys_read(uint64_t fd, uint64_t buf, uint64_t count, ...) {
    char kernel_buf[4096];
    // BUG: No check if count > sizeof(kernel_buf)
    memcpy((void*)buf, kernel_buf, count);  // Overflow!
}
```

## Proposed Fix
Add bounds check:

```c
if (count > sizeof(kernel_buf)) {
    return -EINVAL;
}
```

## Test Case
- Crash input: `tests/fuzz/crashes/id:000042`
- Regression test: `tests/unit/test_syscall_overflow.c`

## CVE
- [ ] Assign CVE number (if public release)
- [ ] Coordinate disclosure (90-day window)
```

### CVE Assignment (Public Bugs)

If bug is exploitable and AutomationOS is public:

1. **Request CVE**: Email cve@mitre.org or use https://cveform.mitre.org/
2. **Provide**:
   - Vendor: AutomationOS
   - Product: AutomationOS Kernel
   - Version: 0.1.0
   - Description: Buffer overflow in sys_read allows arbitrary code execution
   - References: GitHub issue link
3. **Coordinate Disclosure**: 90-day embargo before public disclosure

## Checklist

Before closing a crash bug:

- [ ] Crash is reproducible
- [ ] Root cause identified
- [ ] Patch implemented
- [ ] Patch tested with original crash input
- [ ] Regression test written
- [ ] Code reviewed
- [ ] Fuzzer run for 24h post-patch (no regressions)
- [ ] Bug report filed
- [ ] CVE assigned (if needed)
- [ ] Crash input added to regression corpus

## References

- [AFL++ Crash Exploration](https://github.com/AFLplusplus/AFLplusplus/blob/stable/docs/crash_exploration.md)
- [GDB Cheat Sheet](https://darkdust.net/files/GDB%20Cheat%20Sheet.pdf)
- [AddressSanitizer Documentation](https://github.com/google/sanitizers/wiki/AddressSanitizer)
- [Valgrind Manual](https://valgrind.org/docs/manual/manual.html)
- [CVE Program](https://www.cve.org/)
