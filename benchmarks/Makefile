# Benchmark Suite Makefile
# Compiles all performance validation benchmarks

CC = gcc
CFLAGS = -O2 -Wall -Wextra -march=native -mtune=native -std=gnu11
LDFLAGS = -lpthread -lm

# Common source files
COMMON_SRC = common/bench_common.c
COMMON_OBJ = common/bench_common.o

# Micro-benchmarks
MICRO_TARGETS = \
	micro/context_switch_bench \
	micro/allocation_bench \
	micro/syscall_bench

# Macro-benchmarks
MACRO_TARGETS = \
	macro/boot_time_bench \
	macro/process_bench

# Workload benchmarks
WORKLOAD_TARGETS = \
	workloads/compile_bench \
	workloads/server_bench

# Stress tests
STRESS_TARGETS = \
	stress/cpu_stress \
	stress/memory_stress \
	stress/io_stress

# Regression tests
REGRESSION_TARGETS = \
	regression/baseline

# All targets
ALL_TARGETS = $(MICRO_TARGETS) $(MACRO_TARGETS) $(WORKLOAD_TARGETS) $(STRESS_TARGETS) $(REGRESSION_TARGETS)

.PHONY: all clean micro macro workloads stress regression run help

all: $(ALL_TARGETS)

# Build groups
micro: $(MICRO_TARGETS)
macro: $(MACRO_TARGETS)
workloads: $(WORKLOAD_TARGETS)
stress: $(STRESS_TARGETS)
regression: $(REGRESSION_TARGETS)

# Common object
$(COMMON_OBJ): $(COMMON_SRC)
	$(CC) $(CFLAGS) -c $< -o $@

# Pattern rule for benchmarks
%_bench: %_bench.c $(COMMON_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $< $(COMMON_OBJ) $(LDFLAGS) -o $@
	@echo "Built: $@"

# Specific targets
micro/context_switch_bench: micro/context_switch_bench.c $(COMMON_OBJ)
	$(CC) $(CFLAGS) $< $(COMMON_OBJ) $(LDFLAGS) -o $@

micro/allocation_bench: micro/allocation_bench.c $(COMMON_OBJ)
	$(CC) $(CFLAGS) $< $(COMMON_OBJ) $(LDFLAGS) -o $@

micro/syscall_bench: micro/syscall_bench.c $(COMMON_OBJ)
	$(CC) $(CFLAGS) $< $(COMMON_OBJ) $(LDFLAGS) -o $@

macro/boot_time_bench: macro/boot_time_bench.c $(COMMON_OBJ)
	$(CC) $(CFLAGS) $< $(COMMON_OBJ) $(LDFLAGS) -o $@

macro/process_bench: macro/process_bench.c $(COMMON_OBJ)
	$(CC) $(CFLAGS) $< $(COMMON_OBJ) $(LDFLAGS) -o $@

# Run all benchmarks
run: all
	@echo "========================================"
	@echo "Running Micro-Benchmarks"
	@echo "========================================"
	@for bench in $(MICRO_TARGETS); do \
		echo ""; \
		echo "Running $$bench..."; \
		./$$bench || true; \
	done
	@echo ""
	@echo "========================================"
	@echo "Running Macro-Benchmarks"
	@echo "========================================"
	@for bench in $(MACRO_TARGETS); do \
		echo ""; \
		echo "Running $$bench..."; \
		./$$bench || true; \
	done

# Run only micro-benchmarks
run-micro: micro
	@echo "Running micro-benchmarks..."
	@for bench in $(MICRO_TARGETS); do \
		echo ""; \
		./$$bench; \
	done

# Run only macro-benchmarks
run-macro: macro
	@echo "Running macro-benchmarks..."
	@for bench in $(MACRO_TARGETS); do \
		echo ""; \
		./$$bench; \
	done

# Generate performance report
report: all
	@echo "Generating performance validation report..."
	@./scripts/generate_report.sh

# Clean
clean:
	rm -f $(ALL_TARGETS)
	rm -f $(COMMON_OBJ)
	rm -f **/*.o
	rm -f results/*.txt results/*.json
	@echo "Cleaned build artifacts"

# Help
help:
	@echo "Benchmark Suite - Available targets:"
	@echo ""
	@echo "  all          - Build all benchmarks"
	@echo "  micro        - Build micro-benchmarks only"
	@echo "  macro        - Build macro-benchmarks only"
	@echo "  workloads    - Build workload benchmarks"
	@echo "  stress       - Build stress tests"
	@echo "  regression   - Build regression tests"
	@echo ""
	@echo "  run          - Build and run all benchmarks"
	@echo "  run-micro    - Run micro-benchmarks"
	@echo "  run-macro    - Run macro-benchmarks"
	@echo "  report       - Generate performance report"
	@echo ""
	@echo "  clean        - Remove build artifacts"
	@echo "  help         - Show this help"
	@echo ""
	@echo "Micro-benchmarks:"
	@for bench in $(MICRO_TARGETS); do echo "    $$bench"; done
	@echo ""
	@echo "Macro-benchmarks:"
	@for bench in $(MACRO_TARGETS); do echo "    $$bench"; done
