# Makefile for AutomationOS Stress Tests
# Build and run comprehensive stress testing suite

CC = gcc
CFLAGS = -O2 -Wall -Wextra -Werror -std=c11 -I../../kernel/include
LDFLAGS =

# Test executables
STRESS_TESTS = stress_test_suite chaos_engineering race_condition_fuzzer

# Test sources
STRESS_SRCS = stress_test_suite.c
CHAOS_SRCS = chaos_engineering.c
RACE_SRCS = race_condition_fuzzer.c

# Common kernel sources needed for testing
KERNEL_SRCS = \
	../../kernel/core/mem/heap.c \
	../../kernel/core/mem/pmm.c \
	../../kernel/core/sched/process.c \
	../../kernel/lib/string.c \
	../../kernel/lib/printf.c

# Object files
STRESS_OBJS = $(STRESS_SRCS:.c=.o)
CHAOS_OBJS = $(CHAOS_SRCS:.c=.o)
RACE_OBJS = $(RACE_SRCS:.c=.o)
KERNEL_OBJS = $(KERNEL_SRCS:.c=.o)

.PHONY: all clean stress-test chaos-test race-test test-all help

all: $(STRESS_TESTS)
	@echo "Stress tests built successfully!"
	@echo "Run 'make test-all' to execute all tests"

# Build individual test suites
stress_test_suite: $(STRESS_OBJS)
	@echo "[BUILD] Stress Test Suite"
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

chaos_engineering: $(CHAOS_OBJS)
	@echo "[BUILD] Chaos Engineering Tests"
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

race_condition_fuzzer: $(RACE_OBJS)
	@echo "[BUILD] Race Condition Fuzzer"
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Compile object files
%.o: %.c
	@echo "[CC] $<"
	$(CC) $(CFLAGS) -c -o $@ $<

# Run tests
stress-test: stress_test_suite
	@echo ""
	@echo "=========================================="
	@echo " Running Stress Tests"
	@echo "=========================================="
	./stress_test_suite | tee stress_test_results.log
	@echo ""
	@echo "Results saved to: stress_test_results.log"

chaos-test: chaos_engineering
	@echo ""
	@echo "=========================================="
	@echo " Running Chaos Engineering Tests"
	@echo "=========================================="
	./chaos_engineering | tee chaos_test_results.log
	@echo ""
	@echo "Results saved to: chaos_test_results.log"

race-test: race_condition_fuzzer
	@echo ""
	@echo "=========================================="
	@echo " Running Race Condition Fuzzer"
	@echo "=========================================="
	./race_condition_fuzzer | tee race_test_results.log
	@echo ""
	@echo "Results saved to: race_test_results.log"

# Run all tests
test-all: all
	@echo ""
	@echo "=========================================="
	@echo " Running Complete Test Suite"
	@echo "=========================================="
	@echo ""
	$(MAKE) stress-test
	@echo ""
	$(MAKE) chaos-test
	@echo ""
	$(MAKE) race-test
	@echo ""
	@echo "=========================================="
	@echo " All Tests Complete"
	@echo "=========================================="
	@echo ""
	@echo "Test Results:"
	@echo "  - stress_test_results.log"
	@echo "  - chaos_test_results.log"
	@echo "  - race_test_results.log"
	@echo ""

# Run with QEMU (if kernel image exists)
qemu-stress: all
	@echo "[QEMU] Running stress tests in emulator"
	qemu-system-x86_64 \
		-kernel ../../kernel/kernel.elf \
		-serial stdio \
		-no-reboot \
		-no-shutdown \
		-append "stress-test"

# Clean build artifacts
clean:
	@echo "[CLEAN] Removing build artifacts"
	rm -f $(STRESS_TESTS)
	rm -f *.o
	rm -f *.log
	rm -f core
	@echo "Clean complete"

# Help message
help:
	@echo "AutomationOS Stress Test Suite"
	@echo ""
	@echo "Targets:"
	@echo "  all            - Build all stress tests"
	@echo "  stress-test    - Run stress test suite"
	@echo "  chaos-test     - Run chaos engineering tests"
	@echo "  race-test      - Run race condition fuzzer"
	@echo "  test-all       - Run all tests and generate reports"
	@echo "  qemu-stress    - Run tests in QEMU emulator"
	@echo "  clean          - Remove build artifacts"
	@echo "  help           - Show this help message"
	@echo ""
	@echo "Examples:"
	@echo "  make                  # Build tests"
	@echo "  make test-all         # Run complete test suite"
	@echo "  make stress-test      # Run stress tests only"
	@echo ""

# Dependency tracking
-include $(STRESS_OBJS:.o=.d)
-include $(CHAOS_OBJS:.o=.d)
-include $(RACE_OBJS:.o=.d)
