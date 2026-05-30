# Unit Test Suite Makefile
# Builds unit tests for AutomationOS kernel components

CC = gcc
CFLAGS = -Wall -Wextra -O2 -I../../kernel/include -DUSER_SPACE_END=0x0000800000000000ULL
LDFLAGS =

# Coverage support (use: make COVERAGE=1)
ifeq ($(COVERAGE),1)
    CFLAGS += --coverage -fprofile-arcs -ftest-coverage
    LDFLAGS += -lgcov --coverage
endif

# Source files
KERNEL_SRC = ../../kernel/core/mem/vmm.c
HEAP_SRC = ../../kernel/core/mem/heap.c

# Test executables
TESTS = test_pmm test_heap test_scheduler test_user_copy test_null_checks \
        test_vmm test_syscall_handlers

all: $(TESTS)

test_pmm: test_pmm.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

test_heap: test_heap.c $(HEAP_SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test_scheduler: test_scheduler.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

test_user_copy: test_user_copy.c $(KERNEL_SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test_null_checks: test_null_checks.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

test_vmm: test_vmm.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

test_syscall_handlers: test_syscall_handlers.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TESTS) *.o
	rm -f *.gcda *.gcno *.gcov
	rm -f coverage.info coverage_filtered.info

run: all
	@echo "Running unit tests..."
	@echo ""
	@echo "=== test_user_copy ==="
	@./test_user_copy
	@echo ""
	@echo "=== test_vmm ==="
	@./test_vmm
	@echo ""
	@echo "=== test_syscall_handlers ==="
	@./test_syscall_handlers
	@echo ""
	@echo "All unit tests completed!"

coverage: clean
	$(MAKE) COVERAGE=1 all
	$(MAKE) run
	@echo ""
	@echo "Coverage data generated. Run 'make coverage-report' from root to generate HTML report."

.PHONY: all clean run coverage
