#!/bin/bash
#
# Audit Rate Limiter SMP Verification Script
# ===========================================
#
# Builds and runs the SMP stress test for the audit rate limiter.
# Verifies correctness under concurrent load from multiple CPUs.

set -e

KERNEL_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
TEST_DIR="${KERNEL_ROOT}/tests/smp"
BUILD_DIR="${KERNEL_ROOT}/build/tests/smp"

echo "========================================="
echo "Audit Rate Limiter SMP Test"
echo "========================================="
echo ""

# Create build directory
mkdir -p "${BUILD_DIR}"

# Compile test
echo "[BUILD] Compiling test_audit_rate_limit_smp.c..."

gcc -O2 \
    -I"${KERNEL_ROOT}/kernel/include" \
    -I"${KERNEL_ROOT}/kernel/arch/x86_64" \
    -DSMP_ENABLE \
    -DHAVE_TIMER \
    -c "${TEST_DIR}/test_audit_rate_limit_smp.c" \
    -o "${BUILD_DIR}/test_audit_rate_limit_smp.o"

# Link with kernel audit subsystem
echo "[BUILD] Linking with audit subsystem..."

gcc -O2 \
    "${BUILD_DIR}/test_audit_rate_limit_smp.o" \
    "${KERNEL_ROOT}/kernel/audit/log.o" \
    "${KERNEL_ROOT}/kernel/audit/buffer.o" \
    "${KERNEL_ROOT}/kernel/audit/filter.o" \
    "${KERNEL_ROOT}/kernel/audit/rules.o" \
    -o "${BUILD_DIR}/test_audit_rate_limit_smp"

echo "[BUILD] Build successful"
echo ""

# Run test
echo "[RUN] Executing SMP stress test..."
echo ""

"${BUILD_DIR}/test_audit_rate_limit_smp"
TEST_RESULT=$?

echo ""
if [ $TEST_RESULT -eq 0 ]; then
    echo "========================================="
    echo "  TEST PASSED"
    echo "========================================="
    exit 0
else
    echo "========================================="
    echo "  TEST FAILED"
    echo "========================================="
    exit 1
fi
