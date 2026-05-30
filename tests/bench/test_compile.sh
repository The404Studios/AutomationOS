#!/bin/bash
#
# Quick compile test for all benchmarks
# Verifies that all benchmarks compile without errors
#

set -e

echo "Testing benchmark compilation..."
echo ""

BENCHMARKS=(
    "bench_futex"
    "bench_file_io"
    "bench_network"
    "bench_fork"
    "bench_malloc"
    "bench_epoll"
)

CC="${CC:-gcc}"
CFLAGS="-Wall -Wextra -O2 -I../../kernel/include -static"

SUCCESS=0
FAILED=0

for bench in "${BENCHMARKS[@]}"; do
    echo -n "Compiling ${bench}.c ... "

    if ${CC} ${CFLAGS} -o "/tmp/${bench}_test" "${bench}.c" 2>/tmp/compile_error.txt; then
        echo "✓ OK"
        rm -f "/tmp/${bench}_test"
        SUCCESS=$((SUCCESS + 1))
    else
        echo "✗ FAILED"
        cat /tmp/compile_error.txt
        FAILED=$((FAILED + 1))
    fi
done

echo ""
echo "===================="
echo "Compilation Summary"
echo "===================="
echo "Success: ${SUCCESS}"
echo "Failed:  ${FAILED}"
echo ""

if [ ${FAILED} -eq 0 ]; then
    echo "[PASS] All benchmarks compile successfully"
    exit 0
else
    echo "[FAIL] Some benchmarks failed to compile"
    exit 1
fi
