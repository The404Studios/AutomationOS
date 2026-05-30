# Makefile for Race Condition Tests
# Requires ThreadSanitizer support

CC = gcc
CFLAGS = -Wall -Wextra -g -O0 -fsanitize=thread -pthread
LDFLAGS = -fsanitize=thread -pthread

# Test targets
TESTS = test_scheduler_race test_process_table_race test_pid_race

all: $(TESTS)

test_scheduler_race: test_scheduler_race.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

test_process_table_race: test_process_table_race.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

test_pid_race: test_pid_race.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

# Run all tests
test: $(TESTS)
	@echo "=== Running Race Condition Tests ==="
	@for test in $(TESTS); do \
		echo "Running $$test..."; \
		./$$test || exit 1; \
	done
	@echo "=== All Race Tests Passed ==="

# Run with verbose ThreadSanitizer output
test-verbose: $(TESTS)
	@for test in $(TESTS); do \
		echo "Running $$test with verbose output..."; \
		TSAN_OPTIONS="verbosity=2 history_size=7" ./$$test || exit 1; \
	done

# Stress test (run multiple times)
stress: $(TESTS)
	@echo "=== Running Stress Tests (10 iterations) ==="
	@for i in 1 2 3 4 5 6 7 8 9 10; do \
		echo "Iteration $$i/10..."; \
		make test || exit 1; \
	done
	@echo "=== Stress Tests Passed ==="

clean:
	rm -f $(TESTS) *.o

.PHONY: all test test-verbose stress clean
