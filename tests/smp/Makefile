# Makefile for SMP Audit Rate Limiter Test
# =========================================

KERNEL_ROOT = ../..
KERNEL_INCLUDE = $(KERNEL_ROOT)/kernel/include
KERNEL_ARCH = $(KERNEL_ROOT)/kernel/arch/x86_64
BUILD_DIR = $(KERNEL_ROOT)/build/tests/smp

# Compiler and flags
CC = gcc
CFLAGS = -O2 -Wall -Wextra -I$(KERNEL_INCLUDE) -I$(KERNEL_ARCH)
CFLAGS_SMP = $(CFLAGS) -DSMP_ENABLE
CFLAGS_SINGLE = $(CFLAGS)

# Audit subsystem object files
AUDIT_OBJS = \
	$(KERNEL_ROOT)/kernel/audit/log.o \
	$(KERNEL_ROOT)/kernel/audit/buffer.o \
	$(KERNEL_ROOT)/kernel/audit/filter.o \
	$(KERNEL_ROOT)/kernel/audit/rules.o

# Test targets
TEST_SMP = $(BUILD_DIR)/test_audit_rate_limit_smp
TEST_SINGLE = $(BUILD_DIR)/test_audit_rate_limit_single

.PHONY: all clean test test-smp test-single

all: test-smp test-single

# Build directory
$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

# SMP stress test (multi-CPU)
$(TEST_SMP): test_audit_rate_limit_smp.c $(BUILD_DIR)
	@echo "Building SMP stress test..."
	$(CC) $(CFLAGS_SMP) test_audit_rate_limit_smp.c $(AUDIT_OBJS) -o $@
	@echo "Built: $@"

# Single-CPU test (baseline verification)
$(TEST_SINGLE): test_audit_rate_limit_smp.c $(BUILD_DIR)
	@echo "Building single-CPU test..."
	$(CC) $(CFLAGS_SINGLE) -DTEST_SINGLE_CPU_ONLY test_audit_rate_limit_smp.c $(AUDIT_OBJS) -o $@
	@echo "Built: $@"

# Run SMP test
test-smp: $(TEST_SMP)
	@echo ""
	@echo "========================================="
	@echo "Running SMP Audit Rate Limiter Test"
	@echo "========================================="
	@$(TEST_SMP)

# Run single-CPU test
test-single: $(TEST_SINGLE)
	@echo ""
	@echo "========================================="
	@echo "Running Single-CPU Rate Limiter Test"
	@echo "========================================="
	@$(TEST_SINGLE)

# Run all tests
test: test-single test-smp

# Clean build artifacts
clean:
	@echo "Cleaning test builds..."
	@rm -rf $(BUILD_DIR)
	@echo "Clean complete"

# Help
help:
	@echo "SMP Audit Rate Limiter Test Makefile"
	@echo ""
	@echo "Targets:"
	@echo "  all          - Build all tests (default)"
	@echo "  test-smp     - Build and run SMP stress test"
	@echo "  test-single  - Build and run single-CPU test"
	@echo "  test         - Run all tests"
	@echo "  clean        - Remove build artifacts"
	@echo "  help         - Show this help"
