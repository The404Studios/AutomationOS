# AutomationOS Fuzzing Infrastructure Makefile

CC = gcc
AFL_CC = afl-gcc
CFLAGS = -Wall -Wextra -O2 -g -pthread
AFL_CFLAGS = -Wall -Wextra -O2 -g -pthread -fsanitize=address -fsanitize=undefined
LDFLAGS = -pthread

# Check if AFL++ is available
AFL_AVAILABLE := $(shell command -v afl-gcc 2> /dev/null)

# Targets
FUZZERS = syscall_fuzzer heap_fuzzer driver_fuzzer
CORPUS_DIRS = corpus/syscall_seeds corpus/heap_seeds corpus/driver_seeds
OUTPUT_DIRS = output/syscall output/heap output/driver crashes

.PHONY: all clean fuzzers corpus install-deps check-afl help coverage

all: check-afl fuzzers corpus

# Build all fuzzers
fuzzers: $(FUZZERS)

# Build individual fuzzers with AFL++ if available, otherwise GCC
syscall_fuzzer: syscall_fuzzer.c fuzzer_common.h
ifdef AFL_AVAILABLE
	@echo "[AFL++] Building syscall_fuzzer with AFL++"
	$(AFL_CC) $(AFL_CFLAGS) syscall_fuzzer.c -o syscall_fuzzer $(LDFLAGS)
else
	@echo "[GCC] Building syscall_fuzzer (AFL++ not available)"
	$(CC) $(CFLAGS) syscall_fuzzer.c -o syscall_fuzzer $(LDFLAGS)
endif

heap_fuzzer: heap_fuzzer.c fuzzer_common.h
ifdef AFL_AVAILABLE
	@echo "[AFL++] Building heap_fuzzer with AFL++"
	$(AFL_CC) $(AFL_CFLAGS) heap_fuzzer.c -o heap_fuzzer $(LDFLAGS)
else
	@echo "[GCC] Building heap_fuzzer (AFL++ not available)"
	$(CC) $(CFLAGS) heap_fuzzer.c -o heap_fuzzer $(LDFLAGS)
endif

driver_fuzzer: driver_fuzzer.c fuzzer_common.h
ifdef AFL_AVAILABLE
	@echo "[AFL++] Building driver_fuzzer with AFL++"
	$(AFL_CC) $(AFL_CFLAGS) driver_fuzzer.c -o driver_fuzzer $(LDFLAGS)
else
	@echo "[GCC] Building driver_fuzzer (AFL++ not available)"
	$(CC) $(CFLAGS) driver_fuzzer.c -o driver_fuzzer $(LDFLAGS)
endif

# Create corpus directories and seed files
corpus: $(CORPUS_DIRS)
	@echo "[CORPUS] Generating seed files..."
	@python3 generate_corpus.py

$(CORPUS_DIRS):
	@echo "[CORPUS] Creating $@"
	@mkdir -p $@

# Create output directories
$(OUTPUT_DIRS):
	@mkdir -p $@

# Check if AFL++ is installed
check-afl:
ifndef AFL_AVAILABLE
	@echo "======================================================================"
	@echo "WARNING: AFL++ not found!"
	@echo "Fuzzers will be built with GCC only (standalone mode)."
	@echo ""
	@echo "To install AFL++:"
	@echo "  Ubuntu/Debian: sudo apt-get install afl++"
	@echo "  Arch Linux:    sudo pacman -S afl++"
	@echo "  From source:   https://github.com/AFLplusplus/AFLplusplus"
	@echo "======================================================================"
	@sleep 2
endif

# Install dependencies
install-deps:
	@echo "[INSTALL] Installing fuzzing dependencies..."
	@if command -v apt-get > /dev/null; then \
		sudo apt-get update && sudo apt-get install -y afl++ gcc-multilib python3; \
	elif command -v pacman > /dev/null; then \
		sudo pacman -S --noconfirm afl++ gcc-multilib python; \
	else \
		echo "Unsupported package manager. Please install AFL++ manually."; \
	fi

# Run quick smoke test
test: fuzzers
	@echo "[TEST] Running fuzzer smoke tests..."
	@echo "[TEST] Testing syscall_fuzzer..."
	@./syscall_fuzzer --iterations 100
	@echo "[TEST] Testing heap_fuzzer..."
	@./heap_fuzzer --iterations 100
	@echo "[TEST] Testing driver_fuzzer (PS/2)..."
	@./driver_fuzzer --driver ps2 --iterations 100
	@echo "[TEST] All smoke tests passed!"

# Generate coverage report (requires gcov)
coverage: CFLAGS += -fprofile-arcs -ftest-coverage
coverage: LDFLAGS += -lgcov --coverage
coverage: clean fuzzers
	@echo "[COVERAGE] Running fuzzers with coverage instrumentation..."
	@./syscall_fuzzer --iterations 10000
	@./heap_fuzzer --iterations 10000
	@./driver_fuzzer --driver ps2 --iterations 10000
	@echo "[COVERAGE] Generating coverage report..."
	@mkdir -p output/coverage
	@gcov *.c
	@lcov --capture --directory . --output-file output/coverage/coverage.info
	@genhtml output/coverage/coverage.info --output-directory output/coverage
	@echo "[COVERAGE] Report generated: output/coverage/index.html"

# Clean build artifacts
clean:
	@echo "[CLEAN] Removing build artifacts..."
	@rm -f $(FUZZERS)
	@rm -f *.o *.gcda *.gcno *.gcov
	@rm -rf output/coverage
	@echo "[CLEAN] Build artifacts removed"

# Clean everything including corpus and output
clean-all: clean
	@echo "[CLEAN] Removing corpus and output directories..."
	@rm -rf corpus output crashes
	@echo "[CLEAN] All fuzzing data removed"

# Help target
help:
	@echo "AutomationOS Fuzzing Infrastructure"
	@echo ""
	@echo "Targets:"
	@echo "  make all          - Build all fuzzers and generate corpus"
	@echo "  make fuzzers      - Build all fuzzers"
	@echo "  make corpus       - Generate seed corpus"
	@echo "  make test         - Run fuzzer smoke tests"
	@echo "  make coverage     - Generate code coverage report"
	@echo "  make install-deps - Install AFL++ and dependencies"
	@echo "  make clean        - Remove build artifacts"
	@echo "  make clean-all    - Remove everything including output"
	@echo "  make help         - Show this help"
	@echo ""
	@echo "Individual Fuzzers:"
	@echo "  make syscall_fuzzer   - Build syscall fuzzer"
	@echo "  make heap_fuzzer      - Build heap fuzzer"
	@echo "  make driver_fuzzer    - Build driver fuzzer"
	@echo ""
	@echo "Running Fuzzers:"
	@echo "  Standalone mode:"
	@echo "    ./syscall_fuzzer --iterations 1000000"
	@echo "    ./heap_fuzzer --stress --concurrent 10"
	@echo "    ./driver_fuzzer --driver ps2 --iterations 100000"
	@echo ""
	@echo "  AFL++ mode (requires AFL++ installation):"
	@echo "    afl-fuzz -i corpus/syscall_seeds -o output/syscall -- ./syscall_fuzzer @@"
	@echo "    afl-fuzz -i corpus/heap_seeds -o output/heap -- ./heap_fuzzer @@"
	@echo "    afl-fuzz -i corpus/driver_seeds -o output/driver -- ./driver_fuzzer @@"
	@echo ""
	@echo "  Orchestration:"
	@echo "    ../../scripts/run-fuzzer.sh --all --time 1h"
