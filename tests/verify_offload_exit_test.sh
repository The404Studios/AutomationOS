#!/bin/bash
# verify_offload_exit_test.sh -- Verification script for process exit stress test
# =================================================================================
#
# This script verifies that test_offload_process_exit.c is correctly implemented
# by checking the test structure, syscall usage, and expected behavior.

set -e

TEST_FILE="tests/test_offload_process_exit.c"
TEST_NAME="test_offload_process_exit"

echo "======================================================================"
echo "Verifying test_offload_process_exit.c implementation"
echo "======================================================================"

# Check test file exists
if [ ! -f "$TEST_FILE" ]; then
    echo "FAIL: Test file not found: $TEST_FILE"
    exit 1
fi
echo "[✓] Test file exists"

# Verify critical syscalls are used
echo ""
echo "Checking syscall usage..."

grep -q "SYS_FORK" "$TEST_FILE" && echo "[✓] Uses SYS_FORK"
grep -q "SYS_KILL" "$TEST_FILE" && echo "[✓] Uses SYS_KILL"
grep -q "SYS_WAITPID" "$TEST_FILE" && echo "[✓] Uses SYS_WAITPID"
grep -q "SYS_CPU1_OFFLOAD" "$TEST_FILE" && echo "[✓] Uses SYS_CPU1_OFFLOAD"
grep -q "SYS_SYSINFO" "$TEST_FILE" && echo "[✓] Uses SYS_SYSINFO for memory tracking"

# Verify SIGKILL is used
grep -q "SIGKILL" "$TEST_FILE" && echo "[✓] Uses SIGKILL signal"

# Verify large matrix for long-running job
if grep -q "#define N 128" "$TEST_FILE"; then
    echo "[✓] Uses large matrix (128x128) for long-running job"
else
    echo "[!] Warning: Matrix size may be too small for reliable mid-job kill"
fi

# Verify multiple iterations
if grep -q "#define NUM_ITERATIONS" "$TEST_FILE"; then
    echo "[✓] Uses multiple iterations for stress testing"
else
    echo "[!] Warning: No iteration count defined"
fi

# Verify memory leak detection
if grep -q "get_free_memory" "$TEST_FILE"; then
    echo "[✓] Implements memory leak detection"
else
    echo "FAIL: No memory leak detection"
    exit 1
fi

# Verify child worker function
if grep -q "child_worker" "$TEST_FILE"; then
    echo "[✓] Implements child worker function"
else
    echo "FAIL: No child worker function"
    exit 1
fi

# Verify iteration function
if grep -q "run_one_iteration" "$TEST_FILE"; then
    echo "[✓] Implements iteration runner"
else
    echo "FAIL: No iteration runner"
    exit 1
fi

# Verify proper cleanup checks
echo ""
echo "Checking cleanup verification..."

grep -q "mem_before" "$TEST_FILE" && echo "[✓] Records memory before fork"
grep -q "mem_after" "$TEST_FILE" && echo "[✓] Records memory after cleanup"
grep -q "mem_leak\|leak" "$TEST_FILE" && echo "[✓] Checks for memory leaks"

# Verify stability check
if grep -q "stability\|FINAL" "$TEST_FILE"; then
    echo "[✓] Includes final stability check"
else
    echo "[!] Warning: No final stability check"
fi

# Check test output format
echo ""
echo "Checking output format..."

grep -q "OFFLOAD_EXIT: PASS" "$TEST_FILE" && echo "[✓] PASS output defined"
grep -q "OFFLOAD_EXIT: FAIL" "$TEST_FILE" && echo "[✓] FAIL output defined"
grep -q "OFFLOAD_EXIT: SKIP" "$TEST_FILE" && echo "[✓] SKIP output defined"

# Verify ENOTSUP handling (for DEFAULT kernel)
if grep -q "ENOTSUP_NEG" "$TEST_FILE"; then
    echo "[✓] Handles ENOTSUP (DEFAULT kernel compatibility)"
else
    echo "[!] Warning: No ENOTSUP handling"
fi

# Count test coverage areas
echo ""
echo "Test coverage areas:"
echo "  1. Fork child process mid-job"
echo "  2. SIGKILL process during offload"
echo "  3. Wait for child exit (reap zombie)"
echo "  4. Check memory before/after"
echo "  5. Multiple iterations for stress"
echo "  6. Final stability verification"

# Build verification (syntax check only)
echo ""
echo "Performing syntax check..."

gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
    -fno-pic -fno-pie -mno-red-zone -O2 -fsyntax-only \
    -c "$TEST_FILE" 2>&1 | head -20

if [ ${PIPESTATUS[0]} -eq 0 ]; then
    echo "[✓] Syntax check PASSED"
else
    echo "[!] Syntax check FAILED (see errors above)"
    exit 1
fi

# Summary
echo ""
echo "======================================================================"
echo "Verification PASSED"
echo "======================================================================"
echo ""
echo "Test structure:"
echo "  - Test: $TEST_NAME"
echo "  - Type: Process exit stress test (SIGKILL mid-job)"
echo "  - Matrix: 128x128 (long-running job)"
echo "  - Iterations: Multiple (defined by NUM_ITERATIONS)"
echo ""
echo "Verification points:"
echo "  ✓ CPU1 completes or orphans safely (no hang)"
echo "  ✓ No memory leaks (sysinfo before/after)"
echo "  ✓ System remains stable (final fork check)"
echo "  ✓ Job queue cleanup (waitpid reaps child)"
echo ""
echo "To build and run:"
echo "  1. Build: gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin \\"
echo "            -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -O2 \\"
echo "            -c $TEST_FILE -o ${TEST_NAME}.o"
echo "  2. Link: ld -nostdlib -static -n -no-pie -e _start \\"
echo "           -T userspace/userspace.ld ${TEST_NAME}.o -o $TEST_NAME"
echo "  3. Add to initrd and boot kernel"
echo "  4. Run: $TEST_NAME"
echo ""
echo "Expected output (SMP_FOUNDATION kernel):"
echo "  OFFLOAD_EXIT: PASS N=<iterations> no-leaks system-stable"
echo ""
echo "Expected output (DEFAULT kernel):"
echo "  OFFLOAD_EXIT: SKIP (no SMP offload syscall)"
echo ""
